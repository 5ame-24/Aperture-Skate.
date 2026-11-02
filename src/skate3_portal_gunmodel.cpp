#include "skate3_portal_gunmodel.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
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
Slot g_gun, g_chars[kCharacters];

// Merge identical vertices so a mesh can be drawn indexed (the renderer's per-frame vertex space is only 1 MiB).
void Index(const std::vector<float>& raw, Mesh& m, const std::vector<Skin>* raw_skin = nullptr) {
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
      if (raw_skin) m.skin.push_back((*raw_skin)[i]);  // same position, normal and uv = the same source vertex, the same weights
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
  if (!rd(magic, 4) || std::memcmp(magic, "PGUN", 4) != 0 || !rd(head, sizeof(head)) || (head[0] != 1 && head[0] != 2) || head[1] > 8 || head[2] > 64) return false;
  const bool v2 = head[0] == 2;
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
    if (!v2) {
      Index(raw, m);
      continue;
    }
    std::vector<Skin> skin(th[1]);
    for (Skin& k : skin) {
      uint8_t b[4];
      if (!rd(b, 4) || !rd(k.weight, sizeof(k.weight))) return false;
      std::memcpy(k.bone, b, 3);
      k.count = uint8_t(b[3] < 1 ? 1 : b[3] > 3 ? 3 : b[3]);
    }
    Index(raw, m, &skin);
  }
  if (v2) {
    uint32_t nb = 0;
    if (!rd(&nb, 4) || nb == 0 || nb > 256) return false;
    model.bones.resize(nb);
    for (BoneDef& b : model.bones) {
      if (!rd(b.name, 32) || !rd(&b.parent, 4) || !rd(b.rest, sizeof(b.rest))) return false;
      b.name[31] = 0;
    }
    for (const Mesh& m : model.mesh)
      for (const Skin& k : m.skin)
        for (int j = 0; j < k.count; ++j)
          if (k.bone[j] >= nb) return false;
  }
  return true;
}

const Model* Fetch(Slot& slot, const char* what, const char* file, const char* cvar_path) {
  static std::mutex mutex;  // the render thread and the key binds (character cycling) both ask
  std::lock_guard<std::mutex> lock(mutex);
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
      REXLOG_INFO("portal-gun: loaded the {} from '{}' ({} meshes, {} triangles, {} unique vertices, {} textures, {} bones{})", what, path,
                  slot.model.mesh.size(), tris, verts, slot.model.tex.size(), slot.model.bones.size(),
                  slot.model.bones.empty() ? ": version 1 cache, no skeleton; re-run portalgun_extract.py --character" : "");
    } else {
      REXLOG_INFO("portal-gun: no {} cache at '{}' (run tools/re/portalgun_extract.py)", what, path);
    }
  }
  return slot.ok ? &slot.model : nullptr;
}
}  // namespace

const Model* Get() { return Fetch(g_gun, "Portal gun model", "portalgun.bin", REXCVAR_GET(skate3_portal_gun_cache).c_str()); }
const char* CharacterName(int c) {
  static const char* kNames[kCharacters] = {"chell", "atlas", "pbody"};
  return c >= 0 && c < kCharacters ? kNames[c] : "none";
}

int CharacterFromName(const char* name) {
  for (int c = 0; c < kCharacters; ++c)
    if (std::strcmp(name, CharacterName(c)) == 0) return c;
  return -1;
}

const Model* GetCharacter(int c) {
  if (c < 0 || c >= kCharacters) return nullptr;
  static const char* kWhat[kCharacters] = {"Chell model", "Atlas model", "P-body model"};
  static const char* kFile[kCharacters] = {"chell.bin", "atlas.bin", "pbody.bin"};
  return Fetch(g_chars[c], kWhat[c], kFile[c], c == kChell ? REXCVAR_GET(skate3_portal_chell_cache).c_str() : "");
}

const Model* GetChell() { return GetCharacter(kChell); }

}  // namespace skate3::portal_gun
