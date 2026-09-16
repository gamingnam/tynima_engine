// GJK for distance and closest points, EPA for penetration. Both work on
// the Minkowski difference A - B through the shapes' support mappings, and
// both keep every point's parents (the support points of A and B) so the
// answer comes back as points on the shapes, not only as a distance.
//
// GJK follows Erin Catto's formulation (GDC 2010, Box2D's b2Distance): the
// simplex is reduced each iteration to the smallest sub-simplex holding the
// closest point to the origin, with that point's barycentric weights, and
// the next support point is taken toward the origin from there. The
// tetrahedron case is the 3D addition: the origin is inside it, or the
// closest point is on one of the faces the origin can see.
//
// EPA keeps a convex polytope of Minkowski-difference points as a
// consistently wound triangle mesh. Each round takes the face nearest the
// origin, pushes a support point out through it, and re-stitches the mesh
// over the horizon that point can see. When the support point is no further
// out than the face, that face's normal and distance are the answer.
#include <tynima/physics/collision.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace tynima::physics {

namespace {

constexpr int kGjkMaxIterations = 40;
constexpr int kEpaMaxIterations = 64;
constexpr std::uint32_t kEpaMaxVertices = 4 + kEpaMaxIterations;
constexpr std::uint32_t kEpaMaxFaces = 512;
constexpr float kEpsilon = 1e-6f;
// GJK's tolerances scale with its simplex. A float carries about seven
// digits, so with support points ten metres out (a box resting on a 20 m
// floor) the closest point comes with a micron or so of noise, and a
// distance of a tenth of a micron means nothing there; between two small
// boxes the same rule is still well under a hundredth of a millimetre.
constexpr float kGjkRelativeTolerance = 1e-5f;

Simplex::Vertex minkowski_support(const Convex& a, const Convex& b, const Vec3& direction) noexcept {
    Simplex::Vertex v;
    v.wa = a.support(direction);
    v.wb = b.support(direction * -1.0f);
    v.w = v.wa - v.wb;
    return v;
}

Vec3 closest_point(const Simplex& s) noexcept {
    Vec3 p{0.0f};
    for (std::uint32_t i = 0; i < s.count; ++i) {
        p += s.v[i].w * s.v[i].weight;
    }
    return p;
}

// ------------------------------------------------------- simplex solvers

// The closest point on the segment v0-v1 to the origin, as weights.
void solve2(Simplex& s) noexcept {
    const Vec3 w1 = s.v[0].w;
    const Vec3 w2 = s.v[1].w;
    const Vec3 e12 = w2 - w1;
    const float d12_2 = -dot(w1, e12);
    if (d12_2 <= 0.0f) {
        s.v[0].weight = 1.0f; // before w1: w1 itself
        s.count = 1;
        return;
    }
    const float d12_1 = dot(w2, e12);
    if (d12_1 <= 0.0f) {
        s.v[0] = s.v[1]; // past w2: w2 itself
        s.v[0].weight = 1.0f;
        s.count = 1;
        return;
    }
    const float inv = 1.0f / (d12_1 + d12_2);
    s.v[0].weight = d12_1 * inv;
    s.v[1].weight = d12_2 * inv;
    s.count = 2;
}

// The closest point on the triangle v0-v1-v2 to the origin: a vertex, an
// edge or the face interior, by the origin's Voronoi region.
void solve3(Simplex& s) noexcept {
    const Vec3 w1 = s.v[0].w;
    const Vec3 w2 = s.v[1].w;
    const Vec3 w3 = s.v[2].w;

    const Vec3 e12 = w2 - w1;
    const float d12_1 = dot(w2, e12);
    const float d12_2 = -dot(w1, e12);
    const Vec3 e13 = w3 - w1;
    const float d13_1 = dot(w3, e13);
    const float d13_2 = -dot(w1, e13);
    const Vec3 e23 = w3 - w2;
    const float d23_1 = dot(w3, e23);
    const float d23_2 = -dot(w2, e23);

    const Vec3 n = cross(e12, e13);
    const float d123_1 = dot(cross(w2, w3), n);
    const float d123_2 = dot(cross(w3, w1), n);
    const float d123_3 = dot(cross(w1, w2), n);

    if (d12_2 <= 0.0f && d13_2 <= 0.0f) { // vertex 1
        s.v[0].weight = 1.0f;
        s.count = 1;
        return;
    }
    if (d12_1 > 0.0f && d12_2 > 0.0f && d123_3 <= 0.0f) { // edge 12
        const float inv = 1.0f / (d12_1 + d12_2);
        s.v[0].weight = d12_1 * inv;
        s.v[1].weight = d12_2 * inv;
        s.count = 2;
        return;
    }
    if (d13_1 > 0.0f && d13_2 > 0.0f && d123_2 <= 0.0f) { // edge 13
        const float inv = 1.0f / (d13_1 + d13_2);
        s.v[0].weight = d13_1 * inv;
        s.v[2].weight = d13_2 * inv;
        s.v[1] = s.v[2];
        s.count = 2;
        return;
    }
    if (d12_1 <= 0.0f && d23_2 <= 0.0f) { // vertex 2
        s.v[0] = s.v[1];
        s.v[0].weight = 1.0f;
        s.count = 1;
        return;
    }
    if (d13_1 <= 0.0f && d23_1 <= 0.0f) { // vertex 3
        s.v[0] = s.v[2];
        s.v[0].weight = 1.0f;
        s.count = 1;
        return;
    }
    if (d23_1 > 0.0f && d23_2 > 0.0f && d123_1 <= 0.0f) { // edge 23
        const float inv = 1.0f / (d23_1 + d23_2);
        s.v[1].weight = d23_1 * inv;
        s.v[2].weight = d23_2 * inv;
        s.v[0] = s.v[1];
        s.v[1] = s.v[2];
        s.count = 2;
        return;
    }
    const float sum = d123_1 + d123_2 + d123_3; // |n|^2, zero only for a degenerate triangle
    if (sum <= 0.0f) {
        s.count = 2; // collinear: the longest edge holds the answer
        solve2(s);
        return;
    }
    const float inv = 1.0f / sum;
    s.v[0].weight = d123_1 * inv;
    s.v[1].weight = d123_2 * inv;
    s.v[2].weight = d123_3 * inv;
    s.count = 3;
}

// True when the origin is inside the tetrahedron. Otherwise reduces the
// simplex to the closest of the faces the origin lies in front of.
bool solve4(Simplex& s) noexcept {
    // Each face with the vertex opposite it; normals are made to point away
    // from that vertex, i.e. outward.
    constexpr std::uint32_t kFaces[4][4] = {{0, 1, 2, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {1, 3, 2, 0}};
    const Vec3 w0 = s.v[0].w, w1 = s.v[1].w, w2 = s.v[2].w, w3 = s.v[3].w;
    const float volume = dot(cross(w1 - w0, w2 - w0), w3 - w0);
    float scale = 0.0f;
    for (const Vec3& w : {w0, w1, w2, w3}) {
        scale = std::max(scale, dot(w, w));
    }
    const bool flat = std::fabs(volume) <= 1e-7f * scale * std::sqrt(scale);

    bool inside = !flat;
    Simplex best;
    float best_dist_sq = 3.402823466e+38f;
    for (const auto& face : kFaces) {
        const Vec3 wi = s.v[face[0]].w;
        Vec3 n = cross(s.v[face[1]].w - wi, s.v[face[2]].w - wi);
        if (dot(n, s.v[face[3]].w - wi) > 0.0f) {
            n = n * -1.0f;
        }
        const bool origin_in_front = dot(n, wi * -1.0f) > 0.0f;
        if (!flat && !origin_in_front) {
            continue;
        }
        inside = false;
        Simplex candidate;
        candidate.v[0] = s.v[face[0]];
        candidate.v[1] = s.v[face[1]];
        candidate.v[2] = s.v[face[2]];
        candidate.count = 3;
        solve3(candidate);
        const Vec3 p = closest_point(candidate);
        const float dist_sq = dot(p, p);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best = candidate;
        }
    }
    if (inside) {
        return true;
    }
    s = best;
    return false;
}

Vec3 witness_a(const Simplex& s) noexcept {
    Vec3 p{0.0f};
    for (std::uint32_t i = 0; i < s.count; ++i) {
        p += s.v[i].wa * s.v[i].weight;
    }
    return p;
}

Vec3 witness_b(const Simplex& s) noexcept {
    Vec3 p{0.0f};
    for (std::uint32_t i = 0; i < s.count; ++i) {
        p += s.v[i].wb * s.v[i].weight;
    }
    return p;
}

} // namespace

// ------------------------------------------------------------------- GJK

GjkResult gjk(const Convex& a, const Convex& b) noexcept {
    GjkResult result;
    Simplex& s = result.simplex;

    Vec3 direction = a.pose.position - b.pose.position;
    if (dot(direction, direction) < kEpsilon * kEpsilon) {
        direction = Vec3::unit_x();
    }
    s.v[0] = minkowski_support(a, b, direction);
    s.v[0].weight = 1.0f;
    s.count = 1;
    float scale_sq = dot(s.v[0].w, s.v[0].w); // how far out the simplex reaches

    Vec3 p = s.v[0].w;
    for (int iteration = 0; iteration < kGjkMaxIterations; ++iteration) {
        switch (s.count) {
        case 1:
            s.v[0].weight = 1.0f;
            break;
        case 2:
            solve2(s);
            break;
        case 3:
            solve3(s);
            break;
        default:
            if (solve4(s)) {
                result.intersecting = true;
                return result;
            }
            break;
        }
        p = closest_point(s);
        const float dist_sq = dot(p, p);
        float tolerance = kGjkRelativeTolerance * std::sqrt(scale_sq);
        if (dist_sq <= tolerance * tolerance) {
            result.intersecting = true; // the origin is on the simplex
            return result;
        }
        // Search toward the origin. If the difference does not extend past
        // the current closest point in that direction, that point is it.
        const Vec3 d = p * -1.0f;
        const Simplex::Vertex w = minkowski_support(a, b, d);
        scale_sq = std::max(scale_sq, dot(w.w, w.w));
        tolerance = kGjkRelativeTolerance * std::sqrt(scale_sq);
        const float dist = std::sqrt(dist_sq);
        const float progress = dist + dot(w.w, d) / dist; // how much closer w gets, along d
        if (progress <= tolerance) {
            break;
        }
        bool duplicate = false;
        for (std::uint32_t i = 0; i < s.count; ++i) {
            const Vec3 diff = w.w - s.v[i].w;
            duplicate = duplicate || dot(diff, diff) <= tolerance * tolerance;
        }
        if (duplicate) {
            break;
        }
        s.v[s.count++] = w;
    }

    result.distance = std::sqrt(dot(p, p));
    result.on_a = witness_a(s);
    result.on_b = witness_b(s);
    return result;
}

// ------------------------------------------------------------------- EPA

namespace {

struct EpaVertex {
    Vec3 w, wa, wb;
};

struct EpaFace {
    std::uint32_t v[3];
    Vec3 normal;    // unit, outward
    float distance; // from the origin to the face plane, along the normal
    bool alive;
};

struct Polytope {
    EpaVertex vertices[kEpaMaxVertices];
    std::uint32_t vertex_count = 0;
    EpaFace faces[kEpaMaxFaces];
    std::uint32_t face_count = 0;
    Vec3 centroid{0.0f}; // of the first tetrahedron: strictly inside forever after

    bool add_face(std::uint32_t i, std::uint32_t j, std::uint32_t k) noexcept {
        if (face_count == kEpaMaxFaces) {
            compact();
            if (face_count == kEpaMaxFaces) {
                return false;
            }
        }
        EpaFace& f = faces[face_count++];
        f.v[0] = i;
        f.v[1] = j;
        f.v[2] = k;
        f.alive = true;
        const Vec3& a = vertices[i].w;
        Vec3 n = cross(vertices[j].w - a, vertices[k].w - a);
        const float length = std::sqrt(dot(n, n));
        if (length <= 1e-12f) {
            // A sliver: keep it in the mesh for the horizon walk, never pick it.
            f.normal = Vec3{0.0f};
            f.distance = 3.402823466e+38f;
            return true;
        }
        n = n * (1.0f / length);
        if (dot(n, centroid - a) > 0.0f) {
            n = n * -1.0f; // outward, away from the inside point
            f.v[1] = k;
            f.v[2] = j; // and wound to match, so edges stay consistent
        }
        f.normal = n;
        f.distance = dot(n, a);
        return true;
    }

    void compact() noexcept {
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < face_count; ++i) {
            if (faces[i].alive) {
                faces[kept++] = faces[i];
            }
        }
        face_count = kept;
    }
};

// Grows a simplex of fewer than four points into a tetrahedron by adding
// support points in directions that make it non-degenerate. GJK stops short
// of four when the origin lands exactly on a simplex — resting contact.
bool blow_up(const Convex& a, const Convex& b, EpaVertex* v, std::uint32_t& count) noexcept {
    const auto support = [&](const Vec3& direction) {
        const Simplex::Vertex s = minkowski_support(a, b, direction);
        return EpaVertex{s.w, s.wa, s.wb};
    };
    if (count == 1) {
        // Any axis along which the difference has some extent.
        const Vec3 axes[6] = {Vec3::unit_x(),         Vec3::unit_y(),         Vec3::unit_z(),
                              Vec3::unit_x() * -1.0f, Vec3::unit_y() * -1.0f, Vec3::unit_z() * -1.0f};
        for (const Vec3& axis : axes) {
            const EpaVertex candidate = support(axis);
            const Vec3 d = candidate.w - v[0].w;
            if (dot(d, d) > kEpsilon * kEpsilon) {
                v[count++] = candidate;
                break;
            }
        }
        if (count == 1) {
            return false;
        }
    }
    if (count == 2) {
        // A direction perpendicular to the segment, then two more at 120
        // degrees around it: one of them leaves the line.
        const Vec3 e = v[1].w - v[0].w;
        Vec3 least = Vec3::unit_z();
        if (std::fabs(e.x) < std::fabs(e.y) && std::fabs(e.x) < std::fabs(e.z)) {
            least = Vec3::unit_x();
        } else if (std::fabs(e.y) < std::fabs(e.z)) {
            least = Vec3::unit_y();
        }
        const Vec3 u = normalize(cross(e, least));
        const Vec3 t = normalize(cross(e, u));
        const Vec3 tries[3] = {u, u * -0.5f + t * 0.8660254f, u * -0.5f - t * 0.8660254f};
        for (const Vec3& direction : tries) {
            const EpaVertex candidate = support(direction);
            const Vec3 n = cross(e, candidate.w - v[0].w);
            if (dot(n, n) > kEpsilon * kEpsilon * dot(e, e)) {
                v[count++] = candidate;
                break;
            }
        }
        if (count == 2) {
            return false;
        }
    }
    if (count == 3) {
        // Off the triangle's plane, whichever side reaches further.
        const Vec3 n = cross(v[1].w - v[0].w, v[2].w - v[0].w);
        const EpaVertex front = support(n);
        const EpaVertex back = support(n * -1.0f);
        const float forward = std::fabs(dot(front.w - v[0].w, n));
        const float backward = std::fabs(dot(back.w - v[0].w, n));
        if (std::max(forward, backward) <= kEpsilon * std::sqrt(dot(n, n))) {
            return false; // the difference is flat: nothing to expand into
        }
        v[count++] = forward >= backward ? front : back;
    }
    return count == 4;
}

} // namespace

EpaResult epa(const Convex& a, const Convex& b, const Simplex& simplex) noexcept {
    EpaResult result;
    Polytope poly;
    for (std::uint32_t i = 0; i < simplex.count && i < 4; ++i) {
        poly.vertices[i] = {simplex.v[i].w, simplex.v[i].wa, simplex.v[i].wb};
    }
    poly.vertex_count = std::min<std::uint32_t>(simplex.count, 4);
    if (poly.vertex_count < 4 && !blow_up(a, b, poly.vertices, poly.vertex_count)) {
        return result;
    }
    poly.centroid =
        (poly.vertices[0].w + poly.vertices[1].w + poly.vertices[2].w + poly.vertices[3].w) * 0.25f;
    poly.add_face(0, 1, 2);
    poly.add_face(0, 3, 1);
    poly.add_face(0, 2, 3);
    poly.add_face(1, 3, 2);

    std::uint32_t best = 0;
    for (int iteration = 0;; ++iteration) {
        // The face nearest the origin is the current best guess at the way out.
        best = kEpaMaxFaces;
        float best_distance = 3.402823466e+38f;
        for (std::uint32_t i = 0; i < poly.face_count; ++i) {
            if (poly.faces[i].alive && poly.faces[i].distance < best_distance) {
                best_distance = poly.faces[i].distance;
                best = i;
            }
        }
        if (best == kEpaMaxFaces) {
            return result; // the mesh fell apart: no answer
        }
        const EpaFace face = poly.faces[best];
        const Simplex::Vertex w = minkowski_support(a, b, face.normal);
        const float reach = dot(w.w, face.normal);
        if (reach - face.distance <= 1e-5f * (1.0f + face.distance) || iteration == kEpaMaxIterations ||
            poly.vertex_count == kEpaMaxVertices) {
            break;
        }

        // Everything this point can see comes out; the hole's rim is the
        // horizon, and the new faces fan from it to the point.
        struct Edge {
            std::uint32_t a, b;
        };
        Edge edges[3 * kEpaMaxFaces / 4];
        std::uint32_t edge_count = 0;
        for (std::uint32_t i = 0; i < poly.face_count; ++i) {
            EpaFace& f = poly.faces[i];
            if (!f.alive || dot(f.normal, w.w - poly.vertices[f.v[0]].w) <= 0.0f) {
                continue;
            }
            f.alive = false;
            for (std::uint32_t k = 0; k < 3; ++k) {
                const Edge e{f.v[k], f.v[(k + 1) % 3]};
                // Consistent winding: the neighbour across e holds it reversed.
                // If that neighbour was removed too, the edge is interior.
                bool interior = false;
                for (std::uint32_t j = 0; j < edge_count; ++j) {
                    if (edges[j].a == e.b && edges[j].b == e.a) {
                        edges[j] = edges[--edge_count];
                        interior = true;
                        break;
                    }
                }
                if (!interior && edge_count < sizeof(edges) / sizeof(edges[0])) {
                    edges[edge_count++] = e;
                }
            }
        }
        const std::uint32_t added = poly.vertex_count;
        poly.vertices[poly.vertex_count++] = {w.w, w.wa, w.wb};
        for (std::uint32_t j = 0; j < edge_count; ++j) {
            if (!poly.add_face(edges[j].a, edges[j].b, added)) {
                break;
            }
        }
    }

    const EpaFace& face = poly.faces[best];
    result.ok = true;
    result.normal = face.normal;
    result.depth = face.distance;

    // The closest point on the face is normal * distance; its barycentric
    // weights carry over to the parents on A and B.
    const Vec3 p = face.normal * face.distance;
    const Vec3& w0 = poly.vertices[face.v[0]].w;
    const Vec3 e1 = poly.vertices[face.v[1]].w - w0;
    const Vec3 e2 = poly.vertices[face.v[2]].w - w0;
    const Vec3 ep = p - w0;
    const float d11 = dot(e1, e1), d12 = dot(e1, e2), d22 = dot(e2, e2);
    const float dp1 = dot(ep, e1), dp2 = dot(ep, e2);
    const float denom = d11 * d22 - d12 * d12;
    float l1 = 0.0f, l2 = 0.0f;
    if (denom > 1e-20f) {
        l1 = (d22 * dp1 - d12 * dp2) / denom;
        l2 = (d11 * dp2 - d12 * dp1) / denom;
    }
    l1 = math::clamp(l1, 0.0f, 1.0f);
    l2 = math::clamp(l2, 0.0f, 1.0f - l1);
    const float l0 = 1.0f - l1 - l2;
    result.on_a = poly.vertices[face.v[0]].wa * l0 + poly.vertices[face.v[1]].wa * l1 +
                  poly.vertices[face.v[2]].wa * l2;
    result.on_b = poly.vertices[face.v[0]].wb * l0 + poly.vertices[face.v[1]].wb * l1 +
                  poly.vertices[face.v[2]].wb * l2;
    return result;
}

} // namespace tynima::physics
