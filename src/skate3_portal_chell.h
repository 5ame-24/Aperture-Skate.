#pragma once

// Chell as the skater: her mesh (from the user's own Portal 2 install, tools/re/portalgun_extract.py) is given the skater's own skin weights
// by nearest vertex (after scaling and turning it onto the skater's rest pose) and then skinned every frame on the CPU with the skater's bone
// palettes, so she moves with the skater's skeleton. The renderer draws the result in place of the skater's character items.

#include <cstdint>
#include <vector>

namespace skate3::portal_chell {

bool Wanted();   // skate3_portal_chell on and the model cache present
void Toggle();   // Ctrl+0

// Render thread, from the vertex decode: the skater's rest-pose vertices (the scene VS layout: 14 floats a vertex; position at 0..2, four
// weight bytes in word 7, four bone-index bytes in word 8). Kept for every skinned mesh (capped); Build picks the skater's parts.
void Capture(uint32_t mesh, const float* v14, uint32_t n);
bool Captured(uint32_t mesh);
bool IsBoard(uint32_t mesh);  // a captured part that looks like the deck / trucks / wheels: left alone

struct Palette {
  uint32_t mesh;
  const float* bones;  // 12 floats a bone: three rows of [R | t], model -> world
  uint32_t n_bones;
};

// Once, when the skater's parts are known: scale / turn / move Chell onto the skater's rest pose and give each vertex the weights of the
// nearest skater vertex. False if the skater's parts are not captured yet.
bool Build(const std::vector<uint32_t>& parts);
bool Ready();
bool IsPart(uint32_t mesh);  // one of the meshes Build took as the skater's

// Every frame: skin all of Chell's vertices with these palettes. False if a needed palette is missing.
bool Skin(const std::vector<Palette>& palettes);
// Skinned positions of mesh `m` (3 floats per unique vertex of portal_gun::GetChell()->mesh[m]).
const std::vector<float>& Positions(size_t m);

}  // namespace skate3::portal_chell
