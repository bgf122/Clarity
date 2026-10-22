#include "game.h"

#include "config.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace prk::game {

namespace {

using GetWorldMatrixFn = const float*(__thiscall*)(void* self);
using DiaBillInitFn = void(__thiscall*)(void* self, const void* sprites, int count);
using SetRenderStateFn = void(__thiscall*)(void* self, int state, unsigned long value);
using GetDynelFn = void*(__cdecl*)(const Identity& id);
using N3InterfaceInstanceFn = void*(__cdecl*)();
using GetIndicatorPositionFn = const Vec3*(__thiscall*)(void* self, const Identity& id);

using TextureCtorFn = void*(__thiscall*)(void* self, const char* name, unsigned short w, unsigned short h,
                                         int format, bool mipmaps, void* palette);
using LockTextureFn = bool(__thiscall*)(void* self, void** bits, int lockType, unsigned char level);
using UnlockTextureFn = bool(__thiscall*)(void* self);
using TextureIntFn = unsigned(__thiscall*)(void* self);
using TextureShortFn = unsigned short(__thiscall*)(void* self);
using MaterialCopyFn = void*(__thiscall*)(void* self, const void* other);
using MaterialSetTextureFn = void(__thiscall*)(void* self, void* texture, unsigned char stage, int index);
using CrtNewFn = void*(__cdecl*)(size_t);
using ResourceRefFn = void(__thiscall*)(void* self);

constexpr int kDiaBillDeltaState = 0xB8; // GfxVisualDiaBill: RDeltaState* (set in its ctor)
constexpr int kDiaBillMaterial = 0x17C;  // GfxVisualDiaBill: RMaterial_t* (render: RViewPort_t::SetMaterial)
constexpr int kD3DX_SF_A8R8G8B8 = 2;     // RTexture_t formats are D3DX7 surface formats (4 = R5G6B5)
constexpr int kDDLOCK_WRITEONLY = 0x20;
constexpr size_t kSizeofRTexture = 0xBC;  // operator_new(0xbc) before RTexture_t ctors
constexpr size_t kSizeofRMaterial = 0xC0; // operator_new(0xc0) before RMaterial_t copy
constexpr size_t kSizeofVisualMaterial = 0x14; // material, texture, w, h, format
constexpr int kRSpriteMaterial = 0x204;         // RSprite ctor/dtor
constexpr int kMaterialDeltaState = 0x70;       // RMaterial_t::SetDeltaState (randy31.c, export 0x40a65)
constexpr size_t kSizeofRDeltaState = 0x16C;    // operator_new(0x16c) before RDeltaState("none")
constexpr int kD3DRS_ZWRITEENABLE = 14;
constexpr int kD3DRS_SRCBLEND = 19;
constexpr int kD3DRS_DESTBLEND = 20;
constexpr int kD3DRS_ALPHABLENDENABLE = 27;
constexpr int kD3DRS_LIGHTING = 137;
constexpr int kD3DBLEND_SRCALPHA = 5;
constexpr int kD3DBLEND_INVSRCALPHA = 6;

using DeltaStateCtorFn = void*(__thiscall*)(void* self, const char* name);
using SetDeltaStateFn = void(__thiscall*)(void* self, void* state);

using VisualMaterialCtorFn = void*(__thiscall*)(void* self, int w, int h, void* data, int format, char* name);
using ThisVoidFn = void(__thiscall*)(void* self);
using CrtDeleteFn = void(__cdecl*)(void*);
constexpr int kDynelIdentity = 0x14;     // n3Dynel_t: Identity_t (n3Dynel_t::PutInDir)
constexpr int kD3DRS_ZENABLE = 7;

const float* g_view = nullptr; // TMatrix4_t, 16 floats
const float* g_proj = nullptr;
GetWorldMatrixFn g_getWorldMatrix = nullptr;
DiaBillInitFn g_diaBillInit = nullptr;
SetRenderStateFn g_setRenderState = nullptr;
GetDynelFn g_getDynel = nullptr;
N3InterfaceInstanceFn g_n3Interface = nullptr;
GetIndicatorPositionFn g_getIndicatorPosition = nullptr;
using GetClientInstFn = uint32_t(__thiscall*)(void* self);
GetClientInstFn g_getClientInst = nullptr;
TextureCtorFn g_textureCtor = nullptr;
LockTextureFn g_lockTexture = nullptr;
UnlockTextureFn g_unlockTexture = nullptr;
TextureIntFn g_texturePitch = nullptr;
TextureIntFn g_textureFormat = nullptr;
TextureShortFn g_textureWidth = nullptr;
TextureShortFn g_textureHeight = nullptr;
MaterialCopyFn g_materialCopy = nullptr;
MaterialSetTextureFn g_materialSetTexture = nullptr;
CrtNewFn g_crtNew = nullptr;
DeltaStateCtorFn g_deltaStateCtor = nullptr;
SetDeltaStateFn g_materialSetDeltaState = nullptr;
VisualMaterialCtorFn g_visualMaterialCtor = nullptr;
ThisVoidFn g_visualMaterialDtor = nullptr;
ThisVoidFn g_spriteSetInvisible = nullptr;
CrtDeleteFn g_crtDelete = nullptr;
ResourceRefFn g_addRefResource = nullptr;  // RResource_t::AddRefRResource() const
ResourceRefFn g_releaseResource = nullptr; // RResource_t::ReleaseRResource() const
std::atomic<HWND> g_gameWindow{nullptr};
std::atomic<int> g_viewportWidth{0};
std::atomic<int> g_viewportHeight{0};

FARPROC Export(const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    FARPROC p = m ? GetProcAddress(m, name) : nullptr; // follows randy-vk's forwarders
    if (!p)
        Log("[game] missing export %s!%s", module, name);
    return p;
}

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-3f;
}

// D3D-style row-vector matrix: translation in elements 12..14, column 3 = (0,0,0,1).
// The transposed layout is accepted too, in case a renderer stores it that way.
bool IsRowVector(const float* m)
{
    return Near(m[3], 0.0f) && Near(m[7], 0.0f) && Near(m[11], 0.0f) && Near(m[15], 1.0f);
}

// Element (row, col) of a matrix applied as v' = v * M.
float At(const float* m, bool rowVector, int row, int col)
{
    return rowVector ? m[row * 4 + col] : m[col * 4 + row];
}

} // namespace

bool Init()
{
    g_view = reinterpret_cast<const float*>(Export("randy31.dll", "?m_CurrentViewMatrix@RViewPort_t@@0VTMatrix4_t@@A"));
    g_proj = reinterpret_cast<const float*>(Export("randy31.dll", "?m_CurrentProjectionMatrix@RViewPort_t@@0VTMatrix4_t@@A"));
    g_getWorldMatrix = reinterpret_cast<GetWorldMatrixFn>(Export("randy31.dll", "?GetWorldMatrix@RRefFrame_t@@QBEABVTMatrix4_t@@XZ"));
    g_setRenderState = reinterpret_cast<SetRenderStateFn>(Export("randy31.dll", "?SetRenderState@RDeltaState@@QAEXW4_D3DRENDERSTATETYPE@@K@Z"));
    g_diaBillInit = reinterpret_cast<DiaBillInitFn>(Export("DisplaySystem.dll", "?Init@GfxVisualDiaBill@@QAEXPBUSprite_t@DiaBill_n@@H@Z"));
    g_getDynel = reinterpret_cast<GetDynelFn>(Export("N3.dll", "?GetDynel@n3Dynel_t@@SAPAV1@ABVIdentity_t@@@Z"));
    g_n3Interface = reinterpret_cast<N3InterfaceInstanceFn>(Export("Interfaces.dll", "?GetInstance@N3InterfaceModule_t@@SAPAV1@XZ"));
    g_getIndicatorPosition = reinterpret_cast<GetIndicatorPositionFn>(
        Export("Interfaces.dll", "?N3Msg_GetIndicatorPosition@N3InterfaceModule_t@@QBEABVVector3_t@@ABVIdentity_t@@@Z"));
    g_textureCtor = reinterpret_cast<TextureCtorFn>(Export("randy31.dll", "??0RTexture_t@@QAE@PBDGGW4TextureFormat_e@0@_NPAX@Z"));
    g_lockTexture = reinterpret_cast<LockTextureFn>(Export("randy31.dll", "?LockRTexture@RTexture_t@@QAE_NAAPAXW4TextureLockType_e@1@E@Z"));
    g_unlockTexture = reinterpret_cast<UnlockTextureFn>(Export("randy31.dll", "?UnlockRTexture@RTexture_t@@QAE_NXZ"));
    g_texturePitch = reinterpret_cast<TextureIntFn>(Export("randy31.dll", "?GetPitch@RTexture_t@@QBEIXZ"));
    g_textureFormat = reinterpret_cast<TextureIntFn>(Export("randy31.dll", "?GetTextureFormat@RTexture_t@@QBE?AW4TextureFormat_e@1@XZ"));
    g_textureWidth = reinterpret_cast<TextureShortFn>(Export("randy31.dll", "?GetWidth@RTexture_t@@QBEGXZ"));
    g_textureHeight = reinterpret_cast<TextureShortFn>(Export("randy31.dll", "?GetHeight@RTexture_t@@QBEGXZ"));
    g_materialCopy = reinterpret_cast<MaterialCopyFn>(Export("randy31.dll", "??0RMaterial_t@@QAE@ABV0@@Z"));
    g_materialSetTexture = reinterpret_cast<MaterialSetTextureFn>(Export("randy31.dll", "?SetTexture@RMaterial_t@@QAEXPAVRTexture_t@@EH@Z"));
    g_crtNew = reinterpret_cast<CrtNewFn>(Export("msvcr100.dll", "??2@YAPAXI@Z"));
    g_getClientInst = reinterpret_cast<GetClientInstFn>(Export("Interfaces.dll", "?GetClientInst@N3InterfaceModule_t@@QBEIXZ"));
    g_visualMaterialCtor = reinterpret_cast<VisualMaterialCtorFn>(Export("DisplaySystem.dll", "??0VisualMaterial_t@@QAE@HHPAXW4_D3DX_SURFACEFORMAT@@PAD@Z"));
    g_visualMaterialDtor = reinterpret_cast<ThisVoidFn>(Export("DisplaySystem.dll", "??1VisualMaterial_t@@QAE@XZ"));
    g_spriteSetInvisible = reinterpret_cast<ThisVoidFn>(Export("DisplaySystem.dll", "?SetInvisible@VisualSprite_t@@QAEXXZ"));
    g_crtDelete = reinterpret_cast<CrtDeleteFn>(Export("msvcr100.dll", "??3@YAXPAX@Z"));
    g_deltaStateCtor = reinterpret_cast<DeltaStateCtorFn>(Export("randy31.dll", "??0RDeltaState@@QAE@PBD@Z"));
    g_materialSetDeltaState = reinterpret_cast<SetDeltaStateFn>(Export("randy31.dll", "?SetDeltaState@RMaterial_t@@QAEXPAVRDeltaState@@@Z"));
    g_addRefResource = reinterpret_cast<ResourceRefFn>(Export("randy31.dll", "?AddRefRResource@RResource_t@@QBEXXZ"));
    g_releaseResource = reinterpret_cast<ResourceRefFn>(Export("randy31.dll", "?ReleaseRResource@RResource_t@@QBEXXZ"));
    UpdateViewportSize();
    Log("[game] view=%p proj=%p GetWorldMatrix=%p DiaBillInit=%p SetRenderState=%p GetDynel=%p Indicator=%p viewport=%dx%d",
        g_view, g_proj, g_getWorldMatrix, g_diaBillInit, g_setRenderState, g_getDynel, g_getIndicatorPosition,
        g_viewportWidth.load(), g_viewportHeight.load());
    // The anchor/depth extras are optional; the core needs only the first four.
    return g_view && g_proj && g_getWorldMatrix && g_diaBillInit;
}

bool WorldPosition(void* refFrame, Vec3* out)
{
    const float* m = g_getWorldMatrix(refFrame);
    if (!m)
        return false;
    if (IsRowVector(m))
        *out = {m[12], m[13], m[14]};
    else
        *out = {m[3], m[7], m[11]};
    return true;
}

Vec3 ToView(const Vec3& p)
{
    const float* v = g_view;
    const bool vr = IsRowVector(v);
    float r[3];
    for (int c = 0; c < 3; ++c)
        r[c] = p.x * At(v, vr, 0, c) + p.y * At(v, vr, 1, c) + p.z * At(v, vr, 2, c) + At(v, vr, 3, c);
    return {r[0], r[1], r[2]};
}

Vec3 WorldDirToView(const Vec3& d)
{
    const float* v = g_view;
    const bool vr = IsRowVector(v);
    float r[3];
    for (int c = 0; c < 3; ++c)
        r[c] = d.x * At(v, vr, 0, c) + d.y * At(v, vr, 1, c) + d.z * At(v, vr, 2, c);
    return {r[0], r[1], r[2]};
}

void ViewportSize(int* width, int* height)
{
    int w = g_viewportWidth.load(std::memory_order_relaxed);
    int h = g_viewportHeight.load(std::memory_order_relaxed);
    if (w <= 0 || h <= 0) {
        w = 1024;
        h = 768;
    }
    *width = w;
    *height = h;
}

namespace {

// Perspective scale factors: _11 = cot(fovX/2), _22 = cot(fovY/2). Same index in both layouts.
void ProjScale(float* sx, float* sy)
{
    float x = g_proj[0];
    float y = g_proj[5];
    static bool warned = false;
    if (!(y > 0.05f && y < 50.0f) || !(x > 0.05f && x < 50.0f)) {
        if (!warned) {
            Log("[game] projection matrix looks invalid (_11=%f _22=%f), assuming 60 deg fov", x, y);
            warned = true;
        }
        int w, h;
        ViewportSize(&w, &h);
        y = 1.7320508f;
        x = y * static_cast<float>(h) / static_cast<float>(w);
    }
    *sx = x;
    *sy = y;
}

} // namespace

bool ViewToScreen(const Vec3& v, float* sx, float* sy)
{
    if (v.z < 0.05f)
        return false;
    float px, py;
    ProjScale(&px, &py);
    int w, h;
    ViewportSize(&w, &h);
    *sx = (v.x * px / v.z * 0.5f + 0.5f) * static_cast<float>(w);
    *sy = (0.5f - v.y * py / v.z * 0.5f) * static_cast<float>(h);
    return true;
}

Vec3 ScreenToView(float sx, float sy, float depth)
{
    float px, py;
    ProjScale(&px, &py);
    int w, h;
    ViewportSize(&w, &h);
    const float ndcX = sx / static_cast<float>(w) * 2.0f - 1.0f;
    const float ndcY = 1.0f - sy / static_cast<float>(h) * 2.0f;
    return {ndcX * depth / px, ndcY * depth / py, depth};
}

float PxPerUnit(float depth)
{
    float px, py;
    ProjScale(&px, &py);
    int w, h;
    ViewportSize(&w, &h);
    return py * 0.5f * static_cast<float>(h) / (depth > 0.05f ? depth : 0.05f);
}

uint32_t ClientInstance()
{
    if (!g_getClientInst || !g_n3Interface)
        return 0;
    void* module = g_n3Interface();
    return module ? g_getClientInst(module) : 0;
}

void* DynelById(const Identity& id)
{
    return g_getDynel ? g_getDynel(id) : nullptr;
}

bool IdentityOf(void* dynel, Identity* out)
{
    if (!dynel || !g_getDynel)
        return false;
    const Identity id = *reinterpret_cast<const Identity*>(static_cast<uint8_t*>(dynel) + kDynelIdentity);
    if (g_getDynel(id) != dynel) {
        static bool warned = false;
        if (!warned) {
            Log("[game] dynel %p: identity at +0x%X (%u:%u) doesn't map back - name-tag anchoring off",
                dynel, kDynelIdentity, id.type, id.instance);
            warned = true;
        }
        return false;
    }
    *out = id;
    return true;
}

bool IndicatorPosition(const Identity& id, Vec3* out)
{
    if (!g_getIndicatorPosition || !g_n3Interface || !g_getDynel)
        return false;
    // The indicator lookup itself also tolerates a missing dynel, but then returns a dummy
    // (0, 2, 0); checking first keeps us from mistaking that for a real position.
    if (!g_getDynel(id))
        return false;
    void* module = g_n3Interface();
    if (!module)
        return false;
    const Vec3* p = g_getIndicatorPosition(module, id);
    if (!p || (p->x == 0.0f && p->y == 2.0f && p->z == 0.0f))
        return false;
    *out = *p;
    return true;
}

void DiaBillInit(void* visual, const void* sprites, int count)
{
    g_diaBillInit(visual, sprites, count);
}

bool DiaBillDisableDepthTest(void* visual)
{
    if (!g_setRenderState)
        return false;
    void* state = *reinterpret_cast<void**>(static_cast<uint8_t*>(visual) + kDiaBillDeltaState);
    if (!state)
        return false;
    g_setRenderState(state, kD3DRS_ZENABLE, 0);
    return true;
}

void* DiaBillMaterial(void* visual)
{
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(visual) + kDiaBillMaterial);
}

bool SwapDiaBillMaterial(void* visual, void* material)
{
    if (!g_addRefResource || !g_releaseResource || !material)
        return false;
    void*& slot = *reinterpret_cast<void**>(static_cast<uint8_t*>(visual) + kDiaBillMaterial);
    g_addRefResource(material);
    if (slot)
        g_releaseResource(slot);
    slot = material;
    return true;
}

void* CreateTextureARGB(const char* name, int width, int height)
{
    if (!g_textureCtor || !g_crtNew || !g_textureFormat || !g_texturePitch || !g_textureWidth || !g_textureHeight)
        return nullptr;
    void* mem = g_crtNew(kSizeofRTexture);
    if (!mem)
        return nullptr;
    memset(mem, 0, kSizeofRTexture);
    void* tex = g_textureCtor(mem, name, static_cast<unsigned short>(width), static_cast<unsigned short>(height),
                              kD3DX_SF_A8R8G8B8, false, nullptr);
    if (!tex)
        return nullptr;
    const unsigned fmt = g_textureFormat(tex);
    const unsigned pitch = g_texturePitch(tex);
    const int w = g_textureWidth(tex);
    const int h = g_textureHeight(tex);
    Log("[game] texture '%s': %dx%d format %u pitch %u", name, w, h, fmt, pitch);
    // D3DX may substitute a format or size; we only write exact A8R8G8B8 at the requested size.
    if (fmt != kD3DX_SF_A8R8G8B8 || w != width || h != height || pitch < static_cast<unsigned>(width) * 4)
        return nullptr;
    return tex;
}

bool WriteTexture(void* texture, const uint32_t* pixels, int width, int height)
{
    if (!g_lockTexture || !g_unlockTexture)
        return false;
    void* bits = nullptr;
    if (!g_lockTexture(texture, &bits, kDDLOCK_WRITEONLY, 0) || !bits)
        return false;
    const unsigned pitch = g_texturePitch(texture);
    for (int y = 0; y < height; ++y)
        memcpy(static_cast<uint8_t*>(bits) + static_cast<size_t>(y) * pitch, pixels + static_cast<size_t>(y) * width,
               static_cast<size_t>(width) * 4);
    g_unlockTexture(texture);
    return true;
}

void* CloneMaterialWithTexture(void* material, void* texture)
{
    if (!material || !texture || !g_materialCopy || !g_materialSetTexture || !g_crtNew)
        return nullptr;
    void* mem = g_crtNew(kSizeofRMaterial);
    if (!mem)
        return nullptr;
    void* clone = g_materialCopy(mem, material);
    if (clone) {
        g_materialSetTexture(clone, texture, 0, -1);
        // Our own permanent reference: the clone must outlive every visual that uses it.
        if (g_addRefResource)
            g_addRefResource(clone);
    }
    return clone;
}

void* CreateVisualMaterial(int width, int height, const char* name)
{
    if (!g_visualMaterialCtor || !g_crtNew)
        return nullptr;
    void* mem = g_crtNew(kSizeofVisualMaterial);
    if (!mem)
        return nullptr;
    memset(mem, 0, kSizeofVisualMaterial);
    void* vm = g_visualMaterialCtor(mem, width, height, nullptr, kD3DX_SF_A8R8G8B8, const_cast<char*>(name));
    if (!vm || !VisualMaterialMaterial(vm) || !VisualMaterialTexture(vm))
        return nullptr;
    void* tex = VisualMaterialTexture(vm);
    if (g_textureFormat(tex) != kD3DX_SF_A8R8G8B8 || g_textureWidth(tex) != width || g_textureHeight(tex) != height) {
        DestroyVisualMaterial(vm);
        return nullptr;
    }
    return vm;
}

void DestroyVisualMaterial(void* vm)
{
    if (!vm)
        return;
    if (g_visualMaterialDtor)
        g_visualMaterialDtor(vm);
    if (g_crtDelete)
        g_crtDelete(vm);
}

void* VisualMaterialMaterial(void* vm)
{
    return static_cast<void**>(vm)[0];
}

void* VisualMaterialTexture(void* vm)
{
    return static_cast<void**>(vm)[1];
}

bool MaterialSetLabelStates(void* material, bool onTop)
{
    if (!material || !g_setRenderState)
        return false;
    void* state = *reinterpret_cast<void**>(static_cast<uint8_t*>(material) + kMaterialDeltaState);
    if (!state) {
        if (!g_deltaStateCtor || !g_materialSetDeltaState || !g_crtNew || !g_releaseResource)
            return false;
        void* mem = g_crtNew(kSizeofRDeltaState);
        if (!mem)
            return false;
        memset(mem, 0, kSizeofRDeltaState);
        state = g_deltaStateCtor(mem, "none");
        if (!state)
            return false;
        g_materialSetDeltaState(material, state); // AddRefs it
        g_releaseResource(state);                 // drop our construction reference
    }
    // Unlit and alpha-blended, like the game's own name labels: ao-vk recognises labels by these states (plus a
    // 4-vertex quad with a texture at least twice as wide as tall) and draws them after its shadow, ambient
    // occlusion and depth-of-field passes. Without them the plate is ordinary scene geometry and those passes
    // darken or blur it.
    g_setRenderState(state, kD3DRS_LIGHTING, 0);
    g_setRenderState(state, kD3DRS_ALPHABLENDENABLE, 1);
    g_setRenderState(state, kD3DRS_SRCBLEND, kD3DBLEND_SRCALPHA);
    g_setRenderState(state, kD3DRS_DESTBLEND, kD3DBLEND_INVSRCALPHA);
    if (onTop) {
        g_setRenderState(state, kD3DRS_ZENABLE, 0);
        g_setRenderState(state, kD3DRS_ZWRITEENABLE, 0);
    }
    return true;
}

bool SwapSpriteMaterial(void* rsprite, void* material)
{
    if (!rsprite || !material || !g_addRefResource || !g_releaseResource)
        return false;
    void*& slot = *reinterpret_cast<void**>(static_cast<uint8_t*>(rsprite) + kRSpriteMaterial);
    if (slot == material)
        return true;
    g_addRefResource(material);
    if (slot)
        g_releaseResource(slot);
    slot = material;
    return true;
}

void VisualSpriteSetInvisible(void* visualSprite)
{
    if (g_spriteSetInvisible && visualSprite)
        g_spriteSetInvisible(visualSprite);
}

HWND GameWindow()
{
    return g_gameWindow.load();
}

void UpdateViewportSize()
{
    struct Best {
        DWORD pid;
        int area;
        int width;
        int height;
        HWND hwnd;
    } best{GetCurrentProcessId(), 0, 0, 0, nullptr};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* b = reinterpret_cast<Best*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != b->pid || !IsWindowVisible(hwnd))
                return TRUE;
            RECT rc;
            if (GetClientRect(hwnd, &rc)) {
                int area = (rc.right - rc.left) * (rc.bottom - rc.top);
                if (area > b->area) {
                    b->area = area;
                    b->width = rc.right - rc.left;
                    b->height = rc.bottom - rc.top;
                    b->hwnd = hwnd;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));

    if (best.height > 0) {
        g_gameWindow.store(best.hwnd);
        g_viewportWidth.store(best.width, std::memory_order_relaxed);
        g_viewportHeight.store(best.height, std::memory_order_relaxed);
    }
}

} // namespace prk::game
