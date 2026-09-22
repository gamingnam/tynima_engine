#include <tynima.h>

#include <tynima/physics/physics.h>
#include <tynima/platform/input.h>
#include <tynima/scene/components.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/game_module.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

namespace scene = tynima::scene;
namespace platform = tynima::platform;

namespace {
struct Pos {
    static constexpr const char* kName = "Pos";
    float x = 0, y = 0, z = 0;
};
} // namespace

TEST_CASE("the C API drives the World, and agrees with the C++ API by name") {
    scene::World world(64);
    platform::Input input;
    tynima_engine engine;
    engine.world = &world;
    engine.input = &input;
    engine.time_seconds = 1.5;
    const tynima_api& api = tynima::sdk::api();
    CHECK(api.version == TYNIMA_API_VERSION);

    // Register from the C side, then use the same component from C++.
    const tynima_component_id pos = api.register_component(&engine, "Pos", sizeof(Pos), alignof(Pos));
    CHECK(api.register_component(&engine, "Pos", sizeof(Pos), alignof(Pos)) == pos);
    CHECK(world.component_id<Pos>() == pos);

    const Pos value{1, 2, 3};
    const void* values[] = {&value};
    const tynima_entity e = api.create_entity(&engine, &pos, values, 1);
    CHECK(e.generation != 0);
    CHECK(api.entity_alive(&engine, e));
    CHECK(api.entity_count(&engine) == 1);
    auto* p = static_cast<Pos*>(api.get_component(&engine, e, pos));
    REQUIRE(p != nullptr);
    CHECK(p->y == 2);
    p->y = 20;
    CHECK(world.get<Pos>(scene::Entity{e.index, e.generation})->y == 20); // the very same storage

    // add / remove through the API
    const tynima_component_id transform = api.register_component(
        &engine, scene::Transform::kName, sizeof(scene::Transform), alignof(scene::Transform));
    const scene::Transform t{.position = {5, 6, 7}};
    CHECK(api.add_component(&engine, e, transform, &t));
    CHECK_FALSE(api.add_component(&engine, e, transform, &t));
    CHECK(world.get<scene::Transform>(scene::Entity{e.index, e.generation})->position.z == 7);
    CHECK(api.remove_component(&engine, e, transform));
    CHECK(api.get_component(&engine, e, transform) == nullptr);

    // each_chunk hands out the contiguous columns
    for (int i = 0; i < 20; ++i) {
        const Pos more{float(i), 0, 0};
        const void* more_values[] = {&more};
        (void)api.create_entity(&engine, &pos, more_values, 1);
    }
    struct Sum {
        float x = 0;
        std::uint32_t count = 0;
    } sum;
    api.each_chunk(
        &engine, &pos, 1,
        [](void* user, const tynima_entity*, uint32_t count, void* const* columns) {
            auto* s = static_cast<Sum*>(user);
            const auto* column = static_cast<const Pos*>(columns[0]);
            for (uint32_t i = 0; i < count; ++i) {
                s->x += column[i].x;
            }
            s->count += count;
        },
        &sum);
    CHECK(sum.count == 21);
    CHECK(sum.x == doctest::Approx(1.0f + 190.0f));

    // keys and time come from the host's frame
    CHECK(api.time_seconds(&engine) == 1.5);
    CHECK_FALSE(api.key_down(&engine, TYNIMA_KEY_W));
    platform::Input::Writer::begin_frame(input);
    platform::Input::Writer::key(input, platform::Key::W, true, false);
    CHECK(api.key_down(&engine, TYNIMA_KEY_W));
    CHECK(api.key_pressed(&engine, TYNIMA_KEY_W));
    CHECK_FALSE(api.key_released(&engine, TYNIMA_KEY_W));

    CHECK(api.destroy_entity(&engine, e));
    CHECK_FALSE(api.entity_alive(&engine, e));
    CHECK(api.get_component(&engine, e, pos) == nullptr);
    CHECK_FALSE(api.destroy_entity(&engine, e));
}

TEST_CASE("physics through the C API: impulses reach the body, and nothing else is required") {
    scene::World world(8);
    tynima_engine engine;
    engine.world = &world;
    const tynima_api& api = tynima::sdk::api();

    // No physics world attached: a harmless no-op.
    api.body_add_impulse(&engine, tynima_body{1, 1}, tynima_vec3{0, 1, 0});

    auto physics = tynima::physics::create_jolt_world({.gravity = tynima::math::Vec3{0.0f}});
    engine.physics = physics.get();
    tynima::physics::BodyDesc box;
    box.shape = tynima::physics::Shape::box(tynima::math::Vec3{0.5f});
    box.mass = 1.0f;
    const tynima::physics::BodyHandle body = physics->create_body(box);
    REQUIRE(body);

    // The component's handle is the same bits the C side passes around.
    const scene::Entity e = world.create(scene::RigidBody{body});
    const auto* rb = world.get<scene::RigidBody>(e);
    const tynima_body c_body{rb->body.index, rb->body.generation};
    api.body_add_impulse(&engine, c_body, tynima_vec3{3.0f, 0.0f, 0.0f});
    api.body_add_impulse_at(&engine, c_body, tynima_vec3{0.0f, 0.0f, 1.0f}, tynima_vec3{0.5f, 0.0f, 0.0f});
    physics->step(1.0f / 60.0f);
    const tynima::physics::BodyState state = physics->body_state(body);
    CHECK(state.linear_velocity.x == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(state.angular_velocity.y != 0.0f); // the off-centre impulse spun it

    api.body_add_impulse(&engine, tynima_body{99, 99}, tynima_vec3{0, 1, 0}); // stale: ignored
    engine.physics = nullptr;
}

TEST_CASE("a missing game module fails to load with a reason") {
    scene::World world(8);
    tynima_engine engine;
    engine.world = &world;
    tynima::sdk::GameModule module("/nonexistent/game.module");
    CHECK_FALSE(module.load(engine));
    CHECK_FALSE(module.loaded());
    CHECK(module.last_error()[0] != '\0');
    CHECK_FALSE(module.poll(engine));
    module.update(engine, 0.016f); // harmless when nothing is loaded
    module.unload(engine);
}

// What sdk/tests/c_abi.c, compiled as C11, reports back about the header.
extern "C" {
int tynima_c_abi_api_version(void);
int tynima_c_abi_table_entries(void);
int tynima_c_abi_math(void);
int tynima_c_abi_structs(void);
const tynima_game* tynima_c_abi_game(void);
}

TEST_CASE("the header is C: a C11 translation unit sees the same ABI and the same answers") {
    // The same numbers on both sides of the language boundary.
    CHECK(tynima_c_abi_api_version() == static_cast<int>(TYNIMA_API_VERSION));
    CHECK(tynima_c_abi_table_entries() == static_cast<int>(sizeof(tynima_api) / sizeof(void*)) - 1);
    // The inline math and the value types, used the way a game in C uses them.
    CHECK(tynima_c_abi_math());
    CHECK(tynima_c_abi_structs());
    // A game module's entry point, filled in by C, read by the engine's loader.
    const tynima_game* game = tynima_c_abi_game();
    REQUIRE(game != nullptr);
    CHECK(game->api_version == TYNIMA_API_VERSION);
    CHECK(game->load != nullptr);
    CHECK(game->update != nullptr);
    CHECK(game->unload == nullptr); // a module need not want every one
}

TEST_CASE("the ABI's promises: the table only grows at the end, and older modules still load") {
    const tynima_api& api = tynima::sdk::api();
    CHECK(api.version == TYNIMA_API_VERSION);
    // Every entry is filled in: a null one is a module calling nothing.
    const void* const* entries = reinterpret_cast<const void* const*>(&api);
    const std::size_t count = sizeof(tynima_api) / sizeof(void*);
    for (std::size_t i = 1; i < count; ++i) {
        CHECK_MESSAGE(entries[i] != nullptr, "table entry ", i, " is null");
    }
    // The entries a version 6 module knows sit where they always did: its
    // header ended at set_component_defaults, entry 33 of the table.
    CHECK(offsetof(tynima_api, set_component_defaults) == sizeof(void*) * 33);
    CHECK(offsetof(tynima_api, quit) == sizeof(void*) * 34);
    CHECK(TYNIMA_API_VERSION_MIN <= TYNIMA_API_VERSION);
}
