// The minimap ([Minimap]): a round map in a Clarity window.
//
// Drawing: a plain GUI View (GUI.dll exports) whose vtable is a copy of View's with Render replaced.
// View::Render(RViewPort_t*, float alpha, unsigned) is virtual and empty; the game's local map
// (PFMapGroundRenderer_c, GUI 0x10046B79) overrides the same slot and draws in it with render_t:
// screen-space triangle fans (FVF XYZRHW | DIFFUSE | TEX1, z 0.01, rhw 100), textures bound with
// DeviceState::SetTexture(viewport+8 -> its DeviceState, RTexture_t+0x30 -> its surface, stage, 10),
// then RViewPort_t::RealizeRenderStates, render states in StateBlob_c blocks (all randy31 exports).
// Clarity does the same, so the minimap draws under any renderer the game's map draws under.
//
// The map (as the game's local map, GUI 0x10046E28): MapData_c (the GUI's singleton, SIG_MAPDATA_GET)
// keeps the dungeon's rooms, a std::vector at +0x1C..+0x20 of 0x44-byte entries (built by 0x10042CC4
// from n3GroundRenderer_t::CreateDungeonRoom): +0x04 ARGB, +0x08 position, +0x14 offset, +0x20 rotation
// (quaternion about +y, quarter turns), +0x30 / +0x34 tiles across / down, +0x38 floor, +0x3C tile size,
// +0x40 the tiles' textures (DbObject_t*, vtable +0x38 -> an AddRef'd RTexture_t, or null). Tile (i, j)
// is the square of the tile size around pos + R*offset + R*(i*size, 0, j*size), texture unrotated (v
// up the map). Only the rooms on MapData +0x2C's floor are drawn; a black room only with +0x82 (the
// whole map known), in grey. Texture x colour x 2. North (+z) is up.
//
// Outdoors (MapData +0x08, the ground): see Ground().
#include "minimap.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/n3.h"
#include "../../core/signatures.h"
#include "../../core/text_raster.h"
#include "../../core/wndproc.h"
#include "../pfmap/pfmap.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace prk::minimap {

namespace {

// --- the game's GUI and renderer ------------------------------------------------------------

// Rect / Point (Utils.dll): plain floats, right and bottom inclusive; returned through a hidden pointer.
struct GuiRect {
    float left, top, right, bottom;
};
struct GuiPoint {
    float x, y;
};

using CrtNewFn = void*(__cdecl*)(size_t);
using CrtDeleteFn = void(__cdecl*)(void*);
using StringCtorFn = void*(__thiscall*)(void* self, const char* s);
using StringDtorFn = void(__thiscall*)(void* self);
using InstanceFn = void*(__cdecl*)();
using WindowCtorFn = void*(__thiscall*)(void* self, const GuiRect* frame, const void* name, const void* title, int style, unsigned flags);
using WindowShowFn = void(__thiscall*)(void* self, bool show);
using WindowIsVisibleFn = bool(__thiscall*)(const void* self);
using WindowAddChildFn = void(__thiscall*)(void* self, void* view, bool);
using WindowMoveToFn = void(__thiscall*)(void* self, float x, float y);
using WindowResizeToFn = void(__thiscall*)(void* self, float w, float h);
using WindowSizeLimitsFn = void(__thiscall*)(void* self, const GuiPoint* min, const GuiPoint* max);
using ScreenSizeFn = GuiPoint*(__cdecl*)(GuiPoint* ret);
using MouseWindowFn = void*(__thiscall*)(void* self);
using ViewCtorFn = void*(__thiscall*)(void* self, const GuiRect* frame, const void* name, unsigned resizeMask, unsigned flags);
using ViewSetFrameFn = void(__thiscall*)(void* self, const GuiRect* frame, bool);
using ViewGetBoundsFn = GuiRect*(__thiscall*)(const void* self, GuiRect* ret);
using ViewToScreenFn = GuiRect*(__thiscall*)(const void* self, GuiRect* ret, const GuiRect* rect, bool);

using BlobCtorFn = void*(__thiscall*)(void* self);
using BlobFn = void(__thiscall*)(void* self);
using BlobValidateFn = bool(__thiscall*)(void* self);
using BlobSetRsFn = void(__thiscall*)(void* self, int state, unsigned value);
using BlobSetTssFn = void(__thiscall*)(void* self, unsigned stage, int state, unsigned value);
using SetTextureFn = bool(__thiscall*)(void* deviceState, void* surface, unsigned stage, int priority);
using ViewportFn = void(__thiscall*)(void* viewport);
using RenderFn = void(__thiscall*)(void* render, unsigned long fvf, void* vertices, unsigned long count, unsigned long flags);

CrtNewFn CrtNew;
CrtDeleteFn CrtDelete;
StringCtorFn StringCtor;
StringDtorFn StringDtor;
InstanceFn WindowController; // WindowController_c::GetInstanceIfAny
InstanceFn ControlCenter;    // ControlCenterModule_c::GetInstanceIfAny
WindowCtorFn WindowCtor;
WindowShowFn WindowShow;
WindowIsVisibleFn WindowIsVisible;
WindowAddChildFn WindowAddChild;
WindowMoveToFn WindowMoveTo;
WindowResizeToFn WindowResizeTo;
WindowSizeLimitsFn WindowSizeLimits;
ScreenSizeFn ScreenSize;
MouseWindowFn MouseWindow; // WindowController_c::GetMouseWindow
using EvaluateFn = bool(__thiscall*)(void* self);
EvaluateFn CriteriaEvaluate; // CriteriaMonitor_c::Evaluate (optional)
const uint8_t* g_guiBase = nullptr;
size_t g_guiSize = 0;
ViewCtorFn ViewCtor;
ViewSetFrameFn ViewSetFrame;
ViewGetBoundsFn ViewGetBounds;
ViewToScreenFn ViewToScreen;
void* ViewRender; // View::Render: the vtable slot replaced

BlobCtorFn BlobCtor;
BlobFn BlobSet;
BlobFn BlobReset;
BlobValidateFn BlobValidate;
BlobSetRsFn BlobSetRs;
BlobSetTssFn BlobSetTss;
SetTextureFn DeviceSetTexture;
ViewportFn RealizeStates;
ViewportFn ResetMaterial;
void** RenderInstance; // render_t::m_pcInstance
RenderFn RenderFan;
using ReleaseFn = void(__thiscall*)(const void* self);
ReleaseFn ReleaseTexture; // RTexture_t::ReleaseRTexture
using N3InstanceFn = void*(__cdecl*)();
using CharVecFn = void(__thiscall*)(const void* n3, float* out);
N3InstanceFn N3Instance;     // N3InterfaceModule_t::GetInstance
CharVecFn CharPosition;      // N3Msg_GetGlobalCharacterPosition(Vector3_t&)
CharVecFn CharRotation;      // N3Msg_GetGlobalCharacterRotation(Quaternion_t&): x, y, z, w
using MapCharsFn = bool(__thiscall*)(const void* n3, int range, void* list, int* count);
MapCharsFn MapCharacters;    // N3Msg_GetMapCharacters(range, std::list<MapCharacterEntry_t>*, int&)
using PositionDataFn = bool(__thiscall*)(const void* n3, const game::Identity* who, game::Identity* pf, float* pos);
PositionDataFn PositionData; // N3Msg_GetPositionData(who, playfield&, position&)
using GetDValueFn = void*(__cdecl*)(void* ret, const void* name, bool);
using VariantStringFn = void*(__thiscall*)(const void* self, void* ret);
using VariantDtorFn = void(__thiscall*)(void* self);
GetDValueFn GetDValue;          // DistributedValue_c::GetDValue(String const&, bool) -> Variant
VariantStringFn VariantString;  // Variant::AsString() -> std::string
VariantDtorFn VariantDtor;
using VariantBoolFn = void*(__thiscall*)(void* self, bool v);
using VariantAsBoolFn = bool(__thiscall*)(const void* self);
using SetDValueFn = void(__cdecl*)(const void* name, const void* value);
VariantBoolFn VariantBool;      // Variant::Variant(bool)
VariantAsBoolFn VariantAsBool;  // Variant::AsBool
SetDValueFn SetDValue;          // DistributedValue_c::SetDValue(String const&, Variant const&)
using GuiTextureFn = void*(__thiscall*)(void* self, int id, int format);
using GuiReleaseFn = void(__thiscall*)(void* self, void* sprite);
using SpriteDataFn = const uint8_t*(__thiscall*)(void* self);
using SpriteIntFn = int(__thiscall*)(const void* self);
InstanceFn GuiResources;      // GuiResourceManager_t::GetInstance
GuiTextureFn GuiTexture;      // GuiResourceManager_t::GetGuiTexture(id, Format_e)
GuiReleaseFn GuiRelease;      // GuiResourceManager_t::ReleaseTexture
SpriteDataFn SpriteData;
SpriteIntFn SpriteWidth, SpriteHeight, SpriteStride, SpriteBpp, SpriteFormat;
using AllocTextureFn = void*(__cdecl*)(int w, int h, bool mips, int format, void* palette);
using TextureLoadFn = bool(__thiscall*)(void* self, const void* data, int format, unsigned pitch, const RECT* rect,
                                        void* palette, bool);
AllocTextureFn AllocateTexture; // DisplaySystem AllocateTexture(w, h, mips, D3DX format, palette)
TextureLoadFn TextureLoad;      // RTexture_t::Load(data, format, pitch, rect, palette, bool)
using ShadowTextureFn = void*(__thiscall*)(const void* ground, int x, int z);
using HiTextureFn = void(__thiscall*)(const void* data, const void* patch);
using BuildingMapFn = void(__thiscall*)(void* patch, bool, void* palette);
ShadowTextureFn GroundShadow;  // AnarchyGround_t::GetShadowTexture(x, z): a patch's lightmap
HiTextureFn GroundHiTexture;   // AnarchyGroundData_t::CreateHiTexture(patch)
BuildingMapFn BuildingMap;     // AnarchyGroundData_t::Patch_t::CreateBuildingMap(bool, palette)

// As the bars: Window 0x74 bytes and View 0x128 (allocated larger), StateBlob_c 0x84 (the local map's
// blocks are 0x84 apart). Window style and flags as CharBarWindow_c; the view resizes with the window.
constexpr size_t kWindowAlloc = 0x200;
constexpr size_t kViewAlloc = 0x200;
constexpr size_t kBlobAlloc = 0x100;
constexpr int kWindowStyle = 3;
constexpr unsigned kWindowFlags = 0xE3C;
constexpr unsigned kViewResize = 0xF;
constexpr unsigned kViewFlags = 2;
constexpr int kWcWindowList = 0x88; // WindowController_c: std::list<Window*>
constexpr int kCompassCriteria = 0x74; // CompassWindow_c: CriteriaMonitor_c (its show condition)
constexpr char kCompassClass[] = ".?AVCompassWindow_c@@";
constexpr int kCcAlive = 0xB8;      // ControlCenterModule_c: the GUI was made
constexpr int kViewportDeviceState = 0x08; // RViewPort_t: its DeviceState
constexpr int kTextureSurface = 0x30;      // RTexture_t: its surface_t
constexpr int kVtableSlots = 64;           // View's vtable is shorter; only Render's slot is looked for

// D3D7 render and texture stage states.
enum : int {
    RS_ZENABLE = 7, RS_ZWRITEENABLE = 14, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22,
    RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_LIGHTING = 137,
    TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
    TSS_ADDRESS = 12,
};
enum : unsigned {
    BLEND_ZERO = 1, BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6, BLEND_DESTCOLOR = 9, CULL_NONE = 1,
    TOP_MODULATE = 4, TOP_MODULATE2X = 5, TOP_SELECTARG2 = 3, TA_DIFFUSE = 0, TA_TEXTURE = 2, TADDRESS_CLAMP = 3,
};
constexpr unsigned long kFvf = 0x144; // XYZRHW | DIFFUSE | TEX1, as the local map
constexpr float kZ = 0.01f, kRhw = 100.0f;

struct Vertex {
    float x, y, z, rhw;
    uint32_t argb;
    float u, v;
};
static_assert(sizeof(Vertex) == 28, "FVF 0x144 vertex");

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[minimap] missing export %s!%s", module, name);
    return fn != nullptr;
}

bool ResolveAll()
{
    const char* gui = "GUI.dll";
    const char* randy = "randy31.dll";
    bool ok = true;
    ok &= Resolve(CrtNew, "msvcr100.dll", "??2@YAPAXI@Z");
    ok &= Resolve(CrtDelete, "msvcr100.dll", "??3@YAXPAX@Z");
    ok &= Resolve(StringCtor, "Utils.dll", "??0String@@QAE@PBD@Z");
    ok &= Resolve(StringDtor, "Utils.dll", "??1String@@QAE@XZ");
    ok &= Resolve(WindowController, gui, "?GetInstanceIfAny@WindowController_c@@SAPAV1@XZ");
    ok &= Resolve(ControlCenter, gui, "?GetInstanceIfAny@ControlCenterModule_c@@SAPAV1@XZ");
    ok &= Resolve(WindowCtor, gui, "??0Window@@QAE@ABVRect@@ABVString@@1W4WindowStyle_e@@I@Z");
    ok &= Resolve(WindowShow, gui, "?Show@Window@@QAEX_N@Z");
    ok &= Resolve(WindowIsVisible, gui, "?IsVisible@Window@@QBE_NXZ");
    ok &= Resolve(WindowAddChild, gui, "?AddChild@Window@@QAEXPAVView@@_N@Z");
    ok &= Resolve(WindowMoveTo, gui, "?MoveTo@Window@@QAEXMM@Z");
    ok &= Resolve(WindowResizeTo, gui, "?ResizeTo@Window@@QAEXMM@Z");
    ok &= Resolve(WindowSizeLimits, gui, "?SetSizeLimits@Window@@QAEXABVPoint@@0@Z");
    ok &= Resolve(ScreenSize, gui, "?GetScreenSize@Window@@SA?AVPoint@@XZ");
    ok &= Resolve(MouseWindow, gui, "?GetMouseWindow@WindowController_c@@QAEPAVWindow@@XZ");
    ok &= Resolve(ViewCtor, gui, "??0View@@QAE@ABVRect@@ABVString@@II@Z");
    ok &= Resolve(ViewSetFrame, gui, "?SetFrame@View@@UAEXABVRect@@_N@Z");
    ok &= Resolve(ViewGetBounds, gui, "?GetBounds@View@@QBE?AVRect@@XZ");
    ok &= Resolve(ViewToScreen, gui, "?ConvertToScreen@View@@QBE?AVRect@@ABV2@_N@Z");
    ok &= Resolve(ViewRender, gui, "?Render@View@@UAEXPAVRViewPort_t@@MI@Z");
    ok &= Resolve(BlobCtor, randy, "??0StateBlob_c@@QAE@XZ");
    ok &= Resolve(BlobSet, randy, "?Set@StateBlob_c@@QAEXXZ");
    ok &= Resolve(BlobReset, randy, "?Reset@StateBlob_c@@QAEXXZ");
    ok &= Resolve(BlobValidate, randy, "?Validate@StateBlob_c@@QAE_NXZ");
    ok &= Resolve(BlobSetRs, randy, "?SetRenderState@StateBlob_c@@QAEXW4_D3DRENDERSTATETYPE@@K@Z");
    ok &= Resolve(BlobSetTss, randy, "?SetTextureStageState@StateBlob_c@@QAEXIW4_D3DTEXTURESTAGESTATETYPE@@K@Z");
    ok &= Resolve(DeviceSetTexture, randy, "?SetTexture@DeviceState@@QAE_NPAVsurface_t@@IW4Priority_e@1@@Z");
    ok &= Resolve(RealizeStates, randy, "?RealizeRenderStates@RViewPort_t@@QAEXXZ");
    ok &= Resolve(ResetMaterial, randy, "?ResetMaterial@RViewPort_t@@QAEXXZ");
    ok &= Resolve(RenderInstance, randy, "?m_pcInstance@render_t@@0PAV1@A");
    ok &= Resolve(RenderFan, randy, "?RenderTriangleFan@render_t@@QAEXKPAXKK@Z");
    ok &= Resolve(ReleaseTexture, randy, "?ReleaseRTexture@RTexture_t@@QBEXXZ");
    ok &= Resolve(N3Instance, "Interfaces.dll", "?GetInstance@N3InterfaceModule_t@@SAPAV1@XZ");
    ok &= Resolve(CharPosition, "Interfaces.dll", "?N3Msg_GetGlobalCharacterPosition@N3InterfaceModule_t@@QBEXAAVVector3_t@@@Z");
    ok &= Resolve(CharRotation, "Interfaces.dll", "?N3Msg_GetGlobalCharacterRotation@N3InterfaceModule_t@@QBEXAAVQuaternion_t@@@Z");
    ok &= Resolve(MapCharacters, "Interfaces.dll",
                  "?N3Msg_GetMapCharacters@N3InterfaceModule_t@@QBE_NHPAV?$list@VMapCharacterEntry_t@@V?$allocator@VMapCharacterEntry_t@@@std@@@std@@AAH@Z");
    ok &= Resolve(PositionData, "Interfaces.dll", "?N3Msg_GetPositionData@N3InterfaceModule_t@@QBE_NABVIdentity_t@@AAV2@AAVVector3_t@@@Z");
    ok &= Resolve(GetDValue, "Utils.dll", "?GetDValue@DistributedValue_c@@SA?AVVariant@@ABVString@@_N@Z");
    ok &= Resolve(VariantString, "Utils.dll", "?AsString@Variant@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    ok &= Resolve(VariantDtor, "Utils.dll", "??1Variant@@QAE@XZ");
    ok &= Resolve(VariantBool, "Utils.dll", "??0Variant@@QAE@_N@Z");
    ok &= Resolve(VariantAsBool, "Utils.dll", "?AsBool@Variant@@QBE_NXZ");
    ok &= Resolve(SetDValue, "Utils.dll", "?SetDValue@DistributedValue_c@@SAXABVString@@ABVVariant@@@Z");
    ok &= Resolve(GuiResources, "Interfaces.dll", "?GetInstance@GuiResourceManager_t@@SAPAV1@XZ");
    ok &= Resolve(GuiTexture, "Interfaces.dll", "?GetGuiTexture@GuiResourceManager_t@@QAEPAVSpriteInfo_t@@HW4Format_e@@@Z");
    ok &= Resolve(GuiRelease, "Interfaces.dll", "?ReleaseTexture@GuiResourceManager_t@@QAEXPAVSpriteInfo_t@@@Z");
    ok &= Resolve(SpriteData, "DisplaySystem.dll", "?GetData@SpriteInfo_t@@QAEPAEXZ");
    ok &= Resolve(SpriteWidth, "DisplaySystem.dll", "?GetWidth@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteHeight, "DisplaySystem.dll", "?GetHeight@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteStride, "DisplaySystem.dll", "?GetBytesPerLine@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteBpp, "DisplaySystem.dll", "?GetBytesPerPixel@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteFormat, "DisplaySystem.dll", "?GetSurfaceFormat@SpriteInfo_t@@QBE?AW4_D3DX_SURFACEFORMAT@@XZ");
    ok &= Resolve(AllocateTexture, "DisplaySystem.dll", "?AllocateTexture@@YAPAVRTexture_t@@HH_NW4_D3DX_SURFACEFORMAT@@PAUIDirectDrawPalette@@@Z");
    ok &= Resolve(TextureLoad, randy, "?Load@RTexture_t@@QAE_NPBXW4TextureFormat_e@1@IPAUtagRECT@@PAX_N@Z");
    ok &= Resolve(GroundShadow, "DisplaySystem.dll", "?GetShadowTexture@AnarchyGround_t@@QBEPBVRTexture_t@@HH@Z");
    ok &= Resolve(GroundHiTexture, "DisplaySystem.dll", "?CreateHiTexture@AnarchyGroundData_t@@QBEXPBX@Z");
    ok &= Resolve(BuildingMap, "DisplaySystem.dll", "?CreateBuildingMap@Patch_t@AnarchyGroundData_t@@QAEX_NPAUIDirectDrawPalette@@@Z");
    if (HMODULE h = GetModuleHandleA(gui)) {
        g_guiBase = reinterpret_cast<const uint8_t*>(h);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_guiBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(h)->e_lfanew);
        g_guiSize = nt->OptionalHeader.SizeOfImage;
    }
    // Optional: without it the game's compass comes back only after a relog. In GUI.dll or Utils.dll
    // depending on the build.
    CriteriaEvaluate = nullptr;
    for (const char* m : {gui, "Utils.dll"}) {
        if (HMODULE h = GetModuleHandleA(m))
            CriteriaEvaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(h, "?Evaluate@CriteriaMonitor_c@@QAE_NXZ"));
        if (CriteriaEvaluate)
            break;
    }
    if (!CriteriaEvaluate)
        Log("[minimap] CriteriaMonitor_c::Evaluate not found - the game's compass comes back only after a relog");
    return ok;
}

struct TempString {
    alignas(8) uint8_t buf[0x40] = {};
    explicit TempString(const char* s) { StringCtor(buf, s); }
    ~TempString() { StringDtor(buf); }
    TempString(const TempString&) = delete;
    TempString& operator=(const TempString&) = delete;
};

struct ListNode {
    ListNode* next;
    ListNode* prev;
    void* value;
};

bool InWindowList(void* wc, void* window)
{
    auto* head = *reinterpret_cast<ListNode**>(static_cast<uint8_t*>(wc) + kWcWindowList);
    int guard = 0;
    for (ListNode* n = head ? head->next : nullptr; n && n != head && guard < 4096; n = n->next, ++guard)
        if (n->value == window)
            return true;
    return false;
}

bool InGame()
{
    void* cc = ControlCenter();
    return cc && *(static_cast<const uint8_t*>(cc) + kCcAlive) && game::ClientInstance() != 0;
}

bool InGui(const void* p, size_t len = 4)
{
    const auto* b = static_cast<const uint8_t*>(p);
    return g_guiBase && b >= g_guiBase && b + len <= g_guiBase + g_guiSize;
}

// The object's class name from MSVC run-time type info (as the bars): vtable[-1] is the complete
// object locator, whose type descriptor (+0x0C) has the decorated name at +8; every pointer in GUI.dll.
bool IsClass(void* obj, const char* name)
{
    if (!obj)
        return false;
    void** vtable = *static_cast<void***>(obj);
    if (!InGui(vtable - 1))
        return false;
    const auto* col = static_cast<const uint8_t*>(vtable[-1]);
    if (!InGui(col, 0x14))
        return false;
    const auto* td = *reinterpret_cast<const uint8_t* const*>(col + 0x0C);
    const size_t len = strlen(name) + 1;
    return InGui(td, 8 + len) && memcmp(td + 8, name, len) == 0;
}

// The game's compass (CompassWindow_c "compass_window", GUI 0x1006D433; shown while
// "dvalue:cc_section1 && dvalue:cc_compass") is hidden while the minimap is on, which has its own
// compass; when it's off, the one Clarity hid comes back if its own condition says so.
void* g_hiddenCompass = nullptr;

void SyncGameCompass(void* wc, bool hide)
{
    void* compass = nullptr;
    auto* head = *reinterpret_cast<ListNode**>(static_cast<uint8_t*>(wc) + kWcWindowList);
    int guard = 0;
    for (ListNode* n = head ? head->next : nullptr; n && n != head && guard < 4096 && !compass; n = n->next, ++guard)
        if (IsClass(n->value, kCompassClass))
            compass = n->value;
    if (!compass) {
        g_hiddenCompass = nullptr;
        return;
    }
    if (hide) {
        if (WindowIsVisible(compass))
            WindowShow(compass, false);
        g_hiddenCompass = compass;
    } else if (g_hiddenCompass == compass) {
        g_hiddenCompass = nullptr;
        if (CriteriaEvaluate && CriteriaEvaluate(static_cast<uint8_t*>(compass) + kCompassCriteria))
            WindowShow(compass, true);
    }
}

// --- the window -----------------------------------------------------------------------------

// Outside the map's circle: the frame's ring (to 10 px out), with the badges (radius 11) and the rim
// markers centred on it (kRingMid).
constexpr float kFramePx = 15.0f;
constexpr float kRingMid = 5.5f; // the gunmetal ring's middle, from the map's edge
constexpr float kMarginPx = 2.0f;

struct Minimap {
    void* window = nullptr;
    void* windowVtable = nullptr;
    void* view = nullptr;
    float gw = 0.0f;  // window size, GUI units
    float x = -1.0f, y = -1.0f;
    float sizeSeen = -1.0f, xSeen = -2.0f, ySeen = -2.0f;
};
Minimap g_map;
void* g_wc = nullptr;
bool g_disabled = false;

void* g_vtable[kVtableSlots]; // View's vtable with our Render
void* g_flat = nullptr;     // untextured, alpha-blended
void* g_textured = nullptr; // texture x diffuse (colour and alpha)
void* g_rooms = nullptr;    // texture x diffuse x 2, as the game's map; alpha from the diffuse
void* g_multiply = nullptr; // texture x what's there (the ground's textures over its lightmap)
void* g_compass = nullptr;  // N, E, S, W: four 32x32 cells, white with a dark outline
void** g_mapData = nullptr; // GUI's MapData_c* (SIG_MAPDATA_GET)
bool g_loggedRender = false;
float g_opacity = 1.0f; // [Minimap] Opacity, for this frame's drawing

void Forget()
{
    g_map = Minimap{};
}

void CommonStates(void* blob)
{
    BlobSetRs(blob, RS_LIGHTING, 0);
    BlobSetRs(blob, RS_FOGENABLE, 0);
    BlobSetRs(blob, RS_ZENABLE, 0);
    BlobSetRs(blob, RS_ZWRITEENABLE, 0);
    BlobSetRs(blob, RS_CULLMODE, CULL_NONE);
    BlobSetRs(blob, RS_ALPHABLENDENABLE, 1);
    BlobSetRs(blob, RS_SRCBLEND, BLEND_SRCALPHA);
    BlobSetRs(blob, RS_DESTBLEND, BLEND_INVSRCALPHA);
}

enum class Blob { Flat, Textured, Rooms, Multiply };

void* MakeBlob(Blob kind)
{
    void* blob = CrtNew(kBlobAlloc);
    if (!blob)
        return nullptr;
    memset(blob, 0, kBlobAlloc);
    BlobCtor(blob);
    CommonStates(blob);
    if (kind == Blob::Flat) {
        BlobSetTss(blob, 0, TSS_COLOROP, TOP_SELECTARG2);
        BlobSetTss(blob, 0, TSS_COLORARG2, TA_DIFFUSE);
        BlobSetTss(blob, 0, TSS_ALPHAOP, TOP_SELECTARG2);
        BlobSetTss(blob, 0, TSS_ALPHAARG2, TA_DIFFUSE);
        return blob;
    }
    BlobSetTss(blob, 0, TSS_COLOROP, kind == Blob::Rooms ? TOP_MODULATE2X : TOP_MODULATE);
    BlobSetTss(blob, 0, TSS_COLORARG1, TA_TEXTURE);
    BlobSetTss(blob, 0, TSS_COLORARG2, TA_DIFFUSE);
    BlobSetTss(blob, 0, TSS_ALPHAOP, kind == Blob::Rooms ? TOP_SELECTARG2 : TOP_MODULATE);
    BlobSetTss(blob, 0, TSS_ALPHAARG1, TA_TEXTURE);
    BlobSetTss(blob, 0, TSS_ALPHAARG2, TA_DIFFUSE);
    BlobSetTss(blob, 0, TSS_ADDRESS, TADDRESS_CLAMP);
    if (kind == Blob::Multiply) {
        BlobSetRs(blob, RS_SRCBLEND, BLEND_DESTCOLOR);
        BlobSetRs(blob, RS_DESTBLEND, BLEND_ZERO);
    }
    return blob;
}

// The compass letters N, E, S, W, the zoom buttons' + and -, and the layer button's symbol, in 32x32
// cells of a 256x32 texture,
// white over a dark outline.
constexpr int kCell = 32;
constexpr wchar_t kCompassLetters[] = L"NESW"; // cells 4-6: +, - and the layers symbol (drawn)
constexpr int kCompassCells = 8; // the texture has room for 8 (256x32)
constexpr int kGlyphPlus = 4, kGlyphMinus = 5, kGlyphLayers = 6;

// The layers symbol: three stacked flat diamonds, the top one whole, the two below showing as chevrons
// under it with a gap - 4x4 supersampled coverage in a 32x32 cell.
raster::Mask LayersSymbol()
{
    raster::Mask m(kCell, kCell);
    constexpr float cx = 16.0f, hw = 6.5f, hh = 3.25f, step = 3.0f, gap = 1.2f;
    constexpr float top = 13.0f; // the top diamond's centre (the three span 9.75..22.25: centred on 16)
    auto in = [&](float x, float y, float cy, float grow) {
        return std::fabs(x - cx) / (hw + grow) + std::fabs(y - cy) / (hh + grow * 0.5f) <= 1.0f;
    };
    for (int py = 0; py < kCell; ++py)
        for (int px = 0; px < kCell; ++px) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float x = px + (sx + 0.5f) / 4.0f, y = py + (sy + 0.5f) / 4.0f;
                    bool on = in(x, y, top, 0.0f);
                    for (int k = 1; k < 3 && !on; ++k) // a chevron: its diamond, minus the one above and a gap
                        on = in(x, y, top + step * k, 0.0f) && !in(x, y, top + step * (k - 1), gap) && y > top + step * k - 1.0f;
                    hits += on ? 1 : 0;
                }
            m.a[static_cast<size_t>(py) * kCell + px] = hits / 16.0f;
        }
    return m;
}

// The zoom buttons' + (both bars) and - (the level one): 12 px bars 2.6 px thick, centred in the cell.
raster::Mask Bars(bool plus)
{
    raster::Mask m(kCell, kCell);
    constexpr float c = 16.0f, half = 6.0f, thick = 1.3f;
    for (int py = 0; py < kCell; ++py)
        for (int px = 0; px < kCell; ++px) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = std::fabs(px + (sx + 0.5f) / 4.0f - c), dy = std::fabs(py + (sy + 0.5f) / 4.0f - c);
                    hits += ((dx <= half && dy <= thick) || (plus && dy <= half && dx <= thick)) ? 1 : 0;
                }
            m.a[static_cast<size_t>(py) * kCell + px] = hits / 16.0f;
        }
    return m;
}

// The centre of a mask's ink (its box of pixels with coverage over 0.1); false if it has none.
bool InkCentre(const raster::Mask& m, float* x, float* y)
{
    int x0 = m.w, y0 = m.h, x1 = -1, y1 = -1;
    for (int py = 0; py < m.h; ++py)
        for (int px = 0; px < m.w; ++px)
            if (m.At(px, py) > 0.1f) {
                x0 = std::min(x0, px);
                y0 = std::min(y0, py);
                x1 = std::max(x1, px);
                y1 = std::max(y1, py);
            }
    if (x1 < 0)
        return false;
    *x = (x0 + x1 + 1) * 0.5f;
    *y = (y0 + y1 + 1) * 0.5f;
    return true;
}

void* MakeCompass()
{
    raster::Font font;
    if (!font.Create(L"Bahnschrift", 14, FW_BOLD) && !font.Create(L"Segoe UI", 14, FW_BOLD))
        return nullptr;
    static uint32_t pixels[kCompassCells * kCell * kCell];
    memset(pixels, 0, sizeof(pixels));
    for (int k = 0; k <= kGlyphLayers; ++k) {
        raster::Mask glyph;
        if (k == kGlyphLayers) {
            glyph = LayersSymbol();
        } else if (k == kGlyphPlus || k == kGlyphMinus) {
            glyph = Bars(k == kGlyphPlus);
        } else { // a letter, moved so its ink is in the middle of the cell (the badge's centre)
            const float tw = font.TextWidth(&kCompassLetters[k], 1);
            int x = static_cast<int>(std::lround((kCell - tw) * 0.5f));
            int y = static_cast<int>(std::lround((kCell - font.Height()) * 0.5f));
            glyph = raster::Rasterize(font, &kCompassLetters[k], 1, x, y, kCell, kCell);
            float ix, iy;
            if (InkCentre(glyph, &ix, &iy)) {
                x += static_cast<int>(std::lround(kCell * 0.5f - ix));
                y += static_cast<int>(std::lround(kCell * 0.5f - iy));
                glyph = raster::Rasterize(font, &kCompassLetters[k], 1, x, y, kCell, kCell);
            }
        }
        const raster::Mask edge = raster::Dilate(glyph, 1);
        for (int py = 0; py < kCell; ++py)
            for (int px = 0; px < kCell; ++px) {
                const float g = glyph.At(px, py), o = edge.At(px, py);
                const float a = g + o * (1.0f - g); // the letter over its outline
                if (a <= 0.0f)
                    continue;
                const float c = g / a;              // white over near-black
                const uint32_t v = static_cast<uint32_t>(std::lround(255.0f * c + 12.0f * (1.0f - c)));
                pixels[py * kCompassCells * kCell + k * kCell + px] =
                    (static_cast<uint32_t>(std::lround(a * 255.0f)) << 24) | (v << 16) | (v << 8) | v;
            }
    }
    font.Destroy();
    void* tex = game::CreateTextureARGB("clarity_minimap_compass", kCompassCells * kCell, kCell);
    if (tex && !game::WriteTexture(tex, pixels, kCompassCells * kCell, kCell))
        tex = nullptr;
    return tex;
}

// --- drawing --------------------------------------------------------------------------------

constexpr int kSegments = 96;
constexpr float kPi = 3.14159265f;

void BindTexture(void* viewport, void* texture)
{
    void* deviceState = *reinterpret_cast<void**>(static_cast<uint8_t*>(viewport) + kViewportDeviceState);
    void* surface = texture ? *reinterpret_cast<void**>(static_cast<uint8_t*>(texture) + kTextureSurface) : nullptr;
    DeviceSetTexture(deviceState, surface, 0, 10);
    RealizeStates(viewport);
}

bool Begin(void* viewport, void* blob, void* texture)
{
    if (!blob || !BlobValidate(blob))
        return false;
    BlobSet(blob);
    BindTexture(viewport, texture);
    return true;
}

void End(void* viewport, void* blob)
{
    BindTexture(viewport, nullptr);
    BlobReset(blob);
}

// Every colour goes through here: its alpha times the opacity.
Vertex V(float x, float y, uint32_t argb, float u = 0.0f, float v = 0.0f)
{
    const uint32_t a = static_cast<uint32_t>((argb >> 24) * g_opacity + 0.5f);
    return Vertex{x, y, kZ, kRhw, (a << 24) | (argb & 0xFFFFFF), u, v};
}

// A filled circle (the same kSegments-gon the map is clipped to).
void Disc(void* viewport, float cx, float cy, float r, uint32_t argb)
{
    if (!Begin(viewport, g_flat, nullptr))
        return;
    Vertex v[kSegments + 2];
    v[0] = V(cx, cy, argb);
    for (int i = 0; i <= kSegments; ++i) {
        const float a = 2.0f * kPi * i / kSegments;
        v[i + 1] = V(cx + std::cos(a) * r, cy + std::sin(a) * r, argb);
    }
    RenderFan(*RenderInstance, kFvf, v, kSegments + 2, 0);
    End(viewport, g_flat);
}

// --- the map --------------------------------------------------------------------------------

struct Pt {
    float x, y, u, v;
};

constexpr int kClipCap = kSegments + 8;

// Sutherland-Hodgman: the convex polygon `in` cut to the kSegments-gon of radius r around (cx, cy).
int ClipToCircle(const Pt* in, int n, float cx, float cy, float r, Pt* out)
{
    Pt a[kClipCap], b[kClipCap];
    int count = std::min(n, kClipCap);
    std::copy(in, in + count, a);
    Pt* src = a;
    Pt* dst = b;
    for (int e = 0; e < kSegments && count > 0; ++e) {
        const float a0 = 2.0f * kPi * e / kSegments, a1 = 2.0f * kPi * (e + 1) / kSegments;
        const float ex0 = cx + std::cos(a0) * r, ey0 = cy + std::sin(a0) * r;
        const float ex1 = cx + std::cos(a1) * r, ey1 = cy + std::sin(a1) * r;
        auto side = [&](const Pt& p) { return (ex1 - ex0) * (p.y - ey0) - (ey1 - ey0) * (p.x - ex0); }; // >= 0: inside
        int m = 0;
        for (int i = 0; i < count && m < kClipCap - 1; ++i) {
            const Pt& p = src[i];
            const Pt& q = src[(i + 1) % count];
            const float sp = side(p), sq = side(q);
            if (sp >= 0.0f)
                dst[m++] = p;
            if ((sp >= 0.0f) != (sq >= 0.0f) && m < kClipCap) {
                const float t = sp / (sp - sq);
                dst[m++] = Pt{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.u + (q.u - p.u) * t, p.v + (q.v - p.v) * t};
            }
        }
        std::swap(src, dst);
        count = m;
    }
    std::copy(src, src + count, out);
    return count;
}

struct Vec3f {
    float x, y, z;
};

// v rotated by the unit quaternion q (x, y, z, w), as GUI 0x10043401: v + 2w(q x v) + 2 q x (q x v).
Vec3f Rotate(const float* q, Vec3f v)
{
    const Vec3f t{q[1] * v.z - q[2] * v.y, q[2] * v.x - q[0] * v.z, q[0] * v.y - q[1] * v.x};
    const Vec3f u{q[1] * t.z - q[2] * t.y, q[2] * t.x - q[0] * t.z, q[0] * t.y - q[1] * t.x};
    return Vec3f{v.x + 2.0f * (q[3] * t.x + u.x), v.y + 2.0f * (q[3] * t.y + u.y), v.z + 2.0f * (q[3] * t.z + u.z)};
}

// MapData_c and its room entries (see the top of the file).
constexpr int kMdGround = 0x08, kMdRoomsBegin = 0x1C, kMdRoomsEnd = 0x20, kMdFloor = 0x2C, kMdShowSelected = 0x81,
              kMdKnowAll = 0x82;
constexpr int kRoomSize = 0x44;
constexpr int kRoomArgb = 0x04, kRoomPos = 0x08, kRoomOffset = 0x14, kRoomRotation = 0x20, kRoomW = 0x30,
              kRoomH = 0x34, kRoomFloor = 0x38, kRoomTileSize = 0x3C, kRoomTiles = 0x40;
constexpr int kDbObjectTexture = 0x38; // DbObject_t (RDBTexture_t) vtable byte offset: an AddRef'd RTexture_t*
// The local map's scale (PFMapRenderer_c, GUI 0x100EAEBE -> 0x100E9729): 4 / MapData_c +0xC4 GUI units
// per metre - +0xC4 is 2 indoors (GUI 0x10042996), so 2 per metre, and 4 outdoors (1 per metre). The
// minimap uses the same, times its zoom (1 = the game's).
constexpr int kMdMapScale = 0xC4;
constexpr float kZoomMin = 0.75f, kZoomMax = 1.5f, kZoomStep = 0.25f; // 75%, 100%, 125%, 150%
float g_zoom = 1.0f;          // the minimap's zoom (game thread; the + / - buttons change it)
float g_zoomSeen = -1.0f;     // [Minimap] Zoom as last taken

template <typename T>
T At(const uint8_t* p, int offset)
{
    T v;
    memcpy(&v, p + offset, sizeof(T));
    return v;
}

// The screen rectangle l..rt, t..b with the whole texture on it (u left to right; v vTop at the top,
// vBottom at the bottom), clipped to the circle; with `blob`'s states.
void ScreenQuad(void* viewport, void* blob, void* texture, uint32_t argb, float l, float t, float rt, float b,
                float vTop, float vBottom, float cx, float cy, float r)
{
    // Off the map: the circle's centre is further from the rectangle than r.
    const float nx = std::clamp(cx, l, rt), ny = std::clamp(cy, t, b);
    if ((nx - cx) * (nx - cx) + (ny - cy) * (ny - cy) > r * r)
        return;
    const Pt quad[4] = {{l, t, 0.0f, vTop}, {rt, t, 1.0f, vTop}, {rt, b, 1.0f, vBottom}, {l, b, 0.0f, vBottom}};
    // Inside the circle: every corner is.
    bool inside = true;
    for (const Pt& q : quad)
        inside = inside && (q.x - cx) * (q.x - cx) + (q.y - cy) * (q.y - cy) <= r * r * 0.99f;
    Pt poly[kClipCap];
    int n = 4;
    if (inside)
        std::copy(quad, quad + 4, poly);
    else
        n = ClipToCircle(quad, 4, cx, cy, r, poly);
    if (n < 3 || !Begin(viewport, blob, texture))
        return;
    Vertex v[kClipCap];
    for (int i = 0; i < n; ++i)
        v[i] = V(poly[i].x, poly[i].y, argb, poly[i].u, poly[i].v);
    RenderFan(*RenderInstance, kFvf, v, static_cast<unsigned long>(n), 0);
    End(viewport, blob);
}

// The world rectangle x0..x1 (west to east), z0..z1 (south to north) with the whole texture on it (u
// west to east, v south to north, as the game's map).
void WorldQuad(void* viewport, void* blob, void* texture, uint32_t argb, float x0, float z0, float x1, float z1,
               const game::Vec3& me, float cx, float cy, float r, float ppm)
{
    ScreenQuad(viewport, blob, texture, argb, cx + (x0 - me.x) * ppm, cy - (z1 - me.z) * ppm, cx + (x1 - me.x) * ppm,
               cy - (z0 - me.z) * ppm, 1.0f, 0.0f, cx, cy, r);
}

// The dungeon's rooms on the current floor, as the game's local map draws them.
void Rooms(void* viewport, const uint8_t* md, const game::Vec3& me, float cx, float cy, float r, float ppm)
{
    const uint8_t* begin = At<const uint8_t*>(md, kMdRoomsBegin);
    const uint8_t* end = At<const uint8_t*>(md, kMdRoomsEnd);
    if (!begin || end <= begin || (end - begin) % kRoomSize != 0 || end - begin > kRoomSize * 4096)
        return;
    const int floor = At<int>(md, kMdFloor);
    const bool knowAll = md[kMdKnowAll] != 0;
    for (const uint8_t* room = begin; room < end; room += kRoomSize) {
        if (At<int>(room, kRoomFloor) != floor)
            continue;
        uint32_t argb = At<uint32_t>(room, kRoomArgb);
        if ((argb & 0xFFFFFF) == 0) {
            if (!knowAll)
                continue;
            argb = 0xFF808080;
        }
        const int w = At<int>(room, kRoomW), h = At<int>(room, kRoomH);
        void* const* tiles = At<void* const*>(room, kRoomTiles);
        const float size = At<float>(room, kRoomTileSize);
        if (!tiles || w <= 0 || h <= 0 || w > 512 || h > 512 || !(size > 0.0f))
            continue;
        float q[4];
        memcpy(q, room + kRoomRotation, sizeof(q));
        const Vec3f pos = At<Vec3f>(room, kRoomPos);
        const Vec3f off = Rotate(q, At<Vec3f>(room, kRoomOffset));
        const Vec3f stepX = Rotate(q, Vec3f{size, 0.0f, 0.0f});
        const Vec3f stepZ = Rotate(q, Vec3f{0.0f, 0.0f, size});
        // A room far off the map: skipped without looking at its tiles.
        const float extent = (static_cast<float>(std::max(w, h)) + 1.0f) * size * 1.5f;
        const float reach = r / ppm * 1.5f + extent;
        if (std::fabs(pos.x + off.x - me.x) > reach || std::fabs(pos.z + off.z - me.z) > reach)
            continue;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                void* obj = tiles[j * w + i];
                if (!obj)
                    continue;
                using TextureFn = void*(__thiscall*)(void* self);
                void* tex = reinterpret_cast<TextureFn>((*static_cast<void***>(obj))[kDbObjectTexture / 4])(obj);
                if (!tex)
                    continue;
                const float wx = pos.x + off.x + stepX.x * i + stepZ.x * j;
                const float wz = pos.z + off.z + stepX.z * i + stepZ.z * j;
                const float hs = size * 0.5f;
                WorldQuad(viewport, g_rooms, pfmap::RoomTileTexture(tex), argb, wx - hs, wz - hs, wx + hs, wz + hs, me,
                          cx, cy, r, ppm);
                ReleaseTexture(tex);
            }
    }
}

// --- outdoors: the ground -------------------------------------------------------------------
//
// As the local map (GUI 0x100472D5): MapData_c +0x08 is the playfield's AnarchyGround_t, its
// AnarchyGroundData_t at +0x17C: +0x2C / +0x30 / +0x94 what CreateBuildingMap is given (GUI 0x10047AAC),
// +0x40 tiles a patch is across, +0x50 / +0x54 patches across / down, +0x58 the patches (0x54 bytes:
// +0x40 set from data +0x2C, +0x44 its building map, +0x48 / +0x50 its ground texture, low / high). Patch
// (i, j) covers world x i*S..(i+1)*S and z j*S..(j+1)*S, S = tiles x MapData_c +0xC4 (4 m a tile).
// Three layers: the patches' lightmaps (GetShadowTexture), a white wash (0xBBFFFFFF), then each patch's
// building map and ground texture multiplied over that.
constexpr int kGroundData = 0x17C;
constexpr int kGdBuildingArg = 0x2C, kGdBuildingBool = 0x30, kGdPalette = 0x94, kGdPatchTiles = 0x40,
              kGdPatchesX = 0x50, kGdPatchesZ = 0x54, kGdPatches = 0x58;
constexpr int kPatchSize = 0x54, kPatchBuildingArg = 0x40, kPatchBuildings = 0x44, kPatchLow = 0x48, kPatchHigh = 0x50;

void Ground(void* viewport, const uint8_t* md, const game::Vec3& me, float cx, float cy, float r, float ppm)
{
    const void* ground = At<void*>(md, kMdGround);
    const uint8_t* data = At<const uint8_t*>(static_cast<const uint8_t*>(ground), kGroundData);
    if (!data)
        return;
    const int tiles = At<int>(data, kGdPatchTiles), nx = At<int>(data, kGdPatchesX), nz = At<int>(data, kGdPatchesZ);
    uint8_t* patches = At<uint8_t*>(data, kGdPatches);
    const float tile = At<float>(md, kMdMapScale);
    if (!patches || tiles <= 0 || nx <= 0 || nz <= 0 || nx > 4096 || nz > 4096 || !(tile > 0.0f))
        return;
    const float S = tiles * tile;
    const float reach = r / ppm;
    const int i0 = std::max(0, static_cast<int>(std::floor((me.x - reach) / S)));
    const int i1 = std::min(nx - 1, static_cast<int>(std::floor((me.x + reach) / S)));
    const int j0 = std::max(0, static_cast<int>(std::floor((me.z - reach) / S)));
    const int j1 = std::min(nz - 1, static_cast<int>(std::floor((me.z + reach) / S)));
    if (i0 > i1 || j0 > j1)
        return;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (void* light = GroundShadow(ground, i, j))
                WorldQuad(viewport, g_textured, light, 0xFFFFFFFF, i * S, j * S, (i + 1) * S, (j + 1) * S, me, cx, cy, r, ppm);
    Disc(viewport, cx, cy, r, 0xBBFFFFFF);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            uint8_t* patch = patches + (static_cast<size_t>(j) * nx + i) * kPatchSize;
            memcpy(patch + kPatchBuildingArg, data + kGdBuildingArg, 4);
            if (!At<void*>(patch, kPatchBuildings))
                BuildingMap(patch, data[kGdBuildingBool] != 0, At<void*>(data, kGdPalette));
            if (void* buildings = At<void*>(patch, kPatchBuildings))
                WorldQuad(viewport, g_multiply, buildings, 0xFFFFFFFF, i * S, j * S, (i + 1) * S, (j + 1) * S, me, cx, cy, r, ppm);
            GroundHiTexture(data, patch);
            void* tex = At<void*>(patch, kPatchHigh);
            if (!tex)
                tex = At<void*>(patch, kPatchLow);
            if (tex)
                WorldQuad(viewport, g_multiply, tex, 0xFFFFFFFF, i * S, j * S, (i + 1) * S, (j + 1) * S, me, cx, cy, r, ppm);
        }
}

// --- the planet layer -------------------------------------------------------------------------
//
// The planet map's own files (cd_image/textures/PlanetMap): the map the player picked in the planet map
// window - DValue PlanetMapIndexFile (default "Normal/PlanetMapIndexNormal.txt") for Rubi-Ka,
// ShadowlandMapIndexFile ("Shadowlands/ShadowlandsMap.txt") for the Shadowlands; a zone not on the first
// is looked for on the second. The index (read as GUI 0x1004CB0C does) names the coordinates and tile files and
// lists the zoom levels: TextureSize, Size, Tiles (columns, rows), MapRect, then FilePos for each tile,
// column by column (PNG images in the tile file). Coordinates: <Playfield id x xscale z zscale> per zone.
// Where you are on it, as the planet map window (GUI 0x1004B81A): planet = the zone's (x, z) + your
// position; pixel x = (planet x - 31022) * (MapRect width / 18751) * xscale + MapRect left, pixel y =
// MapRect height - ((planet z - 24880) * (MapRect height / 24115) * zscale + MapRect top). Its zoom
// switches levels, as the planet map window's (GUI 0x1004B447 / 0x1004B468: ZoomLev +1 / -1 within the
// levels, each shown as it is): here too, + and - go up and down the levels, a texel a GUI unit.
// [Minimap] PlanetLevel (-1 = the most detailed). Inside (dungeons, buildings, missions: playfields not
// on the maps) it shows the last place you were outdoors.
constexpr double kPlanetOriginX = 31022.0, kPlanetOriginZ = 24880.0, kPlanetSpanX = 18751.0, kPlanetSpanZ = 24115.0;
constexpr size_t kTileCacheMax = 48;

struct PlanetLevel {
    int texture = 128, cols = 0, rows = 0;
    int left = 0, top = 0, right = 0, bottom = 0; // MapRect
    std::vector<uint32_t> pos;                    // FilePos, column by column
    int file = 0;                                 // its File (each level names one; maps differ in sharing)
};
struct TileFile {
    std::string name;                // as in the index
    std::wstring path;
    std::vector<uint32_t> sortedPos; // every FilePos into it, for the tiles' lengths
    uint64_t size = 0;
};
struct ZoneCoords {
    float x = 0, z = 0, xscale = 1, zscale = 1;
};
struct PlanetMap {
    bool loaded = false, ok = false;
    std::vector<TileFile> files;
    std::vector<PlanetLevel> levels; // as in the index, the overview first
    std::map<uint32_t, ZoneCoords> zones;
};
constexpr int kMaps = 2; // the Rubi-Ka map, the Shadowlands map
const char* const kMapDValue[kMaps] = {"PlanetMapIndexFile", "ShadowlandMapIndexFile"};
const char* const kMapDefault[kMaps] = {"Normal/PlanetMapIndexNormal.txt", "Shadowlands/ShadowlandsMap.txt"};
PlanetMap g_maps[kMaps];
std::string g_mapFile[kMaps];      // the index file each was loaded from
DWORD g_mapChoiceAt = 0;           // when the DValues were last read
int g_mapShown = 0;                // which map the planet layer is on (this frame)
// Locked, dragging the map moves what it shows: the view's centre is you plus g_pan (metres east, north);
// a click without a drag, or a new playfield, centres it on you again; so does moving, on the local map
// (the planet layer stays where it was dragged). g_ppmX / g_ppmZ: GUI units a metre on the layer drawn
// last (for the drag).
struct Pan {
    float x = 0.0f, z = 0.0f;
};
Pan g_pan;
float g_ppmX = 0.0f, g_ppmZ = 0.0f;
struct PlanetView {
    int map = -1, level = 0;
    float px = 0.0f, py = 0.0f; // the pixel at the map's centre
};
PlanetView g_view; // the planet layer as drawn last
const void* g_panPlayfield = nullptr; // the playfield the pan is for
bool g_panning = false;               // locked: the map is being dragged
game::Vec3 g_panMe{};                 // where you were when the map was dragged (local map)
constexpr float kPanMoveM = 0.5f;     // moving this far centres the local map
std::map<uint64_t, void*> g_planetTiles; // map, level, column, row -> RTexture_t (game thread)
int g_planetLevel = -1;                  // the level shown (game thread)
int g_planetLevelSeen = -2;              // [Minimap] PlanetLevel as last taken
IWICImagingFactory* g_wic = nullptr;

// A DValue as text ("" if it isn't set). VS2010 std::string: 16-byte buffer or pointer, size, capacity.
std::string DValueText(const char* name)
{
    alignas(8) uint8_t str[0x40] = {}, variant[0x40] = {}, text[0x20] = {};
    StringCtor(str, name);
    GetDValue(variant, str, false);
    StringDtor(str);
    VariantString(variant, text);
    VariantDtor(variant);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(text + 0x10);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(text + 0x14);
    std::string out;
    const char* chars = cap >= 16 ? *reinterpret_cast<const char* const*>(text) : reinterpret_cast<const char*>(text);
    if (chars && size < 1024 && cap >= size)
        out.assign(chars, size);
    if (cap >= 16 && chars)
        CrtDelete(const_cast<char*>(chars)); // the game's CRT made it
    return out;
}

std::wstring ClientPath(const wchar_t* rel)
{
    wchar_t dir[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, dir);
    return std::wstring(dir) + L"\\cd_image\\textures\\PlanetMap\\" + rel;
}

std::string Attr(const std::string& line, const char* name)
{
    const std::string key = std::string(" ") + name + "=\"";
    const size_t a = line.find(key);
    if (a == std::string::npos)
        return {};
    const size_t b = line.find('"', a + key.size());
    return b == std::string::npos ? std::string() : line.substr(a + key.size(), b - a - key.size());
}

std::wstring Widen(std::string s)
{
    for (char& c : s)
        if (c == '/')
            c = '\\';
    return std::wstring(s.begin(), s.end());
}

void LoadPlanet(PlanetMap& pm, const std::string& indexFile)
{
    pm = PlanetMap{};
    pm.loaded = true;
    std::ifstream index(ClientPath(Widen(indexFile).c_str()));
    if (!index) {
        Log("[minimap] planet map index %s not found", indexFile.c_str());
        return;
    }
    std::string line, coordsFile, tileFile;
    std::vector<PlanetLevel> levels;
    auto fileIndex = [&](const std::string& name) {
        for (size_t i = 0; i < pm.files.size(); ++i)
            if (pm.files[i].name == name)
                return static_cast<int>(i);
        pm.files.push_back(TileFile{name, ClientPath(Widen(name).c_str()), {}, 0});
        return static_cast<int>(pm.files.size() - 1);
    };
    while (std::getline(index, line)) {
        std::istringstream in(line);
        std::string key;
        in >> key;
        if (key == "CoordsFile")
            in >> coordsFile;
        else if (key == "File")
            in >> tileFile;
        else if (key == "TextureSize") {
            levels.emplace_back();
            in >> levels.back().texture;
            levels.back().file = tileFile.empty() ? -1 : fileIndex(tileFile);
        } else if (levels.empty())
            continue;
        else if (key == "Tiles")
            in >> levels.back().cols >> levels.back().rows;
        else if (key == "MapRect")
            in >> levels.back().left >> levels.back().top >> levels.back().right >> levels.back().bottom;
        else if (key == "FilePos") {
            uint32_t pos = 0;
            in >> pos;
            levels.back().pos.push_back(pos);
        }
    }
    for (TileFile& f : pm.files) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(f.path.c_str(), GetFileExInfoStandard, &fa))
            f.size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }
    for (const PlanetLevel& l : levels)
        if (l.file >= 0 && pm.files[static_cast<size_t>(l.file)].size > 0 && l.cols > 0 && l.rows > 0 && l.cols < 16384 &&
            l.rows < 16384 && l.pos.size() == static_cast<size_t>(l.cols) * l.rows && l.right > l.left && l.bottom > l.top)
            pm.levels.push_back(l);
    if (pm.levels.empty() || coordsFile.empty()) {
        Log("[minimap] planet map index %s not understood (or its tile files missing)", indexFile.c_str());
        return;
    }
    for (const PlanetLevel& l : pm.levels) {
        std::vector<uint32_t>& sorted = pm.files[static_cast<size_t>(l.file)].sortedPos;
        sorted.insert(sorted.end(), l.pos.begin(), l.pos.end());
    }
    for (TileFile& f : pm.files)
        std::sort(f.sortedPos.begin(), f.sortedPos.end());
    std::ifstream coords(ClientPath(Widen(coordsFile).c_str()));
    while (std::getline(coords, line)) {
        if (line.find("<Playfield") == std::string::npos)
            continue;
        ZoneCoords zc;
        const std::string id = Attr(line, "id");
        if (id.empty())
            continue;
        zc.x = static_cast<float>(atof(Attr(line, "x").c_str()));
        zc.z = static_cast<float>(atof(Attr(line, "z").c_str()));
        const std::string xs = Attr(line, "xscale"), zs = Attr(line, "zscale");
        zc.xscale = xs.empty() ? 1.0f : static_cast<float>(atof(xs.c_str()));
        zc.zscale = zs.empty() ? 1.0f : static_cast<float>(atof(zs.c_str()));
        pm.zones[static_cast<uint32_t>(atoi(id.c_str()))] = zc;
    }
    pm.ok = !pm.zones.empty();
    for (const PlanetLevel& l : pm.levels)
        Log("[minimap] planet map level: %dx%d tiles of %d px from %s", l.cols, l.rows, l.texture,
            pm.files[static_cast<size_t>(l.file)].name.c_str());
    Log("[minimap] planet map %s: %zu levels, %zu zones%s", indexFile.c_str(), pm.levels.size(),
        pm.zones.size(), pm.ok ? "" : " - no zones in its coordinates file");
}

// A PNG to ARGB with Windows' own decoder (WIC).
bool DecodePng(const uint8_t* data, size_t n, std::vector<uint32_t>* out, int* w, int* h)
{
    if (!g_wic) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // the game thread's COM, if it has none yet
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) {
            g_wic = nullptr;
            return false;
        }
    }
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    UINT uw = 0, uh = 0;
    if (SUCCEEDED(g_wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(n))) &&
        SUCCEEDED(g_wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&uw, &uh)) && uw > 0 && uh > 0 && uw <= 1024 && uh <= 1024) {
        out->assign(static_cast<size_t>(uw) * uh, 0);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, uw * uh * 4, reinterpret_cast<BYTE*>(out->data())));
        *w = static_cast<int>(uw);
        *h = static_cast<int>(uh);
    }
    if (conv)
        conv->Release();
    if (frame)
        frame->Release();
    if (decoder)
        decoder->Release();
    if (stream)
        stream->Release();
    return ok;
}

uint64_t TileKey(int map, int level, int col, int row)
{
    return (static_cast<uint64_t>(map) << 48) | (static_cast<uint64_t>(level) << 32) | (static_cast<uint64_t>(col) << 16) |
           static_cast<uint64_t>(row);
}

void DropTiles(int map)
{
    for (auto it = g_planetTiles.begin(); it != g_planetTiles.end();) {
        if (map < 0 || static_cast<int>(it->first >> 48) == map) {
            if (it->second)
                ReleaseTexture(it->second);
            it = g_planetTiles.erase(it);
        } else {
            ++it;
        }
    }
}

// Tile (col, row) of a level of a map as a texture; made the first time, kept while near you.
void* PlanetTile(const PlanetMap& pm, int map, int level, int col, int row)
{
    const uint64_t key = TileKey(map, level, col, row);
    auto it = g_planetTiles.find(key);
    if (it != g_planetTiles.end())
        return it->second;
    void*& slot = g_planetTiles[key];
    const PlanetLevel& l = pm.levels[static_cast<size_t>(level)];
    const uint32_t pos = l.pos[static_cast<size_t>(col) * l.rows + row];
    const TileFile& file = pm.files[static_cast<size_t>(l.file)];
    auto next = std::upper_bound(file.sortedPos.begin(), file.sortedPos.end(), pos);
    const uint64_t end = next != file.sortedPos.end() ? *next : file.size;
    if (end <= pos || end - pos > (4u << 20))
        return slot;
    std::vector<uint8_t> png(static_cast<size_t>(end - pos));
    std::ifstream f(file.path, std::ios::binary);
    f.seekg(pos);
    if (!f.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size())))
        return slot;
    std::vector<uint32_t> argb;
    int w = 0, h = 0;
    if (!DecodePng(png.data(), png.size(), &argb, &w, &h))
        return slot;
    void* tex = game::CreateTextureARGB("clarity_minimap_planet", w, h);
    if (tex && game::WriteTexture(tex, argb.data(), w, h))
        slot = tex;
    return slot;
}

// Tiles of other maps or levels, or not near (col, row), let go when there are too many.
void TrimPlanetTiles(int map, int level, int col, int row)
{
    if (g_planetTiles.size() <= kTileCacheMax)
        return;
    for (auto it = g_planetTiles.begin(); it != g_planetTiles.end();) {
        const int m = static_cast<int>(it->first >> 48), lv = static_cast<int>((it->first >> 32) & 0xFFFF);
        const int c = static_cast<int>((it->first >> 16) & 0xFFFF), r = static_cast<int>(it->first & 0xFFFF);
        if (m != map || lv != level || std::abs(c - col) > 2 || std::abs(r - row) > 2) {
            if (it->second)
                ReleaseTexture(it->second);
            it = g_planetTiles.erase(it);
        } else {
            ++it;
        }
    }
}

// Where a place (x, z in a zone) is on a level of a planet map (pixels).
void PlanetPixel(const ZoneCoords& z, const PlanetLevel& l, float x, float zz, float* px, float* py)
{
    const double w = l.right - l.left, h = l.bottom - l.top;
    *px = static_cast<float>((z.x + x - kPlanetOriginX) * (w / kPlanetSpanX) * z.xscale + l.left);
    *py = static_cast<float>(h - ((z.z + zz - kPlanetOriginZ) * (h / kPlanetSpanZ) * z.zscale + l.top));
}

// The player's map choices, read again every second; a changed choice drops that map.
void ReadMapChoice()
{
    const DWORD now = GetTickCount();
    if (now - g_mapChoiceAt < 1000 && g_maps[0].loaded)
        return;
    g_mapChoiceAt = now;
    for (int m = 0; m < kMaps; ++m) {
        std::string file = DValueText(kMapDValue[m]);
        if (file.empty())
            file = kMapDefault[m];
        if (file != g_mapFile[m] || !g_maps[m].loaded) {
            g_mapFile[m] = file;
            DropTiles(m);
            LoadPlanet(g_maps[m], file);
        }
    }
}

// The map a zone is on: the Rubi-Ka one, else the Shadowlands one (-1: neither).
int MapOf(uint32_t zone)
{
    for (int m = 0; m < kMaps; ++m)
        if (g_maps[m].ok && g_maps[m].zones.count(zone))
            return m;
    return -1;
}

// Where you are on the planet: a zone on one of the maps and your place in it (N3Msg_GetPositionData, as
// the planet map window, GUI 0x1004B81A). Inside, where it gives a playfield on neither map (the window
// shows no marker then), it's the last place you were outdoors: where you went in. That place is kept in
// [Minimap] LastZone / LastX / LastZ when you go in, for a session that starts inside.
struct Spot {
    uint32_t zone = 0;
    float x = 0.0f, z = 0.0f;
};
Spot g_spot;                 // the last place outdoors
bool g_spotRead = false;     // [Minimap] Last* read
bool g_spotSaved = true;     // g_spot is in Clarity.ini
uint32_t g_insideLogged = 0; // the playfield last logged as inside
uint32_t g_zoneLogged = 0;   // the playfield last logged as on a map

void ReadSpot()
{
    g_spotRead = true;
    const std::wstring path = ConfigPath();
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"Minimap", L"LastZone", L"0", v, 32, path.c_str());
    g_spot.zone = static_cast<uint32_t>(wcstoul(v, nullptr, 10));
    GetPrivateProfileStringW(L"Minimap", L"LastX", L"0", v, 32, path.c_str());
    g_spot.x = wcstof(v, nullptr);
    GetPrivateProfileStringW(L"Minimap", L"LastZ", L"0", v, 32, path.c_str());
    g_spot.z = wcstof(v, nullptr);
}

void SaveSpot()
{
    g_spotSaved = true;
    const std::wstring path = ConfigPath();
    wchar_t v[32];
    swprintf_s(v, L"%u", g_spot.zone);
    WritePrivateProfileStringW(L"Minimap", L"LastZone", v, path.c_str());
    swprintf_s(v, L"%.1f", g_spot.x);
    WritePrivateProfileStringW(L"Minimap", L"LastX", v, path.c_str());
    swprintf_s(v, L"%.1f", g_spot.z);
    WritePrivateProfileStringW(L"Minimap", L"LastZ", v, path.c_str());
}

// False if you're nowhere on the maps (inside, with no place outdoors known).
bool Where(const void* n3, int* map, Spot* spot)
{
    if (!g_spotRead)
        ReadSpot();
    const game::Identity me{50000, game::ClientInstance()};
    game::Identity pf{};
    float pos[3] = {};
    const bool known = PositionData(n3, &me, &pf, pos);
    const int m = known ? MapOf(pf.instance) : -1;
    if (m >= 0) {
        if (pf.instance != g_zoneLogged) {
            g_zoneLogged = pf.instance;
            Log("[minimap] playfield %u (type 0x%X) is on planet map %d (%s)", pf.instance, pf.type, m, g_mapFile[m].c_str());
        }
        g_spot = Spot{pf.instance, pos[0], pos[2]};
        g_spotSaved = false;
        *map = m;
        *spot = g_spot;
        return true;
    }
    if (!g_spotSaved)
        SaveSpot();
    if (known && pf.instance != g_insideLogged) {
        g_insideLogged = pf.instance;
        Log("[minimap] playfield %u (type 0x%X) isn't on the planet maps - the planet layer shows zone %u, where you were last outdoors",
            pf.instance, pf.type, g_spot.zone);
    }
    *map = MapOf(g_spot.zone);
    *spot = g_spot;
    return *map >= 0;
}

// Dungeons the planet map has a picture of (Saavick's map draws some static dungeons, e.g. Temple of
// Three Winds, on the map itself, at their own place in its coordinates file): there the planet layer
// is used whatever [Minimap] Layer says. Such a dungeon is a playfield on one of the maps where
// MapData_c has rooms and no ground (indoors: the local layer would show only the game's room map).
// The layers button there switches between the two for that visit only (g_dungeonLocal, not saved).
uint32_t g_dungeonPf = 0;      // the playfield the choice below is for
bool g_dungeonLocal = false;   // in that dungeon, the local layer was picked with the layers button
bool g_inMappedDungeon = false; // this frame: you're in such a dungeon
bool g_planetWanted = false;    // this frame: the planet layer is the one to show

bool InMappedDungeon(const void* n3, const uint8_t* md)
{
    if (!n3 || !md || At<void*>(md, kMdGround) || At<const uint8_t*>(md, kMdRoomsBegin) == At<const uint8_t*>(md, kMdRoomsEnd))
        return false;
    ReadMapChoice();
    const game::Identity me{50000, game::ClientInstance()};
    game::Identity pf{};
    float pos[3] = {};
    if (!PositionData(n3, &me, &pf, pos))
        return false;
    if (pf.instance != g_dungeonPf) {
        g_dungeonPf = pf.instance;
        g_dungeonLocal = false;
    }
    return MapOf(pf.instance) >= 0;
}

// The level shown: [Minimap] PlanetLevel, -1 (or out of range) = the most detailed.
int PlanetLevelShown()
{
    const int n = static_cast<int>(g_maps[g_mapShown].levels.size());
    return g_planetLevel >= 0 && g_planetLevel < n ? g_planetLevel : n - 1;
}

// The planet layer: its tiles around the view's centre (where you are, moved by the pan), a texel a GUI
// unit; *youX / *youY where you are on the screen. False if there's nothing to show here.
bool Planet(void* viewport, const void* n3, float cx, float cy, float r, float* youX, float* youY)
{
    ReadMapChoice();
    int map = -1;
    Spot spot;
    if (!Where(n3, &map, &spot))
        return false;
    g_mapShown = map;
    const PlanetMap& pm = g_maps[map];
    const int level = PlanetLevelShown();
    const PlanetLevel& l = pm.levels[static_cast<size_t>(level)];
    const ZoneCoords& z = pm.zones.at(spot.zone);
    float ux, uy, px, py;
    PlanetPixel(z, l, spot.x, spot.z, &ux, &uy);
    PlanetPixel(z, l, spot.x + g_pan.x, spot.z + g_pan.z, &px, &py);
    *youX = cx + (ux - px);
    *youY = cy + (uy - py);
    g_view = PlanetView{map, level, px, py};
    g_ppmX = static_cast<float>((l.right - l.left) / kPlanetSpanX * z.xscale);
    g_ppmZ = static_cast<float>((l.bottom - l.top) / kPlanetSpanZ * z.zscale);
    const float k = 1.0f, t = static_cast<float>(l.texture);
    const float reach = r / k;
    const int c0 = std::max(0, static_cast<int>(std::floor((px - reach) / t)));
    const int c1 = std::min(l.cols - 1, static_cast<int>(std::floor((px + reach) / t)));
    const int r0 = std::max(0, static_cast<int>(std::floor((py - reach) / t)));
    const int r1 = std::min(l.rows - 1, static_cast<int>(std::floor((py + reach) / t)));
    for (int row = r0; row <= r1; ++row)
        for (int col = c0; col <= c1; ++col)
            if (void* tex = PlanetTile(pm, map, level, col, row))
                ScreenQuad(viewport, g_textured, tex, 0xFFFFFFFF, cx + (col * t - px) * k, cy + (row * t - py) * k,
                           cx + ((col + 1) * t - px) * k, cy + ((row + 1) * t - py) * k, 0.0f, 1.0f, cx, cy, r);
    TrimPlanetTiles(map, level, static_cast<int>(px / t), static_cast<int>(py / t));
    return true;
}

// --- markers: what's around you ---------------------------------------------------------------
//
// N3Msg_GetMapCharacters(range, list, count) (Gamecode 0x1001BF31) clears a std::list<MapCharacterEntry_t>
// and fills it with what's within `range` metres, as the game's map does every 100 ms (MapData_c +0x40;
// its EventTimer at +0xD0, started for 100000 us by GUI 0x10042416 while the map window is open).
// VS2010 std::list: {head node, size, allocator}; nodes {next, prev, value}. The value, as the map window
// reads it (GUI 0x100EAA06 / 0x100EA626): +0x00 Identity, +0x08 position, +0x14 rotation (x, y, z, w),
// +0x24 / +0x28 a cone's range (metres) / half-angle (radians), +0x2C category, +0x30 the room (zone
// instance) it's in.
// Categories, and the GUI image (number -> name: AFCM.dll's table) the map window shows them with:
// 0 Clan (GFX_GUI_MAPSQUARE_CLAN), 1 Omni (_OMNI), 2 neutral (_NEUTRAL), 3 monsters (_MONSTER), 4 team
// members (_TEAMMEMBER; Gamecode 0x1006581F / 0x10065865), 5 vending machines (_SHOP), 6 reclaim booths
// (GFX_GUI_MAPDOT_CYAN), 7 / 8 (GFX_GUI_MAPFLAG_CYAN; 8 is dynel type 0xC748), 9 cones (turrets and
// the like: what they cover).
//
// Drawn exactly as the map window draws them, from the same textures made the same way:
//  - the marker atlas (GUI 0x1004667C): 32x32, the GUI's 16-bit pixels; 13 images, each 4x4 copied to
//    (2, 2) of its 8x8 cell, cells left to right, 4 a row; a marker is its whole 8x8 cell drawn 8x8 GUI
//    units around the point (GUI 0x10046A30 -> 0x10046159), so it's soft-edged like the game's;
//  - the arrow atlas (GUI 0x100E9BA2): 64x64, 12 images centred in 16x16 cells; an arrow is its cell
//    drawn 16x16 around the point, turned (GUI 0x100E92EF): you GFX_GUI_MAP_ARROW (cell 0), your selected
//    target GFX_GUI_MAP_ARROW_SELECTED (cell 1, with MapData_c +0x81), a character marked by a voice
//    command GFX_GUI_MAP_BIG_ARROW (cell 2). Arrows show on every floor; the other markers only on yours.
//    Marked: a voice command whose text starts with a "[...]" holding M or m (GUI 0x1008C17B, for the ones
//    you send and the tells you get) sends GlobalSignals_c +0x150 (Identity, 15 s), and MapData_c
//    (0x10042B80) keeps a std::map<Identity, __time64_t until> at +0xA0 (head +0xA4; nodes: left +0,
//    parent +4, right +8, key +0x10, until +0x18); its pruning timer (+0x110) only runs while the game's map
//    window is open, so the minimap reads the time too;
//  - cones (GUI 0x100EA626 -> 0x10046400, after the markers, every floor): a fan of the point and 8 points
//    on an arc of the cone's range, from facing - half-angle to facing + half-angle (facing as the arrows'),
//    untextured, 0x47FF0000.
// All x zoom.
constexpr int kMapRange = 500;          // as the map window
constexpr DWORD kMarkersEveryMs = 90; // each 100 ms tick (GetTickCount's steps can make it 9x ms)
constexpr int kNodeValue = 8;
constexpr int kEntryPos = 0x08, kEntryRot = 0x14, kEntryRange = 0x24, kEntryHalfAngle = 0x28, kEntryCategory = 0x2C,
              kEntryRoom = 0x30;
constexpr int kListAlloc = 0x10, kNodeAlloc = 0x80;
constexpr int kCategories = 10;
constexpr int kMarkerImages[13] = {0x102, 0x106, 0x105, 0x104, 0x108, 0xFE, 0x101, 0x109, 0x103, 0x107, 0xFF, 0xFD, 0x100};
constexpr int kCategoryCell[kCategories] = {0, 1, 2, 3, 7, 4, 5, 6, 6, -1};
constexpr int kCategoryCone = 9;
constexpr uint32_t kConeArgb = 0x47FF0000;
constexpr int kConeArcPoints = 8;
constexpr int kArrowImages[12] = {0x10A, 0x10B, 0x10C, 0x111, 0x113, 0x112, 0x114, 0x116, 0x115, 0x10D, 0x10F, 0x10E};
constexpr int kArrowCellYou = 0, kArrowCellSelected = 1, kArrowCellMarked = 2;
constexpr int kMdMarked = 0xA4;                    // MapData_c: the voice-marked characters' std::map head
constexpr int kMarkedKey = 0x10, kMarkedUntil = 0x18; // its nodes
constexpr int kSpriteFormatGui = 2; // SpriteInfo_t Format_e: the GUI's

struct Marker {
    game::Identity id;
    float x, z;
    float heading; // radians, clockwise from north (arrows, cones)
    float range, halfAngle; // cones: metres, radians
    int category;
    uint32_t room;
};
std::vector<Marker> g_markers; // the latest list (game thread)
std::vector<game::Identity> g_marked; // characters a voice command marked, still in time
bool g_markersShown = false;   // N3Msg_GetMapCharacters said yes (the map window draws them only then)
DWORD g_markersAt = 0;
void* g_list = nullptr;        // our std::list
bool g_atlasesMade = false;

struct Atlas {
    void* tex = nullptr;
    int size = 0, cell = 0; // texture and cell size, texels
};
Atlas g_markerAtlas, g_arrowAtlas;

// As the map window: each GUI image's 16-bit pixels copied into its cell (marker atlas: at (2, 2) of an
// 8x8 cell; arrow atlas: centred in a 16x16 one), then AllocateTexture in the images' format and
// RTexture_t::Load. Only for the GUI's 16-bit images (the window copies 2 bytes a pixel too).
Atlas MakeAtlas(const int* ids, int count, int size, int cell, bool centred, const char* what)
{
    Atlas a;
    std::vector<uint16_t> pixels(static_cast<size_t>(size) * size, 0);
    int format = 0;
    void* mgr = GuiResources();
    for (int i = 0; mgr && i < count; ++i) {
        void* image = GuiTexture(mgr, ids[i], kSpriteFormatGui);
        if (!image)
            continue;
        const int w = SpriteWidth(image), h = SpriteHeight(image), stride = SpriteStride(image), bpp = SpriteBpp(image);
        const uint8_t* data = SpriteData(image);
        if (data && bpp == 2) {
            format = SpriteFormat(image);
            const int cx = (i % 4) * cell, cy = (i / 4) * cell;
            const int ox = centred ? (cell - w) / 2 : 2, oy = centred ? (cell - h) / 2 : 2;
            const int cw = centred ? w : std::min(w, 4), ch = centred ? h : std::min(h, 4);
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    const int px = cx + ox + x, py = cy + oy + y;
                    if (px >= 0 && py >= 0 && px < size && py < size)
                        memcpy(&pixels[static_cast<size_t>(py) * size + px], data + y * stride + x * 2, 2);
                }
        }
        GuiRelease(mgr, image);
    }
    if (!format) {
        Log("[minimap] the %s atlas couldn't be made (no 16-bit GUI images)", what);
        return a;
    }
    a.tex = AllocateTexture(size, size, false, format, nullptr);
    if (!a.tex || !TextureLoad(a.tex, pixels.data(), format, static_cast<unsigned>(size * 2), nullptr, nullptr, true)) {
        Log("[minimap] the %s atlas couldn't be loaded (format %d)", what, format);
        a.tex = nullptr;
        return a;
    }
    a.size = size;
    a.cell = cell;
    return a;
}

// One GUI image as a texture (its 16-bit pixels at the top-left of a power-of-two square).
struct Image {
    void* tex = nullptr;
    int w = 0, h = 0, size = 0;
};
Image g_planetYou;
Image g_wpPlanet; // GFX_GUI_PLANETMAP_MISSION_MARKER2: the waypoint on the planet layer

Image MakeImage(int id, const char* what)
{
    Image img;
    void* mgr = GuiResources();
    void* image = mgr ? GuiTexture(mgr, id, kSpriteFormatGui) : nullptr;
    if (image) {
        const int w = SpriteWidth(image), h = SpriteHeight(image), stride = SpriteStride(image), bpp = SpriteBpp(image);
        const uint8_t* data = SpriteData(image);
        int size = 1;
        while (size < std::max(w, h))
            size <<= 1;
        if (data && bpp == 2 && w > 0 && h > 0 && size <= 256) {
            std::vector<uint16_t> pixels(static_cast<size_t>(size) * size, 0);
            for (int y = 0; y < h; ++y)
                memcpy(&pixels[static_cast<size_t>(y) * size], data + y * stride, static_cast<size_t>(w) * 2);
            const int format = SpriteFormat(image);
            void* tex = AllocateTexture(size, size, false, format, nullptr);
            if (tex && TextureLoad(tex, pixels.data(), format, static_cast<unsigned>(size * 2), nullptr, nullptr, true))
                img = Image{tex, w, h, size};
        }
        GuiRelease(mgr, image);
    }
    if (img.tex)
        Log("[minimap] %s image: %dx%d", what, img.w, img.h);
    else
        Log("[minimap] the %s image couldn't be made", what);
    return img;
}

// The planet map window's marker for you (GUI 0x1004BC1F: GFX_GUI_PLANETMAP_PLAYER_MARKER, 0x12B), as a
// ViewSurface_c of the image at its own size with the point (8, 8) of it on your place (0x1004B81A), not
// turned.
constexpr int kPlanetYouImage = 0x12B;
constexpr float kPlanetYouHotspot = 8.0f;
// The mission waypoint: on the planet map window GFX_GUI_PLANETMAP_MISSION_MARKER2 (0x128) with its point
// (8, 0) on the place (0x1004BD7A); on the local map the marker atlas's GFX_GUI_MAPSQUARE_MISSION (0x103,
// cell 8). Its direction: a gold triangle on the frame's rim, pointing out (MissionMarker).
constexpr int kPlanetMissionImage = 0x128;
constexpr float kPlanetMissionHotX = 8.0f, kPlanetMissionHotY = 0.0f;
constexpr int kCellMission = 8;

void MakeAtlases()
{
    g_atlasesMade = true;
    g_markerAtlas = MakeAtlas(kMarkerImages, 13, 32, 8, false, "marker");
    g_arrowAtlas = MakeAtlas(kArrowImages, 12, 64, 16, true, "arrow");
    g_planetYou = MakeImage(kPlanetYouImage, "planet map player marker");
    g_wpPlanet = MakeImage(kPlanetMissionImage, "planet map mission marker");
}

// Cell `i` of an atlas, size x size GUI units around (x, y), turned by `angle` (radians clockwise).
void DrawCell(void* viewport, const Atlas& a, int i, float x, float y, float size, float angle)
{
    if (!a.tex || !Begin(viewport, g_textured, a.tex))
        return;
    const float k = static_cast<float>(a.cell) / a.size;
    const float u0 = (i % 4) * k, v0 = (i / 4) * k;
    const float c = std::cos(angle), s = std::sin(angle), h = size * 0.5f;
    auto pt = [&](float dx, float dy, float u, float v) { return V(x + dx * c - dy * s, y + dx * s + dy * c, 0xFFFFFFFF, u, v); };
    Vertex v[4] = {pt(-h, -h, u0, v0), pt(h, -h, u0 + k, v0), pt(h, h, u0 + k, v0 + k), pt(-h, h, u0, v0 + k)};
    RenderFan(*RenderInstance, kFvf, v, 4, 0);
    End(viewport, g_textured);
}

struct ListNode2 {
    ListNode2* next;
    ListNode2* prev;
};

// Our std::list: frees the nodes the game made and leaves it empty.
void ClearList()
{
    auto* list = static_cast<ListNode2**>(g_list);
    ListNode2* head = list[0];
    for (ListNode2* n = head->next; n && n != head;) {
        ListNode2* next = n->next;
        CrtDelete(n);
        n = next;
    }
    head->next = head->prev = head;
    reinterpret_cast<size_t*>(g_list)[1] = 0;
}

bool MakeList()
{
    g_list = CrtNew(kListAlloc);
    void* head = CrtNew(kNodeAlloc);
    if (!g_list || !head)
        return false;
    memset(g_list, 0, kListAlloc);
    memset(head, 0, kNodeAlloc);
    auto* h = static_cast<ListNode2*>(head);
    h->next = h->prev = h;
    static_cast<void**>(g_list)[0] = head;
    return true;
}

void RefreshMarkers(const void* n3)
{
    if (!g_list && !MakeList()) {
        g_list = nullptr;
        return;
    }
    int count = 0;
    g_markersShown = MapCharacters(n3, kMapRange, g_list, &count);
    g_markers.clear();
    ListNode2* head = static_cast<ListNode2**>(g_list)[0];
    int guard = 0;
    for (ListNode2* n = head->next; n && n != head && guard < 4096; n = n->next, ++guard) {
        const uint8_t* v = reinterpret_cast<const uint8_t*>(n) + kNodeValue;
        const Vec3f pos = At<Vec3f>(v, kEntryPos);
        float rot[4];
        memcpy(rot, v + kEntryRot, sizeof(rot));
        const Vec3f f = Rotate(rot, Vec3f{0.0f, 0.0f, 1.0f});
        const int category = At<int>(v, kEntryCategory);
        g_markers.push_back(Marker{At<game::Identity>(v, 0), pos.x, pos.z,
                                   (f.x != 0.0f || f.z != 0.0f) ? std::atan2(f.x, f.z) : 0.0f, At<float>(v, kEntryRange),
                                   At<float>(v, kEntryHalfAngle), category, At<uint32_t>(v, kEntryRoom)});
    }
    ClearList();
    // The voice-marked characters: MapData_c's std::map, walked from its root (head->parent; the head is
    // every leaf's child), those whose time hasn't run out.
    g_marked.clear();
    const uint8_t* md = g_mapData ? static_cast<const uint8_t*>(*g_mapData) : nullptr;
    const uint8_t* mhead = md ? At<const uint8_t*>(md, kMdMarked) : nullptr;
    if (!mhead)
        return;
    const __time64_t now = _time64(nullptr);
    const uint8_t* stack[64];
    int depth = 0, steps = 0;
    if (const uint8_t* root = At<const uint8_t*>(mhead, 4); root && root != mhead)
        stack[depth++] = root;
    while (depth > 0 && steps++ < 1024) {
        const uint8_t* n = stack[--depth];
        if (At<__time64_t>(n, kMarkedUntil) >= now)
            g_marked.push_back(At<game::Identity>(n, kMarkedKey));
        for (int side : {0, 8}) {
            const uint8_t* c = At<const uint8_t*>(n, side);
            if (c && c != mhead && depth < 64)
                stack[depth++] = c;
        }
    }
}

bool Marked(const game::Identity& id)
{
    for (const game::Identity& m : g_marked)
        if (m.type == id.type && m.instance == id.instance)
            return true;
    return false;
}

// Indoors only what's on your floor (the room it's in, by MapData_c's room ids at entry +0x00).
int RoomFloor(const uint8_t* md, uint32_t room)
{
    const uint8_t* begin = At<const uint8_t*>(md, kMdRoomsBegin);
    const uint8_t* end = At<const uint8_t*>(md, kMdRoomsEnd);
    for (const uint8_t* r = begin; begin && r < end; r += kRoomSize)
        if (At<uint32_t>(r, 0) == room)
            return At<int>(r, kRoomFloor);
    return INT32_MIN;
}

// A cone at (x, y): `radius` GUI units out, facing `heading` (radians clockwise from north) +- halfAngle,
// cut to the map's circle.
void Cone(void* viewport, float x, float y, float radius, float heading, float halfAngle, float cx, float cy, float r)
{
    if (!(radius > 0.0f) || !(halfAngle > 0.0f) || radius > 4096.0f)
        return;
    Pt fan[kConeArcPoints + 1];
    fan[0] = Pt{x, y, 0.0f, 0.0f};
    for (int i = 0; i < kConeArcPoints; ++i) {
        const float a = heading - halfAngle + 2.0f * halfAngle * i / (kConeArcPoints - 1);
        fan[i + 1] = Pt{x + std::sin(a) * radius, y - std::cos(a) * radius, 0.0f, 0.0f};
    }
    Pt poly[kClipCap];
    const int n = ClipToCircle(fan, kConeArcPoints + 1, cx, cy, r, poly);
    if (n < 3 || !Begin(viewport, g_flat, nullptr))
        return;
    Vertex v[kClipCap];
    for (int i = 0; i < n; ++i)
        v[i] = V(poly[i].x, poly[i].y, kConeArgb);
    RenderFan(*RenderInstance, kFvf, v, static_cast<unsigned long>(n), 0);
    End(viewport, g_flat);
}

void Markers(void* viewport, const uint8_t* md, const game::Vec3& me, float cx, float cy, float r, float ppm)
{
    if (!g_markersShown)
        return;
    const bool indoors = !At<void*>(md, kMdGround) && At<const uint8_t*>(md, kMdRoomsBegin) != At<const uint8_t*>(md, kMdRoomsEnd);
    const int floor = At<int>(md, kMdFloor);
    game::Identity selected{};
    const bool showSelected = md[kMdShowSelected] != 0 && n3::SelectedTarget(&selected);
    for (const Marker& m : g_markers) {
        if (m.category < 0 || m.category >= kCategories || m.category == kCategoryCone)
            continue;
        const float sx = cx + (m.x - me.x) * ppm, sy = cy - (m.z - me.z) * ppm;
        if ((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy) > (r - 3.0f) * (r - 3.0f))
            continue;
        // An arrow (as the game: on every floor) for a voice-marked character or your selected target.
        const bool marked = Marked(m.id);
        if ((marked || (showSelected && m.id.type == selected.type && m.id.instance == selected.instance)) &&
            g_arrowAtlas.tex) {
            DrawCell(viewport, g_arrowAtlas, marked ? kArrowCellMarked : kArrowCellSelected, sx, sy, 16.0f * g_zoom,
                     m.heading);
            continue;
        }
        if (indoors) {
            const int f = RoomFloor(md, m.room);
            if (f != INT32_MIN && f != floor)
                continue;
        }
        DrawCell(viewport, g_markerAtlas, kCategoryCell[m.category], sx, sy, 8.0f * g_zoom, 0.0f);
    }
    for (const Marker& m : g_markers) // the cones, over the markers (every floor, as the game's)
        if (m.category == kCategoryCone)
            Cone(viewport, cx + (m.x - me.x) * ppm, cy - (m.z - me.z) * ppm, m.range * ppm, m.heading, m.halfAngle, cx, cy, r);
}

// --- the mission waypoint ---------------------------------------------------------------------
//
// GUIModuleActivator_c (ControlCenterModule_c +0x1A0, made in ControlCenterModule_c::InitialiseMessage)
// keeps the last waypoint the game's waypoint signal sent (GUI 0x1002C8F0; 0x1002C918 clears it when it
// goes): +0x08 the target's Identity, +0x10 a position, +0x1C its playfield. The planet map window
// (0x1004B9C9 / 0x1004B81A) places a target by N3Msg_GetPositionData(target), else the position in that
// playfield; so does the minimap.
constexpr int kCcActivator = 0x1A0, kWpTarget = 0x08, kWpPos = 0x10, kWpZone = 0x1C;

bool Waypoint(const void* n3, uint32_t* zone, float* x, float* z)
{
    const void* cc = ControlCenter();
    const uint8_t* act = cc ? At<const uint8_t*>(static_cast<const uint8_t*>(cc), kCcActivator) : nullptr;
    if (!act)
        return false;
    const game::Identity target = At<game::Identity>(act, kWpTarget);
    if (target.type != 0 || target.instance != 0) {
        game::Identity pf{};
        float pos[3] = {};
        if (!PositionData(n3, &target, &pf, pos))
            return false;
        *zone = pf.instance;
        *x = pos[0];
        *z = pos[2];
        return true;
    }
    const uint32_t pf = At<uint32_t>(act, kWpZone);
    if (pf == 0)
        return false;
    const Vec3f pos = At<Vec3f>(act, kWpPos);
    *zone = pf;
    *x = pos.x;
    *z = pos.z;
    return true;
}

// The waypoint from you (metres east, north), if it's in your playfield (only then is it shown).
bool WaypointOffset(const void* n3, uint32_t zone, float wx, float wz, float* dx, float* dz)
{
    const game::Identity me{50000, game::ClientInstance()};
    game::Identity pf{};
    float pos[3] = {};
    if (!PositionData(n3, &me, &pf, pos) || pf.instance != zone)
        return false;
    *dx = wx - pos[0];
    *dz = wz - pos[2];
    return true;
}

// --- the compass ----------------------------------------------------------------------------

// --- the frame, the badges and their glyphs -----------------------------------------------------
//
// The render states of the game's map draw triangles without smoothing, so the frame and the badges are
// painted in software with smooth edges (coverage per pixel) and drawn 1:1 on whole pixels: the frame as
// one texture made for the minimap's size, each badge from a 32x32 badge texture, each glyph from its
// 32x32 cell of the compass atlas. (Pixel centres are at whole coordinates: quads start half a pixel up
// and left.)

struct Premul {
    float r = 0, g = 0, b = 0, a = 0;
};

void Over(Premul& d, uint32_t rgb, float a)
{
    if (a <= 0.0f)
        return;
    const float cr = ((rgb >> 16) & 0xFF) / 255.0f, cg = ((rgb >> 8) & 0xFF) / 255.0f, cb = (rgb & 0xFF) / 255.0f;
    d.r = cr * a + d.r * (1.0f - a);
    d.g = cg * a + d.g * (1.0f - a);
    d.b = cb * a + d.b * (1.0f - a);
    d.a = a + d.a * (1.0f - a);
}

uint32_t Mix(uint32_t a, uint32_t b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        const float x = ((a >> sh) & 0xFF) * (1.0f - t) + ((b >> sh) & 0xFF) * t;
        out |= static_cast<uint32_t>(std::lround(x)) << sh;
    }
    return out;
}

uint32_t Straight(const Premul& p)
{
    if (p.a <= 0.0f)
        return 0;
    auto c = [&](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v / p.a, 0.0f, 1.0f) * 255.0f)); };
    return (static_cast<uint32_t>(std::lround(std::clamp(p.a, 0.0f, 1.0f) * 255.0f)) << 24) | (c(p.r) << 16) | (c(p.g) << 8) | c(p.b);
}

// How much of the pixel at distance d lies between radii r0 and r1.
float RingCover(float d, float r0, float r1)
{
    return std::clamp(d - r0 + 0.5f, 0.0f, 1.0f) * std::clamp(r1 - d + 0.5f, 0.0f, 1.0f);
}

struct Art {
    void* tex = nullptr;
    int size = 0; // the texture's side (a power of two)
    int w = 0, h = 0;
};
Art g_frameArt; // the frame, for the minimap's current pixel size
Art g_badgeArt; // a badge: dark edge, gunmetal ring as the frame's, dark inside

int PowerOfTwo(int v)
{
    int n = 32;
    while (n < v)
        n *= 2;
    return n;
}

void* MakeArt(const char* name, const std::vector<uint32_t>& pixels, int size)
{
    void* tex = game::CreateTextureARGB(name, size, size);
    if (tex && !game::WriteTexture(tex, pixels.data(), size, size)) {
        ReleaseTexture(tex);
        tex = nullptr;
    }
    return tex;
}

// The frame around a map circle of radius r in a w x h area: a dark gap, the gunmetal ring with teal
// glow arcs, a dark outline - as the bars' frames.
void PaintFrame(int w, int h, float r)
{
    if (g_frameArt.tex)
        ReleaseTexture(g_frameArt.tex);
    g_frameArt = Art{};
    const int size = PowerOfTwo(std::max(w, h));
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size, 0);
    const float cx = w * 0.5f, cy = h * 0.5f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < r - 2.0f || d > r + 11.0f)
                continue;
            const float light = d > 0.0f ? (-dx * 0.45f - dy * 0.9f) / d : 0.0f; // -1..1, the top lighter
            Premul px;
            Over(px, 0x08090B, RingCover(d, r - 1.0f, r + 2.0f));
            Over(px, Mix(0x2C3136, 0x434A51, light * 0.5f + 0.5f), RingCover(d, r + 2.0f, r + 9.0f));
            float a = std::atan2(dy, dx); // clockwise from the right
            if (a < 0.0f)
                a += 2.0f * kPi;
            for (int i = 0; i < 4; ++i) { // the teal arcs, soft at their ends
                const float a0 = kPi * 0.5f * i + 0.35f, a1 = a0 + 0.75f;
                const float along = std::min(a - a0, a1 - a) * d; // pixels from the nearer end
                Over(px, 0x28D2BE, RingCover(d, r + 5.0f, r + 6.0f) * std::clamp(along + 0.5f, 0.0f, 1.0f));
            }
            Over(px, 0x0B0C0E, RingCover(d, r + 9.0f, r + 10.0f));
            pixels[static_cast<size_t>(y) * size + x] = Straight(px);
        }
    g_frameArt.tex = MakeArt("clarity_minimap_frame", pixels, size);
    g_frameArt.size = size;
    g_frameArt.w = w;
    g_frameArt.h = h;
}

void PaintBadge()
{
    std::vector<uint32_t> pixels(32 * 32, 0);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float dx = x + 0.5f - 16.0f, dy = y + 0.5f - 16.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float light = d > 0.0f ? (-dx * 0.45f - dy * 0.9f) / d : 0.0f;
            Premul px;
            Over(px, 0x0B0C0E, RingCover(d, -1.0f, 11.0f)); // a dark edge
            Over(px, Mix(0x2C3136, 0x58606A, light * 0.5f + 0.5f), RingCover(d, -1.0f, 10.0f));
            Over(px, 0x15181B, RingCover(d, -1.0f, 8.0f));
            pixels[static_cast<size_t>(y) * 32 + x] = Straight(px);
        }
    g_badgeArt.tex = MakeArt("clarity_minimap_badge", pixels, 32);
    g_badgeArt.size = 32;
    g_badgeArt.w = g_badgeArt.h = 32;
}

// A texture's w x h part drawn 1:1 with its top left at whole pixel (x, y); u0 offsets into it.
void Blit(void* viewport, void* tex, float x, float y, float w, float h, float u0, float u1, float v1, uint32_t argb)
{
    if (!tex || !Begin(viewport, g_textured, tex))
        return;
    x -= 0.5f;
    y -= 0.5f;
    Vertex v[4] = {V(x, y, argb, u0, 0.0f), V(x + w, y, argb, u1, 0.0f), V(x + w, y + h, argb, u1, v1),
                   V(x, y + h, argb, u0, v1)};
    RenderFan(*RenderInstance, kFvf, v, 4, 0);
    End(viewport, g_textured);
}

// A badge with glyph k (N, E, S, W, +, -, layers) on it at angle a (radians clockwise from the top) on
// the frame's band, rad px from the centre.
void Badge(void* viewport, int k, float cx, float cy, float rad, float a, uint32_t argb)
{
    const float bx = std::round(cx + std::sin(a) * rad) - 16.0f, by = std::round(cy - std::cos(a) * rad) - 16.0f;
    if (!g_badgeArt.tex)
        PaintBadge();
    Blit(viewport, g_badgeArt.tex, bx, by, 32.0f, 32.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
    Blit(viewport, g_compass, bx, by, 32.0f, 32.0f, static_cast<float>(k) / kCompassCells,
         static_cast<float>(k + 1) / kCompassCells, 1.0f, argb);
}

// The four letters on the frame, north at `north` (radians clockwise from the top; 0 = north up); north
// in gold.
void Compass(void* viewport, float cx, float cy, float r, float north)
{
    const float band = r + kRingMid;
    for (int k = 0; k < 4; ++k)
        Badge(viewport, k, cx, cy, band, north + kPi * 0.5f * k, k == 0 ? 0xFFFFC83C : 0xFFE6E8EA);
}

// The zoom buttons: on the frame between E and S (radians clockwise from the top).
constexpr float kZoomInAngle = 2.0f * 3.14159265f * 125.0f / 360.0f;
constexpr float kZoomOutAngle = 2.0f * 3.14159265f * 150.0f / 360.0f;
constexpr float kLayerAngle = 2.0f * 3.14159265f * 225.0f / 360.0f; // between S and W
constexpr float kButtonRadius = 11.0f;

// On the planet layer (when it's showing) + and - step its levels; on the local map they zoom.
bool g_planetShown = false; // this frame showed the planet layer

bool ZoomInAvailable()
{
    return g_planetShown ? PlanetLevelShown() < static_cast<int>(g_maps[g_mapShown].levels.size()) - 1 : g_zoom < kZoomMax;
}

bool ZoomOutAvailable()
{
    return g_planetShown ? PlanetLevelShown() > 0 : g_zoom > kZoomMin;
}

void ZoomButtons(void* viewport, float cx, float cy, float r)
{
    const float band = r + kRingMid;
    Badge(viewport, kGlyphPlus, cx, cy, band, kZoomInAngle, ZoomInAvailable() ? 0xFF28D2BE : 0xFF5A6066);
    Badge(viewport, kGlyphMinus, cx, cy, band, kZoomOutAngle, ZoomOutAvailable() ? 0xFF28D2BE : 0xFF5A6066);
    // The layer button: the layers symbol, teal on the local map, gold on the planet map.
    Badge(viewport, kGlyphLayers, cx, cy, band, kLayerAngle, g_planetWanted ? 0xFFFFC83C : 0xFF28D2BE);
}

void Arrow(void* viewport, float cx, float cy, float size, float heading, uint32_t fill, uint32_t edge)
{
    if (!Begin(viewport, g_flat, nullptr))
        return;
    const float c = std::cos(heading), s = std::sin(heading);
    for (int pass = 0; pass < 2; ++pass) {
        const float k = pass == 0 ? size * 1.25f : size;
        const uint32_t argb = pass == 0 ? edge : fill;
        auto pt = [&](float x, float y) { return V(cx + x * c - y * s, cy + x * s + y * c, argb); };
        Vertex v[4] = {pt(0, -1.0f * k), pt(0.7f * k, 0.8f * k), pt(0, 0.4f * k), pt(-0.7f * k, 0.8f * k)};
        RenderFan(*RenderInstance, kFvf, v, 4, 0);
    }
    End(viewport, g_flat);
}

// You: the local map's arrow (its 16x16 cell, the 8x8 arrow in it), x zoom, pointing `heading`.
void PlayerArrow(void* viewport, float cx, float cy, float heading)
{
    if (g_arrowAtlas.tex)
        DrawCell(viewport, g_arrowAtlas, kArrowCellYou, cx, cy, 16.0f * g_zoom, heading);
    else
        Arrow(viewport, cx, cy, 4.0f * g_zoom, heading, 0xFFFFECAA, 0xFF141000);
}

// You on the planet layer: the planet map window's marker, 1:1.
void PlanetYou(void* viewport, float x, float y, float heading)
{
    if (!g_planetYou.tex) {
        PlayerArrow(viewport, x, y, heading);
        return;
    }
    const float k = 1.0f / g_planetYou.size;
    Blit(viewport, g_planetYou.tex, std::round(x - kPlanetYouHotspot), std::round(y - kPlanetYouHotspot),
         static_cast<float>(g_planetYou.w), static_cast<float>(g_planetYou.h), 0.0f, g_planetYou.w * k,
         g_planetYou.h * k, 0xFFFFFFFF);
}

// A GUI image 1:1 with its top left at (x, y) (rounded to whole pixels).
void BlitImage(void* viewport, const Image& img, float x, float y)
{
    const float k = 1.0f / img.size;
    Blit(viewport, img.tex, std::round(x), std::round(y), static_cast<float>(img.w), static_cast<float>(img.h), 0.0f,
         img.w * k, img.h * k, 0xFFFFFFFF);
}

// The mission waypoint, when it's in your playfield: its marker where it is on the map, and *angle its
// direction from the map's centre (radians clockwise from the top). False if there's none here. ppm: the
// local map's GUI units a metre (the planet layer uses g_view).
bool WaypointMarker(void* viewport, const void* n3, bool planet, float ppm, float cx, float cy, float r, float* angle)
{
    uint32_t zone = 0;
    float wx = 0.0f, wz = 0.0f, sx = 0.0f, sy = 0.0f;
    if (!Waypoint(n3, &zone, &wx, &wz))
        return false;
    float dx, dz;
    if (!WaypointOffset(n3, zone, wx, wz, &dx, &dz))
        return false;
    if (planet) {
        const int m = MapOf(zone);
        if (m < 0 || m != g_view.map)
            return false;
        float px, py;
        PlanetPixel(g_maps[m].zones.at(zone), g_maps[m].levels[static_cast<size_t>(g_view.level)], wx, wz, &px, &py);
        sx = cx + (px - g_view.px);
        sy = cy + (py - g_view.py);
    } else {
        if (!(ppm > 0.0f))
            return false;
        sx = cx + (dx - g_pan.x) * ppm;
        sy = cy - (dz - g_pan.z) * ppm;
    }
    const float ex = sx - cx, ey = sy - cy;
    if (ex * ex + ey * ey <= (r - 6.0f) * (r - 6.0f)) {
        if (planet && g_wpPlanet.tex)
            BlitImage(viewport, g_wpPlanet, sx - kPlanetMissionHotX, sy - kPlanetMissionHotY);
        else if (!planet)
            DrawCell(viewport, g_markerAtlas, kCellMission, sx, sy, 8.0f * g_zoom, 0.0f);
    }
    if (ex * ex + ey * ey < 1.0f)
        return false; // right at the centre: no direction
    *angle = std::atan2(ex, -ey);
    return true;
}

// The rim markers: triangles with a dark edge on the frame's ring. The heading marker (light) points in
// from where you face, the mission marker (gold, as the N) points out toward the waypoint; the heading is
// drawn last, over the other where they meet. Painted once each (16x16, smooth edges, pointing in), drawn
// turned.
Art g_headingArt, g_missionArt;
constexpr int kHeadingPx = 16;
constexpr uint32_t kHeadingRgb = 0xF4F6F8, kMissionRgb = 0xFFC83C;

Art PaintRimMarker(const char* name, uint32_t rgb)
{
    constexpr int S = kHeadingPx;
    const float pts[3][2] = {{2.5f, 3.0f}, {13.5f, 3.0f}, {8.0f, 13.0f}}; // pointing down (in, at the top)
    auto depth = [&](float x, float y) { // how far inside all three edges
        float d = 1e9f;
        for (int i = 0; i < 3; ++i) {
            const float x0 = pts[i][0], y0 = pts[i][1], x1 = pts[(i + 1) % 3][0], y1 = pts[(i + 1) % 3][1];
            const float ex = x1 - x0, ey = y1 - y0, len = std::sqrt(ex * ex + ey * ey);
            d = std::min(d, -((x - x0) * ey - (y - y0) * ex) / len);
        }
        return d;
    };
    auto cover = [&](int px, int py, float grow) { // 4x4 samples
        int n = 0;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                n += depth(px + (i + 0.5f) / 4.0f, py + (j + 0.5f) / 4.0f) >= -grow;
        return n / 16.0f;
    };
    std::vector<uint32_t> pixels(static_cast<size_t>(S) * S, 0);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            Premul px;
            Over(px, 0x0C0E10, cover(x, y, 1.3f));
            Over(px, rgb, cover(x, y, 0.0f));
            pixels[static_cast<size_t>(y) * S + x] = Straight(px);
        }
    Art art;
    art.tex = MakeArt(name, pixels, S);
    art.size = art.w = art.h = S;
    return art;
}

// A rim marker centred rr from the map's centre at `at` (radians clockwise from the top), turned by `turn`
// more (0 = pointing in).
void RimMarker(void* viewport, const Art& art, float cx, float cy, float rr, float at, float turn)
{
    if (!art.tex || !Begin(viewport, g_textured, art.tex))
        return;
    const float x = cx + std::sin(at) * rr, y = cy - std::cos(at) * rr;
    const float c = std::cos(at + turn), sn = std::sin(at + turn), h = kHeadingPx * 0.5f;
    auto pt = [&](float dx, float dy, float u, float v) { return V(x + dx * c - dy * sn, y + dx * sn + dy * c, 0xFFFFFFFF, u, v); };
    Vertex v[4] = {pt(-h, -h, 0.0f, 0.0f), pt(h, -h, 1.0f, 0.0f), pt(h, h, 1.0f, 1.0f), pt(-h, h, 0.0f, 1.0f)};
    RenderFan(*RenderInstance, kFvf, v, 4, 0);
    End(viewport, g_textured);
}

void HeadingMarker(void* viewport, float cx, float cy, float r, float heading)
{
    if (!g_headingArt.tex)
        g_headingArt = PaintRimMarker("clarity_minimap_heading", kHeadingRgb);
    RimMarker(viewport, g_headingArt, cx, cy, r + kRingMid, heading, 0.0f); // on the ring
}

void MissionMarker(void* viewport, float cx, float cy, float r, float angle)
{
    if (!g_missionArt.tex)
        g_missionArt = PaintRimMarker("clarity_minimap_mission", kMissionRgb);
    RimMarker(viewport, g_missionArt, cx, cy, r + kRingMid, angle, kPi); // on the ring; turned around: pointing out
}

// The frame: its painting for this size (made again when the size changes), drawn 1:1.
void Frame(void* viewport, float left, float top, int w, int h, float r)
{
    if (!g_frameArt.tex || g_frameArt.w != w || g_frameArt.h != h)
        PaintFrame(w, h, r);
    const float k = 1.0f / g_frameArt.size;
    Blit(viewport, g_frameArt.tex, std::round(left), std::round(top), static_cast<float>(w), static_cast<float>(h), 0.0f,
         w * k, h * k, 0xFFFFFFFF);
}

void __fastcall Render(void* view, void* /*edx*/, void* viewport, float /*alpha*/, unsigned /*flags*/)
{
    if (!viewport || !*RenderInstance)
        return;
    GuiRect bounds{}, screen{};
    ViewGetBounds(view, &bounds);
    ViewToScreen(view, &screen, &bounds, true);
    const float w = screen.right - screen.left + 1.0f;
    const float h = screen.bottom - screen.top + 1.0f;
    if (w < 8.0f || h < 8.0f)
        return;
    if (!g_loggedRender) {
        g_loggedRender = true;
        Log("[minimap] first draw: view at %.0f,%.0f, %.0fx%.0f", screen.left, screen.top, w, h);
    }
    g_opacity = std::clamp(Cfg().minimap.opacity, 0.2f, 1.0f);
    const float cx = screen.left + w * 0.5f, cy = screen.top + h * 0.5f;
    const float r = std::min(w, h) * 0.5f - kFramePx - kMarginPx;
    Disc(viewport, cx, cy, r, 0xFF000000);
    // Where you are and which way you face, as the game's map window: (0, 0, 1) turned by your
    // rotation; on the map (north up) the arrow turns by atan2(x, z).
    const void* n3 = N3Instance();
    game::Vec3 me{};
    float heading = 0.0f;
    if (n3) {
        float pos[3] = {}, rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        CharPosition(n3, pos);
        CharRotation(n3, rot);
        me = game::Vec3{pos[0], pos[1], pos[2]};
        const Vec3f f = Rotate(rot, Vec3f{0.0f, 0.0f, 1.0f});
        if (f.x != 0.0f || f.z != 0.0f)
            heading = std::atan2(f.x, f.z);
    }
    const uint8_t* md = g_mapData ? static_cast<const uint8_t*>(*g_mapData) : nullptr;
    float youX = cx, youY = cy; // where you are on the screen
    float localPpm = 0.0f;
    g_inMappedDungeon = InMappedDungeon(n3, md);
    g_planetWanted = g_inMappedDungeon ? !g_dungeonLocal : Cfg().minimap.layer == 1;
    const bool planet = n3 && g_planetWanted && Planet(viewport, n3, cx, cy, r, &youX, &youY);
    g_planetShown = planet;
    if (md && n3 && !planet) {
        const float mapScale = At<float>(md, kMdMapScale);
        const float ppm = (mapScale > 0.1f && mapScale < 100.0f ? 4.0f / mapScale : 2.0f) * g_zoom;
        g_ppmX = g_ppmZ = ppm;
        localPpm = ppm;
        if (g_panning) {
            g_panMe = me;
        } else if ((g_pan.x != 0.0f || g_pan.z != 0.0f) &&
                   std::fabs(me.x - g_panMe.x) + std::fabs(me.z - g_panMe.z) > kPanMoveM) {
            g_pan = Pan{}; // you moved: centred on you again
        }
        const game::Vec3 centre{me.x + g_pan.x, me.y, me.z + g_pan.z}; // the map's centre
        if (At<void*>(md, kMdGround))
            Ground(viewport, md, centre, cx, cy, r, ppm);
        else
            Rooms(viewport, md, centre, cx, cy, r, ppm);
        Markers(viewport, md, centre, cx, cy, r, ppm);
        youX = cx - g_pan.x * ppm;
        youY = cy + g_pan.z * ppm;
    }
    float wpAngle = 0.0f;
    const bool waypoint = n3 && WaypointMarker(viewport, n3, planet, localPpm, cx, cy, r, &wpAngle);
    if ((youX - cx) * (youX - cx) + (youY - cy) * (youY - cy) <= (r - 3.0f) * (r - 3.0f)) {
        if (planet)
            PlanetYou(viewport, youX, youY, heading);
        else
            PlayerArrow(viewport, youX, youY, heading);
    }
    Frame(viewport, screen.left, screen.top, static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h)), r);
    Compass(viewport, cx, cy, r, 0.0f);
    ZoomButtons(viewport, cx, cy, r);
    if (waypoint)
        MissionMarker(viewport, cx, cy, r, wpAngle);
    if (n3)
        HeadingMarker(viewport, cx, cy, r, heading);
    ResetMaterial(viewport);
}

bool Create(float k)
{
    g_map.window = CrtNew(kWindowAlloc);
    g_map.view = CrtNew(kViewAlloc);
    if (!g_map.window || !g_map.view) {
        Log("[minimap] the window couldn't be made");
        return false;
    }
    memset(g_map.window, 0, kWindowAlloc);
    memset(g_map.view, 0, kViewAlloc);
    const float px = std::clamp(Cfg().minimap.size, 80.0f, 600.0f);
    g_map.gw = (px + 2.0f * (kFramePx + kMarginPx)) * k;
    const TempString empty("");
    const GuiRect frame{0.0f, 0.0f, g_map.gw - 1.0f, g_map.gw - 1.0f};
    WindowCtor(g_map.window, &frame, empty.buf, empty.buf, kWindowStyle, kWindowFlags);
    g_map.windowVtable = *static_cast<void**>(g_map.window);
    ViewCtor(g_map.view, &frame, empty.buf, kViewResize, kViewFlags);

    // The view's vtable, with Render ours.
    void** vt = *static_cast<void***>(g_map.view);
    int slot = -1;
    for (int i = 0; i < kVtableSlots; ++i) {
        g_vtable[i] = vt[i];
        if (vt[i] == ViewRender && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        Log("[minimap] View::Render not in the view's vtable - minimap off");
        return false;
    }
    g_vtable[slot] = reinterpret_cast<void*>(&Render);
    *static_cast<void***>(g_map.view) = g_vtable;

    WindowAddChild(g_map.window, g_map.view, false);
    const GuiPoint size{g_map.gw, g_map.gw};
    WindowSizeLimits(g_map.window, &size, &size);
    WindowResizeTo(g_map.window, g_map.gw, g_map.gw);
    ViewSetFrame(g_map.view, &frame, true);
    Log("[minimap] window made (%.0f px map, %.2f GUI units per px, Render slot %d)", px, k, slot);
    return true;
}

// While the minimap is on it takes the game's local map's place: the GUI module "map_window"
// (GUIModuleActivator_c, GUI 0x1002E537), whose DValue of that name is whether it's open (the toolbar's
// Map button, 0x100726B3, and the key toggle it; the game keeps it per character). Its handler
// (SIG_LOCAL_MAP_TOGGLE) is hooked so it makes no window then: each call (a press of the button or the key)
// shows or hides the minimap instead, kept in [Minimap] Shown, the same for every character ([Minimap]
// Enable stays as it is). When the minimap starts replacing it, a game window open from before is closed
// (and opened again when the minimap stops, if the game still has it open); turning the minimap on in the
// options shows it. Without the hook, the game's window is closed after it opens and the minimap always
// shows. The planet map ("planetmap_window") is left alone.
bool g_loggedGameMap = false;
using MapToggleFn = void(__thiscall*)(void* self, void* module, bool open);
MapToggleFn g_origMapToggle = nullptr;
void* g_toggleSelf = nullptr;   // the handler's this and module, from its last call
void* g_toggleModule = nullptr;
bool g_replacing = false;       // the minimap replaces the game's local map (as of the last tick)
bool g_enableSeen = false;      // [Minimap] Enable as of the last tick
bool g_shown = true;            // the minimap is shown (the map button / key)
int g_shownSeen = -1;           // [Minimap] Shown as last taken (-1: not yet)

void SetShown(bool shown)
{
    g_shown = shown;
    WritePrivateProfileStringW(L"Minimap", L"Shown", shown ? L"1" : L"0", ConfigPath().c_str());
    g_shownSeen = Cfg().minimap.shown ? 1 : 0; // as a drag: the old value isn't a change to take
}

bool Replacing()
{
    return Cfg().minimap.enable && !g_disabled && InGame();
}

void __fastcall Hook_MapToggle(void* self, void* /*edx*/, void* module, bool open)
{
    g_toggleSelf = self;
    g_toggleModule = module;
    if (Replacing()) {
        SetShown(!g_shown); // the button / key shows or hides the minimap
        return;
    }
    g_origMapToggle(self, module, open);
}

bool GameMapOpen()
{
    alignas(8) uint8_t name[0x40] = {}, value[0x40] = {};
    StringCtor(name, "map_window");
    GetDValue(value, name, false);
    const bool open = VariantAsBool(value);
    VariantDtor(value);
    StringDtor(name);
    return open;
}

void SetGameMapOpen(bool open)
{
    alignas(8) uint8_t name[0x40] = {}, value[0x40] = {};
    StringCtor(name, "map_window");
    VariantBool(value, open);
    SetDValue(name, value);
    VariantDtor(value);
    StringDtor(name);
}

// Each tick: whether the minimap is to show.
bool MinimapWanted()
{
    const MinimapConfig& cfg = Cfg().minimap;
    if ((cfg.shown ? 1 : 0) != g_shownSeen) { // Clarity.ini (re)read
        g_shownSeen = cfg.shown ? 1 : 0;
        g_shown = cfg.shown;
    }
    const bool replacing = Replacing();
    const bool turnedOn = cfg.enable && !g_enableSeen && InGame(); // ticked in the options, in game
    g_enableSeen = cfg.enable;
    if (!g_origMapToggle) { // no hook: the game's window closed after it opens
        if (replacing && GameMapOpen()) {
            SetGameMapOpen(false);
            if (!g_loggedGameMap) {
                g_loggedGameMap = true;
                Log("[minimap] the game's local map window closed (the minimap replaces it)");
            }
        }
        return replacing;
    }
    if (replacing != g_replacing) {
        g_replacing = replacing;
        if (g_toggleSelf && GameMapOpen()) // the game's window out of the way, or back
            g_origMapToggle(g_toggleSelf, g_toggleModule, !replacing);
        if (turnedOn && !g_shown)
            SetShown(true);
    }
    return replacing && g_shown;
}

// Window coordinates (GUI units) per client pixel, and the screen size in GUI units.
float GuiPerPx(HWND hwnd, GuiPoint* screen)
{
    ScreenSize(screen);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    return cw > 0 && screen->x > 0.0f ? screen->x / cw : 1.0f;
}

void Update(HWND hwnd)
{
    void* wc = WindowController();
    if (wc != g_wc) {
        Forget();
        g_hiddenCompass = nullptr;
        g_wc = wc;
    }
    if (!wc)
        return;
    SyncGameCompass(wc, Replacing());
    if (g_map.window && (!InWindowList(wc, g_map.window) || *static_cast<void**>(g_map.window) != g_map.windowVtable)) {
        Log("[minimap] the window was removed by the game - making it again");
        Forget();
    }
    const MinimapConfig& cfg = Cfg().minimap;
    const bool on = MinimapWanted();
    if (!on) {
        if (g_map.window && WindowIsVisible(g_map.window))
            WindowShow(g_map.window, false);
        return;
    }
    if (!g_flat) {
        g_flat = MakeBlob(Blob::Flat);
        g_textured = MakeBlob(Blob::Textured);
        g_rooms = MakeBlob(Blob::Rooms);
        g_multiply = MakeBlob(Blob::Multiply);
        g_compass = MakeCompass();
        if (!g_flat || !g_textured || !g_rooms || !g_multiply) {
            Log("[minimap] render states couldn't be made - minimap off");
            g_disabled = true;
            return;
        }
    }
    GuiPoint screen{};
    const float k = GuiPerPx(hwnd, &screen);
    if (g_map.window && cfg.size != g_map.sizeSeen) { // a new size: a new window
        WindowShow(g_map.window, false);
        Forget();
        g_wc = wc;
    }
    if (!g_map.window) {
        if (!Create(k)) {
            g_disabled = true;
            return;
        }
        g_map.sizeSeen = cfg.size;
    }
    if (cfg.x != g_map.xSeen || cfg.y != g_map.ySeen) {
        g_map.xSeen = cfg.x;
        g_map.ySeen = cfg.y;
        // Default: the top-right corner.
        g_map.x = cfg.x >= 0.0f ? cfg.x : screen.x - g_map.gw - 16.0f * k;
        g_map.y = cfg.y >= 0.0f ? cfg.y : 16.0f * k;
        g_map.x = std::clamp(g_map.x, 0.0f, std::max(0.0f, screen.x - g_map.gw));
        g_map.y = std::clamp(g_map.y, 0.0f, std::max(0.0f, screen.y - g_map.gw));
        WindowMoveTo(g_map.window, g_map.x, g_map.y);
    }
    if (!WindowIsVisible(g_map.window))
        WindowShow(g_map.window, true);
    if (const void* pf = game::CurrentPlayfield(); pf != g_panPlayfield) { // a new playfield: centred on you
        g_panPlayfield = pf;
        g_pan = Pan{};
    }
    if (!g_atlasesMade)
        MakeAtlases();
    if (cfg.planetLevel != g_planetLevelSeen) {
        g_planetLevelSeen = cfg.planetLevel;
        g_planetLevel = cfg.planetLevel;
    }
    if (cfg.zoom != g_zoomSeen) {
        g_zoomSeen = cfg.zoom;
        g_zoom = std::clamp(cfg.zoom, kZoomMin, kZoomMax);
    }
    const DWORD now = GetTickCount();
    if (now - g_markersAt >= kMarkersEveryMs) {
        g_markersAt = now;
        if (const void* n3 = N3Instance())
            RefreshMarkers(n3);
    }
}

// --- dragging -------------------------------------------------------------------------------

constexpr UINT_PTR kTimerId = 0xC1A8;
constexpr UINT kTickMs = 100;
bool g_timerSet = false;
bool g_panMoved = false;              // ... far enough to be a drag, not a click
POINT g_panMouse{};
Pan g_panStart{};
constexpr int kPanSlopPx = 3;
bool g_dragging = false;
POINT g_dragMouse{};
float g_dragX = 0.0f, g_dragY = 0.0f;

// Over the minimap: the GUI's window under the mouse is ours, and the point is inside its round frame.
bool Over(HWND hwnd, LPARAM lp)
{
    if (!g_map.window || !g_wc || !WindowIsVisible(g_map.window) || MouseWindow(g_wc) != g_map.window)
        return false;
    GuiPoint screen{};
    const float k = GuiPerPx(hwnd, &screen);
    const float gx = static_cast<short>(LOWORD(lp)) * k, gy = static_cast<short>(HIWORD(lp)) * k;
    const float r = g_map.gw * 0.5f;
    const float dx = gx - (g_map.x + r), dy = gy - (g_map.y + r);
    return dx * dx + dy * dy <= r * r;
}

// Where a client-area point is, in GUI units relative to the minimap's centre; the map's radius too.
bool Relative(HWND hwnd, int px, int py, float* dx, float* dy, float* mapR)
{
    if (!g_map.window || !g_wc || !WindowIsVisible(g_map.window))
        return false;
    GuiPoint screen{};
    const float k = GuiPerPx(hwnd, &screen);
    const float half = g_map.gw * 0.5f;
    *dx = px * k - (g_map.x + half);
    *dy = py * k - (g_map.y + half);
    *mapR = half - kFramePx - kMarginPx;
    return true;
}

// The button under a client-area point: +1 zoom in, -1 out, 2 the layer, 0 none.
int ButtonAt(HWND hwnd, LPARAM lp)
{
    float dx, dy, r;
    if (!Over(hwnd, lp) || !Relative(hwnd, static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)), &dx, &dy, &r))
        return 0;
    const float band = r + kRingMid;
    const float hit = (kButtonRadius + 2.0f) * (kButtonRadius + 2.0f);
    auto onButton = [&](float a) {
        const float bx = std::sin(a) * band, by = -std::cos(a) * band;
        return (dx - bx) * (dx - bx) + (dy - by) * (dy - by) <= hit;
    };
    return onButton(kZoomInAngle) ? 1 : onButton(kZoomOutAngle) ? -1 : onButton(kLayerAngle) ? 2 : 0;
}

// One zoom step (25 percentage points) in or out, on the 25% grid; saved to [Minimap] Zoom.
void Zoom(int steps)
{
    const float z = std::clamp(std::round(g_zoom / kZoomStep + steps) * kZoomStep, kZoomMin, kZoomMax);
    if (std::fabs(z - g_zoom) < 1e-4f)
        return;
    g_zoom = z;
    wchar_t v[32];
    swprintf_s(v, L"%.3g", z);
    WritePrivateProfileStringW(L"Minimap", L"Zoom", v, ConfigPath().c_str());
    // As a drag: until Clarity.ini is read again, its old value isn't a change to take.
    g_zoomSeen = Cfg().minimap.zoom;
}

// One planet level up (+1, more detail) or down (-1); saved to [Minimap] PlanetLevel.
void PlanetZoom(int step)
{
    const int n = static_cast<int>(g_maps[g_mapShown].levels.size());
    const int level = std::clamp(PlanetLevelShown() + step, 0, std::max(0, n - 1));
    if (level == PlanetLevelShown())
        return;
    g_planetLevel = level;
    wchar_t v[16];
    swprintf_s(v, L"%d", level);
    WritePrivateProfileStringW(L"Minimap", L"PlanetLevel", v, ConfigPath().c_str());
    g_planetLevelSeen = Cfg().minimap.planetLevel; // as a drag: the old value isn't a change to take
}

bool g_swallowUp = false; // a button was pressed: its button-up isn't the game's

void EndPan()
{
    if (!g_panning)
        return;
    g_panning = false;
    if (GetCapture())
        ReleaseCapture();
}

void EndDrag()
{
    if (!g_dragging)
        return;
    g_dragging = false;
    if (GetCapture())
        ReleaseCapture();
    if (!g_map.window)
        return;
    g_map.x = std::round(g_map.x);
    g_map.y = std::round(g_map.y);
    WindowMoveTo(g_map.window, g_map.x, g_map.y);
    wchar_t v[32];
    swprintf_s(v, L"%.0f", g_map.x);
    WritePrivateProfileStringW(L"Minimap", L"X", v, ConfigPath().c_str());
    swprintf_s(v, L"%.0f", g_map.y);
    WritePrivateProfileStringW(L"Minimap", L"Y", v, ConfigPath().c_str());
    // Until Clarity.ini is read again the settings hold the old place: not a change to take (the new
    // one, when it arrives, is where the minimap already is).
    g_map.xSeen = Cfg().minimap.x;
    g_map.ySeen = Cfg().minimap.y;
}

bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result)
{
    if (!g_timerSet) // the first message: this is the game thread
        g_timerSet = SetTimer(hwnd, kTimerId, kTickMs, nullptr) != 0;
    switch (msg) {
    case WM_TIMER:
        if (wp != kTimerId)
            return false;
        Update(hwnd);
        *result = 0;
        return true;
    case WM_LBUTTONDOWN:
        if (g_dragging || (wp & (MK_RBUTTON | MK_SHIFT | MK_CONTROL)))
            return false;
        if (const int b = ButtonAt(hwnd, lp)) {
            if (b == 2 && g_inMappedDungeon) // the other layer, for this dungeon only
                g_dungeonLocal = !g_dungeonLocal;
            else if (b == 2) // the other layer, saved to [Minimap] Layer (read back at once: Clarity.ini is reloaded)
                WritePrivateProfileStringW(L"Minimap", L"Layer", Cfg().minimap.layer == 1 ? L"0" : L"1", ConfigPath().c_str());
            else if (g_planetShown)
                PlanetZoom(b);
            else
                Zoom(b);
            g_swallowUp = true;
            *result = 0;
            return true;
        }
        if (!Over(hwnd, lp))
            return false;
        if (Cfg().minimap.locked) { // locked: dragging moves the map, a click centres it on you
            g_panning = true;
            g_panMoved = false;
            g_panMouse = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            g_panStart = g_pan;
            SetCapture(hwnd);
            *result = 0;
            return true;
        }
        g_dragging = true;
        g_dragMouse = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        g_dragX = g_map.x;
        g_dragY = g_map.y;
        SetCapture(hwnd);
        *result = 0;
        return true;
    case WM_MOUSEMOVE:
        if (g_panning) {
            const int mx = static_cast<short>(LOWORD(lp)), my = static_cast<short>(HIWORD(lp));
            if (!g_panMoved && std::abs(mx - g_panMouse.x) <= kPanSlopPx && std::abs(my - g_panMouse.y) <= kPanSlopPx) {
                *result = 0;
                return true;
            }
            g_panMoved = true;
            GuiPoint screen{};
            const float k = GuiPerPx(hwnd, &screen);
            if (g_ppmX > 0.0f && g_ppmZ > 0.0f) {
                g_pan.x = g_panStart.x - (mx - g_panMouse.x) * k / g_ppmX;
                g_pan.z = g_panStart.z + (my - g_panMouse.y) * k / g_ppmZ;
            }
            *result = 0;
            return true;
        }
        if (!g_dragging)
            return false;
        if (g_map.window) {
            GuiPoint screen{};
            const float k = GuiPerPx(hwnd, &screen);
            g_map.x = std::clamp(g_dragX + (static_cast<short>(LOWORD(lp)) - g_dragMouse.x) * k, 0.0f,
                                 std::max(0.0f, screen.x - g_map.gw));
            g_map.y = std::clamp(g_dragY + (static_cast<short>(HIWORD(lp)) - g_dragMouse.y) * k, 0.0f,
                                 std::max(0.0f, screen.y - g_map.gw));
            WindowMoveTo(g_map.window, g_map.x, g_map.y);
        }
        *result = 0;
        return true;
    case WM_LBUTTONUP:
        if (g_swallowUp) {
            g_swallowUp = false;
            *result = 0;
            return true;
        }
        if (g_panning) {
            if (!g_panMoved)
                g_pan = Pan{};
            EndPan();
            *result = 0;
            return true;
        }
        if (!g_dragging)
            return false;
        EndDrag();
        *result = 0;
        return true;
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
        EndDrag();
        EndPan();
        return false;
    default:
        return false;
    }
}

} // namespace

bool Install()
{
    if (!ResolveAll()) {
        Log("[minimap] disabled");
        return false;
    }
    if (uint8_t* get = FindUnique(SIG_MAPDATA_GET, "MapData_c getter"))
        g_mapData = *reinterpret_cast<void***>(get + 2);
    else
        Log("[minimap] MapData_c not found - no map, only the frame");
    if (uint8_t* toggle = FindUnique(SIG_LOCAL_MAP_TOGGLE, "local map window handler"))
        InstallHook("GUIModuleActivator_c map_window handler", toggle, &Hook_MapToggle, &g_origMapToggle);
    else
        Log("[minimap] the game's local map handler not found - its window is closed after it opens");
    wndproc::Add(&OnMessage);
    Log("[minimap] ready (MapData_c at %p)", static_cast<void*>(g_mapData));
    return true;
}

} // namespace prk::minimap
