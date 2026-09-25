#include <tynima/platform/file.h>
#include <tynima/sdk/project.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace sdk = tynima::sdk;
namespace platform = tynima::platform;

namespace {

// Spelled the way the project reports it: a temporary directory ends in a
// slash on some machines and not on others, and a project's paths are
// normalised. std::filesystem asks the OS where that directory is, which on
// Windows is an absolute path with a drive on it — "/tmp" there is neither,
// and find_project, which makes what it is given absolute, would then
// disagree with load_project about where the project's root is.
std::string temp_dir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = "/tmp";
    }
    return (dir / "tynima_project_test").lexically_normal().generic_string();
}

void write(const std::string& path, const std::string& text) {
    REQUIRE(platform::write_file(path.c_str(), text.data(), text.size()));
}

} // namespace

TEST_CASE("a project file is written, read back the same, and its paths are the project's") {
    const std::string dir = temp_dir();
    REQUIRE(platform::make_directories((dir + "/deep/deeper").c_str()));
    const std::string file = dir + "/" + sdk::kProjectFileName;

    sdk::Project project;
    project.name = "mygame";
    project.script = "src/game.lua";
    project.title = "My Game";
    project.width = 1920;
    project.height = 1080;
    project.physics = "jolt";
    std::string error;
    REQUIRE_MESSAGE(sdk::save_project(project, file.c_str(), error), error);

    sdk::Project loaded;
    REQUIRE_MESSAGE(sdk::load_project(file.c_str(), loaded, error), error);
    CHECK(loaded.name == "mygame");
    CHECK(loaded.script == "src/game.lua");
    CHECK(loaded.title == "My Game");
    CHECK(loaded.window_title() == "My Game");
    CHECK(loaded.width == 1920);
    CHECK(loaded.height == 1080);
    CHECK(loaded.physics == "jolt");
    CHECK(loaded.asset_dir == "assets");
    CHECK(sdk::write_project_text(loaded) == sdk::write_project_text(project));

    // Paths are relative to the file, whatever directory the tool runs in;
    // an absolute one is left alone, and an empty one stays empty.
    CHECK(loaded.script_path() == dir + "/src/game.lua");
    CHECK(loaded.cooked_path() == dir + "/cooked");
    CHECK(loaded.module_path().empty());
    CHECK(loaded.resolve("/somewhere/else") == "/somewhere/else");
    // A directory is as good as the file in it.
    sdk::Project by_directory;
    REQUIRE(sdk::load_project(dir.c_str(), by_directory, error));
    CHECK(by_directory.name == "mygame");
    // However the file is named, the project's paths read the same: a tool
    // that found it by walking up and one that was handed a clumsy path
    // must agree about where the game is.
    sdk::Project clumsy;
    REQUIRE(sdk::load_project((dir + "/./" + sdk::kProjectFileName).c_str(), clumsy, error));
    CHECK(clumsy.root == loaded.root);
    CHECK(clumsy.script_path() == loaded.script_path());
    // And a tool run anywhere inside the project finds it above.
    sdk::Project found;
    REQUIRE_MESSAGE(sdk::find_project((dir + "/deep/deeper").c_str(), found, error), error);
    CHECK(found.name == "mygame");
    CHECK(found.script_path() == loaded.script_path());
    // The name a project without a title is known by.
    sdk::Project untitled;
    untitled.name = "plain";
    CHECK(untitled.window_title() == "plain");
}

TEST_CASE("a project file that is wrong says which line, and nothing is half-read") {
    const std::string dir = temp_dir();
    REQUIRE(platform::make_directories(dir.c_str()));
    const std::string file = dir + "/broken.toml";
    sdk::Project out;
    std::string error;
    const auto refused = [&](const std::string& text, const char* reason) {
        write(file, text);
        CHECK_FALSE(sdk::load_project(file.c_str(), out, error));
        CHECK_MESSAGE(error.find(reason) != std::string::npos, error);
    };
    refused("[game]\nscript = \"a.lua\"\nwidth = \"wide\"\n", "line 3: width wants a number");
    refused("[game]\nscript = 5\n", "line 2: script wants a string");
    refused("[game]\nscript = \"a.lua\"\nphysics = \"havok\"\n", "line 3: physics is");
    refused("[nope]\n", "line 1: no section called [nope]");
    refused("[game]\nscrpit = \"a.lua\"\n", "line 2: [game] has no setting called scrpit");
    refused("script = \"a.lua\"\n", "line 1: a key before any section");
    refused("[game]\nscript = \"open\n", "line 2: a string that never closes");
    refused("[game\n", "line 1: a section that never closes");
    refused("[game]\nscript\n", "line 2: a line that is neither");
    refused("[project]\nname = \"empty\"\n", "neither a script nor a module");
    CHECK_FALSE(sdk::load_project((dir + "/not-there.toml").c_str(), out, error));
    CHECK(error.find("cannot read") != std::string::npos);
    CHECK_FALSE(sdk::load_project(nullptr, out, error));

    // What it forgives: comments, blank lines, spacing, a comment after a value.
    write(file, "# a game\n\n[project]\n   name   =   \"forgiving\"  # its name\n\n"
                "[game]\nscript = \"src/game.lua\"\nwidth = 800 # points\n");
    sdk::Project ok;
    REQUIRE_MESSAGE(sdk::load_project(file.c_str(), ok, error), error);
    CHECK(ok.name == "forgiving");
    CHECK(ok.width == 800);
    CHECK(ok.height == 720); // untouched keeps its default
    (void)platform::remove_file(file.c_str());
}
