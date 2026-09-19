#include <tynima/render/frame_graph.h>
#include <tynima/render/post.h>
#include <tynima/render/scene_renderer.h>

#include <doctest/doctest.h>

#include <string>

using namespace tynima;
using render::DrawItem;
using render::FrameGraph;
using render::GraphTexture;
using render::SceneFrame;
using render::SceneRenderer;
using render::ShadingPath;

namespace {

std::string described(const FrameGraph& graph) {
    char text[4096];
    graph.describe(text, sizeof text);
    return text;
}

// A frame's worth of graph at 1280x720: the scene's passes into HDR and
// depth, then the post stack's (bloom and TAA off, so it is one tonemap).
void declare(FrameGraph& graph, SceneRenderer& scene, render::PostStack& post, ShadingPath path,
             std::uint32_t draw_count) {
    static const DrawItem draws[2]{};
    graph.begin();
    GraphTexture swapchain =
        graph.import("swapchain", rhi::TextureHandle{7, 1}, {rhi::TextureFormat::Bgra8Srgb, 1280, 720});
    GraphTexture hdr = graph.create("hdr", {rhi::TextureFormat::Rgba16Float, 1280, 720});
    GraphTexture depth = graph.create("depth", {rhi::TextureFormat::Depth32Float, 1280, 720});
    SceneFrame frame;
    frame.width = 1280;
    frame.height = 720;
    frame.draws = draws;
    frame.draw_count = draw_count;
    frame.settings.path = path;
    scene.add_passes(graph, hdr, depth, frame);
    post.settings.bloom = false;
    post.settings.anti_aliasing = render::AntiAliasing::None;
    post.add_passes(graph, hdr, depth, swapchain,
                    {.width = 1280,
                     .height = 720,
                     .view_projection = frame.camera.view_projection(16.0f / 9.0f),
                     .view_projection_jittered = frame.camera.view_projection(16.0f / 9.0f)});
}

} // namespace

TEST_CASE("the names of the shading choices") {
    CHECK(std::string(render::shading_path_name(ShadingPath::Fused)) == "fused deferred");
    CHECK(std::string(render::shading_model_name(render::ShadingModel::CookTorrance)) == "cook-torrance");
    CHECK(std::string(render::debug_view_name(render::DebugView::Clusters)) == "lights per cluster");
    CHECK(render::debug_view_is_lit(render::DebugView::Lit));
    CHECK(render::debug_view_is_lit(render::DebugView::Cascades));
    CHECK_FALSE(render::debug_view_is_lit(render::DebugView::Normals));
}

TEST_CASE("the scene's passes, declared without a device, take the shape of each path") {
    FrameGraph graph(nullptr);
    SceneRenderer scene; // never created: no pipelines, the same declarations
    render::PostStack post;
    CHECK_FALSE(scene.ready());

    SUBCASE("forward: four shadow cascades, the scene, the tonemap") {
        declare(graph, scene, post, ShadingPath::Forward, 2);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.pass_count() == 4 + 1 + 1);
        CHECK(graph.stats().culled == 0);
        CHECK(scene.path_taken() == ShadingPath::Forward);
        CHECK(scene.cascades().count == 4);
        const std::string text = described(graph);
        const auto has = [&](const char* line) { return text.find(line) != std::string::npos; };
        CHECK(has("shadow 0: writes shadow 0 (clear, store)\n"));
        CHECK(has(
            "scene: reads shadow 0, shadow 1, shadow 2, shadow 3; writes hdr (clear, store), depth (clear, "
            "discard)\n"));
        CHECK(has("tonemap: reads hdr; writes swapchain (clear, store)\n"));
        CHECK(text.find("shadow 3:") < text.find("scene:"));
        // Nothing reads the depth: it never leaves the tile.
        CHECK(graph.stats().memoryless == 1);
    }
    SUBCASE("nothing to draw: no shadow passes either") {
        declare(graph, scene, post, ShadingPath::Forward, 0);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.pass_count() == 2);
        CHECK(scene.cascades().count == 0);
    }
    SUBCASE("fused: the G-buffer and the lighting in one pass, three attachments never stored") {
        declare(graph, scene, post, ShadingPath::Fused, 2);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.pass_count() == 4 + 1 + 1);
        CHECK(scene.path_taken() == ShadingPath::Fused);
        const std::string text = described(graph);
        CHECK(
            text.find(
                "gbuffer+lighting: reads shadow 0, shadow 1, shadow 2, shadow 3; writes hdr (clear, store), "
                "g albedo (dontcare, discard), g normal (dontcare, discard), g depth (clear, discard), "
                "depth (clear, discard)\n") != std::string::npos);
        // The G-buffer and the depth: four transients that never leave the tile.
        CHECK(graph.stats().memoryless == 4);
        CHECK(graph.stats().attachments_discarded == 4);
    }
    SUBCASE("split: the G-buffer stored by one pass and sampled by the next") {
        declare(graph, scene, post, ShadingPath::Split, 2);
        REQUIRE_MESSAGE(graph.compile(), graph.error());
        CHECK(graph.pass_count() == 4 + 2 + 1);
        CHECK(scene.path_taken() == ShadingPath::Split);
        const std::string text = described(graph);
        CHECK(
            text.find(
                "gbuffer: writes g emissive (clear, store), g albedo (dontcare, store), g normal (dontcare, "
                "store), g depth (clear, store), depth (clear, discard)\n") != std::string::npos);
        CHECK(text.find(
                  "lighting: reads shadow 0, shadow 1, shadow 2, shadow 3, g emissive, g albedo, g normal, "
                  "g depth; writes hdr (dontcare, store)\n") != std::string::npos);
        CHECK(text.find("gbuffer:") < text.find("lighting:"));
        CHECK(graph.stats().memoryless == 1); // only the depth
    }
}
