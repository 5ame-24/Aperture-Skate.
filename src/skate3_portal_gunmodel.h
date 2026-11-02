#pragma once

#include <cstdint>
#include <vector>

namespace skate3::portal_gun {

// Models read from caches that `tools/re/portalgun_extract.py` makes out of the user's own Portal 2 install (the Portal gun view model and
// Chell's player model). Positions are Source model space (inches).
struct Tex {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
struct Skin {  // a vertex's own bone weights (cache version 2)
  uint8_t bone[3] = {0, 0, 0};
  uint8_t count = 1;
  float weight[3] = {1, 0, 0};
};
struct Mesh {
  uint32_t tex = 0xFFFFFFFFu;
  std::vector<float> vtx;     // unique vertices, 8 floats each (x y z nx ny nz u v)
  std::vector<uint16_t> idx;  // triangle list into vtx
  std::vector<Skin> skin;     // per unique vertex (empty for a version 1 cache)
};
struct BoneDef {
  char name[32] = {};
  int parent = -1;
  float rest[12] = {};  // bone -> model, three rows [R | t] (the bind pose)
};
struct Model {
  std::vector<Tex> tex;
  std::vector<Mesh> mesh;
  std::vector<BoneDef> bones;  // empty for a version 1 cache
};

// Loaded once on first call; null when the cache is missing or unreadable.
const Model* Get();       // the Portal gun (portalgun.bin)
const Model* GetChell();  // Chell (chell.bin) = GetCharacter(kChell)

// Portal 2 characters, each its own model with its own skeleton: chell.bin, atlas.bin, pbody.bin
// (tools/re/portalgun_extract.py --character all).
enum Character { kChell = 0, kAtlas = 1, kPbody = 2, kCharacters = 3 };
const Model* GetCharacter(int c);
const char* CharacterName(int c);   // "chell", "atlas", "pbody"
int CharacterFromName(const char* name);  // -1 when unknown

}  // namespace skate3::portal_gun
