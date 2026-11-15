// The buff frame: your running nanos (the game's NCU list) as the game's own icons in a Clarity window,
// buffs then debuffs (hostile nanos), each bordered in its kind's colour, with the time left under it and the
// NCU window's hover text. Right-click a buff to cancel it; shift-click a nano for its description. Unlocked, drag it to move it and its bottom-right corner to
// resize it; the icons flow in the area, from the top left or (RightToLeft) the top right.
//
// The game's side (GUI, worked out from the NCU window, NCUView_c 0x100D6DD6 / NCUListView_c):
//   - N3Msg_GetNanoTemplateInfoList(): your nanos, a VS2010 std::list of NanoTemplateInfo_c (its Identity, the
//     nano, at +0 of each node's value, node +8); refreshed by the list each update (0x1004502A);
//   - nano stat 0: 0x1000000 = not listed (0x10044EC2), 0x8000 = hostile (0x1003EACB: "Hostile", red);
//     else N3Msg_IsTeamNano "Team", N3Msg_IsNanoSelfOnly "Self", else "Friendly" (0x1003CBCB); the kind
//     (0x1003EACB: 1 friendly, 2 hostile, 3 self, 4 team) picks the overlay the NCU window lays over the
//     icon (0x1003E7C0, its table at 0x10264B74: GUI images 0x1DF, 0x1E0, 0x1E1, 0x1E3), whose colour the
//     border takes;
//   - the icon: nano stat 183, else GUI image 0xB7 (0x1003EB30); ViewSurface_c::Load(id, 0x80) makes it
//     (0x80: an RDB icon; without it, a GUI skin image - a wrong id crashes), as the item icon views
//     do (0x1003FA39);
//   - N3Msg_GetBuffCurrentTime / GetBuffTotalTime(nano, (0, 0) = you): hundredths of a second;
//   - the hover text (0x1004518B): the name; the kind, "NCU usage" (nano stat 54), "Remaining hh:mm:ss",
//     "Modifies:" + N3Msg_GetModifierString;
//   - N3Msg_RemoveBuff(nano): cancels it (not hostile ones);
//   - "Look at nano" in its menu (0x100448D4): InfoViewModule_c::ShowURL("itemid://<type>/<instance>").
// The NCU window's opener, InventoryGUIModule_c::SlotNCUViewActivated (the "ncu_window" module's handler,
// exported), is hooked: while the frame is on, it shows and hides the frame instead ([Buffs] Shown).
#include "buffs.h"

#include "buff_paint.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/gui.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/n3.h"
#include "../../core/perf.h"
#include "../../core/wndproc.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace prk::buffs {

namespace {

namespace bp = paint;
using gfx::Canvas;

// --- the game ---------------------------------------------------------------------------------

struct GameString { // VS2010 std::string (the game's CRT)
    union {
        char buf[16];
        char* ptr;
    };
    uint32_t size;
    uint32_t cap;
};

using InstanceFn = void*(__cdecl*)();
using ListFn = void*(__thiscall*)(const void* self);
using BuffTimeFn = int(__thiscall*)(const void* self, const game::Identity& nano, const game::Identity& target);
using IdBoolFn = bool(__thiscall*)(const void* self, const game::Identity& nano);
using ModifiersFn = GameString*(__thiscall*)(const void* self, GameString* ret, const game::Identity& nano, int, const game::Identity& other,
                                             bool);
using CrtDeleteFn = void(__cdecl*)(void*);
using NcuToggleFn = void(__thiscall*)(void* self, void* module, bool open);

InstanceFn N3Instance;
ListFn NanoList;
BuffTimeFn BuffLeft, BuffTotal;
IdBoolFn RemoveBuff, IsTeamNano, IsSelfOnly;
ModifiersFn Modifiers;
CrtDeleteFn CrtDelete;
NcuToggleFn NcuToggleOriginal;

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[buffs] missing export %s!%s", module, name);
    return fn != nullptr;
}

constexpr int kStatFlags = 0, kStatIcon = 183, kStatNcuUsage = 54;
constexpr uint32_t kNotListed = 0x1000000, kHostile = 0x8000;
constexpr int kDefaultIcon = 0xB7;
constexpr game::Identity kYou{0, 0};

// The text of a game std::string, which is then freed with the game's CRT.
std::string Take(GameString& s)
{
    std::string out;
    if (s.size < 0x10000) {
        const char* chars = s.cap >= 16 ? s.ptr : s.buf;
        if (chars)
            out.assign(chars, s.size);
    }
    if (s.cap >= 16 && s.ptr && CrtDelete)
        CrtDelete(s.ptr);
    return out;
}

// The NCU window's kinds (0x1003EACB) and the colour of each one's overlay (0x1003E7C0).
enum Kind { kFriendly = 1, kHostileKind = 2, kSelf = 3, kTeam = 4 };
constexpr int kKindImage[5] = {0, 0x1DF, 0x1E0, 0x1E1, 0x1E3};
constexpr uint32_t kKindFallback[5] = {0xC8CDD2, 0xC8CDD2, 0xD8402F, 0xE0B83A, 0x58C85A}; // white, red, gold, green
uint32_t g_kindRgb[5] = {};
bool g_kindRgbRead = false;

uint32_t KindRgb(int kind)
{
    if (!g_kindRgbRead) {
        g_kindRgbRead = true;
        for (int k = 1; k <= 4; ++k)
            g_kindRgb[k] = gui::SkinColour(kKindImage[k], kKindFallback[k]);
    }
    return kind >= 1 && kind <= 4 ? g_kindRgb[kind] : kKindFallback[0];
}

struct Nano {
    game::Identity id;
    bool hostile = false;
    int kind = kFriendly;
    int left = 0, total = 0; // hundredths of a second
    int icon = kDefaultIcon;
    bool rdbIcon = false; // icon is an RDB icon id (else the GUI image kDefaultIcon)
};

// Your nanos, as the NCU window lists them.
std::vector<Nano> ReadNanos()
{
    std::vector<Nano> out;
    void* n3 = N3Instance ? N3Instance() : nullptr;
    void* list = n3 ? NanoList(n3) : nullptr;
    void* head = list ? *static_cast<void**>(list) : nullptr;
    if (!head)
        return out;
    int guard = 0;
    for (void* node = *static_cast<void**>(head); node && node != head && guard < 512; node = *static_cast<void**>(node), ++guard) {
        Nano n;
        n.id = *reinterpret_cast<const game::Identity*>(static_cast<const uint8_t*>(node) + 8);
        const int flags = n3::GetSkill(n.id, kStatFlags);
        if (flags != n3::kNoValue && (flags & kNotListed))
            continue;
        n.hostile = flags != n3::kNoValue && (flags & kHostile);
        n.kind = n.hostile ? kHostileKind
                 : IsTeamNano && IsTeamNano(n3, n.id) ? kTeam
                 : IsSelfOnly && IsSelfOnly(n3, n.id) ? kSelf
                                                      : kFriendly;
        n.left = BuffLeft(n3, n.id, kYou);
        n.total = BuffTotal(n3, n.id, kYou);
        const int icon = n3::GetSkill(n.id, kStatIcon);
        n.rdbIcon = icon != n3::kNoValue && icon != 0 && icon != -1;
        n.icon = n.rdbIcon ? icon : kDefaultIcon;
        out.push_back(n);
    }
    return out;
}

uint64_t Key(const game::Identity& id)
{
    return (static_cast<uint64_t>(id.type) << 32) | id.instance;
}

// The NCU window's hover text: the kind, the NCU it takes, the time left and what it modifies.
std::unordered_map<uint64_t, std::string> g_modifiers; // per nano, read once

void ToolTip(const Nano& n, std::string* title, std::string* text)
{
    *title = n3::Name(n.id);
    switch (n.kind) {
    case kHostileKind: *text = "<font color=\"red\">Hostile</font>"; break;
    case kTeam: *text = "<font color=\"green\">Team</font>"; break;
    case kSelf: *text = "<font color=\"yellow\">Self</font>"; break;
    default: *text = "<font color=\"white\">Friendly</font>"; break;
    }
    void* n3 = N3Instance();
    char line[96];
    const int ncu = n3::GetSkill(n.id, kStatNcuUsage);
    sprintf_s(line, "<br>NCU usage:&nbsp;%d", ncu == n3::kNoValue ? 0 : ncu);
    *text += line;
    const int s = std::max(0, n.left / 100), m = s / 60;
    sprintf_s(line, "<br>Remaining:&nbsp;%02d:%02d:%02d", m / 60, m % 60, s % 60);
    *text += line;
    auto it = g_modifiers.find(Key(n.id));
    if (it == g_modifiers.end()) {
        std::string mods;
        if (Modifiers) {
            GameString ret{};
            Modifiers(n3, &ret, n.id, 0, kYou, false);
            mods = Take(ret);
        }
        if (g_modifiers.size() > 256)
            g_modifiers.clear();
        it = g_modifiers.emplace(Key(n.id), std::move(mods)).first;
    }
    if (!it->second.empty())
        *text += "<br>Modifies:<br>" + it->second;
}

// --- the frame --------------------------------------------------------------------------------

struct Cell {
    void* view = nullptr;
    void* image = nullptr;   // the icon: for a nano, its normal (left) half, over the time left
    void* dimmed = nullptr;  // a nano's dimmed (right) half, over the time gone, as the NCU window
    gui::Rect source{};      // the whole texture (both halves)
    int fill = -1;           // pixels of the icon in the normal half (what's drawn now)
    int icon = 0;
    bool rdbIcon = false;
    bool shown = false;
    gui::Rect frame{};
    game::Identity id{};
    std::string title, text;
};

constexpr int kMaxCells = 64;
gui::Surface g_frame{"buff frame", 2048, 1024, gui::kNormalFlags};
Cell g_cells[kMaxCells];
void* g_cellsWindow = nullptr; // the window the cells' views are in (a new window: new views)
void* g_wc = nullptr;
bool g_disabled = false;
bool g_shown = true;           // the NCU button's state ([Buffs] Shown)
bool g_shownRead = false;
float g_x = -1, g_y = -1;      // top-left, GUI units
int g_w = 0, g_h = 0;          // size, pixels
float g_cfgSeen[4] = {-2, -2, -2, -2};
bp::Metrics g_metrics;
int g_iconW = 0, g_iconH = 0;  // the game's icon size (pixels), measured on the first icon loaded; 0 = not yet
bp::Layout g_layout;
std::vector<Nano> g_placed;    // per cell, the nano in it

// The game's own NCU window: what its opener was last asked, and with what, to put it back.
void* g_ncuSelf = nullptr;
void* g_ncuModule = nullptr;
bool g_ncuWanted = false;      // the button's state for the game's window
bool g_ncuGameOpen = false;    // the game's window is open (the original opener was told so last)

void ForgetCells()
{
    for (Cell& c : g_cells)
        c = Cell{};
}

void HideCells(int from)
{
    for (int i = from; i < kMaxCells; ++i) {
        Cell& c = g_cells[i];
        if (c.view && c.shown) {
            gui::ViewShow(c.view, false);
            c.shown = false;
        }
    }
}

void SavePlace()
{
    wchar_t v[32];
    const std::wstring& ini = ConfigPath();
    swprintf_s(v, L"%.0f", g_x);
    WritePrivateProfileStringW(L"Buffs", L"X", v, ini.c_str());
    swprintf_s(v, L"%.0f", g_y);
    WritePrivateProfileStringW(L"Buffs", L"Y", v, ini.c_str());
    swprintf_s(v, L"%d", g_w);
    WritePrivateProfileStringW(L"Buffs", L"W", v, ini.c_str());
    swprintf_s(v, L"%d", g_h);
    WritePrivateProfileStringW(L"Buffs", L"H", v, ini.c_str());
    const BuffsConfig& cfg = Cfg().buffs;
    g_cfgSeen[0] = cfg.x, g_cfgSeen[1] = cfg.y, g_cfgSeen[2] = cfg.w, g_cfgSeen[3] = cfg.h; // the old ones aren't a change
}

void SaveShown()
{
    WritePrivateProfileStringW(L"Buffs", L"Shown", g_shown ? L"1" : L"0", ConfigPath().c_str());
}

// The game's NCU window opened or closed as `open` says (through the original opener).
void GameNcu(bool open)
{
    if (!NcuToggleOriginal || !g_ncuSelf || open == g_ncuGameOpen)
        return;
    g_ncuGameOpen = open;
    NcuToggleOriginal(g_ncuSelf, g_ncuModule, open);
}

void __fastcall NcuToggle(void* self, void* /*edx*/, void* module, bool open)
{
    g_ncuSelf = self;
    g_ncuModule = module;
    g_ncuWanted = open;
    if (Cfg().buffs.enable && !g_disabled) {
        g_shown = open;
        g_shownRead = true;
        SaveShown();
        GameNcu(false);
        return;
    }
    g_ncuGameOpen = open;
    NcuToggleOriginal(self, module, open);
}

void UpdatePlace(const gui::Point& screen, float k)
{
    const BuffsConfig& cfg = Cfg().buffs;
    if (cfg.x == g_cfgSeen[0] && cfg.y == g_cfgSeen[1] && cfg.w == g_cfgSeen[2] && cfg.h == g_cfgSeen[3] && g_w > 0)
        return;
    g_cfgSeen[0] = cfg.x, g_cfgSeen[1] = cfg.y, g_cfgSeen[2] = cfg.w, g_cfgSeen[3] = cfg.h;
    g_w = cfg.w > 0 ? static_cast<int>(cfg.w) : 2 * g_metrics.pad + 10 * g_metrics.CellW() - g_metrics.gap;
    g_h = cfg.h > 0 ? static_cast<int>(cfg.h) : 2 * g_metrics.pad + 3 * g_metrics.CellH() - g_metrics.gap + g_metrics.rowGap;
    if (cfg.x >= 0 && cfg.y >= 0) {
        g_x = cfg.x;
        g_y = cfg.y;
    } else { // the upper middle of the screen
        g_x = std::round((screen.x - g_w * k) * 0.5f);
        g_y = std::round(screen.y * 0.12f);
    }
}

void Update(HWND hwnd)
{
    void* wc = gui::WindowController();
    if (wc != g_wc) {
        gui::Forget(g_frame);
        ForgetCells();
        g_wc = wc;
    }
    if (!wc)
        return;
    gui::Check(wc, g_frame);
    if (g_frame.window != g_cellsWindow) { // a new window (or none): its views are new
        ForgetCells();
        g_cellsWindow = g_frame.window;
    }
    const BuffsConfig& cfg = Cfg().buffs;
    if (!g_shownRead) {
        g_shown = cfg.shown;
        g_shownRead = true;
    }
    // Turned on while the game's NCU window was open: it closes (the frame takes its place); turned off: the
    // game's window comes back if its button says so.
    if (cfg.enable && !g_disabled)
        GameNcu(false);
    else
        GameNcu(g_ncuWanted);
    if (!cfg.enable || g_disabled || !g_shown || !gui::InGame()) {
        gui::Show(g_frame, false);
        return;
    }

    gui::Point screen{};
    const float k = gui::GuiPerPx(hwnd, &screen);
    // The icons at the game's own size, a texel a pixel (an icon's texture, one half of a nano's: 40 x 24):
    // scaled, they look bad. Until one is loaded, 40 x 24.
    g_metrics = bp::MetricsFor(g_iconW > 0 ? g_iconW : 40, g_iconH > 0 ? g_iconH : 24);
    if (!bp::EnsureFonts(1.0f)) {
        Log("[buffs] font creation failed - off");
        g_disabled = true;
        return;
    }
    UpdatePlace(screen, k);

    std::vector<Nano> nanos = ReadNanos();
    std::vector<Nano> buffs, debuffs;
    for (const Nano& n : nanos)
        (n.hostile ? debuffs : buffs).push_back(n);
    // By time left, the shortest first, as the NCU window sorts them (its time-left column, 0x100D6DD6);
    // nanos with no end last.
    auto byTimeLeft = [](const Nano& a, const Nano& b) {
        const long long ta = a.left > 0 ? a.left : LLONG_MAX, tb = b.left > 0 ? b.left : LLONG_MAX;
        return ta < tb;
    };
    std::stable_sort(buffs.begin(), buffs.end(), byTimeLeft);
    std::stable_sort(debuffs.begin(), debuffs.end(), byTimeLeft);
    g_layout = bp::Arrange(g_metrics, g_w, g_h, static_cast<int>(buffs.size()), static_cast<int>(debuffs.size()), cfg.rightToLeft);
    g_w = g_layout.w;
    g_h = g_layout.h;
    if (!gui::Ensure(g_frame, g_w, g_h, k)) {
        g_disabled = true;
        return;
    }
    if (g_frame.window != g_cellsWindow) {
        ForgetCells();
        g_cellsWindow = g_frame.window;
    }
    g_x = std::clamp(g_x, 0.0f, std::max(0.0f, screen.x - g_frame.gw));
    g_y = std::clamp(g_y, 0.0f, std::max(0.0f, screen.y - g_frame.gh));
    gui::MoveTo(g_frame, g_x, g_y);
    gui::SetOpacity(g_frame, cfg.opacity);

    // The painted part: borders, times, (unlocked) the panel.
    auto toPaint = [](const Nano& n) {
        bp::Buff b;
        b.hostile = n.hostile;
        b.rgb = KindRgb(n.kind);
        b.time = bp::TimeLeft(n.left);
        b.expiring = n.left > 0 && n.left < 1000;
        return b;
    };
    std::vector<bp::Buff> pb, pd;
    for (const Nano& n : buffs)
        pb.push_back(toPaint(n));
    for (const Nano& n : debuffs)
        pd.push_back(toPaint(n));
    std::string state;
    char head[64];
    sprintf_s(head, "%d|%d|%dx%d|%c|", g_w, g_h, g_metrics.iconW, g_metrics.iconH, cfg.locked ? 'L' : 'U');
    state = head;
    for (const auto* at : {&g_layout.buffAt, &g_layout.debuffAt}) // where the borders go (the direction moves them)
        for (const POINT& p : *at) {
            sprintf_s(head, "%ld,%ld;", p.x, p.y);
            state += head;
        }
    for (const auto* list : {&pb, &pd})
        for (const bp::Buff& b : *list) {
            char rgb[8];
            sprintf_s(rgb, "%06X", b.rgb);
            state += rgb;
            state += b.expiring ? '!' : '.';
            for (wchar_t ch : b.time)
                state += static_cast<char>(ch);
            state += '|';
        }
    if (state != g_frame.drawn) {
        perf::Slow slow("buffs: paint", 0, 2.0);
        Canvas c;
        c.Reset(g_frame.w, g_frame.h);
        bp::Paint(c, g_metrics, g_layout, pb, pd, !cfg.locked);
        gui::Upload(g_frame, c);
        g_frame.drawn = state;
    }
    gui::Show(g_frame, true);

    // The icons: the game's own, one child view per cell.
    perf::Slow slowIcons("buffs: icons", 0, 2.0);
    // The mouse in the frame (pixels), if it's over it: only the hovered cell's tooltip follows the time
    // left ("Remaining" changes every second, and setting ten tooltips a second cost ~9 ms); the others
    // are brought up to date when the mouse comes onto them, before the tooltip shows.
    int mx = -1, my = -1;
    {
        POINT pt{};
        if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && gui::MouseWindow(g_wc) == g_frame.window) {
            mx = static_cast<int>((pt.x * k - g_frame.x) / k);
            my = static_cast<int>((pt.y * k - g_frame.y) / k);
        }
    }
    g_placed.clear();
    int used = 0;
    auto place = [&](const std::vector<Nano>& list, const std::vector<POINT>& at) {
        for (size_t i = 0; i < at.size() && i < list.size() && used < kMaxCells; ++i, ++used) {
            const Nano& n = list[i];
            Cell& cell = g_cells[used];
            const float s = g_metrics.iconW * k, sh = g_metrics.iconH * k; // s: across (the time-left cut is along it)
            const gui::Rect frame{at[i].x * k, at[i].y * k, at[i].x * k + s - 1.0f, at[i].y * k + sh - 1.0f};
            if (!cell.view) {
                cell.view = gui::MakeChildView(g_frame.view, frame);
                if (!cell.view)
                    continue;
                cell.frame = frame;
            } else if (std::memcmp(&frame, &cell.frame, sizeof frame) != 0) {
                gui::SetViewFrame(cell.view, frame);
                cell.frame = frame;
                cell.fill = -1; // its size may have changed: the halves are placed again
            }
            if (cell.icon != n.icon || cell.rdbIcon != n.rdbIcon || !cell.image) {
                gui::DestroyImage(cell.view, cell.image);
                gui::DestroyImage(cell.view, cell.dimmed);
                cell.image = gui::LoadImage(n.icon, n.rdbIcon);
                cell.dimmed = nullptr;
                cell.icon = n.icon;
                cell.rdbIcon = n.rdbIcon;
                cell.fill = -1;
                if (cell.image) {
                    gui::AttachImage(cell.view, cell.image, gui::Rect{0.0f, 0.0f, s - 1.0f, sh - 1.0f});
                    cell.source = gui::ImageSource(cell.image);
                    if (g_iconW == 0) {
                        const float tw = cell.source.right - cell.source.left + 1.0f, th = cell.source.bottom - cell.source.top + 1.0f;
                        g_iconW = static_cast<int>(std::lround(std::floor(tw * 0.5f)));
                        g_iconH = static_cast<int>(std::lround(th));
                        if (cfg.debug)
                            Log("[buffs] the game's icons are %dx%d (texture %.0fx%.0f)", g_iconW, g_iconH, tw, th);
                    }
                    // A nano's icon is always two halves, the default one (GUI image 0xB7) too: the NCU window
                    // splits every nano's (ItemIconView_c 0x1003FA39 clones the surface for a nano whatever it is).
                    cell.dimmed = gui::LoadImage(n.icon, n.rdbIcon);
                    if (cell.dimmed)
                        gui::AttachImage(cell.view, cell.dimmed, gui::Rect{0.0f, 0.0f, s - 1.0f, sh - 1.0f});
                }
            }
            // The time left as the NCU window shows it (ItemIconView_c GUI 0x1003EFE6): the normal half from the
            // left over the time left, the dimmed half over the rest. A nano with no end stays whole.
            if (cell.image) {
                const gui::Rect& src = cell.source;
                const float tw = src.right - src.left + 1.0f;
                const float half = std::floor(tw * 0.5f); // the normal half, even if the dimmed copy didn't load
                const float f = n.total > 0 && n.left > 0 ? std::clamp(static_cast<float>(n.left) / n.total, 0.0f, 1.0f) : 1.0f;
                const int fill = cell.dimmed ? static_cast<int>(std::floor(s * f)) : static_cast<int>(s);
                if (fill != cell.fill) {
                    cell.fill = fill;
                    const float fw = static_cast<float>(fill), cut = half * fw / s; // the cut, in the texture
                    if (fill > 0) {
                        gui::SetImageFrames(cell.image, gui::Rect{src.left, src.top, src.left + cut - 1.0f, src.bottom},
                                            gui::Rect{0.0f, 0.0f, fw - 1.0f, sh - 1.0f});
                        gui::SetImageAlpha(cell.image, 1.0f);
                    } else {
                        gui::SetImageAlpha(cell.image, 0.0f);
                    }
                    if (cell.dimmed) {
                        if (fill < static_cast<int>(s)) {
                            gui::SetImageFrames(cell.dimmed, gui::Rect{src.left + half + cut, src.top, src.left + 2.0f * half - 1.0f, src.bottom},
                                                gui::Rect{fw, 0.0f, s - 1.0f, sh - 1.0f});
                            gui::SetImageAlpha(cell.dimmed, 1.0f);
                        } else {
                            gui::SetImageAlpha(cell.dimmed, 0.0f);
                        }
                    }
                }
            }
            const bool over = mx >= at[i].x - g_metrics.border && my >= at[i].y - g_metrics.border &&
                              mx < at[i].x + g_metrics.iconW + g_metrics.border && my < at[i].y + g_metrics.iconH + g_metrics.border;
            if (over || cell.title.empty() || Key(cell.id) != Key(n.id)) {
                std::string title, text;
                ToolTip(n, &title, &text);
                if (title != cell.title || text != cell.text || Key(cell.id) != Key(n.id)) {
                    gui::SetToolTip(cell.view, title, text);
                    cell.title = title;
                    cell.text = text;
                    if (over)
                        gui::RefreshToolTip(); // the one showing counts down too
                }
            }
            cell.id = n.id;
            if (!cell.shown) {
                gui::ViewShow(cell.view, true);
                cell.shown = true;
            }
            g_placed.push_back(n);
        }
    };
    place(buffs, g_layout.buffAt);
    place(debuffs, g_layout.debuffAt);
    HideCells(used);
    gui::KeepSize(g_frame); // the new cells' views made the GUI refit (shrink) the window
    // The refit also stretches the cells' views (to the frame's size plus their own: 389 x 202 for a 40 x 24
    // icon). A view's bounds are where the GUI looks for its tooltip (GUI 0x10156D66: the first child whose
    // bounds hold the mouse answers), so a stretched cell took its neighbours' tooltips: each back to its icon.
    {
        int fixed = 0;
        for (int i = 0; i < used; ++i) {
            Cell& cell = g_cells[i];
            if (!cell.view)
                continue;
            const gui::Point size = gui::ViewSize(cell.view);
            const float w = cell.frame.right - cell.frame.left + 1.0f, h = cell.frame.bottom - cell.frame.top + 1.0f;
            if (std::fabs(size.x - w) > 0.5f || std::fabs(size.y - h) > 0.5f) {
                gui::SetViewFrame(cell.view, cell.frame);
                ++fixed;
            }
        }
        if (fixed) {
            gui::KeepSize(g_frame);
            static bool logged = false;
            if (cfg.debug && !logged) {
                logged = true;
                Log("[buffs] %d cell views were stretched by the GUI's refit - set back to their icons", fixed);
            }
        }
    }

    static DWORD lastLog = 0;
    const DWORD now = GetTickCount();
    if (cfg.debug && now - lastLog >= 5000) {
        lastLog = now;
        const gui::Rect f = gui::WindowFrame(g_frame.window);
        Log("[buffs] %d nanos (%d buffs, %d debuffs), %d shown; frame at (%.0f, %.0f) %dx%d px; window %s, (%.0f, %.0f)-(%.0f, %.0f), screen %.0fx%.0f",
            static_cast<int>(nanos.size()), static_cast<int>(buffs.size()), static_cast<int>(debuffs.size()), used, g_x, g_y, g_w, g_h,
            gui::WindowVisible(g_frame.window) ? "visible" : "HIDDEN", f.left, f.top, f.right, f.bottom, screen.x, screen.y);
    }
}

// --- the game window: timer, dragging, resizing, cancelling -------------------------------------

constexpr UINT_PTR kTimerId = 0xC1AC;
constexpr UINT kTickMs = 100;
bool g_timerSet = false;
enum class Drag { None, Move, Resize } g_drag = Drag::None;
POINT g_dragMouse{};
float g_dragX = 0, g_dragY = 0;
int g_dragW = 0, g_dragH = 0;
bool g_swallowRightUp = false;
bool g_swallowLeftUp = false;

// The mouse over the frame (no other window over it there): its point in the frame, pixels.
bool OverFrame(HWND hwnd, LPARAM lp, int* px, int* py)
{
    if (!g_wc || !g_frame.window || !gui::WindowVisible(g_frame.window) || gui::MouseWindow(g_wc) != g_frame.window)
        return false;
    gui::Point screen{};
    const float k = gui::GuiPerPx(hwnd, &screen);
    *px = static_cast<int>((static_cast<short>(LOWORD(lp)) * k - g_frame.x) / k);
    *py = static_cast<int>((static_cast<short>(HIWORD(lp)) * k - g_frame.y) / k);
    return true;
}

// The nano under (px, py) in the frame (pixels), or null.
const Nano* NanoAt(int px, int py)
{
    size_t i = 0;
    for (const auto* at : {&g_layout.buffAt, &g_layout.debuffAt})
        for (const POINT& p : *at) {
            if (i < g_placed.size() && px >= p.x && py >= p.y && px < p.x + g_metrics.iconW && py < p.y + g_metrics.iconH)
                return &g_placed[i];
            ++i;
        }
    return nullptr;
}

void EndDrag()
{
    if (g_drag == Drag::None)
        return;
    g_drag = Drag::None;
    if (GetCapture())
        ReleaseCapture();
    g_x = std::round(g_x);
    g_y = std::round(g_y);
    SavePlace();
}

bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result)
{
    if (!g_timerSet)
        g_timerSet = SetTimer(hwnd, kTimerId, kTickMs, nullptr) != 0;
    const BuffsConfig& cfg = Cfg().buffs;
    switch (msg) {
    case WM_TIMER:
        if (wp != kTimerId)
            return false;
        Update(hwnd);
        *result = 0;
        return true;
    case WM_LBUTTONDOWN: {
        int px, py;
        if (!cfg.enable || g_drag != Drag::None || !OverFrame(hwnd, lp, &px, &py))
            return false;
        // Shift-click: the nano's description, as "Look at nano" in the NCU window's menu.
        if (wp & MK_SHIFT) {
            if (const Nano* n = NanoAt(px, py)) {
                char url[64];
                sprintf_s(url, "itemid://%u/%u", n->id.type, n->id.instance);
                const bool shown = gui::ShowInfoUrl(url);
                if (cfg.debug)
                    Log("[buffs] inspect %s%s", url, shown ? "" : " - failed");
                g_swallowLeftUp = true;
                *result = 0;
                return true;
            }
        }
        if (cfg.locked)
            return false;
        const bool grip = px >= g_frame.w - g_metrics.grip - 2 && py >= g_frame.h - g_metrics.grip - 2;
        g_drag = grip ? Drag::Resize : Drag::Move;
        g_dragMouse = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        g_dragX = g_x, g_dragY = g_y, g_dragW = g_w, g_dragH = g_h;
        SetCapture(hwnd);
        *result = 0;
        return true;
    }
    case WM_MOUSEMOVE: {
        if (g_drag == Drag::None)
            return false;
        gui::Point screen{};
        const float k = gui::GuiPerPx(hwnd, &screen);
        const int dx = static_cast<short>(LOWORD(lp)) - g_dragMouse.x, dy = static_cast<short>(HIWORD(lp)) - g_dragMouse.y;
        if (g_drag == Drag::Move) {
            g_x = std::clamp(g_dragX + dx * k, 0.0f, std::max(0.0f, screen.x - g_frame.gw));
            g_y = std::clamp(g_dragY + dy * k, 0.0f, std::max(0.0f, screen.y - g_frame.gh));
            gui::MoveTo(g_frame, g_x, g_y);
        } else {
            int minW, minH;
            bp::MinSize(g_metrics, &minW, &minH);
            g_w = std::clamp(g_dragW + dx, minW, 2000);
            g_h = std::clamp(g_dragH + dy, minH, 1000);
            Update(hwnd); // the icons flow into the new size at once
        }
        *result = 0;
        return true;
    }
    case WM_LBUTTONUP:
        if (g_swallowLeftUp) {
            g_swallowLeftUp = false;
            *result = 0;
            return true;
        }
        if (g_drag == Drag::None)
            return false;
        EndDrag();
        *result = 0;
        return true;
    case WM_RBUTTONDOWN: {
        int px, py;
        if (!cfg.enable || !OverFrame(hwnd, lp, &px, &py))
            return false;
        // A right-click on a buff cancels it, as the game's NCU window's menu does.
        if (const Nano* n = NanoAt(px, py)) {
            if (!n->hostile && RemoveBuff) {
                const game::Identity id = n->id;
                RemoveBuff(N3Instance(), id);
                if (cfg.debug)
                    Log("[buffs] cancelled %u:%u", id.type, id.instance);
            }
        }
        g_swallowRightUp = true;
        *result = 0;
        return true;
    }
    case WM_RBUTTONUP:
        if (!g_swallowRightUp)
            return false;
        g_swallowRightUp = false;
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
    const char* ifc = "Interfaces.dll";
    bool ok = gui::Init("buffs");
    ok &= Resolve(N3Instance, ifc, "?GetInstance@N3InterfaceModule_t@@SAPAV1@XZ");
    ok &= Resolve(NanoList, ifc,
                  "?N3Msg_GetNanoTemplateInfoList@N3InterfaceModule_t@@QBEPAV?$list@VNanoTemplateInfo_c@@V?$allocator@VNanoTemplateInfo_c@@@std@@@"
                  "std@@XZ");
    ok &= Resolve(BuffLeft, ifc, "?N3Msg_GetBuffCurrentTime@N3InterfaceModule_t@@QBEHABVIdentity_t@@0@Z");
    ok &= Resolve(BuffTotal, ifc, "?N3Msg_GetBuffTotalTime@N3InterfaceModule_t@@QBEHABVIdentity_t@@0@Z");
    if (!ok) {
        Log("[buffs] disabled");
        return false;
    }
    // Optional: cancelling, the hover text's kind and modifiers.
    Resolve(RemoveBuff, ifc, "?N3Msg_RemoveBuff@N3InterfaceModule_t@@QBE_NABVIdentity_t@@@Z");
    Resolve(IsTeamNano, ifc, "?N3Msg_IsTeamNano@N3InterfaceModule_t@@QBE_NABVIdentity_t@@@Z");
    Resolve(IsSelfOnly, ifc, "?N3Msg_IsNanoSelfOnly@N3InterfaceModule_t@@QBE_NABVIdentity_t@@@Z");
    Resolve(Modifiers, ifc,
            "?N3Msg_GetModifierString@N3InterfaceModule_t@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@ABVIdentity_t@@"
            "H0_N@Z");
    Resolve(CrtDelete, "msvcr100.dll", "??3@YAXPAX@Z");
    void* toggle = nullptr;
    if (HMODULE gui = GetModuleHandleA("GUI.dll"))
        toggle = reinterpret_cast<void*>(GetProcAddress(gui, "?SlotNCUViewActivated@InventoryGUIModule_c@@AAEXPAVGUIModuleBase_c@@_N@Z"));
    if (!toggle || !InstallHook("NCU window opener", toggle, &NcuToggle, &NcuToggleOriginal))
        Log("[buffs] the NCU button won't show and hide the frame");
    wndproc::Add(&OnMessage, "buffs");
    Log("[buffs] ready");
    return true;
}

} // namespace prk::buffs
