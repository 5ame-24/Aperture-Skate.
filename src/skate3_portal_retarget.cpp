#include "skate3_portal_retarget.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace skate3::portal_retarget {
namespace {

float Dist2(const float* a, const float* b) {
  const float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
  return d0 * d0 + d1 * d1 + d2 * d2;
}

// Uniform grid over a point set (3 floats each) for nearest-point queries.
struct Grid {
  float lo[3] = {}, cell = 0.05f;
  int n[3] = {1, 1, 1};
  std::vector<std::vector<uint32_t>> cells;
  std::vector<float> pts;

  void Build(std::vector<float> p) {
    pts = std::move(p);
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 3; ++k) lo[k] = 1e30f;
    for (size_t i = 0; i < pts.size(); i += 3)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], pts[i + k]), hi[k] = std::max(hi[k], pts[i + k]);
    if (pts.empty()) {
      cells.assign(1, {});
      return;
    }
    const float ext = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-3f});
    cell = std::max(ext / 48.0f, 1e-3f);
    for (int k = 0; k < 3; ++k) n[k] = std::clamp(int((hi[k] - lo[k]) / cell) + 1, 1, 256);
    cells.assign(size_t(n[0]) * n[1] * n[2], {});
    for (uint32_t i = 0; i < pts.size() / 3; ++i) cells[Index(&pts[size_t(i) * 3])].push_back(i);
  }
  bool Empty() const { return pts.empty(); }
  int Coord(float x, int k) const { return std::clamp(int((x - lo[k]) / cell), 0, n[k] - 1); }
  size_t Index(const float* p) const { return (size_t(Coord(p[2], 2)) * n[1] + Coord(p[1], 1)) * n[0] + Coord(p[0], 0); }
  // nearest point (index, -1 when empty), searching growing shells of cells until no closer one can exist; stops at `give_up` metres
  int Nearest(const float* p, float* d2_out, float give_up = 1e30f) const {
    float best = 1e30f;
    int arg = -1;
    if (pts.empty()) {
      if (d2_out) *d2_out = best;
      return -1;
    }
    const int c[3] = {Coord(p[0], 0), Coord(p[1], 1), Coord(p[2], 2)};
    const int max_r = std::max({n[0], n[1], n[2]});
    for (int r = 0; r <= max_r; ++r) {
      for (int z = c[2] - r; z <= c[2] + r; ++z) {
        if (z < 0 || z >= n[2]) continue;
        for (int y = c[1] - r; y <= c[1] + r; ++y) {
          if (y < 0 || y >= n[1]) continue;
          for (int x = c[0] - r; x <= c[0] + r; ++x) {
            if (x < 0 || x >= n[0]) continue;
            if (std::max({std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2])}) != r) continue;  // shell only
            for (uint32_t i : cells[(size_t(z) * n[1] + y) * n[0] + x]) {
              const float d = Dist2(p, &pts[size_t(i) * 3]);
              if (d < best) best = d, arg = int(i);
            }
          }
        }
      }
      const float reach = float(r) * cell;  // every unvisited cell is at least this far
      if ((arg >= 0 && best <= reach * reach) || reach > give_up) break;
    }
    if (d2_out) *d2_out = best;
    return arg;
  }
};

uint32_t DominantKey(const DonorVertex& v) {
  int a = 0;
  for (int k = 1; k < 4; ++k)
    if (v.weight[k] > v.weight[a]) a = k;
  return v.key[a];
}

int DominantBone(const Influence& in) {
  int a = 0;
  for (int k = 1; k < std::min<int>(in.count, 3); ++k)
    if (in.weight[k] > in.weight[a]) a = k;
  return in.weight[a] >= 0.5f ? in.bone[a] : -1;
}

// Rotation (3x3 rows) taking unit a to unit b by the shortest arc, at most max_rad.
void Arc(const float a[3], const float b[3], float max_rad, float R[9], float* angle) {
  float ax[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  const float s = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
  const float c = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0f, 1.0f);
  float th = std::atan2(s, c);
  if (s < 1e-6f) {  // parallel (or opposite: left alone, there is no single best axis)
    th = 0.0f;
    ax[0] = 1, ax[1] = 0, ax[2] = 0;
  } else {
    for (float& x : ax) x /= s;
  }
  th = std::min(th, max_rad);
  *angle = th;
  const float cs = std::cos(th), sn = std::sin(th), t = 1 - cs;
  const float x = ax[0], y = ax[1], z = ax[2];
  const float m[9] = {t * x * x + cs, t * x * y - sn * z, t * x * z + sn * y, t * x * y + sn * z, t * y * y + cs,
                      t * y * z - sn * x, t * x * z - sn * y, t * y * z + sn * x, t * z * z + cs};
  std::copy(m, m + 9, R);
}

void LbsRest(const std::vector<Bone>& bones, const std::vector<float>& G, const std::vector<float>& inv_rest, const std::vector<float>& verts,
             const std::vector<Influence>& infl, std::vector<float>& out) {
  // skin matrices G_b * rest_b^-1
  std::vector<float> S(bones.size() * 12);
  for (size_t b = 0; b < bones.size(); ++b) Mul34(&G[b * 12], &inv_rest[b * 12], &S[b * 12]);
  out.assign(verts.size(), 0.0f);
  for (size_t i = 0; i < infl.size(); ++i) {
    const Influence& in = infl[i];
    float acc[3] = {0, 0, 0}, used = 0;
    for (int k = 0; k < std::min<int>(in.count, 3); ++k) {
      if (in.weight[k] <= 0.0f || in.bone[k] >= bones.size()) continue;
      float q[3];
      Apply34(&S[size_t(in.bone[k]) * 12], &verts[i * 3], q);
      for (int c = 0; c < 3; ++c) acc[c] += in.weight[k] * q[c];
      used += in.weight[k];
    }
    for (int c = 0; c < 3; ++c) out[i * 3 + c] = used > 0 ? acc[c] / used : verts[i * 3 + c];
  }
}

}  // namespace

void Mul34(const float a[12], const float b[12], float out[12]) {
  float o[12];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      float s = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c];
      if (c == 3) s += a[r * 4 + 3];
      o[r * 4 + c] = s;
    }
  }
  std::copy(o, o + 12, out);
}

bool Invert34(const float m[12], float out[12]) {
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
  const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const float det = a * A + b * B + c * C;
  if (std::abs(det) < 1e-12f) return false;
  const float inv[9] = {A / det, -(b * i - c * h) / det, (b * f - c * e) / det, B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
                        C / det, -(a * h - b * g) / det, (a * e - b * d) / det};
  const float t[3] = {m[3], m[7], m[11]};
  for (int r = 0; r < 3; ++r) {
    out[r * 4 + 0] = inv[r * 3 + 0];
    out[r * 4 + 1] = inv[r * 3 + 1];
    out[r * 4 + 2] = inv[r * 3 + 2];
    out[r * 4 + 3] = -(inv[r * 3 + 0] * t[0] + inv[r * 3 + 1] * t[1] + inv[r * 3 + 2] * t[2]);
  }
  return true;
}

void Apply34(const float m[12], const float p[3], float out[3]) {
  const float x = p[0], y = p[1], z = p[2];
  out[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
  out[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
  out[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
}

Result Retarget(const std::vector<Bone>& bones, const std::vector<float>& verts, const std::vector<Influence>& infl,
                const std::vector<DonorVertex>& donor, int passes) {
  Result r;
  const size_t nb = bones.size(), nv = verts.size() / 3;
  r.map.assign(nb, 0);
  r.posed.resize(nb * 12);
  for (size_t b = 0; b < nb; ++b) std::copy(bones[b].rest, bones[b].rest + 12, &r.posed[b * 12]);
  r.rest = verts;
  if (nb == 0 || nv == 0 || donor.empty() || infl.size() != nv) return r;
  std::vector<float> dp;
  dp.reserve(donor.size() * 3);
  for (const DonorVertex& v : donor) dp.insert(dp.end(), v.p, v.p + 3);
  Grid all;
  all.Build(dp);
  std::vector<float> inv_rest(nb * 12);
  for (size_t b = 0; b < nb; ++b)
    if (!Invert34(bones[b].rest, &inv_rest[b * 12])) {
      const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      std::copy(id, id + 12, &inv_rest[b * 12]);
    }
  auto mean_gap = [&](const std::vector<float>& pos) {
    double t = 0;
    const size_t step = std::max<size_t>(1, nv / 2000);
    size_t n = 0;
    for (size_t i = 0; i < nv; i += step, ++n) {
      float d2;
      all.Nearest(&pos[i * 3], &d2);
      t += std::sqrt(d2);
    }
    return n ? float(t / double(n)) : 0.0f;
  };
  r.gap_before = mean_gap(verts);
  std::vector<int> dom(nv);
  for (size_t i = 0; i < nv; ++i) dom[i] = DominantBone(infl[i]);
  std::vector<int> own_count(nb, 0);
  for (size_t i = 0; i < nv; ++i)
    if (dom[i] >= 0 && size_t(dom[i]) < nb) ++own_count[size_t(dom[i])];
  // subtrees (parents come first)
  std::vector<std::vector<bool>> below(nb, std::vector<bool>(nb, false));
  for (size_t b = 0; b < nb; ++b) {
    below[b][b] = true;
    for (size_t c = b + 1; c < nb; ++c)
      if (bones[c].parent >= 0 && size_t(bones[c].parent) < c && below[b][size_t(bones[c].parent)]) below[b][c] = true;
  }
  // 2. pose correction, parent first. A bone carrying a limb (its subtree holds at most 40% of the vertices and reaches out from its joint)
  // is turned about its joint so its subtree lies on the skater vertices that the REST of the character does not already cover: an A-pose arm
  // hanging by the skater's torso does not cover the skater's T-pose arm, so that arm is where it goes (and the other way round).
  std::vector<float> G(nb * 12);
  for (size_t b = 0; b < nb; ++b) std::copy(bones[b].rest, bones[b].rest + 12, &G[b * 12]);
  std::vector<float> cur;
  LbsRest(bones, G, inv_rest, verts, infl, cur);
  constexpr float kClaim = 0.06f;  // a skater vertex this close to the rest of the character is that part's
  // Fibonacci sphere of candidate directions
  std::vector<std::array<float, 3>> dirs;
  constexpr int kDirs = 600;
  for (int i = 0; i < kDirs; ++i) {
    const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(kDirs), rr = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float phi = 2.39996323f * float(i);
    dirs.push_back({std::cos(phi) * rr, y, std::sin(phi) * rr});
  }
  r.turned = 0;
  r.max_turn_deg = 0;
  for (int pass = 0; pass < std::clamp(passes, 1, 2); ++pass) {
    for (size_t b = 0; b < nb; ++b) {
      if (bones[b].parent < 0 || own_count[b] < 12) continue;
      std::vector<size_t> sub, rest;
      for (size_t i = 0; i < nv; ++i) (dom[i] >= 0 && size_t(dom[i]) < nb && below[b][size_t(dom[i])] ? sub : rest).push_back(i);
      if (sub.size() < 12 || sub.size() * 10 > nv * 4 || rest.empty()) continue;
      const float J[3] = {G[b * 12 + 3], G[b * 12 + 7], G[b * 12 + 11]};
      // the subtree's main direction and reach
      double a[3] = {0, 0, 0};
      float reach = 0;
      for (size_t i : sub) {
        for (int k = 0; k < 3; ++k) a[k] += cur[i * 3 + k];
        reach = std::max(reach, std::sqrt(Dist2(&cur[i * 3], J)));
      }
      float u[3] = {float(a[0] / double(sub.size())) - J[0], float(a[1] / double(sub.size())) - J[1], float(a[2] / double(sub.size())) - J[2]};
      const float lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
      if (lu < 0.04f || lu < 0.2f * reach) continue;
      for (float& x : u) x /= lu;
      // free skater vertices within reach: not claimed by the rest of the character
      std::vector<float> rp;
      const size_t rstep = std::max<size_t>(1, rest.size() / 4000);
      for (size_t k = 0; k < rest.size(); k += rstep) rp.insert(rp.end(), &cur[rest[k] * 3], &cur[rest[k] * 3] + 3);
      Grid others;
      others.Build(std::move(rp));
      std::vector<float> fp;
      for (size_t i = 0; i < dp.size(); i += 3) {
        if (Dist2(&dp[i], J) > (1.2f * reach) * (1.2f * reach)) continue;
        float d2;
        others.Nearest(&dp[i], &d2, kClaim);
        if (d2 > kClaim * kClaim) fp.insert(fp.end(), &dp[i], &dp[i] + 3);
      }
      if (fp.size() / 3 < 20) continue;  // the skater has nothing uncovered there: the pose already matches
      Grid free_g;
      free_g.Build(std::move(fp));
      std::vector<size_t> samp;
      const size_t sstep = std::max<size_t>(1, sub.size() / 250);
      for (size_t k = 0; k < sub.size(); k += sstep) samp.push_back(sub[k]);
      auto cost = [&](const float R[9]) {
        double t = 0;
        for (size_t i : samp) {
          const float* q = &cur[i * 3];
          const float d[3] = {q[0] - J[0], q[1] - J[1], q[2] - J[2]};
          const float m[3] = {J[0] + R[0] * d[0] + R[1] * d[1] + R[2] * d[2], J[1] + R[3] * d[0] + R[4] * d[1] + R[5] * d[2],
                              J[2] + R[6] * d[0] + R[7] * d[1] + R[8] * d[2]};
          float d2;
          free_g.Nearest(m, &d2);
          t += std::sqrt(d2);
        }
        return float(t / double(samp.size()));
      };
      float R0[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, best_R[9];
      float ang0 = 0;
      const float stay = cost(R0);
      float best = stay, best_ang = 0;
      std::copy(R0, R0 + 9, best_R);
      for (const auto& dv : dirs) {
        const float c = u[0] * dv[0] + u[1] * dv[1] + u[2] * dv[2];
        if (c < -0.17f) continue;  // at most ~100 degrees
        float R[9];
        Arc(u, dv.data(), 3.2f, R, &ang0);
        const float e = cost(R);
        if (e < best) best = e, best_ang = ang0, std::copy(R, R + 9, best_R);
      }
      if (best_ang < 0.035f || best > 0.8f * stay) continue;  // under 2 degrees, or not clearly better
      const float T[12] = {best_R[0], best_R[1], best_R[2], J[0] - (best_R[0] * J[0] + best_R[1] * J[1] + best_R[2] * J[2]),
                           best_R[3], best_R[4], best_R[5], J[1] - (best_R[3] * J[0] + best_R[4] * J[1] + best_R[5] * J[2]),
                           best_R[6], best_R[7], best_R[8], J[2] - (best_R[6] * J[0] + best_R[7] * J[1] + best_R[8] * J[2])};
      for (size_t c = b; c < nb; ++c)
        if (below[b][c]) Mul34(T, &G[c * 12], &G[c * 12]);
      LbsRest(bones, G, inv_rest, verts, infl, cur);
      ++r.turned;
      r.max_turn_deg = std::max(r.max_turn_deg, best_ang * 57.29578f);
    }
  }
  r.posed = G;
  r.rest = cur;
  // 1. map by vote over the corrected rest positions: each character vertex's nearest skater vertex, its dominant bone, weighted by the
  // character's own weights
  std::vector<std::unordered_map<uint32_t, float>> votes(nb);
  std::unordered_map<uint32_t, float> total;
  for (size_t i = 0; i < nv; ++i) {
    const int j = all.Nearest(&r.rest[i * 3], nullptr);
    if (j < 0) continue;
    const uint32_t key = DominantKey(donor[size_t(j)]);
    const Influence& in = infl[i];
    for (int k = 0; k < std::min<int>(in.count, 3); ++k) {
      if (in.weight[k] <= 0.0f || in.bone[k] >= nb) continue;
      votes[in.bone[k]][key] += in.weight[k];
      total[key] += in.weight[k];
    }
  }
  uint32_t root_key = 0;
  float root_best = -1;
  for (const auto& [k, w] : total)
    if (w > root_best) root_best = w, root_key = k;
  r.mapped = 0;
  for (size_t b = 0; b < nb; ++b) {
    float best = 0;
    for (const auto& [k, w] : votes[b])
      if (w > best) best = w, r.map[b] = k;
    if (best > 0.5f) {
      ++r.mapped;
    } else {
      r.map[b] = bones[b].parent >= 0 && size_t(bones[b].parent) < b ? r.map[size_t(bones[b].parent)] : root_key;
    }
  }
  r.gap_after = mean_gap(r.rest);
  std::unordered_map<uint32_t, bool> keys;
  for (uint32_t k : r.map) keys[k] = true;
  r.distinct_keys = int(keys.size());
  return r;
}

bool SkinVertex(const Result& r, const Influence& in, size_t vertex, PaletteFn palette, void* user, float out[3]) {
  if (vertex * 3 + 2 >= r.rest.size()) return false;
  const float* p = &r.rest[vertex * 3];
  float acc[3] = {0, 0, 0}, used = 0;
  for (int k = 0; k < std::min<int>(in.count, 3); ++k) {
    if (in.weight[k] <= 0.0f || in.bone[k] >= r.map.size()) continue;
    const float* m = palette(r.map[in.bone[k]], user);
    if (!m) continue;
    float q[3];
    Apply34(m, p, q);
    for (int c = 0; c < 3; ++c) acc[c] += in.weight[k] * q[c];
    used += in.weight[k];
  }
  if (used <= 0.0f) return false;
  for (int c = 0; c < 3; ++c) out[c] = acc[c] / used;
  return true;
}

}  // namespace skate3::portal_retarget
