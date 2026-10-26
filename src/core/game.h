#pragma once

#include <windows.h>

#include <cstdint>

// Access to exported client/renderer functions and data used by features.
// Everything here is resolved by export name (stable across the patched client builds).

namespace prk::game {

struct Vec3 {
    float x, y, z;
};

struct Identity {
    uint32_t type;
    uint32_t instance;
};

// Resolves exports. Returns false (and logs) if anything required is missing.
bool Init();

// randy31: RRefFrame_t::GetWorldMatrix() const -> translation of a visual / ref frame.
bool WorldPosition(void* refFrame, Vec3* out);

// Camera space from RViewPort_t::m_CurrentViewMatrix: x right, y up, z forward (depth).
// DiaBill sprite offsets live in these axes.
Vec3 ToView(const Vec3& world);
Vec3 WorldDirToView(const Vec3& dir);

// Camera space <-> game window pixels (y down), using m_CurrentProjectionMatrix and the
// window size. ViewToScreen fails for points behind the camera.
bool ViewToScreen(const Vec3& view, float* sx, float* sy);
Vec3 ScreenToView(float sx, float sy, float depth);

// Screen pixels per world unit at a camera-space depth.
float PxPerUnit(float depth);

void ViewportSize(int* width, int* height);

// N3InterfaceModule_t::GetClientInst: instance of the player's character (0 if unavailable).
uint32_t ClientInstance();

// n3Dynel_t::GetDynel (null if it doesn't exist).
void* DynelById(const Identity& id);

// n3Dynel_t's own Identity (dynel+0x14), checked against n3Dynel_t::GetDynel.
bool IdentityOf(void* dynel, Identity* out);

// N3InterfaceModule_t::N3Msg_GetIndicatorPosition: world position the name tag sits at
// (top of the character). False if the dynel no longer exists or has no mesh.
bool IndicatorPosition(const Identity& id, Vec3* out);

// World position of the camera, from the view matrix.
Vec3 CameraWorldPos();

// n3Playfield_t::LineOfSight on the current playfield: whether nothing static (terrain, rooms,
// buildings) blocks the line. Characters don't count. False with *known = false when unavailable.
bool LineOfSight(const Vec3& from, const Vec3& to, bool* known);

// DisplaySystem: GfxVisualDiaBill::Init(const DiaBill_n::Sprite_t*, int)
void DiaBillInit(void* visual, const void* sprites, int count);

// Turns off the depth test in a GfxVisualDiaBill's own render state block, so it draws
// over geometry. Returns false if the visual has no state block.
bool DiaBillDisableDepthTest(void* visual);

// GfxVisualDiaBill's material (+0x17C, passed to RViewPort_t::SetMaterial when it renders).
// The GfxVisual ctor AddRefs it and the dtor Releases it, so swapping keeps the counts balanced.
void* DiaBillMaterial(void* visual);
bool SwapDiaBillMaterial(void* visual, void* material);

// randy31 RTexture_t(name, w, h, D3DX_SF_A8R8G8B8, no mips, nullptr). Null on failure.
// Objects are allocated with the game's CRT (msvcr100) like the game's own and never freed.
void* CreateTextureARGB(const char* name, int width, int height);

// Lock (write-only), copy width*height ARGB pixels row by row, unlock.
bool WriteTexture(void* texture, const uint32_t* pixels, int width, int height);

// RMaterial_t copy of `material` with `texture` on stage 0 (as DisplaySystem does for meshes).
void* CloneMaterialWithTexture(void* material, void* texture);

// DisplaySystem VisualMaterial_t {RMaterial_t* +0, RTexture_t* +4, w, h, format}: a texture plus a
// material that each start with one reference; the destructor releases both. A8R8G8B8, no data.
void* CreateVisualMaterial(int width, int height, const char* name);
void DestroyVisualMaterial(void* vm);
void* VisualMaterialMaterial(void* vm);
void* VisualMaterialTexture(void* vm);

// Sends one part of a VisualMaterial_t's texture: `rect` (texels, right/bottom exclusive) of an ARGB
// image `width` texels wide, as the GUI updates its surfaces (VisualMaterial_t::Modify -> the renderer's
// RTexture_t::Load with the rectangle). False if it isn't available.
bool ModifyVisualMaterial(void* vm, const uint32_t* pixels, int width, const RECT& rect);

// Sets the render states of a name label (unlit, alpha-blended) in an RMaterial_t's render-state block
// (+0x70, applied by RViewPort_t::SetMaterial), creating the block if the material has none. Depth is
// tested (unless onTop: drawn over everything) and never written.
bool MaterialSetLabelStates(void* material, bool onTop);

// RSprite (randy31) material at +0x204: the RSprite ctor AddRefs it and its dtor Releases it.
bool SwapSpriteMaterial(void* rsprite, void* material);

// DisplaySystem VisualSprite_t::SetInvisible (removes it from the scene root).
void VisualSpriteSetInvisible(void* visualSprite);

// The game window found by UpdateViewportSize (largest visible top-level window of the process).
HWND GameWindow();

// Called periodically from the init thread: refreshes the cached game window size.
void UpdateViewportSize();

} // namespace prk::game
