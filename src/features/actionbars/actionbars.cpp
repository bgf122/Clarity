// Action bars: the game's own shortcut bars, restyled. The game keeps doing everything a slot does
// (icons, drag and drop, use, cooldowns, tooltips, rows); Clarity only moves the bar windows, hides
// their handles and slot backgrounds, and paints round them:
//
//   - the dock: a frame (a window behind the others) with the first [ActionBars] Docked bars set into it,
//     side by side (two to a line, the first two at the bottom), each with a page control on its outer end
//     (the row it shows; a click on its upper / lower half steps it on / back), and the XP and alien XP
//     bars across the top. The docked bars' windows are moved each tick so their slots sit on the frame's;
//   - the other bars float where the game has them: a backing window with the slots' bezels behind each.
//     Locked, their handles (active-bar button, row spinner) are hidden; unlocked, they show, and the game
//     moves the bar by them;
//   - every bar gets a window of key labels just above it (the mouse goes through it).
//
// The game's shortcut bars (GUI 0x100D9A81 ShortcutBarContainer makes NumHotbars of them):
//   ShortcutBarWindow_c (Window, 0x100D94E9): +0x78 its ShortcutBarView_c, +0x7C its index.
//   ShortcutBarView_c (0x100D8C12): +0x12C vertical, +0x1D0 the active-bar button (ButtonBase_c, value at
//   +0x180), +0x1D4 the row spinner (row at +0x130), +0x1D8 the MultiListView_c of slots: a 10 x 10 grid, a
//   column per slot and a row per bar row; the view scrolls to the row shown.
//   MultiListView_c: +0x150 feature flags (0x100 = no cell backgrounds), +0x170 the first cell (scrolled
//   view coordinates), +0x180 the icon size, +0x198 the cell size (ints), +0x2A0 the scrolled view.
// Keys: the key-binding manager (SIG_KEYBIND_MANAGER) maps inputs to command hashes; a slot's commands are
// "SHORTCUT_BAR_<bar>_<slot>" and, on the active bar, "SHORTUCT_BAR_ACTIVE_<slot>" (the number keys).
// XP as the game's XP bar (CharacterBar_c kinds 2 / 3, GUI 0x100668AA / 0x100669F7).
#include "actionbars.h"

#include "ab_paint.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/gui.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/n3.h"
#include "../../core/signatures.h"
#include "../../core/wndproc.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace prk::actionbars {

namespace {

namespace ab = paint;
using gfx::Canvas;

// --- the game's shortcut bars -----------------------------------------------------------------

constexpr char kBarClass[] = ".?AVShortcutBarWindow_c@@";
constexpr int kBarView = 0x78, kBarIndex = 0x7C;
constexpr int kViewVertical = 0x12C, kViewActive = 0x1D0, kViewSpinner = 0x1D4, kViewList = 0x1D8;
constexpr int kButtonValue = 0x180, kSpinnerRow = 0x130;
constexpr int kListOrigin = 0x170, kListIcon = 0x180, kListCell = 0x198, kListScrolled = 0x2A0;
constexpr unsigned kNoCellBackground = 0x100;

struct IPoint {
    int x, y;
};

using GetItemFn = void*(__thiscall*)(const void* list, const IPoint* pos);
using ItemViewFn = void*(__thiscall*)(void* item);
using FeatureGetFn = unsigned(__thiscall*)(const void* list);
using FeatureSetFn = void(__thiscall*)(void* list, unsigned flags);
using RowSetFn = void(__thiscall*)(void* view, int row);

GetItemFn GetItem;
ItemViewFn ItemView;
FeatureGetFn GetFeatures;
FeatureSetFn SetFeatures;
RowSetFn SetRow;          // ShortcutBarView_c's row setter (SIG_SHORTCUT_ROW)
void** g_bindingsGlobal;  // the key-binding manager's global (SIG_KEYBIND_MANAGER)

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[actionbars] missing export %s!%s", module, name);
    return fn != nullptr;
}

template <typename T>
T At(const void* p, int offset)
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(p) + offset);
}

template <typename T>
T* Ptr(void* p, int offset)
{
    return reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset);
}

// --- keys -------------------------------------------------------------------------------------

// boost::hash_combine over signed chars, as the binding manager hashes command names (GUI 0x1001900A).
uint32_t Hash(const char* s)
{
    uint32_t h = 0;
    for (; *s; ++s)
        h ^= static_cast<uint32_t>(static_cast<int>(static_cast<signed char>(*s))) + 0x9E3779B9u + (h << 6) + (h >> 2);
    return h;
}

// An input as a short label: "1", "s1" (shift), "c1", "a1", "F5", "N3" (numpad), "M4" (mouse button).
std::wstring KeyLabel(uint32_t input)
{
    const uint32_t key = input & 0x1FFFF;
    std::wstring name;
    if (key >= 0x6C && key <= 0x75)
        name = std::to_wstring((key - 0x6C + 1) % 10);
    else if (key >= 0x52 && key <= 0x6B)
        name = std::wstring(1, static_cast<wchar_t>(L'A' + (key - 0x52)));
    else if (key >= 0x27 && key <= 0x3E)
        name = L"F" + std::to_wstring(key - 0x27 + 1);
    else if (key >= 0x46 && key <= 0x4F)
        name = L"N" + std::to_wstring(key - 0x46);
    else {
        switch (key) {
        case 0x09: name = L"M3"; break;
        case 0x79: name = L"M4"; break;
        case 0x7A: name = L"M5"; break;
        case 0x0D: name = L"MW"; break;
        case 0x0F: name = L"Tab"; break;
        case 0x17: name = L"Spc"; break;
        case 0x1A: name = L"Ins"; break;
        case 0x1B: name = L"Del"; break;
        case 0x1C: name = L"Hm"; break;
        case 0x1D: name = L"End"; break;
        case 0x1E: name = L"PU"; break;
        case 0x1F: name = L"PD"; break;
        case 0x24: name = L"|"; break;
        case 0x25: name = L"."; break;
        case 0x26: name = L","; break;
        case 0x42: name = L"N/"; break;
        case 0x43: name = L"N*"; break;
        case 0x44: name = L"N-"; break;
        case 0x45: name = L"N+"; break;
        case 0x50: name = L"N."; break;
        case 0x76: name = L"/"; break;
        default: return {};
        }
    }
    std::wstring mods;
    if (input & 0x20000)
        mods += L's';
    if (input & 0x40000)
        mods += L'c';
    if (input & 0x80000)
        mods += L'a';
    return mods + name;
}

// The binding manager's map: provider -> the inputs bound to it.
std::unordered_map<uint32_t, std::vector<uint32_t>> g_keys;
DWORD g_keysRead = 0;

// The map's nodes (VS2010 std::map<int, std::vector<uint32_t>>): its iterator (GUI 0x100D548D) tests isnil
// at +0x21 - the value is 0x14 bytes.
struct TreeNode {
    TreeNode* left;
    TreeNode* parent;
    TreeNode* right;
    int input;
    uint32_t* first;
    uint32_t* last;
    uint32_t* end;
    uint32_t unused;
    uint8_t colour;
    uint8_t isNil;
};
static_assert(offsetof(TreeNode, isNil) == 0x21, "the binding map's node layout");

void ReadKeys()
{
    g_keys.clear();
    const uint8_t* mgr = g_bindingsGlobal ? static_cast<const uint8_t*>(*g_bindingsGlobal) : nullptr;
    if (!mgr)
        return;
    TreeNode* head = *reinterpret_cast<TreeNode* const*>(mgr + 0x14);
    if (!head || !head->isNil)
        return;
    int guard = 0;
    TreeNode* n = head->left; // the leftmost node
    while (n && n != head && !n->isNil && n->left && n->right && n->parent && guard++ < 4096) {
        const size_t count = static_cast<size_t>(n->last - n->first);
        if (n->first && count < 64)
            for (size_t i = 0; i < count; ++i)
                g_keys[n->first[i]].push_back(static_cast<uint32_t>(n->input));
        // In-order successor.
        if (!n->right->isNil) {
            n = n->right;
            while (n->left && !n->left->isNil)
                n = n->left;
        } else {
            TreeNode* p = n->parent;
            while (p && !p->isNil && n == p->right) {
                n = p;
                p = p->parent;
            }
            n = p;
        }
    }
}

// The shortest label of the inputs bound to a command.
std::wstring KeyFor(const char* command)
{
    auto it = g_keys.find(Hash(command));
    if (it == g_keys.end())
        return {};
    std::wstring best;
    for (uint32_t input : it->second) {
        const std::wstring l = KeyLabel(input);
        if (!l.empty() && (best.empty() || l.size() < best.size()))
            best = l;
    }
    return best;
}

// --- state ------------------------------------------------------------------------------------

constexpr int kTex = 1024;
// The key labels: an ordinary window (the mouse goes through it). What is drawn over what is the GUI's depth
// buffer: the labels are nested in their window deeper than the game nests a slot's icon and its count
// (g_labelNesting), so they're over the bar's icons whatever the windows' order, and not over everything.
constexpr unsigned kKeysFlags = gui::kNormalFlags | gui::kWindowNoMouse;
constexpr unsigned kBackFlags = gui::kWindowFlags | gui::kWindowNoMouse;

struct Bar {
    void* window = nullptr;
    void* view = nullptr;
    void* list = nullptr;
    int index = 0;
    bool vertical = false;
    bool docked = false;

    // What Clarity changed, to put back.
    bool listStyled = false;
    unsigned listFeatures = 0;
    bool chromeHidden = false;
    unsigned chromeFlags[2] = {};
    bool noRaiseSet = false; // Clarity set kWindowNoRaise on the bar's window

    ab::Row row;
    // The first cell (screen, GUI units), the cells' size, the icons' size and place in their cells.
    gui::Point cell0{};
    float cellW = 36, cellH = 36;
    // The icons: measured on a filled slot's icon view (where it is from its cell's corner, its size), kept
    // for when the row shown has none.
    bool measured = false;
    gui::Point iconAt{2, 2}, iconSize{32, 32};

    gui::Surface back{"floating bar's slots", kTex, 256, kBackFlags};
    gui::Surface keys{"key labels", kTex, 256, kKeysFlags}; // nesting: g_labelNesting (see gui::Surface)
    bool texVertical = false; // back and keys have upright textures (256 x kTex), for a vertical bar
    bool seen = false;
};

std::vector<std::unique_ptr<Bar>> g_bars;
// How deep the key labels are nested in their windows: one past the game's icon views' count text (an
// icon view's depth + 2, + 1 for our view), measured on the first icon seen; until then a guess.
int g_labelNesting = 10;
bool g_labelNestingMeasured = false;
gui::Surface g_dock{"action bar frame", 2048, 512, gui::kWindowFlags}; // two bars side by side: wide
ab::DockLayout g_layout;
ab::Metrics g_metrics;
std::string g_dockXpDrawn;
void* g_wc = nullptr;
bool g_disabled = false;
float g_dockX = -1, g_dockY = -1;        // the frame's top-left (GUI units)
float g_cfgSeenX = -2, g_cfgSeenY = -2;  // the [ActionBars] place last taken
bool g_dragging = false;

// The mouse wheel over a bar: ShortcutBarView_c::WheelMoved (vtable +0x90, GUI 0x100D7563) hands it to the
// row spinner, whose WheelMoved (0x100DB5B8) goes a row back for the wheel turned up (y > 0) and a row on
// for down. Clarity's page arrows have up = the next row, so the wheel does the same: each bar's spinner gets
// a copy of its class's vtable (the RTTI locator before it too) with WheelMoved turning y round. Nothing in
// the game is patched; Restore puts the spinner's own vtable back.
constexpr int kWheelSlot = 0x90 / 4, kSpinnerVtSlots = 64;
void* g_spinnerVt[kSpinnerVtSlots + 1]; // [0]: the RTTI locator (vtable[-1]), then the slots
void** g_spinnerVtGame = nullptr;
using WheelFn = void(__thiscall*)(void* self, const gui::Point* delta);
WheelFn g_spinnerWheel = nullptr;

void __fastcall SpinnerWheel(void* self, void* /*edx*/, const gui::Point* delta)
{
    const gui::Point turned{delta->x, -delta->y};
    g_spinnerWheel(self, &turned);
}

void TurnWheel(void* spinner, bool turn)
{
    if (!spinner)
        return;
    void*** vp = static_cast<void***>(spinner);
    if (turn) {
        if (!g_spinnerVtGame) {
            g_spinnerVtGame = *vp;
            for (int i = -1; i < kSpinnerVtSlots; ++i)
                g_spinnerVt[i + 1] = g_spinnerVtGame[i];
            g_spinnerWheel = reinterpret_cast<WheelFn>(g_spinnerVtGame[kWheelSlot]);
            g_spinnerVt[kWheelSlot + 1] = reinterpret_cast<void*>(&SpinnerWheel);
        }
        if (*vp == g_spinnerVtGame)
            *vp = g_spinnerVt + 1;
    } else if (g_spinnerVtGame && *vp == g_spinnerVt + 1) {
        *vp = g_spinnerVtGame;
    }
}

void Restore(Bar& b)
{
    if (b.noRaiseSet) {
        if (unsigned* f = gui::WindowFlagsField(b.window))
            *f &= ~gui::kWindowNoRaise;
        b.noRaiseSet = false;
    }
    if (b.view)
        TurnWheel(At<void*>(b.view, kViewSpinner), false);
    if (b.listStyled && b.list) {
        SetFeatures(b.list, b.listFeatures);
        b.listStyled = false;
    }
    if (b.chromeHidden && b.view) {
        void* parts[2] = {At<void*>(b.view, kViewActive), At<void*>(b.view, kViewSpinner)};
        for (int i = 0; i < 2; ++i) {
            if (!parts[i])
                continue;
            gui::ViewShow(parts[i], true);
            gui::ViewSetFlags(parts[i], b.chromeFlags[i]);
        }
        b.chromeHidden = false;
    }
    gui::Show(b.back, false);
    gui::Show(b.keys, false);
}

void Style(Bar& b, bool hideChrome)
{
    // A press on a window brings it to the front (WindowController_c::HandleMouseDown, unless flag 0x400):
    // a bar in front of its key-label window is drawn first, and its icons covered the labels (picking up
    // an icon hid them). So the bars stay where they are when clicked.
    if (!b.noRaiseSet) {
        if (unsigned* f = gui::WindowFlagsField(b.window); f && !(*f & gui::kWindowNoRaise)) {
            *f |= gui::kWindowNoRaise;
            b.noRaiseSet = true;
        }
    }
    if (!b.listStyled) {
        b.listFeatures = GetFeatures(b.list);
        SetFeatures(b.list, b.listFeatures | kNoCellBackground);
        b.listStyled = true;
    }
    void* parts[2] = {At<void*>(b.view, kViewActive), At<void*>(b.view, kViewSpinner)};
    TurnWheel(parts[1], true);
    if (hideChrome && !b.chromeHidden) {
        for (int i = 0; i < 2; ++i) {
            if (!parts[i])
                continue;
            b.chromeFlags[i] = gui::ViewFlags(parts[i]);
            gui::ViewSetFlags(parts[i], b.chromeFlags[i] | gui::kViewCollapse); // gives up its room
            gui::ViewShow(parts[i], false);
        }
        b.chromeHidden = true;
    } else if (!hideChrome && b.chromeHidden) {
        for (int i = 0; i < 2; ++i) {
            if (!parts[i])
                continue;
            gui::ViewShow(parts[i], true);
            gui::ViewSetFlags(parts[i], b.chromeFlags[i]);
        }
        b.chromeHidden = false;
    } else if (hideChrome) {
        for (void* p : parts) // the game may have shown them again
            if (p)
                gui::ViewShow(p, false);
    }
}

// The bars in the window list, by index; bars no longer there are dropped.
void Scan(void* wc)
{
    for (auto& b : g_bars)
        b->seen = false;
    gui::ForEachWindow(wc, [&](void* w) {
        if (!gui::IsClass(w, kBarClass))
            return;
        auto it = std::find_if(g_bars.begin(), g_bars.end(), [&](const auto& b) { return b->window == w; });
        if (it == g_bars.end()) {
            g_bars.push_back(std::make_unique<Bar>());
            g_bars.back()->window = w;
            it = g_bars.end() - 1;
            if (Cfg().actionBars.debug)
                Log("[actionbars] bar %d found (window %p)", At<int>(w, kBarIndex), w);
        }
        (*it)->seen = true;
    });
    for (auto it = g_bars.begin(); it != g_bars.end();) {
        if (!(*it)->seen) {
            // The game deleted it (fewer bars): its views went with it; only our windows are left.
            gui::Show((*it)->back, false);
            gui::Show((*it)->keys, false);
            it = g_bars.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& b : g_bars) {
        b->index = At<int>(b->window, kBarIndex);
        b->view = At<void*>(b->window, kBarView);
        b->list = b->view ? At<void*>(b->view, kViewList) : nullptr;
        gui::Check(wc, b->back);
        gui::Check(wc, b->keys);
    }
    std::sort(g_bars.begin(), g_bars.end(), [](const auto& a, const auto& b) { return a->index < b->index; });
}

// The slots' grid position: a column per slot and a row per bar row (vertical bars the other way).
IPoint GridPos(const Bar& b, int slot)
{
    return b.vertical ? IPoint{b.row.page, slot} : IPoint{slot, b.row.page};
}

// Reads the bar: its row, whether it's the active one, its slots and their keys, and where its cells are.
void ReadBar(Bar& b, bool keys)
{
    b.vertical = At<int>(b.view, kViewVertical) != 0;
    void* spinner = At<void*>(b.view, kViewSpinner);
    void* button = At<void*>(b.view, kViewActive);
    b.row.vertical = b.vertical;
    b.row.page = spinner ? std::clamp(At<int>(spinner, kSpinnerRow), 0, 9) : 0;
    b.row.active = button && At<bool>(button, kButtonValue);
    char command[48];
    for (int i = 0; i < ab::kSlots; ++i) {
        const IPoint pos = GridPos(b, i);
        void* item = GetItem(b.list, &pos);
        ab::Slot& s = b.row.slot[i];
        s.filled = item != nullptr;
        s.key.clear();
        if (keys) {
            sprintf_s(command, "SHORTCUT_BAR_%d_%d", b.index, i);
            s.key = KeyFor(command);
            if (s.key.empty() && b.row.active) {
                sprintf_s(command, "SHORTUCT_BAR_ACTIVE_%d", i);
                s.key = KeyFor(command);
            }
        }
    }
    b.cellW = static_cast<float>(At<int>(b.list, kListCell));
    b.cellH = static_cast<float>(At<int>(b.list, kListCell + 4));
    if (b.cellW < 8 || b.cellW > 200 || b.cellH < 8 || b.cellH > 200)
        b.cellW = b.cellH = 36;
    if (!b.measured) {
        const float icon = At<float>(b.list, kListIcon);
        const float s = icon >= 4 && icon <= b.cellW ? icon : b.cellW - 4;
        b.iconSize = {s, s};
        b.iconAt = {(b.cellW - s) * 0.5f, (b.cellH - s) * 0.5f};
    }
    void* scrolled = At<void*>(b.list, kListScrolled);
    const IPoint g0 = GridPos(b, 0);
    const gui::Point local{At<float>(b.list, kListOrigin) + g0.x * b.cellW, At<float>(b.list, kListOrigin + 4) + g0.y * b.cellH};
    const gui::Point grid0 = scrolled ? gui::ViewToScreen(scrolled, local) : gui::Point{0, 0}; // the first cell's corner
    // The icons as the game places them: measured on a filled slot's icon view.
    for (int i = 0; i < ab::kSlots && scrolled; ++i) {
        if (!b.row.slot[i].filled)
            continue;
        const IPoint pos = GridPos(b, i);
        void* item = GetItem(b.list, &pos);
        void* view = item ? ItemView(item) : nullptr;
        if (!view)
            continue;
        const gui::Point at = gui::ViewToScreen(view, {0, 0});
        const gui::Point size = gui::ViewSize(view);
        const gui::Point off{at.x - (grid0.x + (b.vertical ? 0 : i * b.cellW)), at.y - (grid0.y + (b.vertical ? i * b.cellH : 0))};
        if (size.x < 4 || size.x > b.cellW * 1.5f || size.y < 4 || size.y > b.cellH * 1.5f || std::fabs(off.x) > b.cellW ||
            std::fabs(off.y) > b.cellH)
            break;
        const bool changed = !b.measured || std::fabs(off.x - b.iconAt.x) > 0.1f || std::fabs(off.y - b.iconAt.y) > 0.1f ||
                             std::fabs(size.x - b.iconSize.x) > 0.1f;
        b.iconAt = off;
        b.iconSize = size;
        b.measured = true;
        if (!g_labelNestingMeasured) {
            g_labelNestingMeasured = true;
            const int depth = gui::ViewDepth(view);
            g_labelNesting = std::clamp(depth + 2, 1, gui::kMaxNesting);
            if (Cfg().actionBars.debug)
                Log("[actionbars] icon views are %d deep: key labels %d views deep", depth, g_labelNesting + 1);
        }
        if (changed && Cfg().actionBars.debug)
            Log("[actionbars] bar %d: cells %.0fx%.0f, icons %.1fx%.1f at (%.1f, %.1f) in their cells", b.index, b.cellW, b.cellH, size.x,
                size.y, off.x, off.y);
        break;
    }
    // Clarity's cells are centred on the icons: their corner is half the room round an icon before it.
    b.cell0 = {grid0.x + b.iconAt.x - (b.cellW - b.iconSize.x) * 0.5f, grid0.y + b.iconAt.y - (b.cellH - b.iconSize.y) * 0.5f};
}

std::string RowState(const ab::Row& r)
{
    std::string s;
    s += static_cast<char>('0' + r.page);
    s += r.active ? 'A' : '-';
    s += r.vertical ? 'V' : 'H';
    for (const ab::Slot& slot : r.slot) {
        s += slot.filled ? '#' : '.';
        for (wchar_t ch : slot.key)
            s += static_cast<char>(ch);
        s += '|';
    }
    return s;
}

// --- XP ---------------------------------------------------------------------------------------

std::wstring Grouped(long long v)
{
    std::wstring g = std::to_wstring(v < 0 ? -v : v);
    for (int i = static_cast<int>(g.size()) - 3; i > 0; i -= 3)
        g.insert(static_cast<size_t>(i), L",");
    return v < 0 ? L"-" + g : g;
}

// The game's XP and alien XP bar fills: CharacterBar_c kinds 2 / 3 make PowerbarView_c(rect, background,
// fill, ...) with fill images 0x15 / 9 (GFX_GUI_ACTIONVIEW_XPBAR / _ALIENXPBAR, upright), read from the
// active skin once; the ndGUI / default skins' colours if they can't be.
struct Colours {
    uint32_t rgb;
    std::vector<uint32_t> shading;
};
Colours g_xpColours{ab::kGameXpRgb, ab::kGameXpShading}, g_alienColours{ab::kGameAlienRgb, ab::kGameAlienShading};
bool g_coloursRead = false;
constexpr int kXpFillImage = 0x15, kAlienFillImage = 9;

void ReadColours()
{
    if (g_coloursRead)
        return;
    g_coloursRead = true;
    auto read = [](int id, Colours* c) {
        std::vector<uint32_t> across = gui::SkinProfile(id);
        if (across.empty())
            return;
        c->shading = across;
        c->rgb = Canvas::ProfileAt(across, 0.0f, 1.0f);
    };
    read(kXpFillImage, &g_xpColours);
    read(kAlienFillImage, &g_alienColours);
    Log("[actionbars] XP #%06X, alien XP #%06X", g_xpColours.rgb, g_alienColours.rgb);
}

void ReadXp(ab::Xp* xp, ab::Xp* alien)
{
    ReadColours();
    const game::Identity self{50000, game::ClientInstance()};
    auto stat = [&](int id) { return n3::GetSkill(self, id); };
    // XP (kind 2): (XP - previous level's) / (next level's - previous level's); from level 200 the
    // Shadowknowledge, its base 0 unless stat 576 bit 1.
    const int level = stat(54);
    int cur = 0, next = stat(350), prev = stat(57);
    const bool sk = level != n3::kNoValue && level >= 200;
    if (sk) {
        cur = stat(573);
        if ((stat(576) & 1) == 0)
            prev = 0;
    } else {
        cur = stat(52);
    }
    xp->shown = true;
    if (cur == n3::kNoValue || next == n3::kNoValue || prev == n3::kNoValue || next - prev <= 0) {
        xp->fraction = -1.0f;
        xp->text.clear();
    } else {
        const long long have = static_cast<long long>(cur) - prev, need = static_cast<long long>(next) - prev;
        xp->fraction = std::clamp(static_cast<float>(have) / need, 0.0f, 1.0f);
        xp->text = Grouped(have) + L" / " + Grouped(need); // just the numbers
    }
    xp->rgb = g_xpColours.rgb;
    xp->shading = g_xpColours.shading;
    // Alien XP (kind 3), only with the expansion that has it (stat 389 bits 0x18, as the control centre).
    const int expansions = stat(389);
    alien->shown = expansions != n3::kNoValue && (expansions & 0x18) != 0;
    if (alien->shown) {
        int acur = stat(40);
        const int amax = stat(178);
        if (acur == n3::kNoValue)
            acur = 0;
        if (amax == n3::kNoValue || amax <= 0) {
            alien->fraction = -1.0f;
            alien->text.clear();
        } else {
            alien->fraction = std::clamp(static_cast<float>(acur) / amax, 0.0f, 1.0f);
            alien->text = Grouped(acur) + L" / " + Grouped(amax);
        }
        alien->rgb = g_alienColours.rgb;
        alien->shading = g_alienColours.shading;
    }
}

std::string XpState(const ab::Xp& x)
{
    std::string s = x.shown ? "1" : "0";
    for (wchar_t ch : x.text)
        s += static_cast<char>(ch);
    char f[16];
    sprintf_s(f, "|%.4f|", x.fraction);
    return s + f;
}

// --- tick -------------------------------------------------------------------------------------

// The frame's place: [ActionBars] X / Y when they changed, else bottom centre.
void UpdateHome(const gui::Point& screen, float k)
{
    const ActionBarsConfig& cfg = Cfg().actionBars;
    if (cfg.x == g_cfgSeenX && cfg.y == g_cfgSeenY && g_dockX >= 0)
        return;
    g_cfgSeenX = cfg.x;
    g_cfgSeenY = cfg.y;
    if (cfg.x >= 0 && cfg.y >= 0) {
        g_dockX = cfg.x;
        g_dockY = cfg.y;
    } else {
        g_dockX = std::round((screen.x - g_layout.w * k) * 0.5f);
        g_dockY = std::round(screen.y - g_layout.h * k - 6.0f * k);
    }
}

void HideAll()
{
    gui::Show(g_dock, false);
    for (auto& b : g_bars)
        Restore(*b);
}

// The game's own XP and alien XP bars: CharBarWindow_c windows (GUI 0x1006D7C7) "CCXPBarConfig" and
// "CCAlienXPBarConfig" (config name at +0x74), made by ControlCenterModule_c::SlotPlayerCharacterAlive.
// Hidden while the frame shows Clarity's; when it doesn't, the ones Clarity hid are shown again if their own
// show condition (a CriteriaMonitor_c at +0x90: "dvalue:cc_section1 && dvalue:cc_xp_bar") holds.
constexpr char kCharBarClass[] = ".?AVCharBarWindow_c@@";
constexpr int kCharBarConfig = 0x74, kCharBarCriteria = 0x90;
using EvaluateFn = bool(__thiscall*)(void* self);
EvaluateFn CriteriaEvaluate;
std::vector<void*> g_hiddenGameXp;
DWORD g_lastXpSync = 0;

std::string ConfigName(void* window)
{
    const auto* s = static_cast<const uint8_t*>(window) + kCharBarConfig; // VS2010 std::string
    const uint32_t size = *reinterpret_cast<const uint32_t*>(s + 0x10);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(s + 0x14);
    if (size > 64 || cap < size)
        return {};
    const char* chars = cap >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    return chars ? std::string(chars, size) : std::string();
}

void SyncGameXp(void* wc, bool hide)
{
    std::vector<void*> hidden;
    gui::ForEachWindow(wc, [&](void* w) {
        if (!gui::IsClass(w, kCharBarClass))
            return;
        const std::string name = ConfigName(w);
        if (name != "CCXPBarConfig" && name != "CCAlienXPBarConfig")
            return;
        const bool ours = std::find(g_hiddenGameXp.begin(), g_hiddenGameXp.end(), w) != g_hiddenGameXp.end();
        if (hide) {
            if (gui::WindowVisible(w))
                gui::WindowSetShown(w, false);
            gui::MouseThrough(w, true); // hidden, it still took the tooltips where it is
            hidden.push_back(w);
        } else if (ours) {
            gui::MouseThrough(w, false);
            if (CriteriaEvaluate && CriteriaEvaluate(static_cast<uint8_t*>(w) + kCharBarCriteria))
                gui::WindowSetShown(w, true); // its own condition says it's on
        }
    });
    g_hiddenGameXp.swap(hidden);
}

// The bar's own windows' textures the bar's way round: kTex along it, 256 across (a vertical bar of 10 cells
// is taller than 256: in a wide texture it was cut off after 6 slots). Turned round, they're made again (the
// GUI has no way to delete a window: the old ones stay hidden).
void FitTextures(Bar& b)
{
    if (b.texVertical == b.vertical)
        return;
    b.texVertical = b.vertical;
    for (gui::Surface* s : {&b.back, &b.keys}) {
        if (s->window) {
            gui::Show(*s, false);
            gui::Forget(*s);
        }
        s->texW = b.vertical ? 256 : kTex;
        s->texH = b.vertical ? kTex : 256;
    }
}

void PresentKeys(void* wc, Bar& b, float k, bool show)
{
    const ActionBarsConfig& cfg = Cfg().actionBars;
    if (!show || !cfg.showKeys) {
        gui::Show(b.keys, false);
        return;
    }
    int w, h;
    ab::RowSize(g_metrics, b.row, &w, &h);
    // A pixel larger than the cells all round: with the bar window's own frame exactly on its cells, a label
    // window of the same rectangle showed the world through its letters (2026-11-14 test).
    constexpr int kPad = 1;
    if (!b.keys.window)
        b.keys.nesting = g_labelNesting;
    else if (b.keys.nesting != g_labelNesting) { // measured since it was made: make it again
        gui::Show(b.keys, false);
        gui::Forget(b.keys);
        b.keys.nesting = g_labelNesting;
    }
    if (!gui::Ensure(b.keys, w + 2 * kPad, h + 2 * kPad, k)) {
        g_disabled = true;
        return;
    }
    gui::MoveTo(b.keys, std::round(b.cell0.x) - kPad * k, std::round(b.cell0.y) - kPad * k);
    const std::string state = RowState(b.row) + (cfg.hideEmpty ? "E" : "-");
    if (state != b.keys.drawn) {
        Canvas c;
        c.Reset(b.keys.w, b.keys.h);
        ab::PaintKeys(c, g_metrics, kPad, kPad, b.row, cfg.hideEmpty);
        gui::Upload(b.keys, c);
        b.keys.drawn = state;
    }
    gui::Show(b.keys, true);
    // The labels are drawn over the icons only from a window in front of the bar's: if the bar came in front
    // anyway (the game reorders windows on a drag), the labels go back in front - only then, as moving them
    // to the front every tick made them flicker.
    if (b.keys.window && !gui::Above(wc, b.keys.window, b.window)) {
        gui::WindowFront(b.keys.window);
        if (cfg.debug)
            Log("[actionbars] bar %d came in front of its key labels - labels moved to the front", b.index);
    }
}

void PresentBack(Bar& b, float k, bool show, bool hideEmpty)
{
    if (!show) {
        gui::Show(b.back, false);
        return;
    }
    int w, h;
    ab::RowSize(g_metrics, b.row, &w, &h);
    if (!gui::Ensure(b.back, w, h, k)) {
        g_disabled = true;
        return;
    }
    gui::MoveTo(b.back, std::round(b.cell0.x), std::round(b.cell0.y));
    const std::string state = RowState(b.row) + (hideEmpty ? "E" : "-");
    if (state != b.back.drawn) {
        Canvas c;
        c.Reset(b.back.w, b.back.h);
        ab::PaintSlots(c, g_metrics, 0, 0, b.row, hideEmpty);
        gui::Upload(b.back, c);
        b.back.drawn = state;
    }
    gui::Show(b.back, true);
}

void Update(HWND hwnd)
{
    void* wc = gui::WindowController();
    if (wc != g_wc) {
        g_bars.clear();
        gui::Forget(g_dock);
        g_wc = wc;
    }
    if (!wc)
        return;
    gui::Check(wc, g_dock);
    Scan(wc);
    const ActionBarsConfig& cfg = Cfg().actionBars;
    const DWORD tick = GetTickCount();
    if (tick - g_lastXpSync >= 250) {
        g_lastXpSync = tick;
        SyncGameXp(wc, cfg.enable && cfg.showXp && !g_disabled && gui::InGame());
    }
    if (!cfg.enable || g_disabled || !gui::InGame() || g_bars.empty()) {
        HideAll();
        return;
    }

    gui::Point screen{};
    const float k = gui::GuiPerPx(hwnd, &screen);
    const DWORD now = GetTickCount();
    if (now - g_keysRead >= 500 || g_keys.empty()) {
        g_keysRead = now;
        ReadKeys();
    }
    const bool dragging = gui::Dragging(wc);
    const bool hideEmpty = cfg.hideEmpty && !dragging; // while something is dragged every slot shows, to drop on

    // The bars as they are now.
    for (auto& b : g_bars) {
        if (!b->view || !b->list)
            continue;
        ReadBar(*b, cfg.showKeys);
    }
    const Bar& first = *g_bars.front();
    g_metrics = ab::MetricsFor(first.cellW / k, first.iconSize.x / k);
    if (!ab::EnsureFonts(g_metrics.scale)) {
        Log("[actionbars] font creation failed - off");
        g_disabled = true;
        HideAll();
        return;
    }

    // The dock: the first bars that are shown, by index.
    std::vector<Bar*> docked;
    for (auto& b : g_bars) {
        b->docked = false;
        if (static_cast<int>(docked.size()) < cfg.docked && b->index < cfg.docked && b->view && b->list && !b->vertical &&
            gui::WindowVisible(b->window)) {
            b->docked = true;
            docked.push_back(b.get());
        }
    }
    ab::Dock dock;
    dock.rows = static_cast<int>(docked.size());
    dock.hideEmpty = hideEmpty;
    for (size_t i = 0; i < docked.size(); ++i)
        dock.row[i] = docked[i]->row;
    if (cfg.showXp)
        ReadXp(&dock.xp, &dock.alien);

    if (!docked.empty()) {
        g_layout = ab::LayoutDock(g_metrics, dock.rows, dock.xp.shown, dock.alien.shown);
        UpdateHome(screen, k);
        if (!gui::Ensure(g_dock, g_layout.w, g_layout.h, k)) {
            g_disabled = true;
            HideAll();
            return;
        }
        if (!g_dragging) {
            g_dockX = std::clamp(g_dockX, 0.0f, std::max(0.0f, screen.x - g_dock.gw));
            g_dockY = std::clamp(g_dockY, 0.0f, std::max(0.0f, screen.y - g_dock.gh));
            gui::MoveTo(g_dock, g_dockX, g_dockY);
        }
        std::string state;
        for (int i = 0; i < dock.rows; ++i)
            state += RowState(dock.row[i]) + "/";
        char m[64];
        sprintf_s(m, "%d|%.3f|%d|%d|%c", g_metrics.cell, g_metrics.cellF, g_layout.w, g_layout.h, hideEmpty ? 'E' : '-');
        state += m;
        const std::string xpState = XpState(dock.xp) + XpState(dock.alien);
        if (state != g_dock.drawn || xpState != g_dockXpDrawn) {
            Canvas c;
            c.Reset(g_dock.w, g_dock.h);
            ab::PaintDock(c, g_metrics, g_layout, dock);
            if (state != g_dock.drawn) {
                gui::Upload(g_dock, c);
            } else {
                const RECT part = ab::XpArea(g_metrics, g_layout);
                gui::Upload(g_dock, c, &part);
            }
            g_dock.drawn = state;
            g_dockXpDrawn = xpState;
        }
        gui::Show(g_dock, true);
        // The docked bars' windows: moved so their first cell is on the frame's.
        for (int i = 0; i < dock.rows; ++i) {
            Bar& b = *docked[static_cast<size_t>(i)];
            const float wantX = g_dock.x + g_layout.cellX[i] * k, wantY = g_dock.y + g_layout.rowY[i] * k;
            const float dx = wantX - b.cell0.x, dy = wantY - b.cell0.y;
            if (std::fabs(dx) > 0.05f || std::fabs(dy) > 0.05f) {
                // On whole pixels: anywhere between, the game's icons are drawn blurred.
                const gui::Rect f = gui::WindowFrame(b.window);
                const float x = std::round(f.left + dx), y = std::round(f.top + dy);
                gui::WindowMoveTo(b.window, x, y);
                b.cell0.x += x - f.left;
                b.cell0.y += y - f.top;
            }
        }
    } else {
        gui::Show(g_dock, false);
    }

    for (auto& b : g_bars) {
        if (!b->view || !b->list)
            continue;
        const bool shown = gui::WindowVisible(b->window);
        Style(*b, b->docked || cfg.locked);
        FitTextures(*b);
        PresentBack(*b, k, shown && !b->docked, hideEmpty);
        PresentKeys(wc, *b, k, shown);
    }

    static DWORD lastLog = 0;
    if (cfg.debug && now - lastLog >= 5000) {
        lastLog = now;
        Log("[actionbars] %.3f GUI units a pixel, frame at (%.0f, %.0f) %dx%d px, %d keys bound", k, g_dock.x, g_dock.y, g_dock.w, g_dock.h,
            static_cast<int>(g_keys.size()));
        for (auto& b : g_bars) {
            const gui::Rect f = gui::WindowFrame(b->window);
            std::string keys;
            for (const ab::Slot& slot : b->row.slot) {
                keys += slot.filled ? '#' : '.';
                for (wchar_t ch : slot.key)
                    keys += static_cast<char>(ch);
                keys += ' ';
            }
            Log("[actionbars] bar %d%s%s%s row %d: window (%.1f, %.1f)-(%.1f, %.1f), first cell (%.1f, %.1f), cells %.0fx%.0f: %s",
                b->index, gui::WindowVisible(b->window) ? "" : " (hidden)", b->docked ? " docked" : "", b->row.active ? " active" : "",
                b->row.page + 1, f.left, f.top, f.right, f.bottom, b->cell0.x, b->cell0.y, b->cellW, b->cellH, keys.c_str());
        }
    }
}

// --- the game window: timer, page clicks and dragging -------------------------------------------

constexpr UINT_PTR kTimerId = 0xC1AB;
constexpr UINT kTickMs = 33;
bool g_timerSet = false;
POINT g_dragMouse{};
float g_dragX = 0, g_dragY = 0;
bool g_swallowUp = false;

void SaveDock()
{
    g_dockX = std::round(g_dockX);
    g_dockY = std::round(g_dockY);
    gui::MoveTo(g_dock, g_dockX, g_dockY);
    wchar_t v[32];
    swprintf_s(v, L"%.0f", g_dockX);
    WritePrivateProfileStringW(L"ActionBars", L"X", v, ConfigPath().c_str());
    swprintf_s(v, L"%.0f", g_dockY);
    WritePrivateProfileStringW(L"ActionBars", L"Y", v, ConfigPath().c_str());
    g_cfgSeenX = Cfg().actionBars.x; // until Clarity.ini is read again, the old place isn't a change
    g_cfgSeenY = Cfg().actionBars.y;
}

void EndDrag()
{
    if (!g_dragging)
        return;
    g_dragging = false;
    if (GetCapture())
        ReleaseCapture();
    if (g_dock.window)
        SaveDock();
}

// A press on the frame: a page control steps its bar's row; else (unlocked) the frame is dragged.
bool OnPress(HWND hwnd, LPARAM lp)
{
    const ActionBarsConfig& cfg = Cfg().actionBars;
    if (!cfg.enable || !g_wc || !g_dock.window || !gui::WindowVisible(g_dock.window))
        return false;
    // Over the frame, or over a docked bar's window: a bar's window may reach past its slots (the room its
    // hidden handles had) onto the frame's page controls.
    void* under = gui::MouseWindow(g_wc);
    bool overBar = false;
    for (auto& b : g_bars)
        overBar = overBar || (b->docked && b->window == under);
    if (under != g_dock.window && !overBar)
        return false;
    gui::Point screen{};
    const float k = gui::GuiPerPx(hwnd, &screen);
    const float gx = static_cast<short>(LOWORD(lp)) * k, gy = static_cast<short>(HIWORD(lp)) * k;
    const int px = static_cast<int>((gx - g_dock.x) / k), py = static_cast<int>((gy - g_dock.y) / k);
    int row = 0;
    for (auto& b : g_bars) {
        if (!b->docked)
            continue;
        const RECT& r = g_layout.page[row++];
        if (px < r.left || px >= r.right || py < r.top || py >= r.bottom || !SetRow)
            continue;
        const bool up = py < (r.top + r.bottom) / 2; // the up arrow: the next row (a higher number)
        const int next = (b->row.page + (up ? 1 : 9)) % 10;
        SetRow(b->view, next);
        if (cfg.debug)
            Log("[actionbars] bar %d: row %d -> %d", b->index, b->row.page, next);
        return true;
    }
    if (overBar)
        return false; // the bar's own
    if (cfg.locked)
        return true; // the frame takes the click (it isn't the world's)
    g_dragging = true;
    g_dragMouse = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    g_dragX = g_dock.x;
    g_dragY = g_dock.y;
    SetCapture(hwnd);
    return true;
}

bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result)
{
    if (!g_timerSet)
        g_timerSet = SetTimer(hwnd, kTimerId, kTickMs, nullptr) != 0;
    switch (msg) {
    case WM_TIMER:
        if (wp != kTimerId)
            return false;
        Update(hwnd);
        *result = 0;
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (g_dragging || (wp & (MK_RBUTTON | MK_SHIFT | MK_CONTROL)) || !OnPress(hwnd, lp))
            return false;
        g_swallowUp = !g_dragging;
        *result = 0;
        return true;
    case WM_MOUSEMOVE:
        if (!g_dragging)
            return false;
        if (g_dock.window) {
            gui::Point screen{};
            const float k = gui::GuiPerPx(hwnd, &screen);
            g_dockX = std::clamp(g_dragX + (static_cast<short>(LOWORD(lp)) - g_dragMouse.x) * k, 0.0f, std::max(0.0f, screen.x - g_dock.gw));
            g_dockY = std::clamp(g_dragY + (static_cast<short>(HIWORD(lp)) - g_dragMouse.y) * k, 0.0f, std::max(0.0f, screen.y - g_dock.gh));
            gui::MoveTo(g_dock, g_dockX, g_dockY);
            Update(hwnd); // the bars follow at once
        }
        *result = 0;
        return true;
    case WM_LBUTTONUP:
        if (g_swallowUp) {
            g_swallowUp = false;
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
        return false;
    default:
        return false;
    }
}

} // namespace

bool Install()
{
    const char* gui = "GUI.dll";
    bool ok = gui::Init("actionbars");
    ok &= Resolve(GetItem, gui, "?GetItem@MultiListView_c@@QBEPAVMultiListViewItem_c@@ABVIPoint@@@Z");
    ok &= Resolve(ItemView, gui, "?GetItemView@MultiListViewItem_c@@QAEPAVView@@XZ");
    ok &= Resolve(GetFeatures, gui, "?GetFeatureFlags@MultiListView_c@@QBEIXZ");
    ok &= Resolve(SetFeatures, gui, "?SetFeatureFlags@MultiListView_c@@QAEXI@Z");
    if (!ok) {
        Log("[actionbars] disabled");
        return false;
    }
    // In GUI.dll or Utils.dll depending on the build (only to show the game's XP bars again).
    for (const char* m : {gui, "Utils.dll"}) {
        if (HMODULE h = GetModuleHandleA(m))
            CriteriaEvaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(h, "?Evaluate@CriteriaMonitor_c@@QAE_NXZ"));
        if (CriteriaEvaluate)
            break;
    }
    if (uint8_t* p = FindUnique(SIG_SHORTCUT_ROW, "shortcut bar row setter"))
        SetRow = reinterpret_cast<RowSetFn>(p);
    else
        Log("[actionbars] no row setter - the frame's page controls do nothing");
    if (uint8_t* p = FindUnique(SIG_KEYBIND_MANAGER, "key-binding manager"))
        g_bindingsGlobal = *reinterpret_cast<void***>(p + 12);
    else
        Log("[actionbars] no key-binding manager - no key labels");
    wndproc::Add(&OnMessage, "action bars");
    Log("[actionbars] ready");
    return true;
}

} // namespace prk::actionbars
