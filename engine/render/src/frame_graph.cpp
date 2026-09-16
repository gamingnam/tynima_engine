#include <tynima/render/frame_graph.h>

#include <tynima/core/assert.h>
#include <tynima/core/log.h>
#include <tynima/core/profile.h>
#include <tynima/platform/platform.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tynima::render {

namespace {

constexpr std::uint64_t bit(std::uint32_t index) noexcept {
    return std::uint64_t{1} << index;
}

std::uint64_t texture_bytes(const TextureInfo& info) noexcept {
    return std::uint64_t{info.width} * info.height * rhi::bytes_per_pixel(info.format);
}

const char* load_name(rhi::LoadOp op) noexcept {
    switch (op) {
    case rhi::LoadOp::Load: return "load";
    case rhi::LoadOp::Clear: return "clear";
    case rhi::LoadOp::DontCare: return "dontcare";
    }
    return "?";
}

const char* store_name(rhi::StoreOp op) noexcept {
    return op == rhi::StoreOp::Store ? "store" : "discard";
}

// snprintf into a bounded buffer, advancing; keeps counting past the end so
// the caller learns the full length.
struct Writer {
    char* out;
    std::size_t capacity;
    std::size_t length = 0;

    void put(const char* text) noexcept {
        const std::size_t n = std::strlen(text);
        if (length < capacity) {
            const std::size_t room = capacity - length - 1;
            std::memcpy(out + length, text, n < room ? n : room);
        }
        length += n;
    }
    void finish() noexcept {
        if (capacity > 0) {
            out[length < capacity ? length : capacity - 1] = '\0';
        }
    }
};

} // namespace

// ------------------------------------------------------------ PassResources

rhi::TextureHandle PassResources::texture(GraphTexture texture) const noexcept {
    return graph_.resolve(texture);
}

rhi::BufferHandle PassResources::buffer(GraphBuffer buffer) const noexcept {
    return graph_.resolve(buffer);
}

// -------------------------------------------------------------- PassBuilder

GraphTexture PassBuilder::read(GraphTexture texture) noexcept {
    FrameGraph::Pass& pass = graph_.passes_[pass_];
    if (!texture || texture.index >= graph_.textures_.size()) {
        graph_.fail("a pass reads a texture the graph does not know");
        return texture;
    }
    if (pass.read_count == FrameGraph::kMaxReads) {
        graph_.fail("a pass reads more textures than the graph allows");
        return texture;
    }
    pass.reads[pass.read_count++] = texture;
    return texture;
}

GraphTexture PassBuilder::write_color(GraphTexture texture, rhi::LoadOp load,
                                      const rhi::ClearColor& clear) noexcept {
    FrameGraph::Pass& pass = graph_.passes_[pass_];
    if (!texture || texture.index >= graph_.textures_.size()) {
        graph_.fail("a pass writes a texture the graph does not know");
        return texture;
    }
    FrameGraph::Texture& node = graph_.textures_[texture.index];
    if (texture.version != node.versions) {
        graph_.fail("a pass writes an old version of a texture: write the one the last writer returned");
        return texture;
    }
    if (pass.color_count == rhi::kMaxColorTargets) {
        graph_.fail("a pass writes more color attachments than the RHI allows");
        return texture;
    }
    pass.colors[pass.color_count++] =
        FrameGraph::Attachment{.texture = texture, .load = load, .clear = clear};
    ++node.versions;
    return GraphTexture{texture.index, node.versions};
}

GraphTexture PassBuilder::write_depth(GraphTexture texture, rhi::LoadOp load, float clear) noexcept {
    FrameGraph::Pass& pass = graph_.passes_[pass_];
    if (!texture || texture.index >= graph_.textures_.size()) {
        graph_.fail("a pass writes a texture the graph does not know");
        return texture;
    }
    FrameGraph::Texture& node = graph_.textures_[texture.index];
    if (texture.version != node.versions) {
        graph_.fail("a pass writes an old version of a texture: write the one the last writer returned");
        return texture;
    }
    if (pass.has_depth) {
        graph_.fail("a pass writes two depth attachments");
        return texture;
    }
    pass.depth = FrameGraph::Attachment{.texture = texture, .load = load, .depth_clear = clear};
    pass.has_depth = true;
    ++node.versions;
    return GraphTexture{texture.index, node.versions};
}

GraphBuffer PassBuilder::read_buffer(GraphBuffer buffer) noexcept {
    FrameGraph::Pass& pass = graph_.passes_[pass_];
    if (!buffer || buffer.index >= graph_.buffers_.size()) {
        graph_.fail("a pass reads a buffer the graph does not know");
        return buffer;
    }
    if (pass.buffer_read_count == FrameGraph::kMaxReads) {
        graph_.fail("a pass reads more buffers than the graph allows");
        return buffer;
    }
    pass.buffer_reads[pass.buffer_read_count++] = buffer;
    return buffer;
}

GraphBuffer PassBuilder::write_buffer(GraphBuffer buffer) noexcept {
    FrameGraph::Pass& pass = graph_.passes_[pass_];
    if (!buffer || buffer.index >= graph_.buffers_.size()) {
        graph_.fail("a pass writes a buffer the graph does not know");
        return buffer;
    }
    FrameGraph::Buffer& node = graph_.buffers_[buffer.index];
    if (buffer.version != node.versions) {
        graph_.fail("a pass writes an old version of a buffer: write the one the last writer returned");
        return buffer;
    }
    if (pass.buffer_write_count == FrameGraph::kMaxBufferWrites) {
        graph_.fail("a pass writes more buffers than the RHI allows");
        return buffer;
    }
    pass.buffer_writes[pass.buffer_write_count++] = buffer;
    ++node.versions;
    return GraphBuffer{buffer.index, node.versions};
}

void PassBuilder::side_effect() noexcept {
    graph_.passes_[pass_].side_effect = true;
}

// --------------------------------------------------------------- FrameGraph

FrameGraph::FrameGraph(rhi::Device* device) : FrameGraph(device, Limits{}) {}

FrameGraph::FrameGraph(rhi::Device* device, const Limits& limits) : device_(device), limits_(limits) {
    limits_.max_passes = std::min<std::uint32_t>(limits_.max_passes, 64); // dependencies are one bit per pass
    passes_.reserve(limits_.max_passes);
    textures_.reserve(limits_.max_textures);
    buffers_.reserve(limits_.max_buffers);
    physicals_.reserve(limits_.max_physical);
    order_.reserve(limits_.max_passes);
    scratch_.reserve(limits_.max_textures);
}

FrameGraph::~FrameGraph() {
    if (device_ != nullptr) {
        for (const Physical& physical : physicals_) {
            device_->destroy_texture(physical.handle);
        }
    }
}

void FrameGraph::begin() noexcept {
    passes_.clear();
    textures_.clear();
    buffers_.clear();
    order_.clear();
    scratch_.clear();
    stats_ = Stats{};
    error_ = "";
    compiled_ = false;
    for (Physical& physical : physicals_) {
        physical.used_this_frame = false;
        physical.last_use = kNone;
    }
}

GraphTexture FrameGraph::import(const char* name, rhi::TextureHandle texture,
                                const TextureInfo& info) noexcept {
    if (textures_.size() == textures_.capacity()) {
        fail("more textures than the graph allows");
        return {};
    }
    if (!texture) {
        fail("an imported texture handle is null");
        return {};
    }
    textures_.push_back(Texture{.name = name, .info = info, .imported = true, .imported_handle = texture});
    return GraphTexture{static_cast<std::uint16_t>(textures_.size() - 1), 0};
}

GraphTexture FrameGraph::create(const char* name, const TextureInfo& info) noexcept {
    if (textures_.size() == textures_.capacity()) {
        fail("more textures than the graph allows");
        return {};
    }
    if (info.width == 0 || info.height == 0) {
        fail("a transient texture has no size");
        return {};
    }
    textures_.push_back(Texture{.name = name, .info = info});
    ++stats_.transients;
    return GraphTexture{static_cast<std::uint16_t>(textures_.size() - 1), 0};
}

GraphBuffer FrameGraph::import_buffer(const char* name, rhi::BufferHandle buffer) noexcept {
    if (buffers_.size() == buffers_.capacity()) {
        fail("more buffers than the graph allows");
        return {};
    }
    if (!buffer) {
        fail("an imported buffer handle is null");
        return {};
    }
    buffers_.push_back(Buffer{.name = name, .handle = buffer});
    return GraphBuffer{static_cast<std::uint16_t>(buffers_.size() - 1), 0};
}

std::uint32_t FrameGraph::open_pass(const char* name, bool compute) noexcept {
    if (passes_.size() == passes_.capacity()) {
        fail("more passes than the graph allows");
        return kNone;
    }
    passes_.push_back(Pass{.name = name, .compute = compute});
    return static_cast<std::uint32_t>(passes_.size() - 1);
}

void FrameGraph::fail(const char* message) noexcept {
    if (error_[0] == '\0') {
        error_ = message; // the first problem is the one worth reading
    }
}

std::uint32_t FrameGraph::writer_of(GraphTexture texture) const noexcept {
    if (texture.version == 0) {
        return kNone; // the imported contents, or nothing at all
    }
    for (std::uint32_t p = 0; p < passes_.size(); ++p) {
        const Pass& pass = passes_[p];
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            if (pass.colors[i].texture.index == texture.index &&
                pass.colors[i].texture.version + 1 == texture.version) {
                return p;
            }
        }
        if (pass.has_depth && pass.depth.texture.index == texture.index &&
            pass.depth.texture.version + 1 == texture.version) {
            return p;
        }
    }
    return kNone;
}

std::uint32_t FrameGraph::writer_of(GraphBuffer buffer) const noexcept {
    if (buffer.version == 0) {
        return kNone; // the imported contents
    }
    for (std::uint32_t p = 0; p < passes_.size(); ++p) {
        const Pass& pass = passes_[p];
        for (std::uint32_t i = 0; i < pass.buffer_write_count; ++i) {
            const GraphBuffer& written = pass.buffer_writes[i];
            if (written.index == buffer.index && written.version + 1 == buffer.version) {
                return p;
            }
        }
    }
    return kNone;
}

bool FrameGraph::validate() noexcept {
    for (const Pass& pass : passes_) {
        if (pass.compute) {
            if (pass.color_count > 0 || pass.has_depth || pass.read_count > 0) {
                fail("a compute pass reads and writes buffers, not textures");
                return false;
            }
            if (pass.buffer_write_count == 0 && !pass.side_effect) {
                fail("a compute pass writes nothing: it needs a buffer to write, or a side effect");
                return false;
            }
        } else {
            if (pass.buffer_write_count > 0) {
                fail("a render pass cannot write a buffer: that takes a compute pass");
                return false;
            }
            if (pass.color_count == 0 && !pass.has_depth) {
                fail("a pass writes nothing: every pass needs an attachment");
                return false;
            }
        }
        for (std::uint32_t r = 0; r < pass.buffer_read_count; ++r) {
            for (std::uint32_t w = 0; w < pass.buffer_write_count; ++w) {
                if (pass.buffer_reads[r].index == pass.buffer_writes[w].index) {
                    fail("a pass reads a buffer it also writes");
                    return false;
                }
            }
        }
        // Attachments: the right kind of format, one size for all of them,
        // and contents to load when loading.
        std::uint32_t width = 0, height = 0;
        const auto check = [&](const Attachment& a, bool is_depth) {
            const Texture& node = textures_[a.texture.index];
            if (rhi::is_depth_format(node.info.format) != is_depth) {
                fail(is_depth ? "a depth attachment has a color format"
                              : "a color attachment has a depth format");
                return false;
            }
            if (width == 0) {
                width = node.info.width;
                height = node.info.height;
            } else if (width != node.info.width || height != node.info.height) {
                fail("a pass's attachments differ in size");
                return false;
            }
            if (a.load == rhi::LoadOp::Load && !node.imported && a.texture.version == 0) {
                fail("a pass loads a transient that nothing has written");
                return false;
            }
            return true;
        };
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            if (!check(pass.colors[i], false)) {
                return false;
            }
        }
        if (pass.has_depth && !check(pass.depth, true)) {
            return false;
        }
        for (std::uint32_t r = 0; r < pass.read_count; ++r) {
            const GraphTexture read = pass.reads[r];
            const Texture& node = textures_[read.index];
            if (!node.imported && read.version == 0) {
                fail("a pass reads a transient that nothing has written");
                return false;
            }
            for (std::uint32_t i = 0; i < pass.color_count; ++i) {
                if (pass.colors[i].texture.index == read.index) {
                    fail("a pass reads a texture it also writes");
                    return false;
                }
            }
            if (pass.has_depth && pass.depth.texture.index == read.index) {
                fail("a pass reads a texture it also writes");
                return false;
            }
        }
    }
    return true;
}

// A pass runs after the writer of every version it reads or overwrites, and
// after every reader of the version it overwrites. Then, in the order the
// passes were added, each one goes as soon as those before it have gone.
bool FrameGraph::order_passes() noexcept {
    const std::uint32_t count = static_cast<std::uint32_t>(passes_.size());
    for (std::uint32_t p = 0; p < count; ++p) {
        Pass& pass = passes_[p];
        pass.depends_on = 0;
        for (std::uint32_t r = 0; r < pass.read_count; ++r) {
            const std::uint32_t writer = writer_of(pass.reads[r]);
            if (writer != kNone) {
                pass.depends_on |= bit(writer);
            }
        }
        const auto overwrite = [&](const Attachment& a) {
            const std::uint32_t writer = writer_of(a.texture);
            if (writer != kNone) {
                pass.depends_on |= bit(writer);
            }
            for (std::uint32_t q = 0; q < count; ++q) {
                if (q == p) {
                    continue;
                }
                for (std::uint32_t r = 0; r < passes_[q].read_count; ++r) {
                    if (passes_[q].reads[r].index == a.texture.index &&
                        passes_[q].reads[r].version == a.texture.version) {
                        pass.depends_on |= bit(q);
                    }
                }
            }
        };
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            overwrite(pass.colors[i]);
        }
        if (pass.has_depth) {
            overwrite(pass.depth);
        }
        // Buffers: the same rules, versioned the same way.
        for (std::uint32_t r = 0; r < pass.buffer_read_count; ++r) {
            const std::uint32_t writer = writer_of(pass.buffer_reads[r]);
            if (writer != kNone) {
                pass.depends_on |= bit(writer);
            }
        }
        for (std::uint32_t w = 0; w < pass.buffer_write_count; ++w) {
            const GraphBuffer over = pass.buffer_writes[w];
            const std::uint32_t writer = writer_of(over);
            if (writer != kNone) {
                pass.depends_on |= bit(writer);
            }
            for (std::uint32_t q = 0; q < count; ++q) {
                if (q == p) {
                    continue;
                }
                for (std::uint32_t r = 0; r < passes_[q].buffer_read_count; ++r) {
                    if (passes_[q].buffer_reads[r].index == over.index &&
                        passes_[q].buffer_reads[r].version == over.version) {
                        pass.depends_on |= bit(q);
                    }
                }
            }
        }
        pass.order = kNone;
    }

    order_.clear();
    std::uint64_t scheduled = 0;
    while (order_.size() < count) {
        std::uint32_t next = kNone;
        for (std::uint32_t p = 0; p < count; ++p) {
            if ((scheduled & bit(p)) == 0 && (passes_[p].depends_on & ~scheduled) == 0) {
                next = p;
                break;
            }
        }
        if (next == kNone) {
            fail("the passes depend on each other in a cycle");
            return false;
        }
        passes_[next].order = static_cast<std::uint32_t>(order_.size());
        order_.push_back(next);
        scheduled |= bit(next);
    }
    return true;
}

// A pass is kept if it has a side effect or writes an imported texture;
// then, walking the order backwards, whatever a kept pass reads — or loads
// under its own write — keeps its writer. Everything else is dropped.
void FrameGraph::cull() noexcept {
    for (Pass& pass : passes_) {
        pass.culled = true;
        // A written buffer is always imported: its contents persist, so the
        // pass counts as a side effect.
        if (pass.side_effect || pass.buffer_write_count > 0) {
            pass.culled = false;
        }
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            if (textures_[pass.colors[i].texture.index].imported) {
                pass.culled = false;
            }
        }
        if (pass.has_depth && textures_[pass.depth.texture.index].imported) {
            pass.culled = false;
        }
    }
    for (std::uint32_t i = static_cast<std::uint32_t>(order_.size()); i-- > 0;) {
        Pass& pass = passes_[order_[i]];
        if (pass.culled) {
            continue;
        }
        const auto keep_writer = [&](GraphTexture texture) {
            const std::uint32_t writer = writer_of(texture);
            if (writer != kNone) {
                passes_[writer].culled = false;
            }
        };
        for (std::uint32_t r = 0; r < pass.read_count; ++r) {
            keep_writer(pass.reads[r]);
        }
        for (std::uint32_t c = 0; c < pass.color_count; ++c) {
            if (pass.colors[c].load == rhi::LoadOp::Load) {
                keep_writer(pass.colors[c].texture);
            }
        }
        if (pass.has_depth && pass.depth.load == rhi::LoadOp::Load) {
            keep_writer(pass.depth.texture);
        }
        for (std::uint32_t r = 0; r < pass.buffer_read_count; ++r) {
            const std::uint32_t writer = writer_of(pass.buffer_reads[r]);
            if (writer != kNone) {
                passes_[writer].culled = false;
            }
        }
    }
    stats_.passes = static_cast<std::uint32_t>(passes_.size());
    stats_.culled = 0;
    for (const Pass& pass : passes_) {
        stats_.culled += pass.culled ? 1 : 0;
    }
}

void FrameGraph::decide_lifetimes() noexcept {
    for (Texture& node : textures_) {
        node.used = false;
        node.usage = rhi::TextureUsage::Default;
        node.first_use = kNone;
        node.last_use = kNone;
        node.physical = kNone;
        node.memoryless = false;
    }
    const auto touch = [&](std::uint32_t index, std::uint32_t order, rhi::TextureUsage usage) {
        Texture& node = textures_[index];
        node.used = true;
        node.usage = node.usage | usage;
        node.first_use = node.first_use == kNone ? order : std::min(node.first_use, order);
        node.last_use = node.last_use == kNone ? order : std::max(node.last_use, order);
    };
    for (Buffer& node : buffers_) {
        node.used = false;
    }
    for (const Pass& pass : passes_) {
        if (pass.culled) {
            continue;
        }
        for (std::uint32_t r = 0; r < pass.read_count; ++r) {
            touch(pass.reads[r].index, pass.order, rhi::TextureUsage::Sampled);
        }
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            touch(pass.colors[i].texture.index, pass.order, rhi::TextureUsage::ColorTarget);
        }
        if (pass.has_depth) {
            touch(pass.depth.texture.index, pass.order, rhi::TextureUsage::DepthStencilTarget);
        }
        for (std::uint32_t r = 0; r < pass.buffer_read_count; ++r) {
            buffers_[pass.buffer_reads[r].index].used = true;
        }
        for (std::uint32_t w = 0; w < pass.buffer_write_count; ++w) {
            buffers_[pass.buffer_writes[w].index].used = true;
        }
    }
}

// A write is stored when something that runs will use the version it made:
// a read, a later write that loads it, or the outside world (an import).
void FrameGraph::decide_stores() noexcept {
    stats_.attachments_stored = 0;
    stats_.attachments_discarded = 0;
    const auto wanted = [&](GraphTexture produced) {
        if (textures_[produced.index].imported) {
            return true;
        }
        for (const Pass& other : passes_) {
            if (other.culled) {
                continue;
            }
            for (std::uint32_t r = 0; r < other.read_count; ++r) {
                if (other.reads[r].index == produced.index && other.reads[r].version == produced.version) {
                    return true;
                }
            }
            for (std::uint32_t i = 0; i < other.color_count; ++i) {
                const Attachment& a = other.colors[i];
                if (a.texture.index == produced.index && a.texture.version == produced.version &&
                    a.load == rhi::LoadOp::Load) {
                    return true;
                }
            }
            if (other.has_depth && other.depth.texture.index == produced.index &&
                other.depth.texture.version == produced.version && other.depth.load == rhi::LoadOp::Load) {
                return true;
            }
        }
        return false;
    };
    for (Pass& pass : passes_) {
        if (pass.culled) {
            continue;
        }
        const auto decide = [&](Attachment& a) {
            const GraphTexture produced{a.texture.index, static_cast<std::uint16_t>(a.texture.version + 1)};
            a.store = wanted(produced) ? rhi::StoreOp::Store : rhi::StoreOp::DontCare;
            if (a.store == rhi::StoreOp::Store) {
                ++stats_.attachments_stored;
            } else {
                ++stats_.attachments_discarded;
            }
        };
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            decide(pass.colors[i]);
        }
        if (pass.has_depth) {
            decide(pass.depth);
        }
    }
    // A transient no pass samples and no write stores never needs memory of
    // its own: it lives and dies inside the passes that draw it.
    stats_.memoryless = 0;
    for (std::uint32_t t = 0; t < textures_.size(); ++t) {
        Texture& node = textures_[t];
        if (node.imported || !node.used || rhi::has_usage(node.usage, rhi::TextureUsage::Sampled)) {
            continue;
        }
        bool stored = false;
        for (const Pass& pass : passes_) {
            if (pass.culled) {
                continue;
            }
            for (std::uint32_t i = 0; i < pass.color_count; ++i) {
                const Attachment& a = pass.colors[i];
                stored = stored || (a.texture.index == t && a.store == rhi::StoreOp::Store);
            }
            stored = stored || (pass.has_depth && pass.depth.texture.index == t &&
                                pass.depth.store == rhi::StoreOp::Store);
        }
        node.memoryless = !stored;
        stats_.memoryless += node.memoryless ? 1 : 0;
    }
}

// Transients in order of first use; each takes a pooled texture of its
// format, size and usage whose last user this frame has already run, or a
// new one. Physical textures outlive the frame, so next frame's identical
// graph finds them again.
void FrameGraph::assign_physical() noexcept {
    scratch_.clear();
    for (std::uint32_t t = 0; t < textures_.size(); ++t) {
        if (!textures_[t].imported && textures_[t].used) {
            scratch_.push_back(t);
        }
    }
    std::sort(scratch_.begin(), scratch_.end(), [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t first_a = textures_[a].first_use;
        const std::uint32_t first_b = textures_[b].first_use;
        return first_a != first_b ? first_a < first_b : a < b;
    });
    stats_.transients_used = static_cast<std::uint32_t>(scratch_.size());
    stats_.bytes_requested = 0;
    for (const std::uint32_t t : scratch_) {
        Texture& node = textures_[t];
        stats_.bytes_requested += texture_bytes(node.info);
        std::uint32_t slot = kNone;
        for (std::uint32_t p = 0; p < physicals_.size(); ++p) {
            const Physical& physical = physicals_[p];
            const bool same = physical.info.format == node.info.format &&
                              physical.info.width == node.info.width &&
                              physical.info.height == node.info.height && physical.usage == node.usage;
            const bool free = !physical.used_this_frame || physical.last_use < node.first_use;
            if (same && free) {
                slot = p;
                break;
            }
        }
        if (slot == kNone) {
            if (physicals_.size() == physicals_.capacity()) {
                fail("more transient textures alive at once than the graph allows");
                return;
            }
            physicals_.push_back(Physical{.info = node.info, .usage = node.usage});
            slot = static_cast<std::uint32_t>(physicals_.size() - 1);
        }
        Physical& physical = physicals_[slot];
        physical.used_this_frame = true;
        physical.last_use = node.last_use;
        physical.idle_frames = 0;
        node.physical = slot;
    }
    stats_.physical_textures = 0;
    stats_.bytes_allocated = 0;
    for (const Physical& physical : physicals_) {
        if (physical.used_this_frame) {
            ++stats_.physical_textures;
            stats_.bytes_allocated += texture_bytes(physical.info);
        }
    }
}

bool FrameGraph::compile() noexcept {
    TY_PROFILE_SCOPE_NAMED("FrameGraph::compile");
    compiled_ = false;
    if (error_[0] != '\0') {
        return false; // something went wrong while declaring
    }
    if (!validate() || !order_passes()) {
        return false;
    }
    cull();
    decide_lifetimes();
    decide_stores();
    assign_physical();
    if (error_[0] != '\0') {
        return false;
    }
    compiled_ = true;
    return true;
}

rhi::TextureHandle FrameGraph::resolve(GraphTexture texture) const noexcept {
    if (!texture || texture.index >= textures_.size()) {
        return {};
    }
    const Texture& node = textures_[texture.index];
    if (node.imported) {
        return node.imported_handle;
    }
    return node.physical == kNone ? rhi::TextureHandle{} : physicals_[node.physical].handle;
}

rhi::BufferHandle FrameGraph::resolve(GraphBuffer buffer) const noexcept {
    if (!buffer || buffer.index >= buffers_.size()) {
        return {};
    }
    return buffers_[buffer.index].handle;
}

void FrameGraph::execute(rhi::Frame& frame) noexcept {
    TY_PROFILE_SCOPE_NAMED("FrameGraph::execute");
    TY_ASSERT(device_ != nullptr, "FrameGraph::execute needs a device");
    if (!compiled_ || device_ == nullptr) {
        return;
    }
    // Physical textures that exist only on paper so far.
    for (Physical& physical : physicals_) {
        if (physical.used_this_frame && !physical.handle) {
            physical.handle = device_->create_texture({.format = physical.info.format,
                                                       .width = physical.info.width,
                                                       .height = physical.info.height,
                                                       .usage = physical.usage});
            if (!physical.handle) {
                TY_LOG_ERROR("graph", "cannot create a %ux%u %s transient: %s", physical.info.width,
                             physical.info.height, rhi::texture_format_name(physical.info.format),
                             platform::last_error());
                return;
            }
        }
    }

    const PassResources resources(*this);
    for (const std::uint32_t index : order_) {
        Pass& pass = passes_[index];
        if (pass.culled) {
            continue;
        }
        if (pass.compute) {
            rhi::ComputePassDesc desc{.name = pass.name};
            for (std::uint32_t w = 0; w < pass.buffer_write_count; ++w) {
                desc.writes[w] = resolve(pass.buffer_writes[w]);
            }
            desc.write_count = pass.buffer_write_count;
            auto compute_pass = frame.begin_compute_pass(desc);
            if (!compute_pass) {
                TY_LOG_ERROR("graph", "compute pass '%s' could not begin: %s", pass.name,
                             platform::last_error());
                continue;
            }
            pass.compute_execute(*compute_pass, resources);
            compute_pass->end();
            continue;
        }
        rhi::RenderPassDesc desc{.name = pass.name};
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            const Attachment& a = pass.colors[i];
            desc.colors[i] = rhi::ColorAttachment{
                .texture = resolve(a.texture), .load = a.load, .store = a.store, .clear = a.clear};
        }
        desc.color_count = pass.color_count;
        if (pass.has_depth) {
            desc.depth = rhi::DepthAttachment{.texture = resolve(pass.depth.texture),
                                              .load = pass.depth.load,
                                              .store = pass.depth.store,
                                              .clear = pass.depth.depth_clear};
        }
        auto render_pass = frame.begin_pass(desc);
        if (!render_pass) {
            TY_LOG_ERROR("graph", "pass '%s' could not begin: %s", pass.name, platform::last_error());
            continue;
        }
        pass.execute(*render_pass, resources);
        render_pass->end();
    }

    // Pooled textures nobody has asked for in a while go back to the device.
    for (std::size_t p = physicals_.size(); p-- > 0;) {
        Physical& physical = physicals_[p];
        if (physical.used_this_frame) {
            continue;
        }
        if (++physical.idle_frames >= limits_.idle_frames_before_free) {
            device_->destroy_texture(physical.handle);
            physicals_[p] = physicals_.back();
            physicals_.pop_back();
        }
    }
}

std::size_t FrameGraph::describe(char* out, std::size_t capacity) const noexcept {
    Writer w{out, capacity};
    char number[32];
    const auto attachment = [&](const Attachment& a) {
        w.put(textures_[a.texture.index].name);
        w.put(" (");
        w.put(load_name(a.load));
        w.put(", ");
        w.put(store_name(a.store));
        w.put(")");
    };
    const auto line = [&](const Pass& pass) {
        w.put(pass.name);
        if (pass.culled) {
            w.put(": culled\n");
            return;
        }
        w.put(pass.compute ? ": compute; " : ": ");
        if (pass.read_count + pass.buffer_read_count > 0) {
            w.put("reads ");
            for (std::uint32_t r = 0; r < pass.read_count; ++r) {
                w.put(r > 0 ? ", " : "");
                w.put(textures_[pass.reads[r].index].name);
            }
            for (std::uint32_t r = 0; r < pass.buffer_read_count; ++r) {
                w.put(r + pass.read_count > 0 ? ", " : "");
                w.put(buffers_[pass.buffer_reads[r].index].name);
            }
            w.put("; ");
        }
        w.put("writes ");
        std::uint32_t written = 0;
        for (std::uint32_t i = 0; i < pass.color_count; ++i) {
            w.put(written++ > 0 ? ", " : "");
            attachment(pass.colors[i]);
        }
        if (pass.has_depth) {
            w.put(written++ > 0 ? ", " : "");
            attachment(pass.depth);
        }
        for (std::uint32_t b = 0; b < pass.buffer_write_count; ++b) {
            w.put(written++ > 0 ? ", " : "");
            w.put(buffers_[pass.buffer_writes[b].index].name);
        }
        w.put("\n");
    };
    if (compiled_) {
        for (const std::uint32_t index : order_) {
            if (!passes_[index].culled) {
                line(passes_[index]);
            }
        }
        for (const Pass& pass : passes_) {
            if (pass.culled) {
                line(pass);
            }
        }
        for (std::uint32_t t = 0; t < textures_.size(); ++t) {
            const Texture& node = textures_[t];
            if (node.imported || !node.used) {
                continue;
            }
            w.put(node.name);
            w.put(": ");
            std::snprintf(number, sizeof number, "%ux%u ", node.info.width, node.info.height);
            w.put(number);
            w.put(rhi::texture_format_name(node.info.format));
            std::snprintf(number, sizeof number, ", passes %u-%u, texture %u", node.first_use, node.last_use,
                          node.physical);
            w.put(number);
            w.put(node.memoryless ? ", never leaves the tile\n" : "\n");
        }
    } else {
        w.put(error_[0] != '\0' ? error_ : "not compiled");
        w.put("\n");
    }
    w.finish();
    return w.length;
}

} // namespace tynima::render
