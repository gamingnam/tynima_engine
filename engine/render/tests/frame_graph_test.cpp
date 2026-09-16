#include <doctest/doctest.h>
#include <tynima/core/memory.h>
#include <tynima/render/frame_graph.h>

#include <cstring>
#include <string>

using namespace tynima;
using render::FrameGraph;
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
