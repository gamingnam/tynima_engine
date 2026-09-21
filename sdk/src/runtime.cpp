#include <tynima/sdk/runtime.h>

#include <tynima/assets/model_blob.h>
#include <tynima/cooker/cook.h>
#include <tynima/core/assert.h>
#include <tynima/core/memory.h>
#include <tynima/core/profile.h>
#include <tynima/core/version.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/scene/components.h>
#include <tynima/scene/systems.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace tynima::sdk {

using namespace tynima::math;

// The last TYNIMA_LOG_RING events, copied out of the log as they happen.
// Sinks run on the logging thread, so a lock guards the ring against the
// host reading it from the main thread.
struct Runtime::LogRing {
    mutable std::mutex mutex;
    tynima_log_entry entries[TYNIMA_LOG_RING];
    std::uint64_t count = 0; // ever kept

    void push(const core::LogEvent& event) noexcept {
        const std::lock_guard<std::mutex> lock(mutex);
        tynima_log_entry& entry = entries[count % TYNIMA_LOG_RING];
        entry.index = count;
        entry.time_ns = event.time_ns;
        entry.level = static_cast<tynima_log_level>(event.level);
        entry.thread = event.thread_index;
        std::snprintf(entry.category, sizeof entry.category, "%s",
                      event.category != nullptr ? event.category : "");
        std::snprintf(entry.message, sizeof entry.message, "%s",
                      event.message != nullptr ? event.message : "");
        ++count;
    }
    [[nodiscard]] bool get(std::uint64_t index, tynima_log_entry& out) const noexcept {
        const std::lock_guard<std::mutex> lock(mutex);
        if (index >= count || count - index > TYNIMA_LOG_RING) {
            return false;
        }
        out = entries[index % TYNIMA_LOG_RING];
        return true;
    }
};

void Runtime::log_sink(const core::LogEvent& event, void* user) noexcept {
    static_cast<LogRing*>(user)->push(event);
}

Runtime::Runtime() = default;

Runtime::~Runtime() {
    destroy();
}

bool Runtime::create(const RuntimeDesc& desc) {
    destroy();
    desc_ = desc;
    log_ = std::make_unique<LogRing>();
    (void)core::add_log_sink(log_sink, log_.get());

    if (desc.replay != nullptr) {
        desc_.max_frames = static_cast<long>(desc.replay->frames.size());
        desc_.jolt = desc.replay->backend.rfind("Jolt", 0) == 0;
    }
    if (desc.record) {
        // Room for the whole recording up front, so that taking a frame down
        // is not a heap allocation in that frame.
        constexpr long kTenMinutesOfFrames = 10 * 60 * 60;
        log_record_.frames.reserve(
            static_cast<std::size_t>(desc_.max_frames >= 0 ? desc_.max_frames : kTenMinutesOfFrames));
    }

    TY_LOG_INFO("runtime", "engine %s, log level %s", core::version_string(),
                core::log_level_name(core::log_level()));
    platform_ours_ = !platform::is_initialized();
    bool platform_up = platform::init({.headless = desc.headless});
    if (!platform_up && desc.offscreen && platform_ours_) {
        // No display to speak of: the dummy driver will do for a window
        // nobody sees, and the native GPU backend does not need one.
        TY_LOG_INFO("platform", "no display (%s): drawing offscreen without one", platform::last_error());
        platform::shutdown();
        platform_up = platform::init({.headless = true});
    }
    if (!platform_up) {
        TY_LOG_ERROR("platform", "init failed: %s", platform::last_error());
        return false;
    }
    window_ = platform::Window::create(
        {.title = desc.title, .width = desc.width, .height = desc.height, .hidden = desc.offscreen});
    if (window_ == nullptr) {
        TY_LOG_ERROR("window", "creation failed: %s", platform::last_error());
        return false;
    }
    TY_LOG_INFO("window", "%dx%d points, %dx%d pixels, density %.2f%s", window_->width(), window_->height(),
                window_->pixel_width(), window_->pixel_height(),
                static_cast<double>(window_->pixel_density()), desc.headless ? " (headless)" : "");
    TY_LOG_INFO("profile", "%s",
                core::profiling_compiled()
                    ? "tracy instrumentation compiled in; connect the Tracy GUI to 127.0.0.1"
                    : "off (TYNIMA_PROFILE=OFF)");

    jobs_ = std::make_unique<core::JobSystem>();
    TY_LOG_INFO("jobs", "%u worker thread(s) + main, for %u performance cores", jobs_->worker_count(),
                core::JobSystem::performance_core_count());

    // Physics steps on the same job system; bodies drive entity Transforms.
    const physics::WorldDesc physics_desc{.max_bodies = desc_.max_bodies, .jobs = jobs_.get()};
    physics_ =
        desc_.jolt ? physics::create_jolt_world(physics_desc) : physics::create_tynima_world(physics_desc);
    TY_LOG_INFO("physics", "%s%s", physics_->backend_name(),
                desc_.jolt ? " (the reference; the default is the engine's own)"
                           : " (--physics jolt for the reference)");
    world_ = std::make_unique<scene::World>(desc_.max_entities);
    frame_arena_ = std::make_unique<core::Arena>(std::max<std::size_t>(
        4u * 1024u * 1024u, desc_.max_entities * sizeof(render::DrawItem) + 1024u * 1024u));
    // The built-in components, registered up front so a host finds them by
    // name before any entity has them.
    (void)world_->component_id<scene::Name>();
    (void)world_->component_id<scene::Transform>();
    (void)world_->component_id<scene::LocalToWorld>();
    (void)world_->component_id<scene::Parent>();
    (void)world_->component_id<scene::MeshRenderer>();
    (void)world_->component_id<scene::RigidBody>();

    // The GPU: a missing one is a message, and the runtime runs without drawing.
    if (!desc.headless) {
        device_ = rhi::Device::create({.backend = desc.backend, .debug = desc.gpu_debug});
        if (device_ == nullptr) {
            TY_LOG_ERROR("gpu", "unavailable: %s", platform::last_error());
        } else {
            TY_LOG_INFO("gpu", "%s backend on %s, wants %s shaders, depth %s",
                        rhi::backend_name(device_->backend()), device_->backend_name(),
                        rhi::shader_format_name(device_->shader_format()),
                        rhi::texture_format_name(device_->preferred_depth_format()));
            if (desc.offscreen) {
                TY_LOG_INFO("gpu", "offscreen: nothing is presented");
            } else if (!device_->attach_window(*window_)) {
                TY_LOG_ERROR("gpu", "cannot present to this window: %s", platform::last_error());
                device_.reset();
            } else {
                TY_LOG_INFO("swap", "%s",
                            device_->swapchain_is_linear() ? "sRGB-encoded by the display hardware"
                                                           : "plain SDR; the shader encodes sRGB itself");
            }
        }
    }
    // The graph and the stacks exist without a device too: they declare, and draw nothing.
    graph_ = std::make_unique<render::FrameGraph>(device_.get());
    if (device_ != nullptr) {
        if (!render::create_fallback_textures(*device_, fallbacks_)) {
            TY_LOG_ERROR("gpu", "fallback textures failed: %s", platform::last_error());
        }
        if (!scene_.create(*device_, kHdrFormat)) {
            TY_LOG_ERROR("gpu", "scene renderer failed: %s", platform::last_error());
        }
        if (!post_.create(*device_, kHdrFormat, device_->swapchain_format())) {
            TY_LOG_ERROR("gpu", "post stack failed: %s", platform::last_error());
        }
    }
    if (!ui_.create(device_.get(),
                    device_ != nullptr ? device_->swapchain_format() : rhi::TextureFormat::Bgra8Unorm,
                    device_ != nullptr && device_->swapchain_is_linear())) {
        TY_LOG_ERROR("ui", "%s", platform::last_error());
    }

    context_ = tynima_engine{};
    context_.world = world_.get();
    context_.input = &input_;
    context_.physics = physics_.get();
    context_.runtime = this;
    if (desc.game_module != nullptr && desc.game_module[0] != '\0') {
        game_ = std::make_unique<GameModule>(desc.game_module);
        TY_LOG_INFO("game", "loading %s", game_->path().c_str());
        if (!game_->load(context_)) {
            TY_LOG_ERROR("game", "%s - the scene will not move", game_->last_error());
        }
    }
    model_watch_ = assets::FileWatch(static_cast<double>(desc.asset_poll_seconds));
    events_.reserve(64); // a frame's worth; growing later would count as a frame allocation
    last_time_ = platform::now_seconds();
    last_report_ = last_time_;
    return true;
}

void Runtime::destroy() noexcept {
    if (log_ == nullptr) {
        return;
    }
    if (frame_open_) {
        ui_.end_frame(*window_);
        frame_open_ = false;
    }
    if (game_ != nullptr) {
        game_->unload(context_);
        game_.reset();
    }
    // GPU objects go before their device, and the device before the window.
    ui_.destroy();
    graph_.reset();
    post_.destroy();
    scene_.destroy();
    if (device_ != nullptr) {
        for (render::Model& model : models_) {
            render::destroy_model(*device_, model);
        }
        render::destroy_fallback_textures(*device_, fallbacks_);
        device_->destroy_texture(viewport_);
    }
    models_.clear();
    model_files_.clear();
    model_watch_ = assets::FileWatch();
    viewport_ = {};
    viewport_width_ = viewport_height_ = 0;
    device_.reset();
    world_.reset();
    physics_.reset();
    frame_arena_.reset();
    jobs_.reset();
    window_.reset();
    if (platform_ours_) {
        platform::shutdown();
    }
    (void)core::remove_log_sink(log_sink, log_.get());
    log_.reset();
    context_ = tynima_engine{};
    quitting_ = false;
    frame_index_ = 0;
    game_time_ = 0.0;
    light_count_ = 0;
    stats_ = Stats{};
}

bool Runtime::begin_frame() {
    TY_ASSERT(created(), "Runtime::begin_frame before create()");
    if (!created() || quitting_ || (desc_.max_frames >= 0 && frame_index_ >= desc_.max_frames)) {
        return false;
    }
    frame_arena_->reset();
    begin_allocations_ = core::engine_allocation_count();
    begin_total_ = core::heap_allocation_count();
    {
        TY_PROFILE_SCOPE_NAMED("events");
        platform::pump_events(input_, events_);
    }
    // Time and input: the clock and the keyboard, or the log's — and
    // headless, a fixed frame of a sixtieth and the script — settled here,
    // before anything reads either.
    const double now = platform::now_seconds();
    dt_ = static_cast<float>(std::min(now - last_time_, 0.1)); // clamp hitches
    last_time_ = now;
    if (desc_.replay != nullptr) {
        const platform::InputFrame& frame = desc_.replay->frames[static_cast<std::size_t>(frame_index_)];
        platform::apply_frame(frame, input_);
        dt_ = frame.dt;
    } else if (desc_.headless || desc_.offscreen) {
        dt_ = 1.0f / 60.0f;
        if (desc_.scripted_input != nullptr) {
            platform::apply_frame(desc_.scripted_input(frame_index_, dt_, desc_.scripted_input_user), input_);
        }
    }
    if (desc_.record) {
        log_record_.frames.push_back(platform::capture_frame(input_, dt_));
    }
    game_time_ += static_cast<double>(dt_);
    for (const platform::Event& event : events_) {
        switch (event.type) {
        case platform::EventType::Quit:
        case platform::EventType::WindowClose:
            quitting_ = true;
            break;
        case platform::EventType::WindowResized:
            TY_LOG_INFO("window", "resized to %dx%d points, %dx%d pixels", event.width, event.height,
                        event.pixel_width, event.pixel_height);
            break;
        default:
            break;
        }
    }
    viewport_requested_ = false;
    ui_.begin_frame(*window_, input_, events_, dt_);
    frame_open_ = true;
    // What the frame's engine work allocated so far: the events. Counted
    // again at the end; the host's own code between is its own business.
    begin_allocations_ = core::engine_allocation_count() - begin_allocations_;
    begin_total_ = core::heap_allocation_count() - begin_total_;
    return true;
}

void Runtime::end_frame() {
    if (!frame_open_) {
        return;
    }
    const std::uint64_t engine_start = core::engine_allocation_count();
    const std::uint64_t total_start = core::heap_allocation_count();
    ui_.end_frame(*window_);
    frame_open_ = false;

    context_.time_seconds = game_time_;
    game_reloaded_ =
        game_ != nullptr && game_->poll(context_); // a reload allocates; that frame is exempt below
    if (game_reloaded_) {
        ++stats_.game_reloads;
    }
    models_reloaded_ = false;
    poll_models(); // so do these, and they are drawn this frame
    {
        TY_PROFILE_SCOPE_NAMED("systems");
        if (game_ != nullptr) {
            game_->update(context_, dt_);
        }
        // Physics runs at a fixed 60 Hz whatever the frame rate: the frame's
        // time accumulates into whole steps, and what is drawn is blended
        // between the last two states.
        stepper_.advance(*physics_, dt_, [&](float step) {
            scene::record_previous_poses(*world_, *physics_);
            if (before_step_ != nullptr) {
                before_step_(step, before_step_user_);
            }
        });
        scene::update_bodies(*world_, *physics_, stepper_.alpha());
        scene::update_transforms(*world_);
    }

    if (device_ != nullptr) {
        TY_PROFILE_SCOPE_NAMED("render");
        render_frame();
    } else {
        // Nothing to draw and nothing to wait on: pace the loop by hand.
        platform::sleep_ns(4'000'000);
    }

    // The rule from the roadmap, enforced: after warm-up, engine code
    // allocates nothing on the heap during a frame. The GPU driver and the
    // OS allocate plenty inside the calls we make; that is reported, not judged.
    const std::uint64_t engine_allocations =
        core::engine_allocation_count() - engine_start + begin_allocations_;
    const std::uint64_t total = core::heap_allocation_count() - total_start + begin_total_;
    const std::uint64_t external_allocations = total - engine_allocations;
    TY_PROFILE_PLOT("engine heap allocations / frame", static_cast<std::int64_t>(engine_allocations));
    TY_PROFILE_PLOT("external heap allocations / frame", static_cast<std::int64_t>(external_allocations));
    TY_PROFILE_PLOT("frame arena bytes", static_cast<std::int64_t>(frame_arena_->used()));
    stats_.heap_allocations = engine_allocations;
    if (frame_index_ == 60) {
        TY_LOG_INFO("heap", "per frame: engine %llu, external (driver, OS, Jolt) %llu",
                    static_cast<unsigned long long>(engine_allocations),
                    static_cast<unsigned long long>(external_allocations));
    }
    if (frame_index_ >= 10 && engine_allocations > 0 && !game_reloaded_ && !models_reloaded_) {
        TY_LOG_ERROR("heap", "frame %ld: engine code made %llu heap allocation(s)", frame_index_,
                     static_cast<unsigned long long>(engine_allocations));
        TY_ASSERT(engine_allocations == 0, "a frame allocated on the heap from engine code");
    }

    TY_PROFILE_FRAME();
    const double now = platform::now_seconds();
    stats_.frame_ms = static_cast<float>((now - last_time_) * 1000.0);
    ++frame_index_;
    ++frames_since_report_;
    if (now - last_report_ >= 5.0) {
        const rhi::Device::GpuStats gpu = device_ != nullptr ? device_->gpu_stats() : rhi::Device::GpuStats{};
        TY_LOG_INFO("runtime", "%.0f frames/s, GPU %.2f ms a frame",
                    static_cast<double>(frames_since_report_) / (now - last_report_), gpu.frame_ms);
        last_report_ = now;
        frames_since_report_ = 0;
    }
}

bool Runtime::ensure_viewport(std::uint32_t width, std::uint32_t height) {
    if (device_ == nullptr) {
        return false;
    }
    width = std::clamp(width, 1u, 8192u);
    height = std::clamp(height, 1u, 8192u);
    if (viewport_ && viewport_width_ == width && viewport_height_ == height) {
        return true;
    }
    // The old texture may still be read by the frame in flight; the device
    // keeps it alive until that frame completes.
    device_->destroy_texture(viewport_);
    viewport_ =
        device_->create_texture({.format = device_->swapchain_format(),
                                 .width = width,
                                 .height = height,
                                 .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled});
    if (!viewport_) {
        TY_LOG_ERROR("gpu", "viewport texture %ux%u failed: %s", width, height, platform::last_error());
        viewport_width_ = viewport_height_ = 0;
        return false;
    }
    viewport_width_ = width;
    viewport_height_ = height;
    return true;
}

std::uint64_t Runtime::scene_texture(std::uint32_t width, std::uint32_t height) {
    if (!ensure_viewport(width, height)) {
        return 0;
    }
    viewport_requested_ = true;
    return ui::ImGuiLayer::texture_id(viewport_);
}

bool Runtime::read_scene_texture(std::uint8_t* rgba, std::size_t size) noexcept {
    if (device_ == nullptr || !viewport_ || rgba == nullptr ||
        size != std::size_t{viewport_width_} * viewport_height_ * 4) {
        return false;
    }
    if (!device_->download_texture(viewport_, rgba, static_cast<std::uint32_t>(size))) {
        TY_LOG_ERROR("gpu", "reading the viewport back failed: %s", platform::last_error());
        return false;
    }
    // The texture is in the swapchain's format, which is BGRA on every
    // backend so far; a PNG wants RGBA. Either way the bytes are sRGB: the
    // post stack encoded them, or the hardware did.
    const rhi::TextureFormat format = device_->swapchain_format();
    if (format == rhi::TextureFormat::Bgra8Unorm || format == rhi::TextureFormat::Bgra8Srgb) {
        for (std::size_t i = 0; i + 3 < size; i += 4) {
            std::swap(rgba[i], rgba[i + 2]);
        }
    }
    return true;
}

// The frame as a graph: the scene's passes into an HDR transient with a
// depth transient beside it, the post stack from there to the window or
// the viewport texture, and the UI over the swapchain.
void Runtime::render_frame() {
    // begin_frame() blocks for vsync, which is what paces the loop.
    std::optional<rhi::Frame> frame = device_->begin_frame();
    if (!frame.has_value()) {
        TY_LOG_ERROR("gpu", "frame failed: %s", platform::last_error());
        quitting_ = true;
        return;
    }
    // Something to draw to: the window's image, or offscreen the viewport
    // texture alone (no window, so no UI over it).
    const bool to_viewport = viewport_requested_ && static_cast<bool>(viewport_);
    const bool has_image = frame->has_swapchain_image();
    if (has_image || (desc_.offscreen && to_viewport)) {
        if (has_image && !reported_swapchain_) {
            TY_LOG_INFO("swap", "%ux%u pixels", frame->width(), frame->height());
            reported_swapchain_ = true;
        }
        render::FrameGraph& graph = *graph_;
        const std::uint32_t width = to_viewport ? viewport_width_ : frame->width();
        const std::uint32_t height = to_viewport ? viewport_height_ : frame->height();
        const float aspect = static_cast<float>(width) / static_cast<float>(height);

        graph.begin();
        render::GraphTexture swapchain;
        if (has_image) {
            swapchain = graph.import(
                "swapchain", frame->swapchain_texture(),
                {.format = device_->swapchain_format(), .width = frame->width(), .height = frame->height()});
        }
        render::GraphTexture hdr =
            graph.create("hdr", {.format = kHdrFormat, .width = width, .height = height});
        render::GraphTexture depth = graph.create(
            "depth", {.format = device_->preferred_depth_format(), .width = width, .height = height});

        // What the world says is there, into the frame's memory.
        const std::uint32_t max_draws = world_->entity_count();
        std::span<render::DrawItem> draws = frame_arena_->create_array<render::DrawItem>(max_draws);
        const std::uint32_t draw_count = scene::collect_draws(
            *world_, models_.data(), model_count(), draws.data(), static_cast<std::uint32_t>(draws.size()));

        // A data view goes to the screen as it is, untonemapped and without bloom.
        const bool lit = render::debug_view_is_lit(scene_settings.debug_view);
        const render::Tonemap tonemap = post_.settings.tonemap;
        const bool bloom = post_.settings.bloom;
        post_.settings.tonemap = lit ? tonemap : render::Tonemap::None;
        post_.settings.bloom = lit && bloom;
        render::SceneFrame scene_frame;
        scene_frame.width = width;
        scene_frame.height = height;
        scene_frame.camera = camera;
        scene_frame.jitter = post_.jitter(width, height);
        scene_frame.sky = sky;
        scene_frame.draws = draws.data();
        scene_frame.draw_count = std::min(draw_count, static_cast<std::uint32_t>(draws.size()));
        scene_frame.sun = sun;
        scene_frame.lights = lights_;
        scene_frame.light_count = light_count_;
        scene_frame.settings = scene_settings;
        scene_.add_passes(graph, hdr, depth, scene_frame);
        render::GraphTexture target = swapchain;
        if (to_viewport) {
            target = graph.import("viewport", viewport_,
                                  {.format = device_->swapchain_format(), .width = width, .height = height});
        }
        // The post stack's last pass produces the picture: the UI reads or
        // loads that version, which is what puts the UI pass after it.
        target = post_.add_passes(graph, hdr, depth, target,
                                  {.width = width,
                                   .height = height,
                                   .encode_srgb = !device_->swapchain_is_linear(),
                                   .view_projection = camera.view_projection(aspect),
                                   .view_projection_jittered =
                                       camera.view_projection(aspect, scene_frame.jitter)});
        post_.settings.tonemap = tonemap;
        post_.settings.bloom = bloom;
        if (!to_viewport) {
            swapchain = target;
        }
        // The UI over the top: over the picture, or over a cleared window
        // when the picture went to the viewport texture (which the UI shows).
        if (has_image && (to_viewport || (ui_.can_draw() && ui_.has_draw_data()))) {
            graph.add_pass(
                "ui",
                [&](render::PassBuilder& b) {
                    if (to_viewport) {
                        b.read(target);
                        swapchain = b.write_color(swapchain, rhi::LoadOp::Clear, {0.06f, 0.06f, 0.07f, 1.0f});
                    } else {
                        swapchain = b.write_color(swapchain, rhi::LoadOp::Load);
                    }
                },
                [this](rhi::RenderPass& pass, const render::PassResources&) { ui_.draw(pass); });
        }
        if (graph.compile()) {
            // Streams go up before any pass runs: copies on the frame's timeline, no wait.
            (void)scene_.stream(*frame);
            (void)ui_.upload(*frame);
            graph.execute(*frame);
            const std::uint32_t culled = graph.stats().culled;
            if (frame_index_ == 60 || (frame_index_ > 60 && culled != last_culled_)) {
                report_graph();
                last_culled_ = culled;
            }
        } else {
            TY_LOG_ERROR("graph", "%s", graph.error());
        }
    }
    frame->submit();
}

// What the graph made of the frame, once it has settled.
void Runtime::report_graph() noexcept {
    const render::FrameGraph::Stats& stats = graph_->stats();
    TY_LOG_INFO("graph",
                "%u passes (%u culled); %u transients in %u textures, %.1f MB in memory, %.1f MB memoryless, "
                "%.1f MB asked; %u attachments stored, %u discarded; %u never leave the tile",
                stats.passes, stats.culled, stats.transients_used, stats.physical_textures,
                static_cast<double>(stats.bytes_allocated) / 1048576.0,
                static_cast<double>(stats.bytes_memoryless) / 1048576.0,
                static_cast<double>(stats.bytes_requested) / 1048576.0, stats.attachments_stored,
                stats.attachments_discarded, stats.memoryless);
    const rhi::Device::GpuStats gpu = device_->gpu_stats();
    if (gpu.frame_ms > 0.0) {
        TY_LOG_INFO("gpu", "%.2f ms a frame on the GPU, %.1f MB allocated by the device", gpu.frame_ms,
                    static_cast<double>(gpu.allocated_bytes) / 1048576.0);
    }
    char text[4096];
    graph_->describe(text, sizeof text);
    for (char* line = text; *line != '\0';) {
        char* end = std::strchr(line, '\n');
        if (end == nullptr) {
            break;
        }
        *end = '\0';
        TY_LOG_INFO("graph", "  %s", line);
        line = end + 1;
    }
}

std::uint32_t Runtime::add_model(const render::ModelData& data) {
    render::Model model;
    if (device_ != nullptr && fallbacks_.white) {
        if (!render::upload_model(*device_, data, fallbacks_, model)) {
            TY_LOG_ERROR("gpu", "model upload failed: %s", platform::last_error());
            render::destroy_model(*device_, model);
            model = render::Model{};
        }
    } else {
        // No GPU: nothing to draw with, but the shape is still known — what
        // picking and bounds need, headless or not.
        model.mesh.index_count = static_cast<std::uint32_t>(data.mesh.indices.size());
        model.mesh.bounds_min = data.mesh.bounds_min;
        model.mesh.bounds_max = data.mesh.bounds_max;
    }
    models_.push_back(std::move(model));
    return static_cast<std::uint32_t>(models_.size() - 1);
}

std::uint32_t Runtime::load_model(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        TY_LOG_ERROR("model", "no path to load");
        return kNoModel;
    }
    ModelFile file;
    file.index = static_cast<std::uint32_t>(models_.size());
    if (cooker::is_cooked_model(path)) {
        file.cooked = path;
    } else if (cooker::is_model_source(path)) {
        file.source = path;
    } else {
        TY_LOG_ERROR("model", "%s: not a model (.gltf, .glb) nor a cooked one (%s)", path,
                     assets::kModelBlobExtension);
        return kNoModel;
    }
    render::Model model;
    if (!load_model_file(file, model)) {
        return kNoModel;
    }
    models_.push_back(std::move(model));
    // Watched from now: the file as it is at this moment is the baseline.
    if (!file.source.empty()) {
        file.source_watch = model_watch_.watch(file.source.c_str());
    }
    file.cooked_watch = model_watch_.watch(file.cooked.c_str());
    model_files_.push_back(std::move(file));
    return model_files_.back().index;
}

// Cooks the source when the blob is missing or older, loads the blob, and
// uploads it — into `out`, leaving the model already in its slot alone
// until this has succeeded, so a broken save keeps the last good one.
bool Runtime::load_model_file(ModelFile& file, render::Model& out) {
    TY_PROFILE_SCOPE_NAMED("sdk::load_model_file");
    const double start = platform::now_seconds();
    const char* path = !file.source.empty() ? file.source.c_str() : file.cooked.c_str();
    std::string error;
    bool cooked_now = false;
    if (!file.source.empty() &&
        !cooker::ensure_cooked(file.source.c_str(), desc_.cook_dir, {.jobs = jobs_.get()}, file.cooked, error,
                               &cooked_now)) {
        TY_LOG_ERROR("model", "%s: %s", path, error.c_str());
        return false;
    }
    const double cooked_at = platform::now_seconds();
    assets::CookedModel cooked;
    if (!assets::load_model_blob_file(file.cooked.c_str(), cooked, error)) {
        TY_LOG_ERROR("model", "%s: %s", file.cooked.c_str(), error.c_str());
        return false;
    }
    out = render::Model{};
    if (device_ != nullptr && fallbacks_.white) {
        if (!assets::upload_cooked_model(*device_, cooked, fallbacks_, out)) {
            TY_LOG_ERROR("model", "%s: upload failed: %s", path, platform::last_error());
            render::destroy_model(*device_, out);
            out = render::Model{};
            return false;
        }
    } else {
        // No GPU: nothing to draw with, but the shape is still known — what
        // picking and bounds need, headless or not.
        out.mesh.index_count = static_cast<std::uint32_t>(cooked.mesh.indices.size());
        out.mesh.bounds_min = cooked.mesh.bounds_min;
        out.mesh.bounds_max = cooked.mesh.bounds_max;
    }
    const Vec3 size = cooked.mesh.bounds_max - cooked.mesh.bounds_min;
    const double now = platform::now_seconds();
    TY_LOG_INFO("model",
                "%s: %zu vertices, %zu triangles, %zu submeshes, %zu materials, %zu images, "
                "%.3f x %.3f x %.3f m; %s in %.2f s",
                path, cooked.mesh.vertices.size(), cooked.mesh.indices.size() / 3,
                cooked.mesh.submeshes.size(), cooked.materials.size(), cooked.images.size(),
                static_cast<double>(size.x), static_cast<double>(size.y), static_cast<double>(size.z),
                cooked_now ? "cooked and loaded" : "loaded from the blob", now - start);
    if (cooked_now) {
        TY_LOG_DEBUG("model", "%s: cooked to %s in %.2f s", path, file.cooked.c_str(), cooked_at - start);
    }
    return true;
}

void Runtime::poll_models() {
    assets::FileWatch::Id changed[16];
    const std::uint32_t count = model_watch_.poll(platform::now_seconds(), changed, 16);
    for (std::uint32_t c = 0; c < count; ++c) {
        for (ModelFile& file : model_files_) {
            if (file.source_watch != changed[c] && file.cooked_watch != changed[c]) {
                continue;
            }
            models_reloaded_ = true; // allocates: this frame is exempt from the heap rule
            render::Model fresh;
            if (load_model_file(file, fresh)) {
                if (device_ != nullptr) {
                    render::destroy_model(*device_, models_[file.index]);
                }
                models_[file.index] = std::move(fresh);
                ++stats_.model_reloads;
                TY_LOG_INFO("model", "reloaded %s into model %u",
                            !file.source.empty() ? file.source.c_str() : file.cooked.c_str(), file.index);
            }
            // What was just cooked is not news; what changed underneath, once, is done.
            model_watch_.acknowledge(file.cooked_watch);
            break;
        }
    }
}

void Runtime::set_lights(const render::PointLight* lights, std::uint32_t count) noexcept {
    light_count_ = std::min(count, render::kMaxSceneLights);
    if (light_count_ > 0 && lights != nullptr) {
        std::memcpy(lights_, lights, light_count_ * sizeof(render::PointLight));
    } else {
        light_count_ = 0;
    }
}

void Runtime::clear_scene() {
    std::vector<scene::Entity> entities;
    world_->each_chunk_raw(
        nullptr, 0,
        [](void* user, const scene::ChunkView& view) {
            auto* out = static_cast<std::vector<scene::Entity>*>(user);
            out->insert(out->end(), view.entities, view.entities + view.count);
        },
        &entities);
    for (const scene::Entity entity : entities) {
        (void)world_->destroy(entity);
    }
}

scene::Entity Runtime::pick(const Vec3& origin, const Vec3& direction, float& distance) const noexcept {
    // Every drawable's bounds are in its model's space: the ray goes there
    // through the inverse of the entity's world matrix, and t means the
    // same distance on either side of it.
    scene::Entity nearest;
    float nearest_t = 3.402823466e+38f;
    world_->each<scene::LocalToWorld, scene::MeshRenderer>(
        [&](scene::Entity entity, scene::LocalToWorld& local_to_world, scene::MeshRenderer& renderer) {
            const render::Model* m = model(renderer.model);
            if (!renderer.visible || m == nullptr || m->mesh.index_count == 0) {
                return;
            }
            const Mat4 to_local = inverse(local_to_world.matrix);
            const math::Aabb bounds{m->mesh.bounds_min, m->mesh.bounds_max};
            float t = 0.0f;
            const Vec3 local_origin = transform_point(to_local, origin);
            const Vec3 local_direction = transform_vector(to_local, direction);
            if (bounds.intersects_ray(local_origin, local_direction, nearest_t, t) && t < nearest_t) {
                nearest_t = t;
                nearest = entity;
            }
        });
    distance = nearest ? nearest_t : 0.0f;
    return nearest;
}

platform::InputLog Runtime::recording() const {
    platform::InputLog log = log_record_;
    if (physics_ != nullptr) {
        log.backend = physics_->backend_name();
        log.steps = stepper_.total_steps;
        log.state_hash = physics_->state_hash();
        log.has_end = true;
    }
    return log;
}

std::uint64_t Runtime::log_count() const noexcept {
    if (log_ == nullptr) {
        return 0;
    }
    const std::lock_guard<std::mutex> lock(log_->mutex);
    return log_->count;
}

bool Runtime::log_get(std::uint64_t index, tynima_log_entry& out) const noexcept {
    return log_ != nullptr && log_->get(index, out);
}

} // namespace tynima::sdk
