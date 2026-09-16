#include <doctest/doctest.h>
#include <tynima/core/memory.h>
#include <tynima/render/frame_graph.h>

#include <cstring>
#include <string>

using namespace tynima;
using render::FrameGraph;
using render::GraphBuffer;
using render::GraphTexture;
using render::PassBuilder;
using render::PassResources;
using render::TextureInfo;

namespace {

constexpr TextureInfo kHdr{rhi::TextureFormat::Rgba16Float, 1280, 720};
constexpr TextureInfo kDepth{rhi::TextureFormat::Depth32Float, 1280, 720};
constexpr TextureInfo kSwapchain{rhi::TextureFormat::Bgra8Srgb, 1280, 720};
constexpr TextureInfo kSmall{rhi::TextureFormat::Rgba8Unorm, 256, 256};

// A handle that stands for a real texture, as the swapchain's would.
const rhi::TextureHandle kFakeSwapchain{7, 1};

void nothing(rhi::RenderPass&, const PassResources&) {}

std::string described(const FrameGraph& graph) {
    char text[2048];
    graph.describe(text, sizeof text);
    return text;
}

// The graph every real frame has: a scene drawn into HDR with depth, then
// tonemapped onto the swapchain.
struct SceneAndTonemap {
    GraphTexture hdr, depth, swapchain;
    std::uint32_t scene = 0, tonemap = 1;

    void declare(FrameGraph& graph) {
        graph.begin();
        swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
        hdr = graph.create("hdr", kHdr);
        depth = graph.create("depth", kDepth);
        graph.add_pass(
            "scene",
            [&](PassBuilder& b) {
                hdr = b.write_color(hdr, rhi::LoadOp::Clear, {0.1f, 0.1f, 0.1f});
                depth = b.write_depth(depth, rhi::LoadOp::Clear, 0.0f);
            },
            nothing);
        graph.add_pass(
            "tonemap",
            [&](PassBuilder& b) {
                b.read(hdr);
                swapchain = b.write_color(swapchain, rhi::LoadOp::DontCare);
            },
            nothing);
    }
};

} // namespace

TEST_CASE("a scene and a tonemap: the order, the stores, and a depth that never leaves the tile") {
    FrameGraph graph(nullptr);
    SceneAndTonemap frame;
    frame.declare(graph);
    REQUIRE_MESSAGE(graph.compile(), graph.error());

    // Versions advanced with each write, and the passes run as added.
    CHECK(frame.hdr.version == 1);
    CHECK(frame.swapchain.version == 1);
    CHECK_FALSE(graph.pass(frame.scene).culled);
    CHECK_FALSE(graph.pass(frame.tonemap).culled);
    CHECK(graph.pass(frame.scene).order == 0);
    CHECK(graph.pass(frame.tonemap).order == 1);
    CHECK(graph.pass(frame.tonemap).depends_on == 1u << frame.scene);

    // HDR is read later, so it is stored; depth is not, so it is discarded —
    // and, never sampled or stored, it could be memoryless.
    CHECK(graph.pass(frame.scene).colors[0].store == rhi::StoreOp::Store);
    CHECK(graph.pass(frame.scene).depth.store == rhi::StoreOp::DontCare);
    CHECK(graph.pass(frame.tonemap).colors[0].store == rhi::StoreOp::Store); // the swapchain: imported
    CHECK(graph.texture(frame.depth.index).memoryless);
    CHECK_FALSE(graph.texture(frame.hdr.index).memoryless);
    CHECK(graph.texture(frame.hdr.index).usage ==
          (rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled));
    CHECK(graph.texture(frame.depth.index).usage == rhi::TextureUsage::DepthStencilTarget);
    CHECK(graph.texture(frame.hdr.index).first_use == 0);
    CHECK(graph.texture(frame.hdr.index).last_use == 1);

    const FrameGraph::Stats& stats = graph.stats();
    CHECK(stats.passes == 2);
    CHECK(stats.culled == 0);
    CHECK(stats.transients == 2);
    CHECK(stats.physical_textures == 2);
    CHECK(stats.bytes_requested == 1280u * 720u * (8u + 4u));
    CHECK(stats.bytes_allocated == stats.bytes_requested);
    CHECK(stats.attachments_stored == 2);
    CHECK(stats.attachments_discarded == 1);
    CHECK(stats.memoryless == 1);

    const std::string text = described(graph);
    CHECK(text.find("scene: writes hdr (clear, store), depth (clear, discard)\n") != std::string::npos);
    CHECK(text.find("tonemap: reads hdr; writes swapchain (dontcare, store)\n") != std::string::npos);
    CHECK(text.find("depth: 1280x720 D32_FLOAT, passes 0-0, texture 1, never leaves the tile\n") !=
          std::string::npos);
}

TEST_CASE("passes nobody consumes are culled, and so is whatever only fed them") {
    FrameGraph graph(nullptr);
    graph.begin();
    GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
    GraphTexture a = graph.create("a", kSmall);
    GraphTexture b = graph.create("b", kSmall);
    GraphTexture c = graph.create("c", kSmall);
    GraphTexture d = graph.create("d", kSmall);
    GraphTexture e = graph.create("e", kSmall);
    graph.add_pass("make a", [&](PassBuilder& p) { a = p.write_color(a); }, nothing);
    graph.add_pass(
        "a to b",
        [&](PassBuilder& p) {
            p.read(a);
            b = p.write_color(b);
        },
        nothing);
    graph.add_pass(
        "present b",
        [&](PassBuilder& p) {
            p.read(b);
            swapchain = p.write_color(swapchain);
        },
        nothing);
    graph.add_pass("make c", [&](PassBuilder& p) { c = p.write_color(c); }, nothing); // nobody reads c
    graph.add_pass(
        "c to d",
        [&](PassBuilder& p) {
            p.read(c);
            d = p.write_color(d);
        },
        nothing); // nor d
    graph.add_pass(
        "debug e",
        [&](PassBuilder& p) {
            e = p.write_color(e);
            p.side_effect();
        },
        nothing);
    REQUIRE_MESSAGE(graph.compile(), graph.error());
    CHECK_FALSE(graph.pass(0).culled);
    CHECK_FALSE(graph.pass(1).culled);
    CHECK_FALSE(graph.pass(2).culled);
    CHECK(graph.pass(3).culled);
    CHECK(graph.pass(4).culled);
    CHECK_FALSE(graph.pass(5).culled);
    CHECK(graph.stats().culled == 2);
    CHECK_FALSE(graph.texture(c.index).used);
    CHECK(graph.texture(c.index).physical == FrameGraph::kNone);
    CHECK(graph.stats().transients_used == 3); // a, b, e
    CHECK(described(graph).find("make c: culled\n") != std::string::npos);
}

TEST_CASE("passes are ordered by what they read, not by when they were added") {
    FrameGraph graph(nullptr);
    graph.begin();
    GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
    const GraphTexture hdr = graph.create("hdr", kHdr);
    const GraphTexture hdr_drawn{hdr.index, 1}; // the version the scene pass will make
    graph.add_pass(
        "tonemap",
        [&](PassBuilder& b) {
            b.read(hdr_drawn);
            swapchain = b.write_color(swapchain);
        },
        nothing);
    graph.add_pass("scene", [&](PassBuilder& b) { (void)b.write_color(hdr); }, nothing);
    REQUIRE_MESSAGE(graph.compile(), graph.error());
    CHECK(graph.pass(1).order == 0); // scene first
    CHECK(graph.pass(0).order == 1);
    CHECK(graph.pass(1).colors[0].store == rhi::StoreOp::Store);
    const std::string text = described(graph);
    CHECK(text.find("scene:") < text.find("tonemap:"));
}

TEST_CASE("a write that a later pass loads is stored; one it clears over is not, nor is its pass") {
    for (const rhi::LoadOp second_load : {rhi::LoadOp::Load, rhi::LoadOp::Clear}) {
        FrameGraph graph(nullptr);
        graph.begin();
        GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
        GraphTexture t = graph.create("t", kSmall);
        graph.add_pass("first", [&](PassBuilder& b) { t = b.write_color(t, rhi::LoadOp::Clear); }, nothing);
        graph.add_pass("second", [&](PassBuilder& b) { t = b.write_color(t, second_load); }, nothing);
        graph.add_pass(
            "present",
            [&](PassBuilder& b) {
                b.read(t);
                swapchain = b.write_color(swapchain);
            },
            nothing);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(t.version == 2);
        if (second_load == rhi::LoadOp::Load) {
            CHECK_FALSE(graph.pass(0).culled);
            CHECK(graph.pass(0).colors[0].store == rhi::StoreOp::Store);
            CHECK(graph.pass(1).depends_on == 1u); // after the first write
        } else {
            CHECK(graph.pass(0).culled); // its clear is overwritten by the next clear
        }
        CHECK(graph.pass(1).colors[0].store == rhi::StoreOp::Store); // read by present
    }
}

TEST_CASE("readers of a version run before the pass that overwrites it") {
    FrameGraph graph(nullptr);
    graph.begin();
    GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
    GraphTexture t = graph.create("t", kSmall);
    GraphTexture copy = graph.create("copy", kSmall);
    graph.add_pass("draw", [&](PassBuilder& b) { t = b.write_color(t); }, nothing);
    const GraphTexture drawn = t;
    // Added before the reader, but it overwrites what the reader wants: it
    // must wait.
    graph.add_pass("redraw", [&](PassBuilder& b) { t = b.write_color(t, rhi::LoadOp::Clear); }, nothing);
    graph.add_pass(
        "read the first drawing",
        [&](PassBuilder& b) {
            b.read(drawn);
            copy = b.write_color(copy);
        },
        nothing);
    graph.add_pass(
        "present both",
        [&](PassBuilder& b) {
            b.read(t);
            b.read(copy);
            swapchain = b.write_color(swapchain);
        },
        nothing);
    REQUIRE_MESSAGE(graph.compile(), graph.error());
    CHECK(graph.pass(0).order == 0);
    CHECK(graph.pass(2).order == 1); // the reader
    CHECK(graph.pass(1).order == 2); // then the overwrite
    CHECK(graph.pass(3).order == 3);
    CHECK((graph.pass(1).depends_on & (1u << 2)) != 0);
}

TEST_CASE("transients that are never alive at once share a physical texture") {
    FrameGraph graph(nullptr);
    graph.begin();
    GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
    GraphTexture t1 = graph.create("t1", kSmall);
    GraphTexture t2 = graph.create("t2", kSmall);
    GraphTexture t3 = graph.create("t3", kSmall);
    graph.add_pass("a", [&](PassBuilder& b) { t1 = b.write_color(t1); }, nothing);
    graph.add_pass(
        "b",
        [&](PassBuilder& b) {
            b.read(t1);
            t2 = b.write_color(t2);
        },
        nothing);
    graph.add_pass(
        "c",
        [&](PassBuilder& b) {
            b.read(t2);
            t3 = b.write_color(t3);
        },
        nothing);
    graph.add_pass(
        "d",
        [&](PassBuilder& b) {
            b.read(t3);
            swapchain = b.write_color(swapchain);
        },
        nothing);
    REQUIRE_MESSAGE(graph.compile(), graph.error());
    // t1 lives through passes 0-1, t2 through 1-2, t3 through 2-3: t1 and t3
    // never overlap, t2 overlaps both.
    CHECK(graph.texture(t1.index).physical == graph.texture(t3.index).physical);
    CHECK(graph.texture(t2.index).physical != graph.texture(t1.index).physical);
    CHECK(graph.stats().transients_used == 3);
    CHECK(graph.stats().physical_textures == 2);
    CHECK(graph.stats().bytes_allocated * 3 == graph.stats().bytes_requested * 2);

    // The same graph next frame lands on the same textures.
    const std::uint32_t slot1 = graph.texture(t1.index).physical;
    const std::uint32_t slot2 = graph.texture(t2.index).physical;
    graph.begin();
    swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
    t1 = graph.create("t1", kSmall);
    t2 = graph.create("t2", kSmall);
    graph.add_pass("a", [&](PassBuilder& b) { t1 = b.write_color(t1); }, nothing);
    graph.add_pass(
        "b",
        [&](PassBuilder& b) {
            b.read(t1);
            t2 = b.write_color(t2);
        },
        nothing);
    graph.add_pass(
        "d",
        [&](PassBuilder& b) {
            b.read(t2);
            swapchain = b.write_color(swapchain);
        },
        nothing);
    REQUIRE_MESSAGE(graph.compile(), graph.error());
    CHECK(graph.texture(t1.index).physical == slot1);
    CHECK(graph.texture(t2.index).physical == slot2);
    CHECK(graph.stats().physical_textures == 2);
}

TEST_CASE("a malformed graph is refused with a reason") {
    FrameGraph graph(nullptr);
    const auto error_contains = [&](const char* fragment) {
        const bool ok = !graph.compile();
        CHECK_MESSAGE(std::strstr(graph.error(), fragment) != nullptr, graph.error());
        return ok;
    };

    SUBCASE("a read of something nothing wrote") {
        graph.begin();
        GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
        const GraphTexture t = graph.create("t", kSmall);
        graph.add_pass(
            "present",
            [&](PassBuilder& b) {
                b.read(t);
                swapchain = b.write_color(swapchain);
            },
            nothing);
        CHECK(error_contains("nothing has written"));
    }
    SUBCASE("a load of something nothing wrote") {
        graph.begin();
        GraphTexture t = graph.create("t", kSmall);
        graph.add_pass(
            "draw",
            [&](PassBuilder& b) {
                t = b.write_color(t, rhi::LoadOp::Load);
                b.side_effect();
            },
            nothing);
        CHECK(error_contains("loads a transient"));
    }
    SUBCASE("a pass with nothing to draw into") {
        graph.begin();
        const GraphTexture t = graph.create("t", kSmall);
        graph.add_pass("draw", [&](PassBuilder& b) { (void)b.write_color(t); }, nothing);
        graph.add_pass("empty", [&](PassBuilder&) {}, nothing);
        CHECK(error_contains("every pass needs an attachment"));
    }
    SUBCASE("a write of an old version") {
        graph.begin();
        const GraphTexture t = graph.create("t", kSmall);
        graph.add_pass("first", [&](PassBuilder& b) { (void)b.write_color(t); }, nothing);
        // t itself again, not the version the first pass returned.
        graph.add_pass("second", [&](PassBuilder& b) { (void)b.write_color(t); }, nothing);
        CHECK(error_contains("old version"));
    }
    SUBCASE("a cycle") {
        graph.begin();
        const GraphTexture t1 = graph.create("t1", kSmall);
        const GraphTexture t2 = graph.create("t2", kSmall);
        graph.add_pass(
            "one",
            [&](PassBuilder& b) {
                b.read(GraphTexture{t2.index, 1});
                (void)b.write_color(t1);
                b.side_effect();
            },
            nothing);
        graph.add_pass(
            "two",
            [&](PassBuilder& b) {
                b.read(GraphTexture{t1.index, 1});
                (void)b.write_color(t2);
                b.side_effect();
            },
            nothing);
        CHECK(error_contains("cycle"));
    }
    SUBCASE("attachments of different sizes") {
        graph.begin();
        const GraphTexture hdr = graph.create("hdr", kHdr);
        const GraphTexture small = graph.create("small", kSmall);
        graph.add_pass(
            "draw",
            [&](PassBuilder& b) {
                (void)b.write_color(hdr);
                (void)b.write_color(small);
                b.side_effect();
            },
            nothing);
        CHECK(error_contains("differ in size"));
    }
    SUBCASE("a depth texture as a color attachment") {
        graph.begin();
        const GraphTexture depth = graph.create("depth", kDepth);
        graph.add_pass(
            "draw",
            [&](PassBuilder& b) {
                (void)b.write_color(depth);
                b.side_effect();
            },
            nothing);
        CHECK(error_contains("depth format"));
    }
    SUBCASE("reading what the same pass writes") {
        graph.begin();
        GraphTexture t = graph.create("t", kSmall);
        graph.add_pass("draw", [&](PassBuilder& b) { t = b.write_color(t); }, nothing);
        graph.add_pass(
            "feedback",
            [&](PassBuilder& b) {
                b.read(t);
                t = b.write_color(t, rhi::LoadOp::Load);
                b.side_effect();
            },
            nothing);
        CHECK(error_contains("also writes"));
    }
    SUBCASE("over budget") {
        FrameGraph tiny(nullptr, FrameGraph::Limits{.max_passes = 1, .max_textures = 2});
        tiny.begin();
        const GraphTexture t = tiny.create("t", kSmall);
        tiny.add_pass("one", [&](PassBuilder& b) { (void)b.write_color(t); }, nothing);
        tiny.add_pass("two", [&](PassBuilder& b) { (void)b.write_color(t); }, nothing);
        CHECK_FALSE(tiny.compile());
        CHECK(std::strstr(tiny.error(), "more passes") != nullptr);
    }
}

TEST_CASE("declaring and compiling a frame's graph allocates nothing") {
    FrameGraph graph(nullptr);
    SceneAndTonemap frame;
    frame.declare(graph); // the first frame may reserve; it does not, but the rule is about steady state
    REQUIRE(graph.compile());
    const core::HeapAllocationScope scope;
    for (int i = 0; i < 3; ++i) {
        frame.declare(graph);
        REQUIRE(graph.compile());
    }
    CHECK(scope.allocations() == 0);
}

TEST_CASE("a compute pass writes a buffer the scene reads, and runs before it") {
    FrameGraph graph(nullptr);
    graph.begin();
    GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
    const rhi::BufferHandle lights_handle{3, 1};
    const rhi::BufferHandle clusters_handle{4, 1};
    GraphBuffer lights = graph.import_buffer("lights", lights_handle);
    GraphBuffer clusters = graph.import_buffer("clusters", clusters_handle);
    int ran = 0;
    // Added after the pass that reads its result: the order comes from the versions.
    graph.add_pass(
        "scene",
        [&](PassBuilder& b) {
            b.read_buffer(GraphBuffer{clusters.index, 1});
            b.read_buffer(lights);
            swapchain = b.write_color(swapchain);
        },
        nothing);
    graph.add_compute_pass(
        "cull lights",
        [&](PassBuilder& b) {
            b.read_buffer(lights);
            clusters = b.write_buffer(clusters);
        },
        [&](rhi::ComputePass&, const PassResources& r) {
            ran += r.buffer(clusters) == clusters_handle ? 1 : 0;
        });
    REQUIRE_MESSAGE(graph.compile(), graph.error());
    CHECK(clusters.version == 1);
    CHECK(graph.pass(1).compute);
    CHECK_FALSE(graph.pass(1).culled);
    CHECK(graph.pass(1).order == 0);
    CHECK(graph.pass(0).order == 1);
    CHECK(graph.pass(0).depends_on == (1u << 1));
    CHECK(graph.buffer(lights.index).used);
    const std::string text = described(graph);
    CHECK(text.find("cull lights: compute; reads lights; writes clusters\n") != std::string::npos);
    CHECK(text.find("scene: reads clusters, lights; writes swapchain (clear, store)\n") != std::string::npos);
    CHECK(ran == 0); // nothing executed without a device
}

TEST_CASE("compute passes keep to buffers, render passes to attachments") {
    FrameGraph graph(nullptr);
    SUBCASE("a compute pass with an attachment") {
        graph.begin();
        const GraphTexture t = graph.create("t", kSmall);
        graph.add_compute_pass(
            "wrong", [&](PassBuilder& b) { (void)b.write_color(t); },
            [](rhi::ComputePass&, const PassResources&) {});
        CHECK_FALSE(graph.compile());
        CHECK(std::strstr(graph.error(), "not textures") != nullptr);
    }
    SUBCASE("a compute pass that writes nothing") {
        graph.begin();
        const GraphBuffer buf = graph.import_buffer("buf", rhi::BufferHandle{1, 1});
        graph.add_compute_pass(
            "idle", [&](PassBuilder& b) { b.read_buffer(buf); },
            [](rhi::ComputePass&, const PassResources&) {});
        CHECK_FALSE(graph.compile());
        CHECK(std::strstr(graph.error(), "writes nothing") != nullptr);
    }
    SUBCASE("a render pass that writes a buffer") {
        graph.begin();
        const GraphBuffer buf = graph.import_buffer("buf", rhi::BufferHandle{1, 1});
        const GraphTexture t = graph.create("t", kSmall);
        graph.add_pass(
            "wrong",
            [&](PassBuilder& b) {
                (void)b.write_color(t);
                (void)b.write_buffer(buf);
            },
            nothing);
        CHECK_FALSE(graph.compile());
        CHECK(std::strstr(graph.error(), "takes a compute pass") != nullptr);
    }
    SUBCASE("a pass that writes an old version of a buffer") {
        graph.begin();
        const GraphBuffer buf = graph.import_buffer("buf", rhi::BufferHandle{1, 1});
        graph.add_compute_pass(
            "first", [&](PassBuilder& b) { (void)b.write_buffer(buf); },
            [](rhi::ComputePass&, const PassResources&) {});
        graph.add_compute_pass(
            "second", [&](PassBuilder& b) { (void)b.write_buffer(buf); },
            [](rhi::ComputePass&, const PassResources&) {});
        CHECK_FALSE(graph.compile());
        CHECK(std::strstr(graph.error(), "old version of a buffer") != nullptr);
    }
}

TEST_CASE("a compute pass whose buffer a later pass overwrites still runs, in order") {
    // Two in-place updates of one table: the second depends on the first
    // (it may read what is there), and both stay since buffers persist.
    FrameGraph graph(nullptr);
    graph.begin();
    GraphBuffer table = graph.import_buffer("table", rhi::BufferHandle{2, 1});
    graph.add_compute_pass(
        "clear", [&](PassBuilder& b) { table = b.write_buffer(table); },
        [](rhi::ComputePass&, const PassResources&) {});
    graph.add_compute_pass(
        "fill", [&](PassBuilder& b) { table = b.write_buffer(table); },
        [](rhi::ComputePass&, const PassResources&) {});
    REQUIRE_MESSAGE(graph.compile(), graph.error());
    CHECK(table.version == 2);
    CHECK_FALSE(graph.pass(0).culled);
    CHECK_FALSE(graph.pass(1).culled);
    CHECK(graph.pass(1).depends_on == 1u);
    CHECK(graph.pass(0).order == 0);
}

TEST_CASE("a fused G-buffer pass keeps its attachments on the tile; a split one stores them") {
    const TextureInfo albedo{rhi::TextureFormat::Rgba8Unorm, 1280, 720};
    const TextureInfo normal{rhi::TextureFormat::Rgba16Float, 1280, 720};
    const TextureInfo depth_occlusion{rhi::TextureFormat::Rg32Float, 1280, 720};

    SUBCASE("fused: material and lighting in one pass") {
        FrameGraph graph(nullptr);
        graph.begin();
        GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
        GraphTexture hdr = graph.create("hdr", kHdr);
        GraphTexture depth = graph.create("depth", kDepth);
        GraphTexture g1 = graph.create("g albedo", albedo);
        GraphTexture g2 = graph.create("g normal", normal);
        GraphTexture g3 = graph.create("g depth", depth_occlusion);
        graph.add_pass(
            "gbuffer+lighting",
            [&](PassBuilder& b) {
                hdr = b.write_color(hdr);
                g1 = b.write_color(g1, rhi::LoadOp::DontCare);
                g2 = b.write_color(g2, rhi::LoadOp::DontCare);
                g3 = b.write_color(g3);
                depth = b.write_depth(depth);
            },
            nothing);
        graph.add_pass(
            "tonemap",
            [&](PassBuilder& b) {
                b.read(hdr);
                swapchain = b.write_color(swapchain);
            },
            nothing);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        // Only HDR is read afterwards: the G-buffer and the depth never leave the tile.
        CHECK(graph.stats().memoryless == 4);
        CHECK(graph.texture(g1.index).memoryless);
        CHECK(graph.texture(g2.index).memoryless);
        CHECK(graph.texture(g3.index).memoryless);
        CHECK(graph.texture(depth.index).memoryless);
        CHECK_FALSE(graph.texture(hdr.index).memoryless);
        CHECK(graph.stats().attachments_stored == 2); // hdr and the swapchain
        CHECK(graph.stats().attachments_discarded == 4);
        // Twenty bytes a pixel of G-buffer plus four of depth, asked for and — with a
        // backend that can — never allocated. Without one they are counted as
        // ordinary textures, which is what a null device gets here.
        CHECK(graph.stats().bytes_memoryless == 0);
        CHECK(graph.stats().bytes_requested == 1280u * 720u * (8u + 4u + 4u + 8u + 8u));
    }
    SUBCASE("split: the G-buffer stored by one pass and sampled by the next") {
        FrameGraph graph(nullptr);
        graph.begin();
        GraphTexture swapchain = graph.import("swapchain", kFakeSwapchain, kSwapchain);
        GraphTexture emissive = graph.create("g emissive", kHdr);
        GraphTexture hdr = graph.create("hdr", kHdr);
        GraphTexture depth = graph.create("depth", kDepth);
        GraphTexture g1 = graph.create("g albedo", albedo);
        GraphTexture g2 = graph.create("g normal", normal);
        GraphTexture g3 = graph.create("g depth", depth_occlusion);
        graph.add_pass(
            "gbuffer",
            [&](PassBuilder& b) {
                emissive = b.write_color(emissive);
                g1 = b.write_color(g1, rhi::LoadOp::DontCare);
                g2 = b.write_color(g2, rhi::LoadOp::DontCare);
                g3 = b.write_color(g3);
                depth = b.write_depth(depth);
            },
            nothing);
        graph.add_pass(
            "lighting",
            [&](PassBuilder& b) {
                b.read(emissive);
                b.read(g1);
                b.read(g2);
                b.read(g3);
                hdr = b.write_color(hdr, rhi::LoadOp::DontCare);
            },
            nothing);
        graph.add_pass(
            "tonemap",
            [&](PassBuilder& b) {
                b.read(hdr);
                swapchain = b.write_color(swapchain);
            },
            nothing);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.stats().memoryless == 1); // only the depth
        CHECK(graph.stats().attachments_stored == 6);
        CHECK(graph.stats().attachments_discarded == 1);
        // Two more textures in memory than the fused frame, and the G-buffer
        // written out and read back: what the fused pass saves.
        CHECK(graph.stats().physical_textures == 6);
    }
}
