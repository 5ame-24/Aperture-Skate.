#pragma once

// Portal 2 characters in the world (log prefix `portal-chell: CHAR`), each in a slot of its own:
//   slot 0 = you (Ctrl+0 cycles off -> Chell -> Atlas -> P-body; cvars skate3_portal_chell, skate3_portal_character),
//   slots 1-2 = co-op: the two nearest NPC characters become Atlas and P-body (Ctrl+Shift+0; skate3_portal_coop, skate3_portal_coop_characters).
// Each character is its own model from the user's own Portal 2 install (tools/re/portalgun_extract.py --character all): its own mesh, textures,
// skeleton and skin weights. With a skeleton (cache version 2) it is retargeted (skate3_portal_retarget.h): its bones are mapped onto the
// skeleton of the skater it stands in for, its bind pose is turned to match that skater's, and every frame it is skinned on the CPU with its
// OWN weights through its own bones, driven by that skater's bone palettes, so the skater's animation plays on the character's own skeleton.
// Without one (an old version 1 cache) each vertex borrows the weights of the nearest skater vertex, as before. The renderer draws the result in
// place of that skater's own character items.

#include <cstdint>
#include <string>
#include <vector>

#include "skate3_portal_gunmodel.h"

namespace skate3::portal_chell {

constexpr int kSlots = 3;

bool Wanted();             // any slot active
bool SlotActive(int s);    // the slot is on and its character's cache is loaded
int SlotCharacter(int s);  // portal_gun::Character, or -1
const portal_gun::Model* SlotModel(int s);
void Toggle();      // Ctrl+0: your character
void ToggleCoop();  // Ctrl+Shift+0: the two co-op slots on the nearest NPCs
// Ctrl+Shift+9: one read-only scan of guest memory for the game's own character-select names (CHARSEL lines, spec request R11).
void RequestSelectorProbe();
bool TakeSelectorProbe();

// Character slots panel (Ctrl+Shift+C, drawn by the portal overlay): slot s gets character c (portal_gun::Character, -1 = off). The same cvars
// as Ctrl+0 / Ctrl+Shift+0, so the panel, the keys and the command line always agree.
void SetSlotCharacter(int s, int c);
std::string SlotStatus(int s);  // one line: off / no cache / waiting for its skater / built: bones, mapped, gap
bool PickerOpen();
void TogglePicker();

// Render thread: where the slot's skater is this frame (you: the tracked point; co-op: the NPC character it follows). False = nobody (yet).
bool SlotTarget(int s, float out[3]);
// Search radius (m) around the target for the skater's skinned parts (palette bone-0 translation).
float SlotRadius(int s);

// Render thread, from the vertex decode: rest-pose vertices of every skinned mesh (the scene VS layout: 14 floats a vertex; position at
// 0..2, four weight bytes in word 7, four bone-index bytes in word 8). Kept for every skinned mesh (capped).
void Capture(uint32_t mesh, const float* v14, uint32_t n);
bool Captured(uint32_t mesh);
bool IsBoard(uint32_t mesh);  // a captured part that looks like the deck / trucks / wheels: left alone

struct Palette {
  uint32_t mesh;
  const float* bones;  // 12 floats a bone: three rows of [R | t], model -> world
  uint32_t n_bones;
};

// Once per slot and character, when the slot's skater parts are known. False if they are not captured yet.
bool Build(int s, const std::vector<uint32_t>& parts);
bool Ready(int s);
bool IsPart(int s, uint32_t mesh);  // one of the meshes Build took for this slot
// Every frame: skin the slot's character with these palettes.
bool Skin(int s, const std::vector<Palette>& palettes);
// Skinned positions of mesh `m` of SlotModel(s) (3 floats per unique vertex).
const std::vector<float>& Positions(int s, size_t m);

// Old single-slot names (slot 0), kept for callers that only know Chell.
inline bool Build(const std::vector<uint32_t>& parts) { return Build(0, parts); }
inline bool Ready() { return Ready(0); }
inline bool IsPart(uint32_t mesh) { return IsPart(0, mesh); }
inline bool Skin(const std::vector<Palette>& palettes) { return Skin(0, palettes); }
inline const std::vector<float>& Positions(size_t m) { return Positions(0, m); }

}  // namespace skate3::portal_chell
