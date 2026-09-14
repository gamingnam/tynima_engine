#include <doctest/doctest.h>
#include <tynima/render/camera.h>
#include <tynima/render/mesh.h>

using namespace tynima;
using namespace tynima::math;

TEST_CASE("the default camera looks down -z from the origin") {
    render::Camera cam;
    CHECK(approx_equal(cam.forward(), -Vec3::unit_z()));
    CHECK(approx_equal(cam.right(), Vec3::unit_x()));
    CHECK(approx_equal(cam.up(), Vec3::unit_y()));
    CHECK(approx_equal(cam.view(), Mat4::identity()));
}

TEST_CASE("view() is the inverse of the camera pose") {
    render::Camera cam;
    cam.position = {1.0f, 2.0f, 3.0f};
    cam.rotation = Quat::from_axis_angle(normalize(Vec3{0.3f, 1.0f, 0.2f}), 0.8f);
    const Mat4 pose = trs(cam.position, cam.rotation, Vec3::one());
    CHECK(approx_equal(cam.view() * pose, Mat4::identity()));
    CHECK(approx_equal(cam.view(), inverse(pose), 1e-4f));
}

TEST_CASE("look_at faces the target and keeps the horizon level") {
    render::Camera cam;
    cam.position = {0.0f, 1.0f, 5.0f};
    cam.look_at(Vec3::zero());
    const Vec3 to_target = normalize(Vec3::zero() - cam.position);
    CHECK(approx_equal(cam.forward(), to_target));
    CHECK(cam.right().y == doctest::Approx(0.0f)); // level
    CHECK(cam.up().y > 0.0f);
    // The target ends up dead centre in front of the camera.
    const Vec3 v = transform_point(cam.view(), Vec3::zero());
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.y == doctest::Approx(0.0f));
    CHECK(v.z < 0.0f);

    // Every branch of the matrix->quaternion conversion: look in several directions.
    for (const Vec3 target : {Vec3{5.0f, 0.0f, 0.0f}, Vec3{-5.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 10.0f},
                              Vec3{0.0f, 5.0f, 0.1f}, Vec3{2.0f, -3.0f, -4.0f}}) {
        render::Camera c;
        c.look_at(target);
        CHECK(approx_equal(c.forward(), normalize(target), 1e-4f));
        CHECK(length(c.rotation) == doctest::Approx(1.0f));
    }
}

TEST_CASE("vertex layout describes Vertex exactly") {
    const rhi::VertexLayout& layout = render::vertex_layout();
    CHECK(layout.stride == sizeof(render::Vertex));
    CHECK(layout.attribute_count == 4);
    CHECK(layout.attributes[0].offset == 0);
    CHECK(layout.attributes[1].offset == 12);
    CHECK(layout.attributes[2].offset == 24);
    CHECK(layout.attributes[3].offset == 32);
    CHECK(layout.attributes[3].format == rhi::VertexFormat::Float4);
}

TEST_CASE("MeshData computes tangent frames from the UV layout") {
    render::MeshData data;
    // A quad in the xy plane facing +z, with u running along +x and v along +y.
    data.vertices = {{{-1.0f, -1.0f, 0.0f}, Vec3::unit_z(), {0.0f, 0.0f}, {}},
                     {{1.0f, -1.0f, 0.0f}, Vec3::unit_z(), {1.0f, 0.0f}, {}},
                     {{1.0f, 1.0f, 0.0f}, Vec3::unit_z(), {1.0f, 1.0f}, {}},
                     {{-1.0f, 1.0f, 0.0f}, Vec3::unit_z(), {0.0f, 1.0f}, {}}};
    data.indices = {0, 1, 2, 0, 2, 3};
    data.compute_tangents();
    for (const render::Vertex& v : data.vertices) {
        CHECK(approx_equal(v.tangent.xyz(), Vec3::unit_x()));
        CHECK(v.tangent.w == 1.0f);
    }

    // Mirror the UVs horizontally: the tangent flips and so does the handedness,
    // which keeps bitangent = cross(n, t) * w pointing along +v.
    for (render::Vertex& v : data.vertices) {
        v.uv.x = 1.0f - v.uv.x;
    }
    data.compute_tangents();
    for (const render::Vertex& v : data.vertices) {
        CHECK(approx_equal(v.tangent.xyz(), -Vec3::unit_x()));
        CHECK(v.tangent.w == -1.0f);
        CHECK(approx_equal(cross(v.normal, v.tangent.xyz()) * v.tangent.w, Vec3::unit_y()));
    }

    // Degenerate UVs (all zero): still a unit tangent perpendicular to the normal.
    for (render::Vertex& v : data.vertices) {
        v.uv = Vec2::zero();
    }
    data.compute_tangents();
    for (const render::Vertex& v : data.vertices) {
        CHECK(length(v.tangent.xyz()) == doctest::Approx(1.0f));
        CHECK(dot(v.tangent.xyz(), v.normal) == doctest::Approx(0.0f));
    }
}

TEST_CASE("MeshData computes bounds and normals") {
    render::MeshData data;
    // A quad in the xy plane, counter-clockwise from the front (+z).
    data.vertices = {{{-1.0f, -1.0f, 0.0f}, {}, {0.0f, 0.0f}, {}},
                     {{1.0f, -1.0f, 0.0f}, {}, {1.0f, 0.0f}, {}},
                     {{1.0f, 1.0f, 0.0f}, {}, {1.0f, 1.0f}, {}},
                     {{-1.0f, 1.0f, 0.5f}, {}, {0.0f, 1.0f}, {}}};
    data.indices = {0, 1, 2, 0, 2, 3};
    data.compute_bounds();
    CHECK(data.bounds_min == Vec3{-1.0f, -1.0f, 0.0f});
    CHECK(data.bounds_max == Vec3{1.0f, 1.0f, 0.5f});
    data.compute_normals();
    CHECK(approx_equal(data.vertices[1].normal, Vec3::unit_z())); // only in the flat triangle
    CHECK(data.vertices[0].normal.z > 0.9f);                       // shared, slightly tilted
    CHECK(length(data.vertices[3].normal) == doctest::Approx(1.0f));
}
