#pragma once

#include <tynima.h>

#include <tynima/assets/file_watch.h>
#include <tynima/core/arena.h>
#include <tynima/core/jobs.h>
#include <tynima/core/log.h>
#include <tynima/physics/fixed_step.h>
#include <tynima/physics/physics.h>
#include <tynima/platform/events.h>
#include <tynima/platform/input.h>
#include <tynima/platform/input_log.h>
#include <tynima/platform/window.h>
#include <tynima/render/camera.h>
#include <tynima/render/frame_graph.h>
#include <tynima/render/model.h>
#include <tynima/render/post.h>
#include <tynima/render/scene_renderer.h>
#include <tynima/rhi/device.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/game_module.h>
#include <tynima/ui/imgui_layer.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The runtime: everything a program needs to run the engine, in one object.
// The platform, a window, a GPU device, the world, the physics, the scene
// renderer with the post stack behind it, the UI, the game module — brought
// up by create(), run a frame at a time by begin_frame() and end_frame(),
// which do what apps/sandbox's main() did by hand up to Phase 4. The C
// functions in tynima.h's host half wrap this for a host that sees only
// the public header (the editor); a host that links the engine (the
// sandbox) reaches the parts directly.
//
// A frame, in order: begin_frame() pumps the window's events, settles the
// clock and the input — or a replay's, or a script's — and starts the UI
// frame. The host then reads and changes the world, moves its camera and
// draws its panels. end_frame() reloads the game module when its file has
// changed and any model whose file has, runs the module, steps the physics
// at a fixed 60 Hz, updates the transforms, and renders: the world's draws
// through the scene renderer into HDR, the post stack from there to the
// window (or to the viewport texture the host asked for), the UI over the
// top.
namespace tynima::sdk {

struct RuntimeDesc {
    const char* title = "Tynima";
    int width = 1280; // points
    int height = 720;
    bool headless = false; // no window, no GPU, a fixed sixtieth of a second a frame
    // A GPU but no window: nothing is shown or presented, and the scene is
    // drawn only into the texture scene_texture() asks for, to be read back
    // with read_scene_texture() — a tool that renders to a file. A fixed
    // sixtieth of a second a frame, like headless. The platform starts with
    // a display when there is one and on the dummy driver when not; the
    // native backend draws either way, SDL GPU needs the display.
    bool offscreen = false;
    rhi::Backend backend = rhi::Backend::Auto;
    bool gpu_debug =
#ifndef NDEBUG
        true; // Metal validation: catches API misuse loudly
#else
        false;
#endif
    bool jolt = false; // the reference physics instead of the engine's own
    std::uint32_t max_entities = 65536;
    std::uint32_t max_bodies = 4096;
    const char* game_module = nullptr; // a shared library to load and hot-reload, or null
    long max_frames = -1;              // begin_frame() returns false past this many; -1: never

    // Time and input from a log instead of the clock and the keyboard: the
    // run is the recorded one, frame for frame. Sets max_frames to its length.
    const platform::InputLog* replay = nullptr;
    // Keep every frame's input and time, for recording(); reserves room for
    // max_frames frames, or ten minutes at 60 Hz.
    bool record = false;
    // Headless there is nobody at the keyboard: this plays one, a frame at a
    // time, through the same door as a replay.
    platform::InputFrame (*scripted_input)(long frame_index, float dt, void* user) = nullptr;
    void* scripted_input_user = nullptr;

    // Where a source model given to load_model() cooks to; null: a .cooked
    // directory beside the source.
    const char* cook_dir = nullptr;
    // How often the models' files are looked at for a change, in seconds.
    float asset_poll_seconds = 0.25f;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // False, with the reason in platform::last_error(), when the platform, the
    // window or the world cannot be made. A missing GPU is a message, not a
    // failure: the runtime runs without drawing.
    [[nodiscard]] bool create(const RuntimeDesc& desc);
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept { return world_ != nullptr; }

    // ---- the frame ----

    // False once the window has been closed, the OS asked to quit, or the
    // frame budget ran out; the host then stops.
    [[nodiscard]] bool begin_frame();
    void end_frame();
    [[nodiscard]] float dt() const noexcept { return dt_; }
    [[nodiscard]] double time() const noexcept { return game_time_; }
    [[nodiscard]] long frame_index() const noexcept { return frame_index_; }
    // Ends the run early: the next begin_frame() returns false.
    void quit() noexcept { quitting_ = true; }

    // ---- the parts ----

    [[nodiscard]] scene::World& world() noexcept { return *world_; }
    [[nodiscard]] physics::PhysicsWorld& physics() noexcept { return *physics_; }
    [[nodiscard]] const platform::Input& input() const noexcept { return input_; }
    [[nodiscard]] const std::vector<platform::Event>& events() const noexcept { return events_; }
    [[nodiscard]] platform::Window* window() noexcept { return window_.get(); }
    [[nodiscard]] rhi::Device* device() noexcept { return device_.get(); }
    [[nodiscard]] core::JobSystem& jobs() noexcept { return *jobs_; }
    // Per-frame scratch memory: reset at the top of every frame, never freed
    // piecemeal. Sized for a draw item per entity, and a few megabytes over.
    [[nodiscard]] core::Arena& frame_arena() noexcept { return *frame_arena_; }
    [[nodiscard]] ui::ImGuiLayer& ui() noexcept { return ui_; }
    [[nodiscard]] render::SceneRenderer& scene() noexcept { return scene_; }
    [[nodiscard]] const render::SceneRenderer& scene() const noexcept { return scene_; }
    [[nodiscard]] render::PostStack& post() noexcept { return post_; }
    [[nodiscard]] const render::PostStack& post() const noexcept { return post_; }
    [[nodiscard]] const render::FrameGraph& graph() const noexcept { return *graph_; }
    [[nodiscard]] GameModule* game() noexcept { return game_.get(); }
    [[nodiscard]] tynima_engine& context() noexcept { return context_; }
    [[nodiscard]] const physics::FixedStepper& stepper() const noexcept { return stepper_; }
    [[nodiscard]] const RuntimeDesc& desc() const noexcept { return desc_; }

    // What the scene is seen from and lit by, and how it is drawn. The host
    // changes these between frames; the renderer reads them in end_frame().
    render::Camera camera;
    render::Sun sun;
    render::SceneSettings scene_settings;
    math::Vec3 sky{0.09f, 0.10f, 0.12f};

    // ---- content ----

    // A model the world can draw: uploaded to the device (kept as an empty
    // slot without one, so the indices are the same headless) and given the
    // index MeshRenderer::model refers to. Never freed until destroy().
    std::uint32_t add_model(const render::ModelData& data);
    // A model from a file: a cooked blob (.tymodel), or a source (.gltf,
    // .glb) cooked first — into cook_dir, unless a blob newer than it is
    // there already — and loaded from the blob. kNoModel with the error
    // logged. The file is watched from then on: when it changes (the blob,
    // or the source, which is cooked again), the model is loaded again into
    // the same index, and every entity drawing it shows the new one.
    static constexpr std::uint32_t kNoModel = TYNIMA_NO_MODEL;
    std::uint32_t load_model(const char* path);
    [[nodiscard]] std::uint32_t model_count() const noexcept {
        return static_cast<std::uint32_t>(models_.size());
    }
    [[nodiscard]] const render::Model* model(std::uint32_t index) const noexcept {
        return index < models_.size() ? &models_[index] : nullptr;
    }
    // This frame's point lights, copied: at most render::kMaxSceneLights.
    void set_lights(const render::PointLight* lights, std::uint32_t count) noexcept;
    // Every entity destroyed. (Physics bodies are the host's, and stay.)
    void clear_scene();

    // The nearest drawable entity along a ray (origin, unit direction, in
    // world space): the first whose model's bounds the ray enters, with
    // the distance to that face. The null entity when it hits nothing.
    [[nodiscard]] scene::Entity pick(const math::Vec3& origin, const math::Vec3& direction,
                                     float& distance) const noexcept;

    // ---- hooks ----

    // Called before every fixed physics step with the step's length: a
    // character controller moves here, so it moves once per step.
    void set_before_step(void (*fn)(float step, void* user), void* user) noexcept {
        before_step_ = fn;
        before_step_user_ = user;
    }

    // ---- the UI's viewport ----

    // Draws the scene this frame into a texture of this many pixels instead
    // of the window, and returns it as ImGui names it (ui::ImGuiLayer::texture_id).
    // 0 without a device. The window is cleared behind the UI instead.
    std::uint64_t scene_texture(std::uint32_t width, std::uint32_t height);
    // The last picture drawn into that texture, as width x height x 4 bytes
    // of display-encoded (sRGB) RGBA, rows top to bottom — a PNG's bytes.
    // Waits for the GPU. False without a device or a texture, or when `size`
    // is not the texture's.
    [[nodiscard]] bool read_scene_texture(std::uint8_t* rgba, std::size_t size) noexcept;
    [[nodiscard]] std::uint32_t scene_texture_width() const noexcept { return viewport_width_; }
    [[nodiscard]] std::uint32_t scene_texture_height() const noexcept { return viewport_height_; }

    // ---- the record ----

    // With RuntimeDesc::record: every frame so far, ended with the physics
    // world's hash and step count as of now.
    [[nodiscard]] platform::InputLog recording() const;

    // ---- the log ring ----

    [[nodiscard]] std::uint64_t log_count() const noexcept;
    [[nodiscard]] bool log_get(std::uint64_t index, tynima_log_entry& out) const noexcept;

    // ---- statistics, as of the last end_frame() ----

    struct Stats {
        float frame_ms = 0.0f;
        std::uint64_t heap_allocations = 0; // engine code's, in the last frame
        std::uint32_t game_reloads = 0;
        std::uint32_t model_reloads = 0; // models loaded again after their file changed
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct LogRing;
    static constexpr rhi::TextureFormat kHdrFormat = rhi::TextureFormat::Rgba16Float;

    // A model that came from a file, and so can come from it again.
    struct ModelFile {
        std::uint32_t index = 0;
        std::string source; // empty when a blob was given directly
        std::string cooked;
        assets::FileWatch::Id source_watch = assets::FileWatch::kNone;
        assets::FileWatch::Id cooked_watch = assets::FileWatch::kNone;
    };
    [[nodiscard]] bool load_model_file(ModelFile& file, render::Model& out);
    void poll_models();
    void render_frame();
    void report_graph() noexcept;
    [[nodiscard]] bool ensure_viewport(std::uint32_t width, std::uint32_t height);
    static void log_sink(const core::LogEvent& event, void* user) noexcept;

    RuntimeDesc desc_{};
    std::unique_ptr<LogRing> log_;
    std::unique_ptr<core::JobSystem> jobs_;
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<render::FrameGraph> graph_;
    render::SceneRenderer scene_;
    render::PostStack post_;
    ui::ImGuiLayer ui_;
    render::FallbackTextures fallbacks_{};
    std::vector<render::Model> models_;
    std::vector<ModelFile> model_files_;
    assets::FileWatch model_watch_;
    std::unique_ptr<scene::World> world_;
    std::unique_ptr<physics::PhysicsWorld> physics_;
    std::unique_ptr<GameModule> game_;
    tynima_engine context_{};
    std::unique_ptr<core::Arena> frame_arena_;
    platform::Input input_;
    std::vector<platform::Event> events_;
    platform::InputLog log_record_;
    physics::FixedStepper stepper_;
    render::PointLight lights_[render::kMaxSceneLights]{};
    std::uint32_t light_count_ = 0;
    void (*before_step_)(float, void*) = nullptr;
    void* before_step_user_ = nullptr;

    // The viewport texture, when a host asks for one.
    rhi::TextureHandle viewport_;
    std::uint32_t viewport_width_ = 0, viewport_height_ = 0;
    bool viewport_requested_ = false;

    bool frame_open_ = false;
    bool quitting_ = false;
    bool platform_ours_ = false;
    bool reported_swapchain_ = false;
    bool game_reloaded_ = false;
    bool models_reloaded_ = false;
    std::uint32_t last_culled_ = 0;
    long frame_index_ = 0;
    float dt_ = 0.0f;
    double game_time_ = 0.0;
    double last_time_ = 0.0;
    double last_report_ = 0.0;
    long frames_since_report_ = 0;
    std::uint64_t begin_allocations_ = 0, begin_total_ = 0;
    Stats stats_{};
};

} // namespace tynima::sdk
