#include <tynima.h>

#include <tynima/assets/model_blob.h>
#include <tynima/platform/file.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/render/shapes.h>
#include <tynima/scene/components.h>
#include <tynima/sdk/runtime.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace scene = tynima::scene;
namespace sdk = tynima::sdk;
using namespace tynima::math;

namespace {

// The whole engine, headless, through the public header: what the editor
// does, minus the panels.
struct HostedEngine {
    tynima_engine* engine = nullptr;
    const tynima_api* api = nullptr;

    explicit HostedEngine(uint32_t max_entities = 256) {
        tynima_engine_desc desc{};
        desc.title = "sdk test";
        desc.headless = true;
        desc.max_entities = max_entities;
        engine = tynima_engine_create(&desc);
        if (engine != nullptr) {
            api = tynima_engine_api(engine);
        }
    }
    ~HostedEngine() { tynima_engine_destroy(engine); }
};

} // namespace

TEST_CASE("a hosted engine runs frames headless, keeps its clock, and stops when told") {
    HostedEngine hosted;
    REQUIRE_MESSAGE(hosted.engine != nullptr, tynima_last_error());
    REQUIRE(hosted.api != nullptr);
    CHECK(hosted.api->version == TYNIMA_API_VERSION);
    CHECK(tynima_engine_frame_index(hosted.engine) == 0);

    // An entity with a name, a transform and a world matrix, seen through the table.
    const tynima_api& api = *hosted.api;
    const tynima_component_id name = api.find_component(hosted.engine, TYNIMA_COMPONENT_NAME);
    const tynima_component_id transform = api.find_component(hosted.engine, TYNIMA_COMPONENT_TRANSFORM);
    const tynima_component_id local_to_world =
        api.find_component(hosted.engine, TYNIMA_COMPONENT_LOCAL_TO_WORLD);
    REQUIRE(name != TYNIMA_NO_COMPONENT);
    REQUIRE(transform != TYNIMA_NO_COMPONENT);
    REQUIRE(local_to_world != TYNIMA_NO_COMPONENT);
    CHECK(api.find_component(hosted.engine, "NoSuchComponent") == TYNIMA_NO_COMPONENT);
    tynima_component_info info{};
    REQUIRE(api.component_info(hosted.engine, transform, &info));
    CHECK(std::string(info.name) == "Transform");
    CHECK(info.size == sizeof(tynima_transform));
    CHECK_FALSE(api.component_info(hosted.engine, 999, &info));
    CHECK(api.component_count(hosted.engine) >= 6);

    tynima_name n{};
    std::strcpy(n.text, "probe");
    tynima_transform t{};
    t.position = tynima_vec3_make(1.0f, 2.0f, 3.0f);
    t.rotation = tynima_quat_from_axis_angle(tynima_vec3_make(0.0f, 1.0f, 0.0f), 1.0f);
    t.scale = tynima_vec3_make(1.0f, 1.0f, 1.0f);
    tynima_local_to_world l{};
    const tynima_component_id ids[3] = {name, transform, local_to_world};
    const void* values[3] = {&n, &t, &l};
    const tynima_entity e = api.create_entity(hosted.engine, ids, values, 3);
    REQUIRE(e.generation != 0);
    tynima_component_id have[TYNIMA_MAX_COMPONENT_TYPES];
    CHECK(api.entity_components(hosted.engine, e, have, TYNIMA_MAX_COMPONENT_TYPES) == 3);

    for (int i = 0; i < 30; ++i) {
        REQUIRE(tynima_engine_begin_frame(hosted.engine));
        CHECK(tynima_engine_dt(hosted.engine) == doctest::Approx(1.0f / 60.0f)); // headless: a fixed step
        CHECK(tynima_ui_context(hosted.engine) != nullptr);
        CHECK(tynima_ui_scene_texture(hosted.engine, 64, 64) == 0); // no GPU headless
        tynima_engine_end_frame(hosted.engine);
    }
    CHECK(tynima_engine_frame_index(hosted.engine) == 30);
    CHECK(tynima_engine_time(hosted.engine) == doctest::Approx(0.5).epsilon(1e-3));

    // The world matrix followed the transform: the engine's systems ran.
    const auto* world_matrix =
        static_cast<const tynima_local_to_world*>(api.get_component(hosted.engine, e, local_to_world));
    REQUIRE(world_matrix != nullptr);
    CHECK(world_matrix->matrix.m[12] == doctest::Approx(1.0f)); // column 3: the translation
    CHECK(world_matrix->matrix.m[13] == doctest::Approx(2.0f));
    CHECK(world_matrix->matrix.m[14] == doctest::Approx(3.0f));

    // The log ring holds the engine's startup lines, oldest still readable.
    const uint64_t count = tynima_log_count(hosted.engine);
    CHECK(count > 0);
    tynima_log_entry entry{};
    REQUIRE(tynima_log_get(hosted.engine, 0, &entry));
    CHECK(entry.index == 0);
    CHECK(std::string(entry.category) == "runtime");
    CHECK(std::string(entry.message).find("engine ") == 0);
    CHECK_FALSE(tynima_log_get(hosted.engine, count, &entry));
    api.log(hosted.engine, TYNIMA_LOG_WARN, "from the host");
    REQUIRE(tynima_log_get(hosted.engine, tynima_log_count(hosted.engine) - 1, &entry));
    CHECK(entry.level == TYNIMA_LOG_WARN);
    CHECK(std::string(entry.category) == "game");
    CHECK(std::string(entry.message) == "from the host");

    tynima_stats stats{};
    tynima_get_stats(hosted.engine, &stats);
    CHECK(stats.entities == 1);
    CHECK(stats.heap_allocations == 0); // the frame rule, through the public API too
    CHECK(stats.gpu_ms == 0.0f);

    // The camera and the settings round-trip.
    tynima_camera camera{};
    tynima_get_camera(hosted.engine, &camera);
    CHECK(camera.fov_y > 0.0f);
    camera.position = tynima_vec3_make(0.0f, 5.0f, 0.0f);
    tynima_set_camera(hosted.engine, &camera);
    tynima_get_camera(hosted.engine, &camera);
    CHECK(camera.position.y == 5.0f);
    tynima_render_settings settings{};
    tynima_get_render_settings(hosted.engine, &settings);
    CHECK(settings.path == TYNIMA_SHADING_FORWARD);
    settings.path = TYNIMA_SHADING_FUSED;
    settings.debug_view = 7;
    tynima_set_render_settings(hosted.engine, &settings);
    tynima_get_render_settings(hosted.engine, &settings);
    CHECK(settings.path == TYNIMA_SHADING_FUSED);
    CHECK(std::string(tynima_debug_view_name(settings.debug_view)) == "lights per cluster");
    CHECK(std::string(tynima_debug_view_name(99)) == "?");
}

TEST_CASE("a bad description is refused with a reason") {
    tynima_engine_desc desc{};
    desc.headless = true;
    desc.physics = "havok";
    CHECK(tynima_engine_create(&desc) == nullptr);
    CHECK(std::string(tynima_last_error()).find("havok") != std::string::npos);
    desc.physics = nullptr;
    desc.rhi = "vulkan";
    CHECK(tynima_engine_create(&desc) == nullptr);
    CHECK(std::string(tynima_last_error()).find("vulkan") != std::string::npos);
}

TEST_CASE("the runtime, linked directly: frames stop at the budget, models take slots, replays record") {
    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_frames = 5;
    desc.record = true;
    desc.max_entities = 128;
    REQUIRE(runtime.create(desc));
    // A model without a device: an empty slot at the same index.
    tynima::render::ModelData data;
    CHECK(runtime.add_model(data) == 0);
    CHECK(runtime.add_model(data) == 1);
    CHECK(runtime.model_count() == 2);
    CHECK(runtime.model(1) != nullptr);
    CHECK(runtime.model(2) == nullptr);
    CHECK(runtime.load_model("/no/such/model.glb") == sdk::Runtime::kNoModel);

    int frames = 0;
    int steps_before = 0;
    runtime.set_before_step([](float, void* user) { ++*static_cast<int*>(user); }, &steps_before);
    while (runtime.begin_frame()) {
        ++frames;
        runtime.end_frame();
    }
    CHECK(frames == 5);
    CHECK(steps_before == static_cast<int>(runtime.stepper().total_steps));
    CHECK(steps_before >= 4); // five sixtieths at 60 Hz
    const tynima::platform::InputLog log = runtime.recording();
    CHECK(log.frames.size() == 5);
    CHECK(log.has_end);
    CHECK(log.steps == runtime.stepper().total_steps);
    CHECK(log.backend == runtime.physics().backend_name());
    runtime.destroy();
    CHECK_FALSE(runtime.created());
}

TEST_CASE("a ray picks the nearest drawable by its model's bounds, headless too") {
    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_entities = 64;
    REQUIRE(runtime.create(desc));
    // A unit cube's worth of bounds; the mesh itself never reaches a GPU here.
    tynima::render::ModelData cube;
    cube.mesh.vertices.resize(8);
    cube.mesh.indices.resize(36);
    cube.mesh.bounds_min = Vec3{-0.5f, -0.5f, -0.5f};
    cube.mesh.bounds_max = Vec3{0.5f, 0.5f, 0.5f};
    const std::uint32_t model = runtime.add_model(cube);
    tynima_vec3 lo{}, hi{};
    REQUIRE(tynima_model_bounds(&runtime.context(), model, &lo, &hi));
    CHECK(lo.x == -0.5f);
    CHECK(hi.z == 0.5f);
    CHECK_FALSE(tynima_model_bounds(&runtime.context(), 7, &lo, &hi));

    scene::World& world = runtime.world();
    const scene::Entity near =
        world.create(scene::Name("near"), scene::Transform{.position = {0.0f, 0.0f, 0.0f}},
                     scene::LocalToWorld{}, scene::MeshRenderer{.model = model});
    const scene::Entity far =
        world.create(scene::Name("far"), scene::Transform{.position = {0.0f, 0.0f, -4.0f}},
                     scene::LocalToWorld{}, scene::MeshRenderer{.model = model});
    const scene::Entity aside = world.create(
        scene::Name("aside"), scene::Transform{.position = {3.0f, 0.0f, 0.0f}, .scale = {2.0f, 2.0f, 2.0f}},
        scene::LocalToWorld{}, scene::MeshRenderer{.model = model});
    (void)far;
    REQUIRE(runtime.begin_frame()); // a frame computes the world matrices
    runtime.end_frame();

    float distance = 0.0f;
    tynima_entity hit{};
    const tynima_vec3 origin = tynima_vec3_make(0.0f, 0.0f, 5.0f);
    REQUIRE(tynima_pick(&runtime.context(), origin, tynima_vec3_make(0.0f, 0.0f, -1.0f), &hit, &distance));
    CHECK(hit.index == near.index); // the first box along the ray, not the one behind it
    CHECK(distance == doctest::Approx(4.5f));
    // A scaled entity: the bounds scale with it, so a ray past the unit box still hits.
    REQUIRE(tynima_pick(&runtime.context(), tynima_vec3_make(3.8f, 0.0f, 5.0f),
                        tynima_vec3_make(0.0f, 0.0f, -1.0f), &hit, &distance));
    CHECK(hit.index == aside.index);
    CHECK(distance == doctest::Approx(4.0f));
    CHECK_FALSE(tynima_pick(&runtime.context(), tynima_vec3_make(0.0f, 10.0f, 5.0f),
                            tynima_vec3_make(0.0f, 0.0f, -1.0f), &hit, &distance));
    // Invisible entities are not picked.
    world.get<scene::MeshRenderer>(near)->visible = false;
    REQUIRE(tynima_pick(&runtime.context(), origin, tynima_vec3_make(0.0f, 0.0f, -1.0f), &hit, &distance));
    CHECK(hit.index == far.index);
    CHECK(distance == doctest::Approx(8.5f));

    // The camera helpers: a ray through the middle of the view looks down
    // the camera's -z, and a point projects back to where it came from.
    tynima_camera camera{};
    tynima_get_camera(&runtime.context(), &camera);
    camera.position = origin;
    camera.rotation = tynima_quat_identity();
    tynima_set_camera(&runtime.context(), &camera);
    const tynima_vec3 middle = tynima_camera_ray(&camera, 16.0f / 9.0f, 0.0f, 0.0f);
    CHECK(middle.z == doctest::Approx(-1.0f));
    const tynima_vec3 corner = tynima_camera_ray(&camera, 16.0f / 9.0f, 1.0f, 1.0f);
    float ndc_x = 0.0f, ndc_y = 0.0f, depth = 0.0f;
    const tynima_vec3 ahead = tynima_vec3_add(origin, tynima_vec3_scale(corner, 3.0f));
    REQUIRE(tynima_camera_project(&camera, 16.0f / 9.0f, ahead, &ndc_x, &ndc_y, &depth));
    CHECK(ndc_x == doctest::Approx(1.0f));
    CHECK(ndc_y == doctest::Approx(1.0f));
    CHECK(depth == doctest::Approx(-corner.z * 3.0f));
    CHECK_FALSE(
        tynima_camera_project(&camera, 1.0f, tynima_vec3_make(0.0f, 0.0f, 9.0f), &ndc_x, &ndc_y, &depth));
}

TEST_CASE("the inline C math agrees with the engine's") {
    const tynima_quat yaw = tynima_quat_from_axis_angle(tynima_vec3_make(0.0f, 1.0f, 0.0f), 0.7f);
    const tynima_quat pitch = tynima_quat_from_axis_angle(tynima_vec3_make(1.0f, 0.0f, 0.0f), -0.3f);
    const tynima_quat q = tynima_quat_mul(yaw, pitch);
    const Quat expected =
        Quat::from_axis_angle(Vec3::unit_y(), 0.7f) * Quat::from_axis_angle(Vec3::unit_x(), -0.3f);
    CHECK(q.x == doctest::Approx(expected.x));
    CHECK(q.y == doctest::Approx(expected.y));
    CHECK(q.z == doctest::Approx(expected.z));
    CHECK(q.w == doctest::Approx(expected.w));
    const tynima_vec3 f = tynima_quat_rotate(q, tynima_vec3_make(0.0f, 0.0f, -1.0f));
    const Vec3 expected_forward = expected.rotate(-Vec3::unit_z());
    CHECK(f.x == doctest::Approx(expected_forward.x));
    CHECK(f.y == doctest::Approx(expected_forward.y));
    CHECK(f.z == doctest::Approx(expected_forward.z));
    // Euler angles round-trip through a quaternion.
    const tynima_quat e = tynima_quat_from_euler(0.7f, -0.3f, 0.2f);
    float y = 0.0f, p = 0.0f, r = 0.0f;
    tynima_quat_to_euler(e, &y, &p, &r);
    CHECK(y == doctest::Approx(0.7f));
    CHECK(p == doctest::Approx(-0.3f));
    CHECK(r == doctest::Approx(0.2f));
    CHECK(tynima_vec3_length(tynima_vec3_normalize(tynima_vec3_make(3.0f, 4.0f, 0.0f))) ==
          doctest::Approx(1.0f));
}

TEST_CASE("fields cross the C API: the engine's components come described, a module's are described") {
    HostedEngine hosted;
    REQUIRE_MESSAGE(hosted.engine != nullptr, tynima_last_error());
    const tynima_api& api = *hosted.api;
    const tynima_component_id transform = api.find_component(hosted.engine, TYNIMA_COMPONENT_TRANSFORM);
    REQUIRE(api.component_field_count(hosted.engine, transform) == 3);
    tynima_field field{};
    REQUIRE(api.component_field(hosted.engine, transform, 1, &field));
    CHECK(std::string(field.name) == "rotation");
    CHECK(field.kind == TYNIMA_FIELD_QUAT);
    CHECK(field.offset == offsetof(tynima_transform, rotation));
    CHECK(field.size == sizeof(tynima_quat));
    CHECK_FALSE(api.component_field(hosted.engine, transform, 3, &field));
    const tynima_component_id name = api.find_component(hosted.engine, TYNIMA_COMPONENT_NAME);
    REQUIRE(api.component_field(hosted.engine, name, 0, &field));
    CHECK(field.kind == TYNIMA_FIELD_STRING);
    CHECK(field.count == TYNIMA_NAME_CAPACITY);
    const tynima_component_id body = api.find_component(hosted.engine, TYNIMA_COMPONENT_RIGID_BODY);
    REQUIRE(api.component_field(hosted.engine, body, 0, &field));
    CHECK(field.kind == TYNIMA_FIELD_HANDLE);
    CHECK(field.flags == TYNIMA_FIELD_READ_ONLY);
    REQUIRE(api.component_field(hosted.engine, body, 1, &field));
    CHECK(field.flags == TYNIMA_FIELD_HIDDEN);

    // What a game module does for its own component, in C terms.
    struct Launcher {
        float speed;
        float spread;
        bool armed;
    };
    const tynima_component_id launcher =
        api.register_component(hosted.engine, "Launcher", sizeof(Launcher), alignof(Launcher));
    CHECK(api.component_field_count(hosted.engine, launcher) == 0);
    const tynima_field fields[3] = {
        {"speed", TYNIMA_FIELD_FLOAT, offsetof(Launcher, speed), sizeof(float), 1, 0},
        {"spread", TYNIMA_FIELD_FLOAT, offsetof(Launcher, spread), sizeof(float), 1, 0},
        {"armed", TYNIMA_FIELD_BOOL, offsetof(Launcher, armed), sizeof(bool), 1, 0},
    };
    CHECK(api.describe_component(hosted.engine, launcher, fields, 3));
    REQUIRE(api.component_field_count(hosted.engine, launcher) == 3);
    REQUIRE(api.component_field(hosted.engine, launcher, 2, &field));
    CHECK(std::string(field.name) == "armed");
    CHECK(field.kind == TYNIMA_FIELD_BOOL);
    CHECK(field.offset == 8);
    const tynima_field outside = {"outside", TYNIMA_FIELD_MAT4, 0, 64, 1, 0};
    // The description stands, whatever comes later.
    CHECK_FALSE(api.describe_component(hosted.engine, launcher, &outside, 1));
    CHECK_FALSE(api.describe_component(hosted.engine, 99, fields, 3));
    CHECK_FALSE(api.describe_component(hosted.engine, launcher, fields, 0));
}

TEST_CASE("a scene goes to a file and comes back through the public header") {
    HostedEngine hosted;
    REQUIRE_MESSAGE(hosted.engine != nullptr, tynima_last_error());
    const tynima_api& api = *hosted.api;
    const tynima_component_id name = api.find_component(hosted.engine, TYNIMA_COMPONENT_NAME);
    const tynima_component_id transform = api.find_component(hosted.engine, TYNIMA_COMPONENT_TRANSFORM);
    const tynima_component_id parent = api.find_component(hosted.engine, TYNIMA_COMPONENT_PARENT);
    // The defaults an editor adds a component as.
    const auto* transform_defaults =
        static_cast<const tynima_transform*>(api.component_defaults(hosted.engine, transform));
    REQUIRE(transform_defaults != nullptr);
    CHECK(transform_defaults->scale.x == 1.0f);
    CHECK(transform_defaults->rotation.w == 1.0f);
    CHECK(api.component_defaults(hosted.engine, 99) == nullptr);

    tynima_name floor_name{};
    std::strcpy(floor_name.text, "floor");
    tynima_transform t = *transform_defaults;
    t.position = tynima_vec3_make(0.0f, -0.25f, 0.0f);
    const tynima_component_id floor_ids[2] = {name, transform};
    const void* floor_values[2] = {&floor_name, &t};
    const tynima_entity floor = api.create_entity(hosted.engine, floor_ids, floor_values, 2);
    tynima_name crate_name{};
    std::strcpy(crate_name.text, "crate");
    const tynima_parent on_floor{floor};
    const tynima_component_id crate_ids[3] = {name, transform, parent};
    const void* crate_values[3] = {&crate_name, transform_defaults, &on_floor};
    (void)api.create_entity(hosted.engine, crate_ids, crate_values, 3);

    const std::string path = std::string(std::getenv("TMPDIR") != nullptr ? std::getenv("TMPDIR") : "/tmp") +
                             "/tynima_sdk_scene.toml";
    REQUIRE_MESSAGE(tynima_save_scene(hosted.engine, path.c_str()), tynima_last_error());
    tynima_clear_scene(hosted.engine);
    CHECK(api.entity_count(hosted.engine) == 0);
    REQUIRE_MESSAGE(tynima_load_scene(hosted.engine, path.c_str(), true), tynima_last_error());
    CHECK(api.entity_count(hosted.engine) == 2);
    REQUIRE_MESSAGE(tynima_load_scene(hosted.engine, path.c_str(), false), tynima_last_error());
    CHECK(api.entity_count(hosted.engine) == 4);
    // The crate's parent is the loaded floor, not the old handle.
    struct Found {
        const tynima_api* api;
        tynima_engine* engine;
        tynima_component_id name;
        int crates_on_a_floor = 0;
    } found{&api, hosted.engine, name};
    const tynima_component_id query[2] = {parent, name};
    api.each_chunk(
        hosted.engine, query, 2,
        [](void* user, const tynima_entity*, uint32_t count, void* const* columns) {
            auto* f = static_cast<Found*>(user);
            const auto* parents = static_cast<const tynima_parent*>(columns[0]);
            for (uint32_t i = 0; i < count; ++i) {
                const auto* parent_name = static_cast<const tynima_name*>(
                    f->api->get_component(f->engine, parents[i].entity, f->name));
                if (parent_name != nullptr && std::string(parent_name->text) == "floor") {
                    ++f->crates_on_a_floor;
                }
            }
        },
        &found);
    CHECK(found.crates_on_a_floor == 2);
    CHECK_FALSE(tynima_load_scene(hosted.engine, "/no/such/scene.toml", true));
    CHECK(std::string(tynima_last_error()).find("cannot read") != std::string::npos);
    CHECK(api.entity_count(hosted.engine) == 4); // untouched
    std::remove(path.c_str());
}

namespace {

// A triangle from the origin out to `scale` along x and y, as glTF text
// with its buffer inline: the smallest source a cook needs.
std::string triangle_gltf(const char* scale) {
    const std::string s = scale;
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
           R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
           R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)" +
           (s == "1" ? std::string("AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA")
                     : std::string("AAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAA")) +
           R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
           R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
           R"("min":[0,0,0],"max":[)" +
           s + "," + s + R"(,0]}]})";
}

std::string temp_dir() {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir != nullptr ? dir : "/tmp");
}

// Writes until the file's recorded time moves past what it was.
void write_later(const std::string& path, const std::string& text) {
    const std::uint64_t before = tynima::platform::file_write_time(path.c_str());
    for (int attempt = 0; attempt < 1000; ++attempt) {
        REQUIRE(tynima::platform::write_file(path.c_str(), text.data(), text.size()));
        if (tynima::platform::file_write_time(path.c_str()) > before) {
            return;
        }
        tynima::platform::sleep_ns(1'000'000);
    }
    FAIL("the file's write time never moved");
}

} // namespace

TEST_CASE("a source model cooks on load, loads from its blob after, and reloads when either changes") {
    const std::string dir = temp_dir() + "/tynima_runtime_model_test";
    REQUIRE(tynima::platform::make_directories(dir.c_str()));
    const std::string source = dir + "/tri.gltf";
    const std::string cooked = dir + "/.cooked/tri.tymodel";
    (void)tynima::platform::remove_file(cooked.c_str());
    const std::string small = triangle_gltf("1");
    REQUIRE(tynima::platform::write_file(source.c_str(), small.data(), small.size()));

    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_entities = 64;
    desc.asset_poll_seconds = 0.0f; // every frame, and a change settles at once
    REQUIRE(runtime.create(desc));
    const std::uint32_t model = runtime.load_model(source.c_str());
    REQUIRE(model == 0);
    CHECK(tynima::assets::model_blob_version(cooked.c_str()) == tynima::assets::kModelBlobVersion);
    CHECK(runtime.model(model)->mesh.index_count == 3);
    CHECK(runtime.model(model)->mesh.bounds_max == Vec3{1.0f, 1.0f, 0.0f});
    // The blob directly, as a shipping build would: the same shape at a new index.
    CHECK(runtime.load_model(cooked.c_str()) == 1);
    CHECK(runtime.model(1)->mesh.bounds_max == Vec3{1.0f, 1.0f, 0.0f});
    // Neither a model nor a blob: refused.
    CHECK(runtime.load_model((dir + "/tri.txt").c_str()) == sdk::Runtime::kNoModel);
    CHECK(runtime.load_model("") == sdk::Runtime::kNoModel);
    CHECK(runtime.model_count() == 2);

    // Frames with nothing changed reload nothing.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
    CHECK(runtime.stats().model_reloads == 0);

    // The source grows: the next frames cook it again and load the new
    // shape into the same index — and into the blob's slot too, since the
    // blob changed underneath it.
    write_later(source, triangle_gltf("2"));
    for (int i = 0; i < 4 && runtime.stats().model_reloads < 2; ++i) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
    CHECK(runtime.stats().model_reloads == 2);
    CHECK(runtime.model(0)->mesh.bounds_max == Vec3{2.0f, 2.0f, 0.0f});
    CHECK(runtime.model(1)->mesh.bounds_max == Vec3{2.0f, 2.0f, 0.0f});
    CHECK(runtime.model_count() == 2);

    // A source that no longer cooks keeps the model it had.
    write_later(source, "not glTF");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
    CHECK(runtime.stats().model_reloads == 2);
    CHECK(runtime.model(0)->mesh.bounds_max == Vec3{2.0f, 2.0f, 0.0f});

    // Cooking through the header, somewhere else.
    REQUIRE(tynima::platform::write_file(source.c_str(), small.data(), small.size()));
    const std::string elsewhere = dir + "/out/tri.tymodel";
    REQUIRE_MESSAGE(tynima_cook_model(&runtime.context(), source.c_str(), elsewhere.c_str()),
                    tynima_last_error());
    CHECK(tynima::assets::model_blob_version(elsewhere.c_str()) == tynima::assets::kModelBlobVersion);
    CHECK_FALSE(tynima_cook_model(&runtime.context(), (dir + "/none.glb").c_str(), elsewhere.c_str()));
    CHECK_FALSE(tynima_cook_model(nullptr, source.c_str(), elsewhere.c_str()));

    (void)tynima::platform::remove_file(elsewhere.c_str());
    (void)tynima::platform::remove_file(cooked.c_str());
    (void)tynima::platform::remove_file(source.c_str());
}

TEST_CASE("an offscreen runtime draws into a texture and reads it back, or says it has no GPU") {
    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.offscreen = true;
    desc.width = 64;
    desc.height = 36;
    desc.gpu_debug = false;
    desc.max_entities = 64;
    REQUIRE_MESSAGE(runtime.create(desc), tynima::platform::last_error()); // with or without a device
    CHECK(runtime.window() != nullptr);
    std::uint8_t pixels[64 * 36 * 4] = {};
    if (runtime.device() == nullptr) {
        MESSAGE("no GPU device here: offscreen drawing not exercised");
        CHECK(runtime.scene_texture(64, 36) == 0);
        CHECK_FALSE(runtime.read_scene_texture(pixels, sizeof pixels));
        return;
    }
    // A cube in front of the camera, lit; three frames, then the picture.
    tynima::render::ModelData cube = tynima::render::plain_model(tynima::render::box_mesh(Vec3{0.5f}),
                                                                 Vec4{0.8f, 0.3f, 0.2f, 1.0f}, 0.5f);
    const std::uint32_t model = runtime.add_model(cube);
    (void)runtime.world().create(scene::Transform{.position = Vec3{0.0f, 0.0f, -2.0f}}, scene::LocalToWorld{},
                                 scene::MeshRenderer{.model = model});
    runtime.post().settings.anti_aliasing = tynima::render::AntiAliasing::None;
    runtime.sun.direction = normalize(Vec3{0.3f, 0.8f, 0.6f}); // on the face the camera sees
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(runtime.begin_frame());
        CHECK(runtime.scene_texture(64, 36) != 0);
        runtime.end_frame();
    }
    CHECK(runtime.scene_texture_width() == 64);
    CHECK(runtime.scene_texture_height() == 36);
    REQUIRE(runtime.read_scene_texture(pixels, sizeof pixels));
    CHECK_FALSE(runtime.read_scene_texture(pixels, 16)); // not the texture's size
    // The middle pixel is the cube (warm: red well over blue), a corner the
    // sky (cool: blue over red), and the two are not the same colour; alpha is one.
    const std::uint8_t* middle = pixels + (std::size_t{18} * 64 + 32) * 4;
    const std::uint8_t* corner = pixels;
    CHECK(middle[3] == 255);
    CHECK(corner[3] == 255);
    CHECK(middle[0] > middle[2] + 20);
    CHECK(corner[2] > corner[0]);
    CHECK(middle[2] != corner[2]);
    MESSAGE("middle ", int{middle[0]}, " ", int{middle[1]}, " ", int{middle[2]}, ", corner ", int{corner[0]},
            " ", int{corner[1]}, " ", int{corner[2]});
}
