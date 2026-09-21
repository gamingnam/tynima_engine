// tynima-golden — the renderer's golden image tests. Each scene below is
// built through the runtime, drawn offscreen for a few frames and read
// back as a picture; the picture is compared with the one kept under
// tools/golden/goldens/ (the golden), and the test fails when they look
// different — a perceptual comparison (engine/cooker/image_diff.h), so a
// GPU that puts an edge a pixel over or rounds its shading elsewhere
// passes, and a pass that broke does not. Every render goes to the output
// directory, and a failure leaves a strip beside it: the golden, the
// render and a heatmap of where they differ, for a look.
//
//   tynima-golden                 draw every scene and compare
//   tynima-golden shapes lights   only these
//   tynima-golden --update        make the renders the new goldens (look at them first)
//
// Exit status: 0 the same, 1 a difference or a missing golden, 2 a bad
// call, 77 no GPU to draw with (what CTest reads as skipped).
#include <tynima/cooker/image.h>
#include <tynima/cooker/image_diff.h>
#include <tynima/core/log.h>
#include <tynima/platform/file.h>
#include <tynima/platform/platform.h>
#include <tynima/render/shapes.h>
#include <tynima/scene/components.h>
#include <tynima/sdk/runtime.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tynima;
using namespace tynima::math;

namespace {

constexpr std::uint32_t kWidth = 480;
constexpr std::uint32_t kHeight = 270;

#ifndef TYNIMA_GOLDEN_DIR
#define TYNIMA_GOLDEN_DIR "goldens"
#endif
#ifndef TYNIMA_GOLDEN_OUT
#define TYNIMA_GOLDEN_OUT "golden"
#endif
#ifndef TYNIMA_GOLDEN_SAMPLE
#define TYNIMA_GOLDEN_SAMPLE ""
#endif

// The models every scene may use, by index into the runtime's slots.
struct Models {
    std::uint32_t ground = 0, box = 0, tall = 0, capsule = 0, sphere = 0;
    std::uint32_t bottle = sdk::Runtime::kNoModel;
};

Models load_models(sdk::Runtime& runtime, bool with_sample) {
    Models m;
    m.ground = runtime.add_model(render::plain_model(render::box_mesh(Vec3{6.0f, 0.1f, 6.0f}),
                                                     Vec4{0.42f, 0.42f, 0.40f, 1.0f}, 0.85f));
    m.box = runtime.add_model(
        render::plain_model(render::box_mesh(Vec3{0.5f, 0.5f, 0.5f}), Vec4{0.70f, 0.20f, 0.15f, 1.0f}, 0.5f));
    m.tall = runtime.add_model(
        render::plain_model(render::box_mesh(Vec3{0.3f, 1.0f, 0.3f}), Vec4{0.30f, 0.60f, 0.30f, 1.0f}, 0.7f));
    m.capsule = runtime.add_model(
        render::plain_model(render::capsule_mesh(0.4f, 0.4f), Vec4{0.20f, 0.45f, 0.85f, 1.0f}, 0.3f));
    m.sphere = runtime.add_model(
        render::plain_model(render::capsule_mesh(0.5f, 0.0f), Vec4{0.90f, 0.70f, 0.30f, 1.0f}, 0.25f));
    if (with_sample) {
        m.bottle = runtime.load_model(TYNIMA_GOLDEN_SAMPLE);
    }
    return m;
}

scene::Entity place(sdk::Runtime& runtime, const char* name, std::uint32_t model, const Vec3& position,
                    const Quat& rotation = Quat::identity(), const Vec3& scale = Vec3{1.0f}) {
    return runtime.world().create(
        scene::Name(name), scene::Transform{.position = position, .rotation = rotation, .scale = scale},
        scene::LocalToWorld{}, scene::MeshRenderer{.model = model});
}

// The shapes on the ground that most scenes draw: four objects, a ground
// slab, the sun from front-left, the camera from front-right and above.
void build_shapes(sdk::Runtime& runtime, const Models& m) {
    place(runtime, "ground", m.ground, Vec3{0.0f, -0.1f, 0.0f});
    place(runtime, "box", m.box, Vec3{-1.2f, 0.5f, 0.0f},
          Quat::from_axis_angle(Vec3::unit_y(), radians(30.0f)));
    place(runtime, "capsule", m.capsule, Vec3{0.6f, 0.8f, 0.3f});
    place(runtime, "sphere", m.sphere, Vec3{1.8f, 0.5f, -0.8f});
    place(runtime, "tall", m.tall, Vec3{-0.2f, 1.0f, -1.5f},
          Quat::from_axis_angle(Vec3::unit_y(), radians(-15.0f)));
    runtime.camera.position = Vec3{3.2f, 2.4f, 4.0f};
    runtime.camera.look_at(Vec3{0.3f, 0.6f, 0.0f});
    runtime.camera.fov_y = radians(50.0f);
    runtime.sun.direction = normalize(Vec3{0.4f, 1.0f, 0.3f});
    runtime.sun.intensity = 3.0f;
    runtime.sun.ambient = 0.10f;
    runtime.scene_settings.shadows = true;
    runtime.scene_settings.point_lights = false;
    runtime.scene_settings.model = render::ShadingModel::CookTorrance;
    runtime.scene_settings.path = render::ShadingPath::Forward;
    runtime.post().settings.tonemap = render::Tonemap::Aces;
    runtime.post().settings.bloom = true;
    runtime.post().settings.anti_aliasing = render::AntiAliasing::None;
}

struct Scene {
    const char* name;
    const char* about;
    void (*build)(sdk::Runtime&, const Models&);
    int frames = 3;              // drawn before the picture is taken; TAA needs many
    float max_fraction = 0.005f; // of pixels that may differ
    float max_mean = 0.004f;     // average OKLab distance over every pixel
    bool needs_sample = false;   // the water bottle, when the sandbox downloaded it
};

const Scene kScenes[] = {
    {"shapes", "the forward path: Cook-Torrance, cascaded shadows, ACES, bloom", build_shapes},
    {"shapes-fused", "the same through the fused deferred path (split where a GPU cannot fuse)",
     [](sdk::Runtime& runtime, const Models& m) {
         build_shapes(runtime, m);
         runtime.scene_settings.path = render::ShadingPath::Fused;
     }},
    {"shapes-split", "the same through the split deferred path",
     [](sdk::Runtime& runtime, const Models& m) {
         build_shapes(runtime, m);
         runtime.scene_settings.path = render::ShadingPath::Split;
     }},
    {"normals", "the mapped-normal data view: untonemapped, no bloom",
     [](sdk::Runtime& runtime, const Models& m) {
         build_shapes(runtime, m);
         runtime.scene_settings.debug_view = render::DebugView::Normals;
     }},
    {"lights", "six point lights through the clusters, the sun turned down",
     [](sdk::Runtime& runtime, const Models& m) {
         build_shapes(runtime, m);
         runtime.sun.intensity = 0.3f;
         runtime.scene_settings.point_lights = true;
         render::PointLight lights[6];
         const Vec3 colors[6] = {{3.0f, 0.4f, 0.4f}, {0.4f, 3.0f, 0.4f}, {0.4f, 0.4f, 3.0f},
                                 {3.0f, 3.0f, 0.4f}, {3.0f, 0.4f, 3.0f}, {0.4f, 3.0f, 3.0f}};
         for (int i = 0; i < 6; ++i) {
             const float angle = static_cast<float>(i) * (kTwoPi / 6.0f);
             lights[i].position_radius = Vec4{2.6f * std::cos(angle), 1.4f, 2.6f * std::sin(angle), 4.0f};
             lights[i].color = Vec4{colors[i], 0.0f};
         }
         runtime.set_lights(lights, 6);
     }},
    {"taa", "temporal anti-aliasing, converged over two dozen frames",
     [](sdk::Runtime& runtime, const Models& m) {
         build_shapes(runtime, m);
         runtime.post().settings.anti_aliasing = render::AntiAliasing::Taa;
     },
     24},
    {"fxaa", "FXAA over the same frame",
     [](sdk::Runtime& runtime, const Models& m) {
         build_shapes(runtime, m);
         runtime.post().settings.anti_aliasing = render::AntiAliasing::Fxaa;
     }},
    {"bottle", "the water bottle three times, near to far: its textures, normal map and mip chain",
     [](sdk::Runtime& runtime, const Models& m) {
         place(runtime, "ground", m.ground, Vec3{0.0f, -0.1f, 0.0f});
         const float base = -runtime.model(m.bottle)->mesh.bounds_min.y; // standing on the ground
         place(runtime, "near", m.bottle, Vec3{0.0f, base, 0.0f},
               Quat::from_axis_angle(Vec3::unit_y(), radians(20.0f)));
         place(runtime, "middle", m.bottle, Vec3{0.55f, base, -1.2f},
               Quat::from_axis_angle(Vec3::unit_y(), radians(-40.0f)));
         place(runtime, "far", m.bottle, Vec3{1.6f, base, -5.0f});
         runtime.camera.position = Vec3{0.28f, 0.26f, 0.48f}; // close: the near bottle fills half the height
         runtime.camera.look_at(Vec3{0.08f, 0.13f, -0.35f});
         runtime.camera.fov_y = radians(45.0f);
         runtime.sun.direction = normalize(Vec3{-0.5f, 1.0f, 0.6f});
         runtime.sun.intensity = 3.0f;
         runtime.sun.ambient = 0.12f;
         runtime.scene_settings.shadows = true;
         runtime.scene_settings.point_lights = false;
         runtime.post().settings.tonemap = render::Tonemap::Aces;
         runtime.post().settings.bloom = true;
         runtime.post().settings.anti_aliasing = render::AntiAliasing::None;
     },
     3, 0.005f, 0.004f, true},
};

struct Options {
    std::vector<std::string> only;
    std::string golden_dir = TYNIMA_GOLDEN_DIR;
    std::string out_dir = TYNIMA_GOLDEN_OUT;
    rhi::Backend backend = rhi::Backend::Auto;
    bool update = false;
    bool list = false;
    bool verbose = false;
};

void print_usage() {
    std::puts("usage: tynima-golden [options] [scene...]\n"
              "\n"
              "Draws each scene offscreen and compares the picture with its golden image;\n"
              "with no scene named, every one. A render always goes to the output directory,\n"
              "and a failure leaves <scene>.diff.png beside it: golden, render, heatmap.\n"
              "\n"
              "  -u, --update        write the renders as the new goldens\n"
              "  -o, --out <dir>     where renders and diffs go (default: the build tree's golden/)\n"
              "  -g, --golden <dir>  where the goldens are (default: tools/golden/goldens)\n"
              "      --rhi sdl|metal which backend draws\n"
              "  -l, --list          the scenes, and stop\n"
              "  -v, --verbose       the engine's log\n"
              "  -h, --help\n"
              "\n"
              "exit 0 the same, 1 different or no golden, 2 a bad call, 77 no GPU");
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "tynima-golden: %s needs a value\n", arg.c_str());
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return false;
        }
        if (arg == "-u" || arg == "--update") {
            options.update = true;
        } else if (arg == "-l" || arg == "--list") {
            options.list = true;
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "-o" || arg == "--out") {
            if (!value(options.out_dir)) {
                return false;
            }
        } else if (arg == "-g" || arg == "--golden") {
            if (!value(options.golden_dir)) {
                return false;
            }
        } else if (arg == "--rhi") {
            std::string name;
            if (!value(name)) {
                return false;
            }
            if (name == "sdl") {
                options.backend = rhi::Backend::SdlGpu;
            } else if (name == "metal") {
                options.backend = rhi::Backend::Metal;
            } else {
                std::fprintf(stderr, "tynima-golden: --rhi wants sdl or metal, not %s\n", name.c_str());
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "tynima-golden: unknown option %s\n", arg.c_str());
            return false;
        } else {
            bool known = false;
            for (const Scene& scene : kScenes) {
                known = known || arg == scene.name;
            }
            if (!known) {
                std::fprintf(stderr, "tynima-golden: no scene called %s (--list shows them)\n", arg.c_str());
                return false;
            }
            options.only.push_back(arg);
        }
    }
    return true;
}

bool wanted(const Options& options, const Scene& scene) {
    if (options.only.empty()) {
        return true;
    }
    for (const std::string& name : options.only) {
        if (name == scene.name) {
            return true;
        }
    }
    return false;
}

enum class Outcome { Same, Different, NoGolden, Updated, Skipped, NoGpu, Failed };

// Draws one scene into `picture`. NoGpu when the runtime came up without a
// device; Failed when the picture could not be taken.
Outcome render_scene(const Scene& scene, const Options& options, bool sample_present,
                     render::ImageData& picture) {
    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.title = "tynima-golden";
    desc.width = static_cast<int>(kWidth);
    desc.height = static_cast<int>(kHeight);
    desc.offscreen = true;
    desc.backend = options.backend;
    desc.gpu_debug = false;
    desc.max_entities = 256;
    desc.max_bodies = 16;
    if (!runtime.create(desc)) {
        std::fprintf(stderr, "tynima-golden: the runtime failed: %s\n", platform::last_error());
        return Outcome::Failed;
    }
    if (runtime.device() == nullptr) {
        return Outcome::NoGpu;
    }
    const Models models = load_models(runtime, scene.needs_sample && sample_present);
    if (scene.needs_sample && models.bottle == sdk::Runtime::kNoModel) {
        return Outcome::Skipped;
    }
    scene.build(runtime, models);
    for (int frame = 0; frame < scene.frames; ++frame) {
        if (!runtime.begin_frame()) {
            std::fprintf(stderr, "tynima-golden: %s: frame %d refused\n", scene.name, frame);
            return Outcome::Failed;
        }
        (void)runtime.scene_texture(kWidth, kHeight);
        runtime.end_frame();
    }
    picture.width = kWidth;
    picture.height = kHeight;
    picture.srgb = true;
    picture.pixels.resize(std::size_t{kWidth} * kHeight * 4);
    if (!runtime.read_scene_texture(picture.pixels.data(), picture.pixels.size())) {
        std::fprintf(stderr, "tynima-golden: %s: reading the picture back failed: %s\n", scene.name,
                     platform::last_error());
        return Outcome::Failed;
    }
    return Outcome::Same;
}

Outcome run_scene(const Scene& scene, const Options& options, bool sample_present) {
    render::ImageData picture;
    const Outcome drawn = render_scene(scene, options, sample_present, picture);
    if (drawn != Outcome::Same) {
        return drawn;
    }
    std::string error;
    const std::string golden_path = options.golden_dir + "/" + scene.name + ".png";
    const std::string out_path = options.out_dir + "/" + scene.name + ".png";
    if (!cooker::save_png(out_path.c_str(), picture, error)) {
        std::fprintf(stderr, "tynima-golden: %s\n", error.c_str());
        return Outcome::Failed;
    }
    if (options.update) {
        if (!cooker::save_png(golden_path.c_str(), picture, error)) {
            std::fprintf(stderr, "tynima-golden: %s\n", error.c_str());
            return Outcome::Failed;
        }
        return Outcome::Updated;
    }
    render::ImageData golden;
    if (!cooker::load_image_file(golden_path.c_str(), golden, error)) {
        std::printf("no golden  %-13s %s (tynima-golden --update makes it; look at it first)\n", scene.name,
                    golden_path.c_str());
        return Outcome::NoGolden;
    }
    render::ImageData heatmap;
    const cooker::DiffResult diff = cooker::diff_images(golden, picture, {}, &heatmap);
    const bool same = diff.comparable && diff.fraction <= scene.max_fraction && diff.mean <= scene.max_mean;
    if (!diff.comparable) {
        std::printf("different  %-13s the golden is %ux%u, the render %ux%u\n", scene.name, golden.width,
                    golden.height, picture.width, picture.height);
    } else {
        std::printf("%-10s %-13s %u pixel(s) differ (%.3f%%, at most %.3f%%), mean distance %.4f (at most "
                    "%.4f), largest %.3f\n",
                    same ? "same" : "different", scene.name, diff.differing,
                    static_cast<double>(diff.fraction) * 100.0,
                    static_cast<double>(scene.max_fraction) * 100.0, static_cast<double>(diff.mean),
                    static_cast<double>(scene.max_mean), static_cast<double>(diff.max));
    }
    if (!same) {
        const std::string diff_path = options.out_dir + "/" + scene.name + ".diff.png";
        const render::ImageData strip = cooker::diff_strip(golden, picture, heatmap);
        if (!strip.pixels.empty() && cooker::save_png(diff_path.c_str(), strip, error)) {
            std::printf("           golden | render | heatmap: %s\n", diff_path.c_str());
        }
        return Outcome::Different;
    }
    return Outcome::Same;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    if (options.list) {
        for (const Scene& scene : kScenes) {
            std::printf("%-13s %s%s\n", scene.name, scene.about,
                        scene.needs_sample ? " (needs the sample model)" : "");
        }
        return 0;
    }
    if (!options.verbose) {
        core::set_log_level(core::LogLevel::Warn);
    }
    if (!platform::make_directories(options.out_dir.c_str())) {
        std::fprintf(stderr, "tynima-golden: cannot make %s\n", options.out_dir.c_str());
        return 2;
    }
    if (options.update && !platform::make_directories(options.golden_dir.c_str())) {
        std::fprintf(stderr, "tynima-golden: cannot make %s\n", options.golden_dir.c_str());
        return 2;
    }
    const bool sample_present =
        TYNIMA_GOLDEN_SAMPLE[0] != '\0' && platform::file_write_time(TYNIMA_GOLDEN_SAMPLE) != 0;

    int failures = 0;
    int compared = 0;
    for (const Scene& scene : kScenes) {
        if (!wanted(options, scene)) {
            continue;
        }
        switch (run_scene(scene, options, sample_present)) {
        case Outcome::Same:
            ++compared;
            break;
        case Outcome::Updated:
            std::printf("updated    %-13s %s/%s.png\n", scene.name, options.golden_dir.c_str(), scene.name);
            ++compared;
            break;
        case Outcome::Skipped:
            std::printf("skipped    %-13s no sample model at %s\n", scene.name, TYNIMA_GOLDEN_SAMPLE);
            break;
        case Outcome::NoGpu:
            std::printf("tynima-golden: no GPU device to draw with; the engine's log (-v) says why\n");
            return 77;
        case Outcome::Different:
        case Outcome::NoGolden:
        case Outcome::Failed:
            ++failures;
            ++compared;
            break;
        }
    }
    if (compared == 0 && failures == 0) {
        std::printf("tynima-golden: nothing to compare\n");
    }
    std::printf("%d of %d %s\n", compared - failures, compared, options.update ? "updated" : "the same");
    return failures > 0 ? 1 : 0;
}
