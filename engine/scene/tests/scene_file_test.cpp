#include <tynima/core/reflect.h>
#include <tynima/scene/components.h>
#include <tynima/scene/scene_file.h>
#include <tynima/scene/world.h>

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using namespace tynima;
using namespace tynima::math;
using scene::Entity;
using scene::World;

namespace {

// Every kind of field there is, in one component.
struct Probe {
    static constexpr const char* kName = "Probe";
    bool flag = false;
    std::int8_t i8 = -3;
    std::uint16_t u16 = 60000;
    std::int32_t i32 = -100000;
    std::uint64_t u64 = 1ull << 40;
    float f = 0.1f;
    double d = 1.0 / 3.0;
    Vec2 v2{1.5f, -2.5f};
    Vec4 v4{0.0f, 0.25f, 0.5f, 1.0f};
    char label[24] = "";
    std::uint8_t raw[3] = {1, 2, 3}; // no traits: bytes
    physics::BodyHandle body;        // a handle: never saved
    float derived = 99.0f;           // read-only: never saved
    float secret = 7.0f;             // hidden: never saved
    Entity friend_entity;
};
TY_REFLECT(Probe, TY_FIELD(flag), TY_FIELD(i8), TY_FIELD(u16), TY_FIELD(i32), TY_FIELD(u64), TY_FIELD(f),
           TY_FIELD(d), TY_FIELD(v2), TY_FIELD(v4), TY_FIELD(label), TY_FIELD(raw), TY_FIELD(body),
           TY_FIELD_FLAGS(derived, core::kFieldReadOnly), TY_FIELD_FLAGS(secret, core::kFieldHidden),
           TY_FIELD(friend_entity));

struct Bare {
    static constexpr const char* kName = "Bare";
    int x = 0;
};

void register_all(World& world) {
    (void)world.component_id<scene::Name>();
    (void)world.component_id<scene::Transform>();
    (void)world.component_id<scene::LocalToWorld>();
    (void)world.component_id<scene::Parent>();
    (void)world.component_id<scene::MeshRenderer>();
    (void)world.component_id<scene::RigidBody>();
    (void)world.component_id<Probe>();
    (void)world.component_id<Bare>();
}

// Where this machine puts temporary files. std::filesystem asks the OS —
// TMPDIR where it is set, the user's Temp on Windows — rather than assuming
// "/tmp", which on Windows names a directory on whatever drive the test
// happens to run from and usually is not there at all.
std::string temp_path(const char* name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = "/tmp";
    }
    return (dir / name).lexically_normal().generic_string();
}

} // namespace

TEST_CASE("a scene is saved as TOML, one field a line, and loads back to the same values") {
    World world(64);
    register_all(world);
    const Entity floor =
        world.create(scene::Name("floor"),
                     scene::Transform{.position = {0.0f, -0.25f, 0.0f}, .scale = {12.0f, 0.5f, 12.0f}},
                     scene::LocalToWorld{}, scene::MeshRenderer{.model = 1});
    Probe probe;
    std::strcpy(probe.label, "he said \"hi\"\n");
    probe.friend_entity = floor;
    (void)world.create(scene::Name("crate 1"), scene::Transform{.position = {0.1f, 0.2f, 0.3f}},
                       scene::Parent{floor}, scene::RigidBody{}, probe);
    (void)world.create(); // an entity with nothing on it, saved as a bare [[entities]]
    const Entity gone = world.create(scene::Name("gone"));
    (void)world.destroy(gone);
    (void)world.create(Bare{.x = 5}); // undescribed: its name alone

    const std::string text = scene::save_scene_text(world);
    MESSAGE(text);
    const auto has = [&](const char* line) { return text.find(line) != std::string::npos; };
    CHECK(has("# Tynima scene\nversion = 1\n"));
    CHECK(has("[[entities]]\n[entities.Name]\ntext = \"floor\"\n[entities.Transform]\nposition = [0.0, "
              "-0.25, 0.0]\n"
              "rotation = [0.0, 0.0, 0.0, 1.0]\nscale = [12.0, 0.5, 12.0]\n[entities.LocalToWorld]\n"
              "[entities.MeshRenderer]\nmodel = 1\nvisible = true\n"));
    CHECK(has("text = \"crate 1\"\n"));
    CHECK(has("[entities.Parent]\nentity = 0\n"));          // the floor is the file's first entity
    CHECK(has("[entities.RigidBody]\n[entities.Probe]\n")); // the body's fields are all runtime state
    CHECK(has("f = 0.1\n"));                                // the fewest digits that read back
    CHECK(has("d = 0.3333333333333333\n"));
    CHECK(has("i8 = -3\nu16 = 60000\ni32 = -100000\nu64 = 1099511627776\n"));
    CHECK(has("v2 = [1.5, -2.5]\n"));
    CHECK(has("label = \"he said \\\"hi\\\"\\n\"\n"));
    CHECK(has("raw = \"010203\"\n"));
    CHECK(has("friend_entity = 0\n"));
    CHECK_FALSE(has("body"));
    CHECK_FALSE(has("derived"));
    CHECK_FALSE(has("secret"));
    CHECK_FALSE(has("gone"));
    CHECK(has("[[entities]]\n\n[[entities]]\n[entities.Bare]\n")); // the empty entity, then the bare one

    // Loaded into another world, it is the same scene: and saved again, the same text.
    World loaded(64);
    register_all(loaded);
    std::string error;
    REQUIRE_MESSAGE(scene::load_scene_text(loaded, text, error), error);
    CHECK(loaded.entity_count() == 4);
    CHECK(scene::save_scene_text(loaded) == text);
    Entity loaded_floor, loaded_crate;
    loaded.each<scene::Name>([&](Entity e, scene::Name& name) {
        if (std::string(name.text) == "floor")
            loaded_floor = e;
        if (std::string(name.text) == "crate 1")
            loaded_crate = e;
    });
    REQUIRE(loaded_floor);
    REQUIRE(loaded_crate);
    CHECK(loaded.get<scene::Parent>(loaded_crate)->entity == loaded_floor);
    const Probe* p = loaded.get<Probe>(loaded_crate);
    REQUIRE(p != nullptr);
    CHECK(p->f == 0.1f);
    CHECK(p->d == 1.0 / 3.0);
    CHECK(p->u64 == (1ull << 40));
    CHECK(p->i8 == -3);
    CHECK(p->v4.w == 1.0f);
    CHECK(std::string(p->label) == "he said \"hi\"\n");
    CHECK(p->raw[2] == 3);
    CHECK(p->friend_entity == loaded_floor);
    CHECK(p->derived == 99.0f); // from the defaults, since it was never saved
    CHECK(p->secret == 7.0f);
    CHECK_FALSE(p->body);
    CHECK(loaded.get<scene::Transform>(loaded_floor)->scale.x == 12.0f);
    CHECK(loaded.get<scene::LocalToWorld>(loaded_floor)->matrix.cols[0].x == 1.0f); // the default: identity
    CHECK(loaded.get<scene::RigidBody>(loaded_crate) != nullptr);

    // Loading again over it replaces; loading additively doubles.
    REQUIRE_MESSAGE(scene::load_scene_text(loaded, text, error), error);
    CHECK(loaded.entity_count() == 4);
    REQUIRE_MESSAGE(scene::load_scene_text(loaded, text, error, {.clear = false}), error);
    CHECK(loaded.entity_count() == 8);
}

TEST_CASE("a scene file is written to disk and read back, and a bad one leaves the world alone") {
    World world(64);
    register_all(world);
    (void)world.create(scene::Name("only"), scene::Transform{});
    const std::string path = temp_path("tynima_scene_test.toml");
    std::string error;
    REQUIRE_MESSAGE(scene::save_scene_file(world, path.c_str(), error), error);
    World loaded(64);
    register_all(loaded);
    REQUIRE_MESSAGE(scene::load_scene_file(loaded, path.c_str(), error), error);
    CHECK(loaded.entity_count() == 1);
    CHECK_FALSE(scene::load_scene_file(loaded, "/no/such/dir/scene.toml", error));
    CHECK(error.find("cannot read") != std::string::npos);
    std::remove(path.c_str());

    const auto refused = [&](const char* text, const char* reason) {
        std::string why;
        const bool ok = scene::load_scene_text(loaded, text, why);
        CHECK_MESSAGE(!ok, text);
        CHECK_MESSAGE(why.find(reason) != std::string::npos, why);
        CHECK(loaded.entity_count() == 1); // untouched
    };
    refused("version = 99\n", "line 1: a scene file version");
    refused("[[entities]]\n[entities.Nope]\n", "line 2: component Nope is not registered");
    refused("[[entities]]\n[entities.Transform]\nposition = [1, 2]\n", "line 3: position wants 3 numbers");
    refused("[[entities]]\n[entities.Transform]\nheight = 1.0\n", "line 3: Transform has no field height");
    refused("[[entities]]\n[entities.MeshRenderer]\nvisible = 1\n", "line 3: visible wants true or false");
    refused("[[entities]]\n[entities.MeshRenderer]\nmodel = -1\n",
            "line 3: model wants an integer that fits");
    refused("[[entities]]\n[entities.Name]\ntext = \"a name that is much too long for the component\"\n",
            "line 3: text wants a shorter string");
    refused("[entities.Name]\n", "line 1: a component table before any [[entities]]");
    refused("[[entities]]\nname = 3\n", "line 2: a key directly under [[entities]]");
    refused("[[entities]]\n[entities.Name]\ntext = \"open\n", "line 3: a string that never closes");
    refused("[[entities]]\n[entities.Transform]\nposition = [1, 2, 3] extra\n", "line 3: unexpected text");
    refused("[[entities]]\n[entities.Transform]\nposition = [1, 2, 3]\n[entities.Transform]\n",
            "line 4: Transform twice");
    refused("[[entities]]\n[entities.Probe]\nbody = 3\n", "line 3: Probe has no field body to load");
    refused("[[entities]]\n[entities.Probe]\nraw = \"0102\"\n", "line 3: raw wants a hex string of its size");
    // What the reader forgives: comments, blank lines, underscores in numbers,
    // integers where floats are wanted, a quoted table name, CRLF.
    std::string why;
    REQUIRE_MESSAGE(scene::load_scene_text(loaded,
                                           "# a comment\r\n\r\nversion = 1 # trailing\r\n[[entities]]\r\n"
                                           "[entities.\"Transform\"]\r\nposition = [1, 2_000, 3]\r\n"
                                           "[entities.Probe]\r\nf = 2\r\nfriend_entity = -1\r\n",
                                           why),
                    why);
    CHECK(loaded.entity_count() == 1);
    loaded.each<scene::Transform, Probe>([](Entity, scene::Transform& t, Probe& p) {
        CHECK(t.position.y == 2000.0f);
        CHECK(p.f == 2.0f);
        CHECK_FALSE(p.friend_entity);
    });
}
