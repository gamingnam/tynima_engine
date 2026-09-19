#include <tynima/platform/platform.h>
#include <tynima/platform/window.h>
#include <tynima/rhi/device.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace platform = tynima::platform;
namespace rhi = tynima::rhi;

namespace {

// The smallest useful MSL pair; the driver compiles it at runtime.
constexpr const char* kMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 position [[position]]; };
constant float2 kPositions[3] = { float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.0, 0.5) };
vertex VSOut vs_main(uint vid [[vertex_id]]) {
    VSOut out;
    out.position = float4(kPositions[vid], 0.0, 1.0);
    return out;
}
fragment float4 fs_main(VSOut in [[stage_in]]) { return float4(1.0, 0.5, 0.0, 1.0); }
)";

// A vertex-buffer pipeline: positions from [[stage_in]], a pushed uniform at [[buffer(0)]].
constexpr const char* kMeshMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { float3 position [[attribute(0)]]; float3 color [[attribute(1)]]; };
struct Uniforms { float4x4 transform; };
struct VSOut { float4 position [[position]]; float3 color; };
vertex VSOut vs_main(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {
    VSOut out;
    out.position = u.transform * float4(in.position, 1.0);
    out.color = in.color;
    return out;
}
fragment float4 fs_main(VSOut in [[stage_in]]) { return float4(in.color, 1.0); }
fragment float4 fs_textured(VSOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                            sampler s [[sampler(0)]]) {
    return tex.sample(s, in.position.xy * 0.01);
}
)";

struct TestVertex {
    float position[3];
    float color[3];
};

// SDL's GPU backends hang off the video driver, and the dummy driver has no
// Metal/Vulkan/D3D surfaces — so GPU tests need the real driver. Where it
// cannot start (no display: the build sandbox, some CI), the tests degrade to
// checking that failure is explained rather than silent.
struct Session {
    bool real_driver = false;

    Session() {
        real_driver = platform::init();
        if (!real_driver) {
            platform::shutdown();
            REQUIRE(platform::init({.headless = true}));
        }
    }
    ~Session() { platform::shutdown(); }
};

// Every GPU test runs on every backend the platform has: SDL GPU and, on
// Apple, the native Metal one — the same calls, the same expectations.
constexpr rhi::Backend kBackends[] = {rhi::Backend::SdlGpu, rhi::Backend::Metal};

std::unique_ptr<rhi::Device> make_device(rhi::Backend backend) {
    auto device = rhi::Device::create({.backend = backend, .debug = false});
    if (device == nullptr) {
        MESSAGE("skipped ", std::string(rhi::backend_name(backend)), ": ",
                std::string(platform::last_error()));
    }
    return device;
}

rhi::ShaderDesc msl_shader(rhi::ShaderStage stage, const char* entry_point, const char* source = kMsl) {
    return {.stage = stage,
            .format = rhi::ShaderFormat::Msl,
            .code = source,
            .code_size = std::strlen(source),
            .entry_point = entry_point};
}

} // namespace

TEST_CASE("mip level counts") {
    CHECK(rhi::mip_level_count(1, 1) == 1);
    CHECK(rhi::mip_level_count(2, 2) == 2);
    CHECK(rhi::mip_level_count(3, 2) == 2);
    CHECK(rhi::mip_level_count(256, 256) == 9);
    CHECK(rhi::mip_level_count(2048, 1024) == 12);
    CHECK(rhi::is_depth_format(rhi::TextureFormat::Depth32Float));
    CHECK_FALSE(rhi::is_depth_format(rhi::TextureFormat::Rgba8Srgb));
    CHECK(rhi::bytes_per_pixel(rhi::TextureFormat::Rgba8Srgb) == 4);
    CHECK(rhi::bytes_per_pixel(rhi::TextureFormat::Rgba16Float) == 8);
    CHECK(rhi::bytes_per_pixel(rhi::TextureFormat::Depth16) == 2);
    CHECK(rhi::bytes_per_pixel(rhi::TextureFormat::Depth32Float) == 4);
}

TEST_CASE("shader format names") {
    CHECK(std::strcmp(rhi::shader_format_name(rhi::ShaderFormat::Msl), "MSL") == 0);
    CHECK(std::strcmp(rhi::shader_format_name(rhi::ShaderFormat::SpirV), "SPIR-V") == 0);
    CHECK(std::strcmp(rhi::shader_format_name(rhi::ShaderFormat::Dxil), "DXIL") == 0);
}

TEST_CASE("a device is created, or the failure is explained") {
    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                CHECK(platform::last_error()[0] != '\0');
                WARN_MESSAGE(!session.real_driver,
                             "real video driver but no GPU device: ", platform::last_error());
                continue;
            }
            CHECK(std::strlen(device->backend_name()) > 0);
            CHECK(device->shader_format() == rhi::ShaderFormat::Msl);
            MESSAGE("backend: ", std::string(device->backend_name()));
        }
    }
}

TEST_CASE("runtime MSL compilation, a pipeline, and three frames") {
    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                continue;
            }

            // Shaders compile without a window attached.
            const rhi::ShaderHandle vs =
                device->create_shader(msl_shader(rhi::ShaderStage::Vertex, "vs_main"));
            const rhi::ShaderHandle fs =
                device->create_shader(msl_shader(rhi::ShaderStage::Fragment, "fs_main"));
            REQUIRE_MESSAGE(static_cast<bool>(vs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(fs), platform::last_error());

            // A broken shader fails cleanly, not loudly.
            CHECK(
                !device->create_shader(msl_shader(rhi::ShaderStage::Vertex, "vs_main", "this is not metal")));

            // A pipeline is built for its target formats: none at all is refused.
            CHECK(!device->create_graphics_pipeline({.vertex_shader = vs, .fragment_shader = fs}));
            CHECK(platform::last_error()[0] != '\0');
            // An offscreen target needs no window.
            rhi::GraphicsPipelineDesc offscreen{.vertex_shader = vs, .fragment_shader = fs};
            offscreen.color_formats[0] = rhi::TextureFormat::Rgba16Float;
            offscreen.color_target_count = 1;
            const rhi::PipelineHandle offscreen_pipeline = device->create_graphics_pipeline(offscreen);
            CHECK_MESSAGE(static_cast<bool>(offscreen_pipeline), platform::last_error());
            device->destroy_graphics_pipeline(offscreen_pipeline);

            auto window = platform::Window::create({.title = "tynima rhi test", .width = 320, .height = 240});
            REQUIRE(window != nullptr);
            REQUIRE_MESSAGE(device->attach_window(*window), platform::last_error());
            MESSAGE("swapchain format: ", std::string(rhi::texture_format_name(device->swapchain_format())));
            CHECK(!rhi::is_depth_format(device->swapchain_format()));

            rhi::GraphicsPipelineDesc desc{.vertex_shader = vs, .fragment_shader = fs};
            desc.color_formats[0] = device->swapchain_format();
            desc.color_target_count = 1;
            const rhi::PipelineHandle pipeline = device->create_graphics_pipeline(desc);
            REQUIRE_MESSAGE(static_cast<bool>(pipeline), platform::last_error());
            device->destroy_shader(vs); // the pipeline keeps what it needs
            device->destroy_shader(fs);

            for (int i = 0; i < 3; ++i) {
                auto frame = device->begin_frame();
                REQUIRE_MESSAGE(frame.has_value(), platform::last_error());
                if (frame->has_swapchain_image()) {
                    CHECK(frame->width() >= 320);
                    CHECK(frame->height() >= 240);
                    auto pass = frame->begin_swapchain_pass({.r = 0.1f, .g = 0.1f, .b = 0.1f});
                    REQUIRE(pass.has_value());
                    pass->bind_pipeline(pipeline);
                    pass->draw(3);
                    pass->end();
                }
                frame->submit();
            }

            device->destroy_graphics_pipeline(pipeline);
            device.reset(); // GPU objects go before the window they present to
            window.reset();
        }
    }
}

TEST_CASE("buffers, a depth texture, and an indexed draw with a vertex layout") {
    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                continue;
            }

            // Buffers upload without a window; the bounds check refuses an overrun.
            const TestVertex vertices[] = {{{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
                                           {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
                                           {{0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
            const std::uint32_t indices[] = {0, 1, 2};
            const rhi::BufferHandle vb =
                device->create_buffer_with_data(rhi::BufferUsage::Vertex, vertices, sizeof(vertices));
            const rhi::BufferHandle ib =
                device->create_buffer_with_data(rhi::BufferUsage::Index, indices, sizeof(indices));
            REQUIRE_MESSAGE(static_cast<bool>(vb), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(ib), platform::last_error());
            CHECK_FALSE(
                device->upload_buffer(ib, indices, sizeof(indices), 8)); // 12 bytes at offset 8 into 12
            CHECK(!device->create_buffer({.usage = rhi::BufferUsage::Vertex, .size = 0}));

            auto window = platform::Window::create({.title = "tynima rhi test", .width = 320, .height = 240});
            REQUIRE(window != nullptr);
            REQUIRE_MESSAGE(device->attach_window(*window), platform::last_error());

            rhi::TextureHandle depth = device->create_texture(
                {.format = device->preferred_depth_format(), .width = 320, .height = 240});
            REQUIRE_MESSAGE(static_cast<bool>(depth), platform::last_error());
            CHECK(device->texture_extent(depth).width == 320);
            CHECK(!device->create_texture({.width = 0, .height = 0}));

            const rhi::VertexAttribute attributes[] = {{0, rhi::VertexFormat::Float3, 0},
                                                       {1, rhi::VertexFormat::Float3, sizeof(float) * 3}};
            rhi::ShaderDesc vs_desc = msl_shader(rhi::ShaderStage::Vertex, "vs_main", kMeshMsl);
            vs_desc.num_uniform_buffers = 1;
            const rhi::ShaderHandle vs = device->create_shader(vs_desc);
            const rhi::ShaderHandle fs =
                device->create_shader(msl_shader(rhi::ShaderStage::Fragment, "fs_main", kMeshMsl));
            REQUIRE_MESSAGE(static_cast<bool>(vs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(fs), platform::last_error());
            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader = vs;
            desc.fragment_shader = fs;
            desc.vertex_layout = {sizeof(TestVertex), attributes, 2};
            desc.cull = rhi::CullMode::Back;
            desc.depth = {.test = true, .write = true, .compare = rhi::CompareOp::Greater};
            desc.color_formats[0] = device->swapchain_format();
            desc.color_target_count = 1;
            desc.depth_format = device->preferred_depth_format();
            const rhi::PipelineHandle pipeline = device->create_graphics_pipeline(desc);
            REQUIRE_MESSAGE(static_cast<bool>(pipeline), platform::last_error());
            device->destroy_shader(vs);
            device->destroy_shader(fs);

            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            for (int i = 0; i < 3; ++i) {
                auto frame = device->begin_frame();
                REQUIRE_MESSAGE(frame.has_value(), platform::last_error());
                if (frame->has_swapchain_image()) {
                    // The swapchain may differ from 320x240 in pixels on a HiDPI display;
                    // the depth texture must match it exactly.
                    rhi::TextureHandle frame_depth = depth;
                    const rhi::Extent2D extent = device->texture_extent(depth);
                    if (extent.width != frame->width() || extent.height != frame->height()) {
                        device->destroy_texture(depth);
                        depth = frame_depth =
                            device->create_texture({.format = device->preferred_depth_format(),
                                                    .width = frame->width(),
                                                    .height = frame->height()});
                        REQUIRE(static_cast<bool>(depth));
                    }
                    auto pass =
                        frame->begin_swapchain_pass({.r = 0.1f, .g = 0.1f, .b = 0.1f}, frame_depth, 0.0f);
                    REQUIRE(pass.has_value());
                    pass->bind_pipeline(pipeline);
                    pass->push_vertex_uniforms(0, identity, sizeof(identity));
                    pass->bind_vertex_buffer(vb);
                    pass->bind_index_buffer(ib, rhi::IndexType::Uint32);
                    pass->draw_indexed(3);
                    pass->end();
                }
                frame->submit();
            }

            device->destroy_graphics_pipeline(pipeline);
            device->destroy_texture(depth);
            device->destroy_buffer(vb);
            device->destroy_buffer(ib);

            // Handles outlive their objects harmlessly: the use-after-free is refused.
            CHECK_FALSE(device->valid(vb));
            CHECK_FALSE(device->upload_buffer(vb, indices, sizeof(indices)));
            CHECK(device->texture_extent(depth).width == 0);
            device->destroy_buffer(vb); // a second destroy is a no-op
            const rhi::Device::ResourceCounts counts = device->resource_counts();
            CHECK(counts.buffers == 0);
            CHECK(counts.textures == 0);
            CHECK(counts.pipelines == 0);

            device.reset();
            window.reset();
        }
    }
}

TEST_CASE("sampled textures: upload, GPU mipmaps, samplers, and a textured draw") {
    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                continue;
            }

            // A 2x2 sRGB texture with its full (2-level) chain generated on the GPU.
            const std::uint8_t pixels[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                                             0,   0, 255, 255, 255, 255, 255, 255};
            const rhi::TextureHandle texture =
                device->create_texture_with_data(rhi::TextureFormat::Rgba8Srgb, 2, 2, pixels, 16, true);
            REQUIRE_MESSAGE(static_cast<bool>(texture), platform::last_error());
            CHECK(device->texture_extent(texture).width == 2);

            // Wrong byte counts and depth uploads are refused up front.
            const rhi::TextureHandle single =
                device->create_texture({.format = rhi::TextureFormat::Rgba8Unorm, .width = 2, .height = 2});
            REQUIRE(static_cast<bool>(single));
            CHECK_FALSE(device->upload_texture(single, pixels, 15));
            CHECK_FALSE(device->upload_texture(single, pixels, 16, 1)); // no such level
            CHECK_FALSE(device->generate_mipmaps(single));              // single level
            CHECK(!device->create_texture_with_data(rhi::TextureFormat::Depth32Float, 2, 2, pixels, 16));
            device->destroy_texture(single);

            const rhi::SamplerHandle sampler = device->create_sampler({.max_anisotropy = 8.0f});
            REQUIRE_MESSAGE(static_cast<bool>(sampler), platform::last_error());

            auto window = platform::Window::create({.title = "tynima rhi test", .width = 320, .height = 240});
            REQUIRE(window != nullptr);
            REQUIRE_MESSAGE(device->attach_window(*window), platform::last_error());
            MESSAGE("swapchain linear: ", device->swapchain_is_linear());

            const TestVertex vertices[] = {{{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
                                           {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
                                           {{0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
            const rhi::BufferHandle vb =
                device->create_buffer_with_data(rhi::BufferUsage::Vertex, vertices, sizeof(vertices));
            REQUIRE(static_cast<bool>(vb));
            const rhi::VertexAttribute attributes[] = {{0, rhi::VertexFormat::Float3, 0},
                                                       {1, rhi::VertexFormat::Float3, sizeof(float) * 3}};
            rhi::ShaderDesc vs_desc = msl_shader(rhi::ShaderStage::Vertex, "vs_main", kMeshMsl);
            vs_desc.num_uniform_buffers = 1;
            rhi::ShaderDesc fs_desc = msl_shader(rhi::ShaderStage::Fragment, "fs_textured", kMeshMsl);
            fs_desc.num_samplers = 1;
            const rhi::ShaderHandle vs = device->create_shader(vs_desc);
            const rhi::ShaderHandle fs = device->create_shader(fs_desc);
            REQUIRE_MESSAGE(static_cast<bool>(vs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(fs), platform::last_error());
            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader = vs;
            desc.fragment_shader = fs;
            desc.vertex_layout = {sizeof(TestVertex), attributes, 2};
            desc.color_formats[0] = device->swapchain_format();
            desc.color_target_count = 1;
            const rhi::PipelineHandle pipeline = device->create_graphics_pipeline(desc);
            REQUIRE_MESSAGE(static_cast<bool>(pipeline), platform::last_error());
            device->destroy_shader(vs);
            device->destroy_shader(fs);

            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            for (int i = 0; i < 2; ++i) {
                auto frame = device->begin_frame();
                REQUIRE_MESSAGE(frame.has_value(), platform::last_error());
                if (frame->has_swapchain_image()) {
                    auto pass = frame->begin_swapchain_pass({});
                    REQUIRE(pass.has_value());
                    pass->bind_pipeline(pipeline);
                    pass->push_vertex_uniforms(0, identity, sizeof(identity));
                    pass->bind_fragment_texture(0, texture, sampler);
                    pass->bind_vertex_buffer(vb);
                    pass->draw(3);
                    pass->end();
                }
                frame->submit();
            }

            device->destroy_graphics_pipeline(pipeline);
            device->destroy_buffer(vb);
            device->destroy_sampler(sampler);
            device->destroy_texture(texture);
            device.reset();
            window.reset();
        }
    }
}

TEST_CASE("offscreen passes: draw into a texture, then sample it into the swapchain") {
    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                continue;
            }
            auto window = platform::Window::create({.title = "tynima rhi test", .width = 320, .height = 240});
            REQUIRE(window != nullptr);
            REQUIRE_MESSAGE(device->attach_window(*window), platform::last_error());

            // The first pass draws a flat triangle into an HDR texture; the second
            // samples that texture onto the swapchain.
            const rhi::ShaderHandle vs =
                device->create_shader(msl_shader(rhi::ShaderStage::Vertex, "vs_main"));
            const rhi::ShaderHandle fs =
                device->create_shader(msl_shader(rhi::ShaderStage::Fragment, "fs_main"));
            REQUIRE_MESSAGE(static_cast<bool>(vs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(fs), platform::last_error());
            rhi::GraphicsPipelineDesc offscreen{.vertex_shader = vs, .fragment_shader = fs};
            offscreen.color_formats[0] = rhi::TextureFormat::Rgba16Float;
            offscreen.color_target_count = 1;
            offscreen.depth_format = device->preferred_depth_format();
            const rhi::PipelineHandle draw_pipeline = device->create_graphics_pipeline(offscreen);
            REQUIRE_MESSAGE(static_cast<bool>(draw_pipeline), platform::last_error());
            device->destroy_shader(vs);
            device->destroy_shader(fs);

            const rhi::VertexAttribute attributes[2] = {{0, rhi::VertexFormat::Float3, 0},
                                                        {1, rhi::VertexFormat::Float3, 12}};
            rhi::ShaderDesc vs_desc = msl_shader(rhi::ShaderStage::Vertex, "vs_main", kMeshMsl);
            vs_desc.num_uniform_buffers = 1;
            rhi::ShaderDesc fs_desc = msl_shader(rhi::ShaderStage::Fragment, "fs_textured", kMeshMsl);
            fs_desc.num_samplers = 1;
            const rhi::ShaderHandle blit_vs = device->create_shader(vs_desc);
            const rhi::ShaderHandle blit_fs = device->create_shader(fs_desc);
            REQUIRE_MESSAGE(static_cast<bool>(blit_vs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(blit_fs), platform::last_error());
            rhi::GraphicsPipelineDesc blit{.vertex_shader = blit_vs, .fragment_shader = blit_fs};
            blit.vertex_layout = {sizeof(TestVertex), attributes, 2};
            blit.color_formats[0] = device->swapchain_format();
            blit.color_target_count = 1;
            const rhi::PipelineHandle blit_pipeline = device->create_graphics_pipeline(blit);
            REQUIRE_MESSAGE(static_cast<bool>(blit_pipeline), platform::last_error());
            device->destroy_shader(blit_vs);
            device->destroy_shader(blit_fs);

            const TestVertex vertices[3] = {{{-1.0f, -1.0f, 0.0f}, {1, 1, 1}},
                                            {{3.0f, -1.0f, 0.0f}, {1, 1, 1}},
                                            {{-1.0f, 3.0f, 0.0f}, {1, 1, 1}}};
            const rhi::BufferHandle vb =
                device->create_buffer_with_data(rhi::BufferUsage::Vertex, vertices, sizeof vertices);
            REQUIRE(static_cast<bool>(vb));
            const rhi::SamplerHandle sampler = device->create_sampler({});
            REQUIRE(static_cast<bool>(sampler));
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

            rhi::TextureHandle hdr, depth;
            for (int i = 0; i < 2; ++i) {
                auto frame = device->begin_frame();
                REQUIRE_MESSAGE(frame.has_value(), platform::last_error());
                if (!frame->has_swapchain_image()) {
                    frame->submit();
                    continue;
                }
                CHECK(static_cast<bool>(frame->swapchain_texture()));
                CHECK(device->texture_extent(frame->swapchain_texture()).width == frame->width());
                if (!hdr) {
                    const rhi::TextureUsage target =
                        rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled;
                    hdr = device->create_texture({.format = rhi::TextureFormat::Rgba16Float,
                                                  .width = frame->width(),
                                                  .height = frame->height(),
                                                  .usage = target});
                    depth = device->create_texture({.format = device->preferred_depth_format(),
                                                    .width = frame->width(),
                                                    .height = frame->height()});
                    REQUIRE_MESSAGE(static_cast<bool>(hdr), platform::last_error());
                    REQUIRE_MESSAGE(static_cast<bool>(depth), platform::last_error());
                }

                // Misuse is refused with a reason, not a crash: no attachments, a
                // depth texture as a color target, sizes that differ.
                CHECK(!frame->begin_pass({}));
                rhi::RenderPassDesc wrong{.name = "wrong"};
                wrong.colors[0] = {.texture = depth};
                wrong.color_count = 1;
                CHECK(!frame->begin_pass(wrong));
                CHECK(platform::last_error()[0] != '\0');

                rhi::RenderPassDesc scene{.name = "scene"};
                scene.colors[0] = {.texture = hdr, .load = rhi::LoadOp::Clear, .store = rhi::StoreOp::Store};
                scene.color_count = 1;
                scene.depth = rhi::DepthAttachment{.texture = depth};
                {
                    auto pass = frame->begin_pass(scene);
                    REQUIRE_MESSAGE(pass.has_value(), platform::last_error());
                    pass->bind_pipeline(draw_pipeline);
                    pass->draw(3);
                    pass->end();
                }
                rhi::RenderPassDesc present{.name = "present"};
                present.colors[0] = {.texture = frame->swapchain_texture(), .load = rhi::LoadOp::DontCare};
                present.color_count = 1;
                {
                    auto pass = frame->begin_pass(present);
                    REQUIRE_MESSAGE(pass.has_value(), platform::last_error());
                    pass->bind_pipeline(blit_pipeline);
                    pass->push_vertex_uniforms(0, identity, sizeof(identity));
                    pass->bind_fragment_texture(0, hdr, sampler);
                    pass->bind_vertex_buffer(vb);
                    pass->draw(3);
                    pass->end();
                }
                const rhi::TextureHandle swapchain = frame->swapchain_texture();
                frame->submit();
                CHECK(!device->valid(swapchain)); // gone with the frame
            }

            device->destroy_graphics_pipeline(draw_pipeline);
            device->destroy_graphics_pipeline(blit_pipeline);
            device->destroy_buffer(vb);
            device->destroy_sampler(sampler);
            device->destroy_texture(hdr);
            device->destroy_texture(depth);
            CHECK(device->resource_counts().textures == 0);
            device.reset();
            window.reset();
        }
    }
}

TEST_CASE("compute: a kernel reads one storage buffer, writes another, and the result comes back") {
    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                continue;
            }
            // Doubles every value, plus the uniform's offset: [[buffer(0)]] is the
            // uniform, then the read-only storage buffer, then the read-write one.
            constexpr const char* kKernel = R"(
#include <metal_stdlib>
using namespace metal;
struct Params { uint count; uint offset; uint pad0; uint pad1; };
kernel void cs_main(constant Params& p [[buffer(0)]], const device uint* in [[buffer(1)]],
                    device uint* out [[buffer(2)]], uint id [[thread_position_in_grid]]) {
    if (id < p.count) {
        out[id] = in[id] * 2 + p.offset;
    }
}
)";
            const rhi::ComputePipelineHandle pipeline =
                device->create_compute_pipeline({.code = kKernel,
                                                 .code_size = std::strlen(kKernel),
                                                 .entry_point = "cs_main",
                                                 .num_uniform_buffers = 1,
                                                 .num_readonly_storage_buffers = 1,
                                                 .num_readwrite_storage_buffers = 1,
                                                 .threads_x = 64});
            REQUIRE_MESSAGE(static_cast<bool>(pipeline), platform::last_error());
            CHECK(device->valid(pipeline));
            CHECK(device->resource_counts().compute_pipelines == 1);

            constexpr std::uint32_t kCount = 1000;
            std::uint32_t values[kCount];
            for (std::uint32_t i = 0; i < kCount; ++i) {
                values[i] = i * 3;
            }
            const rhi::BufferDesc storage{.usage = rhi::BufferUsage::Storage, .size = sizeof values};
            const rhi::BufferHandle in = device->create_buffer(storage);
            const rhi::BufferHandle out = device->create_buffer(storage);
            REQUIRE(static_cast<bool>(in));
            REQUIRE(static_cast<bool>(out));

            // Two frames: the input streamed in, the kernel run; no window needed.
            for (std::uint32_t offset = 1; offset <= 2; ++offset) {
                auto frame = device->begin_frame();
                REQUIRE_MESSAGE(frame.has_value(), platform::last_error());
                CHECK_FALSE(frame->has_swapchain_image());
                REQUIRE_MESSAGE(frame->write_buffer(in, values, sizeof values), platform::last_error());
                rhi::ComputePassDesc desc{.name = "double"};
                desc.writes[0] = out;
                desc.write_count = 1;
                auto pass = frame->begin_compute_pass(desc);
                REQUIRE_MESSAGE(pass.has_value(), platform::last_error());
                pass->bind_pipeline(pipeline);
                pass->bind_storage_buffer(0, in);
                const std::uint32_t params[4] = {kCount, offset, 0, 0};
                pass->push_uniforms(0, params, sizeof params);
                pass->dispatch((kCount + 63) / 64);
                pass->end();
                frame->submit();

                std::uint32_t result[kCount];
                REQUIRE_MESSAGE(device->download_buffer(out, result, sizeof result), platform::last_error());
                std::uint32_t wrong = 0;
                for (std::uint32_t i = 0; i < kCount; ++i) {
                    wrong += result[i] == values[i] * 2 + offset ? 0 : 1;
                }
                CHECK(wrong == 0);
            }
            // A partial write, at an offset.
            {
                const std::uint32_t patch[2] = {7, 9};
                auto frame = device->begin_frame();
                REQUIRE(frame.has_value());
                REQUIRE(frame->write_buffer(out, patch, sizeof patch, 8));
                CHECK_FALSE(frame->write_buffer(out, patch, sizeof patch, sizeof values)); // past the end
                frame->submit();
                std::uint32_t head[4];
                REQUIRE(device->download_buffer(out, head, sizeof head));
                CHECK(head[0] == 0 * 6 + 2);
                CHECK(head[1] == 1 * 6 + 2);
                CHECK(head[2] == 7);
                CHECK(head[3] == 9);
            }

            device->destroy_buffer(in);
            device->destroy_buffer(out);
            device->destroy_compute_pipeline(pipeline);
            CHECK(device->resource_counts().compute_pipelines == 0);
            CHECK(device->resource_counts().buffers == 0);
        }
    }
}

TEST_CASE("an attachment written and read back in one pass, through the tile, never stored") {
    // The fused-pass idea at its smallest: one draw fills a scratch
    // attachment, a second reads it as a [[color(n)]] input in the same
    // pass and writes the result to the attachment that is kept. The scratch
    // is memoryless where the backend can make it so, and never stored
    // anywhere.
    constexpr const char* kFusedMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 position [[position]]; };
constant float2 kCorners[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
vertex VSOut vs_main(uint vid [[vertex_id]]) {
    VSOut out;
    out.position = float4(kCorners[vid], 0.0, 1.0);
    return out;
}
struct ScratchOut { float4 scratch [[color(1)]]; };
fragment ScratchOut fs_write(VSOut in [[stage_in]]) {
    ScratchOut out;
    out.scratch = float4(1.0, 0.5, 0.0, 1.0);
    return out;
}
struct ScratchIn { float4 scratch [[color(1)]]; };
fragment float4 fs_read(VSOut in [[stage_in]], ScratchIn g) { return g.scratch * 0.5 + 0.25; }
)";
    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                continue;
            }
            MESSAGE(std::string(rhi::backend_name(backend)), device->supports_memoryless()
                                                                 ? ": memoryless attachments"
                                                                 : ": no memoryless attachments");
            const rhi::ShaderHandle vs =
                device->create_shader(msl_shader(rhi::ShaderStage::Vertex, "vs_main", kFusedMsl));
            const rhi::ShaderHandle write_fs =
                device->create_shader(msl_shader(rhi::ShaderStage::Fragment, "fs_write", kFusedMsl));
            const rhi::ShaderHandle read_fs =
                device->create_shader(msl_shader(rhi::ShaderStage::Fragment, "fs_read", kFusedMsl));
            REQUIRE_MESSAGE(static_cast<bool>(vs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(write_fs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(read_fs), platform::last_error());
            rhi::GraphicsPipelineDesc write{.vertex_shader = vs, .fragment_shader = write_fs};
            write.color_formats[0] = rhi::TextureFormat::Rgba8Unorm;
            write.color_formats[1] = rhi::TextureFormat::Rgba8Unorm;
            write.color_target_count = 2;
            write.color_write[0] = false; // this draw touches the scratch only
            rhi::GraphicsPipelineDesc read = write;
            read.fragment_shader = read_fs;
            read.color_write[0] = true;
            read.color_write[1] = false; // and this one the kept attachment only
            const rhi::PipelineHandle write_pipeline = device->create_graphics_pipeline(write);
            const rhi::PipelineHandle read_pipeline = device->create_graphics_pipeline(read);
            REQUIRE_MESSAGE(static_cast<bool>(write_pipeline), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(read_pipeline), platform::last_error());
            device->destroy_shader(vs);
            device->destroy_shader(write_fs);
            device->destroy_shader(read_fs);

            const rhi::TextureHandle kept = device->create_texture({.format = rhi::TextureFormat::Rgba8Unorm,
                                                                    .width = 8,
                                                                    .height = 8,
                                                                    .usage = rhi::TextureUsage::ColorTarget});
            const rhi::TextureHandle scratch =
                device->create_texture({.format = rhi::TextureFormat::Rgba8Unorm,
                                        .width = 8,
                                        .height = 8,
                                        .usage = rhi::TextureUsage::ColorTarget,
                                        .memoryless = device->supports_memoryless()});
            REQUIRE_MESSAGE(static_cast<bool>(kept), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(scratch), platform::last_error());
            // A memoryless texture cannot be sampled, so asking for that is refused.
            if (device->supports_memoryless()) {
                CHECK_FALSE(device->create_texture(
                    {.format = rhi::TextureFormat::Rgba8Unorm,
                     .width = 8,
                     .height = 8,
                     .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
                     .memoryless = true}));
            }

            auto frame = device->begin_frame();
            REQUIRE_MESSAGE(frame.has_value(), platform::last_error());
            rhi::RenderPassDesc desc{.name = "fused"};
            desc.colors[0] = {.texture = kept, .load = rhi::LoadOp::Clear, .store = rhi::StoreOp::Store};
            desc.colors[1] = {
                .texture = scratch, .load = rhi::LoadOp::DontCare, .store = rhi::StoreOp::DontCare};
            desc.color_count = 2;
            {
                auto pass = frame->begin_pass(desc);
                REQUIRE_MESSAGE(pass.has_value(), platform::last_error());
                pass->bind_pipeline(write_pipeline);
                pass->draw(3);
                pass->bind_pipeline(read_pipeline);
                pass->draw(3);
                pass->end();
            }
            frame->submit();

            std::uint8_t pixels[8 * 8 * 4];
            REQUIRE_MESSAGE(device->download_texture(kept, pixels, sizeof pixels), platform::last_error());
            // (1, 0.5, 0, 1) * 0.5 + 0.25 = (0.75, 0.5, 0.25, 0.75): 191, 128, 64, 191, give or take one.
            int wrong = 0;
            for (int i = 0; i < 64; ++i) {
                const std::uint8_t* p = pixels + i * 4;
                wrong += std::abs(int{p[0]} - 191) <= 1 && std::abs(int{p[1]} - 128) <= 1 &&
                                 std::abs(int{p[2]} - 64) <= 1 && std::abs(int{p[3]} - 191) <= 1
                             ? 0
                             : 1;
            }
            MESSAGE("pixel 0: ", int{pixels[0]}, " ", int{pixels[1]}, " ", int{pixels[2]}, " ",
                    int{pixels[3]});
            CHECK(wrong == 0);

            device->destroy_texture(kept);
            device->destroy_texture(scratch);
            device->destroy_graphics_pipeline(write_pipeline);
            device->destroy_graphics_pipeline(read_pipeline);
        }
    }
}

TEST_CASE("alpha blending, a scissor rectangle, and packed byte colours") {
    // What a UI draw needs: a quad of Float2 positions and Ubyte4Norm
    // colours, blended over what the target holds, clipped to a rectangle.
    // The target is cleared red; a half-transparent white quad covers it
    // all, but the scissor lets only the left half through.
    constexpr const char* kQuadMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { float2 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct VSOut { float4 position [[position]]; float4 color; };
vertex VSOut vs_main(VertexIn in [[stage_in]]) {
    VSOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.color = in.color;
    return out;
}
fragment float4 fs_main(VSOut in [[stage_in]]) { return in.color; }
)";
    struct QuadVertex {
        float x, y;
        std::uint8_t r, g, b, a;
    };
    static_assert(sizeof(QuadVertex) == 12);
    const QuadVertex vertices[4] = {{-1.0f, -1.0f, 255, 255, 255, 128},
                                    {1.0f, -1.0f, 255, 255, 255, 128},
                                    {1.0f, 1.0f, 255, 255, 255, 128},
                                    {-1.0f, 1.0f, 255, 255, 255, 128}};
    const std::uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    const rhi::VertexAttribute attributes[2] = {
        {.location = 0, .format = rhi::VertexFormat::Float2, .offset = 0},
        {.location = 1, .format = rhi::VertexFormat::Ubyte4Norm, .offset = 8}};

    Session session;
    for (const rhi::Backend backend : kBackends) {
        SUBCASE(rhi::backend_name(backend)) {
            auto device = make_device(backend);
            if (device == nullptr) {
                continue;
            }
            const rhi::ShaderHandle vs =
                device->create_shader(msl_shader(rhi::ShaderStage::Vertex, "vs_main", kQuadMsl));
            const rhi::ShaderHandle fs =
                device->create_shader(msl_shader(rhi::ShaderStage::Fragment, "fs_main", kQuadMsl));
            REQUIRE_MESSAGE(static_cast<bool>(vs), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(fs), platform::last_error());
            rhi::GraphicsPipelineDesc desc{.vertex_shader = vs, .fragment_shader = fs};
            desc.vertex_layout = {
                .stride = sizeof(QuadVertex), .attributes = attributes, .attribute_count = 2};
            desc.color_formats[0] = rhi::TextureFormat::Rgba8Unorm;
            desc.color_target_count = 1;
            desc.blend[0] = rhi::BlendMode::Alpha;
            const rhi::PipelineHandle pipeline = device->create_graphics_pipeline(desc);
            REQUIRE_MESSAGE(static_cast<bool>(pipeline), platform::last_error());
            device->destroy_shader(vs);
            device->destroy_shader(fs);

            const rhi::BufferHandle vertex_buffer =
                device->create_buffer_with_data(rhi::BufferUsage::Vertex, vertices, sizeof vertices);
            const rhi::BufferHandle index_buffer =
                device->create_buffer_with_data(rhi::BufferUsage::Index, indices, sizeof indices);
            const rhi::TextureHandle target =
                device->create_texture({.format = rhi::TextureFormat::Rgba8Unorm,
                                        .width = 8,
                                        .height = 4,
                                        .usage = rhi::TextureUsage::ColorTarget});
            REQUIRE_MESSAGE(static_cast<bool>(vertex_buffer), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(index_buffer), platform::last_error());
            REQUIRE_MESSAGE(static_cast<bool>(target), platform::last_error());

            auto frame = device->begin_frame();
            REQUIRE_MESSAGE(frame.has_value(), platform::last_error());
            rhi::RenderPassDesc pass_desc{.name = "blend"};
            pass_desc.colors[0] = {.texture = target,
                                   .load = rhi::LoadOp::Clear,
                                   .store = rhi::StoreOp::Store,
                                   .clear = {1.0f, 0.0f, 0.0f, 1.0f}};
            pass_desc.color_count = 1;
            {
                auto pass = frame->begin_pass(pass_desc);
                REQUIRE_MESSAGE(pass.has_value(), platform::last_error());
                pass->bind_pipeline(pipeline);
                pass->bind_vertex_buffer(vertex_buffer);
                pass->bind_index_buffer(index_buffer, rhi::IndexType::Uint16);
                pass->set_scissor(0, 0, 4, 4);
                pass->draw_indexed(6);
                pass->set_scissor(100, 100, 100, 100); // past the target: this draws nothing at all
                pass->draw_indexed(6);
                pass->set_scissor(6, 0, 100, 100); // reaching past it: clipped to the last two columns
                pass->draw_indexed(6);
                pass->end();
            }
            frame->submit();

            std::uint8_t pixels[8 * 4 * 4];
            REQUIRE_MESSAGE(device->download_texture(target, pixels, sizeof pixels), platform::last_error());
            // Columns 0-3 and 6-7: red blended with half-transparent white =
            // (1, 0.5, 0.5, 1); columns 4 and 5: red, untouched.
            int wrong = 0;
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 8; ++x) {
                    const std::uint8_t* p = pixels + (y * 8 + x) * 4;
                    const int g = x < 4 || x >= 6 ? 128 : 0;
                    wrong += std::abs(int{p[0]} - 255) <= 1 && std::abs(int{p[1]} - g) <= 1 &&
                                     std::abs(int{p[2]} - g) <= 1 && std::abs(int{p[3]} - 255) <= 1
                                 ? 0
                                 : 1;
                }
            }
            MESSAGE("pixel (0,0): ", int{pixels[0]}, " ", int{pixels[1]}, " ", int{pixels[2]}, " ",
                    int{pixels[3]}, "; pixel (5,0): ", int{pixels[20]}, " ", int{pixels[21]}, " ",
                    int{pixels[22]}, " ", int{pixels[23]}, "; pixel (7,3): ", int{pixels[124]}, " ",
                    int{pixels[125]}, " ", int{pixels[126]}, " ", int{pixels[127]});
            CHECK(wrong == 0);

            device->destroy_texture(target);
            device->destroy_buffer(vertex_buffer);
            device->destroy_buffer(index_buffer);
            device->destroy_graphics_pipeline(pipeline);
        }
    }
}
