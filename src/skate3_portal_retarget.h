#pragma once

// Skeleton retarget for Portal 2 characters on the Skate 3 skater (pure maths, no game, no renderer; unit-tested in
// tests/skate3_portal_retarget_test.cpp).
//
// Inputs, all in the skater's rest-pose model space (the character already scaled / turned / centred onto it, portal_chell::Build):
//   - the character's bones (parent index, rest bone -> model matrix) and its vertices with their own (up to 3) bone weights, as authored;
//   - the skater's rest-pose vertices with their (up to 4) weights, each bone named by a "key" (skater part << 8 | palette index), because the
//     skater is drawn as several skinned parts with a palette each.
// Steps:
//   1. map every character bone to the skater bone its vertices sit on (vote of the nearest skater vertex's dominant bone, weighted by the
//      character's own weights); bones without vertices follow their parent;
//   2. pose the character into the skater's rest pose with its OWN skeleton: parent first, a limb bone is turned about its joint so the centroid
//      of the vertices it carries points at the centroid of the skater bone it maps to (an A-pose arm meets a T-pose arm, and the other way),
//      and its children follow; steps 1-2 run twice (the better pose gives a better map);
//   3. the corrected rest vertices (linear blend skinning with the character's own weights) are what gets skinned every frame with the skater's
//      palettes through the map: v = sum_i w_i P[map(b_i)] v_rest.

#include <cstdint>
#include <vector>

namespace skate3::portal_retarget {

struct Bone {
  int parent = -1;   // < own index (parents come first)
  float rest[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // bone -> model, three rows [R | t]
};
struct Influence {
  uint8_t bone[3] = {0, 0, 0};
  uint8_t count = 1;
  float weight[3] = {1, 0, 0};
};
struct DonorVertex {
  float p[3] = {};
  uint32_t key[4] = {};
  float weight[4] = {};  // 0 = unused slot
};

struct Result {
  std::vector<uint32_t> map;       // per character bone: the skater bone key driving it
  std::vector<float> posed;        // per character bone: 12 floats, its corrected rest bone -> model matrix
  std::vector<float> rest;         // per character vertex: 3 floats, its corrected rest position
  int mapped = 0;                  // bones mapped by their own vertices (the rest follow a parent)
  int turned = 0;                  // bones turned in step 2 (last pass)
  float max_turn_deg = 0;          // largest single turn (last pass)
  float gap_before = 0, gap_after = 0;  // mean distance character vertex -> nearest skater vertex, before and after the pose correction
  int distinct_keys = 0;           // how many different skater bones the character uses
};

// verts: 3 floats per vertex; infl: one per vertex. passes: 1..3 (2 recommended).
Result Retarget(const std::vector<Bone>& bones, const std::vector<float>& verts, const std::vector<Influence>& infl,
                const std::vector<DonorVertex>& donor, int passes);

// 3x4 [R | t] helpers (rows), exposed for tests and the skinning.
void Mul34(const float a[12], const float b[12], float out[12]);  // a * b (as 4x4 with a last row 0 0 0 1)
bool Invert34(const float m[12], float out[12]);
void Apply34(const float m[12], const float p[3], float out[3]);

// The palette lookup the runtime gives Skin: 12 floats for a skater bone key, or null when that bone is not drawn this frame.
using PaletteFn = const float* (*)(uint32_t key, void* user);
// v_out = sum_i w_i P[map(b_i)] rest_v, renormalised over the bones found; false (out untouched) when none was found.
bool SkinVertex(const Result& r, const Influence& in, size_t vertex, PaletteFn palette, void* user, float out[3]);

}  // namespace skate3::portal_retarget
