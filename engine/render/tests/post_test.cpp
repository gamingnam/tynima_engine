#include <doctest/doctest.h>
#include <tynima/render/camera.h>
#include <tynima/render/frame_graph.h>
#include <tynima/render/post.h>

#include <cmath>
#include <string>

using namespace tynima;
using namespace tynima::math;
using render::AntiAliasing;
using render::FrameGraph;
using render::GraphTexture;
using render::PassBuilder;
using render::PassResources;
using render::PostFrame;
using render::PostStack;
using render::TextureInfo;

namespace {

void nothing(rhi::RenderPass&, const PassResources&) {}

std::string described(const FrameGraph& graph) {
    char text[4096];
    graph.describe(text, sizeof text);
    return text;
}

// A frame's worth of graph: a scene pass into HDR and depth, then the stack.
struct Declared {
    GraphTexture hdr, depth, swapchain;
    std::uint32_t scene = 0;
};

Declared declare(FrameGraph& graph, PostStack& stack, std::uint32_t width, std::uint32_t height) {
    Declared d;
    graph.begin();
    d.swapchain =
        graph.import("swapchain", rhi::TextureHandle{7, 1}, {rhi::TextureFormat::Bgra8Srgb, width, height});
    d.hdr = graph.create("hdr", {rhi::TextureFormat::Rgba16Float, width, height});
    d.depth = graph.create("depth", {rhi::TextureFormat::Depth32Float, width, height});
    graph.add_pass(
        "scene",
        [&](PassBuilder& b) {
            d.hdr = b.write_color(d.hdr);
            d.depth = b.write_depth(d.depth);
        },
        nothing);
    render::Camera camera;
    const PostFrame frame{.width = width,
                          .height = height,
                          .encode_srgb = false,
                          .view_projection = camera.view_projection(16.0f / 9.0f),
                          .view_projection_jittered = camera.view_projection(16.0f / 9.0f)};
    d.swapchain = stack.add_passes(graph, d.hdr, d.depth, d.swapchain, frame);
    return d;
}

} // namespace

TEST_CASE("the Halton jitter covers the pixel and repeats after eight frames") {
    for (std::uint32_t i = 0; i < 8; ++i) {
        const Vec2 j = render::halton_jitter(i);
        CHECK(j.x >= -0.5f);
        CHECK(j.x <= 0.5f);
        CHECK(j.y >= -0.5f);
        CHECK(j.y <= 0.5f);
        for (std::uint32_t k = 0; k < i; ++k) {
            const Vec2 other = render::halton_jitter(k);
            CHECK((std::fabs(other.x - j.x) > 1e-3f || std::fabs(other.y - j.y) > 1e-3f));
        }
    }
    CHECK(render::halton_jitter(8).x == render::halton_jitter(0).x);
    CHECK(render::halton_jitter(0).x == doctest::Approx(0.0f)); // 1/2 - 1/2
    CHECK(render::halton_jitter(0).y == doctest::Approx(1.0f / 3.0f - 0.5f));
}

TEST_CASE("bloom halves the image until it is small") {
    CHECK(render::bloom_level_count(1280, 720) == 5);  // 640, 320, 160, 80, 40; 20x11 would be too small
    CHECK(render::bloom_level_count(2560, 1440) == 6); // capped
    CHECK(render::bloom_level_count(100, 40) == 1);
    CHECK(render::bloom_level_count(20, 20) == 0);
}

TEST_CASE("a jittered projection shifts the image by exactly the jitter") {
    render::Camera camera;
    camera.position = {0.3f, -0.2f, 1.0f};
    camera.rotation = Quat::from_axis_angle(Vec3::unit_y(), 0.4f);
    const Vec3 point =
        camera.position + camera.forward() * 3.0f + camera.right() * 0.7f + camera.up() * -0.4f;
    const Vec2 jitter{0.0011f, -0.0007f};
    const Vec3 plain = project_point(camera.view_projection(1.5f), point);
    const Vec3 shifted = project_point(camera.view_projection(1.5f, jitter), point);
    CHECK(shifted.x - plain.x == doctest::Approx(jitter.x).epsilon(1e-3));
    CHECK(shifted.y - plain.y == doctest::Approx(jitter.y).epsilon(1e-3));
    CHECK(shifted.z == doctest::Approx(plain.z));
}

TEST_CASE("the stack declares bloom, tonemap and FXAA passes in order, and TAA only with a device") {
    FrameGraph graph(nullptr);
    PostStack stack; // never created: no device, no pipelines, but the same declarations
    CHECK_FALSE(stack.ready());
    CHECK(stack.jitter(1280, 720).x == 0.0f); // no TAA without a device

    SUBCASE("bloom and tonemap") {
        stack.settings.anti_aliasing = AntiAliasing::None;
        const Declared d = declare(graph, stack, 1280, 720);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.pass_count() == 1 + 5 + 4 + 1); // scene, five down, four up, tonemap
        CHECK(graph.stats().culled == 0);
        const std::string text = described(graph);
        const auto has = [&](const char* line) { return text.find(line) != std::string::npos; };
        CHECK(has("bloom down 0: reads hdr; writes bloom down 0 (dontcare, store)\n"));
        CHECK(has("bloom up 0: reads bloom up 1, bloom down 0; writes bloom up 0 (dontcare, store)\n"));
        CHECK(has("tonemap: reads hdr, bloom up 0; writes swapchain (clear, store)\n"));
        CHECK(text.find("bloom down 4:") < text.find("bloom up 3:"));
        CHECK(text.find("bloom up 0:") < text.find("tonemap:"));
        // Without TAA nothing reads the depth: it never leaves the tile.
        CHECK(graph.texture(d.depth.index).memoryless);
        // Nothing in the chain can share a texture: every level's up-pass
        // reads that level's down-image while writing, and the levels
        // differ in size. Eleven transients, eleven textures — for now.
        CHECK(graph.stats().transients_used == 2 + 5 + 4);
        CHECK(graph.stats().physical_textures == graph.stats().transients_used);
    }
    SUBCASE("no bloom") {
        stack.settings.anti_aliasing = AntiAliasing::None;
        stack.settings.bloom = false;
        (void)declare(graph, stack, 1280, 720);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.pass_count() == 2);
        CHECK(described(graph).find("tonemap: reads hdr; writes swapchain") != std::string::npos);
    }
    SUBCASE("a pass over the picture, reading the version the stack returns, runs after it") {
        // The editor's frame: the stack draws into a viewport texture, and
        // the UI pass shows it on the window — so it must read the version
        // the stack wrote, and run after the pass that wrote it.
        stack.settings.anti_aliasing = AntiAliasing::Fxaa;
        stack.settings.bloom = false;
        const Declared d = declare(graph, stack, 1280, 720);
        CHECK(d.swapchain.version == graph.texture(d.swapchain.index).versions);
        CHECK(d.swapchain.version == 1); // the tonemap wrote the LDR image; FXAA wrote the picture once
        const GraphTexture viewport = d.swapchain;
        GraphTexture window =
            graph.import("window", rhi::TextureHandle{8, 1}, {rhi::TextureFormat::Bgra8Srgb, 1600, 960});
        graph.add_pass(
            "ui",
            [&](PassBuilder& b) {
                b.read(viewport);
                window = b.write_color(window, rhi::LoadOp::Clear);
            },
            nothing);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        const std::string text = described(graph);
        CHECK(text.find("fxaa:") < text.find("ui:"));
        CHECK(text.find("ui: reads swapchain; writes window (clear, store)") != std::string::npos);
    }
    SUBCASE("TAA asked for, no device: the frame still presents") {
        stack.settings.anti_aliasing = AntiAliasing::Taa;
        stack.settings.bloom = false;
        (void)declare(graph, stack, 1280, 720);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.pass_count() == 2);
    }
}
