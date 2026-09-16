#pragma once

#include <tynima/core/inline_function.h>
#include <tynima/rhi/device.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The frame graph: a frame's rendering declared as passes that read and
// write named textures, compiled into an order, a set of physical textures
// and the load/store action for every attachment — then executed against
// the RHI. Rebuilt every frame (declaring is cheap; nothing here allocates
// after construction), in the shape Frostbite described at GDC 2017.
//
//   graph.begin();
//   GraphTexture swapchain = graph.import("swapchain", frame.swapchain_texture(), {...});
//   GraphTexture hdr = graph.create("hdr", {.format = Rgba16Float, .width = w, .height = h});
//   graph.add_pass("scene",
//       [&](PassBuilder& b) { hdr = b.write_color(hdr, LoadOp::Clear, sky); },
//       [&](rhi::RenderPass& pass, const PassResources&) { ...draw... });
//   graph.add_pass("tonemap",
//       [&](PassBuilder& b) { b.read(hdr); swapchain = b.write_color(swapchain, LoadOp::DontCare); },
//       [&](rhi::RenderPass& pass, const PassResources& r) { ...sample r.texture(hdr)... });
//   if (graph.compile()) graph.execute(frame);
//
// What compiling decides:
// - Order: a pass runs after whatever wrote what it reads (and after whatever
//   read what it overwrites), otherwise in the order it was added.
// - Culling: a pass whose results nobody consumes — not read, not loaded by a
//   later write, not an imported texture, not declared a side effect — is not
//   run, nor are the passes that only fed it.
// - Transients: a texture created by the graph exists only between its first
//   and last use, so two whose lives do not overlap share one physical
//   texture; physical textures persist across frames and are dropped when
//   unused for a while.
// - Load/store: a write that nothing reads afterwards is stored DontCare —
//   which is what lets a tile-based GPU keep it on-chip and never write it
//   out. A transient never stored and never sampled is noted as one that
//   could live entirely in tile memory (memoryless on Metal; Phase 4's
//   native backend acts on that, SDL GPU cannot).
//
// Buffers go through the graph too, as imported resources (a light list the
// frame streams in, a cluster table a compute pass fills): a compute pass
// writes them, any pass reads them, and the versions order the passes the
// same way. The graph does not allocate buffers of its own yet.
//
// Barriers: on SDL GPU, ordering passes is what synchronises them (the
// backend tracks hazards within a command buffer). The graph knows every
// edge; a backend that needs explicit barriers reads them off the same plan.
namespace tynima::render {

// A texture as the graph names it: an index, and a version that every write
// advances. A read names the version it wants, so the graph knows exactly
// which pass produced what another consumes.
struct GraphTexture {
    static constexpr std::uint16_t kNone = 0xFFFF;
    std::uint16_t index = kNone;
    std::uint16_t version = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return index != kNone; }
};

struct TextureInfo {
    rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// A buffer as the graph names it, versioned like a texture.
struct GraphBuffer {
    static constexpr std::uint16_t kNone = 0xFFFF;
    std::uint16_t index = kNone;
    std::uint16_t version = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return index != kNone; }
};

class FrameGraph;

// What a pass's execute callback gets: the physical resource behind a handle.
class PassResources {
public:
    [[nodiscard]] rhi::TextureHandle texture(GraphTexture texture) const noexcept;
    [[nodiscard]] rhi::BufferHandle buffer(GraphBuffer buffer) const noexcept;

private:
    friend class FrameGraph;
    explicit PassResources(const FrameGraph& graph) noexcept : graph_(graph) {}
    const FrameGraph& graph_;
};

// Runs inside the pass; captures by reference or plain value only, up to
// a couple of dozen of them.
using PassExecute = core::InlineFunction<void(rhi::RenderPass&, const PassResources&), 192>;
using ComputeExecute = core::InlineFunction<void(rhi::ComputePass&, const PassResources&), 128>;

// Given to a pass's setup callback to declare what the pass touches. Color
// attachments take the slots in the order they are declared, and so do a
// compute pass's written buffers.
class PassBuilder {
public:
    // Sampled by this pass's shaders.
    GraphTexture read(GraphTexture texture) noexcept;
    // A storage buffer this pass's shaders read.
    GraphBuffer read_buffer(GraphBuffer buffer) noexcept;
    // A storage buffer this compute pass writes, in place. Returns the new
    // version, which is what later passes read.
    GraphBuffer write_buffer(GraphBuffer buffer) noexcept;
    // Drawn into by this pass. Returns the new version, which is what later
    // passes read. Load keeps the previous version's contents, so it can
    // only follow a write.
    GraphTexture write_color(GraphTexture texture, rhi::LoadOp load = rhi::LoadOp::Clear,
                             const rhi::ClearColor& clear = {}) noexcept;
    GraphTexture write_depth(GraphTexture texture, rhi::LoadOp load = rhi::LoadOp::Clear,
                             float clear = 0.0f) noexcept;
    // Never cull this pass, whatever becomes of its writes (a readback, a
    // debug overlay).
    void side_effect() noexcept;

private:
    friend class FrameGraph;
    PassBuilder(FrameGraph& graph, std::uint32_t pass) noexcept : graph_(graph), pass_(pass) {}
    FrameGraph& graph_;
    std::uint32_t pass_;
};

class FrameGraph {
public:
    static constexpr std::uint32_t kMaxReads = 8;
    static constexpr std::uint32_t kMaxBufferWrites = rhi::kMaxComputeWrites;
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    // Budgets, fixed at construction: declaring past them fails compile().
    struct Limits {
        std::uint32_t max_passes = 32; // at most 64
        std::uint32_t max_textures = 64;
        std::uint32_t max_buffers = 32;
        std::uint32_t max_physical = 32; // pooled transient textures
        std::uint32_t idle_frames_before_free = 120;
    };

    // What one pass does to one attachment.
    struct Attachment {
        GraphTexture texture; // the version written over; the pass produces the next
        rhi::LoadOp load = rhi::LoadOp::Clear;
        rhi::StoreOp store = rhi::StoreOp::Store; // decided by compile()
        rhi::ClearColor clear{};
        float depth_clear = 0.0f;
    };

    struct Pass {
        const char* name = "";
        bool compute = false;
        Attachment colors[rhi::kMaxColorTargets]{};
        std::uint32_t color_count = 0;
        Attachment depth{};
        bool has_depth = false;
        GraphTexture reads[kMaxReads]{};
        std::uint32_t read_count = 0;
        GraphBuffer buffer_reads[kMaxReads]{};
        std::uint32_t buffer_read_count = 0;
        GraphBuffer buffer_writes[kMaxBufferWrites]{}; // the version written over; the pass produces the next
        std::uint32_t buffer_write_count = 0;
        bool side_effect = false;
        PassExecute execute;
        ComputeExecute compute_execute;
        // Decided by compile():
        bool culled = false;
        std::uint32_t order = kNone;     // position in the execution order
        std::uint64_t depends_on = 0;    // bit per pass that must run first
    };

    struct Texture {
        const char* name = "";
        TextureInfo info{};
        bool imported = false;
        rhi::TextureHandle imported_handle;
        std::uint16_t versions = 0; // writes declared so far; the latest version
        // Decided by compile():
        bool used = false; // touched by a pass that runs
        rhi::TextureUsage usage = rhi::TextureUsage::Default;
        std::uint32_t first_use = kNone; // in execution order
        std::uint32_t last_use = kNone;
        std::uint32_t physical = kNone; // transients: the pooled texture behind it
        bool memoryless = false;        // never leaves the pass that draws it
    };

    struct Buffer {
        const char* name = "";
        rhi::BufferHandle handle;
        std::uint16_t versions = 0;
        bool used = false; // decided by compile(): touched by a pass that runs
    };

    struct Stats {
        std::uint32_t passes = 0;
        std::uint32_t culled = 0;
        std::uint32_t transients = 0;
        std::uint32_t transients_used = 0;
        std::uint32_t physical_textures = 0; // in use this frame
        std::uint64_t bytes_requested = 0;   // every used transient at full size
        std::uint64_t bytes_allocated = 0;   // what the physical textures take
        std::uint32_t attachments_stored = 0;
        std::uint32_t attachments_discarded = 0;
        std::uint32_t memoryless = 0;
    };

    // `device` may be null for a graph that is only compiled (tests); execute()
    // needs one.
    explicit FrameGraph(rhi::Device* device);
    FrameGraph(rhi::Device* device, const Limits& limits);
    ~FrameGraph();
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    // Starts a frame's graph; what was declared last frame is forgotten.
    void begin() noexcept;
    // A texture owned elsewhere (the swapchain image, a shadow atlas kept
    // across frames): whatever is written to it is kept, so its writers run.
    GraphTexture import(const char* name, rhi::TextureHandle texture, const TextureInfo& info) noexcept;
    // A texture the graph provides for this frame only.
    GraphTexture create(const char* name, const TextureInfo& info) noexcept;
    // A buffer owned elsewhere that passes read, or compute passes write.
    GraphBuffer import_buffer(const char* name, rhi::BufferHandle buffer) noexcept;
    // `setup(PassBuilder&)` declares; `execute(rhi::RenderPass&, const PassResources&)` draws.
    template <typename Setup, typename Execute>
    void add_pass(const char* name, Setup&& setup, Execute&& execute) noexcept {
        const std::uint32_t index = open_pass(name, false);
        if (index == kNone) {
            return;
        }
        PassBuilder builder(*this, index);
        setup(builder);
        passes_[index].execute.assign(std::forward<Execute>(execute));
    }
    // A compute pass: it reads and writes buffers, and
    // `execute(rhi::ComputePass&, const PassResources&)` dispatches.
    template <typename Setup, typename Execute>
    void add_compute_pass(const char* name, Setup&& setup, Execute&& execute) noexcept {
        const std::uint32_t index = open_pass(name, true);
        if (index == kNone) {
            return;
        }
        PassBuilder builder(*this, index);
        setup(builder);
        passes_[index].compute_execute.assign(std::forward<Execute>(execute));
    }

    // Decides everything above. false when the graph is malformed (a read of
    // something never written, a cycle, a budget exceeded): error() says.
    [[nodiscard]] bool compile() noexcept;
    // Runs the compiled passes in order on this frame's command buffer.
    void execute(rhi::Frame& frame) noexcept;
    [[nodiscard]] const char* error() const noexcept { return error_; }

    // The plan, for tests and the log.
    [[nodiscard]] std::uint32_t pass_count() const noexcept {
        return static_cast<std::uint32_t>(passes_.size());
    }
    [[nodiscard]] const Pass& pass(std::uint32_t index) const noexcept { return passes_[index]; }
    [[nodiscard]] std::uint32_t texture_count() const noexcept {
        return static_cast<std::uint32_t>(textures_.size());
    }
    [[nodiscard]] const Texture& texture(std::uint32_t index) const noexcept { return textures_[index]; }
    [[nodiscard]] std::uint32_t buffer_count() const noexcept {
        return static_cast<std::uint32_t>(buffers_.size());
    }
    [[nodiscard]] const Buffer& buffer(std::uint32_t index) const noexcept { return buffers_[index]; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    // One line per pass in execution order, culled ones after — e.g.
    // "scene: writes hdr (clear, store), depth (clear, discard)". Returns the
    // length it needed; the text is truncated to fit.
    std::size_t describe(char* out, std::size_t capacity) const noexcept;

private:
    friend class PassBuilder;
    friend class PassResources;

    struct Physical {
        rhi::TextureHandle handle;
        TextureInfo info{};
        rhi::TextureUsage usage = rhi::TextureUsage::Default;
        bool used_this_frame = false;
        std::uint32_t last_use = kNone; // in this frame's execution order
        std::uint32_t idle_frames = 0;
    };

    std::uint32_t open_pass(const char* name, bool compute) noexcept;
    void fail(const char* message) noexcept;
    [[nodiscard]] std::uint32_t writer_of(GraphTexture texture) const noexcept; // the pass that produced it
    [[nodiscard]] std::uint32_t writer_of(GraphBuffer buffer) const noexcept;
    [[nodiscard]] bool validate() noexcept;
    [[nodiscard]] bool order_passes() noexcept;
    void cull() noexcept;
    void decide_lifetimes() noexcept;
    void decide_stores() noexcept;
    void assign_physical() noexcept;
    [[nodiscard]] rhi::TextureHandle resolve(GraphTexture texture) const noexcept;
    [[nodiscard]] rhi::BufferHandle resolve(GraphBuffer buffer) const noexcept;

    rhi::Device* device_;
    Limits limits_;
    std::vector<Pass> passes_;
    std::vector<Texture> textures_;
    std::vector<Buffer> buffers_;
    std::vector<Physical> physicals_;
    std::vector<std::uint32_t> order_;   // pass indices in execution order
    std::vector<std::uint32_t> scratch_; // transient indices sorted by first use
    Stats stats_{};
    const char* error_ = "";
    bool compiled_ = false;
};

} // namespace tynima::render
