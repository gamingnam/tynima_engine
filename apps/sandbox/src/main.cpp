// Engine developer's playground. A pile of glTF models dropped on a floor
// by the physics world, as entities in the scene World, lit with
// Cook-Torrance, a reverse-Z depth buffer, an sRGB swapchain, and a fly
// camera — all of it run by the SDK's Runtime, which the sandbox drives the
// way the editor and a game's runtime do, reaching into the engine only
// for its own scene and the keys that show it off.
//
//   tynima-sandbox [--headless] [--frames N] [--model path.glb] [--physics tynima|jolt]
//                  [--rhi sdl|metal] [--shading forward|fused|split]
//                  [--record log.tyrec | --replay log.tyrec] [--save-scene scene.toml]
//
// The pile runs on the engine's own physics by default; --physics jolt
// drops the same pile through Jolt, and the report line at exit compares.
// The frame is drawn through the engine's native Metal backend on a Mac
// and SDL GPU elsewhere; --rhi sdl asks for SDL GPU on a Mac too: the same
// frame, the same shaders, through SDL's Metal instead of ours. --shading
// fused lights the scene from a G-buffer that never leaves the GPU's tile
// memory; --shading split stores and reads it back the way a desktop GPU
// must, for comparison (Y cycles them).
// --headless runs without a window or a GPU, at a fixed sixtieth of a
// second a frame, with a scripted player at the keyboard.
//
// --record writes every frame's input and frame time, and the physics
// world's hash at the end, to a text log; --replay feeds that log back in
// place of the clock and the keyboard and checks the run ends on the same
// hash — the simulation is deterministic, so it must, and CI replays a
// recorded pile to prove it (exit code 3 when it does not). --save-scene
// writes the world as it ended to a scene file, for the editor to open.
//
// Controls: hold the right mouse button to look; W/A/S/D move, Q/E descend
// and climb, Shift runs; R drops the pile again; L (the game module)
// launches it; F follows the character; C shows the shadow cascades, X
// toggles shadows, K shows the lights per cluster, P toggles the point
// lights, G bloom; T cycles the tonemapper, H the anti-aliasing, Y the
// shading path; Escape quits.
#include <tynima/core/log.h>
#include <tynima/core/profile.h>
#include <tynima/physics/character.h>
#include <tynima/physics/physics.h>
#include <tynima/platform/input.h>
#include <tynima/platform/input_log.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/platform/window.h>
#include <tynima/render/camera.h>
#include <tynima/render/clusters.h>
#include <tynima/render/mesh.h>
#include <tynima/render/model.h>
#include <tynima/render/post.h>
#include <tynima/render/scene_renderer.h>
#include <tynima/rhi/device.h>
#include <tynima/scene/components.h>
#include <tynima/scene/scene_file.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace platform = tynima::platform;
namespace rhi = tynima::rhi;
namespace render = tynima::render;
namespace scene = tynima::scene;
namespace physics = tynima::physics;
namespace sdk = tynima::sdk;
using namespace tynima::math;

namespace {

// The runtime's model slots, in the order the sandbox adds them.
constexpr std::uint32_t kModelBottle = 0;
constexpr std::uint32_t kModelFloor = 1;
constexpr std::uint32_t kModelCharacter = 2; // the capsule the controller walks around in
constexpr std::uint32_t kModelGate = 3;      // a slab on a hinge

constexpr float kFloorHalfWidth = 6.0f;
constexpr float kFloorHalfThickness = 0.25f;

// The pile: the model dropped over a 5x5 footprint in four waves, every one
// shoved sideways, tilted and given a little spin, so they hit each other on
// the way down and end up in a heap rather than in neat standing stacks.
struct Pile {
    static constexpr int kSide = 5;
    static constexpr int kLayers = 4;
    std::vector<scene::Entity> entities;
    std::vector<scene::Transform> rest_poses; // where R puts them back
    float footprint = 1.0f;                   // half the width of the drop area, metres
    float drop_height = 1.0f;                 // the top of the highest wave
};

// Deterministic noise in [0, 1): the same pile drops the same way every run,
// which is what makes the headless run and the determinism work of task 7
// meaningful.
float noise(std::uint32_t n, std::uint32_t salt) {
    std::uint32_t h = n * 2654435761u + salt * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// Every body is a box around the mesh's bounds, offset from the entity's
// origin the way the mesh is; the entity's Transform is the body's pose.
Pile populate_pile(scene::World& world, physics::PhysicsWorld& physics, const Aabb& mesh) {
    Pile pile;
    const Vec3 size = mesh.max - mesh.min;
    const Vec3 half_extents = size * 0.5f;
    const Vec3 center = (mesh.min + mesh.max) * 0.5f;
    const float spacing = std::max(size.x, size.z) * 1.4f;
    const float wave_height = size.y * 1.8f;
    const float half = static_cast<float>(Pile::kSide - 1) * 0.5f;
    pile.footprint = (half + 0.5f) * spacing;
    pile.drop_height = size.y * 2.0f + static_cast<float>(Pile::kLayers) * wave_height;
    for (int layer = 0; layer < Pile::kLayers; ++layer) {
        for (int z = 0; z < Pile::kSide; ++z) {
            for (int x = 0; x < Pile::kSide; ++x) {
                const auto n = static_cast<std::uint32_t>((layer * Pile::kSide + z) * Pile::kSide + x);
                // Up to nearly half a slot sideways: a bottle often lands on
                // the edge of the one below, or between two, and topples.
                const float shove_x = (noise(n, 1) - 0.5f) * spacing * 0.9f;
                const float shove_z = (noise(n, 2) - 0.5f) * spacing * 0.9f;
                const float lift = noise(n, 3) * wave_height * 0.5f; // no two in a column arrive together
                const Vec3 position{(static_cast<float>(x) - half) * spacing + shove_x,
                                    size.y * 2.0f + static_cast<float>(layer) * wave_height + lift -
                                        mesh.min.y,
                                    (static_cast<float>(z) - half) * spacing + shove_z};
                // A tilt of up to ~30 degrees about a random horizontal axis, then a random yaw.
                const float tilt_direction = noise(n, 4) * kTwoPi;
                const Vec3 tilt_axis{cosine(tilt_direction), 0.0f, sine(tilt_direction)};
                const Quat rotation = Quat::from_axis_angle(Vec3::unit_y(), noise(n, 5) * kTwoPi) *
                                      Quat::from_axis_angle(tilt_axis, (noise(n, 6) - 0.5f) * 1.0f);
                physics::BodyDesc body;
                body.shape = physics::Shape::box(half_extents, center);
                body.position = position;
                body.rotation = rotation;
                body.angular_velocity =
                    Vec3{noise(n, 7) - 0.5f, noise(n, 8) - 0.5f, noise(n, 9) - 0.5f} * 3.0f; // a little spin
                body.mass = 0.6f; // a full 0.6 l bottle
                body.friction = 0.5f;
                body.restitution = 0.1f;
                const physics::BodyHandle handle = physics.create_body(body);
                const scene::Transform pose{.position = position, .rotation = rotation};
                char name[scene::Name::kCapacity];
                std::snprintf(name, sizeof name, "bottle %u", n);
                const scene::Entity entity =
                    world.create(scene::Name(name), pose, scene::LocalToWorld{},
                                 scene::MeshRenderer{.model = kModelBottle}, scene::RigidBody{handle});
                pile.entities.push_back(entity);
                pile.rest_poses.push_back(pose);
            }
        }
    }
    return pile;
}

// Puts every body back where the pile started and lets it fall again, with
// the same spins as the first time: R replays the drop exactly.
void reset_pile(scene::World& world, physics::PhysicsWorld& physics, const Pile& pile) {
    for (std::size_t i = 0; i < pile.entities.size(); ++i) {
        if (const auto* body = world.get<scene::RigidBody>(pile.entities[i])) {
            const auto n = static_cast<std::uint32_t>(i);
            physics.set_velocity(body->body, Vec3{0.0f},
                                 Vec3{noise(n, 7) - 0.5f, noise(n, 8) - 0.5f, noise(n, 9) - 0.5f} * 3.0f);
            physics.set_transform(body->body, pile.rest_poses[i].position, pile.rest_poses[i].rotation);
        }
    }
}

// How the pile ended up: how many bottles still stand, how far it spread,
// how high it is. One line to compare a run against another — the same
// scene through our own solver later must come out about the same.
void report_pile(scene::World& world, const Pile& pile) {
    std::uint32_t upright = 0, toppled = 0;
    float spread = 0.0f, top = 0.0f;
    for (const scene::Entity entity : pile.entities) {
        const auto* transform = world.get<scene::Transform>(entity);
        if (transform == nullptr) {
            continue;
        }
        const Vec3 up = transform->rotation.rotate(Vec3::unit_y());
        (up.y > 0.7f ? upright : toppled)++;
        spread = std::max(spread, std::sqrt(transform->position.x * transform->position.x +
                                            transform->position.z * transform->position.z));
        top = std::max(top, transform->position.y);
    }
    TY_LOG_INFO("scene", "pile: %u upright, %u toppled, spread %.2f m, top at %.2f m", upright, toppled,
                static_cast<double>(spread), static_cast<double>(top));
}

// A closed box with hard edges: 24 vertices, 6 quads, UVs per face, tangents
// from those. The floor is one of these, stretched.
render::MeshData make_box_mesh(const Vec3& half_extents) {
    render::MeshData mesh;
    const Vec3 h = half_extents;
    struct Face {
        Vec3 normal, u_axis, v_axis;
    };
    const Face faces[6] = {
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  // top
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // bottom
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  // +x
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -x
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +z
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}, // -z
    };
    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        // Each axis is a signed unit vector: scaling it component-wise by the
        // half extents gives the face centre and its two half-edges.
        const Vec3 origin = face.normal * h;
        const Vec3 du = face.u_axis * h;
        const Vec3 dv = face.v_axis * h;
        const float tile = 2.0f; // UV repeats per metre, so a big floor still shows texture
        const Vec2 uv_scale{length(du) * tile, length(dv) * tile};
        for (int corner = 0; corner < 4; ++corner) {
            const float su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            const float sv = (corner >= 2) ? 1.0f : -1.0f;
            const Vec2 uv{(su + 1.0f) * 0.5f * uv_scale.x, (1.0f - sv) * 0.5f * uv_scale.y};
            mesh.vertices.push_back({.position = origin + du * su + dv * sv,
                                     .normal = face.normal,
                                     .uv = uv,
                                     .tangent = Vec4{face.u_axis, 1.0f}});
        }
        for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + i);
        }
    }
    mesh.submeshes.push_back(
        {.first_index = 0, .index_count = static_cast<std::uint32_t>(mesh.indices.size()), .material = 0});
    mesh.compute_bounds();
    return mesh;
}

render::ModelData make_floor_model() {
    render::ModelData floor;
    floor.mesh = make_box_mesh(Vec3{kFloorHalfWidth, kFloorHalfThickness, kFloorHalfWidth});
    render::MaterialData material;
    material.base_color_factor = Vec4{0.42f, 0.42f, 0.40f, 1.0f};
    material.metallic_factor = 0.0f;
    material.roughness_factor = 0.85f;
    floor.materials.push_back(material);
    return floor;
}

// A capsule standing on y: a cylinder of `half_height` each way with a
// hemisphere on each end, `segments` around and `rings` up each cap.
render::MeshData make_capsule_mesh(float radius, float half_height) {
    render::MeshData mesh;
    constexpr int kSegments = 24;
    constexpr int kRings = 6; // per cap
    // Rows of vertices from the bottom pole to the top pole: each cap has
    // kRings + 1 rows, the equator rows of the two caps being the cylinder.
    for (int cap = 0; cap < 2; ++cap) {
        for (int ring = 0; ring <= kRings; ++ring) {
            // Latitude from the pole (-90 degrees) to the equator for the
            // bottom cap, the equator to the pole for the top one.
            const float t = static_cast<float>(ring) / static_cast<float>(kRings);
            const float latitude = cap == 0 ? -kPi * 0.5f * (1.0f - t) : kPi * 0.5f * t;
            const float y = std::sin(latitude);
            const float r = std::cos(latitude);
            const float offset = cap == 0 ? -half_height : half_height;
            for (int seg = 0; seg <= kSegments; ++seg) {
                const float u = static_cast<float>(seg) / static_cast<float>(kSegments);
                const float longitude = u * kTwoPi;
                const Vec3 normal{r * std::cos(longitude), y, r * std::sin(longitude)};
                const float height = offset + y * radius; // -half_height - radius .. half_height + radius
                const float v = 1.0f - (height + half_height + radius) / (2.0f * (half_height + radius));
                const Vec4 tangent{-std::sin(longitude), 0.0f, std::cos(longitude), 1.0f};
                mesh.vertices.push_back({.position = normal * radius + Vec3{0.0f, offset, 0.0f},
                                         .normal = normal,
                                         .uv = Vec2{u, v},
                                         .tangent = tangent});
            }
        }
    }
    const auto rows = static_cast<std::uint32_t>(2 * (kRings + 1));
    const auto columns = static_cast<std::uint32_t>(kSegments + 1);
    for (std::uint32_t row = 0; row + 1 < rows; ++row) {
        for (std::uint32_t col = 0; col + 1 < columns; ++col) {
            const std::uint32_t a = row * columns + col;
            const std::uint32_t b = a + columns;
            // Counter-clockwise seen from outside (longitude runs clockwise seen from above).
            for (const std::uint32_t i : {a, b, a + 1, a + 1, b, b + 1}) {
                mesh.indices.push_back(i);
            }
        }
    }
    mesh.submeshes.push_back(
        {.first_index = 0, .index_count = static_cast<std::uint32_t>(mesh.indices.size()), .material = 0});
    mesh.compute_bounds();
    return mesh;
}

render::ModelData make_plain_model(render::MeshData mesh, const Vec4& color, float roughness) {
    render::ModelData model;
    model.mesh = std::move(mesh);
    render::MaterialData material;
    material.base_color_factor = color;
    material.metallic_factor = 0.0f;
    material.roughness_factor = roughness;
    model.materials.push_back(material);
    return model;
}

// ---------------------------------------------------- the rest of the scene

// The character: a capsule of human proportions the controller drives, drawn
// as one. WASD walks it (relative to the camera), Space jumps, Shift runs.
constexpr float kCharacterRadius = 0.3f;
constexpr float kCharacterHalfHeight = 0.5f;
constexpr Vec3 kCharacterStart{0.0f, kCharacterRadius + kCharacterHalfHeight, 4.5f};
constexpr float kWalkSpeed = 3.0f; // m/s; Shift doubles it

// A gate on a vertical hinge beside the pile, free to swing 110 degrees each
// way: a launched bottle knocks it, the character walks through it.
constexpr Vec3 kGateHalf{0.05f, 0.9f, 0.6f};
constexpr Vec3 kGatePin{2.6f, 0.9f, -0.6f}; // the hinge edge; the slab hangs off it towards +z
constexpr float kGateSwing = radians(110.0f);

// A chain of bottles hanging from a point over the floor on the other side,
// each a short distance joint from the one above.
constexpr Vec3 kChainTop{-2.6f, 3.2f, 0.0f};
constexpr int kChainLinks = 4;
constexpr float kChainGap = 0.06f;

struct Rest {
    scene::Entity character_entity;
    physics::JointHandle gate;
    std::vector<scene::Entity> chain;
};

Rest populate_rest(scene::World& world, physics::PhysicsWorld& physics, physics::BodyHandle character_body,
                   const Aabb& bottle) {
    Rest rest;
    rest.character_entity = world.create(
        scene::Name("character"), scene::Transform{.position = kCharacterStart}, scene::LocalToWorld{},
        scene::MeshRenderer{.model = kModelCharacter}, scene::RigidBody{character_body});
    {
        physics::BodyDesc gate;
        gate.shape = physics::Shape::box(kGateHalf);
        gate.position = kGatePin + Vec3{0.0f, 0.0f, kGateHalf.z};
        gate.mass = 15.0f;
        gate.friction = 0.4f;
        const physics::BodyHandle body = physics.create_body(gate);
        (void)world.create(scene::Name("gate"), scene::Transform{.position = gate.position},
                           scene::LocalToWorld{}, scene::MeshRenderer{.model = kModelGate},
                           scene::RigidBody{body});
        physics::JointDesc hinge;
        hinge.type = physics::JointType::Hinge;
        hinge.a = body;
        hinge.anchor_a = Vec3{0.0f, 0.0f, -kGateHalf.z};
        hinge.anchor_b = kGatePin;
        hinge.axis_a = Vec3::unit_y();
        hinge.axis_b = Vec3::unit_y();
        hinge.limited = true;
        hinge.min_angle = -kGateSwing;
        hinge.max_angle = kGateSwing;
        rest.gate = physics.create_joint(hinge);
    }
    {
        // Each bottle hangs from its top, off the bottom of the one above.
        const Vec3 size = bottle.max - bottle.min;
        const Vec3 center = (bottle.min + bottle.max) * 0.5f;
        const Vec3 top{center.x, bottle.max.y, center.z};
        const Vec3 bottom{center.x, bottle.min.y, center.z};
        physics::BodyHandle above;
        float hang = kChainTop.y;
        for (int i = 0; i < kChainLinks; ++i) {
            hang -= kChainGap;
            physics::BodyDesc body;
            body.shape = physics::Shape::box(size * 0.5f, center);
            body.position = Vec3{kChainTop.x, hang - bottle.max.y, kChainTop.z};
            body.mass = 0.6f;
            body.friction = 0.5f;
            const physics::BodyHandle handle = physics.create_body(body);
            char name[scene::Name::kCapacity];
            std::snprintf(name, sizeof name, "chain %d", i);
            const scene::Entity entity = world.create(
                scene::Name(name), scene::Transform{.position = body.position}, scene::LocalToWorld{},
                scene::MeshRenderer{.model = kModelBottle}, scene::RigidBody{handle});
            rest.chain.push_back(entity);
            physics::JointDesc link;
            link.type = physics::JointType::Distance;
            link.a = handle;
            link.anchor_a = top;
            link.b = above;
            link.anchor_b = above ? bottom : kChainTop;
            link.length = kChainGap;
            (void)physics.create_joint(link);
            above = handle;
            hang -= size.y;
        }
    }
    return rest;
}

void report_rest(scene::World& world, const physics::PhysicsWorld& physics,
                 const physics::CharacterController& character, const Rest& rest) {
    const Vec3 p = character.position();
    TY_LOG_INFO("scene", "character at %.2f %.2f %.2f, %s", static_cast<double>(p.x),
                static_cast<double>(p.y), static_cast<double>(p.z),
                character.on_ground() ? "on the ground" : "in the air");
    float lowest = kChainTop.y;
    for (const scene::Entity entity : rest.chain) {
        if (const auto* transform = world.get<scene::Transform>(entity)) {
            lowest = std::min(lowest, transform->position.y);
        }
    }
    const float gate_degrees = physics.hinge_angle(rest.gate) * 180.0f / kPi;
    TY_LOG_INFO("scene", "gate at %.0f degrees, chain hangs down to %.2f m, %u joints",
                static_cast<double>(gate_degrees), static_cast<double>(lowest), physics.joint_count());
}

#ifndef TYNIMA_SANDBOX_GAME_MODULE
#define TYNIMA_SANDBOX_GAME_MODULE ""
#endif

#ifndef TYNIMA_SANDBOX_ASSETS_DIR
#define TYNIMA_SANDBOX_ASSETS_DIR "."
#endif

struct Options {
    bool headless = false;
    long max_frames = -1; // -1: run until closed
    std::string model = TYNIMA_SANDBOX_ASSETS_DIR "/WaterBottle.glb";
    bool jolt = false; // the reference physics instead of the engine's own
    rhi::Backend rhi = rhi::Backend::Auto;
    render::ShadingPath shading = render::ShadingPath::Forward;
    std::string record; // write the input log here at exit
    std::string replay; // play this input log instead of live input and time
    std::string save_scene; // write the scene here at exit
};

// Exit codes past the usual 0 and 1.
constexpr int kExitUsage = 2;
constexpr int kExitReplayMismatch = 3;
constexpr int kExitSkipped = 77; // what CTest's SKIP_RETURN_CODE reads as "not run"

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            options.headless = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            options.max_frames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            options.model = argv[++i];
        } else if (std::strcmp(argv[i], "--physics") == 0 && i + 1 < argc) {
            const char* which = argv[++i];
            options.jolt = std::strcmp(which, "jolt") == 0;
            if (!options.jolt && std::strcmp(which, "tynima") != 0) {
                std::fprintf(stderr, "--physics: expected 'tynima' or 'jolt', got '%s'\n", which);
                std::exit(kExitUsage);
            }
        } else if (std::strcmp(argv[i], "--rhi") == 0 && i + 1 < argc) {
            const char* which = argv[++i];
            if (std::strcmp(which, "metal") == 0) {
                options.rhi = rhi::Backend::Metal;
            } else if (std::strcmp(which, "sdl") == 0) {
                options.rhi = rhi::Backend::SdlGpu;
            } else {
                std::fprintf(stderr, "--rhi: expected 'sdl' or 'metal', got '%s'\n", which);
                std::exit(kExitUsage);
            }
        } else if (std::strcmp(argv[i], "--shading") == 0 && i + 1 < argc) {
            const char* which = argv[++i];
            if (std::strcmp(which, "forward") == 0) {
                options.shading = render::ShadingPath::Forward;
            } else if (std::strcmp(which, "fused") == 0) {
                options.shading = render::ShadingPath::Fused;
            } else if (std::strcmp(which, "split") == 0) {
                options.shading = render::ShadingPath::Split;
            } else {
                std::fprintf(stderr, "--shading: expected 'forward', 'fused' or 'split', got '%s'\n", which);
                std::exit(kExitUsage);
            }
        } else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            options.record = argv[++i];
        } else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            options.replay = argv[++i];
        } else if (std::strcmp(argv[i], "--save-scene") == 0 && i + 1 < argc) {
            options.save_scene = argv[++i];
        } else {
            std::fprintf(stderr, "usage: tynima-sandbox [--headless] [--frames N] [--model path.glb] "
                                 "[--physics tynima|jolt] [--rhi sdl|metal] [--shading forward|fused|split] "
                                 "[--record log.tyrec | --replay log.tyrec] [--save-scene scene.toml]\n");
            std::exit(kExitUsage);
        }
    }
    if (!options.record.empty() && !options.replay.empty()) {
        std::fprintf(stderr, "--record and --replay are one or the other\n");
        std::exit(kExitUsage);
    }
    return options;
}

// ------------------------------------------------------------------ shading

// What the keys toggle, on the runtime's settings. Printed whenever it changes.
struct Shading {
    float light_azimuth = radians(35.0f);   // around +y, from +z
    float light_elevation = radians(50.0f); // above the horizon

    Vec3 light_direction() const {
        return {std::cos(light_elevation) * std::sin(light_azimuth), std::sin(light_elevation),
                std::cos(light_elevation) * std::cos(light_azimuth)};
    }

    static void print(const sdk::Runtime& runtime, float azimuth_radians, float elevation_radians) {
        static constexpr const char* kTonemaps[] = {"off", "aces", "agx"};
        static constexpr const char* kAntiAliasing[] = {"off", "fxaa", "taa"};
        const render::SceneSettings& scene = runtime.scene_settings;
        const render::PostSettings& post = runtime.post().settings;
        float azimuth = std::fmod(degrees(azimuth_radians), 360.0f);
        if (azimuth < 0.0f)
            azimuth += 360.0f;
        TY_LOG_INFO("shading",
                    "%s, %s | view %s | tonemap %s | bloom %s | aa %s | shadows %s | point lights %s | "
                    "sun az %.0f el %.0f",
                    render::shading_path_name(scene.path), render::shading_model_name(scene.model),
                    render::debug_view_name(scene.debug_view), kTonemaps[static_cast<int>(post.tonemap)],
                    post.bloom ? "on" : "off", kAntiAliasing[static_cast<int>(post.anti_aliasing)],
                    scene.shadows ? "on" : "off", scene.point_lights ? "on" : "off",
                    static_cast<double>(azimuth), static_cast<double>(degrees(elevation_radians)));
    }

    // Returns true when something changed.
    bool update(sdk::Runtime& runtime, const platform::Input& input, float dt) {
        using platform::Key;
        using render::DebugView;
        render::SceneSettings& scene = runtime.scene_settings;
        render::PostSettings& post = runtime.post().settings;
        const render::SceneSettings old_scene = scene;
        const render::Tonemap old_tonemap = post.tonemap;
        const render::AntiAliasing old_aa = post.anti_aliasing;
        const bool old_bloom = post.bloom;
        const auto toggle_view = [&](DebugView view) {
            scene.debug_view = scene.debug_view == view ? DebugView::Lit : view;
        };
        if (input.key_pressed(Key::Y)) {
            scene.path = static_cast<render::ShadingPath>((static_cast<int>(scene.path) + 1) % 3);
        }
        if (input.key_pressed(Key::Digit1))
            scene.model = render::ShadingModel::Unlit;
        if (input.key_pressed(Key::Digit2))
            scene.model = render::ShadingModel::BlinnPhong;
        if (input.key_pressed(Key::Digit3))
            scene.model = render::ShadingModel::CookTorrance;
        if (input.key_pressed(Key::N))
            toggle_view(DebugView::Normals);
        if (input.key_pressed(Key::M))
            toggle_view(DebugView::MetallicRoughness);
        if (input.key_pressed(Key::O))
            toggle_view(DebugView::Occlusion);
        if (input.key_pressed(Key::V))
            toggle_view(DebugView::VertexNormals);
        if (input.key_pressed(Key::B))
            toggle_view(DebugView::Tangents);
        if (input.key_pressed(Key::C))
            toggle_view(DebugView::Cascades);
        if (input.key_pressed(Key::K))
            toggle_view(DebugView::Clusters);
        if (input.key_pressed(Key::Digit0))
            scene.debug_view = DebugView::Lit;
        if (input.key_pressed(Key::T)) {
            post.tonemap = static_cast<render::Tonemap>((static_cast<int>(post.tonemap) + 1) % 3);
        }
        if (input.key_pressed(Key::G))
            post.bloom = !post.bloom;
        if (input.key_pressed(Key::H)) {
            post.anti_aliasing =
                static_cast<render::AntiAliasing>((static_cast<int>(post.anti_aliasing) + 1) % 3);
        }
        if (input.key_pressed(Key::X))
            scene.shadows = !scene.shadows;
        if (input.key_pressed(Key::P))
            scene.point_lights = !scene.point_lights;
        const float turn = radians(60.0f) * dt;
        bool moved = false;
        if (input.key_down(Key::Left)) {
            light_azimuth -= turn;
            moved = true;
        }
        if (input.key_down(Key::Right)) {
            light_azimuth += turn;
            moved = true;
        }
        if (input.key_down(Key::Up)) {
            light_elevation = std::min(light_elevation + turn, radians(89.0f));
            moved = true;
        }
        if (input.key_down(Key::Down)) {
            light_elevation = std::max(light_elevation - turn, radians(-10.0f));
            moved = true;
        }
        runtime.sun.direction = light_direction();
        const bool changed = scene.model != old_scene.model || scene.debug_view != old_scene.debug_view ||
                             post.tonemap != old_tonemap || post.bloom != old_bloom ||
                             post.anti_aliasing != old_aa || scene.shadows != old_scene.shadows ||
                             scene.point_lights != old_scene.point_lights || scene.path != old_scene.path;
        if (changed)
            print(runtime, light_azimuth, light_elevation);
        return changed || moved;
    }
};

// --------------------------------------------------------------- fly camera

struct FlyCamera {
    render::Camera camera;
    float yaw = 0.0f;   // radians about world +y
    float pitch = 0.0f; // radians about local +x
    float speed = 1.0f; // metres per second
    bool looking = false;
    bool follow = false; // F: hang behind the character instead of flying free
    static constexpr float kFollowDistance = 4.0f;
    static constexpr Vec3 kFollowFocus{0.0f, 0.5f, 0.0f}; // above the capsule's centre: head height

    void frame(const Vec3& center, float radius) {
        camera.position = center + Vec3{0.0f, radius * 0.35f, radius * 2.4f};
        camera.look_at(center);
        // Recover yaw/pitch from the resulting forward so mouse-look continues from here.
        const Vec3 f = camera.forward();
        yaw = arctan2(-f.x, -f.z); // deterministic: in follow mode the camera steers the character
        pitch = arcsin(f.y);
        camera.near = std::max(radius * 0.02f, 0.005f);
        speed = std::max(radius * 1.5f, 0.2f);
    }

    // Where WASD point, on the ground, seen from this camera.
    [[nodiscard]] Vec3 walk_direction(const platform::Input& input) const {
        Vec3 forward = camera.forward();
        forward.y = 0.0f;
        if (length_squared(forward) < 1e-6f) {
            forward = Quat::from_axis_angle(Vec3::unit_y(), yaw).rotate(-Vec3::unit_z());
        }
        forward = normalize(forward);
        const Vec3 right = cross(forward, Vec3::unit_y());
        Vec3 move = Vec3::zero();
        if (input.key_down(platform::Key::W))
            move += forward;
        if (input.key_down(platform::Key::S))
            move -= forward;
        if (input.key_down(platform::Key::D))
            move += right;
        if (input.key_down(platform::Key::A))
            move -= right;
        return length_squared(move) > 0.0f ? normalize(move) : move;
    }

    void update(const platform::Input& input, platform::Window& window, float dt, const Vec3* follow_target) {
        if (input.mouse_pressed(platform::MouseButton::Right)) {
            looking = true;
            window.set_relative_mouse_mode(true);
        }
        if (input.mouse_released(platform::MouseButton::Right)) {
            looking = false;
            window.set_relative_mouse_mode(false);
        }
        if (looking) {
            constexpr float kSensitivity = 0.0025f; // radians per pixel
            yaw -= input.mouse_dx() * kSensitivity;
            pitch = clamp(pitch - input.mouse_dy() * kSensitivity, radians(-89.0f), radians(89.0f));
        }
        camera.rotation =
            Quat::from_axis_angle(Vec3::unit_y(), yaw) * Quat::from_axis_angle(Vec3::unit_x(), pitch);
        if (follow_target != nullptr) {
            // Behind and a little above the character, looking at its head.
            camera.position = *follow_target + kFollowFocus - camera.forward() * kFollowDistance;
            return;
        }

        Vec3 move = Vec3::zero();
        if (input.key_down(platform::Key::W))
            move += camera.forward();
        if (input.key_down(platform::Key::S))
            move -= camera.forward();
        if (input.key_down(platform::Key::D))
            move += camera.right();
        if (input.key_down(platform::Key::A))
            move -= camera.right();
        if (input.key_down(platform::Key::E))
            move += Vec3::unit_y();
        if (input.key_down(platform::Key::Q))
            move -= Vec3::unit_y();
        if (length_squared(move) > 0.0f) {
            const bool run =
                input.key_down(platform::Key::LeftShift) || input.key_down(platform::Key::RightShift);
            camera.position += normalize(move) * (speed * (run ? 4.0f : 1.0f) * dt);
        }
    }
};

// ------------------------------------------------------------------- lights

// A hundred coloured point lights circling the pile, listed per cell of the
// cluster grid by the scene renderer's compute pass each frame.
constexpr std::uint32_t kLightCount = 100;

// A hue as linear RGB, fully saturated.
Vec3 hue_color(float hue) {
    const float h = hue * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    switch (static_cast<int>(h) % 6) {
    case 0:
        return {1.0f, x, 0.0f};
    case 1:
        return {x, 1.0f, 0.0f};
    case 2:
        return {0.0f, 1.0f, x};
    case 3:
        return {0.0f, x, 1.0f};
    case 4:
        return {x, 0.0f, 1.0f};
    default:
        return {1.0f, 0.0f, x};
    }
}

// The point lights at time t: each on its own circle around the pile, at
// its own height, bobbing, coloured by where it is in the hundred.
void animate_lights(float t, render::PointLight* out, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const float direction = noise(i, 21) > 0.5f ? 1.0f : -1.0f;
        const float angle = t * direction * (0.15f + 0.35f * noise(i, 20)) + noise(i, 22) * kTwoPi;
        const float radius = 1.2f + 4.5f * noise(i, 23);
        const float bob = std::sin(t * (0.4f + 0.8f * noise(i, 24)) + noise(i, 25) * kTwoPi);
        const float height = 0.4f + 1.6f * (0.5f + 0.5f * bob);
        const Vec3 position{radius * std::cos(angle), height, radius * std::sin(angle)};
        const float reach = 1.8f + 1.4f * noise(i, 26);
        out[i].position_radius = {position, reach};
        out[i].color = {hue_color(static_cast<float>(i) / static_cast<float>(count)) * 4.0f, 0.0f};
    }
}

// -------------------------------------------------------------------- input

void report_edges(const platform::Input& input) {
    for (std::size_t i = 0; i < platform::kKeyCount; ++i) {
        const auto key = static_cast<platform::Key>(i);
        if (input.key_pressed(key)) {
            TY_LOG_DEBUG("input", "%s", platform::key_name(key));
        }
    }
}

// Headless there is nobody at the keyboard, so the sandbox plays one: F on
// the first frame to follow the character, D held for a second and a half
// to walk it along the edge of the pile, a jump on the way, and L once the
// pile has settled to launch it through the game module. It goes in through
// the same door as a replay, so a log recorded headless carries all of it.
platform::InputFrame scripted_frame(long index, float dt, void*) {
    platform::InputFrame frame;
    frame.dt = dt;
    const auto bit = [](platform::Key key) { return static_cast<std::size_t>(key); };
    const auto press = [&](platform::Key key) {
        frame.keys_down.set(bit(key));
        frame.keys_pressed.set(bit(key));
    };
    if (index == 0) {
        press(platform::Key::F);
        press(platform::Key::D);
    } else if (index < 90) {
        frame.keys_down.set(bit(platform::Key::D));
    } else if (index == 90) {
        frame.keys_released.set(bit(platform::Key::D));
    }
    if (index == 40) {
        press(platform::Key::Space);
    } else if (index == 41) {
        frame.keys_released.set(bit(platform::Key::Space));
    }
    if (index == 180) {
        press(platform::Key::L);
    } else if (index == 181) {
        frame.keys_released.set(bit(platform::Key::L));
    }
    return frame;
}

// The character walks once per physics step, where the camera's WASD
// pointed this frame; a jump pressed between steps is latched until one runs.
struct CharacterStep {
    physics::CharacterController* character = nullptr;
    Vec3 walk = Vec3::zero();
    bool jump_latched = false;

    static void before_step(float step, void* user) {
        auto* self = static_cast<CharacterStep*>(user);
        self->character->move(self->walk, self->jump_latched, step);
        self->jump_latched = false;
    }
};

} // namespace

int main(int argc, char** argv) {
    TY_PROFILE_THREAD("main");
    Options options = parse_options(argc, argv);

    // A replay decides the physics backend and the frame count: the log
    // was recorded on one, for that many frames.
    platform::InputLog log;
    if (!options.replay.empty()) {
        std::string error;
        if (!log.load(options.replay.c_str(), error)) {
            std::fprintf(stderr, "--replay: %s\n", error.c_str());
            return kExitUsage;
        }
    }

    sdk::RuntimeDesc desc;
    desc.title = "tynima sandbox";
    desc.headless = options.headless;
    desc.backend = options.rhi;
    desc.jolt = options.jolt;
    desc.max_entities = 4096;
    desc.max_bodies = 1024;
    desc.game_module = TYNIMA_SANDBOX_GAME_MODULE;
    desc.max_frames = options.max_frames;
    desc.replay = options.replay.empty() ? nullptr : &log;
    desc.record = !options.record.empty();
    desc.scripted_input = scripted_frame;
    sdk::Runtime runtime;
    if (!runtime.create(desc)) {
        return 1;
    }
    // The game module owns the systems that make the scene move. It is a
    // shared library, and the runtime swaps it for a new build whenever one appears.
    if (runtime.game() != nullptr && runtime.game()->loaded()) {
        TY_LOG_INFO("game", "edit apps/sandbox/game/src/game.cpp, then: cmake --build --preset macos-debug "
                            "--target tynima_sandbox_game");
    }
    scene::World& world = runtime.world();
    physics::PhysicsWorld& physics = runtime.physics();
    {
        physics::BodyDesc floor;
        floor.shape = physics::Shape::box(Vec3{kFloorHalfWidth, kFloorHalfThickness, kFloorHalfWidth});
        floor.position = Vec3{0.0f, -kFloorHalfThickness, 0.0f}; // top face at y = 0
        floor.motion = physics::MotionType::Static;
        floor.friction = 0.6f;
        (void)physics.create_body(floor);
    }

    // The model comes through the runtime, cooked on the way in when its
    // blob is missing or stale, in every mode: the headless run covers the
    // cooker and the blob reader too. Its bounds are known without a GPU.
    const bool has_model = runtime.load_model(options.model.c_str()) == kModelBottle;
    Aabb bottle_bounds;
    if (has_model) {
        const render::Mesh& bottle = runtime.model(kModelBottle)->mesh;
        bottle_bounds = {bottle.bounds_min, bottle.bounds_max};
        // The runtime's model slots, in kModel* order.
        (void)runtime.add_model(make_floor_model());
        (void)runtime.add_model(make_plain_model(make_capsule_mesh(kCharacterRadius, kCharacterHalfHeight),
                                                 Vec4{0.20f, 0.45f, 0.85f, 1.0f}, 0.6f));
        (void)runtime.add_model(
            make_plain_model(make_box_mesh(kGateHalf), Vec4{0.55f, 0.36f, 0.20f, 1.0f}, 0.8f));
    } else {
        TY_LOG_WARN("model", "no %s - an empty sky, then", options.model.c_str());
    }
    TY_LOG_INFO("controls", "right-drag looks, WASD/QE fly, Shift runs, R re-drops the pile, "
                            "L launches it, Escape quits");
    TY_LOG_INFO("controls", "F follows the character: then WASD walk it, Space jumps, Shift runs");
    TY_LOG_INFO("controls", "1/2/3 unlit / Blinn-Phong / Cook-Torrance, N/M/O/V/B debug views, C shows the "
                            "shadow cascades, K the lights per cluster; X toggles shadows, P the point "
                            "lights, G bloom; T cycles the tonemapper, H the anti-aliasing, Y the shading "
                            "path; arrows move the sun");

    // The scene: the floor and a pile of the model, every one a rigid body.
    // The character stands at the edge of the pile; the gate and the chain
    // hang either side of it.
    Pile pile;
    physics::CharacterController character(
        physics,
        {.radius = kCharacterRadius, .half_height = kCharacterHalfHeight, .position = kCharacterStart});
    Rest rest;
    if (has_model) {
        (void)world.create(scene::Name("floor"),
                           scene::Transform{.position = Vec3{0.0f, -kFloorHalfThickness, 0.0f}},
                           scene::LocalToWorld{}, scene::MeshRenderer{.model = kModelFloor});
        pile = populate_pile(world, physics, bottle_bounds);
        rest = populate_rest(world, physics, character.body(), bottle_bounds);
        TY_LOG_INFO("scene", "%u entities in %u archetype(s), %u chunk(s); %u bodies, %u joints",
                    world.entity_count(), world.archetype_count(), world.chunk_count(), physics.body_count(),
                    physics.joint_count());
    }

    Shading shading;
    runtime.scene_settings.path = options.shading;
    runtime.sun.direction = shading.light_direction();
    FlyCamera fly;
    if (has_model) {
        // Frame where the pile lands, with the falling column in view above it.
        fly.frame(Vec3{0.0f, pile.drop_height * 0.3f, 0.0f},
                  std::max(pile.footprint * 2.2f, pile.drop_height * 0.6f));
    }
    runtime.camera = fly.camera;
    CharacterStep character_step{.character = &character};
    runtime.set_before_step(CharacterStep::before_step, &character_step);
    render::PointLight lights[kLightCount];

    while (runtime.begin_frame()) {
        const platform::Input& input = runtime.input();
        const float dt = runtime.dt();
        report_edges(input);
        if (input.key_pressed(platform::Key::Escape)) {
            runtime.quit();
        }
        if (input.key_pressed(platform::Key::R)) {
            reset_pile(world, physics, pile);
        }
        if (input.key_pressed(platform::Key::F)) {
            fly.follow = !fly.follow;
            TY_LOG_INFO("camera", "%s", fly.follow ? "following the character" : "flying free");
        }
        const Vec3 character_position = character.position();
        fly.update(input, *runtime.window(), dt, fly.follow ? &character_position : nullptr);
        runtime.camera = fly.camera;
        shading.update(runtime, input, dt);

        // The character walks where the camera's WASD point, when the camera
        // is following it.
        character_step.walk = Vec3::zero();
        if (fly.follow) {
            const bool run =
                input.key_down(platform::Key::LeftShift) || input.key_down(platform::Key::RightShift);
            character_step.walk = fly.walk_direction(input) * (kWalkSpeed * (run ? 2.0f : 1.0f));
            character_step.jump_latched =
                character_step.jump_latched || input.key_pressed(platform::Key::Space);
        }

        // The lights, which the game module knows nothing about; then the
        // runtime runs the module, the physics and the renderer.
        animate_lights(static_cast<float>(runtime.time()), lights, kLightCount);
        runtime.set_lights(lights, kLightCount);
        runtime.end_frame();
    }

    const auto state_hash = static_cast<unsigned long long>(physics.state_hash());
    const auto steps_taken = static_cast<unsigned long long>(runtime.stepper().total_steps);
    TY_LOG_INFO("sandbox", "ran %ld frames, %llu physics steps, %u bodies awake, state hash %016llx",
                runtime.frame_index(), steps_taken, physics.active_body_count(), state_hash);
    if (has_model) {
        report_pile(world, pile);
        report_rest(world, physics, character, rest);
    }
    int exit_code = 0;
    if (!options.save_scene.empty()) {
        std::string error;
        if (scene::save_scene_file(world, options.save_scene.c_str(), error)) {
            TY_LOG_INFO("scene", "saved %u entities to %s", world.entity_count(), options.save_scene.c_str());
        } else {
            TY_LOG_ERROR("scene", "%s", error.c_str());
            exit_code = 1;
        }
    }
    if (!options.record.empty()) {
        const platform::InputLog recorded = runtime.recording();
        std::string error;
        if (recorded.save(options.record.c_str(), error)) {
            TY_LOG_INFO("replay", "recorded %zu frames to %s", recorded.frames.size(),
                        options.record.c_str());
        } else {
            TY_LOG_ERROR("replay", "%s", error.c_str());
            exit_code = 1;
        }
    } else if (!options.replay.empty()) {
        if (!has_model) {
            // Without the pile there is nothing the log was about: not a failure, not a pass.
            TY_LOG_WARN("replay", "no model, so no pile to replay: skipped");
            exit_code = kExitSkipped;
        } else if (!log.has_end) {
            TY_LOG_WARN("replay", "the log has no end hash to compare with (an unfinished recording)");
        } else if (log.state_hash == state_hash && log.steps == runtime.stepper().total_steps) {
            TY_LOG_INFO("replay", "%zu frames replayed: %llu steps and state hash %016llx, as recorded",
                        log.frames.size(), steps_taken, state_hash);
        } else {
            TY_LOG_ERROR("replay",
                         "the replay diverged: %llu steps and state hash %016llx; recorded %llu "
                         "steps and %016llx (on %s)",
                         steps_taken, state_hash, static_cast<unsigned long long>(log.steps),
                         static_cast<unsigned long long>(log.state_hash), log.backend.c_str());
            exit_code = kExitReplayMismatch;
        }
    }
    // The character controller holds its body in the runtime's physics world:
    // it goes first, and the runtime last, as the destructors run.
    return exit_code;
}
