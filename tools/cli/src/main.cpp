// tynima — the command a game is made and run with.
//
//     tynima new mygame     a project that runs, out of a template
//     tynima run            play it; save the script and it reloads
//     tynima cook           every model under assets/ into cooked blobs
//     tynima build          cook, and build a native game module if there is one
//
// Everything it does, it does through what a game has: the project file
// (sdk/project.h), the cooker, and the same runtime a game ships with —
// `tynima run` is the engine hosting the project, not a wrapper around
// another program. A project that outgrows the command loses nothing by
// leaving it: the file it reads is a dozen lines of TOML.
#include <tynima/cooker/cook.h>
#include <tynima/core/log.h>
#include <tynima/platform/file.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/sdk/project.h>
#include <tynima/sdk/runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace tynima;

namespace {

#ifndef TYNIMA_TEMPLATE_DIR
#define TYNIMA_TEMPLATE_DIR "templates"
#endif

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;

// The templates there are: the directories where `new` copies from. Read
// rather than listed, so a template is added by adding a directory.
std::string template_names() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(TYNIMA_TEMPLATE_DIR, ec)) {
        const std::string name = entry.path().filename().string();
        // A hidden directory is an editor's or a tool's, not a template.
        if (entry.is_directory(ec) && !name.empty() && name[0] != '.') {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out.empty() ? std::string("none: " TYNIMA_TEMPLATE_DIR " is not there") : out;
}

void print_usage() {
    std::puts("usage: tynima <command> [options]\n"
              "\n"
              "  new <name>     a project that runs, out of a template");
    std::printf("                   --template <t>  which one (%s)\n", template_names().c_str());
    std::puts("                   --into <dir>    where to put it (default: ./<name>)\n"
              "  run            play the project here\n"
              "                   --headless      no window and no GPU\n"
              "                   --frames <n>    stop after n frames\n"
              "                   --script <f>    a script other than the project's\n"
              "  cook           every model under the project's assets, into its cooked/\n"
              "                   --force         cook even what is already current\n"
              "  build          cook, then build a native game module if the project has one\n"
              "  help, version\n"
              "\n"
              "Every command but `new` looks for tynima.toml here and in the directories\n"
              "above, so they work from anywhere inside a project.");
}

// ---- the arguments a command takes, in the order they were given ----

struct Args {
    std::vector<std::string> positional;
    [[nodiscard]] bool flag(const char* name) const {
        for (const std::string& arg : flags) {
            if (arg == name) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] std::string option(const char* name, const std::string& fallback = {}) const {
        for (std::size_t i = 0; i + 1 < options.size(); i += 2) {
            if (options[i] == name) {
                return options[i + 1];
            }
        }
        return fallback;
    }
    std::vector<std::string> flags;
    std::vector<std::string> options;
    bool bad = false;
    std::string bad_reason;
};

// Options are `--name value`; flags are `--name` alone. Which is which is
// the command's business, so both are kept and asked for by name.
Args parse(int argc, char** argv, const std::vector<std::string>& takes_value) {
    Args args;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            args.positional.push_back(std::move(arg));
            continue;
        }
        const bool wants_value = std::find(takes_value.begin(), takes_value.end(), arg) != takes_value.end();
        if (!wants_value) {
            args.flags.push_back(std::move(arg));
            continue;
        }
        if (i + 1 >= argc) {
            args.bad = true;
            args.bad_reason = arg + " needs a value";
            return args;
        }
        args.options.push_back(std::move(arg));
        args.options.emplace_back(argv[++i]);
    }
    return args;
}

[[nodiscard]] bool open_project(sdk::Project& project) {
    std::string error;
    if (!sdk::find_project(".", project, error)) {
        std::fprintf(stderr, "tynima: %s\n", error.c_str());
        return false;
    }
    return true;
}

// ---- new ----

// The files a template is made of are text with {{name}} in them; anything
// else is copied as it is.
bool is_text(const std::filesystem::path& path) {
    static const char* kText[] = {".lua", ".md", ".toml", ".txt", ".json", ".h", ".c", ".cpp", ".cmake"};
    const std::string extension = path.extension().string();
    if (path.filename() == ".gitignore" || path.filename() == "CMakeLists.txt") {
        return true;
    }
    for (const char* known : kText) {
        if (extension == known) {
            return true;
        }
    }
    return false;
}

std::string substitute(std::string text, const std::string& name) {
    const std::string token = "{{name}}";
    for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at)) {
        text.replace(at, token.size(), name);
        at += name.size();
    }
    return text;
}

int command_new(int argc, char** argv) {
    const Args args = parse(argc, argv, {"--template", "--into"});
    if (args.bad || args.positional.size() != 1) {
        std::fprintf(stderr, "tynima new: %s\n",
                     args.bad ? args.bad_reason.c_str() : "expected one name, as in `tynima new mygame`");
        return kUsage;
    }
    const std::string name = args.positional[0];
    const std::string which = args.option("--template", "basic");
    const std::string into = args.option("--into", "./" + name);
    const std::filesystem::path source = std::filesystem::path(TYNIMA_TEMPLATE_DIR) / which;

    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec)) {
        std::fprintf(stderr, "tynima new: no template called '%s'. There is %s, in %s\n", which.c_str(),
                     template_names().c_str(), TYNIMA_TEMPLATE_DIR);
        return kUsage;
    }
    if (std::filesystem::exists(into, ec) && !std::filesystem::is_empty(into, ec)) {
        std::fprintf(stderr, "tynima new: %s is already there and is not empty\n", into.c_str());
        return kFailed;
    }
    if (!platform::make_directories(into.c_str())) {
        std::fprintf(stderr, "tynima new: cannot make %s\n", into.c_str());
        return kFailed;
    }

    int copied = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(source, ec)) {
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), source, ec);
        const std::filesystem::path destination = std::filesystem::path(into) / relative;
        if (entry.is_directory(ec)) {
            if (!platform::make_directories(destination.string().c_str())) {
                std::fprintf(stderr, "tynima new: cannot make %s\n", destination.string().c_str());
                return kFailed;
            }
            continue;
        }
        core::Bytes bytes;
        if (!platform::read_file(entry.path().string().c_str(), bytes)) {
            std::fprintf(stderr, "tynima new: cannot read %s\n", entry.path().string().c_str());
            return kFailed;
        }
        bool written = false;
        if (is_text(entry.path())) {
            const std::string text =
                substitute(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), name);
            written = platform::write_file(destination.string().c_str(), text.data(), text.size());
        } else {
            written = platform::write_file(destination.string().c_str(), bytes.data(), bytes.size());
        }
        if (!written) {
            std::fprintf(stderr, "tynima new: cannot write %s\n", destination.string().c_str());
            return kFailed;
        }
        ++copied;
    }

    // The project file is written rather than copied: one writer for the
    // format, so a template cannot fall behind it.
    sdk::Project project;
    project.name = name;
    project.script = "src/game.lua";
    project.title = name;
    std::string error;
    const std::string project_file = (std::filesystem::path(into) / sdk::kProjectFileName).string();
    if (!sdk::save_project(project, project_file.c_str(), error)) {
        std::fprintf(stderr, "tynima new: %s\n", error.c_str());
        return kFailed;
    }
    std::printf("%s: %d file(s) from the '%s' template, and %s\n", into.c_str(), copied, which.c_str(),
                sdk::kProjectFileName);
    std::printf("\n    cd %s\n    tynima run\n\nThen edit src/game.lua while it runs.\n", into.c_str());
    return kOk;
}

// ---- cook ----

int cook_project(const sdk::Project& project, bool force, core::JobSystem& jobs) {
    const std::string assets = project.asset_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(assets, ec)) {
        std::printf("nothing to cook: %s is not there\n", assets.c_str());
        return kOk;
    }
    const std::string cooked = project.cooked_path();
    if (!platform::make_directories(cooked.c_str())) {
        std::fprintf(stderr, "tynima cook: cannot make %s\n", cooked.c_str());
        return kFailed;
    }
    int failed = 0, done = 0, current = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(assets, ec)) {
        const std::string path = entry.path().generic_string();
        if (!entry.is_regular_file(ec) || !cooker::is_model_source(path.c_str())) {
            continue;
        }
        const std::string blob = cooker::cooked_path_for(path.c_str(), cooked.c_str());
        if (!force && cooker::cooked_is_current(path.c_str(), blob.c_str())) {
            ++current;
            continue;
        }
        std::string error;
        const double start = platform::now_seconds();
        if (!cooker::cook_model_file(path.c_str(), blob.c_str(), {.jobs = &jobs}, error)) {
            std::fprintf(stderr, "failed  %s: %s\n", path.c_str(), error.c_str());
            ++failed;
            continue;
        }
        std::printf("cooked  %s -> %s  %.2f s\n", path.c_str(), blob.c_str(),
                    platform::now_seconds() - start);
        ++done;
    }
    std::printf("%d cooked, %d already current, %d failed\n", done, current, failed);
    return failed > 0 ? kFailed : kOk;
}

int command_cook(int argc, char** argv) {
    const Args args = parse(argc, argv, {});
    sdk::Project project;
    if (!open_project(project)) {
        return kFailed;
    }
    core::JobSystem jobs;
    return cook_project(project, args.flag("--force"), jobs);
}

// ---- build ----

int command_build(int argc, char** argv) {
    const Args args = parse(argc, argv, {});
    sdk::Project project;
    if (!open_project(project)) {
        return kFailed;
    }
    core::JobSystem jobs;
    const int cooked = cook_project(project, args.flag("--force"), jobs);
    if (cooked != kOk) {
        return cooked;
    }
    // A project with C++ in it has its own CMake; one without needs nothing
    // built at all, which is the point of writing a game in Lua.
    const std::string cmake_file = project.resolve("CMakeLists.txt");
    if (platform::file_write_time(cmake_file.c_str()) == 0) {
        if (!project.module.empty()) {
            std::printf("this project names a module (%s) but has no CMakeLists.txt to build it\n",
                        project.module.c_str());
            return kFailed;
        }
        std::printf("nothing to build: %s is Lua\n", project.name.c_str());
        return kOk;
    }
    const std::string build_dir = project.resolve("build");
    const std::string configure = "cmake -S \"" + project.root + "\" -B \"" + build_dir + "\"";
    const std::string build = "cmake --build \"" + build_dir + "\"";
    std::printf("%s\n", configure.c_str());
    if (std::system(configure.c_str()) != 0) {
        return kFailed;
    }
    std::printf("%s\n", build.c_str());
    return std::system(build.c_str()) == 0 ? kOk : kFailed;
}

// ---- run ----

int command_run(int argc, char** argv) {
    const Args args = parse(argc, argv, {"--frames", "--script"});
    if (args.bad) {
        std::fprintf(stderr, "tynima run: %s\n", args.bad_reason.c_str());
        return kUsage;
    }
    sdk::Project project;
    if (!open_project(project)) {
        return kFailed;
    }
    const std::string script =
        args.option("--script").empty() ? project.script_path() : project.resolve(args.option("--script"));
    const std::string module = project.module_path();
    if (!script.empty() && platform::file_write_time(script.c_str()) == 0) {
        std::fprintf(stderr, "tynima run: %s is not there\n", script.c_str());
        return kFailed;
    }

    sdk::RuntimeDesc desc;
    desc.title = project.window_title().c_str();
    desc.width = project.width;
    desc.height = project.height;
    desc.headless = args.flag("--headless");
    desc.jolt = project.physics == "jolt";
    desc.script = script.empty() ? nullptr : script.c_str();
    desc.game_module = module.empty() ? nullptr : module.c_str();
    const std::string frames = args.option("--frames");
    desc.max_frames = frames.empty() ? -1 : std::strtol(frames.c_str(), nullptr, 10);

    sdk::Runtime runtime;
    if (!runtime.create(desc)) {
        std::fprintf(stderr, "tynima run: %s\n", platform::last_error());
        return kFailed;
    }
    if (runtime.script() == nullptr && !script.empty()) {
        std::fprintf(stderr, "tynima run: no script VM in this build\n");
        return kFailed;
    }
    if (runtime.script() != nullptr && !runtime.script()->loaded()) {
        std::fprintf(stderr, "tynima run: %s\n", runtime.script()->last_error());
        return kFailed;
    }
    while (runtime.begin_frame()) {
        runtime.end_frame();
    }
    return kOk;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return kUsage;
    }
    const std::string command = argv[1];
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return kOk;
    }
    if (command == "version" || command == "--version") {
        std::printf("tynima %s\n", tynima_get_version_string());
        return kOk;
    }
    if (command == "new") {
        return command_new(argc, argv);
    }
    if (command == "run") {
        return command_run(argc, argv);
    }
    if (command == "cook") {
        return command_cook(argc, argv);
    }
    if (command == "build") {
        return command_build(argc, argv);
    }
    std::fprintf(stderr, "tynima: no command called '%s'\n\n", command.c_str());
    print_usage();
    return kUsage;
}
