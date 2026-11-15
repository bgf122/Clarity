#include "gui.h"

#include "game.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace prk::gui {

namespace {

using CrtNewFn = void*(__cdecl*)(size_t);
using StringCtorFn = void*(__thiscall*)(void* self, const char* s);
using StringDtorFn = void(__thiscall*)(void* self);
using InstanceFn = void*(__cdecl*)();
using WindowCtorFn = void*(__thiscall*)(void* self, const Rect* frame, const void* name, const void* title, int style, unsigned flags);
using WindowShowFn = void(__thiscall*)(void* self, bool show);
using WindowIsVisibleFn = bool(__thiscall*)(const void* self);
using WindowAddChildFn = void(__thiscall*)(void* self, void* view, bool);
using ViewAddChildFn = void(__thiscall*)(void* self, void* view, bool);
using WindowMoveToFn = void(__thiscall*)(void* self, float x, float y);
using WindowFn = void(__thiscall*)(void* self);
using WindowResizeToFn = void(__thiscall*)(void* self, float w, float h);
using WindowSizeLimitsFn = void(__thiscall*)(void* self, const Point* min, const Point* max);
using WindowFrameFn = Rect*(__thiscall*)(const void* self, Rect* ret, bool client);
using ScreenSizeFn = Point*(__cdecl*)(Point* ret);
using ObjectFn = void*(__thiscall*)(const void* self);
using ViewCtorFn = void*(__thiscall*)(void* self, const Rect* frame, const void* name, unsigned resizeMask, unsigned flags);
using ViewSetFrameFn = void(__thiscall*)(void* self, const Rect* frame, bool);
using ViewSetResizeMaskFn = void(__thiscall*)(void* self, unsigned mask);
using ViewAddSurfaceFn = void(__thiscall*)(void* self, void* surface, bool);
using ViewRemoveSurfaceFn = void(__thiscall*)(void* self, void* surface);
using ViewAlphaFn = void(__thiscall*)(void* self, float alpha);
using ImageLoadFn = void*(__cdecl*)(int id, unsigned flags);
using ImageDtorFn = void(__thiscall*)(void* self);
using ImageSrcFn = Rect*(__thiscall*)(const void* self, Rect* ret);
using ImageAlphaFn = void(__thiscall*)(void* self, float alpha);
using CrtDeleteFn = void(__cdecl*)(void*);
// View::SetToolTip(const std::string& title, const std::string& text): VS2010 std::strings (the game's CRT).
struct GameString {
    union {
        char buf[16];
        const char* ptr;
    };
    uint32_t size;
    uint32_t cap;
};
using ViewToolTipFn = void(__thiscall*)(void* self, const GameString* title, const GameString* text);
using ViewShowFn = void(__thiscall*)(void* self, bool show, bool absolute);
using ViewGetFlagsFn = unsigned(__thiscall*)(const void* self, unsigned mask);
using ViewSetFlagsFn = void(__thiscall*)(void* self, unsigned flags);
using ViewToScreenFn = void(__thiscall*)(const void* self, Point* p, bool screen);
using ViewFloatFn = float(__thiscall*)(const void* self);
using SurfaceCtorFn = void*(__thiscall*)(void* self, void* sprite, unsigned flags);
using SurfaceUpdateFn = void(__thiscall*)(void* self, void* sprite, const Rect* from, const Point* to);
using SurfaceFrameFn = void(__thiscall*)(void* self, const Rect* frame);
using SpriteCtorFn = void*(__thiscall*)(void* self, int w, int h, int format);
using SpriteDataFn = uint8_t*(__thiscall*)(void* self);
using SpriteIntFn = int(__thiscall*)(const void* self);
using GuiTextureFn = void*(__thiscall*)(void* self, int id, int format);
using ReleaseTextureFn = void(__thiscall*)(void* self, void* sprite);

CrtNewFn CrtNew;
StringCtorFn StringCtor;
StringDtorFn StringDtor;
InstanceFn WcInstance, CcInstance;
WindowCtorFn WindowCtor;
WindowShowFn WindowShow;
WindowIsVisibleFn WindowIsVisible;
WindowAddChildFn WindowAddChild;
ViewAddChildFn ViewAddChild;
WindowMoveToFn WindowMove;
WindowFn WindowMoveToFront;
WindowResizeToFn WindowResizeTo;
WindowSizeLimitsFn WindowSizeLimits;
WindowFrameFn WindowGetFrame;
ScreenSizeFn ScreenSize;
ObjectFn MouseWindowFn, DragObjectFn, TopChild, LowerSibling;
ViewCtorFn ViewCtor;
ViewSetFrameFn ViewSetFrame;
ViewSetResizeMaskFn ViewSetResizeMask;
ViewAddSurfaceFn ViewAddSurface;
ViewRemoveSurfaceFn ViewRemoveSurface;
ViewAlphaFn ViewAlpha;
ImageLoadFn ImageLoad;
ImageDtorFn ImageDtor;
ImageSrcFn ImageSrc;
ImageAlphaFn ImageAlpha;
CrtDeleteFn CrtDelete;
ViewToolTipFn ViewToolTip;
ViewShowFn ViewShowFnPtr;
ViewGetFlagsFn ViewGetFlags;
ViewSetFlagsFn ViewSetFlagsFnPtr;
ViewToScreenFn ViewToScreenFnPtr;
ViewFloatFn ViewWidthFn, ViewHeightFn;
ObjectFn ViewParent;
SurfaceCtorFn SurfaceCtor;
SurfaceUpdateFn SurfaceUpdate;
SurfaceFrameFn SurfaceSrc, SurfaceDst;
SpriteCtorFn SpriteCtor;
SpriteDataFn SpriteData;
SpriteIntFn SpriteStride, SpriteBpp, SpriteSurfaceFormat, SpriteWidth, SpriteHeight;
InstanceFn GuiResources;
GuiTextureFn GuiTexture;
ReleaseTextureFn ReleaseTexture;

const uint8_t* g_guiBase = nullptr;
size_t g_guiSize = 0;
bool g_ok = false;

// Allocation sizes: as bars.cpp (Window 0x74 + the class's own, View 0x128, ViewSurface_c 0x68,
// SpriteInfo_t 0x44).
constexpr size_t kWindowAlloc = 0x200, kViewAlloc = 0x200, kSurfaceAlloc = 0x68, kSpriteAlloc = 0x44;
constexpr int kWindowStyle = 3;
constexpr unsigned kViewResize = 0xF, kViewFlags = 2;
constexpr int kSpriteFormatGui = 2, kSpriteFormatAsIs = 3, kSpriteD3dxFormat = 0x38;
constexpr int kSfA8R8G8B8 = 2, kSfR5G6B5 = 4, kSfA1R5G5B5 = 8, kSfA4R4G4B4 = 10;
constexpr int kWcRootView = 0x94; // WindowController_c: the view the windows' root views are children of
constexpr int kCcAlive = 0xB8;    // ControlCenterModule_c: the GUI was made

const char* g_who = "gui";

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[%s] missing export %s!%s", g_who, module, name);
    return fn != nullptr;
}

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

bool MakeSprite(Surface& s)
{
    s.sprite = CrtNew(kSpriteAlloc);
    s.surface = CrtNew(kSurfaceAlloc);
    if (!s.sprite || !s.surface)
        return false;
    memset(s.sprite, 0, kSpriteAlloc);
    *reinterpret_cast<int*>(static_cast<uint8_t*>(s.sprite) + kSpriteD3dxFormat) = kSfA8R8G8B8;
    SpriteCtor(s.sprite, s.texW, s.texH, kSpriteFormatAsIs);
    if (SpriteSurfaceFormat(s.sprite) != kSfA8R8G8B8 || SpriteBpp(s.sprite) != 4) {
        memset(s.sprite, 0, kSpriteAlloc);
        SpriteCtor(s.sprite, s.texW, s.texH, kSpriteFormatGui);
    }
    const int format = SpriteSurfaceFormat(s.sprite);
    if (format != kSfA8R8G8B8 && format != kSfA1R5G5B5 && format != kSfA4R4G4B4) {
        Log("[%s] unexpected surface format %d", g_who, format);
        return false;
    }
    SurfaceCtor(s.surface, s.sprite, 0);
    return true;
}

void ApplySize(Surface& s)
{
    const Point size{s.gw, s.gh};
    WindowSizeLimits(s.window, &size, &size);
    WindowResizeTo(s.window, s.gw, s.gh);
    const Rect view{0.0f, 0.0f, s.gw - 1.0f, s.gh - 1.0f};
    for (int i = 0; i < s.nesting; ++i) // each clips what's in it: all the content's size
        if (s.chain[i])
            ViewSetFrame(s.chain[i], &view, true);
    ViewSetFrame(s.view, &view, true);
}

} // namespace

bool Init(const char* who)
{
    if (g_ok)
        return true;
    g_who = who;
    const char* gui = "GUI.dll";
    const char* ds = "DisplaySystem.dll";
    bool ok = true;
    ok &= Resolve(CrtNew, "msvcr100.dll", "??2@YAPAXI@Z");
    ok &= Resolve(StringCtor, "Utils.dll", "??0String@@QAE@PBD@Z");
    ok &= Resolve(StringDtor, "Utils.dll", "??1String@@QAE@XZ");
    ok &= Resolve(WcInstance, gui, "?GetInstanceIfAny@WindowController_c@@SAPAV1@XZ");
    ok &= Resolve(CcInstance, gui, "?GetInstanceIfAny@ControlCenterModule_c@@SAPAV1@XZ");
    ok &= Resolve(WindowCtor, gui, "??0Window@@QAE@ABVRect@@ABVString@@1W4WindowStyle_e@@I@Z");
    ok &= Resolve(WindowShow, gui, "?Show@Window@@QAEX_N@Z");
    ok &= Resolve(WindowIsVisible, gui, "?IsVisible@Window@@QBE_NXZ");
    ok &= Resolve(WindowAddChild, gui, "?AddChild@Window@@QAEXPAVView@@_N@Z");
    ok &= Resolve(ViewAddChild, gui, "?AddChild@View@@UAEXPAV1@_N@Z");
    ok &= Resolve(WindowMove, gui, "?MoveTo@Window@@QAEXMM@Z");
    ok &= Resolve(WindowMoveToFront, gui, "?MoveToFront@Window@@QAEXXZ");
    ok &= Resolve(WindowResizeTo, gui, "?ResizeTo@Window@@QAEXMM@Z");
    ok &= Resolve(WindowSizeLimits, gui, "?SetSizeLimits@Window@@QAEXABVPoint@@0@Z");
    ok &= Resolve(WindowGetFrame, gui, "?GetFrame@Window@@QBE?AVRect@@_N@Z");
    ok &= Resolve(ScreenSize, gui, "?GetScreenSize@Window@@SA?AVPoint@@XZ");
    ok &= Resolve(MouseWindowFn, gui, "?GetMouseWindow@WindowController_c@@QAEPAVWindow@@XZ");
    ok &= Resolve(DragObjectFn, gui, "?GetDragObject@WindowController_c@@QBEPAVDragObject_c@@XZ");
    ok &= Resolve(TopChild, gui, "?GetTopChild@View@@QBEPAV1@XZ");
    ok &= Resolve(LowerSibling, gui, "?GetLowerSibling@View@@QBEPAV1@XZ");
    ok &= Resolve(ViewCtor, gui, "??0View@@QAE@ABVRect@@ABVString@@II@Z");
    ok &= Resolve(ViewSetFrame, gui, "?SetFrame@View@@UAEXABVRect@@_N@Z");
    ok &= Resolve(ViewSetResizeMask, gui, "?SetResizeMask@View@@QAEXI@Z");
    ok &= Resolve(ViewAddSurface, gui, "?AddRenderSurface@View@@QAEXPAVViewSurface_c@@_N@Z");
    ok &= Resolve(ViewRemoveSurface, gui, "?RemoveRenderSurface@View@@QAEXPAVViewSurface_c@@@Z");
    ok &= Resolve(ViewAlpha, gui, "?SetAlpha@View@@QAEXM@Z");
    ok &= Resolve(ImageLoad, gui, "?Load@ViewSurface_c@@SAPAV1@HI@Z");
    ok &= Resolve(ImageDtor, gui, "??1ViewSurface_c@@QAE@XZ");
    ok &= Resolve(ImageSrc, gui, "?GetSrcFrame@ViewSurface_c@@QBE?AVRect@@XZ");
    ok &= Resolve(ImageAlpha, gui, "?SetAlpha@ViewSurface_c@@QAEXM@Z");
    ok &= Resolve(CrtDelete, "msvcr100.dll", "??3@YAXPAX@Z");
    ok &= Resolve(ViewToolTip, gui, "?SetToolTip@View@@QAEXABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0@Z");
    ok &= Resolve(ViewShowFnPtr, gui, "?Show@View@@QAEX_N0@Z");
    ok &= Resolve(ViewGetFlags, gui, "?GetFlags@View@@QBEII@Z");
    ok &= Resolve(ViewSetFlagsFnPtr, gui, "?SetFlags@View@@QAEXI@Z");
    ok &= Resolve(ViewToScreenFnPtr, gui, "?ConvertToScreen@View@@QBEXPAVPoint@@_N@Z");
    ok &= Resolve(ViewWidthFn, gui, "?Width@View@@QBEMXZ");
    ok &= Resolve(ViewParent, gui, "?GetParent@View@@QBEPAV1@XZ");
    ok &= Resolve(ViewHeightFn, gui, "?Height@View@@QBEMXZ");
    ok &= Resolve(SurfaceCtor, gui, "??0ViewSurface_c@@QAE@PAVSpriteInfo_t@@I@Z");
    ok &= Resolve(SurfaceUpdate, gui, "?UpdateTexture@ViewSurface_c@@QAEXPAVSpriteInfo_t@@ABVRect@@ABVPoint@@@Z");
    ok &= Resolve(SurfaceSrc, gui, "?SetSrcFrame@ViewSurface_c@@QAEXABVRect@@@Z");
    ok &= Resolve(SurfaceDst, gui, "?SetDstFrame@ViewSurface_c@@QAEXABVRect@@@Z");
    ok &= Resolve(SpriteCtor, ds, "??0SpriteInfo_t@@QAE@HHW4Format_e@@@Z");
    ok &= Resolve(SpriteData, ds, "?GetData@SpriteInfo_t@@QAEPAEXZ");
    ok &= Resolve(SpriteStride, ds, "?GetBytesPerLine@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteBpp, ds, "?GetBytesPerPixel@SpriteInfo_t@@QBEHXZ");
    ok &= Resolve(SpriteSurfaceFormat, ds, "?GetSurfaceFormat@SpriteInfo_t@@QBE?AW4_D3DX_SURFACEFORMAT@@XZ");
    // Skin images (optional: fixed colours without them).
    Resolve(SpriteWidth, ds, "?GetWidth@SpriteInfo_t@@QBEHXZ");
    Resolve(SpriteHeight, ds, "?GetHeight@SpriteInfo_t@@QBEHXZ");
    Resolve(GuiResources, "Interfaces.dll", "?GetInstance@GuiResourceManager_t@@SAPAV1@XZ");
    Resolve(GuiTexture, "Interfaces.dll", "?GetGuiTexture@GuiResourceManager_t@@QAEPAVSpriteInfo_t@@HW4Format_e@@@Z");
    Resolve(ReleaseTexture, "Interfaces.dll", "?ReleaseTexture@GuiResourceManager_t@@QAEXPAVSpriteInfo_t@@@Z");
    if (HMODULE h = GetModuleHandleA(gui)) {
        g_guiBase = reinterpret_cast<const uint8_t*>(h);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_guiBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(h)->e_lfanew);
        g_guiSize = nt->OptionalHeader.SizeOfImage;
    }
    g_ok = ok && g_guiBase;
    return g_ok;
}

void* WindowController()
{
    return WcInstance();
}

bool InGame()
{
    void* cc = CcInstance();
    return cc && *(static_cast<const uint8_t*>(cc) + kCcAlive) && game::ClientInstance() != 0;
}

bool InWindowList(void* wc, void* window)
{
    bool found = false;
    ForEachWindow(wc, [&](void* w) { found = found || w == window; });
    return found;
}

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

namespace {
std::vector<void*> g_madeThrough; // windows Clarity set flag 1 on
} // namespace

unsigned* WindowFlagsField(void* window)
{
    auto* priv = window ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(window) + 0x70) : nullptr;
    return priv ? reinterpret_cast<unsigned*>(priv + 0x3C) : nullptr;
}

void MouseThrough(void* window, bool on)
{
    auto* priv = window ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(window) + 0x70) : nullptr;
    if (!priv)
        return;
    unsigned& flags = *reinterpret_cast<unsigned*>(priv + 0x3C);
    auto it = std::find(g_madeThrough.begin(), g_madeThrough.end(), window);
    if (on) {
        if (!(flags & 1u) && it == g_madeThrough.end()) {
            flags |= 1u;
            g_madeThrough.push_back(window);
        }
    } else if (it != g_madeThrough.end()) {
        flags &= ~1u;
        g_madeThrough.erase(it);
    }
}

const char* ClassName(void* obj)
{
    if (!obj)
        return "?";
    void** vtable = *static_cast<void***>(obj);
    if (!InGui(vtable - 1))
        return "?";
    const auto* col = static_cast<const uint8_t*>(vtable[-1]);
    if (!InGui(col, 0x14))
        return "?";
    const auto* td = *reinterpret_cast<const uint8_t* const*>(col + 0x0C);
    return InGui(td, 9) ? reinterpret_cast<const char*>(td + 8) : "?";
}

float GuiPerPx(HWND hwnd, Point* screen)
{
    ScreenSize(screen);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const float k = cw > 0 && screen->x > 0.0f ? screen->x / cw : 1.0f;
    // Unscaled GUIs come out a hair off 1 (1919 / 1920): take that as 1, or every texture is drawn a
    // fraction of a pixel small and blurred.
    return std::fabs(k - 1.0f) < 0.01f ? 1.0f : k;
}

void* MouseWindow(void* wc)
{
    return MouseWindowFn(wc);
}

bool Dragging(void* wc)
{
    return DragObjectFn(wc) != nullptr;
}

Rect WindowFrame(void* window)
{
    Rect r{};
    WindowGetFrame(window, &r, false);
    return r;
}

void WindowMoveTo(void* window, float x, float y)
{
    WindowMove(window, x, y);
}

bool WindowVisible(void* window)
{
    return WindowIsVisible(window);
}

void WindowFront(void* window)
{
    WindowMoveToFront(window);
}

void WindowSetShown(void* window, bool show)
{
    WindowShow(window, show);
}

void* WindowRoot(void* window)
{
    // Window +0x70: its private data; +0x80 there: the WndBorder view the Window ctor makes.
    const auto* priv = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(window) + 0x70);
    return priv ? *reinterpret_cast<void* const*>(priv + 0x80) : nullptr;
}

bool Above(void* wc, void* a, void* b)
{
    void* root = *reinterpret_cast<void**>(static_cast<uint8_t*>(wc) + kWcRootView);
    void* ra = WindowRoot(a);
    void* rb = WindowRoot(b);
    if (!root || !ra || !rb)
        return true;
    int guard = 0;
    for (void* v = TopChild(root); v && guard < 4096; v = LowerSibling(v), ++guard) { // the top one first
        if (v == ra)
            return true;
        if (v == rb)
            return false;
    }
    return true;
}

Point ViewSize(void* view)
{
    return {ViewWidthFn(view) + 1.0f, ViewHeightFn(view) + 1.0f};
}

int ViewDepth(void* view)
{
    void* wc = WcInstance();
    void* top = wc ? *reinterpret_cast<void**>(static_cast<uint8_t*>(wc) + kWcRootView) : nullptr;
    int depth = 0;
    for (void* p = ViewParent(view); p && p != top && depth < 64; p = ViewParent(p))
        ++depth;
    return depth;
}

void ViewShow(void* view, bool show)
{
    ViewShowFnPtr(view, show, true);
}

unsigned ViewFlags(void* view)
{
    return ViewGetFlags(view, 0xFFFFFFFFu);
}

void ViewSetFlags(void* view, unsigned flags)
{
    ViewSetFlagsFnPtr(view, flags);
}

Point ViewToScreen(void* view, Point local)
{
    ViewToScreenFnPtr(view, &local, true);
    return local;
}

void* MakeChildView(void* parent, const Rect& frame)
{
    void* v = CrtNew(kViewAlloc);
    if (!v)
        return nullptr;
    memset(v, 0, kViewAlloc);
    const TempString empty("");
    ViewCtor(v, &frame, empty.buf, kViewResize, kViewFlags);
    ViewAddChild(parent, v, false);
    ViewSetFrame(v, &frame, true);
    return v;
}

void SetViewFrame(void* view, const Rect& frame)
{
    ViewSetFrame(view, &frame, true);
}

void SetViewAlpha(void* view, float alpha)
{
    ViewAlpha(view, alpha);
}

void* LoadImage(int id, bool icon)
{
    return ImageLoad(id, icon ? 0x80u : 0u); // as ItemIconView_c (GUI 0x1003FA39)
}

void AttachImage(void* view, void* image, const Rect& dst, bool leftHalf)
{
    ViewAddSurface(view, image, true);
    if (leftHalf) {
        Rect src{};
        ImageSrc(image, &src);
        const float w = src.right - src.left + 1.0f, h = src.bottom - src.top + 1.0f;
        if (w >= h * 1.5f) { // two side by side: the left one (as GUI 0x1003EFE6: half = floor((w + 1) / 2))
            const float half = std::floor(w * 0.5f);
            const Rect left{src.left, src.top, src.left + half - 1.0f, src.bottom};
            SurfaceSrc(image, &left);
        }
    }
    SurfaceDst(image, &dst);
}

Rect ImageSource(void* image)
{
    Rect r{};
    ImageSrc(image, &r);
    return r;
}

void SetImageFrames(void* image, const Rect& src, const Rect& dst)
{
    SurfaceSrc(image, &src);
    SurfaceDst(image, &dst);
}

void SetImageAlpha(void* image, float alpha)
{
    ImageAlpha(image, alpha);
}

void DestroyImage(void* view, void* image)
{
    if (!image)
        return;
    if (view)
        ViewRemoveSurface(view, image);
    ImageDtor(image);
    CrtDelete(image);
}

namespace {

GameString Borrow(const std::string& s)
{
    GameString g{};
    g.size = static_cast<uint32_t>(s.size());
    if (s.size() < 16) {
        memcpy(g.buf, s.c_str(), s.size() + 1);
        g.cap = 15;
    } else {
        g.ptr = s.c_str(); // read (copied) by the game during the call only
        g.cap = g.size;
    }
    return g;
}

} // namespace

void SetToolTip(void* view, const std::string& title, const std::string& text)
{
    const GameString t = Borrow(title), x = Borrow(text);
    ViewToolTip(view, &t, &x);
}

void KeepSize(Surface& s)
{
    if (!s.window)
        return;
    const Rect f = WindowFrame(s.window);
    if (std::fabs((f.right - f.left + 1.0f) - s.gw) > 0.5f || std::fabs((f.bottom - f.top + 1.0f) - s.gh) > 0.5f)
        ApplySize(s);
}

void SetOpacity(Surface& s, float alpha)
{
    if (void* root = s.window ? WindowRoot(s.window) : nullptr)
        ViewAlpha(root, alpha);
}

bool Ensure(Surface& s, int w, int h, float k)
{
    w = std::clamp(w, 1, s.texW);
    h = std::clamp(h, 1, s.texH);
    if (!s.window) {
        s.window = CrtNew(kWindowAlloc);
        s.view = CrtNew(kViewAlloc);
        if (!s.window || !s.view || !MakeSprite(s)) {
            Log("[%s] the %s window couldn't be made", g_who, s.what);
            s.window = s.view = s.surface = s.sprite = nullptr;
            return false;
        }
        memset(s.window, 0, kWindowAlloc);
        memset(s.view, 0, kViewAlloc);
        s.w = w;
        s.h = h;
        s.gw = w * k;
        s.gh = h * k;
        const TempString empty("");
        const Rect frame{0.0f, 0.0f, s.gw - 1.0f, s.gh - 1.0f};
        WindowCtor(s.window, &frame, empty.buf, empty.buf, kWindowStyle, s.flags);
        s.windowVtable = *static_cast<void**>(s.window);
        s.nesting = std::clamp(s.nesting, 0, kMaxNesting);
        void* outer = nullptr; // the view the next one goes in (none: the window)
        for (int i = 0; i < s.nesting; ++i) {
            void* v = CrtNew(kViewAlloc);
            if (!v)
                break;
            memset(v, 0, kViewAlloc);
            ViewCtor(v, &frame, empty.buf, kViewResize, kViewFlags);
            ViewSetResizeMask(v, kViewResize);
            if (outer)
                ViewAddChild(outer, v, false);
            else
                WindowAddChild(s.window, v, false);
            s.chain[i] = v;
            outer = v;
        }
        ViewCtor(s.view, &frame, empty.buf, kViewResize, kViewFlags);
        ViewSetResizeMask(s.view, kViewResize);
        if (outer)
            ViewAddChild(outer, s.view, false);
        else
            WindowAddChild(s.window, s.view, false);
        ViewAddSurface(s.view, s.surface, true);
        ApplySize(s);
        s.x = s.y = -1;
        s.drawn.clear();
        Log("[%s] %s window made (%dx%d px)", g_who, s.what, w, h);
        return true;
    }
    if (w != s.w || h != s.h || std::fabs(s.gw - w * k) > 0.01f) {
        s.w = w;
        s.h = h;
        s.gw = w * k;
        s.gh = h * k;
        ApplySize(s);
        s.drawn.clear();
    }
    return true;
}

void Check(void* wc, Surface& s)
{
    if (s.window && (!InWindowList(wc, s.window) || *static_cast<void**>(s.window) != s.windowVtable)) {
        Log("[%s] the %s window was removed by the game", g_who, s.what);
        Forget(s);
    }
}

void Forget(Surface& s)
{
    s.window = s.view = s.surface = s.sprite = nullptr;
    for (void*& v : s.chain)
        v = nullptr;
    s.windowVtable = nullptr;
    s.w = s.h = 0;
    s.x = s.y = -1;
    s.drawn.clear();
}

void MoveTo(Surface& s, float x, float y)
{
    if (!s.window || (x == s.x && y == s.y))
        return;
    s.x = x;
    s.y = y;
    WindowMove(s.window, x, y);
}

void Show(Surface& s, bool show)
{
    if (s.window && WindowIsVisible(s.window) != show)
        WindowShow(s.window, show);
}

void Upload(Surface& s, const gfx::Canvas& c, const RECT* part)
{
    if (!s.window)
        return;
    uint8_t* data = SpriteData(s.sprite);
    const int stride = SpriteStride(s.sprite);
    const int format = SpriteSurfaceFormat(s.sprite);
    if (!data)
        return;
    const int cw = std::min({c.w, s.texW, s.w}), chh = std::min({c.h, s.texH, s.h});
    RECT r{0, 0, cw, chh};
    if (part) {
        r = {std::max(0L, part->left), std::max(0L, part->top), std::min<LONG>(cw, part->right), std::min<LONG>(chh, part->bottom)};
        if (r.right <= r.left || r.bottom <= r.top)
            return;
    }
    for (int y = r.top; y < r.bottom; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * stride;
        for (int x = r.left; x < r.right; ++x) {
            uint32_t argb = 0;
            const float* p = &c.px[(static_cast<size_t>(y) * c.w + x) * 4];
            const float a = p[3];
            if (a > 0.0f) {
                auto ch = [&](float v) { return static_cast<uint32_t>(std::clamp(v / a, 0.0f, 1.0f) * 255.0f + 0.5f); };
                argb = (static_cast<uint32_t>(std::min(a, 1.0f) * 255.0f + 0.5f) << 24) | (ch(p[0]) << 16) | (ch(p[1]) << 8) | ch(p[2]);
            }
            if (format == kSfA8R8G8B8) {
                memcpy(row + x * 4, &argb, 4);
            } else if (format == kSfA1R5G5B5) {
                const uint16_t v = static_cast<uint16_t>(((argb >> 31) << 15) | (((argb >> 19) & 0x1F) << 10) | (((argb >> 11) & 0x1F) << 5) |
                                                         ((argb >> 3) & 0x1F));
                memcpy(row + x * 2, &v, 2);
            } else {
                const uint16_t v = static_cast<uint16_t>(((argb >> 28) << 12) | (((argb >> 20) & 0xF) << 8) | (((argb >> 12) & 0xF) << 4) |
                                                         ((argb >> 4) & 0xF));
                memcpy(row + x * 2, &v, 2);
            }
        }
    }
    const Rect src{static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - 1), static_cast<float>(r.bottom - 1)};
    const Point at{static_cast<float>(r.left), static_cast<float>(r.top)};
    SurfaceUpdate(s.surface, s.sprite, &src, &at);
    if (!part) {
        const Rect all{0.0f, 0.0f, static_cast<float>(cw - 1), static_cast<float>(chh - 1)};
        const Rect dst{0.0f, 0.0f, s.gw - 1.0f, s.gh - 1.0f};
        SurfaceSrc(s.surface, &all);
        SurfaceDst(s.surface, &dst);
    }
}

namespace {

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

} // namespace

std::vector<uint32_t> SkinProfile(int id)
{
    void* mgr = GuiResources && GuiTexture && ReleaseTexture && SpriteWidth && SpriteHeight ? GuiResources() : nullptr;
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
    Log("[%s] skin image 0x%02X: %dx%d, format %d, %d colours across", g_who, id, w, h, format, static_cast<int>(rows.size()));
    ReleaseTexture(mgr, image);
    return rows;
}

uint32_t SkinColour(int id, uint32_t fallback)
{
    void* mgr = GuiResources && GuiTexture && ReleaseTexture && SpriteWidth && SpriteHeight ? GuiResources() : nullptr;
    void* image = mgr ? GuiTexture(mgr, id, kSpriteFormatGui) : nullptr;
    if (!image)
        return fallback;
    const int w = SpriteWidth(image), h = SpriteHeight(image), stride = SpriteStride(image), bpp = SpriteBpp(image);
    const int format = SpriteSurfaceFormat(image);
    const uint8_t* data = SpriteData(image);
    double r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; data && y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t rgb;
            float a;
            if (!DecodePixel(data + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * bpp, format, &rgb, &a) || a <= 0.0f)
                continue;
            const int cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
            const double wgt = a * (1.0 + std::max({cr, cg, cb}) - std::min({cr, cg, cb})); // colourful pixels count more
            r += cr * wgt, g += cg * wgt, b += cb * wgt, n += wgt;
        }
    ReleaseTexture(mgr, image);
    if (n <= 0.0)
        return fallback;
    const uint32_t rgb = (static_cast<uint32_t>(r / n + 0.5) << 16) | (static_cast<uint32_t>(g / n + 0.5) << 8) | static_cast<uint32_t>(b / n + 0.5);
    Log("[%s] skin image 0x%02X: %dx%d, colour %06X", g_who, id, w, h, rgb);
    return rgb;
}

void RefreshToolTip()
{
    using UpdateFn = void(__thiscall*)(void* self, bool hide);
    static UpdateFn update = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE m = GetModuleHandleA("GUI.dll"))
            update = reinterpret_cast<UpdateFn>(GetProcAddress(m, "?UpdateToolTip@WindowController_c@@AAEX_N@Z"));
        if (!update)
            Log("[%s] missing export GUI.dll!WindowController_c::UpdateToolTip", g_who);
    }
    if (void* wc = update ? WindowController() : nullptr)
        update(wc, false);
}

bool ShowInfoUrl(const std::string& url)
{
    using InstanceFn = void*(__cdecl*)();
    using ShowUrlFn = void(__thiscall*)(void* self, const GameString* url);
    static InstanceFn instance = nullptr;
    static ShowUrlFn show = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE m = GetModuleHandleA("GUI.dll")) {
            instance = reinterpret_cast<InstanceFn>(GetProcAddress(m, "?GetInstance@InfoViewModule_c@@SAPAV1@XZ"));
            show = reinterpret_cast<ShowUrlFn>(GetProcAddress(
                m, "?ShowURL@InfoViewModule_c@@QAEXABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"));
        }
        if (!instance || !show)
            Log("[%s] missing export GUI.dll!InfoViewModule_c::GetInstance / ShowURL", g_who);
    }
    void* self = instance && show ? instance() : nullptr;
    if (!self)
        return false;
    const GameString u = Borrow(url);
    show(self, &u);
    return true;
}

} // namespace prk::gui
