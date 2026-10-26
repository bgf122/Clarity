// Health / nano and target panels. Both are plain windows of the game's GUI (GUI.dll exports, nothing
// hooked): Window -> View -> ViewSurface_c, the textured quad the GUI draws everything with. The
// surface is made from a SpriteInfo_t (DisplaySystem.dll) - a CPU image Clarity paints and uploads with
// ViewSurface_c::UpdateTexture, the way the GUI's TextRenderer_c (0x1016095B) does it for text.
//
// The game's own bars are CharBarWindow_c windows (GUI 0x1006D7C7), made by ControlCenterModule_c:
// "CCHealthBarConfig" / "CCNanoBarConfig" (SlotPlayerCharacterAlive) and "CCFriendlyHealthBarConfig" /
// "CCHostileHealthBarConfig" (CreateTargetMenus). They are found in WindowController_c's window list
// by their class (run-time type info) and config name, hidden while the panels are on, and shown again
// (when their own show condition, a CriteriaMonitor_c, says so) when the panels are turned off.
//
// Everything runs on the game thread, from a window timer in the game window's procedure.
#include "bars.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/log.h"
#include "../../core/n3.h"
#include "../../core/text_raster.h"
#include "../../core/wndproc.h"
#include "../nameplates/nameplates.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace prk::bars {

namespace {

// --- the game's GUI -----------------------------------------------------------------------

// Rect and Point (Utils.dll) are plain floats; a Rect's right and bottom are inclusive (a w x h area
// is (0, 0, w - 1, h - 1)) and its destructor does nothing. Returned by value through a hidden pointer.
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
using ScreenSizeFn = GuiPoint*(__cdecl*)(GuiPoint* ret);
using MouseWindowFn = void*(__thiscall*)(void* self);
using ViewCtorFn = void*(__thiscall*)(void* self, const GuiRect* frame, const void* name, unsigned resizeMask, unsigned flags);
using ViewSetFrameFn = void(__thiscall*)(void* self, const GuiRect* frame, bool);
using ViewSetResizeMaskFn = void(__thiscall*)(void* self, unsigned mask);
using ViewAddSurfaceFn = void(__thiscall*)(void* self, void* surface, bool);
using SurfaceCtorFn = void*(__thiscall*)(void* self, void* sprite, unsigned flags);
using SurfaceUpdateFn = void(__thiscall*)(void* self, void* sprite, const GuiRect* from, const GuiPoint* to);
using SurfaceAlphaFn = void(__thiscall*)(void* self, float alpha);
using SurfaceFrameFn = void(__thiscall*)(void* self, const GuiRect* frame);
using SpriteCtorFn = void*(__thiscall*)(void* self, int w, int h, int format);
using SpriteDataFn = uint8_t*(__thiscall*)(void* self);
using SpriteIntFn = int(__thiscall*)(const void* self);
using EvaluateFn = bool(__thiscall*)(void* self);
using WindowSizeLimitsFn = void(__thiscall*)(void* self, const GuiPoint* min, const GuiPoint* max);
using GuiTextureFn = void*(__thiscall*)(void* self, int id, int format);
using ReleaseTextureFn = void(__thiscall*)(void* self, void* sprite);

CrtNewFn CrtNew;
CrtDeleteFn CrtDelete;
StringCtorFn StringCtor;
StringDtorFn StringDtor;
InstanceFn WindowController;  // WindowController_c::GetInstanceIfAny
InstanceFn ControlCenter;     // ControlCenterModule_c::GetInstanceIfAny
WindowCtorFn WindowCtor;
WindowShowFn WindowShow;
WindowIsVisibleFn WindowIsVisible;
WindowAddChildFn WindowAddChild;
WindowMoveToFn WindowMoveTo;
WindowResizeToFn WindowResizeTo;
ScreenSizeFn ScreenSize;
MouseWindowFn MouseWindow;    // WindowController_c::GetMouseWindow
ViewCtorFn ViewCtor;
ViewSetFrameFn ViewSetFrame;
ViewSetResizeMaskFn ViewSetResizeMask;
ViewAddSurfaceFn ViewAddSurface;
SurfaceCtorFn SurfaceCtor;
SurfaceUpdateFn SurfaceUpdate; // (a part of the sprite: only the texture tiles under it are sent)
SurfaceAlphaFn SurfaceAlpha;
SurfaceFrameFn SurfaceSrc, SurfaceDst;
SpriteCtorFn SpriteCtor;
SpriteDataFn SpriteData;
SpriteIntFn SpriteStride, SpriteBpp, SpriteSurfaceFormat;
EvaluateFn CriteriaEvaluate;  // CriteriaMonitor_c::Evaluate
WindowSizeLimitsFn WindowSizeLimits;
InstanceFn GuiResources;      // GuiResourceManager_t::GetInstance
GuiTextureFn GuiTexture;      // GuiResourceManager_t::GetGuiTexture(id, Format_e): a skin image as pixels
ReleaseTextureFn ReleaseTexture;
SpriteIntFn SpriteWidth, SpriteHeight;

// Allocation sizes: Window is 0x74 bytes (CharBarWindow_c's own fields start there), View 0x128
// (CharacterBar_c's), ViewSurface_c 0x68 and SpriteInfo_t 0x44 (as TextRenderer_c allocates them).
constexpr size_t kWindowAlloc = 0x200;
constexpr size_t kViewAlloc = 0x200;
constexpr size_t kSurfaceAlloc = 0x68;
constexpr size_t kSpriteAlloc = 0x44;
constexpr int kWindowStyle = 3;        // as CharBarWindow_c
constexpr unsigned kWindowFlags = 0xE3C;
constexpr unsigned kViewResize = 0xF;  // as CharacterBar_c
constexpr unsigned kViewFlags = 2;
// SpriteInfo_t's Format_e: 1 the display's, 2 the GUI's (D3DX A1R5G5B5: one bit of alpha, no smooth
// edges). Any other value keeps the D3DX surface format already at +0x38, so the sprite is made with
// A8R8G8B8 there (as Clarity's other textures) and Format_e 3.
constexpr int kSpriteFormatGui = 2;
constexpr int kSpriteFormatAsIs = 3;
constexpr int kSpriteD3dxFormat = 0x38;
// D3DX7 surface formats.
constexpr int kSfA8R8G8B8 = 2, kSfR5G6B5 = 4, kSfA1R5G5B5 = 8, kSfA4R4G4B4 = 10;
constexpr int kWcWindowList = 0x88;    // WindowController_c: std::list<Window*> (AddWindow / RemoveWindow)
constexpr int kCcAlive = 0xB8;         // ControlCenterModule_c: the GUI was made (SlotPlayerCharacterAlive)
constexpr int kCharBarConfig = 0x74;   // CharBarWindow_c: config name (std::string)
constexpr int kCharBarCriteria = 0x90; // CharBarWindow_c: CriteriaMonitor_c (its show condition)
constexpr char kCharBarClass[] = ".?AVCharBarWindow_c@@";

const uint8_t* g_guiBase = nullptr;
size_t g_guiSize = 0;

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[bars] missing export %s!%s", module, name);
    return fn != nullptr;
}

bool ResolveAll()
{
    const char* gui = "GUI.dll";
    const char* ds = "DisplaySystem.dll";
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
    ok &= Resolve(ViewSetResizeMask, gui, "?SetResizeMask@View@@QAEXI@Z");
    ok &= Resolve(ViewAddSurface, gui, "?AddRenderSurface@View@@QAEXPAVViewSurface_c@@_N@Z");
    ok &= Resolve(SurfaceCtor, gui, "??0ViewSurface_c@@QAE@PAVSpriteInfo_t@@I@Z");
    ok &= Resolve(SurfaceUpdate, gui, "?UpdateTexture@ViewSurface_c@@QAEXPAVSpriteInfo_t@@ABVRect@@ABVPoint@@@Z");
    ok &= Resolve(SurfaceAlpha, gui, "?SetAlpha@ViewSurface_c@@QAEXM@Z");
    ok &= Resolve(SurfaceSrc, gui, "?SetSrcFrame@ViewSurface_c@@QAEXABVRect@@@Z");
    ok &= Resolve(SurfaceDst, gui, "?SetDstFrame@ViewSurface_c@@QAEXABVRect@@@Z");
    ok &= Resolve(SpriteCtor, ds, "??0SpriteInfo_t@@QAE@HHW4Format_e@@@Z");
    ok &= Resolve(SpriteData, ds, "?GetData@SpriteInfo_t@@QAEPAEXZ");
    ok &= Resolve(SpriteStride, ds, "?GetBytesPerLine@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteBpp, ds, "?GetBytesPerPixel@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteSurfaceFormat, ds, "?GetSurfaceFormat@SpriteInfo_t@@QBE?AW4_D3DX_SURFACEFORMAT@@XZ");
    ok &= Resolve(SpriteWidth, ds, "?GetWidth@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteHeight, ds, "?GetHeight@SpriteInfo_t@@QBEHXZ");
    // The skin's own bar images, for the colours (optional: fixed colours without them).
    Resolve(GuiResources, "Interfaces.dll", "?GetInstance@GuiResourceManager_t@@SAPAV1@XZ");
    Resolve(GuiTexture, "Interfaces.dll", "?GetGuiTexture@GuiResourceManager_t@@QAEPAVSpriteInfo_t@@HW4Format_e@@@Z");
    Resolve(ReleaseTexture, "Interfaces.dll", "?ReleaseTexture@GuiResourceManager_t@@QAEXPAVSpriteInfo_t@@@Z");
    // In GUI.dll or Utils.dll depending on the build.
    CriteriaEvaluate = nullptr;
    for (const char* m : {gui, "Utils.dll"}) {
        if (HMODULE h = GetModuleHandleA(m))
            CriteriaEvaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(h, "?Evaluate@CriteriaMonitor_c@@QAE_NXZ"));
        if (CriteriaEvaluate)
            break;
    }
    if (!CriteriaEvaluate)
        Log("[bars] CriteriaMonitor_c::Evaluate not found - the game's bars come back only after a relog");
    if (HMODULE h = GetModuleHandleA(gui)) {
        g_guiBase = reinterpret_cast<const uint8_t*>(h);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_guiBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(h)->e_lfanew);
        g_guiSize = nt->OptionalHeader.SizeOfImage;
    }
    return ok && g_guiBase;
}

// A Utils String for the duration of a call.
struct TempString {
    alignas(8) uint8_t buf[0x20] = {};
    explicit TempString(const char* s) { StringCtor(buf, s); }
    ~TempString() { StringDtor(buf); }
    TempString(const TempString&) = delete;
    TempString& operator=(const TempString&) = delete;
};

bool InGui(const void* p, size_t len = 4)
{
    const auto* b = static_cast<const uint8_t*>(p);
    return b >= g_guiBase && b + len <= g_guiBase + g_guiSize;
}

// The object's class name from MSVC run-time type info: vtable[-1] is the complete object locator,
// whose type descriptor (+0x0C) has the decorated name at +8. Every pointer is checked to lie in GUI.dll.
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

// WindowController_c's window list: std::list nodes {next, prev, Window*}, from a head node.
struct ListNode {
    ListNode* next;
    ListNode* prev;
    void* value;
};

template <typename F>
void ForEachWindow(void* wc, F&& f)
{
    auto* head = *reinterpret_cast<ListNode**>(static_cast<uint8_t*>(wc) + kWcWindowList);
    int guard = 0;
    for (ListNode* n = head ? head->next : nullptr; n && n != head && guard < 4096; n = n->next, ++guard)
        f(n->value);
}

bool InWindowList(void* wc, void* window)
{
    bool found = false;
    ForEachWindow(wc, [&](void* w) { found = found || w == window; });
    return found;
}

// A CharBarWindow_c's config name (VS2010 std::string: 16-byte buffer or pointer, size, capacity).
std::string ConfigName(void* window)
{
    const auto* s = static_cast<const uint8_t*>(window) + kCharBarConfig;
    const uint32_t size = *reinterpret_cast<const uint32_t*>(s + 0x10);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(s + 0x14);
    if (size > 64 || cap < size)
        return {};
    const char* chars = cap >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    return chars ? std::string(chars, size) : std::string();
}

// Window coordinates (GUI units) per client pixel.
float GuiPerPx(HWND hwnd, GuiPoint* screen)
{
    ScreenSize(screen);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    return cw > 0 && screen->x > 0.0f ? screen->x / cw : 1.0f;
}

// --- painting -------------------------------------------------------------------------------

float g_scale = 0.0f; // the scale the fonts are made for
int S(float v)
{
    return static_cast<int>(std::lround(v * g_scale));
}

// Premultiplied float RGBA.
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;

    void Reset(int width, int height)
    {
        w = width;
        h = height;
        px.assign(static_cast<size_t>(w) * h * 4, 0.0f);
    }

    void Blend(int x, int y, uint32_t rgb, float a)
    {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f)
            return;
        a = std::min(a, 1.0f);
        float* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        p[0] = ((rgb >> 16) & 0xFF) / 255.0f * a + p[0] * (1.0f - a);
        p[1] = ((rgb >> 8) & 0xFF) / 255.0f * a + p[1] * (1.0f - a);
        p[2] = (rgb & 0xFF) / 255.0f * a + p[2] * (1.0f - a);
        p[3] = a + p[3] * (1.0f - a);
    }

    // An antialiased rounded rectangle from (x0, y0) to (x1, y1) (exclusive, fractional), its colour
    // going from rgbTop to rgbBottom, cut off left of cx0 and right of cx1 (the filled part of a bar).
    void RoundRect(float x0, float y0, float x1, float y1, float radius, uint32_t rgbTop, uint32_t rgbBottom, float alpha,
                   float cx0 = -1e9f, float cx1 = 1e9f, const std::vector<uint32_t>* profile = nullptr)
    {
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
        if (hx <= 0.0f || hy <= 0.0f || cx1 <= cx0)
            return;
        const float mx = x0 + hx, my = y0 + hy;
        const float r = std::min(radius, std::min(hx, hy));
        for (int y = static_cast<int>(std::floor(y0)); y < static_cast<int>(std::ceil(y1)); ++y) {
            const float cy = y + 0.5f;
            const float t = std::clamp((cy - y0) / (y1 - y0), 0.0f, 1.0f);
            const uint32_t rgb = profile && !profile->empty()
                                     ? ProfileAt(*profile, (y - y0) / (y1 - y0), (y + 1 - y0) / (y1 - y0))
                                     : Mix(rgbTop, rgbBottom, t);
            for (int x = static_cast<int>(std::floor(std::max(x0, cx0))); x < static_cast<int>(std::ceil(std::min(x1, cx1))); ++x) {
                const float cx = x + 0.5f;
                const float qx = std::fabs(cx - mx) - (hx - r), qy = std::fabs(cy - my) - (hy - r);
                const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
                const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                const float horiz = std::clamp(std::min(x + 1.0f, cx1) - std::max(static_cast<float>(x), cx0), 0.0f, 1.0f);
                Blend(x, y, rgb, alpha * cover * horiz);
            }
        }
    }

    // Another canvas over this one, its top-left at (x0, y0).
    void Paste(const Canvas& o, int x0, int y0)
    {
        for (int y = 0; y < o.h; ++y) {
            for (int x = 0; x < o.w; ++x) {
                const int tx = x0 + x, ty = y0 + y;
                const float* s = &o.px[(static_cast<size_t>(y) * o.w + x) * 4];
                if (s[3] <= 0.0f || tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
                float* d = &px[(static_cast<size_t>(ty) * w + tx) * 4];
                for (int i = 0; i < 4; ++i)
                    d[i] = s[i] + d[i] * (1.0f - s[3]);
            }
        }
    }

    void Mask(int x0, int y0, const raster::Mask& m, uint32_t rgb, float alpha)
    {
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x)
                Blend(x0 + x, y0 + y, rgb, m.a[static_cast<size_t>(y) * m.w + x] * alpha);
    }

    // The average colour of the profile between fractions t0 and t1 of its length.
    static uint32_t ProfileAt(const std::vector<uint32_t>& profile, float t0, float t1)
    {
        const float n = static_cast<float>(profile.size());
        const float a = std::clamp(t0, 0.0f, 1.0f) * n, b = std::max(std::clamp(t1, 0.0f, 1.0f) * n, a + 1e-3f);
        float r = 0, g = 0, bl = 0, wsum = 0;
        for (int i = static_cast<int>(a); i < static_cast<int>(std::ceil(b)) && i < static_cast<int>(n); ++i) {
            const float w = std::min(b, i + 1.0f) - std::max(a, static_cast<float>(i));
            if (w <= 0.0f)
                continue;
            const uint32_t c = profile[static_cast<size_t>(i)];
            r += ((c >> 16) & 0xFF) * w;
            g += ((c >> 8) & 0xFF) * w;
            bl += (c & 0xFF) * w;
            wsum += w;
        }
        if (wsum <= 0.0f)
            return profile.back();
        return (static_cast<uint32_t>(r / wsum + 0.5f) << 16) | (static_cast<uint32_t>(g / wsum + 0.5f) << 8) |
               static_cast<uint32_t>(bl / wsum + 0.5f);
    }

    static uint32_t Mix(uint32_t a, uint32_t b, float t)
    {
        auto ch = [&](int shift) {
            const float va = static_cast<float>((a >> shift) & 0xFF), vb = static_cast<float>((b >> shift) & 0xFF);
            return static_cast<uint32_t>(std::lround(va + (vb - va) * t)) << shift;
        };
        return ch(16) | ch(8) | ch(0);
    }
};

uint32_t Darker(uint32_t rgb, float f)
{
    return Canvas::Mix(0x000000, rgb, f);
}

enum FontId { kFontValue, kFontName, kFontCount };
raster::Font g_fonts[kFontCount];
bool g_fontsOk = false;

struct TextMasks {
    raster::Mask fill, outline;
    float width = 0.0f;
};
std::unordered_map<std::wstring, TextMasks> g_text; // key: font id, then the text

constexpr int kTextPad = 2;

bool EnsureFonts(float scale)
{
    if (scale == g_scale)
        return g_fontsOk;
    for (raster::Font& f : g_fonts)
        f.Destroy();
    g_text.clear();
    g_scale = scale;
    g_fontsOk = g_fonts[kFontValue].Create(L"Segoe UI", S(15), 700) && g_fonts[kFontName].Create(L"Segoe UI", S(15), 600);
    if (!g_fontsOk)
        Log("[bars] font creation failed - panels off");
    return g_fontsOk;
}

const TextMasks& Text(FontId font, const std::wstring& s)
{
    std::wstring key(1, static_cast<wchar_t>(L'0' + font));
    key += s;
    auto it = g_text.find(key);
    if (it != g_text.end())
        return it->second;
    if (g_text.size() > 256)
        g_text.clear();
    const raster::Font& f = g_fonts[font];
    TextMasks t;
    t.width = f.TextWidth(s.c_str(), static_cast<int>(s.size()));
    const int w = static_cast<int>(std::ceil(t.width)) + 2 * kTextPad;
    const int h = static_cast<int>(std::ceil(f.Height())) + 2 * kTextPad;
    t.fill = raster::Rasterize(f, s.c_str(), static_cast<int>(s.size()), kTextPad, kTextPad, w, h);
    t.outline = raster::Dilate(t.fill, 1);
    return g_text.emplace(std::move(key), std::move(t)).first->second;
}

// Outlined text with its top-left at (x, y).
void DrawText(Canvas& c, FontId font, const std::wstring& s, float x, float y, uint32_t rgb)
{
    if (s.empty())
        return;
    const TextMasks& t = Text(font, s);
    const int x0 = static_cast<int>(std::lround(x)) - kTextPad, y0 = static_cast<int>(std::lround(y)) - kTextPad;
    c.Mask(x0, y0, t.outline, 0x000000, 0.85f);
    c.Mask(x0, y0, t.fill, rgb, 1.0f);
}

float TextWidth(FontId font, const std::wstring& s)
{
    return s.empty() ? 0.0f : Text(font, s).width;
}

// Top of a line of text whose capitals (and digits) are centred on y.
float CentredTop(FontId font, float y)
{
    const float ascent = g_fonts[font].Ascent();
    return y - ascent * (1.0f - 0.36f); // capitals are ~0.72 of the ascent
}

std::wstring Fit(FontId font, std::wstring s, float maxW)
{
    if (TextWidth(font, s) <= maxW)
        return s;
    while (s.size() > 1) {
        s.pop_back();
        const std::wstring t = s + L"\x2026";
        if (g_fonts[font].TextWidth(t.c_str(), static_cast<int>(t.size())) <= maxW)
            return t;
    }
    return s;
}

// A name without the game's colour codes (byte 0x10 and a code) or other control bytes.
std::wstring PlainName(const game::Identity& id)
{
    const std::string raw = n3::Name(id);
    std::string clean;
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw[i]);
        if (ch == 0x10)
            ++i;
        else if (ch >= 0x20)
            clean += raw[i];
    }
    if (clean.empty())
        return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, clean.c_str(), static_cast<int>(clean.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, clean.c_str(), static_cast<int>(clean.size()), w.data(), n);
    return w;
}

// --- bars -----------------------------------------------------------------------------------

// Value in tenths of a percent (0..1000), -1 if unknown.
int Tenths(int value, int max)
{
    if (value == n3::kNoValue || max == n3::kNoValue || max <= 0)
        return -1;
    return std::clamp(static_cast<int>(1000.0 * std::min(value, max) / max), 0, 1000);
}

// Health just lost: a light segment from the new value to the old one that holds a moment and then
// shrinks away. Further losses meanwhile extend it from where it is.
struct Trail {
    static constexpr DWORD kHoldMs = 400;
    static constexpr float kShrinkMs = 500.0f;
    uint64_t key = 0;
    int last = -1;
    float from = -1.0f;
    DWORD start = 0;

    float Current(DWORD now, int value) const
    {
        if (from < 0.0f)
            return -1.0f;
        const DWORD t = now - start;
        if (t < kHoldMs)
            return from;
        const float f = (t - kHoldMs) / kShrinkMs;
        const float s = from + (value - from) * f;
        return f < 1.0f && s > value ? s : -1.0f;
    }

    // The trail's end (tenths, above value) or -1.
    float Update(uint64_t k, int value, DWORD now, bool on)
    {
        if (!on || k != key || value < 0) {
            key = k;
            last = value;
            from = -1.0f;
            return -1.0f;
        }
        if (last >= 0 && value < last) {
            from = std::max(Current(now, value), static_cast<float>(last));
            start = now;
        }
        last = value;
        const float cur = Current(now, value);
        if (cur < 0.0f)
            from = -1.0f;
        return cur;
    }
};

constexpr uint32_t kTrackRgb = 0x0C0D10;

// The game's own bar colours: the fill image of its health / nano bar (CharacterBar_c makes a
// PowerbarView_c of background, fill and two end caps: health 0x0E, 0x0D, 0x0F, 0x10; nano 0x12, 0x11,
// 0x13, 0x14), averaged across its length - the skin's colours across the bar, with their shading.
// The game's bars stand upright (filled bottom to top, 35x200 in the default skin): their colour
// across is the image's columns, left to right, which become ours top to bottom. Along the length
// the image has its segment marks, which an average over the length leaves out.
struct Colours {
    uint32_t fill = 0;
};
Colours g_healthColours, g_nanoColours;
bool g_coloursLoaded = false;
constexpr int kHealthFillImage = 0x0D;
constexpr float kHealthDarker = 0.72f;
constexpr int kNanoFillImage = 0x11;

bool DecodePixel(const uint8_t* p, int format, uint32_t* rgb, float* alpha)
{
    if (format == kSfA8R8G8B8) {
        uint32_t v;
        memcpy(&v, p, 4);
        *rgb = v & 0xFFFFFF;
        *alpha = (v >> 24) / 255.0f;
        return true;
    }
    uint16_t v;
    memcpy(&v, p, 2);
    auto c5 = [](unsigned x) { return (x * 255 + 15) / 31; };
    auto c4 = [](unsigned x) { return x * 17; };
    switch (format) {
    case kSfA1R5G5B5:
        *rgb = (c5((v >> 10) & 0x1F) << 16) | (c5((v >> 5) & 0x1F) << 8) | c5(v & 0x1F);
        *alpha = (v >> 15) ? 1.0f : 0.0f;
        return true;
    case kSfA4R4G4B4:
        *rgb = (c4((v >> 8) & 0xF) << 16) | (c4((v >> 4) & 0xF) << 8) | c4(v & 0xF);
        *alpha = (v >> 12) / 15.0f;
        return true;
    case kSfR5G6B5:
        *rgb = (c5((v >> 11) & 0x1F) << 16) | (((((v >> 5) & 0x3F) * 255 + 31) / 63) << 8) | c5(v & 0x1F);
        *alpha = 1.0f;
        return true;
    default:
        return false;
    }
}

// Rows of the skin image `id`, each the average of its visible pixels; empty if it can't be read.
std::vector<uint32_t> ImageProfile(int id)
{
    void* mgr = GuiResources && GuiTexture && ReleaseTexture ? GuiResources() : nullptr;
    void* image = mgr ? GuiTexture(mgr, id, kSpriteFormatGui) : nullptr;
    if (!image)
        return {};
    std::vector<uint32_t> rows;
    const int w = SpriteWidth(image), h = SpriteHeight(image), stride = SpriteStride(image), bpp = SpriteBpp(image);
    const int format = SpriteSurfaceFormat(image);
    const uint8_t* data = SpriteData(image);
    const bool upright = h > w; // across = columns; else across = rows
    const int across = upright ? w : h, along = upright ? h : w;
    for (int i = 0; data && i < across; ++i) {
        float r = 0, g = 0, b = 0, n = 0;
        for (int j = 0; j < along; ++j) {
            const int x = upright ? i : j, y = upright ? j : i;
            uint32_t rgb;
            float a;
            if (!DecodePixel(data + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * bpp, format, &rgb, &a) || a <= 0.0f)
                continue;
            r += ((rgb >> 16) & 0xFF) * a;
            g += ((rgb >> 8) & 0xFF) * a;
            b += (rgb & 0xFF) * a;
            n += a;
        }
        if (n > 0.0f)
            rows.push_back((static_cast<uint32_t>(r / n + 0.5f) << 16) | (static_cast<uint32_t>(g / n + 0.5f) << 8) |
                           static_cast<uint32_t>(b / n + 0.5f));
    }
    Log("[bars] skin image 0x%02X: %dx%d, format %d, %d colours across", id, w, h, format, static_cast<int>(rows.size()));
    ReleaseTexture(mgr, image);
    return rows;
}

// One colour for the bar: the image's average across, or `fallback` if it couldn't be read.
Colours MakeColours(const std::vector<uint32_t>& across, uint32_t fallback)
{
    Colours c{fallback};
    if (!across.empty())
        c.fill = Canvas::ProfileAt(across, 0.0f, 1.0f);
    Log("[bars] bar colour #%06X", c.fill);
    return c;
}

void LoadColours()
{
    if (g_coloursLoaded)
        return;
    g_coloursLoaded = true;
    g_healthColours = MakeColours(ImageProfile(kHealthFillImage), 0xC83232);
    g_healthColours.fill = Darker(g_healthColours.fill, kHealthDarker); // a deeper red than the skin's
    g_nanoColours = MakeColours(ImageProfile(kNanoFillImage), 0x3C6ED2);
}

// A bar: a dark track, the filled part in one colour (`colours`, else rgb), the trail lighter, and
// the label centred. Square, no border.
void DrawBar(Canvas& c, float x, float y, float w, float h, int tenths, float trail, uint32_t rgb, const Colours* colours,
             FontId font, const std::wstring& label)
{
    c.RoundRect(x, y, x + w, y + h, 0.0f, kTrackRgb, kTrackRgb, 0.7f);
    if (tenths >= 0) {
        const float fill = x + w * tenths / 1000.0f;
        const uint32_t fillRgb = colours ? colours->fill : rgb;
        const uint32_t trailRgb = Canvas::Mix(fillRgb, 0xFFFFFF, 0.6f);
        if (trail > tenths)
            c.RoundRect(x, y, x + w, y + h, 0.0f, trailRgb, trailRgb, 0.9f, fill, x + w * trail / 1000.0f);
        c.RoundRect(x, y, x + w, y + h, 0.0f, fillRgb, fillRgb, 1.0f, x, fill);
    }
    if (!label.empty())
        DrawText(c, font, label, x + (w - TextWidth(font, label)) * 0.5f, CentredTop(font, y + h * 0.5f), 0xFFFFFF);
}

std::wstring Percent(int tenths, bool decimal)
{
    if (tenths < 0)
        return {};
    wchar_t s[16];
    if (decimal)
        swprintf_s(s, L"%d.%d%%", tenths / 10, tenths % 10);
    else
        swprintf_s(s, L"%d%%", (tenths + 5) / 10);
    return s;
}

// --- panels ---------------------------------------------------------------------------------

constexpr float kBarWidth = 232.0f;      // pixels at Scale 1
constexpr float kSmallBarWidth = 190.0f; // the nano target while there is a fight target
constexpr int kOwnBarH = 40;             // your health bar (the nano bar 75% of it)
constexpr int kOwnBarGap = 0;
constexpr float kTargetBarScale = 0.8f;  // the main target bar: this much of your two bars' height

struct Panel {
    const char* what;
    const wchar_t* keyX;
    const wchar_t* keyY;
    float BarsConfig::*cfgX;
    float BarsConfig::*cfgY;
    int texW, texH;            // the sprite (room for the largest Scale)

    void* window = nullptr;
    void* windowVtable = nullptr; // to notice the GUI deleting it and reusing the memory
    void* view = nullptr;
    void* surface = nullptr;
    void* sprite = nullptr;
    int w = 0, h = 0;          // content, pixels
    float gw = 0, gh = 0;      // content, GUI units
    float x = -1, y = -1;      // where the window is (top-left, GUI units)
    float homeX = 0, homeY = 0; // its own place: from [Bars], or the default
    float cfgSeenX = -2, cfgSeenY = -2; // the [Bars] position last taken
    std::string drawn;         // what the texture shows (redrawn when this changes)
    // A second surface over the first, for what changes only in strength (the fight target's
    // outline): faded with its alpha, without repainting or sending anything.
    void* overlaySprite = nullptr;
    void* overlaySurface = nullptr;
    std::string overlayDrawn;
    float overlayAlpha = 1.0f;
};

// Sprite sizes: the content at Scale 2 with room to spare.
Panel g_self{"self", L"SelfX", L"SelfY", &BarsConfig::selfX, &BarsConfig::selfY, 480, 224};
Panel g_fight{"fight target", L"TargetX", L"TargetY", &BarsConfig::targetX, &BarsConfig::targetY, 480, 224};
Panel g_nano{"nano target", L"NanoX", L"NanoY", &BarsConfig::nanoX, &BarsConfig::nanoY, 480, 224};
Panel g_tot{"nano target's target", L"TotX", L"TotY", &BarsConfig::totX, &BarsConfig::totY, 480, 224};
Panel* const g_panels[] = {&g_self, &g_fight, &g_nano, &g_tot};
bool g_nanoInFightPlace = false; // no fight target: the nano target is shown where the fight target's panel is

void* g_wc = nullptr;          // the window controller the panels belong to
bool g_disabled = false;       // a GUI object couldn't be made: no more tries
bool g_loggedFormat = false;
Panel* g_drag = nullptr;       // the panel being dragged

void Forget(Panel& p)
{
    p.window = p.view = p.surface = p.sprite = nullptr;
    p.overlaySprite = p.overlaySurface = nullptr;
    p.windowVtable = nullptr;
    p.w = p.h = 0;
    p.x = p.y = -1;
    p.drawn.clear();
    p.overlayDrawn.clear();
    p.overlayAlpha = 1.0f;
    if (g_drag == &p)
        g_drag = nullptr;
}

void Clamp(Panel& p, const GuiPoint& screen)
{
    p.x = std::clamp(p.x, 0.0f, std::max(0.0f, screen.x - p.gw));
    p.y = std::clamp(p.y, 0.0f, std::max(0.0f, screen.y - p.gh));
}

// The panel's own place: the [Bars] position when it changed (start, Clarity.ini edited, a drag saved),
// else the default - either side of the screen's middle, a little below it, the nano target under the
// fight target and the nano target's target right of it.
void UpdateHome(Panel& p, const GuiPoint& screen, float k)
{
    const BarsConfig& cfg = Cfg().bars;
    const float cx = cfg.*p.cfgX, cy = cfg.*p.cfgY;
    if (cx == p.cfgSeenX && cy == p.cfgSeenY)
        return;
    p.cfgSeenX = cx;
    p.cfgSeenY = cy;
    if (cx >= 0.0f && cy >= 0.0f) {
        p.homeX = cx;
        p.homeY = cy;
        return;
    }
    const float gap = 12.0f * k;
    p.homeX = &p == &g_self ? screen.x * 0.5f - gap - S(kBarWidth) * k : screen.x * 0.5f + gap;
    if (&p == &g_tot)
        p.homeX += S(kBarWidth + 4.0f) * k + gap;
    p.homeY = screen.y * 0.66f + (&p == &g_nano ? S(86) * k : 0.0f);
}

// The window exactly the panel's size: a plain View has no preferred size, and the window would
// shrink to nothing around it.
void ApplySize(Panel& p)
{
    const GuiPoint size{p.gw, p.gh};
    WindowSizeLimits(p.window, &size, &size);
    WindowResizeTo(p.window, p.gw, p.gh);
    const GuiRect view{0.0f, 0.0f, p.gw - 1.0f, p.gh - 1.0f};
    ViewSetFrame(p.view, &view, true);
}

// A texW x texH image (32-bit if the GUI takes it) and the surface showing it.
bool MakeSurface(int texW, int texH, void** sprite, void** surface)
{
    *sprite = CrtNew(kSpriteAlloc);
    *surface = CrtNew(kSurfaceAlloc);
    if (!*sprite || !*surface)
        return false;
    memset(*sprite, 0, kSpriteAlloc);
    *reinterpret_cast<int*>(static_cast<uint8_t*>(*sprite) + kSpriteD3dxFormat) = kSfA8R8G8B8;
    SpriteCtor(*sprite, texW, texH, kSpriteFormatAsIs);
    if (SpriteSurfaceFormat(*sprite) != kSfA8R8G8B8 || SpriteBpp(*sprite) != 4) {
        Log("[bars] 32-bit surface refused (format %d, %d bytes per pixel) - using the GUI's", SpriteSurfaceFormat(*sprite),
            SpriteBpp(*sprite));
        memset(*sprite, 0, kSpriteAlloc);
        SpriteCtor(*sprite, texW, texH, kSpriteFormatGui);
    }
    const int format = SpriteSurfaceFormat(*sprite);
    if (!g_loggedFormat) {
        g_loggedFormat = true;
        Log("[bars] surface format %d, %d bytes per pixel, %d per line", format, SpriteBpp(*sprite), SpriteStride(*sprite));
    }
    if (format != kSfA8R8G8B8 && format != kSfA1R5G5B5 && format != kSfA4R4G4B4) {
        Log("[bars] unexpected surface format %d - panels off", format);
        return false;
    }
    SurfaceCtor(*surface, *sprite, 0);
    return true;
}

bool Create(Panel& p, int w, int h, float k)
{
    p.window = CrtNew(kWindowAlloc);
    p.view = CrtNew(kViewAlloc);
    if (!p.window || !p.view || !MakeSurface(p.texW, p.texH, &p.sprite, &p.surface)) {
        Log("[bars] the %s panel couldn't be made", p.what);
        return false;
    }
    memset(p.window, 0, kWindowAlloc);
    memset(p.view, 0, kViewAlloc);

    p.w = w;
    p.h = h;
    p.gw = w * k;
    p.gh = h * k;
    const TempString empty("");
    const GuiRect frame{0.0f, 0.0f, p.gw - 1.0f, p.gh - 1.0f};
    WindowCtor(p.window, &frame, empty.buf, empty.buf, kWindowStyle, kWindowFlags);
    p.windowVtable = *static_cast<void**>(p.window);
    ViewCtor(p.view, &frame, empty.buf, kViewResize, kViewFlags);
    ViewSetResizeMask(p.view, kViewResize);
    WindowAddChild(p.window, p.view, false);
    ViewAddSurface(p.view, p.surface, true);
    ApplySize(p);
    Log("[bars] %s panel made (%dx%d px, %.2f GUI units per px)", p.what, w, h, k);
    return true;
}

void Resize(Panel& p, int w, int h, float k)
{
    if (w == p.w && h == p.h && std::fabs(p.gw - w * k) < 0.01f)
        return;
    p.w = w;
    p.h = h;
    p.gw = w * k;
    p.gh = h * k;
    ApplySize(p);
    p.drawn.clear();
}

// The canvas into the top-left of the sprite (straight alpha; 1555 / 4444 if the GUI's 16-bit surfaces
// are used), then just that part up, shown at the panel's size. The rest of the sprite isn't shown.
void Upload(const Panel& p, void* sprite, void* surface, const Canvas& c)
{
    uint8_t* data = SpriteData(sprite);
    const int stride = SpriteStride(sprite);
    const int format = SpriteSurfaceFormat(sprite);
    if (!data)
        return;
    const int cw = std::min(c.w, p.texW), chh = std::min(c.h, p.texH);
    for (int y = 0; y < chh; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * stride;
        for (int x = 0; x < cw; ++x) {
            uint32_t argb = 0;
            const float* s = &c.px[(static_cast<size_t>(y) * c.w + x) * 4];
            const float a = s[3];
            if (a > 0.0f) {
                auto ch = [&](float v) { return static_cast<uint32_t>(std::clamp(v / a, 0.0f, 1.0f) * 255.0f + 0.5f); };
                argb = (static_cast<uint32_t>(std::min(a, 1.0f) * 255.0f + 0.5f) << 24) | (ch(s[0]) << 16) | (ch(s[1]) << 8) | ch(s[2]);
            }
            if (format == kSfA8R8G8B8) {
                memcpy(row + x * 4, &argb, 4);
            } else if (format == kSfA1R5G5B5) {
                const uint16_t v = static_cast<uint16_t>(((argb >> 31) << 15) | (((argb >> 19) & 0x1F) << 10) |
                                                         (((argb >> 11) & 0x1F) << 5) | ((argb >> 3) & 0x1F));
                memcpy(row + x * 2, &v, 2);
            } else {
                const uint16_t v = static_cast<uint16_t>(((argb >> 28) << 12) | (((argb >> 20) & 0xF) << 8) |
                                                         (((argb >> 12) & 0xF) << 4) | ((argb >> 4) & 0xF));
                memcpy(row + x * 2, &v, 2);
            }
        }
    }
    const GuiRect src{0.0f, 0.0f, static_cast<float>(cw - 1), static_cast<float>(chh - 1)};
    const GuiPoint at{0.0f, 0.0f};
    SurfaceUpdate(surface, sprite, &src, &at);
    const GuiRect dst{0.0f, 0.0f, p.gw - 1.0f, p.gh - 1.0f};
    SurfaceSrc(surface, &src);
    SurfaceDst(surface, &dst);
}

// The panel's overlay (made on first use): repainted when `state` changed, shown at `alpha`.
template <typename Paint>
void PresentOverlay(Panel& p, const std::string& state, float alpha, Paint&& paint)
{
    if (!p.window)
        return;
    if (!p.overlaySurface) {
        if (!MakeSurface(p.texW, p.texH, &p.overlaySprite, &p.overlaySurface)) {
            p.overlaySprite = p.overlaySurface = nullptr;
            return;
        }
        ViewAddSurface(p.view, p.overlaySurface, true);
        p.overlayDrawn.clear();
        p.overlayAlpha = 1.0f;
    }
    if (state != p.overlayDrawn) {
        Canvas c;
        c.Reset(p.w, p.h);
        paint(c);
        Upload(p, p.overlaySprite, p.overlaySurface, c);
        p.overlayDrawn = state;
    }
    if (std::fabs(alpha - p.overlayAlpha) > 0.004f) {
        SurfaceAlpha(p.overlaySurface, alpha);
        p.overlayAlpha = alpha;
    }
}

void Show(Panel& p, bool show)
{
    if (p.window && WindowIsVisible(p.window) != show)
        WindowShow(p.window, show);
}

// Makes the panel if needed, sizes it, puts it at (x, y) (unless it's being dragged), and repaints
// it when `state` changed.
template <typename Paint>
void Present(Panel& p, int w, int h, float x, float y, const std::string& state, HWND hwnd, Paint&& paint)
{
    GuiPoint screen{};
    const float k = GuiPerPx(hwnd, &screen);
    w = std::min(w, p.texW);
    h = std::min(h, p.texH);
    if (!p.window) {
        if (!Create(p, w, h, k)) {
            g_disabled = true;
            Forget(p);
            return;
        }
    }
    Resize(p, w, h, k);
    if (g_drag != &p) {
        const float oldX = p.x, oldY = p.y;
        p.x = x;
        p.y = y;
        Clamp(p, screen);
        if (p.x != oldX || p.y != oldY)
            WindowMoveTo(p.window, p.x, p.y);
    }
    if (state != p.drawn) {
        Canvas c;
        c.Reset(w, h);
        paint(c);
        Upload(p, p.sprite, p.surface, c);
        p.drawn = state;
    }
    Show(p, true);
}

uint64_t Key(const game::Identity& id)
{
    return (static_cast<uint64_t>(id.type) << 32) | id.instance;
}

// A number as it fits in a bar: in full below 100,000 ("12,950"), else in thousands, millions or
// billions with one decimal ("250.5k", "12.3M", "1.0B").
std::wstring Short(int v)
{
    v = std::max(v, 0);
    wchar_t s[32];
    if (v >= 1000000000)
        swprintf_s(s, L"%.1fB", v / 1e9);
    else if (v >= 1000000)
        swprintf_s(s, L"%.1fM", v / 1e6);
    else if (v >= 100000)
        swprintf_s(s, L"%.1fk", v / 1e3);
    else {
        std::wstring g = std::to_wstring(v);
        for (int i = static_cast<int>(g.size()) - 3; i > 0; i -= 3)
            g.insert(static_cast<size_t>(i), L",");
        return g;
    }
    return s;
}

// A character's level, empty if unknown.
std::wstring Level(const game::Identity& id)
{
    const int level = id.type == 50000 ? n3::GetSkill(id, n3::kStatLevel) : n3::kNoValue;
    return level > 0 && level != n3::kNoValue ? std::to_wstring(level) : std::wstring();
}

// "8,412 / 12,950"
std::wstring Amount(int value, int max)
{
    if (value == n3::kNoValue || max == n3::kNoValue || max <= 0)
        return {};
    return Short(std::min(value, max)) + L" / " + Short(max);
}

// Numbers in a bar ([Bars] Numbers): the amount at the left, the percent at the right; Amount leaves
// out the percent, Percentage the amount.
constexpr int kNumbersAmount = 0, kNumbersPercent = 1, kNumbersBoth = 2;
void DrawBarText(Canvas& c, float y, float w, float h, const std::wstring& amount, const std::wstring& pct, int numbers)
{
    const float inset = static_cast<float>(S(6));
    const float top = CentredTop(kFontValue, y + h * 0.5f);
    if (numbers != kNumbersPercent && !amount.empty())
        DrawText(c, kFontValue, amount, inset, top, 0xFFFFFF);
    if ((numbers != kNumbersAmount || amount.empty()) && !pct.empty())
        DrawText(c, kFontValue, pct, w - inset - TextWidth(kFontValue, pct), top, 0xFFFFFF);
}

// Your health bar and your nano bar, 75% as thick, edge to edge.
float HealthBarH()
{
    return static_cast<float>(S(kOwnBarH));
}
float NanoBarH()
{
    return std::round(HealthBarH() * 0.75f);
}

void AppendText(std::string& state, const std::wstring& s)
{
    for (const wchar_t ch : s) {
        state += static_cast<char>(ch & 0xFF);
        state += static_cast<char>(ch >> 8);
    }
    state += '|';
}

Trail g_selfTrail;

void UpdateSelf(HWND hwnd, DWORD now)
{
    const BarsConfig& cfg = Cfg().bars;
    const game::Identity self{50000, game::ClientInstance()};
    const int health = n3::GetSkill(self, n3::kStatHealth), life = n3::GetSkill(self, n3::kStatLife);
    const int nano = n3::GetSkill(self, n3::kStatNano), maxNano = n3::GetSkill(self, n3::kStatMaxNano);
    const int hp = Tenths(health, life);
    const int np = Tenths(nano, maxNano);
    const float trail = g_selfTrail.Update(Key(self), hp, now, cfg.trail);
    const std::wstring hpAmount = Amount(health, life);
    const std::wstring npAmount = Amount(nano, maxNano);

    const int w = S(kBarWidth);
    const float healthH = HealthBarH(), gap = static_cast<float>(S(kOwnBarGap)), nanoH = NanoBarH();
    const int h = static_cast<int>(std::ceil(healthH + gap + nanoH));
    LoadColours();

    std::string state;
    char part[96];
    sprintf_s(part, "%d %d %d %d %.2f|", hp, np, trail < 0.0f ? -1 : static_cast<int>(trail * w / 1000.0f), cfg.numbers, g_scale);
    state += part;
    AppendText(state, hpAmount);
    AppendText(state, npAmount);
    // Your health over your nano, in the game's colours.
    Present(g_self, w, h, g_self.homeX, g_self.homeY, state, hwnd, [&](Canvas& c) {
        const float fw = static_cast<float>(w);
        DrawBar(c, 0.0f, 0.0f, fw, healthH, hp, trail, 0, &g_healthColours, kFontValue, {});
        DrawBarText(c, 0.0f, fw, healthH, hpAmount, Percent(hp, false), cfg.numbers);
        DrawBar(c, 0.0f, healthH + gap, fw, nanoH, np, -1.0f, 0, &g_nanoColours, kFontValue, {});
        DrawBarText(c, healthH + gap, fw, nanoH, npAmount, Percent(np, false), cfg.numbers);
    });
}

struct Row {
    game::Identity id;
    uint32_t nameRgb;
    std::wstring name;
    int hp;      // tenths, -1 = no bar
    uint32_t rgb;
    std::wstring amount;
    std::wstring level;
};

// The name and level in the game's consider colours (by level difference); the bar in your health
// bar's red for every target.
bool MakeRow(const game::Identity& id, Row* out)
{
    Row r{id, nameplates::ConsiderTextRgb(id), PlainName(id), -1, 0};
    if (id.type == 50000) { // characters; corpses and objects have a name only
        const int health = n3::GetSkill(id, n3::kStatHealth), life = n3::GetSkill(id, n3::kStatLife);
        r.hp = Tenths(health, life);
        r.amount = Amount(health, life);
        r.level = Level(id);
    }
    if (r.name.empty() && r.hp < 0)
        return false;
    r.rgb = g_healthColours.fill;
    *out = std::move(r);
    return true;
}

// The fight target's bar has an orange outline that fades out and in again: painted once on the
// panel's overlay, faded with the overlay's alpha.
constexpr DWORD kFadeMs = 1200;            // one fade out and in
constexpr uint32_t kOutlineRgb = 0xFF8A1A; // the plates' fight-target orange

float FadeAlpha(DWORD now)
{
    const float phase = static_cast<float>(now % kFadeMs) / kFadeMs;
    return 0.5f + 0.5f * std::cos(phase * 6.2831853f);
}

// A target's panel: the level (bold) and the name above its health bar in the consider colour, and in
// the bar the amount and the percent as on yours. The main one 80% as tall as your two bars together,
// the nano target's two thirds of that while there's a fight target. Every target panel has room for
// the outline round the bar, so the nano target's lines up when it takes the fight target's place.
void PresentTarget(Panel& p, const Row& r, bool small, bool outline, float x, float y, Trail& trailState, HWND hwnd,
                   DWORD now)
{
    const BarsConfig& cfg = Cfg().bars;
    const float trail = trailState.Update(Key(r.id), r.hp, now, cfg.trail);
    const int barW = small ? S(kSmallBarWidth) : S(kBarWidth);
    const float mainH = std::round((HealthBarH() + S(kOwnBarGap) + NanoBarH()) * kTargetBarScale);
    const float barH = small ? std::round(mainH * 2.0f / 3.0f) : mainH;
    const float edge = static_cast<float>(std::max(2, S(2))); // the outline's thickness, room on every side
    const float nameH = std::ceil(g_fonts[kFontName].Height());
    const float barTop = nameH + S(1) + edge;
    const int w = barW + static_cast<int>(2.0f * edge);
    const int h = static_cast<int>(std::ceil(barTop + barH + edge));

    std::string state;
    char part[96];
    sprintf_s(part, "%06X %d %d %06X %d %d %.2f|", r.nameRgb, small, r.hp, r.rgb,
              trail < 0.0f ? -1 : static_cast<int>(trail * barW / 1000.0f), cfg.numbers, g_scale);
    state += part;
    AppendText(state, r.name);
    AppendText(state, r.amount);
    AppendText(state, r.level);
    Present(p, w, h, x, y, state, hwnd, [&](Canvas& c) {
        const float fw = static_cast<float>(barW);
        float left = edge;
        if (!r.level.empty()) {
            DrawText(c, kFontValue, r.level, left, 0.0f, r.nameRgb);
            left += TextWidth(kFontValue, r.level) + S(6);
        }
        DrawText(c, kFontName, Fit(kFontName, r.name, edge + fw - left), left, 0.0f, r.nameRgb);
        // The bar (no health: an empty track) and its numbers, inside the outline's room.
        Canvas bar;
        bar.Reset(barW, static_cast<int>(barH));
        DrawBar(bar, 0.0f, 0.0f, fw, barH, r.hp, trail, r.rgb, nullptr, kFontValue, {});
        DrawBarText(bar, 0.0f, fw, barH, r.amount, Percent(r.hp, true), cfg.numbers);
        c.Paste(bar, static_cast<int>(edge), static_cast<int>(barTop));
    });
    if (!outline)
        return;
    char layout[64];
    sprintf_s(layout, "%d %d %.1f %.1f", w, h, barTop, barH);
    PresentOverlay(p, layout, FadeAlpha(now), [&](Canvas& c) {
        const float fw = static_cast<float>(barW);
        const float l = 0.0f, t = barTop - edge, rr = edge + fw + edge, b = barTop + barH + edge;
        c.RoundRect(l, t, rr, barTop, 0.0f, kOutlineRgb, kOutlineRgb, 1.0f);
        c.RoundRect(l, barTop + barH, rr, b, 0.0f, kOutlineRgb, kOutlineRgb, 1.0f);
        c.RoundRect(l, barTop, edge, barTop + barH, 0.0f, kOutlineRgb, kOutlineRgb, 1.0f);
        c.RoundRect(edge + fw, barTop, rr, barTop + barH, 0.0f, kOutlineRgb, kOutlineRgb, 1.0f);
    });
}

Trail g_fightTrail, g_nanoTrail, g_totTrail;

// The fight target's panel; the nano target's panel smaller at its own place while there's a fight
// target, else in the fight target's place at full size. One character as both: the fight target's
// panel only. The nano target's target (whoever it's targeting, maybe you) small at its own place.
void UpdateTargets(HWND hwnd, DWORD now)
{
    const BarsConfig& cfg = Cfg().bars;
    game::Identity fight{}, nano{};
    const bool hasFight = cfg.showTarget && n3::FightTarget(&fight);
    const bool hasNano = cfg.showTarget && n3::SelectedTarget(&nano);
    const bool same = hasFight && hasNano && Key(fight) == Key(nano);
    Row fightRow{}, nanoRow{};
    const bool fightShown = hasFight && MakeRow(fight, &fightRow);
    const bool nanoShown = hasNano && !same && MakeRow(nano, &nanoRow);

    if (fightShown) {
        PresentTarget(g_fight, fightRow, false, true, g_fight.homeX, g_fight.homeY, g_fightTrail, hwnd, now);
    } else {
        Show(g_fight, false);
        g_fightTrail.Update(0, -1, now, false);
    }
    if (g_disabled)
        return;
    if (nanoShown) {
        g_nanoInFightPlace = !fightShown;
        const Panel& home = fightShown ? g_nano : g_fight;
        PresentTarget(g_nano, nanoRow, fightShown, false, home.homeX, home.homeY, g_nanoTrail, hwnd, now);
    } else {
        Show(g_nano, false);
        g_nanoTrail.Update(0, -1, now, false);
    }
    if (g_disabled)
        return;
    game::Identity tot{};
    Row totRow{};
    if (hasNano && cfg.showTargetTarget && n3::TargetOf(nano, &tot) && MakeRow(tot, &totRow)) {
        PresentTarget(g_tot, totRow, true, false, g_tot.homeX, g_tot.homeY, g_totTrail, hwnd, now);
    } else {
        Show(g_tot, false);
        g_totTrail.Update(0, -1, now, false);
    }
}

// --- the game's bars ------------------------------------------------------------------------

std::vector<void*> g_hiddenGameBars; // CharBarWindow_c windows Clarity hid
DWORD g_lastScan = 0;

// Hides the game's health/nano bars (hideSelf) and target bars (hideTargets); gives back the ones
// Clarity hid that shouldn't be hidden any more.
void SyncGameBars(void* wc, bool hideSelf, bool hideTargets)
{
    static const char* const kSelfBars[] = {"CCHealthBarConfig", "CCNanoBarConfig"};
    static const char* const kTargetBars[] = {"CCFriendlyHealthBarConfig", "CCHostileHealthBarConfig"};
    std::vector<void*> hidden;
    ForEachWindow(wc, [&](void* w) {
        if (!IsClass(w, kCharBarClass))
            return;
        const std::string name = ConfigName(w);
        bool hide;
        if (name == kSelfBars[0] || name == kSelfBars[1])
            hide = hideSelf;
        else if (name == kTargetBars[0] || name == kTargetBars[1])
            hide = hideTargets;
        else
            return;
        const bool ours = std::find(g_hiddenGameBars.begin(), g_hiddenGameBars.end(), w) != g_hiddenGameBars.end();
        if (hide) {
            if (WindowIsVisible(w))
                WindowShow(w, false);
            hidden.push_back(w);
        } else if (ours && CriteriaEvaluate && CriteriaEvaluate(static_cast<uint8_t*>(w) + kCharBarCriteria)) {
            WindowShow(w, true); // its own condition says it's on
        }
    });
    g_hiddenGameBars.swap(hidden);
}

bool InGame()
{
    void* cc = ControlCenter();
    return cc && *(static_cast<const uint8_t*>(cc) + kCcAlive) && game::ClientInstance() != 0;
}

void Update(HWND hwnd)
{
    void* wc = WindowController();
    if (wc != g_wc) {
        for (Panel* p : g_panels)
            Forget(*p);
        g_hiddenGameBars.clear();
        g_wc = wc;
    }
    if (!wc)
        return;
    for (Panel* p : g_panels) {
        if (p->window && (!InWindowList(wc, p->window) || *static_cast<void**>(p->window) != p->windowVtable)) {
            Log("[bars] the %s panel was removed by the game - making it again", p->what);
            Forget(*p);
        }
    }
    const BarsConfig& cfg = Cfg().bars;
    const bool on = cfg.enable && !g_disabled && InGame();
    const DWORD now = GetTickCount();
    if (now - g_lastScan >= 250) {
        g_lastScan = now;
        SyncGameBars(wc, on, on && cfg.showTarget);
    }
    if (!on || !EnsureFonts(cfg.scale)) {
        for (Panel* p : g_panels)
            Show(*p, false);
        return;
    }
    GuiPoint screen{};
    const float k = GuiPerPx(hwnd, &screen);
    for (Panel* p : g_panels)
        UpdateHome(*p, screen, k);
    UpdateSelf(hwnd, now);
    if (!g_disabled)
        UpdateTargets(hwnd, now);
}

// --- the game window: timer and dragging ----------------------------------------------------

constexpr UINT_PTR kTimerId = 0xC1A7;
constexpr UINT kTickMs = 33;
bool g_timerSet = false;

POINT g_dragMouse{};
float g_dragX = 0, g_dragY = 0;

// The dropped panel's place becomes its home - or the fight target's, when the nano target was
// standing in for it.
void SavePosition(Panel& p)
{
    p.x = std::round(p.x);
    p.y = std::round(p.y);
    WindowMoveTo(p.window, p.x, p.y);
    Panel& home = (&p == &g_nano && g_nanoInFightPlace) ? g_fight : p;
    home.homeX = p.x;
    home.homeY = p.y;
    wchar_t v[32];
    swprintf_s(v, L"%.0f", p.x);
    WritePrivateProfileStringW(L"Bars", home.keyX, v, ConfigPath().c_str());
    swprintf_s(v, L"%.0f", p.y);
    WritePrivateProfileStringW(L"Bars", home.keyY, v, ConfigPath().c_str());
    // Until Clarity.ini is read again the settings still hold the old place: that isn't a change to
    // take (when the new one arrives, it's where the panel already is).
    home.cfgSeenX = Cfg().bars.*home.cfgX;
    home.cfgSeenY = Cfg().bars.*home.cfgY;
}

void EndDrag()
{
    if (!g_drag)
        return;
    Panel& p = *g_drag;
    g_drag = nullptr;
    if (GetCapture())
        ReleaseCapture();
    if (p.window)
        SavePosition(p);
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
    case WM_LBUTTONDOWN: {
        const BarsConfig& cfg = Cfg().bars;
        if (g_drag || !cfg.enable || cfg.locked || !g_wc || (wp & (MK_RBUTTON | MK_SHIFT | MK_CONTROL)))
            return false;
        GuiPoint screen{};
        const float k = GuiPerPx(hwnd, &screen);
        const float gx = static_cast<short>(LOWORD(lp)) * k, gy = static_cast<short>(HIWORD(lp)) * k;
        void* under = MouseWindow(g_wc);
        for (Panel* p : g_panels) {
            if (!p->window || !WindowIsVisible(p->window) || (under && under != p->window))
                continue;
            if (gx < p->x || gy < p->y || gx >= p->x + p->gw || gy >= p->y + p->gh)
                continue;
            g_drag = p;
            g_dragMouse = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            g_dragX = p->x;
            g_dragY = p->y;
            SetCapture(hwnd);
            *result = 0;
            return true;
        }
        return false;
    }
    case WM_MOUSEMOVE:
        if (!g_drag)
            return false;
        if (g_drag->window) {
            GuiPoint screen{};
            const float k = GuiPerPx(hwnd, &screen);
            g_drag->x = g_dragX + (static_cast<short>(LOWORD(lp)) - g_dragMouse.x) * k;
            g_drag->y = g_dragY + (static_cast<short>(HIWORD(lp)) - g_dragMouse.y) * k;
            Clamp(*g_drag, screen);
            WindowMoveTo(g_drag->window, g_drag->x, g_drag->y);
        }
        *result = 0;
        return true;
    case WM_LBUTTONUP:
        if (!g_drag)
            return false;
        EndDrag();
        *result = 0;
        return true;
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
        EndDrag();
        return false;
    default:
        return false;
    }
}

} // namespace

bool Install()
{
    if (!ResolveAll()) {
        Log("[bars] disabled");
        return false;
    }
    wndproc::Add(&OnMessage);
    Log("[bars] ready");
    return true;
}

} // namespace prk::bars
