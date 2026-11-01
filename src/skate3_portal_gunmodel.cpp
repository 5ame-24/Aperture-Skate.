#include "skate3_portal_gunmodel.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(skate3_portal_gun_cache, "", "Skate 3",
                      "Portal gun model cache made by tools/re/portalgun_extract.py from your own Portal 2 install; empty = "
                      "~/Library/Application Support/Skate3PortalMod/portalgun.bin")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_portal_chell_cache, "", "Skate 3", "Chell model cache (tools/re/portalgun_extract.py, MODEL=models/player/chell/player); empty = ~/Library/Application Support/Skate3PortalMod/chell.bin")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::portal_gun {
namespace {
struct Slot {
  Model model;
  bool tried = false, ok = false;
};
Slot g_gun, g_chell;

// Merge identical vertices so a mesh can be drawn indexed (the renderer's per-frame vertex space is only 1 MiB).
void Index(const std::vector<float>& raw, Mesh& m) {
  struct Key {
    uint32_t w[8];
    bool operator==(const Key& o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; }
  };
  struct Hash {
    size_t operator()(const Key& k) const {
      size_t h = 1469598103934665603ull;
      for (uint32_t x : k.w) h = (h ^ x) * 1099511628211ull;
      return h;
    }
  };
  std::unordered_map<Key, uint16_t, Hash> seen;
  const size_t n = raw.size() / 8;
  m.vtx.clear();
  m.idx.clear();
  m.idx.reserve(n);
  for (size_t i = 0; i < n && m.vtx.size() / 8 < 65535; ++i) {
    Key k;
    std::memcpy(k.w, &raw[i * 8], sizeof(k.w));
    auto it = seen.find(k);
    if (it == seen.end()) {
      it = seen.emplace(k, uint16_t(m.vtx.size() / 8)).first;
      m.vtx.insert(m.vtx.end(), raw.begin() + i * 8, raw.begin() + i * 8 + 8);
    }
    m.idx.push_back(it->second);
  }
}

bool Load(const std::string& path, Model& model) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  auto rd = [&](void* p, size_t n) { return bool(f.read(static_cast<char*>(p), std::streamsize(n))); };
  char magic[4];
  uint32_t head[3];
  if (!rd(magic, 4) || std::memcmp(magic, "PGUN", 4) != 0 || !rd(head, sizeof(head)) || head[0] != 1 || head[1] > 8 || head[2] > 64) return false;
  model.tex.resize(head[1]);
  for (Tex& t : model.tex) {
    uint32_t wh[2];
    if (!rd(wh, sizeof(wh)) || wh[0] == 0 || wh[1] == 0 || wh[0] > 4096 || wh[1] > 4096) return false;
    t.w = wh[0];
    t.h = wh[1];
    t.rgba.resize(size_t(t.w) * t.h * 4);
    if (!rd(t.rgba.data(), t.rgba.size())) return false;
    for (size_t i = 3; i < t.rgba.size(); i += 4) t.rgba[i] = 255;  // the VTF's alpha is a material mask, not opacity (the gun drew see-through)
  }
  model.mesh.resize(head[2]);
  for (Mesh& m : model.mesh) {
    uint32_t th[2];
    if (!rd(th, sizeof(th)) || th[1] % 3 != 0 || th[1] > 200000) return false;
    m.tex = th[0];
    std::vector<float> raw(size_t(th[1]) * 8);
    if (!rd(raw.data(), raw.size() * sizeof(float))) return false;
    Index(raw, m);
  }
  return true;
}

const Model* Fetch(Slot& slot, const char* what, const char* file, const char* cvar_path) {
  if (!slot.tried) {
    slot.tried = true;
    std::string path = cvar_path;
    if (path.empty()) {
      const char* home = std::getenv("HOME");
      if (home) path = std::string(home) + "/Library/Application Support/Skate3PortalMod/" + file;
    }
    slot.ok = Load(path, slot.model);
    if (slot.ok) {
      size_t tris = 0, verts = 0;
      for (const Mesh& m : slot.model.mesh) tris += m.idx.size() / 3, verts += m.vtx.size() / 8;
      REXLOG_INFO("portal-gun: loaded the {} from '{}' ({} meshes, {} triangles, {} unique vertices, {} textures)", what, path, slot.model.mesh.size(), tris,
                  verts, slot.model.tex.size());
    } else {
      REXLOG_INFO("portal-gun: no {} cache at '{}' (run tools/re/portalgun_extract.py)", what, path);
    }
  }
  return slot.ok ? &slot.model : nullptr;
}
}  // namespace

const Model* Get() { return Fetch(g_gun, "Portal gun model", "portalgun.bin", REXCVAR_GET(skate3_portal_gun_cache).c_str()); }
const Model* GetChell() { return Fetch(g_chell, "Chell model", "chell.bin", REXCVAR_GET(skate3_portal_chell_cache).c_str()); }

}  // namespace skate3::portal_gun
