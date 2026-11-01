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
struct Mesh {
  uint32_t tex = 0xFFFFFFFFu;
  std::vector<float> vtx;     // unique vertices, 8 floats each (x y z nx ny nz u v)
  std::vector<uint16_t> idx;  // triangle list into vtx
};
struct Model {
  std::vector<Tex> tex;
  std::vector<Mesh> mesh;
};

// Loaded once on first call; null when the cache is missing or unreadable.
const Model* Get();       // the Portal gun (portalgun.bin)
const Model* GetChell();  // Chell (chell.bin)

}  // namespace skate3::portal_gun
