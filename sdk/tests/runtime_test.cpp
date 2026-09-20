#include <tynima.h>

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
