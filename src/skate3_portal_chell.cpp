#include "skate3_portal_chell.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "skate3_portal_gunmodel.h"

REXCVAR_DEFINE_BOOL(skate3_portal_chell, false, "Skate 3", "Experimental: draw Chell (from your own Portal 2 install) in place of the skater's character (Ctrl+0).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::portal_chell {
namespace {
struct Captured_ {
  std::vector<float> pos;  // 3 per vertex
  std::vector<uint32_t> bw, bi;
  float lo[3], hi[3];  // rest-pose bounds
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Captured_> g_cap;

constexpr int kDonors = 3;  // blending a few donors hides the tears where neighbouring skater parts move differently
struct Vert {
  float rest[3];      // in the skater's rest-pose model space
  struct Donor {
    uint32_t part;    // index into g_parts
    uint32_t bw, bi;  // a nearby skater vertex's weights / bone indices
    float w;          // blend weight (sums to 1)
  } d[kDonors];
};
std::vector<uint32_t> g_parts;                 // skater mesh ids used for the transfer
std::vector<std::vector<Vert>> g_verts;        // per Chell mesh, per unique vertex
std::vector<std::vector<float>> g_pos;         // skinned output
struct Edge {
  uint32_t a, b;
  float rest;
};
std::vector<std::vector<Edge>> g_edges;        // per Chell mesh: unique edges with their rest length (to spot vertices skinned far off)
std::vector<std::vector<float>> g_pal_cache;   // per part: the last palette seen (12 floats a bone), used when the part is not drawn this frame
bool g_ready = false;

float Dist2(const float* a, const float* b) {
  const float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
  return d0 * d0 + d1 * d1 + d2 * d2;
}
}  // namespace

bool Wanted() { return REXCVAR_GET(skate3_portal_chell) && portal_gun::GetChell() != nullptr; }

void Toggle() {
  const bool on = !REXCVAR_GET(skate3_portal_chell);
  rex::cvar::SetFlagByName("skate3_portal_chell", on ? "true" : "false");
  REXLOG_INFO("portal-chell: {}", on ? "on" : "off");
}

void Capture(uint32_t mesh, const float* v, uint32_t n) {
  if (n == 0 || n > 40000) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_cap.count(mesh)) return;
  static size_t g_bytes = 0;
  if (g_bytes + size_t(n) * 20 > (size_t(160) << 20)) return;  // decode happens once per mesh, so every skinned mesh must be kept
  g_bytes += size_t(n) * 20;
  Captured_ c;
  c.pos.resize(size_t(n) * 3);
  c.bw.resize(n);
  c.bi.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    std::memcpy(&c.pos[size_t(i) * 3], v + size_t(i) * 14, 12);
    std::memcpy(&c.bw[i], v + size_t(i) * 14 + 7, 4);
    std::memcpy(&c.bi[i], v + size_t(i) * 14 + 8, 4);
  }
  for (int k = 0; k < 3; ++k) c.lo[k] = 1e30f, c.hi[k] = -1e30f;
  for (uint32_t i = 0; i < n; ++i)
    for (int k = 0; k < 3; ++k) c.lo[k] = std::min(c.lo[k], c.pos[size_t(i) * 3 + k]), c.hi[k] = std::max(c.hi[k], c.pos[size_t(i) * 3 + k]);
  g_cap.emplace(mesh, std::move(c));
}

bool Captured(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_cap.count(mesh) != 0;
}

bool Ready() { return g_ready; }

bool IsPart(uint32_t mesh) { return std::find(g_parts.begin(), g_parts.end(), mesh) != g_parts.end(); }

// The deck, trucks and wheels: low, long and flat in the rest pose (shoes are low too, but a foot long). Not Chell's donors, and not hidden.
bool IsBoard(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cap.find(mesh);
  if (it == g_cap.end()) return false;
  const Captured_& c = it->second;
  const float ex = std::max(c.hi[0] - c.lo[0], c.hi[2] - c.lo[2]);
  return c.hi[1] < 0.3f && ex > 0.6f;
}

bool Build(const std::vector<uint32_t>& parts) {
  const portal_gun::Model* chell = portal_gun::GetChell();
  if (!chell || parts.empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  // the skater's rest-pose cloud, every vertex tagged with its part
  std::vector<float> sk;
  std::vector<uint32_t> sk_part, sk_bw, sk_bi;
  for (size_t p = 0; p < parts.size(); ++p) {
    const auto it = g_cap.find(parts[p]);
    if (it == g_cap.end()) return false;
    const Captured_& c = it->second;
    REXLOG_INFO("portal-chell: part {:08X} {} verts bounds ({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f})", parts[p], c.pos.size() / 3, c.lo[0], c.lo[1],
                c.lo[2], c.hi[0], c.hi[1], c.hi[2]);
    sk.insert(sk.end(), c.pos.begin(), c.pos.end());
    sk_bw.insert(sk_bw.end(), c.bw.begin(), c.bw.end());
    sk_bi.insert(sk_bi.end(), c.bi.begin(), c.bi.end());
    sk_part.insert(sk_part.end(), c.pos.size() / 3, uint32_t(p));
  }
  const size_t nsk = sk.size() / 3;
  if (nsk < 100) return false;
  // Chell's unique vertices (Source model space of the player model: x across, y up, z front/back; inches)
  std::vector<float> ch;
  for (const auto& m : chell->mesh)
    for (size_t i = 0; i < m.vtx.size(); i += 8) ch.insert(ch.end(), m.vtx.begin() + i, m.vtx.begin() + i + 3);
  const size_t nch = ch.size() / 3;
  auto bounds = [](const std::vector<float>& v, float* lo, float* hi) {
    for (int k = 0; k < 3; ++k) lo[k] = 1e30f, hi[k] = -1e30f;
    for (size_t i = 0; i < v.size(); i += 3)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v[i + k]), hi[k] = std::max(hi[k], v[i + k]);
  };
  float slo[3], shi[3], clo[3], chi[3];
  bounds(sk, slo, shi);
  bounds(ch, clo, chi);
  const float scale = (shi[1] - slo[1]) / std::max(1e-3f, chi[1] - clo[1]);
  // torso centroid (30%..70% of the height): where the two bodies are centred on each other (arms and poses differ, the torso does not)
  auto torso = [](const std::vector<float>& v, float lo_y, float hi_y, float* c) {
    double s[3] = {0, 0, 0};
    size_t n = 0;
    const float a = lo_y + 0.3f * (hi_y - lo_y), b = lo_y + 0.7f * (hi_y - lo_y);
    for (size_t i = 0; i < v.size(); i += 3)
      if (v[i + 1] >= a && v[i + 1] <= b) s[0] += v[i], s[1] += v[i + 1], s[2] += v[i + 2], ++n;
    for (int k = 0; k < 3; ++k) c[k] = n ? float(s[k] / double(n)) : 0.0f;
  };
  float sc[3], cc[3];
  torso(sk, slo[1], shi[1], sc);
  torso(ch, clo[1], chi[1], cc);
  // eight ways to put Chell on the skater: a quarter turn about the vertical axis (0..3), mirrored or not
  auto place = [&](const float* p, int rot, int flip, float* o) {
    float x = (p[0] - cc[0]) * (flip ? -1.0f : 1.0f), y = p[1] - clo[1], z = p[2] - cc[2];
    for (int r = 0; r < rot; ++r) {
      const float nx = z, nz = -x;
      x = nx;
      z = nz;
    }
    o[0] = sc[0] + x * scale;
    o[1] = slo[1] + y * scale;
    o[2] = sc[2] + z * scale;
  };
  // pick the turn with the smallest mean nearest-vertex distance between sampled points of both
  const size_t kSample = 700;
  float best = 1e30f;
  int best_rot = 0, best_flip = 0;
  for (int flip = 0; flip < 2; ++flip) {
    for (int rot = 0; rot < 4; ++rot) {
      double total = 0;
      for (size_t i = 0; i < kSample; ++i) {
        float q[3];
        place(&ch[(i * nch / kSample) * 3], rot, flip, q);
        float nn = 1e30f;
        for (size_t j = 0; j < 1200; ++j) nn = std::min(nn, Dist2(q, &sk[(j * nsk / 1200) * 3]));
        total += std::sqrt(nn);
      }
      const float mean = float(total / double(kSample));
      if (mean < best) best = mean, best_rot = rot, best_flip = flip;
    }
  }
  REXLOG_INFO("portal-chell: skater cloud {} vertices from {} parts, height {:.2f}..{:.2f}; Chell {} vertices height {:.1f}; scale {:.4f}, turn {}, mirror {}, mean gap {:.3f}",
              nsk, parts.size(), slo[1], shi[1], nch, chi[1] - clo[1], scale, best_rot * 90, best_flip, best);
  // transfer: each Chell vertex takes the weights of the nearest skater vertex
  g_parts = parts;
  g_verts.assign(chell->mesh.size(), {});
  g_pos.assign(chell->mesh.size(), {});
  double total_gap = 0;
  size_t counted = 0;
  for (size_t m = 0; m < chell->mesh.size(); ++m) {
    const auto& cm = chell->mesh[m];
    g_verts[m].resize(cm.vtx.size() / 8);
    g_pos[m].assign(cm.vtx.size() / 8 * 3, 0.0f);
    for (size_t i = 0; i < g_verts[m].size(); ++i) {
      Vert& v = g_verts[m][i];
      place(&cm.vtx[i * 8], best_rot, best_flip, v.rest);
      float nd[kDonors];
      size_t arg[kDonors];
      for (int k = 0; k < kDonors; ++k) nd[k] = 1e30f, arg[k] = 0;
      for (size_t j = 0; j < nsk; ++j) {
        const float d = Dist2(v.rest, &sk[j * 3]);
        if (d >= nd[kDonors - 1]) continue;
        int k = kDonors - 1;
        while (k > 0 && nd[k - 1] > d) nd[k] = nd[k - 1], arg[k] = arg[k - 1], --k;
        nd[k] = d, arg[k] = j;
      }
      float wsum = 0;
      for (int k = 0; k < kDonors; ++k) {
        v.d[k] = {sk_part[arg[k]], sk_bw[arg[k]], sk_bi[arg[k]], 1.0f / (std::sqrt(nd[k]) + 0.01f)};
        wsum += v.d[k].w;
      }
      for (int k = 0; k < kDonors; ++k) v.d[k].w /= wsum;
      total_gap += std::sqrt(nd[0]);
      ++counted;
    }
  }
  REXLOG_INFO("portal-chell: weights transferred for {} vertices, mean distance to the donor vertex {:.3f}", counted, counted ? total_gap / double(counted) : 0.0);
  g_edges.assign(chell->mesh.size(), {});
  g_pal_cache.assign(parts.size(), {});
  for (size_t m = 0; m < chell->mesh.size(); ++m) {
    std::unordered_map<uint64_t, bool> seen;
    const auto& idx = chell->mesh[m].idx;
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
      for (int e = 0; e < 3; ++e) {
        uint32_t a = idx[t + e], b = idx[t + (e + 1) % 3];
        if (a > b) std::swap(a, b);
        if (!seen.emplace((uint64_t(a) << 32) | b, true).second) continue;
        g_edges[m].push_back({a, b, std::sqrt(Dist2(g_verts[m][a].rest, g_verts[m][b].rest))});
      }
  }
  g_ready = true;
  return true;
}

bool Skin(const std::vector<Palette>& palettes) {
  if (!g_ready) return false;
  std::unordered_map<uint32_t, const Palette*> by_mesh;
  for (const Palette& p : palettes) by_mesh[p.mesh] = &p;
  std::vector<const Palette*> pal(g_parts.size(), nullptr);
  std::vector<Palette> cache_pal(g_parts.size());
  for (size_t i = 0; i < g_parts.size(); ++i) {
    const auto it = by_mesh.find(g_parts[i]);
    if (it != by_mesh.end()) {
      g_pal_cache[i].assign(it->second->bones, it->second->bones + size_t(it->second->n_bones) * 12);
      cache_pal[i] = {g_parts[i], g_pal_cache[i].data(), it->second->n_bones};
      pal[i] = &cache_pal[i];
    } else if (!g_pal_cache[i].empty()) {
      cache_pal[i] = {g_parts[i], g_pal_cache[i].data(), uint32_t(g_pal_cache[i].size() / 12)};
      pal[i] = &cache_pal[i];
    }
  }
  for (size_t m = 0; m < g_verts.size(); ++m) {
    for (size_t i = 0; i < g_verts[m].size(); ++i) {
      const Vert& v = g_verts[m][i];
      float* out = &g_pos[m][i * 3];
      float acc[3] = {0, 0, 0};
      float used = 0;
      for (int d = 0; d < kDonors; ++d) {
        const Palette* p = pal[v.d[d].part];
        if (!p) continue;  // (a part of the skater is missing this frame: that donor drops out)
        uint8_t w[4], b[4];
        std::memcpy(w, &v.d[d].bw, 4);
        std::memcpy(b, &v.d[d].bi, 4);
        float sum = float(w[0]) + w[1] + w[2] + w[3];
        if (sum <= 0.0f) w[0] = 255, sum = 255.0f;
        const float x = v.rest[0], y = v.rest[1], z = v.rest[2];
        for (int k = 0; k < 4; ++k) {
          if (w[k] == 0 || b[k] >= p->n_bones) continue;
          const float* r = p->bones + size_t(b[k]) * 12;
          const float f = v.d[d].w * float(w[k]) / sum;
          acc[0] += f * (r[0] * x + r[1] * y + r[2] * z + r[3]);
          acc[1] += f * (r[4] * x + r[5] * y + r[6] * z + r[7]);
          acc[2] += f * (r[8] * x + r[9] * y + r[10] * z + r[11]);
          used += f;
        }
      }
      if (used <= 0.0f) continue;  // keep last frame's position
      for (int k = 0; k < 3; ++k) acc[k] /= used;
      out[0] = acc[0];
      out[1] = acc[1];
      out[2] = acc[2];
    }
    // A vertex with most of its edges stretched far past their rest length was skinned wrongly (a bad palette frame, high speed): put it back
    // at the mean of its sound neighbours, so the streak never shows.
    const size_t nv = g_verts[m].size();
    std::vector<uint16_t> deg(nv, 0), bad_edges(nv, 0);
    std::vector<float> sum(nv * 3, 0.0f);
    std::vector<uint16_t> good(nv, 0);
    const auto& pos = g_pos[m];
    for (const Edge& e : g_edges[m]) {
      ++deg[e.a], ++deg[e.b];
      if (std::sqrt(Dist2(&pos[e.a * 3], &pos[e.b * 3])) > 3.0f * e.rest + 0.12f) ++bad_edges[e.a], ++bad_edges[e.b];
    }
    std::vector<uint8_t> bad(nv, 0);
    for (size_t i = 0; i < nv; ++i) bad[i] = deg[i] > 0 && bad_edges[i] * 2 > deg[i];
    for (const Edge& e : g_edges[m]) {
      if (!bad[e.a] && bad[e.b]) {
        for (int k = 0; k < 3; ++k) sum[e.b * 3 + k] += pos[e.a * 3 + k];
        ++good[e.b];
      } else if (!bad[e.b] && bad[e.a]) {
        for (int k = 0; k < 3; ++k) sum[e.a * 3 + k] += pos[e.b * 3 + k];
        ++good[e.a];
      }
    }
    for (size_t i = 0; i < nv; ++i)
      if (bad[i] && good[i])
        for (int k = 0; k < 3; ++k) g_pos[m][i * 3 + k] = sum[i * 3 + k] / float(good[i]);
  }
  return true;
}

const std::vector<float>& Positions(size_t m) {
  static const std::vector<float> none;
  return m < g_pos.size() ? g_pos[m] : none;
}

}  // namespace skate3::portal_chell
