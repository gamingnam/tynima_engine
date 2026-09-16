// Engine developer's playground. A pile of glTF models dropped on a floor
// by the physics world, as entities in the scene World, lit with
// Cook-Torrance, a reverse-Z depth buffer, an sRGB swapchain, and a fly
// camera. Falls back to Phase 0's triangle when there is no model.
//
//   tynima-sandbox [--headless] [--frames N] [--model path.glb] [--physics tynima|jolt]
//                  [--record log.tyrec | --replay log.tyrec]
//
// The pile runs on the engine's own physics by default; --physics jolt
// drops the same pile through Jolt, and the report line at exit compares.
// --headless runs without a window or a GPU, at a fixed sixtieth of a
// second a frame, with a scripted player at the keyboard.
//
// --record writes every frame's input and frame time, and the physics
// world's hash at the end, to a text log; --replay feeds that log back in
// place of the clock and the keyboard and checks the run ends on the same
// hash — the simulation is deterministic, so it must, and CI replays a
// recorded pile to prove it (exit code 3 when it does not).
//
// Controls: hold the right mouse button to look; W/A/S/D move, Q/E descend
// and climb, Shift runs; R drops the pile again; L (the game module)
// launches it; F follows the character; C shows the shadow cascades, X
// toggles shadows; Escape quits.
#include <tynima/assets/gltf.h>
#include <tynima/core/arena.h>
#include <tynima/core/assert.h>
#include <tynima/core/jobs.h>
#include <tynima/core/log.h>
#include <tynima/core/memory.h>
#include <tynima/core/profile.h>
#include <tynima/core/version.h>
#include <tynima/physics/character.h>
#include <tynima/physics/fixed_step.h>
#include <tynima/physics/physics.h>
#include <tynima/platform/events.h>
#include <tynima/platform/input.h>
#include <tynima/platform/input_log.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/platform/window.h>
#include <tynima/render/camera.h>
#include <tynima/render/frame_graph.h>
#include <tynima/render/mesh.h>
#include <tynima/render/model.h>
#include <tynima/render/shadows.h>
#include <tynima/rhi/device.h>
#include <tynima/scene/components.h>
#include <tynima/scene/systems.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/game_module.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace platform = tynima::platform;
namespace rhi = tynima::rhi;
namespace render = tynima::render;
namespace assets = tynima::assets;
namespace scene = tynima::scene;
namespace physics = tynima::physics;
using namespace tynima::math;

namespace {

// Model slots the MeshRenderer component indexes into.
constexpr std::uint32_t kModelBottle = 0;
constexpr std::uint32_t kModelFloor = 1;
constexpr std::uint32_t kModelCharacter = 2; // the capsule the controller walks around in
constexpr std::uint32_t kModelGate = 3;      // a slab on a hinge
constexpr std::uint32_t kModelCount = 4;

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
Pile populate_pile(scene::World& world, physics::PhysicsWorld& physics, const render::MeshData& mesh) {
    Pile pile;
    const Vec3 size = mesh.bounds_max - mesh.bounds_min;
    const Vec3 half_extents = size * 0.5f;
    const Vec3 center = (mesh.bounds_min + mesh.bounds_max) * 0.5f;
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
                                        mesh.bounds_min.y,
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
                const scene::Entity entity = world.create(pose, scene::LocalToWorld{},
                                                          scene::MeshRenderer{.model = kModelBottle},
                                                          scene::RigidBody{handle});
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
                   const render::MeshData& bottle) {
    Rest rest;
    rest.character_entity = world.create(scene::Transform{.position = kCharacterStart}, scene::LocalToWorld{},
                                         scene::MeshRenderer{.model = kModelCharacter},
                                         scene::RigidBody{character_body});
    {
        physics::BodyDesc gate;
        gate.shape = physics::Shape::box(kGateHalf);
        gate.position = kGatePin + Vec3{0.0f, 0.0f, kGateHalf.z};
        gate.mass = 15.0f;
        gate.friction = 0.4f;
        const physics::BodyHandle body = physics.create_body(gate);
        (void)world.create(scene::Transform{.position = gate.position}, scene::LocalToWorld{},
                           scene::MeshRenderer{.model = kModelGate}, scene::RigidBody{body});
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
        const Vec3 size = bottle.bounds_max - bottle.bounds_min;
        const Vec3 center = (bottle.bounds_min + bottle.bounds_max) * 0.5f;
        const Vec3 top{center.x, bottle.bounds_max.y, center.z};
        const Vec3 bottom{center.x, bottle.bounds_min.y, center.z};
        physics::BodyHandle above;
        float hang = kChainTop.y;
        for (int i = 0; i < kChainLinks; ++i) {
            hang -= kChainGap;
            physics::BodyDesc body;
            body.shape = physics::Shape::box(size * 0.5f, center);
            body.position = Vec3{kChainTop.x, hang - bottle.bounds_max.y, kChainTop.z};
            body.mass = 0.6f;
            body.friction = 0.5f;
            const physics::BodyHandle handle = physics.create_body(body);
            const scene::Entity entity =
                world.create(scene::Transform{.position = body.position}, scene::LocalToWorld{},
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

#ifndef NDEBUG
constexpr bool kGpuDebug = true; // Metal validation: catches API misuse loudly
#else
constexpr bool kGpuDebug = false;
#endif

#ifndef TYNIMA_SANDBOX_ASSETS_DIR
#define TYNIMA_SANDBOX_ASSETS_DIR "."
#endif

struct Options {
    bool headless = false;
    long max_frames = -1; // -1: run until closed
    std::string model = TYNIMA_SANDBOX_ASSETS_DIR "/WaterBottle.glb";
    bool jolt = false;   // the reference physics instead of the engine's own
    std::string record; // write the input log here at exit
    std::string replay; // play this input log instead of live input and time
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
        } else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            options.record = argv[++i];
        } else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            options.replay = argv[++i];
        } else {
            std::fprintf(stderr, "usage: tynima-sandbox [--headless] [--frames N] [--model path.glb] "
                                 "[--physics tynima|jolt] [--record log.tyrec | --replay log.tyrec]\n");
            std::exit(kExitUsage);
        }
    }
    if (!options.record.empty() && !options.replay.empty()) {
        std::fprintf(stderr, "--record and --replay are one or the other\n");
        std::exit(kExitUsage);
    }
    return options;
}

// ------------------------------------------------------------------ shaders

// Lit, textured mesh with two shading models and a set of debug views.
// Vertex uniforms at [[buffer(0)]]; attributes come from render::vertex_layout()
// through [[stage_in]]. Fragment uniforms: frame at [[buffer(0)]], material at
// [[buffer(1)]]; the four material maps at [[texture(0..3)]] share sampler 0.
// Every texel is linear by the time it is sampled (sRGB formats decode in
// hardware), lighting happens in linear, and the swapchain encodes on the way
// out. Blinn-Phong is here for comparison; Cook-Torrance is the real thing.
constexpr const char* kMeshMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
    float4 tangent  [[attribute(3)]]; // xyz along +u, w = handedness
};

struct Uniforms {
    float4x4 mvp;
    float4x4 model;
};

struct VSOut {
    float4 position [[position]];
    float3 world_position;
    float3 world_normal;
    float4 world_tangent;
    float2 uv;
};

vertex VSOut vs_main(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {
    VSOut out;
    float4 world = u.model * float4(in.position, 1.0);
    out.position = u.mvp * float4(in.position, 1.0);
    out.world_position = world.xyz;
    out.world_normal = (u.model * float4(in.normal, 0.0)).xyz;
    out.world_tangent = float4((u.model * float4(in.tangent.xyz, 0.0)).xyz, in.tangent.w);
    out.uv = in.uv;
    return out;
}

struct FrameUniforms {
    float4 camera_position; // xyz
    float4 light_direction; // xyz: towards the light; w: intensity
    float4 light_color;     // rgb; w: ambient intensity
    float4 params;          // x: shading model, y: debug view
};

struct MaterialUniforms {
    float4 base_color_factor;
    float4 factors;         // x: metallic, y: roughness, z: occlusion strength, w: normal scale
    float4 emissive_factor; // rgb
};

struct ShadowUniforms {
    float4x4 cascade_matrix[4]; // world -> light clip, reverse-Z
    float4 cascade_param[4];    // x: texel size (m), y: depth per metre, z: texel size in uv
    float4 settings;            // x: cascades, y: strength (0: off), z: normal offset, w: bias (in texels)
};

constant float kPi = 3.14159265358979;

// 3x3 taps of the hardware's own 2x2 compare: a soft edge four texels wide.
float shadow_pcf(depth2d<float> map, sampler s, float2 uv, float depth, float texel_uv) {
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += map.sample_compare(s, uv + float2(x, y) * texel_uv, depth);
        }
    }
    return sum / 9.0;
}

// GGX / Trowbridge-Reitz normal distribution: how many microfacets face h.
float d_ggx(float n_dot_h, float alpha) {
    float a2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility (includes the 1 / (4 n.l n.v) term).
float v_smith_ggx_correlated(float n_dot_v, float n_dot_l, float alpha) {
    float a2 = alpha * alpha;
    float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// Schlick's Fresnel: reflectance rises to 1 at grazing angles.
float3 f_schlick(float v_dot_h, float3 f0) {
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (float3(1.0) - f0) * f;
}

// Linear light out, into an HDR target: the tonemap pass makes it a picture.
fragment float4 fs_main(VSOut in [[stage_in]],
                        texture2d<float> base_color_map [[texture(0)]],
                        texture2d<float> metallic_roughness_map [[texture(1)]],
                        texture2d<float> occlusion_map [[texture(2)]],
                        texture2d<float> emissive_map [[texture(3)]],
                        texture2d<float> normal_map [[texture(4)]],
                        depth2d<float> shadow_map0 [[texture(5)]],
                        depth2d<float> shadow_map1 [[texture(6)]],
                        depth2d<float> shadow_map2 [[texture(7)]],
                        depth2d<float> shadow_map3 [[texture(8)]],
                        sampler map_sampler [[sampler(0)]],
                        sampler shadow_sampler [[sampler(5)]],
                        constant FrameUniforms& frame [[buffer(0)]],
                        constant MaterialUniforms& m [[buffer(1)]],
                        constant ShadowUniforms& shadows [[buffer(2)]]) {
    float4 base = base_color_map.sample(map_sampler, in.uv) * m.base_color_factor;
    float4 mr = metallic_roughness_map.sample(map_sampler, in.uv);
    float metallic = mr.b * m.factors.x;                        // glTF: metallic in B
    float roughness = clamp(mr.g * m.factors.y, 0.045, 1.0);    // roughness in G; 0 would make GGX blow up
    float occlusion = mix(1.0, occlusion_map.sample(map_sampler, in.uv).r, m.factors.z);
    float3 emissive = emissive_map.sample(map_sampler, in.uv).rgb * m.emissive_factor.rgb;

    // Tangent frame: re-orthogonalize the interpolated tangent against the
    // interpolated normal, then the bitangent follows from the handedness.
    float3 geometric_n = normalize(in.world_normal);
    float3 t = in.world_tangent.xyz;
    t = normalize(t - geometric_n * dot(geometric_n, t));
    float3 b = cross(geometric_n, t) * (in.world_tangent.w < 0.0 ? -1.0 : 1.0);
    float3 nm = normal_map.sample(map_sampler, in.uv).xyz * 2.0 - 1.0; // tangent space, +z straight up
    nm.xy *= m.factors.w;
    float3 n = normalize(t * nm.x + b * nm.y + geometric_n * nm.z);

    float3 v = normalize(frame.camera_position.xyz - in.world_position);
    float3 l = normalize(frame.light_direction.xyz);
    float3 h = normalize(l + v);
    float n_dot_l = saturate(dot(n, l));
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = saturate(dot(n, h));
    float v_dot_h = saturate(dot(v, h));
    float3 radiance = frame.light_color.rgb * frame.light_direction.w;
    float ambient = frame.light_color.w;

    // Shadow: from the first cascade whose map holds this point. The lookup
    // point is pushed off the surface along the normal, more where the light
    // grazes it, and the depth nudged towards the light — the two biases
    // that keep a lit surface from shadowing itself.
    float shadow = 1.0;
    int cascade = -1;
    if (shadows.settings.y > 0.0) {
        float grazing = 1.0 - saturate(dot(geometric_n, l));
        for (int c = 0; c < int(shadows.settings.x); ++c) {
            float4 param = shadows.cascade_param[c];
            float3 lookup = in.world_position + geometric_n * (param.x * shadows.settings.z * grazing);
            float4 lc = shadows.cascade_matrix[c] * float4(lookup, 1.0);
            float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);
            float border = param.z * 2.0; // the taps must stay inside the map
            bool inside = uv.x > border && uv.x < 1.0 - border && uv.y > border && uv.y < 1.0 - border;
            if (inside && lc.z > 0.0 && lc.z < 1.0) {
                float depth = lc.z + param.x * shadows.settings.w * param.y;
                if (c == 0) {
                    shadow = shadow_pcf(shadow_map0, shadow_sampler, uv, depth, param.z);
                } else if (c == 1) {
                    shadow = shadow_pcf(shadow_map1, shadow_sampler, uv, depth, param.z);
                } else if (c == 2) {
                    shadow = shadow_pcf(shadow_map2, shadow_sampler, uv, depth, param.z);
                } else {
                    shadow = shadow_pcf(shadow_map3, shadow_sampler, uv, depth, param.z);
                }
                cascade = c;
                break;
            }
        }
        shadow = 1.0 - (1.0 - shadow) * shadows.settings.y;
    }

    int mode = int(frame.params.x);
    int debug_view = int(frame.params.y);
    float3 color;
    if (debug_view == 1) {
        color = n * 0.5 + 0.5;
    } else if (debug_view == 2) {
        color = float3(metallic, roughness, 0.0);
    } else if (debug_view == 3) {
        color = float3(occlusion);
    } else if (debug_view == 4) {
        color = geometric_n * 0.5 + 0.5;
    } else if (debug_view == 5) {
        color = t * 0.5 + 0.5;
    } else if (mode == 0) {
        color = base.rgb;
    } else if (mode == 1) {
        // Normalized Blinn-Phong. Shininess is derived from roughness so both
        // models agree on how glossy a surface is; what differs is the shape
        // of the highlight and the missing Fresnel and masking terms.
        float alpha = roughness * roughness;
        float shininess = 2.0 / (alpha * alpha) - 2.0;
        float3 spec_color = mix(float3(0.04), base.rgb, metallic);
        float3 diffuse = base.rgb * (1.0 - metallic) / kPi;
        float3 specular = spec_color * pow(n_dot_h, shininess) * (shininess + 8.0) / (8.0 * kPi);
        float3 direct = (diffuse + specular) * radiance * n_dot_l * shadow;
        color = direct + base.rgb * ambient * occlusion + emissive;
    } else {
        // Cook-Torrance with the metallic workflow: dielectrics reflect 4% at
        // normal incidence and keep their albedo as diffuse; metals reflect
        // their albedo as specular and have no diffuse at all.
        float alpha = roughness * roughness;
        float3 f0 = mix(float3(0.04), base.rgb, metallic);
        float3 diffuse_color = base.rgb * (1.0 - metallic);
        float3 f = f_schlick(v_dot_h, f0);
        float d = d_ggx(n_dot_h, alpha);
        float vis = v_smith_ggx_correlated(n_dot_v, n_dot_l, alpha);
        float3 specular = d * vis * f;
        float3 diffuse = (float3(1.0) - f) * diffuse_color / kPi;
        float3 direct = (diffuse + specular) * radiance * n_dot_l * shadow;
        // A uniform grey environment stands in for image-based lighting (Phase 4).
        float3 indirect = (diffuse_color + f0 * 0.5) * ambient * occlusion;
        color = direct + indirect + emissive;
    }
    if (debug_view == 6) {
        // Which cascade shadowed the pixel: red, green, blue, yellow; grey for none.
        float3 tints[5] = {float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.2), float3(0.2, 0.4, 1.0),
                           float3(1.0, 1.0, 0.2), float3(0.5)};
        color = mix(color, tints[cascade < 0 ? 4 : cascade], 0.4);
    }
    return float4(color, base.a);
}
)";

// The shadow map pass: the casters' depth from the light and nothing else.
// The pipeline has no color target, so the fragment function returns none.
constexpr const char* kShadowMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct Uniforms {
    float4x4 mvp;
    float4x4 model;
};

vertex float4 vs_main(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {
    return u.mvp * float4(in.position, 1.0);
}

fragment void fs_main() {}
)";

// The post pass: one triangle over the screen, the HDR image sampled once
// per pixel, ACES to bring it into range, and the sRGB encode when the
// swapchain does not do it in hardware.
constexpr const char* kTonemapMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

// Three vertices that cover the screen: (-1,-1), (3,-1), (-1,3) in clip
// space; the parts past the edges are clipped away.
vertex VSOut vs_main(uint vid [[vertex_id]]) {
    float2 corner = float2((vid << 1) & 2, vid & 2);
    VSOut out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(corner.x, 1.0 - corner.y); // texture rows run top to bottom
    return out;
}

struct PostUniforms {
    float4 params; // x: tonemap, y: encode sRGB
};

// Narkowicz's ACES fit.
float3 tonemap_aces(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

fragment float4 fs_main(VSOut in [[stage_in]], texture2d<float> hdr [[texture(0)]], sampler s [[sampler(0)]],
                        constant PostUniforms& post [[buffer(0)]]) {
    float3 color = hdr.sample(s, in.uv).rgb;
    if (post.params.x > 0.5) {
        color = tonemap_aces(color);
    }
    if (post.params.y > 0.5) {
        color = pow(max(color, 0.0), float3(1.0 / 2.2));
    }
    return float4(color, 1.0);
}
)";

// Phase 0's triangle, kept as the fallback when there is nothing to load.
constexpr const char* kTriangleMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float4 color;
};

constant float2 kPositions[3] = { float2(-0.6, -0.5), float2(0.6, -0.5), float2(0.0, 0.6) };
constant float3 kColors[3]    = { float3(0.91, 0.64, 0.14), float3(0.29, 0.77, 0.87), float3(0.94, 0.94, 0.96) };

vertex VSOut vs_main(uint vid [[vertex_id]]) {
    VSOut out;
    out.position = float4(kPositions[vid], 0.0, 1.0);
    out.color = float4(kColors[vid], 1.0);
    return out;
}

fragment float4 fs_main(VSOut in [[stage_in]]) {
    return in.color;
}
)";

// Mirrors `struct Uniforms` in kMeshMsl: two column-major float4x4, 128 bytes.
struct MeshUniforms {
    Mat4 mvp;
    Mat4 model;
};
static_assert(sizeof(MeshUniforms) == 128, "matches the MSL Uniforms struct");

// Mirror the MSL uniform structs: float4 members only, so C++ and Metal agree on layout.
struct FrameUniforms {
    Vec4 camera_position;
    Vec4 light_direction;
    Vec4 light_color;
    Vec4 params;
};
static_assert(sizeof(FrameUniforms) == 64, "matches the MSL FrameUniforms struct");

struct MaterialUniforms {
    Vec4 base_color_factor;
    Vec4 factors;
    Vec4 emissive_factor;
};
static_assert(sizeof(MaterialUniforms) == 48, "matches the MSL MaterialUniforms struct");

struct PostUniforms {
    Vec4 params;
};
static_assert(sizeof(PostUniforms) == 16, "matches the MSL PostUniforms struct");

struct ShadowUniforms {
    Mat4 cascade_matrix[render::kMaxCascades];
    Vec4 cascade_param[render::kMaxCascades];
    Vec4 settings;
};
static_assert(sizeof(ShadowUniforms) == 336, "matches the MSL ShadowUniforms struct");

// What the keys toggle. Printed whenever it changes.
struct Shading {
    int model = 2;      // 0 unlit, 1 Blinn-Phong, 2 Cook-Torrance
    int debug_view = 0; // 0 lit, 1 shading normals, 2 metallic/roughness, 3 occlusion, 4 vertex normals,
                        // 5 tangents, 6 lit with the shadow cascades tinted
    bool tonemap = true;
    bool shadows = true;
    float light_azimuth = radians(35.0f);   // around +y, from +z
    float light_elevation = radians(50.0f); // above the horizon
    float light_intensity = 3.0f;           // linear radiance; >1 is what tonemapping is for
    float ambient = 0.10f;

    Vec3 light_direction() const {
        return {std::cos(light_elevation) * std::sin(light_azimuth), std::sin(light_elevation),
                std::cos(light_elevation) * std::cos(light_azimuth)};
    }

    // Debug views 0 and 6 are lit images and go through the tonemapper; the
    // rest are data and go to the screen as they are.
    [[nodiscard]] bool view_is_lit() const { return debug_view == 0 || debug_view == 6; }

    void print() const {
        static constexpr const char* kModels[] = {"unlit", "blinn-phong", "cook-torrance"};
        static constexpr const char* kViews[] = {"lit",       "shading normals (mapped)",
                                                 "metallic (r) / roughness (g)",
                                                 "occlusion", "vertex normals",
                                                 "tangents",  "shadow cascades"};
        float azimuth = std::fmod(degrees(light_azimuth), 360.0f);
        if (azimuth < 0.0f) azimuth += 360.0f;
        TY_LOG_INFO("shading", "%s | view %s | tonemap %s | shadows %s | light az %.0f el %.0f",
                    kModels[model], kViews[debug_view], tonemap ? "aces" : "off", shadows ? "on" : "off",
                    static_cast<double>(azimuth), static_cast<double>(degrees(light_elevation)));
    }

    // Returns true when something changed.
    bool update(const platform::Input& input, float dt) {
        using platform::Key;
        const int old_model = model, old_view = debug_view;
        const bool old_tonemap = tonemap, old_shadows = shadows;
        if (input.key_pressed(Key::Digit1)) model = 0;
        if (input.key_pressed(Key::Digit2)) model = 1;
        if (input.key_pressed(Key::Digit3)) model = 2;
        if (input.key_pressed(Key::N)) debug_view = debug_view == 1 ? 0 : 1;
        if (input.key_pressed(Key::M)) debug_view = debug_view == 2 ? 0 : 2;
        if (input.key_pressed(Key::O)) debug_view = debug_view == 3 ? 0 : 3;
        if (input.key_pressed(Key::V)) debug_view = debug_view == 4 ? 0 : 4;
        if (input.key_pressed(Key::B)) debug_view = debug_view == 5 ? 0 : 5;
        if (input.key_pressed(Key::C)) debug_view = debug_view == 6 ? 0 : 6;
        if (input.key_pressed(Key::Digit0)) debug_view = 0;
        if (input.key_pressed(Key::T)) tonemap = !tonemap;
        if (input.key_pressed(Key::X)) shadows = !shadows;
        const float turn = radians(60.0f) * dt;
        bool moved = false;
        if (input.key_down(Key::Left)) { light_azimuth -= turn; moved = true; }
        if (input.key_down(Key::Right)) { light_azimuth += turn; moved = true; }
        if (input.key_down(Key::Up)) { light_elevation = std::min(light_elevation + turn, radians(89.0f)); moved = true; }
        if (input.key_down(Key::Down)) { light_elevation = std::max(light_elevation - turn, radians(-10.0f)); moved = true; }
        const bool changed =
            model != old_model || debug_view != old_view || tonemap != old_tonemap || shadows != old_shadows;
        if (changed) print();
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
        if (input.key_down(platform::Key::W)) move += forward;
        if (input.key_down(platform::Key::S)) move -= forward;
        if (input.key_down(platform::Key::D)) move += right;
        if (input.key_down(platform::Key::A)) move -= right;
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
        camera.rotation = Quat::from_axis_angle(Vec3::unit_y(), yaw) * Quat::from_axis_angle(Vec3::unit_x(), pitch);
        if (follow_target != nullptr) {
            // Behind and a little above the character, looking at its head.
            camera.position = *follow_target + kFollowFocus - camera.forward() * kFollowDistance;
            return;
        }

        Vec3 move = Vec3::zero();
        if (input.key_down(platform::Key::W)) move += camera.forward();
        if (input.key_down(platform::Key::S)) move -= camera.forward();
        if (input.key_down(platform::Key::D)) move += camera.right();
        if (input.key_down(platform::Key::A)) move -= camera.right();
        if (input.key_down(platform::Key::E)) move += Vec3::unit_y();
        if (input.key_down(platform::Key::Q)) move -= Vec3::unit_y();
        if (length_squared(move) > 0.0f) {
            const bool run = input.key_down(platform::Key::LeftShift) || input.key_down(platform::Key::RightShift);
            camera.position += normalize(move) * (speed * (run ? 4.0f : 1.0f) * dt);
        }
    }
};

// ----------------------------------------------------------------- renderer

// The frame's passes draw the scene into this format; the tonemap pass
// brings it down to the swapchain's.
constexpr rhi::TextureFormat kHdrFormat = rhi::TextureFormat::Rgba16Float;
constexpr rhi::ClearColor kSkyColor{.r = 0.09f, .g = 0.10f, .b = 0.12f};

// The sun's shadow: four cascades out to 60 m, the lookup pushed two texels
// off the surface and one texel towards the light against acne.
constexpr render::CascadeSettings kCascades{.count = 4, .map_size = 2048, .max_distance = 60.0f};
constexpr float kShadowNormalOffsetTexels = 2.0f;
constexpr float kShadowBiasTexels = 1.0f;
constexpr std::uint32_t kShadowTextureSlot = 5; // after the five material maps
constexpr const char* kShadowPassNames[render::kMaxCascades] = {"shadow 0", "shadow 1", "shadow 2",
                                                                 "shadow 3"};

struct Renderer {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::FrameGraph> graph; // owns the frame's transient textures
    rhi::PipelineHandle mesh_pipeline;
    rhi::PipelineHandle triangle_pipeline;
    rhi::PipelineHandle tonemap_pipeline;
    rhi::PipelineHandle shadow_pipeline; // depth only
    render::FallbackTextures fallbacks;
    rhi::SamplerHandle sampler;        // materials: anisotropic, repeating
    rhi::SamplerHandle post_sampler;   // the HDR image: one texel per pixel, clamped
    rhi::SamplerHandle shadow_sampler; // the shadow maps: compared, bilinear, clamped
    rhi::TextureHandle shadow_fallback; // a 1x1 depth texture for the shadow slots when there are no maps
    render::Model models[kModelCount]; // by kModel* slot
    bool shadows = false; // the device can sample its depth format and the shadow pipeline built

    Renderer() = default;
    Renderer(Renderer&& other) noexcept
        : device(std::move(other.device)), graph(std::move(other.graph)),
          mesh_pipeline(std::exchange(other.mesh_pipeline, {})),
          triangle_pipeline(std::exchange(other.triangle_pipeline, {})),
          tonemap_pipeline(std::exchange(other.tonemap_pipeline, {})),
          shadow_pipeline(std::exchange(other.shadow_pipeline, {})),
          fallbacks(std::exchange(other.fallbacks, render::FallbackTextures{})),
          sampler(std::exchange(other.sampler, {})), post_sampler(std::exchange(other.post_sampler, {})),
          shadow_sampler(std::exchange(other.shadow_sampler, {})),
          shadow_fallback(std::exchange(other.shadow_fallback, {})), shadows(other.shadows) {
        for (std::uint32_t i = 0; i < kModelCount; ++i) {
            models[i] = std::exchange(other.models[i], render::Model{});
        }
    }
    Renderer& operator=(Renderer&&) = delete;
    ~Renderer() { destroy(); }

    // GPU objects go before their device, and the device before the window.
    void destroy() noexcept {
        if (device != nullptr) {
            graph.reset(); // returns its textures first
            for (render::Model& model : models) {
                render::destroy_model(*device, model);
            }
            device->destroy_sampler(sampler);
            device->destroy_sampler(post_sampler);
            device->destroy_sampler(shadow_sampler);
            device->destroy_texture(shadow_fallback);
            render::destroy_fallback_textures(*device, fallbacks);
            device->destroy_graphics_pipeline(mesh_pipeline);
            device->destroy_graphics_pipeline(triangle_pipeline);
            device->destroy_graphics_pipeline(tonemap_pipeline);
            device->destroy_graphics_pipeline(shadow_pipeline);
            mesh_pipeline = triangle_pipeline = tonemap_pipeline = shadow_pipeline = {};
            sampler = post_sampler = shadow_sampler = {};
            shadow_fallback = {};
            device.reset();
        }
    }
};

rhi::PipelineHandle make_pipeline(rhi::Device& device, const char* msl, const rhi::GraphicsPipelineDesc& base,
                                  std::uint32_t vertex_uniforms, std::uint32_t fragment_uniforms,
                                  std::uint32_t fragment_samplers) {
    const rhi::ShaderDesc common{.format = rhi::ShaderFormat::Msl, .code = msl, .code_size = std::strlen(msl)};
    rhi::ShaderDesc vs_desc = common;
    vs_desc.stage = rhi::ShaderStage::Vertex;
    vs_desc.entry_point = "vs_main";
    vs_desc.num_uniform_buffers = vertex_uniforms;
    rhi::ShaderDesc fs_desc = common;
    fs_desc.stage = rhi::ShaderStage::Fragment;
    fs_desc.entry_point = "fs_main";
    fs_desc.num_uniform_buffers = fragment_uniforms;
    fs_desc.num_samplers = fragment_samplers;

    const rhi::ShaderHandle vs = device.create_shader(vs_desc);
    const rhi::ShaderHandle fs = device.create_shader(fs_desc);
    rhi::PipelineHandle pipeline;
    if (vs && fs) {
        rhi::GraphicsPipelineDesc desc = base;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        pipeline = device.create_graphics_pipeline(desc);
    }
    if (!pipeline) {
        TY_LOG_ERROR("gpu", "pipeline failed: %s", platform::last_error());
    }
    device.destroy_shader(vs); // the pipeline holds what it needs
    device.destroy_shader(fs);
    return pipeline;
}

// Everything GPU-side. On failure the sandbox keeps running without drawing,
// and says why — a missing backend is a message, not a crash.
Renderer create_renderer(platform::Window& window, const render::ModelData* model_data) {
    Renderer r;
    r.device = rhi::Device::create({.debug = kGpuDebug});
    if (r.device == nullptr) {
        TY_LOG_ERROR("gpu", "unavailable: %s", platform::last_error());
        return r;
    }
    TY_LOG_INFO("gpu", "%s, wants %s shaders, depth %s", r.device->backend_name(),
                rhi::shader_format_name(r.device->shader_format()),
                rhi::texture_format_name(r.device->preferred_depth_format()));
    if (!r.device->attach_window(window)) {
        TY_LOG_ERROR("gpu", "cannot present to this window: %s", platform::last_error());
        r.device.reset();
        return r;
    }
    TY_LOG_INFO("swap", "%s", r.device->swapchain_is_linear() ? "sRGB-encoded by the display hardware"
                                                             : "plain SDR; the shader encodes sRGB itself");
    r.graph = std::make_unique<render::FrameGraph>(r.device.get());
    if (r.device->shader_format() != rhi::ShaderFormat::Msl) {
        TY_LOG_WARN("gpu", "the sandbox only carries MSL until SDL_shadercross lands; drawing nothing");
        return r;
    }

    // The scene pass draws into HDR with depth; the tonemap pass draws into
    // the swapchain with neither.
    rhi::GraphicsPipelineDesc mesh_desc;
    mesh_desc.vertex_layout = render::vertex_layout();
    mesh_desc.cull = rhi::CullMode::Back;
    mesh_desc.depth = {.test = true, .write = true, .compare = rhi::CompareOp::Greater}; // reverse-Z
    mesh_desc.color_formats[0] = kHdrFormat;
    mesh_desc.color_target_count = 1;
    mesh_desc.depth_format = r.device->preferred_depth_format();
    r.mesh_pipeline =
        make_pipeline(*r.device, kMeshMsl, mesh_desc, 1, 3, kShadowTextureSlot + render::kMaxCascades);

    rhi::GraphicsPipelineDesc triangle_desc;
    triangle_desc.color_formats[0] = kHdrFormat; // same pass, so the same targets, even with the test off
    triangle_desc.color_target_count = 1;
    triangle_desc.depth_format = r.device->preferred_depth_format();
    r.triangle_pipeline = make_pipeline(*r.device, kTriangleMsl, triangle_desc, 0, 0, 0);

    rhi::GraphicsPipelineDesc tonemap_desc;
    tonemap_desc.color_formats[0] = r.device->swapchain_format();
    tonemap_desc.color_target_count = 1;
    r.tonemap_pipeline = make_pipeline(*r.device, kTonemapMsl, tonemap_desc, 0, 1, 1);

    // Shadows need the depth format to be sampled as well as drawn into.
    const rhi::TextureFormat depth_format = r.device->preferred_depth_format();
    if (r.device->supports_texture(depth_format, rhi::TextureUsage::DepthStencilTarget |
                                                     rhi::TextureUsage::Sampled)) {
        rhi::GraphicsPipelineDesc shadow_desc;
        shadow_desc.vertex_layout = render::vertex_layout();
        shadow_desc.cull = rhi::CullMode::None; // thin things cast from both sides
        shadow_desc.depth = {.test = true, .write = true, .compare = rhi::CompareOp::Greater};
        shadow_desc.depth_format = depth_format;
        r.shadow_pipeline = make_pipeline(*r.device, kShadowMsl, shadow_desc, 1, 0, 0);
        r.shadow_sampler = r.device->create_sampler({.address_u = rhi::AddressMode::ClampToEdge,
                                                     .address_v = rhi::AddressMode::ClampToEdge,
                                                     .compare = rhi::CompareOp::GreaterEqual});
        // The scene shader declares its shadow maps whether or not there are
        // any this frame, so the slots always need a depth texture in them.
        r.shadow_fallback = r.device->create_texture(
            {.format = depth_format,
             .width = 1,
             .height = 1,
             .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled});
        r.shadows = r.shadow_pipeline && r.shadow_sampler && r.shadow_fallback;
    }
    if (!r.shadows) {
        TY_LOG_WARN("gpu", "no shadows: %s cannot be sampled here, or the shadow pipeline failed",
                    rhi::texture_format_name(depth_format));
    }

    r.sampler = r.device->create_sampler({.max_anisotropy = 8.0f});
    r.post_sampler = r.device->create_sampler({.min_filter = rhi::Filter::Nearest,
                                               .mag_filter = rhi::Filter::Nearest,
                                               .address_u = rhi::AddressMode::ClampToEdge,
                                               .address_v = rhi::AddressMode::ClampToEdge});
    if (!render::create_fallback_textures(*r.device, r.fallbacks) || !r.sampler || !r.post_sampler) {
        TY_LOG_ERROR("gpu", "fallback textures or sampler failed: %s", platform::last_error());
        return r;
    }
    if (model_data != nullptr) {
        const double t0 = platform::now_seconds();
        if (!render::upload_model(*r.device, *model_data, r.fallbacks, r.models[kModelBottle])) {
            TY_LOG_ERROR("gpu", "model upload failed: %s", platform::last_error());
        } else {
            std::size_t uploaded = 0;
            for (const rhi::TextureHandle t : r.models[kModelBottle].textures) {
                uploaded += t ? 1 : 0;
            }
            TY_LOG_INFO("gpu", "%zu of %zu textures uploaded with mipmaps in %.2f s", uploaded,
                        r.models[kModelBottle].textures.size(), platform::now_seconds() - t0);
        }
        const render::ModelData floor = make_floor_model();
        if (!render::upload_model(*r.device, floor, r.fallbacks, r.models[kModelFloor])) {
            TY_LOG_ERROR("gpu", "floor upload failed: %s", platform::last_error());
        }
        const render::ModelData character = make_plain_model(
            make_capsule_mesh(kCharacterRadius, kCharacterHalfHeight), Vec4{0.20f, 0.45f, 0.85f, 1.0f}, 0.6f);
        if (!render::upload_model(*r.device, character, r.fallbacks, r.models[kModelCharacter])) {
            TY_LOG_ERROR("gpu", "character upload failed: %s", platform::last_error());
        }
        const render::ModelData gate =
            make_plain_model(make_box_mesh(kGateHalf), Vec4{0.55f, 0.36f, 0.20f, 1.0f}, 0.8f);
        if (!render::upload_model(*r.device, gate, r.fallbacks, r.models[kModelGate])) {
            TY_LOG_ERROR("gpu", "gate upload failed: %s", platform::last_error());
        }
    }
    return r;
}

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
platform::InputFrame scripted_frame(long index, float dt) {
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

// Every entity with something to draw, its GPU model and its world matrix,
// with the mesh bound once per run of the same model.
template <typename PerEntity>
void each_drawable(scene::World& world, Renderer& renderer, rhi::RenderPass& pass, PerEntity&& per_entity) {
    std::uint32_t bound_model = 0xFFFFFFFFu;
    world.each<scene::LocalToWorld, scene::MeshRenderer>(
        [&](scene::Entity, scene::LocalToWorld& local_to_world, scene::MeshRenderer& mr) {
            if (!mr.visible || mr.model >= kModelCount) {
                return;
            }
            const render::Model& model = renderer.models[mr.model];
            if (model.mesh.index_count == 0) {
                return;
            }
            if (bound_model != mr.model) {
                render::bind_mesh(pass, model.mesh);
                bound_model = mr.model;
            }
            per_entity(model, local_to_world.matrix);
        });
}

// The frame as a graph: the sun's shadow maps, one depth-only pass per
// cascade; the scene into an HDR transient with a depth transient beside
// it, reading the shadow maps; then the tonemap pass reading that onto the
// swapchain. Declared, compiled and run every frame — the graph decides that
// the scene's depth is never stored and that the shadow maps and HDR are,
// and would drop any pass whose result nothing consumed.
void draw_frame(Renderer& renderer, rhi::Frame& frame, scene::World& world, const FlyCamera& fly,
                const Shading& shading) {
    render::FrameGraph& graph = *renderer.graph;
    const rhi::TextureFormat depth_format = renderer.device->preferred_depth_format();
    const render::TextureInfo screen{.format = kHdrFormat, .width = frame.width(), .height = frame.height()};
    graph.begin();
    render::GraphTexture swapchain = graph.import(
        "swapchain", frame.swapchain_texture(),
        {.format = renderer.device->swapchain_format(), .width = frame.width(), .height = frame.height()});
    render::GraphTexture hdr = graph.create("hdr", screen);
    render::GraphTexture depth =
        graph.create("depth", {.format = depth_format, .width = screen.width, .height = screen.height});

    const float aspect = static_cast<float>(frame.width()) / static_cast<float>(frame.height());
    const Mat4 view_projection = fly.camera.view_projection(aspect);
    const Vec3 light = shading.light_direction();
    const bool have_scene = renderer.models[kModelBottle].mesh.index_count > 0 && renderer.mesh_pipeline;
    const bool shadows = have_scene && renderer.shadows && shading.shadows;

    // The cascades: fitted to this frame's view, each drawn from the light.
    render::CascadeSet cascades;
    render::GraphTexture shadow_maps[render::kMaxCascades];
    if (shadows) {
        cascades = render::fit_cascades(fly.camera, aspect, light, kCascades);
        for (std::uint32_t i = 0; i < cascades.count; ++i) {
            shadow_maps[i] = graph.create(kShadowPassNames[i], {.format = depth_format,
                                                                .width = kCascades.map_size,
                                                                .height = kCascades.map_size});
            graph.add_pass(
                kShadowPassNames[i],
                [&, i](render::PassBuilder& b) { shadow_maps[i] = b.write_depth(shadow_maps[i]); },
                [&, i](rhi::RenderPass& pass, const render::PassResources&) {
                    pass.bind_pipeline(renderer.shadow_pipeline);
                    const Mat4& light_view_projection = cascades.cascades[i].view_projection;
                    each_drawable(world, renderer, pass, [&](const render::Model& model, const Mat4& world) {
                        const MeshUniforms uniforms{light_view_projection * world, world};
                        pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
                        pass.draw_indexed(model.mesh.index_count); // every submesh at once: no materials here
                    });
                });
        }
    }
    ShadowUniforms shadow_uniforms{};
    for (std::uint32_t i = 0; i < cascades.count; ++i) {
        const render::Cascade& cascade = cascades.cascades[i];
        shadow_uniforms.cascade_matrix[i] = cascade.view_projection;
        shadow_uniforms.cascade_param[i] = {cascade.texel_size, 1.0f / cascade.depth_range,
                                            1.0f / static_cast<float>(kCascades.map_size), 0.0f};
    }
    shadow_uniforms.settings = {static_cast<float>(cascades.count), shadows ? 1.0f : 0.0f,
                                kShadowNormalOffsetTexels, kShadowBiasTexels};

    graph.add_pass(
        "scene",
        [&](render::PassBuilder& b) {
            for (std::uint32_t i = 0; i < cascades.count; ++i) {
                b.read(shadow_maps[i]);
            }
            hdr = b.write_color(hdr, rhi::LoadOp::Clear, kSkyColor);
            depth = b.write_depth(depth, rhi::LoadOp::Clear, 0.0f);
        },
        [&](rhi::RenderPass& pass, const render::PassResources& resources) {
            if (!have_scene) {
                if (renderer.triangle_pipeline) {
                    pass.bind_pipeline(renderer.triangle_pipeline);
                    pass.draw(3);
                }
                return;
            }
            pass.bind_pipeline(renderer.mesh_pipeline);
            const FrameUniforms frame_uniforms{{fly.camera.position, 1.0f},
                                               {light, shading.light_intensity},
                                               {1.0f, 0.97f, 0.92f, shading.ambient},
                                               {static_cast<float>(shading.model),
                                                static_cast<float>(shading.debug_view), 0.0f, 0.0f}};
            pass.push_fragment_uniforms(0, &frame_uniforms, sizeof(frame_uniforms));
            pass.push_fragment_uniforms(2, &shadow_uniforms, sizeof(shadow_uniforms));
            // Every shadow slot gets a depth texture, since the shader declares
            // four: the cascades, then the last one again, or the 1x1 stand-in
            // when shadows are off (the shader never reads it then).
            if (renderer.shadow_sampler && renderer.shadow_fallback) {
                for (std::uint32_t i = 0; i < render::kMaxCascades; ++i) {
                    rhi::TextureHandle map = renderer.shadow_fallback;
                    if (cascades.count > 0) {
                        map = resources.texture(shadow_maps[std::min(i, cascades.count - 1)]);
                    }
                    pass.bind_fragment_texture(kShadowTextureSlot + i, map, renderer.shadow_sampler);
                }
            }
            each_drawable(world, renderer, pass, [&](const render::Model& model, const Mat4& world_matrix) {
                const MeshUniforms uniforms{view_projection * world_matrix, world_matrix};
                pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
                for (const render::Submesh& sub : model.mesh.submeshes) {
                    const render::Material& material = model.materials[sub.material];
                    const MaterialUniforms material_uniforms{
                        material.base_color_factor,
                        {material.metallic_factor, material.roughness_factor, material.occlusion_strength,
                         material.normal_scale},
                        {material.emissive_factor, 0.0f}};
                    pass.bind_fragment_texture(0, material.base_color, renderer.sampler);
                    pass.bind_fragment_texture(1, material.metallic_roughness, renderer.sampler);
                    pass.bind_fragment_texture(2, material.occlusion, renderer.sampler);
                    pass.bind_fragment_texture(3, material.emissive, renderer.sampler);
                    pass.bind_fragment_texture(4, material.normal, renderer.sampler);
                    pass.push_fragment_uniforms(1, &material_uniforms, sizeof(material_uniforms));
                    pass.draw_indexed(sub.index_count, sub.first_index);
                }
            });
        });
    // Debug views are data, not light: they go to the screen as they are.
    const PostUniforms post{{shading.tonemap && shading.view_is_lit() ? 1.0f : 0.0f,
                             renderer.device->swapchain_is_linear() ? 0.0f : 1.0f, 0.0f, 0.0f}};
    graph.add_pass(
        "tonemap",
        [&](render::PassBuilder& b) {
            b.read(hdr);
            // The triangle covers every pixel, so nothing needs loading — unless
            // there is no pipeline to draw it (no shaders for this backend yet),
            // when a clear is all the frame has.
            if (renderer.tonemap_pipeline) {
                swapchain = b.write_color(swapchain, rhi::LoadOp::DontCare);
            } else {
                swapchain = b.write_color(swapchain, rhi::LoadOp::Clear, kSkyColor);
            }
        },
        [&](rhi::RenderPass& pass, const render::PassResources& resources) {
            if (!renderer.tonemap_pipeline) {
                return;
            }
            pass.bind_pipeline(renderer.tonemap_pipeline);
            pass.bind_fragment_texture(0, resources.texture(hdr), renderer.post_sampler);
            pass.push_fragment_uniforms(0, &post, sizeof post);
            pass.draw(3);
        });
    if (graph.compile()) {
        graph.execute(frame);
    } else {
        TY_LOG_ERROR("graph", "%s", graph.error());
    }
}

// What the graph made of the frame, once it has settled.
void report_graph(const render::FrameGraph& graph) {
    const render::FrameGraph::Stats& stats = graph.stats();
    TY_LOG_INFO("graph", "%u passes (%u culled); %u transients in %u textures, %.1f MB of %.1f MB asked; "
                         "%u attachments stored, %u discarded; %u could live in tile memory",
                stats.passes, stats.culled, stats.transients_used, stats.physical_textures,
                static_cast<double>(stats.bytes_allocated) / 1048576.0,
                static_cast<double>(stats.bytes_requested) / 1048576.0, stats.attachments_stored,
                stats.attachments_discarded, stats.memoryless);
    char text[2048];
    graph.describe(text, sizeof text);
    for (char* line = text; *line != '\0';) {
        char* end = std::strchr(line, '\n');
        if (end == nullptr) {
            break;
        }
        *end = '\0';
        TY_LOG_INFO("graph", "  %s", line);
        line = end + 1;
    }
}

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
        options.jolt = log.backend.rfind("Jolt", 0) == 0;
        options.max_frames = static_cast<long>(log.frames.size());
    } else if (!options.record.empty()) {
        // Room for the whole recording up front, so that taking a frame down
        // is not a heap allocation in that frame: --frames says how many, or
        // ten minutes at 60 Hz (a longer recording grows past it and trips
        // the per-frame check).
        constexpr long kTenMinutesOfFrames = 10 * 60 * 60;
        log.frames.reserve(static_cast<std::size_t>(options.max_frames >= 0 ? options.max_frames
                                                                               : kTenMinutesOfFrames));
    }

    TY_LOG_INFO("sandbox", "engine %s, log level %s", tynima::core::version_string(),
                tynima::core::log_level_name(tynima::core::log_level()));
    if (!platform::init({.headless = options.headless})) {
        TY_LOG_ERROR("platform", "init failed: %s", platform::last_error());
        return 1;
    }

    auto window = platform::Window::create({.title = "tynima sandbox", .width = 1280, .height = 720});
    if (window == nullptr) {
        TY_LOG_ERROR("window", "creation failed: %s", platform::last_error());
        platform::shutdown();
        return 1;
    }

    TY_LOG_INFO("window", "%dx%d points, %dx%d pixels, density %.2f%s", window->width(), window->height(),
                window->pixel_width(), window->pixel_height(), static_cast<double>(window->pixel_density()),
                options.headless ? " (headless)" : "");
    TY_LOG_INFO("profile", "%s", tynima::core::profiling_compiled()
                                     ? "tracy instrumentation compiled in; connect the Tracy GUI to 127.0.0.1"
                                     : "off (TYNIMA_PROFILE=OFF)");

    tynima::core::JobSystem jobs;
    TY_LOG_INFO("jobs", "%u worker thread(s) + main, for %u performance cores", jobs.worker_count(),
                tynima::core::JobSystem::performance_core_count());

    // Physics steps on the same job system; bodies drive entity Transforms.
    const physics::WorldDesc physics_desc{.max_bodies = 1024, .jobs = &jobs};
    auto physics = options.jolt ? physics::create_jolt_world(physics_desc)
                                : physics::create_tynima_world(physics_desc);
    TY_LOG_INFO("physics", "%s%s", physics->backend_name(),
                options.jolt ? " (the reference; the default is the engine's own)"
                             : " (--physics jolt for the reference)");
    {
        physics::BodyDesc floor;
        floor.shape = physics::Shape::box(Vec3{kFloorHalfWidth, kFloorHalfThickness, kFloorHalfWidth});
        floor.position = Vec3{0.0f, -kFloorHalfThickness, 0.0f}; // top face at y = 0
        floor.motion = physics::MotionType::Static;
        floor.friction = 0.6f;
        (void)physics->create_body(floor);
    }

    // The model loads on the CPU in every mode, so the headless run covers the importer too.
    render::ModelData model_data;
    std::string import_error;
    const double import_start = platform::now_seconds();
    const bool has_model =
        assets::import_gltf_file(options.model.c_str(), model_data, import_error, {.jobs = &jobs});
    const render::MeshData& mesh_data = model_data.mesh;
    if (has_model) {
        const Vec3 size = mesh_data.bounds_max - mesh_data.bounds_min;
        TY_LOG_INFO("model", "%s: %zu vertices, %zu triangles, %zu submeshes, %.3f x %.3f x %.3f m",
                    options.model.c_str(), mesh_data.vertices.size(), mesh_data.indices.size() / 3,
                    mesh_data.submeshes.size(), static_cast<double>(size.x), static_cast<double>(size.y),
                    static_cast<double>(size.z));
        TY_LOG_INFO("model", "%zu materials, %zu images decoded in %.2f s", model_data.materials.size(),
                    model_data.images.size(), platform::now_seconds() - import_start);
        for (const render::ImageData& image : model_data.images) {
            TY_LOG_DEBUG("model", "image %ux%u%s", image.width, image.height, image.srgb ? " sRGB" : "");
        }
    } else {
        TY_LOG_WARN("model", "%s - showing the triangle instead", import_error.c_str());
    }
    TY_LOG_INFO("controls", "right-drag looks, WASD/QE fly, Shift runs, R re-drops the pile, "
                            "L launches it, Escape quits");
    TY_LOG_INFO("controls", "F follows the character: then WASD walk it, Space jumps, Shift runs");
    TY_LOG_INFO("controls", "1/2/3 unlit / Blinn-Phong / Cook-Torrance, N/M/O/V/B debug views, C shows the "
                            "shadow cascades, X toggles shadows, T tonemap, arrows move the light");

    Renderer renderer = options.headless ? Renderer{} : create_renderer(*window, has_model ? &model_data : nullptr);
    bool reported_swapchain = false;

    // The scene: the floor and a pile of the model, every one a rigid body.
    // Systems run over the World each frame; the renderer draws what the
    // World says is there.
    scene::World world(4096);
    Pile pile;
    // The character stands at the edge of the pile; the gate and the chain
    // hang either side of it.
    physics::CharacterController character(*physics, {.radius = kCharacterRadius,
                                                      .half_height = kCharacterHalfHeight,
                                                      .position = kCharacterStart});
    Rest rest;
    if (has_model) {
        (void)world.create(scene::Transform{.position = Vec3{0.0f, -kFloorHalfThickness, 0.0f}},
                           scene::LocalToWorld{}, scene::MeshRenderer{.model = kModelFloor});
        pile = populate_pile(world, *physics, mesh_data);
        rest = populate_rest(world, *physics, character.body(), mesh_data);
        TY_LOG_INFO("scene", "%u entities in %u archetype(s), %u chunk(s); %u bodies, %u joints",
                    world.entity_count(), world.archetype_count(), world.chunk_count(), physics->body_count(),
                    physics->joint_count());
    }

    Shading shading;
    FlyCamera fly;
    if (has_model) {
        // Frame where the pile lands, with the falling column in view above it.
        fly.frame(Vec3{0.0f, pile.drop_height * 0.3f, 0.0f},
                  std::max(pile.footprint * 2.2f, pile.drop_height * 0.6f));
    }

    // Per-frame scratch memory: reset at the top of every frame, never freed
    // piecemeal. Nothing uses it yet; the ECS and render packets will.
    tynima::core::Arena frame_arena(4 * 1024 * 1024);

    platform::Input input;
    std::vector<platform::Event> events;
    events.reserve(64); // a frame's worth; growing later would count as a frame allocation

    // The game module owns the systems that make the scene move. It is a
    // shared library, and it is swapped for a new build whenever one appears.
    tynima_engine engine_context;
    engine_context.world = &world;
    engine_context.input = &input;
    engine_context.physics = physics.get();
    tynima::sdk::GameModule game(TYNIMA_SANDBOX_GAME_MODULE);
    TY_LOG_INFO("game", "loading %s", game.path().c_str());
    if (game.load(engine_context)) {
        TY_LOG_INFO("game", "edit apps/sandbox/game/src/game.cpp, then: cmake --build --preset macos-debug "
                            "--target tynima_sandbox_game");
    } else {
        TY_LOG_ERROR("game", "%s - the scene will not move", game.last_error());
    }
    // Physics runs at a fixed 60 Hz whatever the frame rate: the frame's
    // time accumulates into whole steps, and what is drawn is blended
    // between the last two states. Input edges that must reach a step
    // (a jump) are latched until one runs.
    physics::FixedStepper stepper;
    bool jump_latched = false;
    long frame_count = 0;
    double game_time = 0.0; // the game module's clock: frame times summed, so a replay reads the same
    double last_time = platform::now_seconds();
    double last_report = last_time;
    long frames_since_report = 0;
    bool running = true;
    std::uint32_t last_culled = 0;

    while (running) {
        frame_arena.reset();
        const tynima::core::HeapAllocationScope heap_scope;
        {
            TY_PROFILE_SCOPE_NAMED("events");
            platform::pump_events(input, events);
        }
        // Time and input: the clock and the keyboard, or the log's — and
        // headless, a fixed frame of a sixtieth and the script above, so the
        // run is the same twice. Settled here, before anything reads either.
        const double now = platform::now_seconds();
        float dt = static_cast<float>(std::min(now - last_time, 0.1)); // clamp hitches
        last_time = now;
        if (!options.replay.empty()) {
            const platform::InputFrame& frame = log.frames[static_cast<std::size_t>(frame_count)];
            platform::apply_frame(frame, input);
            dt = frame.dt;
        } else if (options.headless) {
            dt = 1.0f / 60.0f;
            platform::apply_frame(scripted_frame(frame_count, dt), input);
        }
        if (!options.record.empty()) {
            log.frames.push_back(platform::capture_frame(input, dt));
        }
        game_time += static_cast<double>(dt);
        report_edges(input);
        for (const auto& event : events) {
            switch (event.type) {
            case platform::EventType::Quit:
            case platform::EventType::WindowClose:
                running = false;
                break;
            case platform::EventType::WindowResized:
                TY_LOG_INFO("window", "resized to %dx%d points, %dx%d pixels", event.width, event.height,
                            event.pixel_width, event.pixel_height);
                break;
            case platform::EventType::WindowFocusGained:
            case platform::EventType::WindowFocusLost:
                break;
            }
        }
        if (input.key_pressed(platform::Key::Escape)) {
            running = false;
        }
        if (input.key_pressed(platform::Key::R)) {
            reset_pile(world, *physics, pile);
        }
        if (input.key_pressed(platform::Key::F)) {
            fly.follow = !fly.follow;
            TY_LOG_INFO("camera", "%s", fly.follow ? "following the character" : "flying free");
        }
        const Vec3 character_position = character.position();
        fly.update(input, *window, dt, fly.follow ? &character_position : nullptr);
        shading.update(input, dt);

        // The character walks where the camera's WASD point, when the camera
        // is following it.
        Vec3 walk = Vec3::zero();
        if (fly.follow) {
            const bool run =
                input.key_down(platform::Key::LeftShift) || input.key_down(platform::Key::RightShift);
            walk = fly.walk_direction(input) * (kWalkSpeed * (run ? 2.0f : 1.0f));
            jump_latched = jump_latched || input.key_pressed(platform::Key::Space);
        }

        engine_context.time_seconds = game_time;
        const bool reloaded = game.poll(engine_context); // a reload allocates; that frame is exempt below
        {
            TY_PROFILE_SCOPE_NAMED("systems");
            game.update(engine_context, dt);
            stepper.advance(*physics, dt, [&](float step) {
                scene::record_previous_poses(world, *physics);
                character.move(walk, jump_latched, step);
                jump_latched = false;
            });
            scene::update_bodies(world, *physics, stepper.alpha());
            scene::update_transforms(world);
        }

        if (renderer.device != nullptr) {
            TY_PROFILE_SCOPE_NAMED("render");
            // begin_frame() blocks for vsync, which is what paces the loop.
            if (auto frame = renderer.device->begin_frame()) {
                if (frame->has_swapchain_image()) {
                    if (!reported_swapchain) {
                        TY_LOG_INFO("swap", "%ux%u pixels", frame->width(), frame->height());
                        reported_swapchain = true;
                    }
                    draw_frame(renderer, *frame, world, fly, shading);
                    const std::uint32_t culled = renderer.graph->stats().culled;
                    if (frame_count == 60 || (frame_count > 60 && culled != last_culled)) {
                        report_graph(*renderer.graph);
                        last_culled = culled;
                    }
                }
                frame->submit();
            } else {
                TY_LOG_ERROR("gpu", "frame failed: %s", platform::last_error());
                running = false;
            }
        } else {
            // Nothing to draw and nothing to wait on: pace the loop by hand.
            platform::sleep_ns(4'000'000);
        }

        // The rule from the roadmap, enforced: after warm-up, engine code
        // allocates nothing on the heap during a frame. The GPU driver and the
        // OS allocate plenty inside the calls we make; that is reported, not judged.
        const std::uint64_t engine_allocations = heap_scope.allocations();
        const std::uint64_t external_allocations = heap_scope.total() - engine_allocations;
        TY_PROFILE_PLOT("engine heap allocations / frame", static_cast<std::int64_t>(engine_allocations));
        TY_PROFILE_PLOT("external heap allocations / frame", static_cast<std::int64_t>(external_allocations));
        TY_PROFILE_PLOT("frame arena bytes", static_cast<std::int64_t>(frame_arena.used()));
        if (frame_count == 60) {
            TY_LOG_INFO("heap", "per frame: engine %llu, external (driver, OS, Jolt) %llu",
                        static_cast<unsigned long long>(engine_allocations),
                        static_cast<unsigned long long>(external_allocations));
        }
        if (frame_count >= 10 && engine_allocations > 0 && !reloaded) {
            TY_LOG_ERROR("heap", "frame %ld: engine code made %llu heap allocation(s)", frame_count,
                         static_cast<unsigned long long>(engine_allocations));
            TY_ASSERT(engine_allocations == 0, "a frame allocated on the heap from engine code");
        }

        TY_PROFILE_FRAME();
        ++frame_count;
        ++frames_since_report;
        if (now - last_report >= 5.0) {
            TY_LOG_INFO("sandbox", "%.0f frames/s",
                        static_cast<double>(frames_since_report) / (now - last_report));
            last_report = now;
            frames_since_report = 0;
        }
        if (options.max_frames >= 0 && frame_count >= options.max_frames) {
            running = false;
        }
    }

    const auto state_hash = static_cast<unsigned long long>(physics->state_hash());
    const auto steps_taken = static_cast<unsigned long long>(stepper.total_steps);
    TY_LOG_INFO("sandbox", "ran %ld frames, %llu physics steps, %u bodies awake, state hash %016llx",
                frame_count, steps_taken, physics->active_body_count(), state_hash);
    if (has_model) {
        report_pile(world, pile);
        report_rest(world, *physics, character, rest);
    }
    int exit_code = 0;
    if (!options.record.empty()) {
        log.backend = physics->backend_name();
        log.steps = stepper.total_steps;
        log.state_hash = state_hash;
        log.has_end = true;
        std::string error;
        if (log.save(options.record.c_str(), error)) {
            TY_LOG_INFO("replay", "recorded %zu frames to %s", log.frames.size(), options.record.c_str());
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
        } else if (log.state_hash == state_hash && log.steps == stepper.total_steps) {
            TY_LOG_INFO("replay", "%zu frames replayed: %llu steps and state hash %016llx, as recorded",
                        log.frames.size(), steps_taken, state_hash);
        } else {
            TY_LOG_ERROR("replay", "the replay diverged: %llu steps and state hash %016llx; recorded %llu "
                                   "steps and %016llx (on %s)",
                         steps_taken, state_hash, static_cast<unsigned long long>(log.steps),
                         static_cast<unsigned long long>(log.state_hash), log.backend.c_str());
            exit_code = kExitReplayMismatch;
        }
    }
    game.unload(engine_context);
    renderer.destroy(); // GPU objects go before the window they present to
    window.reset();
    platform::shutdown();
    return exit_code;
}
