// tynima-cook — the asset cooker's command line. Every model given, or
// found under a directory given, is cooked to a blob the runtime loads as
// it is (engine/cooker; assets/model_blob.h for what a blob holds). A blob
// newer than its source and of this engine's version is left alone, like
// make, so cooking a whole content tree again costs only the stat of each
// file. --watch keeps going: whenever a source changes it is cooked again,
// and an editor with the blob loaded shows the new one — the other half of
// the runtime's own cook-on-load, for a project that keeps blobs in one
// place with -o rather than a .cooked directory beside every source.
//
//   tynima-cook assets/                       cook every .gltf and .glb under assets/
//   tynima-cook bottle.glb -o cooked/         cooked/bottle.tymodel
//   tynima-cook assets/ -o cooked/ --watch    and keep them current
#include <tynima/assets/file_watch.h>
#include <tynima/cooker/cook.h>
#include <tynima/core/jobs.h>
#include <tynima/core/log.h>
#include <tynima/platform/file.h>
#include <tynima/platform/time.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace tynima;

namespace {

struct Options {
    std::vector<std::string> inputs;
    std::string out_dir;
    bool force = false;
    bool watch = false;
    bool quiet = false;
    cooker::CookOptions cook;
};

void print_usage() {
    std::puts(
        "usage: tynima-cook [options] <model or directory>...\n"
        "\n"
        "Cooks glTF models (.gltf, .glb) into .tymodel blobs the engine loads as they are:\n"
        "the mesh in GPU order, every image with its mip chain. A directory is searched\n"
        "for models. A blob newer than its source is left alone.\n"
        "\n"
        "  -o, --out <dir>    write blobs into <dir> (default: a .cooked directory beside each source)\n"
        "  -f, --force        cook even when the blob is newer than its source\n"
        "  -w, --watch        keep running and cook a source again whenever it changes\n"
        "      --no-optimize  keep the source's vertex and index order\n"
        "      --no-mips      level 0 only\n"
        "  -q, --quiet        only failures\n"
        "  -h, --help");
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return false;
        }
        if (arg == "-o" || arg == "--out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "tynima-cook: %s needs a directory\n", arg.c_str());
                return false;
            }
            options.out_dir = argv[++i];
        } else if (arg == "-f" || arg == "--force") {
            options.force = true;
        } else if (arg == "-w" || arg == "--watch") {
            options.watch = true;
        } else if (arg == "--no-optimize") {
            options.cook.optimize_mesh = false;
        } else if (arg == "--no-mips") {
            options.cook.mipmaps = false;
        } else if (arg == "-q" || arg == "--quiet") {
            options.quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "tynima-cook: unknown option %s\n", arg.c_str());
            return false;
        } else {
            options.inputs.push_back(arg);
        }
    }
    if (options.inputs.empty()) {
        print_usage();
        return false;
    }
    return true;
}

// Every model the inputs name: a file as itself, a directory searched.
bool gather(const Options& options, std::vector<std::string>& sources) {
    std::error_code ec;
    for (const std::string& input : options.inputs) {
        if (std::filesystem::is_directory(input, ec)) {
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::recursive_directory_iterator(input, ec)) {
                if (entry.is_regular_file(ec) && cooker::is_model_source(entry.path().string().c_str())) {
                    sources.push_back(entry.path().generic_string());
                }
            }
            if (ec) {
                std::fprintf(stderr, "tynima-cook: cannot search %s: %s\n", input.c_str(),
                             ec.message().c_str());
                return false;
            }
        } else if (cooker::is_model_source(input.c_str())) {
            sources.push_back(input);
        } else {
            std::fprintf(stderr, "tynima-cook: %s is not a model (.gltf, .glb) or a directory\n",
                         input.c_str());
            return false;
        }
    }
    return true;
}

std::string size_text(std::uint64_t bytes) {
    char text[32];
    if (bytes >= 1'000'000) {
        std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / 1'000'000.0);
    } else {
        std::snprintf(text, sizeof text, "%.0f kB", static_cast<double>(bytes) / 1'000.0);
    }
    return text;
}

// One source: cooked, or found current. False when it failed.
bool cook_one(const Options& options, const std::string& source) {
    const std::string cooked =
        cooker::cooked_path_for(source.c_str(), options.out_dir.empty() ? nullptr : options.out_dir.c_str());
    if (!options.force && cooker::cooked_is_current(source.c_str(), cooked.c_str())) {
        if (!options.quiet) {
            std::printf("current  %s\n", source.c_str());
        }
        return true;
    }
    const double start = platform::now_seconds();
    std::string error;
    if (!cooker::cook_model_file(source.c_str(), cooked.c_str(), options.cook, error)) {
        std::fprintf(stderr, "failed   %s: %s\n", source.c_str(), error.c_str());
        return false;
    }
    if (!options.quiet) {
        std::error_code ec;
        const std::uint64_t bytes = std::filesystem::file_size(cooked, ec);
        std::printf("cooked   %s -> %s  %s  %.2f s\n", source.c_str(), cooked.c_str(),
                    size_text(bytes).c_str(), platform::now_seconds() - start);
    }
    std::fflush(stdout);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    core::set_log_level(core::LogLevel::Warn); // the importer's warnings, nothing chattier
    core::JobSystem jobs;
    options.cook.jobs = &jobs;

    std::vector<std::string> sources;
    if (!gather(options, sources)) {
        return 2;
    }
    if (sources.empty()) {
        std::fprintf(stderr, "tynima-cook: no models found\n");
        return 2;
    }
    int failed = 0;
    for (const std::string& source : sources) {
        if (!cook_one(options, source)) {
            ++failed;
        }
    }
    if (!options.watch) {
        return failed > 0 ? 1 : 0;
    }

    // Watching: a source that changes is cooked again; one that failed is
    // tried again when it changes. Directories are searched again every so
    // often for models that are new.
    assets::FileWatch watch(0.25);
    std::vector<std::string> watched = sources;
    for (const std::string& source : watched) {
        (void)watch.watch(source.c_str());
    }
    std::printf("watching %zu model(s); Ctrl-C stops\n", watched.size());
    std::fflush(stdout);
    double last_search = platform::now_seconds();
    for (;;) {
        platform::sleep_ns(50'000'000);
        const double now = platform::now_seconds();
        assets::FileWatch::Id changed[32];
        const std::uint32_t count = watch.poll(now, changed, 32);
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)cook_one(options, watch.path(changed[i]));
        }
        if (now - last_search >= 2.0) {
            last_search = now;
            std::vector<std::string> found;
            if (gather(options, found)) {
                for (const std::string& source : found) {
                    bool known = false;
                    for (const std::string& existing : watched) {
                        known = known || existing == source;
                    }
                    if (!known) {
                        watched.push_back(source);
                        (void)watch.watch(source.c_str());
                        (void)cook_one(options, source);
                    }
                }
            }
        }
    }
}
