#include "skate3_portal.h"

#include <cmath>
#include <limits>

namespace skate3::portal {

namespace {

constexpr float kEpsilon = 1e-6f;
constexpr float kPortalRadius = 0.5f;  // world units; tune to the skater's scale
constexpr float kPortalDepth = 0.25f;  // minimum slab behind the plane that counts as crossed

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Normalize(Vec3 a) {
  const float len = Length(a);
  return len < kEpsilon ? Vec3{} : a * (1.0f / len);
}

// Unit quaternion (v, w).
struct Quat {
  Vec3 v;
  float w;
};

// Shortest-arc rotation taking unit `from` onto unit `to`.
Quat QuatFromArc(Vec3 from, Vec3 to) {
  const float d = Dot(from, to);
  if (d < -1.0f + kEpsilon) {
    // Opposite vectors: any axis perpendicular to `from` gives a half turn.
    Vec3 axis = Cross(from, std::abs(from.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
    return {Normalize(axis), 0.0f};
  }
  const Vec3 axis = Cross(from, to);
  const float len = std::sqrt(Dot(axis, axis) + (1.0f + d) * (1.0f + d));
  return {axis * (1.0f / len), (1.0f + d) / len};
}

Vec3 Rotate(Quat q, Vec3 p) {
  const Vec3 t = Cross(q.v, p) * 2.0f;
  return p + t * q.w + Cross(q.v, t);
}

// Moller-Trumbore against every triangle (two-sided); nearest hit wins.
// `normal` faces back toward the ray origin.
bool RaycastNearest(Vec3 origin, Vec3 dir, const Triangle* tris, size_t count, Vec3* hit,
                    Vec3* normal) {
  float best = std::numeric_limits<float>::infinity();
  const Triangle* best_tri = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const Triangle& t = tris[i];
    const Vec3 e1 = t.b - t.a;
    const Vec3 e2 = t.c - t.a;
    const Vec3 p = Cross(dir, e2);
    const float det = Dot(e1, p);
    if (std::abs(det) < kEpsilon) continue;  // parallel or degenerate
    const float inv = 1.0f / det;
    const Vec3 s = origin - t.a;
    const float u = Dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) continue;
    const Vec3 q = Cross(s, e1);
    const float v = Dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) continue;
    const float dist = Dot(e2, q) * inv;
    if (dist > kEpsilon && dist < best) {
      best = dist;
      best_tri = &t;
    }
  }
  if (!best_tri) return false;
  Vec3 n = Normalize(Cross(best_tri->b - best_tri->a, best_tri->c - best_tri->a));
  if (Dot(n, dir) > 0.0f) n = -n;
  *hit = origin + dir * best;
  *normal = n;
  return true;
}

}  // namespace

bool FirePortalGun(PortalPair& pair, int which, Vec3 origin, Vec3 forward, const Triangle* tris,
                   size_t count) {
  if (which < 0 || which > 1) return false;
  const Vec3 dir = Normalize(forward);
  if (Length(dir) < kEpsilon) return false;
  Vec3 hit;
  Vec3 normal;
  if (!RaycastNearest(origin, dir, tris, count, &hit, &normal)) return false;
  pair.portal[which] = Portal{hit, true, normal};
  return true;
}

bool HandlePortalTeleportation(const PortalPair& pair, Vec3& position, Vec3& velocity, float dt) {
  for (int i = 0; i < 2; ++i) {
    const Portal& in = pair.portal[i];
    const Portal& out = pair.portal[1 - i];
    if (!in.active || !out.active) continue;

    const Vec3 offset = position - in.position;
    const float depth = Dot(offset, in.normal);  // < 0 once behind the plane
    const float reach = kPortalDepth + Length(velocity) * dt;
    if (depth > 0.0f || depth < -reach) continue;
    if (Dot(velocity, in.normal) >= 0.0f) continue;  // leaving, not entering
    const Vec3 lateral = offset - in.normal * depth;
    if (Dot(lateral, lateral) > kPortalRadius * kPortalRadius) continue;

    // Entering against -in.normal must come out along out.normal. Moving that
    // same rotation over the offset keeps the player's penetration depth, now
    // on the open side of the exit plane, so the next call sees them leaving.
    // shortcut: no portal roll axis, so floor-to-floor flips about an arbitrary
    // horizontal axis; track a portal "up" when that needs to look right.
    const Quat rot = QuatFromArc(-in.normal, out.normal);
    position = out.position + Rotate(rot, offset);
    velocity = Rotate(rot, velocity);
    return true;
  }
  return false;
}

}  // namespace skate3::portal
