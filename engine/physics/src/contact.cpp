// collide(): the narrowphase for one pair. GJK on the cores; EPA only when
// the cores themselves overlap (boxes into boxes); then the manifold, by
// clipping the two shapes' supporting faces against each other along the
// contact normal, so a box resting on a box gets its four corners in one
// frame rather than one point per frame.
#include <tynima/physics/collision.h>

#include <cmath>
#include <cstdint>

namespace tynima::physics {

namespace {

constexpr std::uint32_t kMaxClipped = 16;

struct Polygon {
    Vec3 p[kMaxClipped];
    std::uint32_t count = 0;
};

// Sutherland-Hodgman against one plane: keeps dot(q - point, normal) >= 0.
void clip_polygon(Polygon& poly, const Vec3& point, const Vec3& normal) noexcept {
    Polygon out;
    for (std::uint32_t i = 0; i < poly.count; ++i) {
        const Vec3& current = poly.p[i];
        const Vec3& next = poly.p[(i + 1) % poly.count];
        const float dc = dot(current - point, normal);
        const float dn = dot(next - point, normal);
        if (dc >= 0.0f && out.count < kMaxClipped) {
            out.p[out.count++] = current;
        }
        if ((dc >= 0.0f) != (dn >= 0.0f) && out.count < kMaxClipped) {
            out.p[out.count++] = current + (next - current) * (dc / (dc - dn));
        }
    }
    poly = out;
}

// A segment against one plane: 0, 1 or 2 points survive.
void clip_segment(Polygon& seg, const Vec3& point, const Vec3& normal) noexcept {
    const float d0 = dot(seg.p[0] - point, normal);
    const float d1 = dot(seg.p[1] - point, normal);
    if (d0 >= 0.0f && d1 >= 0.0f) {
        return;
    }
    if (d0 < 0.0f && d1 < 0.0f) {
        seg.count = 0;
        return;
    }
    const Vec3 crossing = seg.p[0] + (seg.p[1] - seg.p[0]) * (d0 / (d0 - d1));
    seg.p[d0 < 0.0f ? 0 : 1] = crossing;
}

// Clips `incident` to the prism the reference face casts along the normal:
// the reference's edge planes (a polygon) or its two end planes (a segment).
void clip_to_reference(const Vec3* reference, std::uint32_t reference_count, const Vec3& normal,
                       Polygon& incident) noexcept {
    const bool incident_is_segment = incident.count == 2;
    if (reference_count == 2) {
        const Vec3 axis = normalize(reference[1] - reference[0]);
        for (int end = 0; end < 2 && incident.count > 0; ++end) {
            const Vec3 plane_normal = end == 0 ? axis : axis * -1.0f;
            if (incident_is_segment) {
                clip_segment(incident, reference[end], plane_normal);
            } else {
                clip_polygon(incident, reference[end], plane_normal);
            }
        }
        return;
    }
    Vec3 centroid{0.0f};
    for (std::uint32_t i = 0; i < reference_count; ++i) {
        centroid += reference[i];
    }
    centroid *= 1.0f / static_cast<float>(reference_count);
    for (std::uint32_t i = 0; i < reference_count && incident.count > 0; ++i) {
        const Vec3& p0 = reference[i];
        const Vec3& p1 = reference[(i + 1) % reference_count];
        Vec3 plane_normal = cross(p1 - p0, normal);
        if (dot(plane_normal, centroid - p0) < 0.0f) {
            plane_normal = plane_normal * -1.0f; // inward
        }
        if (incident_is_segment) {
            clip_segment(incident, p0, plane_normal);
        } else {
            clip_polygon(incident, p0, plane_normal);
        }
    }
}

Vec3 face_normal(const Vec3* face, std::uint32_t count) noexcept {
    return count >= 3 ? normalize(cross(face[1] - face[0], face[2] - face[0])) : Vec3{0.0f};
}

struct Candidate {
    Vec3 on_a, on_b; // on the surfaces, radii applied
    float depth;
};

// Up to four of the candidates: the deepest, the one farthest from it, the
// one farthest from that line, and the one farthest from the line on the
// other side — a spread that holds a resting box down, not four points in
// one corner.
void prune(const Candidate* in, std::uint32_t count, const Vec3& normal, Manifold& out) noexcept {
    std::uint32_t keep[4];
    std::uint32_t kept = 0;
    bool used[kMaxClipped] = {};
    if (count <= Manifold::kMaxPoints) {
        for (std::uint32_t i = 0; i < count; ++i) {
            keep[kept++] = i;
        }
    } else {
        std::uint32_t deepest = 0;
        for (std::uint32_t i = 1; i < count; ++i) {
            if (in[i].depth > in[deepest].depth) {
                deepest = i;
            }
        }
        keep[kept++] = deepest;
        used[deepest] = true;

        std::uint32_t farthest = kMaxClipped;
        float best = -1.0f;
        for (std::uint32_t i = 0; i < count; ++i) {
            const Vec3 d = in[i].on_a - in[deepest].on_a;
            if (!used[i] && dot(d, d) > best) {
                best = dot(d, d);
                farthest = i;
            }
        }
        keep[kept++] = farthest;
        used[farthest] = true;

        // Signed distance from the line deepest-farthest, in the contact plane.
        const Vec3 line = in[farthest].on_a - in[deepest].on_a;
        const Vec3 side = cross(normal, line);
        const auto offset = [&](std::uint32_t i) { return dot(in[i].on_a - in[deepest].on_a, side); };
        std::uint32_t third = kMaxClipped;
        best = 0.0f;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!used[i] && std::fabs(offset(i)) > best) {
                best = std::fabs(offset(i));
                third = i;
            }
        }
        if (third != kMaxClipped) {
            keep[kept++] = third;
            used[third] = true;
            const float sign = offset(third) >= 0.0f ? -1.0f : 1.0f;
            std::uint32_t fourth = kMaxClipped;
            best = 0.0f;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!used[i] && offset(i) * sign > best) {
                    best = offset(i) * sign;
                    fourth = i;
                }
            }
            if (fourth != kMaxClipped) {
                keep[kept++] = fourth;
            }
        }
    }
    out.count = kept;
    for (std::uint32_t k = 0; k < kept; ++k) {
        const Candidate& c = in[keep[k]];
        ContactPoint& point = out.points[k];
        point.on_a = c.on_a;
        point.on_b = c.on_b;
        point.depth = c.depth;
        point.normal_impulse = 0.0f;
        point.tangent_impulse[0] = point.tangent_impulse[1] = 0.0f;
    }
}

} // namespace

bool collide(const Convex& a, const Convex& b, float speculative_distance, Manifold& out) noexcept {
    out.count = 0;
    const float ra = a.radius();
    const float rb = b.radius();

    // The cores: closest points and distance, or the way out of an overlap.
    Vec3 normal;
    Vec3 core_a, core_b;
    float depth;
    const GjkResult g = gjk(a, b);
    if (!g.intersecting) {
        if (g.distance - ra - rb > speculative_distance) {
            return false;
        }
        normal = (g.on_b - g.on_a) * (1.0f / g.distance);
        core_a = g.on_a;
        core_b = g.on_b;
        depth = ra + rb - g.distance;
    } else {
        const EpaResult e = epa(a, b, g.simplex);
        if (e.ok) {
            normal = e.normal;
            core_a = e.on_a;
            core_b = e.on_b;
            depth = e.depth + ra + rb;
        } else {
            // Deep inside one another and EPA could not say which way out:
            // push apart along the centres, which is at least a direction.
            const Vec3 between = b.pose.position - a.pose.position;
            normal = dot(between, between) > 1e-12f ? normalize(between) : Vec3::unit_y();
            core_a = a.pose.position;
            core_b = b.pose.position;
            depth = ra + rb;
        }
    }
    out.normal = normal;

    // The manifold: both supporting faces clipped against each other. The
    // reference is the face more nearly perpendicular to the normal; a
    // segment can only be the reference against another segment.
    Vec3 face_a[4], face_b[4];
    const std::uint32_t count_a = a.supporting_face(normal, face_a);
    const std::uint32_t count_b = b.supporting_face(normal * -1.0f, face_b);
    Candidate candidates[kMaxClipped];
    std::uint32_t candidate_count = 0;
    if (count_a >= 2 && count_b >= 2) {
        const Vec3 normal_a = face_normal(face_a, count_a);
        const Vec3 normal_b = face_normal(face_b, count_b);
        bool reference_is_a = count_a >= 3;
        if (count_a >= 3 && count_b >= 3) {
            reference_is_a = std::fabs(dot(normal_a, normal)) >= std::fabs(dot(normal_b, normal));
        }
        const Vec3* reference = reference_is_a ? face_a : face_b;
        const std::uint32_t reference_count = reference_is_a ? count_a : count_b;
        const Vec3 reference_normal = reference_is_a ? normal_a : normal_b;
        Polygon incident;
        incident.count = reference_is_a ? count_b : count_a;
        for (std::uint32_t i = 0; i < incident.count; ++i) {
            incident.p[i] = reference_is_a ? face_b[i] : face_a[i];
        }
        clip_to_reference(reference, reference_count, normal, incident);

        // Each clipped point lies on the incident core; the matching point
        // on the reference core is straight along the normal, onto the
        // reference plane (or, for a segment, its line).
        for (std::uint32_t i = 0; i < incident.count; ++i) {
            const Vec3& q = incident.p[i];
            float separation; // from A's core point to B's, along the normal
            if (reference_count >= 3) {
                const float along = dot(normal, reference_normal);
                if (std::fabs(along) < 1e-4f) {
                    continue; // the reference is edge-on: the point is meaningless
                }
                if (reference_is_a) {
                    separation = dot(q - reference[0], reference_normal) / along;
                } else {
                    separation = dot(reference[0] - q, reference_normal) / along;
                }
            } else {
                // Two segments: the reference point nearest q along its axis.
                const Vec3 axis = reference[1] - reference[0];
                const float t = math::clamp(dot(q - reference[0], axis) / dot(axis, axis), 0.0f, 1.0f);
                const Vec3 on_reference = reference[0] + axis * t;
                separation = reference_is_a ? dot(q - on_reference, normal) : dot(on_reference - q, normal);
            }
            const Vec3 a_core = reference_is_a ? q - normal * separation : q;
            const Vec3 b_core = reference_is_a ? q : q + normal * separation;
            const float point_depth = ra + rb - separation;
            if (point_depth <= -speculative_distance) {
                continue;
            }
            candidates[candidate_count++] = {a_core + normal * ra, b_core - normal * rb, point_depth};
        }
    }
    if (candidate_count == 0) {
        // A sphere, a capsule end, or clipping that found nothing: the one
        // deepest point GJK/EPA gave.
        candidates[candidate_count++] = {core_a + normal * ra, core_b - normal * rb, depth};
    }
    prune(candidates, candidate_count, normal, out);
    for (std::uint32_t i = 0; i < out.count; ++i) {
        out.points[i].local_a = a.pose.to_local(out.points[i].on_a);
        out.points[i].local_b = b.pose.to_local(out.points[i].on_b);
    }
    return true;
}

} // namespace tynima::physics
