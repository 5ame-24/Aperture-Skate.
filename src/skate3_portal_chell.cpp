#include "skate3_portal_chell.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "skate3_portal_gate.h"
#include "skate3_portal_retarget.h"

REXCVAR_DEFINE_BOOL(skate3_portal_chell, false, "Skate 3",
                    "Experimental: draw a Portal 2 character (from your own Portal 2 install) in place of your skater (Ctrl+0 cycles off, Chell, "
                    "Atlas, P-body). Which one: skate3_portal_character.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_portal_character, "chell", "Skate 3", "Your Portal 2 character: chell, atlas or pbody (Ctrl+0 cycles).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_portal_coop, false, "Skate 3",
                    "Co-op slots (Ctrl+Shift+0): the two nearest NPC characters become the characters in skate3_portal_coop_characters.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_portal_coop_characters, "atlas,pbody", "Skate 3", "Characters of the two co-op slots, comma separated.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::portal_chell {
namespace rt = skate3::portal_retarget;
namespace {

int64_t NowNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

struct Captured_ {
  std::vector<float> pos;  // 3 per vertex
  std::vector<uint32_t> bw, bi;
  float lo[3], hi[3];  // rest-pose bounds
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Captured_> g_cap;

constexpr int kDonors = 3;  // (no skeleton) blending a few donors hides the tears where neighbouring skater parts move differently
struct Vert {
  float rest[3];  // in the skater's rest-pose model space
  struct Donor {
    uint32_t part;    // index into parts
    uint32_t bw, bi;  // a nearby skater vertex's weights / bone indices
    float w;          // blend weight (sums to 1)
  } d[kDonors];
};
struct Edge {
  uint32_t a, b;
  float rest;
};

struct Slot {
  int built_char = -1;
  bool ready = false;
  std::vector<uint32_t> parts;              // skater mesh ids this slot's character stands in for
  std::vector<std::vector<Vert>> verts;     // no-skeleton path: per mesh, per unique vertex
  bool rigged = false;                      // skeleton path
  rt::Result rig;
  std::vector<std::vector<rt::Influence>> infl;  // per mesh, per unique vertex
  std::vector<size_t> first;                // per mesh: its first vertex in rig.rest
  std::vector<std::vector<float>> pos;      // skinned output
  std::vector<std::vector<Edge>> edges;     // unique edges with their rest length (to spot vertices skinned far off)
  std::vector<std::vector<float>> pal_cache;  // per part: the last palette seen, used when the part is not drawn this frame
  int64_t last_skin_ns = 0;
  // co-op: the NPC character followed
  bool follow_have = false;
  float follow[3] = {};
  int64_t follow_lost_ns = 0;
};
Slot g_slot[kSlots];

float Dist2(const float* a, const float* b) {
  const float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
  return d0 * d0 + d1 * d1 + d2 * d2;
}

int CoopCharacter(int k) {  // k = 0 or 1
  const std::string list = REXCVAR_GET(skate3_portal_coop_characters);
  size_t start = 0;
  for (int i = 0; i <= k; ++i) {
    const size_t comma = list.find(',', start);
    const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (i == k) return portal_gun::CharacterFromName(name.c_str());
    if (comma == std::string::npos) return -1;
    start = comma + 1;
  }
  return -1;
}

void Reset(Slot& st, const char* why, int s) {
  if (st.ready || !st.parts.empty()) REXLOG_INFO("portal-chell: CHAR slot {} reset ({})", s, why);
  const int64_t keep_lost = st.follow_lost_ns;
  st = Slot{};
  st.follow_lost_ns = keep_lost;
}

// The slot's character changed (or was switched off): start again.
void Sync(int s) {
  Slot& st = g_slot[s];
  const int want = SlotActive(s) ? SlotCharacter(s) : -1;
  if (st.built_char != -1 && st.built_char != want) Reset(st, "character changed", s);
}

}  // namespace

int SlotCharacter(int s) {
  if (s == 0) return portal_gun::CharacterFromName(REXCVAR_GET(skate3_portal_character).c_str());
  if (s == 1 || s == 2) return CoopCharacter(s - 1);
  return -1;
}

bool SlotActive(int s) {
  if (s < 0 || s >= kSlots) return false;
  const bool on = s == 0 ? REXCVAR_GET(skate3_portal_chell) : REXCVAR_GET(skate3_portal_coop);
  return on && portal_gun::GetCharacter(SlotCharacter(s)) != nullptr;
}

const portal_gun::Model* SlotModel(int s) { return SlotActive(s) ? portal_gun::GetCharacter(SlotCharacter(s)) : nullptr; }

bool Wanted() {
  for (int s = 0; s < kSlots; ++s)
    if (SlotActive(s)) return true;
  return false;
}

void Toggle() {
  static std::atomic<int64_t> last{0};
  const int64_t now = NowNs();
  if (now - last.load() < 400'000'000) return;  // the bind repeats while held
  last = now;
  // off -> chell -> atlas -> pbody -> off, skipping characters with no cache
  int cur = REXCVAR_GET(skate3_portal_chell) ? SlotCharacter(0) : -1;
  int next = -1;
  for (int c = cur + 1; c < portal_gun::kCharacters; ++c) {
    if (portal_gun::GetCharacter(c)) {
      next = c;
      break;
    }
  }
  if (next < 0) {
    rex::cvar::SetFlagByName("skate3_portal_chell", "false");
    REXLOG_INFO("portal-chell: CHAR you: off (caches present: chell {}, atlas {}, pbody {})", portal_gun::GetCharacter(0) != nullptr,
                portal_gun::GetCharacter(1) != nullptr, portal_gun::GetCharacter(2) != nullptr);
    return;
  }
  rex::cvar::SetFlagByName("skate3_portal_character", portal_gun::CharacterName(next));
  rex::cvar::SetFlagByName("skate3_portal_chell", "true");
  const portal_gun::Model* m = portal_gun::GetCharacter(next);
  REXLOG_INFO("portal-chell: CHAR you: {} ({} bones{})", portal_gun::CharacterName(next), m->bones.size(),
              m->bones.empty() ? ", no skeleton in the cache: nearest-vertex weights" : ", own skeleton");
}

void ToggleCoop() {
  static std::atomic<int64_t> last{0};
  const int64_t now = NowNs();
  if (now - last.load() < 400'000'000) return;
  last = now;
  const bool on = !REXCVAR_GET(skate3_portal_coop);
  rex::cvar::SetFlagByName("skate3_portal_coop", on ? "true" : "false");
  REXLOG_INFO("portal-chell: CHAR co-op {}: slot 1 {} ({}), slot 2 {} ({})", on ? "on" : "off", portal_gun::CharacterName(CoopCharacter(0)),
              portal_gun::GetCharacter(CoopCharacter(0)) ? "cache found" : "NO CACHE", portal_gun::CharacterName(CoopCharacter(1)),
              portal_gun::GetCharacter(CoopCharacter(1)) ? "cache found" : "NO CACHE");
}

std::atomic<bool> g_selector_probe{false};
void RequestSelectorProbe() {
  g_selector_probe = true;
  REXLOG_INFO("portal-chell: CHARSEL scan requested (runs on the next skater update, in the background)");
}
bool TakeSelectorProbe() { return g_selector_probe.exchange(false); }

bool SlotTarget(int s, float out[3]) {
  if (!SlotActive(s)) return false;
  Sync(s);
  Slot& st = g_slot[s];
  if (s == 0) return portal_gate::TrackedPos(out);
  float me[3];
  if (!portal_gate::TrackedPos(me)) return false;
  float chars[48][3];
  const int n = portal_gate::CharacterPositions(chars, 48);
  const int64_t now = NowNs();
  if (st.ready && now - st.last_skin_ns > 3'000'000'000ll) {
    Reset(st, "its skater parts were not drawn for 3 s", s);
  }
  if (st.follow_have) {
    int best = -1;
    float bd = 3.0f * 3.0f;
    for (int i = 0; i < n; ++i) {
      const float d = Dist2(chars[i], st.follow);
      if (d < bd) bd = d, best = i;
    }
    if (best >= 0) {
      std::copy(chars[best], chars[best] + 3, st.follow);
      std::copy(st.follow, st.follow + 3, out);
      return true;
    }
    Reset(st, "lost the NPC it followed", s);
    st.follow_lost_ns = now;
    return false;
  }
  if (now - st.follow_lost_ns < 1'000'000'000ll) return false;
  // pick: the (s)-th nearest character within 25 m that the other co-op slot does not follow
  const Slot& other = g_slot[s == 1 ? 2 : 1];
  std::vector<std::pair<float, int>> by;
  for (int i = 0; i < n; ++i) {
    const float d = Dist2(chars[i], me);
    if (d > 25.0f * 25.0f || d < 0.8f * 0.8f) continue;
    if (other.follow_have && Dist2(chars[i], other.follow) < 1.0f) continue;
    by.push_back({d, i});
  }
  if (by.empty()) return false;
  std::sort(by.begin(), by.end());
  const int pick = by[0].second;
  std::copy(chars[pick], chars[pick] + 3, st.follow);
  st.follow_have = true;
  REXLOG_INFO("portal-chell: CHAR slot {} ({}) follows the NPC character at ({:.2f}, {:.2f}, {:.2f}), {:.1f} m from you", s,
              portal_gun::CharacterName(SlotCharacter(s)), st.follow[0], st.follow[1], st.follow[2], std::sqrt(by[0].first));
  std::copy(st.follow, st.follow + 3, out);
  return true;
}

float SlotRadius(int s) { return s == 0 ? 1.8f : 1.5f; }

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

bool Ready(int s) {
  if (s < 0 || s >= kSlots) return false;
  Sync(s);
  return g_slot[s].ready;
}

bool IsPart(int s, uint32_t mesh) {
  if (s < 0 || s >= kSlots) return false;
  const auto& p = g_slot[s].parts;
  return std::find(p.begin(), p.end(), mesh) != p.end();
}

// The deck, trucks and wheels: low, long and flat in the rest pose (shoes are low too, but a foot long). Not a character's donors, and not hidden.
bool IsBoard(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cap.find(mesh);
  if (it == g_cap.end()) return false;
  const Captured_& c = it->second;
  const float ex = std::max(c.hi[0] - c.lo[0], c.hi[2] - c.lo[2]);
  return c.hi[1] < 0.3f && ex > 0.6f;
}

bool Build(int s, const std::vector<uint32_t>& parts) {
  if (s < 0 || s >= kSlots) return false;
  Sync(s);
  Slot& st = g_slot[s];
  const int ch_id = SlotCharacter(s);
  const portal_gun::Model* chell = SlotModel(s);
  if (!chell || parts.empty()) return false;
  const int64_t t0 = NowNs();
  std::lock_guard<std::mutex> lock(g_mutex);
  const char* who = portal_gun::CharacterName(ch_id);
  // the skater's rest-pose cloud, every vertex tagged with its part
  std::vector<float> sk;
  std::vector<uint32_t> sk_part, sk_bw, sk_bi;
  for (size_t p = 0; p < parts.size(); ++p) {
    const auto it = g_cap.find(parts[p]);
    if (it == g_cap.end()) return false;
    const Captured_& c = it->second;
    REXLOG_INFO("portal-chell: CHAR slot {} part {:08X} {} verts bounds ({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f})", s, parts[p], c.pos.size() / 3,
                c.lo[0], c.lo[1], c.lo[2], c.hi[0], c.hi[1], c.hi[2]);
    sk.insert(sk.end(), c.pos.begin(), c.pos.end());
    sk_bw.insert(sk_bw.end(), c.bw.begin(), c.bw.end());
    sk_bi.insert(sk_bi.end(), c.bi.begin(), c.bi.end());
    sk_part.insert(sk_part.end(), c.pos.size() / 3, uint32_t(p));
  }
  const size_t nsk = sk.size() / 3;
  if (nsk < 100) return false;
  // the character's unique vertices (its own model space: y up, inches)
  std::vector<float> ch;
  for (const auto& m : chell->mesh)
    for (size_t i = 0; i < m.vtx.size(); i += 8) ch.insert(ch.end(), m.vtx.begin() + i, m.vtx.begin() + i + 3);
  const size_t nch = ch.size() / 3;
  if (nch == 0) return false;
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
    double sum[3] = {0, 0, 0};
    size_t n = 0;
    const float a = lo_y + 0.3f * (hi_y - lo_y), b = lo_y + 0.7f * (hi_y - lo_y);
    for (size_t i = 0; i < v.size(); i += 3)
      if (v[i + 1] >= a && v[i + 1] <= b) sum[0] += v[i], sum[1] += v[i + 1], sum[2] += v[i + 2], ++n;
    for (int k = 0; k < 3; ++k) c[k] = n ? float(sum[k] / double(n)) : 0.0f;
  };
  float sc[3], cc[3];
  torso(sk, slo[1], shi[1], sc);
  torso(ch, clo[1], chi[1], cc);
  // eight ways to put the character on the skater: a quarter turn about the vertical axis (0..3), mirrored or not
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
  REXLOG_INFO("portal-chell: CHAR slot {} ({}): skater cloud {} vertices from {} parts, height {:.2f}..{:.2f}; character {} vertices height {:.1f}; "
              "scale {:.4f}, turn {}, mirror {}, mean gap {:.3f}", s, who, nsk, parts.size(), slo[1], shi[1], nch, chi[1] - clo[1], scale, best_rot * 90,
              best_flip, best);
  {
    // a fresh build keeps only which NPC a co-op slot follows
    const bool fh = st.follow_have;
    float fp[3];
    std::copy(st.follow, st.follow + 3, fp);
    st = Slot{};
    st.follow_have = fh;
    std::copy(fp, fp + 3, st.follow);
  }
  st.parts = parts;
  st.pos.assign(chell->mesh.size(), {});
  for (size_t m = 0; m < chell->mesh.size(); ++m) st.pos[m].assign(chell->mesh[m].vtx.size() / 8 * 3, 0.0f);
  bool skin_ok = !chell->bones.empty();
  for (const auto& m : chell->mesh) skin_ok &= m.skin.size() == m.vtx.size() / 8;
  if (skin_ok) {
    // ---- skeleton path: the character's own bones and weights, retargeted onto this skater ----
    // the placement as an affine map (linear part A, so bone matrices can be placed too; A has the scale and the turn / mirror)
    float o0[3], ex[3], ey[3], ez[3];
    const float zero[3] = {0, 0, 0}, ux[3] = {1, 0, 0}, uy[3] = {0, 1, 0}, uz[3] = {0, 0, 1};
    place(zero, best_rot, best_flip, o0);
    place(ux, best_rot, best_flip, ex);
    place(uy, best_rot, best_flip, ey);
    place(uz, best_rot, best_flip, ez);
    const float A[9] = {ex[0] - o0[0], ey[0] - o0[0], ez[0] - o0[0], ex[1] - o0[1], ey[1] - o0[1], ez[1] - o0[1],
                        ex[2] - o0[2], ey[2] - o0[2], ez[2] - o0[2]};
    std::vector<rt::Bone> bones(chell->bones.size());
    for (size_t b = 0; b < bones.size(); ++b) {
      const portal_gun::BoneDef& d = chell->bones[b];
      bones[b].parent = d.parent >= 0 && size_t(d.parent) < b ? d.parent : -1;
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
          bones[b].rest[r * 4 + c] = A[r * 3 + 0] * d.rest[0 * 4 + c] + A[r * 3 + 1] * d.rest[1 * 4 + c] + A[r * 3 + 2] * d.rest[2 * 4 + c];
      const float t[3] = {d.rest[3], d.rest[7], d.rest[11]};
      float pt[3];
      place(t, best_rot, best_flip, pt);
      bones[b].rest[3] = pt[0], bones[b].rest[7] = pt[1], bones[b].rest[11] = pt[2];
    }
    std::vector<float> verts;
    std::vector<rt::Influence> all_infl;
    st.infl.assign(chell->mesh.size(), {});
    st.first.assign(chell->mesh.size(), 0);
    for (size_t m = 0; m < chell->mesh.size(); ++m) {
      const auto& cm = chell->mesh[m];
      st.first[m] = verts.size() / 3;
      for (size_t i = 0; i < cm.vtx.size() / 8; ++i) {
        float q[3];
        place(&cm.vtx[i * 8], best_rot, best_flip, q);
        verts.insert(verts.end(), q, q + 3);
        rt::Influence in;
        const portal_gun::Skin& k = cm.skin[i];
        in.count = k.count;
        for (int j = 0; j < 3; ++j) in.bone[j] = k.bone[j], in.weight[j] = j < k.count ? k.weight[j] : 0.0f;
        st.infl[m].push_back(in);
        all_infl.push_back(in);
      }
    }
    std::vector<rt::DonorVertex> donor(nsk);
    for (size_t j = 0; j < nsk; ++j) {
      rt::DonorVertex& d = donor[j];
      std::copy(&sk[j * 3], &sk[j * 3] + 3, d.p);
      uint8_t w[4], b[4];
      std::memcpy(w, &sk_bw[j], 4);
      std::memcpy(b, &sk_bi[j], 4);
      for (int k = 0; k < 4; ++k) d.key[k] = (sk_part[j] << 8) | b[k], d.weight[k] = float(w[k]) / 255.0f;
    }
    st.rig = rt::Retarget(bones, verts, all_infl, donor, 2);
    st.rigged = true;
    const float ms = float(NowNs() - t0) * 1e-6f;
    REXLOG_INFO("portal-chell: CHAR slot {} ({}): own skeleton {} bones, {} mapped by their own vertices onto {} skater bones, {} bones turned to the "
                "skater's rest pose (largest {:.0f} deg), gap to the skater {:.3f} -> {:.3f} m, built in {:.0f} ms", s, who, bones.size(), st.rig.mapped,
                st.rig.distinct_keys, st.rig.turned, st.rig.max_turn_deg, st.rig.gap_before, st.rig.gap_after, ms);
    for (size_t b = 0; b < bones.size() && b < 80; ++b) {
      REXLOG_INFO("portal-chell: CHAR slot {} bone {} {} (parent {}) -> skater part {:08X} bone {}", s, b, chell->bones[b].name, chell->bones[b].parent,
                  parts[std::min<size_t>(st.rig.map[b] >> 8, parts.size() - 1)], st.rig.map[b] & 0xFF);
    }
  } else {
    // ---- no skeleton (version 1 cache): each vertex takes the weights of the nearest skater vertices ----
    st.verts.assign(chell->mesh.size(), {});
    double total_gap = 0;
    size_t counted = 0;
    for (size_t m = 0; m < chell->mesh.size(); ++m) {
      const auto& cm = chell->mesh[m];
      st.verts[m].resize(cm.vtx.size() / 8);
      for (size_t i = 0; i < st.verts[m].size(); ++i) {
        Vert& v = st.verts[m][i];
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
    REXLOG_INFO("portal-chell: CHAR slot {} ({}): no skeleton in the cache, weights transferred for {} vertices, mean distance to the donor vertex {:.3f} "
                "(re-run portalgun_extract.py --character all for the own-skeleton path)", s, who, counted, counted ? total_gap / double(counted) : 0.0);
  }
  st.edges.assign(chell->mesh.size(), {});
  st.pal_cache.assign(parts.size(), {});
  for (size_t m = 0; m < chell->mesh.size(); ++m) {
    std::unordered_map<uint64_t, bool> seen;
    const auto& idx = chell->mesh[m].idx;
    auto rest_of = [&](uint32_t v) -> const float* { return st.rigged ? &st.rig.rest[(st.first[m] + v) * 3] : st.verts[m][v].rest; };
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
      for (int e = 0; e < 3; ++e) {
        uint32_t a = idx[t + e], b = idx[t + (e + 1) % 3];
        if (a > b) std::swap(a, b);
        if (!seen.emplace((uint64_t(a) << 32) | b, true).second) continue;
        st.edges[m].push_back({a, b, std::sqrt(Dist2(rest_of(a), rest_of(b)))});
      }
  }
  st.built_char = ch_id;
  st.ready = true;
  st.last_skin_ns = NowNs();
  return true;
}

namespace {
struct PalCtx {
  const std::vector<const Palette*>* pal;
};
const float* PaletteOf(uint32_t key, void* user) {
  const auto& pal = *static_cast<PalCtx*>(user)->pal;
  const uint32_t part = key >> 8, bone = key & 0xFF;
  if (part >= pal.size() || !pal[part] || bone >= pal[part]->n_bones) return nullptr;
  return pal[part]->bones + size_t(bone) * 12;
}
}  // namespace

bool Skin(int s, const std::vector<Palette>& palettes) {
  if (s < 0 || s >= kSlots) return false;
  Slot& st = g_slot[s];
  if (!st.ready) return false;
  std::unordered_map<uint32_t, const Palette*> by_mesh;
  for (const Palette& p : palettes) by_mesh[p.mesh] = &p;
  std::vector<const Palette*> pal(st.parts.size(), nullptr);
  std::vector<Palette> cache_pal(st.parts.size());
  bool any = false;
  for (size_t i = 0; i < st.parts.size(); ++i) {
    const auto it = by_mesh.find(st.parts[i]);
    if (it != by_mesh.end()) {
      st.pal_cache[i].assign(it->second->bones, it->second->bones + size_t(it->second->n_bones) * 12);
      any = true;
    }
    if (!st.pal_cache[i].empty()) {
      cache_pal[i] = {st.parts[i], st.pal_cache[i].data(), uint32_t(st.pal_cache[i].size() / 12)};
      pal[i] = &cache_pal[i];
    }
  }
  if (any) st.last_skin_ns = NowNs();
  PalCtx ctx{&pal};
  for (size_t m = 0; m < st.pos.size(); ++m) {
    const size_t nv = st.pos[m].size() / 3;
    for (size_t i = 0; i < nv; ++i) {
      float* out = &st.pos[m][i * 3];
      if (st.rigged) {
        float q[3];
        if (rt::SkinVertex(st.rig, st.infl[m][i], st.first[m] + i, &PaletteOf, &ctx, q)) std::copy(q, q + 3, out);
        continue;  // (no bone of this vertex is drawn this frame: keep last frame's position)
      }
      const Vert& v = st.verts[m][i];
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
      for (int k = 0; k < 3; ++k) out[k] = acc[k] / used;
    }
    // A vertex with most of its edges stretched far past their rest length was skinned wrongly (a bad palette frame, high speed): put it back
    // at the mean of its sound neighbours, so the streak never shows.
    std::vector<uint16_t> deg(nv, 0), bad_edges(nv, 0), good(nv, 0);
    std::vector<float> sum(nv * 3, 0.0f);
    const auto& pos = st.pos[m];
    for (const Edge& e : st.edges[m]) {
      ++deg[e.a], ++deg[e.b];
      if (std::sqrt(Dist2(&pos[e.a * 3], &pos[e.b * 3])) > 3.0f * e.rest + 0.12f) ++bad_edges[e.a], ++bad_edges[e.b];
    }
    std::vector<uint8_t> bad(nv, 0);
    for (size_t i = 0; i < nv; ++i) bad[i] = deg[i] > 0 && bad_edges[i] * 2 > deg[i];
    for (const Edge& e : st.edges[m]) {
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
        for (int k = 0; k < 3; ++k) st.pos[m][i * 3 + k] = sum[i * 3 + k] / float(good[i]);
  }
  return true;
}

const std::vector<float>& Positions(int s, size_t m) {
  static const std::vector<float> none;
  if (s < 0 || s >= kSlots) return none;
  return m < g_slot[s].pos.size() ? g_slot[s].pos[m] : none;
}

}  // namespace skate3::portal_chell
