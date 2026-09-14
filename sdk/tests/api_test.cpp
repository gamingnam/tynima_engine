#include <doctest/doctest.h>
#include <tynima.h>
#include <tynima/platform/input.h>
#include <tynima/scene/components.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/game_module.h>

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
    const tynima_component_id transform =
        api.register_component(&engine, scene::Transform::kName, sizeof(scene::Transform), alignof(scene::Transform));
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
