// Game side (GUI.dll TargetingModule_t):
//  - every 2 s the module creates an Indicator for each character within 30 m (the selected
//    target and the attacker get their own Indicators with a frame + health bar);
//  - the builder renders "title name"/"org" with the 12 px bitmap font into a w x 32 image and
//    shows it as a VisualSprite_t of 1-2 x 0.3 world units (so it shrinks with distance);
//  - the update moves the sprite to N3Msg_GetIndicatorPosition every frame.
// We keep all of that and only swap the sprite's material for our own texture, then set the
// sprite's quad size each frame.
#include "nameplates.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/n3.h"
#include "../../core/signatures.h"
#include "../../core/text_raster.h"
#include "../../core/threat.h"
#include "../hideui/hideui.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace prk::nameplates {

namespace {

// Indicator (0x38 bytes)
constexpr int kIndIdentity = 0x08;
constexpr int kIndGameMaterial = 0x10;  // the game's VisualMaterial_t*
constexpr int kIndSprite = 0x18;        // VisualSprite_t*
constexpr int kIndIsAttacking = 0x30;
constexpr int kIndShowFrame = 0x31;     // selection/attacker indicators: frame + health bar
constexpr int kIndTextRgb = 0x34;       // 0xFFFFFF, red for battlestation enemies
// RSprite
constexpr int kSpriteHalfW = 0x178;
constexpr int kSpriteHalfH = 0x17C;
constexpr int kSpriteVerts = 0x1A0;
constexpr float kTagRange = 30.0f;   // N3Msg_GetDynelsInVicinity radius (Gamecode 0x10157500)     // 4 x {x, y, z, diffuse, u, v}

struct Vertex {
    float x, y, z;
    uint32_t diffuse;
    float u, v;
};
static_assert(sizeof(Vertex) == 0x18, "RSprite vertex");

constexpr int kTexW = 1024; // wide enough for long names at full size, with the level beside them
constexpr int kTexH = 256;
constexpr int kPad = 8;          // texels around text pieces (outline + shadow room)
// Sizes are designed for a 40 px name font and scaled by g_scale so the texture's text is drawn
// at the size it is shown (MaxPx): no mipmaps, so heavy minification would alias.
constexpr float kShadowAlpha = 0.6f;
constexpr int kDesignNameEm = 40;
constexpr float kCapPerEm = 0.727f; // Verdana cap height / em
float g_scale = 1.0f;
int S(int v)
{
    return std::max(1, static_cast<int>(std::lround(v * g_scale)));
}
int g_outline = 2, g_shadowOffset = 2, g_shadowBlur = 2;
int g_barW = 340, g_barH = 38, g_barBorder = 2; // bar wider than most names (the user's reference)
constexpr uint32_t kTypeCharacter = 50000;
constexpr int kStatPvpFlags = 224;   // 0xE0: bits 0x4000001 = flagged for PvP ("** name **")
constexpr int kStatNameMarks = 673;  // 0x2A1: bit 9 = "= name =" (as the game's tag builder)
constexpr int kStyleClassic = 1; // NameplateConfig::style
// Classic puts the name where the game's tag has it. The game's tag is a 32-texel-tall texture
// centred on the anchor, its name printed from row 1 (12 px font: capitals' middle near row 8.5),
// so the name's middle is this fraction of the quad's half height above the anchor.
constexpr float kClassicNameRise = 1.0f - 2.0f * 8.5f / 32.0f;

enum class Kind { Player, Npc, Pet, Self };
// Which Indicator: the 2 s nametag scan's, the selected target's (frame + bar) or the fight target's.
enum class Role { Normal, Selected, Fighting };

struct Plate {
    void* vm = nullptr;     // our VisualMaterial_t (texture + material)
    void* sprite = nullptr; // the RSprite we last patched
    float signX[4] = {}, signY[4] = {};
    float upSign = 1.0f;
    float rightSign = 1.0f;     // quad x towards the screen's right (the texture's left edge has u = 0)
    Kind kind = Kind::Player;
    std::vector<uint32_t> text; // ARGB without the health bar
    float nameCap = 30.0f;      // texels, what MinPx/MaxPx refer to
    int anchorRow = kTexH;      // texel row placed at the game's anchor (above the head)
    float nameMidRow = 0.0f;    // Classic: texel row of the middle of the name's capitals
    int barY = -1;              // -1 = no bar
    int barW = 0;               // texels: g_barW, or wider to span a long name
    int barH = 0, barBorder = 0; // texels
    bool barLabel = true;       // health percentage inside the bar (Modern)
    std::wstring level;         // drawn by Upload in the bar's colour; empty = none
    float levelX = 0.0f, levelY = 0.0f;
    // "Attacking you" marker: a soft red ring around level, name and bar, blurred once per layout.
    // "Attacking you" brackets (red, outside the target ones): fill and outline masks of their box,
    // whose (0, 0) is texel (atkX, atkY), and how strongly they're painted (0 = off, blinks).
    raster::Mask atkFill, atkOutline;
    int atkX = 0, atkY = 0;
    int atkLevel = 0; // 0..kBlinkSteps
    int hpPct = -1;
    uint32_t barRgb = 0;
    unsigned version = 0;
    DWORD born = 0;          // for the fade-in
    Role role = Role::Normal;
    bool alsoFight = false;  // selected target that is also the fight target (one plate, gold marker)
    // Click-to-target: content extent in texels (half width about the centre, top row) and the
    // last on-screen rectangle, refreshed each frame the plate is drawn.
    game::Identity id{};
    float hitHalfW = 0.0f;
    float visTop = 0.0f, visBottom = 0.0f; // texel rows of the visible content
    RECT screen{};
    float screenTopFull = 0.0f; // screen.top raised by the bracket room (floating text starts above)
    float depth = 0.0f;
    DWORD screenTick = 0;
    float priority = 0.0f;   // stacking order: lower keeps its place (depth; targets first)
    float stackUp = 0.0f;    // current stacking offset, px
    DWORD stackTick = 0;
    float headHeight = 0.0f; // smoothed anchor height above the character's position (world units)
    DWORD headTick = 0;      // 0 = not measured yet
    bool inSight = true;     // the camera sees the plate past walls and terrain (characters don't count)
    DWORD sightTick = 0;     // last line-of-sight test
    float sightAlpha = 1.0f; // fades towards inSight
    DWORD sightFadeTick = 0;
    Vertex origVerts[4] = {}; // the game's quad, restored when modern plates are turned off
    float origHalfW = 0.0f, origHalfH = 0.0f;
};

using BuildFn = void(__thiscall*)(uint8_t* ind);
using UpdateFn = int(__thiscall*)(uint8_t* ind);
using DeleteFn = void*(__thiscall*)(void* self, unsigned flags);

BuildFn g_origBuild = nullptr;
UpdateFn g_origUpdate = nullptr;
DeleteFn g_origDelete = nullptr;
void** g_indicatorVtable = nullptr;

// Game thread only.
std::unordered_map<uint8_t*, Plate> g_plates;
struct LastPlate {
    DWORD tick;
    float stackUp;
    // Its last footprint, still used by the other plates' stacking for kGhostMs, so they don't drop
    // into the gap while the replacing plate (normal tag <-> selection indicator) appears.
    game::Identity id;
    RECT screen;
    float priority;
    bool hadScreen;
};
constexpr DWORD kGhostMs = 400;
std::unordered_map<uint64_t, LastPlate> g_lastPlate; // identity -> its last deleted plate
void** g_targetingInstance = nullptr;            // TargetingModule_t::m_pcInstance
using SetTargetFn = void(__cdecl*)(const game::Identity& id, bool forced);
using InstanceFn = void*(__cdecl*)();
using GetMouseWindowFn = void*(__thiscall*)(void* self);
SetTargetFn g_setTarget = nullptr;            // TargetingModule_t::SetTarget
InstanceFn g_windowController = nullptr;      // WindowController_c::GetInstanceIfAny
GetMouseWindowFn g_getMouseWindow = nullptr;  // WindowController_c::GetMouseWindow (+0x190)
WNDPROC g_prevWndProc = nullptr;
HWND g_clickWnd = nullptr;
bool g_unicodeWnd = false;
bool g_swallowUp = false;
DWORD g_gameThread = 0;
constexpr int kTargetingScanTimer = 0x0C;
constexpr float kStackDepthTie = 1.0f; // metres: closer than this counts as the same distance
constexpr float kStackSwapDepth = 4.0f; // metres: the plate behind must get this much nearer to take the lower spot
constexpr DWORD kPairForgetMs = 1500;   // an order is forgotten once its two plates stop overlapping this long
constexpr DWORD kStackHoldMs = 150;     // "stacked" means overlapping (or within the margin) a moment ago
constexpr float kStackKeepPx = 10.0f;   // stacked plates stay stacked while within this many pixels
constexpr float kStackMaxPlates = 2.0f; // a plate moves up at most this many plate heights (no towers)
constexpr float kStackRiseRate = 10.0f; // 1/s: how fast a plate moves up out of the way
constexpr float kStackFallRate = 6.0f;  // 1/s: how fast it settles back down
constexpr float kHeadFollow = 2.5f;    // 1/s: how fast the plate follows the head's height (walk bob is ~2-3 Hz)
constexpr float kHeadSnap = 1.0f;      // metres: a bigger change in head height is taken at once
constexpr DWORD kSightEveryMs = 100;   // line-of-sight test interval per plate
constexpr float kSightFadeSec = 0.15f; // fade out / in when a wall comes between
constexpr DWORD kBlinkMs = 1600;       // the "attacking you" brackets: one slow blink, full to faint and back
constexpr float kEdgeMarginPx = 8.0f;     // target plates kept this far inside the window edges
constexpr float kFirstPersonRadius = 1.5f; // your own plate's anchor this close to the camera = first person
constexpr int kBlinkSteps = 6;         // painted strengths (each change re-uploads the texture)
constexpr float kBlinkLow = 0.25f;     // faintest strength
raster::Font g_nameFont, g_smallFont, g_pctFont, g_levelFont, g_classicFont;
bool g_fontsTried = false;
bool g_fontsOk = false;
bool g_buildFailed = false; // stop retrying after a texture/font failure
unsigned g_builtVersion = 0;
std::atomic<unsigned> g_version{1};

template <typename T>
T& Field(uint8_t* obj, int off)
{
    return *reinterpret_cast<T*>(obj + off);
}

// --- texture composition ------------------------------------------------------------------

struct Canvas {
    std::vector<float> r, g, b, a; // premultiplied

    Canvas() : r(kTexW * kTexH), g(kTexW * kTexH), b(kTexW * kTexH), a(kTexW * kTexH) {}

    void Over(int x0, int y0, const raster::Mask& m, uint32_t rgb, float alpha)
    {
        const float cr = ((rgb >> 16) & 0xFF) / 255.0f, cg = ((rgb >> 8) & 0xFF) / 255.0f, cb = (rgb & 0xFF) / 255.0f;
        for (int y = 0; y < m.h; ++y) {
            const int ty = y0 + y;
            if (ty < 0 || ty >= kTexH)
                continue;
            for (int x = 0; x < m.w; ++x) {
                const int tx = x0 + x;
                if (tx < 0 || tx >= kTexW)
                    continue;
                const float sa = std::clamp(m.a[y * m.w + x] * alpha, 0.0f, 1.0f);
                if (sa <= 0.0f)
                    continue;
                const size_t i = static_cast<size_t>(ty) * kTexW + tx;
                r[i] = cr * sa + r[i] * (1.0f - sa);
                g[i] = cg * sa + g[i] * (1.0f - sa);
                b[i] = cb * sa + b[i] * (1.0f - sa);
                a[i] = sa + a[i] * (1.0f - sa);
            }
        }
    }

    std::vector<uint32_t> ToArgb() const
    {
        std::vector<uint32_t> out(kTexW * kTexH);
        for (size_t i = 0; i < out.size(); ++i) {
            const float al = a[i];
            if (al <= 0.0f) {
                out[i] = 0;
                continue;
            }
            auto ch = [&](float v) { return static_cast<uint32_t>(std::clamp(v / al, 0.0f, 1.0f) * 255.0f + 0.5f); };
            out[i] = (static_cast<uint32_t>(al * 255.0f + 0.5f) << 24) | (ch(r[i]) << 16) | (ch(g[i]) << 8) | ch(b[i]);
        }
        return out;
    }
};

// Outlined, shadowed text with its top-left at (x, y).
void DrawText(Canvas& c, const raster::Font& font, const std::wstring& s, float x, float y, uint32_t rgb)
{
    if (s.empty())
        return;
    const int w = static_cast<int>(std::ceil(font.TextWidth(s.c_str(), static_cast<int>(s.size())))) + 2 * kPad;
    const int h = static_cast<int>(std::ceil(font.Height())) + 2 * kPad;
    const raster::Mask fill = raster::Rasterize(font, s.c_str(), static_cast<int>(s.size()), kPad, kPad, w, h);
    const raster::Mask outline = raster::Dilate(fill, g_outline);
    raster::Mask edge = outline;
    const raster::Mask shadow = raster::Shadow(outline, g_shadowOffset, g_shadowBlur);
    for (size_t i = 0; i < edge.a.size(); ++i)
        edge.a[i] = std::max(outline.a[i], kShadowAlpha * shadow.a[i]);
    const int x0 = static_cast<int>(std::lround(x)) - kPad;
    const int y0 = static_cast<int>(std::lround(y)) - kPad;
    c.Over(x0, y0, edge, 0x000000, 1.0f);
    c.Over(x0, y0, fill, rgb, 1.0f);
}

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Shortens s with an ellipsis until it fits maxW pixels.
std::wstring Fit(const raster::Font& font, std::wstring s, float maxW)
{
    if (font.TextWidth(s.c_str(), static_cast<int>(s.size())) <= maxW)
        return s;
    while (s.size() > 1) {
        s.pop_back();
        const std::wstring t = s + L"\x2026";
        if (font.TextWidth(t.c_str(), static_cast<int>(t.size())) <= maxW)
            return t;
    }
    return s;
}

// --- the original nametag's colours (client 0x10024E14, 0x10024AF1) --

// Org line colour by Side (stat 33).
uint32_t SideRgb(int side)
{
    switch (side) {
    case 2: return 0xAA88AA;
    case 1: return 0x8888AA;
    default: return 0x88AAAA;
    }
}

// Health-bar colour from N3Msg_Consider, as the selected-target plate does it: green -> yellow ->
// red with the level difference, grey below the range, white when not rated. `show` is false when
// the target can't be considered (the game draws no bar then).
uint32_t ConsiderRgb(const game::Identity& id, bool* show)
{
    float f = 0.0f;
    const int consider = n3::Consider(id, &f);
    *show = consider != 0;
    if (consider != 3)
        return 0xFFFFFF; // Indicator +0x28 default
    if (f > 1.0f)
        f = 1.0f;
    if (f < 0.0f)
        return 0xAAAAAA;
    if (f < 0.5f)
        return (static_cast<uint32_t>(f * 255.0f * 2.0f) << 16) | 0x00FF00;
    return 0xFF0000 | (static_cast<uint32_t>((1.0f - f) * 240.0f * 2.0f) << 8);
}

// Name colour: colour code from Flags bits 22/23 (table at GUI 0x1026349C) or 0xC for
// always-lootable characters, resolved through the GUI's FontSystem_t; else the Indicator's
// text colour (+0x34, white).
uint32_t NameRgb(const game::Identity& id, uint8_t* ind)
{
    static const int kCodes[4] = {0, 0x1F, 0x0A, 0x0A};
    int code = 0;
    if (id.type == kTypeCharacter) {
        const int flags = n3::GetSkill(id, n3::kStatFlags);
        if (flags != n3::kNoValue)
            code = kCodes[(static_cast<uint32_t>(flags) >> 22) & 3];
    }
    if (code == 0 && !n3::IsNpc(id)) {
        const int lootable = n3::GetSkill(id, n3::kStatHasAlwaysLootable);
        if (lootable != 0 && lootable != n3::kNoValue)
            code = 0x0C;
    }
    return code ? n3::ColorOf(code) : Field<uint32_t>(ind, kIndTextRgb) & 0xFFFFFF;
}

// A piece of a name in one colour.
struct TextRun {
    std::wstring text;
    uint32_t rgb;
};

// The name as the game's tag shows it (GUI 0x10024E14): the title first ("Title ** Name **");
// colour codes inside the name (byte 0x10, then the colour code, e.g. a pet named
// "\x10\x18DEADLY \x10\x10TOASTER") switch the colour for the rest of it; a player flagged for PvP
// (stat 224 bits 0x4000001) gets "** name **", one with stat 673 bit 9 "= name =". Other control
// bytes are dropped.
std::vector<TextRun> NameRuns(const game::Identity& id, uint8_t* ind)
{
    const uint32_t base = NameRgb(id, ind);
    std::string before, after;
    if (Cfg().np.showOrg) {
        const std::string title = n3::Title(id);
        if (!title.empty())
            before = title + " ";
    }
    if (id.type == kTypeCharacter && !n3::IsNpc(id)) {
        const int pvp = n3::GetSkill(id, kStatPvpFlags);
        const int marks = n3::GetSkill(id, kStatNameMarks);
        if (pvp != n3::kNoValue && (static_cast<uint32_t>(pvp) & 0x4000001) != 0) {
            before += "** ";
            after = " **";
        } else if (marks != n3::kNoValue && ((static_cast<uint32_t>(marks) >> 9) & 1) != 0) {
            before += "= ";
            after = " =";
        }
    }
    std::vector<TextRun> runs;
    uint32_t rgb = base;
    std::string cur = before;
    auto flush = [&] {
        if (!cur.empty())
            runs.push_back({Widen(cur), rgb});
        cur.clear();
    };
    const std::string raw = n3::Name(id);
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw[i]);
        if (ch == 0x10 && i + 1 < raw.size()) {
            flush();
            rgb = n3::ColorOf(static_cast<unsigned char>(raw[++i]));
        } else if (ch >= 0x20) {
            cur += raw[i];
        }
    }
    flush();
    rgb = base;
    cur = after;
    flush();
    return runs;
}

float RunsWidth(const raster::Font& font, const std::vector<TextRun>& runs)
{
    float w = 0.0f;
    for (const TextRun& r : runs)
        w += font.TextWidth(r.text.c_str(), static_cast<int>(r.text.size()));
    return w;
}

// Too wide: the whole name in its first colour, shortened with an ellipsis.
std::vector<TextRun> FitRuns(const raster::Font& font, std::vector<TextRun> runs, float maxW)
{
    if (runs.empty() || RunsWidth(font, runs) <= maxW)
        return runs;
    std::wstring all;
    for (const TextRun& r : runs)
        all += r.text;
    return {{Fit(font, all, maxW), runs.front().rgb}};
}

void DrawRuns(Canvas& c, const raster::Font& font, const std::vector<TextRun>& runs, float x, float y)
{
    for (const TextRun& r : runs) {
        DrawText(c, font, r.text, x, y, r.rgb);
        x += font.TextWidth(r.text.c_str(), static_cast<int>(r.text.size()));
    }
}

bool EnsureFonts()
{
    if (!g_fontsTried) {
        g_fontsTried = true;
        // Verdana, the GUI's TrueType face (FontSystem_t fonts 4-11).
        const NameplateConfig& np = Cfg().np;
        g_scale = std::clamp(np.maxPx / (kDesignNameEm * kCapPerEm), 0.35f, 1.5f);
        g_outline = S(2);
        g_shadowOffset = S(2);
        g_shadowBlur = S(2);
        g_barW = S(340);
        g_barH = S(38);
        g_barBorder = S(2);
        g_fontsOk = g_nameFont.Create(np.fontName, S(40), np.fontWeight) &&
                    g_smallFont.Create(np.fontName, S(28), np.fontWeight) &&
                    g_pctFont.Create(np.fontName, S(30), np.fontWeight) &&
                    g_levelFont.Create(np.fontName, S(44), std::max(np.fontWeight, 700)) && // the level in bold
                    g_classicFont.Create(np.fontName, S(40), std::max(np.fontWeight, 700)); // Classic: the game's bold names
        Log("[np] text drawn at %.2fx (name %d px for MaxPx %.0f)", g_scale, S(40), np.maxPx);
        if (!g_fontsOk)
            Log("[np] font creation failed - nameplates unchanged");
    }
    return g_fontsOk;
}

void FillRect(std::vector<uint32_t>& px, int x0, int y0, int w, int h, uint32_t argb);
void Composite(std::vector<uint32_t>& px, const Canvas& c);

// Fills a rectangle (texels) of a mask whose (0, 0) is texel (ox, oy).
void MaskRect(raster::Mask& mask, int ox, int oy, int x, int y, int w, int h)
{
    for (int j = std::max(0, y - oy); j < std::min(mask.h, y - oy + h); ++j)
        for (int i = std::max(0, x - ox); i < std::min(mask.w, x - ox + w); ++i)
            mask.a[static_cast<size_t>(j) * mask.w + i] = 1.0f;
}

// Corner brackets of a box into a mask whose (0, 0) is texel (ox, oy). Arms along the top and
// bottom are `arm` texels (at most a third of the width); the ones down the sides stop at about
// a third of the height, so the sides stay open in the middle.
void Brackets(raster::Mask& mask, int ox, int oy, int l, int t, int r, int b, int arm, int thick)
{
    const int armH = std::min(arm, (r - l) / 3);
    const int armV = std::min(arm, std::max(thick * 2, (b - t) * 3 / 10));
    MaskRect(mask, ox, oy, l, t, armH, thick);                MaskRect(mask, ox, oy, l, t, thick, armV);
    MaskRect(mask, ox, oy, r - armH, t, armH, thick);         MaskRect(mask, ox, oy, r - thick, t, thick, armV);
    MaskRect(mask, ox, oy, l, b - thick, armH, thick);        MaskRect(mask, ox, oy, l, b - armV, thick, armV);
    MaskRect(mask, ox, oy, r - armH, b - thick, armH, thick); MaskRect(mask, ox, oy, r - thick, b - armV, thick, armV);
}

// "Attacking you": red brackets a little outside the target frame (box l, t, r, b), blinked by
// Upload. Built as fill + outline masks covering just their box, so blinking repaints only that.
// Returns the row below them.
int AttackBrackets(Plate& p, int l, int t, int r, int b, int e, int arm, int thick)
{
    const int al = std::max(0, l - e), ar = std::min(kTexW, r + e);
    const int at = std::max(0, t - e), ab = std::min(kTexH, b + e);
    const int o = g_outline;
    p.atkX = al - o;
    p.atkY = at - o;
    p.atkFill = raster::Mask(ar - al + 2 * o, ab - at + 2 * o);
    Brackets(p.atkFill, p.atkX, p.atkY, al, at, ar, ab, arm, thick);
    p.atkOutline = raster::Dilate(p.atkFill, o);
    return ab;
}

// The org line, centred at row y; returns the row below it. (The title is on the name line, as in
// the game's tag.)
float OrgLine(Canvas& c, const game::Identity& id, float y, float* halfW)
{
    if (!Cfg().np.showOrg)
        return y;
    const std::string second = n3::Org(id);
    if (second.empty())
        return y;
    const std::wstring line = Fit(g_smallFont, Widen(second), static_cast<float>(kTexW - 4 * kPad));
    const float w = g_smallFont.TextWidth(line.c_str(), static_cast<int>(line.size()));
    DrawText(c, g_smallFont, line, (kTexW - w) * 0.5f, y, SideRgb(n3::GetSkill(id, n3::kStatSide)));
    *halfW = std::max(*halfW, w * 0.5f);
    return y + g_smallFont.Height();
}

// Classic: the game's own tags, sharp at any size. The name alone in bold (no level); your nano
// target gets teal corners at the top, your fight target red corners at the bottom (the same
// character both), and target plates a thin health bar. Every plate keeps room for the bar and both
// corner sets (and the "attacking you" brackets), so a plate doesn't move when it becomes a target.
constexpr uint32_t kClassicNanoRgb = 0x4696A5;
constexpr uint32_t kClassicFightRgb = 0xE11414;
void RenderClassic(Plate& p, const game::Identity& id, uint8_t* ind)
{
    const NameplateConfig& cfg = Cfg().np;
    Canvas c;
    const bool corpse = id.type != kTypeCharacter;
    const std::vector<TextRun> name = FitRuns(g_classicFont, NameRuns(id, ind), static_cast<float>(kTexW - 4 * kPad) * 0.8f);
    const float nameW = RunsWidth(g_classicFont, name);
    const float asc = g_classicFont.Ascent();
    const float cap = asc * 0.72f;
    auto U = [&](float f) { return std::max(1, static_cast<int>(std::lround(cap * f))); }; // in cap heights

    const int th = std::max(3, U(0.22f));     // corner thickness
    const int armH = U(1.15f), armV = U(0.8f); // corner arms
    const int padX = U(1.4f);                 // name to the corners' outer edge
    const int atkTh = th + std::max(1, S(2));
    const int e = atkTh + std::max(1, S(2));  // the "attacking you" brackets outside the corners
    const int barH = std::max(4, U(0.3f)), barBorder = std::max(1, S(2));

    // Name: its capitals' top a little below the corners' top edge.
    const float nameTop = static_cast<float>(kPad + g_outline + e + th) + cap * 0.15f - (asc - cap);
    const float capTop = nameTop + asc - cap;
    const float baseline = nameTop + asc;
    DrawRuns(c, g_classicFont, name, (kTexW - nameW) * 0.5f, nameTop);

    // Bar slot (drawn only on target plates, as the game does).
    p.level.clear();
    p.barH = barH;
    p.barBorder = barBorder;
    p.barLabel = false;
    p.barW = std::max(U(3.0f), static_cast<int>(nameW * 0.3f));
    p.barY = -1;
    if (p.role != Role::Normal && cfg.showHealth && !corpse) {
        const int life = n3::GetSkill(id, n3::kStatLife);
        if (life > 0 && life != n3::kNoValue)
            p.barY = static_cast<int>(baseline + cap * 0.8f);
    }

    // The corner box.
    const int halfBox = static_cast<int>(std::ceil(nameW * 0.5f)) + padX;
    const int left = std::max(0, kTexW / 2 - halfBox), right = kTexW - left;
    const int top = static_cast<int>(std::floor(capTop - cap * 0.15f)) - th;
    const int bottom = std::min(kTexH, static_cast<int>(std::ceil(baseline + cap * 2.6f)));
    if (p.role != Role::Normal) {
        const bool nano = p.role == Role::Selected;
        const bool fight = p.role == Role::Fighting || p.alsoFight;
        raster::Mask nanoMask(kTexW, kTexH), fightMask(kTexW, kTexH);
        if (nano) {
            MaskRect(nanoMask, 0, 0, left, top, armH, th);         MaskRect(nanoMask, 0, 0, left, top, th, armV);
            MaskRect(nanoMask, 0, 0, right - armH, top, armH, th); MaskRect(nanoMask, 0, 0, right - th, top, th, armV);
        }
        if (fight) {
            MaskRect(fightMask, 0, 0, left, bottom - th, armH, th);         MaskRect(fightMask, 0, 0, left, bottom - armV, th, armV);
            MaskRect(fightMask, 0, 0, right - armH, bottom - th, armH, th); MaskRect(fightMask, 0, 0, right - th, bottom - armV, th, armV);
        }
        raster::Mask both(kTexW, kTexH);
        for (size_t i = 0; i < both.a.size(); ++i)
            both.a[i] = std::max(nanoMask.a[i], fightMask.a[i]);
        c.Over(0, 0, raster::Dilate(both, std::max(1, g_outline / 2)), 0x000000, 0.8f);
        c.Over(0, 0, nanoMask, kClassicNanoRgb, 1.0f);
        c.Over(0, 0, fightMask, kClassicFightRgb, 1.0f);
    }

    float visHalfW = std::max(nameW * 0.5f, p.barW * 0.5f + barBorder);
    float y = static_cast<float>(AttackBrackets(p, left, top, right, bottom, e, armH + e + S(8), atkTh) + S(4));
    y = OrgLine(c, id, y, &visHalfW);

    p.anchorRow = static_cast<int>(std::ceil(y)) + 2;
    p.nameMidRow = capTop + cap * 0.5f;
    p.hitHalfW = visHalfW;
    p.visTop = capTop;
    p.visBottom = static_cast<float>(p.anchorRow - 2);
    p.text = c.ToArgb();
    p.nameCap = std::max(1.0f, cap);
    p.hpPct = -1;
    p.barRgb = 0;
}

// Modern layout: the name centred over the health bar (percentage inside), the level larger to the
// left of both and vertically centred on them, and the org/title line below.
void RenderText(Plate& p, const game::Identity& id, uint8_t* ind)
{
    if (Cfg().np.style == kStyleClassic) {
        RenderClassic(p, id, ind);
        return;
    }
    const NameplateConfig& cfg = Cfg().np;
    Canvas c;
    const float maxW = static_cast<float>(kTexW - 4 * kPad);

    // Only characters (Identity type 50000) have level and health; corpses ("Remains of ...") are
    // Corpse_t dynels of type 51050 and get just their name.
    const bool corpse = id.type != kTypeCharacter;

    std::wstring level;
    if (cfg.showLevel && !corpse) {
        const int lvl = n3::GetSkill(id, n3::kStatLevel);
        if (lvl > 0 && lvl != n3::kNoValue)
            level = std::to_wstring(lvl);
    }
    const float gap = level.empty() ? 0.0f : static_cast<float>(S(12));
    const float levelW = level.empty() ? 0.0f : g_levelFont.TextWidth(level.c_str(), static_cast<int>(level.size()));
    // The name is centred on the plate; the level sits beside the name and bar, so it only limits
    // the name when the name is wider than the bar.
    const std::vector<TextRun> name = FitRuns(g_nameFont, NameRuns(id, ind), maxW - 2.0f * (levelW + gap));
    const float nameW = RunsWidth(g_nameFont, name);

    const int m = S(10);               // bracket margin, same on every side
    const int th = std::max(4, S(8));  // target bracket thickness: bold, readable at a distance
    const int len = S(52);             // target bracket arm length
    const int atkTh = th + std::max(1, S(2)); // the "attacking you" brackets: slightly thicker ...
    const int e = atkTh;               // ... and right outside the target ones
    // Every plate has room for both bracket sets, so a plate doesn't move when it becomes a target or
    // starts being attacked.
    const float nameTop = static_cast<float>(kPad + g_outline + e + m + th);
    float y = nameTop;
    const float nameLineW = nameW;
    DrawRuns(c, g_nameFont, name, (kTexW - nameW) * 0.5f, y);
    y += g_nameFont.Height();
    float boxBottom = nameTop + g_nameFont.Ascent(); // without a bar: the name's baseline
    // Visible extent (texels): top of the name's capitals to the lowest thing drawn, and the
    // widest thing drawn - what stacking and clicking measure.
    float visTop = nameTop + g_nameFont.Ascent() * (1.0f - 0.72f);
    float visBottom = nameTop + g_nameFont.Height(); // the name's descenders
    float visHalfW = nameLineW * 0.5f;

    // Health bar slot, right under the name: the usual width, or as wide as the name plus a little
    // so a long name never overhangs it.
    p.barY = -1;
    p.barW = std::max(g_barW, static_cast<int>(std::ceil(nameW)) + 2 * S(8));
    p.barH = g_barH;
    p.barBorder = g_barBorder;
    p.barLabel = true;
    if (cfg.showHealth && !corpse) {
        const int life = n3::GetSkill(id, n3::kStatLife);
        if (life > 0 && life != n3::kNoValue) {
            p.barY = static_cast<int>(y) + S(2);
            boxBottom = static_cast<float>(p.barY + g_barH + g_barBorder);
            visBottom = boxBottom;
            visHalfW = std::max(visHalfW, p.barW * 0.5f + g_barBorder);
            y = static_cast<float>(p.barY + g_barH + S(4));
        }
    }

    // The level: left of the name and bar, its capitals centred on them vertically, in the bar's
    // (consider) colour - so Upload draws it. Kept inside the texture if a very large MaxPx leaves
    // less room than it needs.
    p.level = level;
    if (!level.empty()) {
        float blockLeft = (kTexW - nameW) * 0.5f;
        if (p.barY >= 0)
            blockLeft = std::min(blockLeft, static_cast<float>((kTexW - p.barW) / 2 - g_barBorder));
        p.levelX = std::max(static_cast<float>(kPad), blockLeft - gap - levelW);
        const float centre = (visTop + boxBottom) * 0.5f;
        const float capH = g_levelFont.Ascent() * 0.72f;
        p.levelY = centre - g_levelFont.Ascent() + capH * 0.5f;
        visHalfW = std::max(visHalfW, kTexW * 0.5f - p.levelX);
    }

    // The bracket frame: the level, name and bar with the same margin on every side, symmetric about
    // the texture centre (where name and bar are centred).
    const int left = std::max(0, kTexW / 2 - static_cast<int>(std::ceil(visHalfW)) - m - th);
    const int right = kTexW - left; // exclusive, mirrors left exactly
    const float capTop = nameTop + g_nameFont.Ascent() - g_nameFont.Ascent() * 0.72f;
    const int top = std::max(0, static_cast<int>(std::floor(capTop)) - m - th);
    const int bottom = std::min(kTexH, static_cast<int>(std::ceil(boxBottom)) + m + th); // exclusive
    // Selected (nano) / fight target: the brackets themselves. Fight orange stays apart from the
    // "attacking you" red.
    if (p.role != Role::Normal) {
        const uint32_t rgb = p.role == Role::Fighting ? 0xFF8A1A : (p.alsoFight ? 0xFFD21F : 0xFFFFFF);
        raster::Mask mask(kTexW, kTexH);
        Brackets(mask, 0, 0, left, top, right, bottom, len, th);
        c.Over(0, 0, raster::Dilate(mask, g_outline), 0x000000, 1.0f);
        c.Over(0, 0, mask, rgb, 1.0f);
    }

    // "Attacking you": longer arms and slightly thicker than the target brackets. Below them, title
    // and org.
    y = std::max(y, static_cast<float>(AttackBrackets(p, left, top, right, bottom, e, len + e + S(12), atkTh) + S(4)));
    y = OrgLine(c, id, y, &visHalfW);
    // The row that sits at the game's anchor: the plate's bottom edge. The same for a character's
    // normal, selected and attacking plate, since they share the layout.
    p.anchorRow = static_cast<int>(std::ceil(y)) + 2;

    // Footprint for stacking and clicking: name capitals down to the plate's bottom edge.
    visBottom = static_cast<float>(p.anchorRow - 2);
    p.hitHalfW = visHalfW;
    p.visTop = visTop;
    p.visBottom = visBottom;
    // A faint dark panel behind level, name and bar (the bracket box), so plates stand out against
    // bright backgrounds.
    const float panel = std::clamp(cfg.backgroundOpacity, 0.0f, 1.0f);
    if (panel > 0.0f) {
        p.text.assign(static_cast<size_t>(kTexW) * kTexH, 0);
        FillRect(p.text, left, top, right - left, bottom - top, static_cast<uint32_t>(panel * 255.0f + 0.5f) << 24);
        Composite(p.text, c);
    } else {
        p.text = c.ToArgb();
    }
    p.nameCap = std::max(1.0f, g_nameFont.Ascent() * 0.72f); // cap height ~ 0.72 of the ascent
    p.hpPct = -1;
    p.barRgb = 0;
}

void FillRect(std::vector<uint32_t>& px, int x0, int y0, int w, int h, uint32_t argb)
{
    for (int y = std::max(0, y0); y < std::min(kTexH, y0 + h); ++y)
        for (int x = std::max(0, x0); x < std::min(kTexW, x0 + w); ++x)
            px[static_cast<size_t>(y) * kTexW + x] = argb;
}

// Straight-alpha "over" of one colour through a mask onto ARGB pixels, touching only the mask's
// box (the blinking brackets repaint just that much).
void OverMask(std::vector<uint32_t>& px, int x0, int y0, const raster::Mask& m, uint32_t rgb, float alpha)
{
    for (int y = 0; y < m.h; ++y) {
        const int ty = y0 + y;
        if (ty < 0 || ty >= kTexH)
            continue;
        for (int x = 0; x < m.w; ++x) {
            const int tx = x0 + x;
            const float sa = m.a[static_cast<size_t>(y) * m.w + x] * alpha;
            if (tx < 0 || tx >= kTexW || sa <= 0.0f)
                continue;
            uint32_t& d = px[static_cast<size_t>(ty) * kTexW + tx];
            const float da = (d >> 24) / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            uint32_t out = static_cast<uint32_t>(oa * 255.0f + 0.5f) << 24;
            for (int shift = 0; shift <= 16; shift += 8) {
                const float sc = ((rgb >> shift) & 0xFF) / 255.0f;
                const float dc = ((d >> shift) & 0xFF) / 255.0f;
                const float oc = (sc * sa + dc * da * (1.0f - sa)) / oa;
                out |= static_cast<uint32_t>(std::clamp(oc, 0.0f, 1.0f) * 255.0f + 0.5f) << shift;
            }
            d = out;
        }
    }
}

// Straight-alpha "over" of a canvas onto ARGB pixels.
void Composite(std::vector<uint32_t>& px, const Canvas& c)
{
    const std::vector<uint32_t> top = c.ToArgb();
    for (size_t i = 0; i < px.size(); ++i) {
        const uint32_t s = top[i];
        const float sa = (s >> 24) / 255.0f;
        if (sa <= 0.0f)
            continue;
        const uint32_t d = px[i];
        const float da = (d >> 24) / 255.0f;
        const float oa = sa + da * (1.0f - sa);
        uint32_t out = static_cast<uint32_t>(oa * 255.0f + 0.5f) << 24;
        for (int shift = 0; shift <= 16; shift += 8) {
            const float sc = ((s >> shift) & 0xFF) / 255.0f;
            const float dc = ((d >> shift) & 0xFF) / 255.0f;
            const float oc = (sc * sa + dc * da * (1.0f - sa)) / oa;
            out |= static_cast<uint32_t>(std::clamp(oc, 0.0f, 1.0f) * 255.0f + 0.5f) << shift;
        }
        px[i] = out;
    }
}

// Text + health bar -> texture. Bar: the game's consider colour on its 0x333333 track, black
// border, health percentage centred inside. `tenths` is 0..1000 (percent x 10), -1 = unknown.
void Upload(Plate& p, int tenths, uint32_t barRgb)
{
    std::vector<uint32_t> px = p.text;
    if (p.atkLevel > 0 && !p.atkFill.a.empty()) {
        const float strength = static_cast<float>(p.atkLevel) / kBlinkSteps;
        OverMask(px, p.atkX, p.atkY, p.atkOutline, 0x000000, 0.9f * strength);
        OverMask(px, p.atkX, p.atkY, p.atkFill, 0xFF2A2A, strength);
    }
    if (p.barY >= 0 && tenths >= 0) {
        const int x0 = (kTexW - p.barW) / 2;
        FillRect(px, x0 - p.barBorder, p.barY - p.barBorder, p.barW + 2 * p.barBorder, p.barH + 2 * p.barBorder, 0xF0000000);
        FillRect(px, x0, p.barY, p.barW, p.barH, 0xFF333333);
        FillRect(px, x0, p.barY, p.barW * tenths / 1000, p.barH, 0xFF000000 | barRgb);
    }
    if (p.barY >= 0 && tenths >= 0 && p.barLabel) {

        wchar_t label[16];
        swprintf_s(label, L"%d.%d%%", tenths / 10, tenths % 10);
        const int len = static_cast<int>(wcslen(label));
        Canvas c;
        const float w = g_pctFont.TextWidth(label, len);
        DrawText(c, g_pctFont, label, (kTexW - w) * 0.5f, p.barY + (p.barH - g_pctFont.Height()) * 0.5f, 0xFFFFFF);
        Composite(px, c);
    }
    if (!p.level.empty()) {
        Canvas c;
        DrawText(c, g_levelFont, p.level, p.levelX, p.levelY, barRgb);
        Composite(px, c);
    }
    game::WriteTexture(game::VisualMaterialTexture(p.vm), px.data(), kTexW, kTexH);
    p.hpPct = tenths;
    p.barRgb = barRgb;
}

// Health in tenths of a percent (health capped at max, max >= 1, as the game's bar does).
int HealthTenths(const game::Identity& id)
{
    const int hp = n3::GetSkill(id, n3::kStatHealth);
    int life = n3::GetSkill(id, n3::kStatLife);
    if (hp == n3::kNoValue || life == n3::kNoValue)
        return -1;
    if (life <= 0)
        life = 1;
    return std::clamp(static_cast<int>(1000.0 * std::min(hp, life) / life), 0, 1000);
}

// A fully saturated colour of the given hue (degrees).
uint32_t HueRgb(float hue)
{
    const float h = std::fmod(std::max(0.0f, hue), 360.0f) / 60.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    switch (static_cast<int>(h)) {
    case 0: r = 1.0f; g = x; break;
    case 1: r = x; g = 1.0f; break;
    case 2: g = 1.0f; b = x; break;
    case 3: g = x; b = 1.0f; break;
    case 4: r = x; b = 1.0f; break;
    default: r = 1.0f; b = x; break;
    }
    auto ch = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f); };
    return (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

// Bar colours darker (BarBrightness) and a little less saturated than the pure ones, easier on the eye.
constexpr float kBarDesaturate = 0.2f;
uint32_t Soften(uint32_t rgb)
{
    const float brightness = std::clamp(Cfg().np.barBrightness, 0.3f, 1.0f);
    const float r = ((rgb >> 16) & 0xFF) / 255.0f, g = ((rgb >> 8) & 0xFF) / 255.0f, b = (rgb & 0xFF) / 255.0f;
    const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
    auto ch = [&](float c) {
        const float v = (c + (lum - c) * kBarDesaturate) * brightness;
        return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

// Current bar state for the plate; returns false if nothing needs redrawing. The colour is also the
// level's, so it's worked out for plates without a bar too (health switched off).
bool BarChanged(const Plate& p, const game::Identity& id, int* tenths, uint32_t* rgb)
{
    if (p.barY < 0 && p.level.empty())
        return false;
    // Classic: the game's consider colours (by level difference, grey for those it can't rate). Or one
    // colour for everyone, or by health: green at full health, yellow at half, red at none.
    const NameplateConfig& cfg = Cfg().np;
    *tenths = p.barY >= 0 ? HealthTenths(id) : -1;
    if (cfg.barColor == 1) {
        *rgb = HueRgb(cfg.barHue);
    } else if (cfg.barColor == 2) {
        const int health = *tenths >= 0 ? *tenths : HealthTenths(id);
        *rgb = HueRgb(120.0f * std::clamp(health, 0, 1000) / 1000.0f);
    } else {
        bool considerable = false;
        *rgb = ConsiderRgb(id, &considerable);
        if (!considerable)
            *rgb = 0xAAAAAA; // the game's grey for those it can't rate (e.g. other players), as it is
    }
    if (*rgb != 0xAAAAAA)
        *rgb = Soften(*rgb);
    return *tenths != p.hpPct || *rgb != p.barRgb;
}

bool HasValue(int v)
{
    return v != 0 && v != n3::kNoValue;
}

// Pets: the client doesn't get PetMaster/OwnerInstance/PetType for pets (all "no value"/0 in a
// Debug=1 capture, 2026-10-21); NPCFamily separates them - players' pets were 95/96/97/98, other
// NPCs 0/36; the Devileet vanity pet is 221. The list is built in (config.h) plus PetFamilies in Clarity.ini.
bool IsSelf(const game::Identity& id)
{
    return id.type == kTypeCharacter && id.instance == game::ClientInstance();
}

Kind Classify(const game::Identity& id)
{
    if (IsSelf(id))
        return Kind::Self;
    const bool npc = n3::IsNpc(id);
    const int family = n3::GetSkill(id, n3::kStatNpcFamily);
    if (Cfg().fct.debug)
        Log("[np] plate '%s' %u:%u npc=%d NPCFamily=%d", n3::Name(id).c_str(), id.type, id.instance, npc, family);
    if (!npc)
        return Kind::Player;
    const NameplateConfig& cfg = Cfg().np;
    for (int i = 0; i < cfg.petFamilyCount; ++i) {
        if (cfg.petFamilies[i] == family)
            return Kind::Pet;
    }
    // The pet window's own test, for the client's pets.
    const int master = n3::GetSkill(id, n3::kStatPetMaster);
    if (HasValue(master) && static_cast<uint32_t>(master) == game::ClientInstance())
        return Kind::Pet;
    return Kind::Npc;
}

bool KindVisible(Kind k)
{
    const NameplateConfig& cfg = Cfg().np;
    switch (k) {
    case Kind::Player: return cfg.showPlayers;
    case Kind::Npc: return cfg.showNpcs;
    case Kind::Pet: return cfg.showPets;
    case Kind::Self: return cfg.showSelf;
    }
    return true;
}

void* Sprite(uint8_t* ind)
{
    void* visualSprite = Field<void*>(ind, kIndSprite);
    return visualSprite ? *static_cast<void**>(visualSprite) : nullptr; // VisualSprite_t { RSprite* }
}

// --- hooks ----------------------------------------------------------------------------------

uint64_t Key(const game::Identity& id)
{
    return (static_cast<uint64_t>(id.type) << 32) | id.instance;
}

// Stacking order of two overlapping plates (keyed by their identities, lower key first). It is kept
// while they keep overlapping, so NPCs walking around each other don't make their plates trade
// places every time their distances cross.
struct PairOrder {
    bool lowFirst; // the plate with the lower Key keeps its spot; the other moves above it
    DWORD tick;    // when the two last overlapped
};
std::map<std::pair<uint64_t, uint64_t>, PairOrder> g_pairOrder;

// Whether the two plates were stacked within kStackHoldMs (overlapping, or within the keep margin).
bool RecentlyStacked(const game::Identity& a, const game::Identity& b, DWORD now)
{
    auto it = g_pairOrder.find(std::minmax(Key(a), Key(b)));
    return it != g_pairOrder.end() && now - it->second.tick < kStackHoldMs;
}

// Whether q's plate keeps its spot and me's moves above it. dz = q's depth - me's depth.
// overlapping: they overlap now (refreshes the pair's time); false while only held stacked.
bool StackFirst(const game::Identity& q, const game::Identity& me, float dz, DWORD now, bool overlapping)
{
    const uint64_t kq = Key(q), km = Key(me);
    // Nearer goes first; about the same distance goes by identity.
    bool qFirst = std::fabs(dz) > kStackDepthTie ? dz < 0.0f : kq < km;
    const std::pair<uint64_t, uint64_t> pair = std::minmax(kq, km);
    auto it = g_pairOrder.find(pair);
    if (it != g_pairOrder.end() && now - it->second.tick < kPairForgetMs) {
        const bool kept = (kq < km) == it->second.lowFirst;
        if (kept != qFirst && std::fabs(dz) < kStackSwapDepth)
            qFirst = kept; // not clearly reversed yet: keep the order
    }
    PairOrder& order = g_pairOrder[pair];
    order.lowFirst = (kq < km) == qFirst;
    if (overlapping || !order.tick)
        order.tick = now;
    if (g_pairOrder.size() > 512) {
        for (auto old = g_pairOrder.begin(); old != g_pairOrder.end();)
            old = now - old->second.tick > kPairForgetMs ? g_pairOrder.erase(old) : std::next(old);
    }
    return qFirst;
}

// Makes TargetingModule_t rescan at its next frame, as its own pref callbacks do (timer +0xC set
// to 100), so a character whose selected/fight-target plate went away gets its
// ordinary tag back at once instead of after up to 2 s.
void ForceRescan()
{
    if (!g_targetingInstance || !*g_targetingInstance)
        return;
    *reinterpret_cast<float*>(static_cast<uint8_t*>(*g_targetingInstance) + kTargetingScanTimer) = 100.0f;
}

// Your own nameplate (ShowSelf). The 2 s scan (TargetingModule_t::HandleNametags) lists characters
// with N3Msg_GetDynelsInVicinity(list, includeSelf = false, ...); through GUI.dll's import slot,
// that one call gets includeSelf = true, so the game makes an Indicator for you like for anyone.
using VicinityFn = void(__thiscall*)(void* self, void* list, bool includeSelf, bool all, int type);
VicinityFn g_nextVicinity = nullptr;
uint8_t* g_handleNametags = nullptr;
constexpr uintptr_t kHandleNametagsSize = 0x20E; // GUI 0x10025D96..0x10025FA4
bool g_selfScanned = false;                      // ShowSelf as of the last rescan

bool WantSelf()
{
    const NameplateConfig& cfg = Cfg().np;
    return cfg.enable && cfg.showSelf;
}

void __fastcall Hook_Vicinity(void* self, void* /*edx*/, void* list, bool includeSelf, bool all, int type)
{
    auto* ret = static_cast<uint8_t*>(_ReturnAddress());
    if (g_handleNametags && ret > g_handleNametags && ret < g_handleNametags + kHandleNametagsSize && WantSelf())
        includeSelf = true;
    g_nextVicinity(self, list, includeSelf, all, type);
}

void* __fastcall Hook_Delete(void* self, void* /*edx*/, unsigned flags)
{
    auto it = g_plates.find(static_cast<uint8_t*>(self));
    void* vm = it != g_plates.end() ? it->second.vm : nullptr;
    if (it != g_plates.end()) {
        const DWORD now = GetTickCount();
        if (g_lastPlate.size() > 256) {
            for (auto old = g_lastPlate.begin(); old != g_lastPlate.end();)
                old = now - old->second.tick > 10000 ? g_lastPlate.erase(old) : std::next(old);
        }
        const Plate& gone = it->second;
        g_lastPlate[Key(gone.id)] = {now, gone.stackUp, gone.id, gone.screen, gone.priority,
                                     gone.screenTick != 0 && now - gone.screenTick < 250};
        if (it->second.role != Role::Normal)
            ForceRescan();
        g_plates.erase(it);
    }
    void* r = g_origDelete(self, flags); // releases the sprite (and its reference to our material)
    game::DestroyVisualMaterial(vm);
    return r;
}

void PatchDelete(uint8_t* ind)
{
    void** vtable = *reinterpret_cast<void***>(ind);
    if (g_indicatorVtable)
        return;
    g_origDelete = reinterpret_cast<DeleteFn>(vtable[0]);
    if (!WritePointer(&vtable[0], reinterpret_cast<void*>(&Hook_Delete)))
        return;
    g_indicatorVtable = vtable;
    Log("[np] Indicator vtable %p: deleting dtor %p hooked", vtable, g_origDelete);
}

Role RoleOf(uint8_t* ind)
{
    if (Field<uint8_t>(ind, kIndIsAttacking))
        return Role::Fighting;
    return Field<uint8_t>(ind, kIndShowFrame) ? Role::Selected : Role::Normal;
}

// Ordinary tags of the types switched off (players, NPCs, pets) keep the game's own look. Their
// type, by Indicator; checked against the identity, since an Indicator's memory can be reused
// before our delete hook is in place.
struct GameTag {
    game::Identity id{};
    Kind kind = Kind::Npc;
    unsigned version = 0;
    DWORD seen = 0;
};
std::unordered_map<uint8_t*, GameTag> g_gameTags;

bool KeepsGameTag(uint8_t* ind, const game::Identity& id)
{
    if (RoleOf(ind) != Role::Normal)
        return false; // your selected / fight target always gets a plate
    auto pit = g_plates.find(ind);
    if (pit != g_plates.end())
        return !KindVisible(pit->second.kind);
    const DWORD now = GetTickCount();
    GameTag& tag = g_gameTags[ind];
    if (tag.version != g_version.load() || tag.id.type != id.type || tag.id.instance != id.instance)
        tag = {id, Classify(id), g_version.load(), now};
    tag.seen = now;
    if (g_gameTags.size() > 512) {
        for (auto old = g_gameTags.begin(); old != g_gameTags.end();)
            old = now - old->second.seen > 10000 ? g_gameTags.erase(old) : std::next(old);
    }
    return !KindVisible(tag.kind);
}

void Build(uint8_t* ind)
{
    void* sprite = Sprite(ind);
    if (!sprite || g_buildFailed)
        return;
    if (KeepsGameTag(ind, Field<game::Identity>(ind, kIndIdentity)))
        return; // the update restores the game's tag if we had a plate here
    if (!EnsureFonts()) {
        g_buildFailed = true;
        return;
    }
    PatchDelete(ind);
    if (!g_indicatorVtable || *reinterpret_cast<void***>(ind) != g_indicatorVtable)
        return; // without the delete hook we couldn't free our texture

    const game::Identity id = Field<game::Identity>(ind, kIndIdentity);
    Plate& p = g_plates[ind];
    if (!p.vm) {
        p.vm = game::CreateVisualMaterial(kTexW, kTexH, "Clarity nameplate");
        if (!p.vm) {
            g_plates.erase(ind);
            g_buildFailed = true;
            Log("[np] couldn't create a nameplate texture - leaving the game's tags");
            return;
        }
    }
    if (!p.born) {
        p.born = GetTickCount();
        // Replacing another plate of the same character (target change): no fade-in blink.
        // Also keep its stacking offset, so it doesn't drop down and glide back.
        auto last = g_lastPlate.find(Key(id));
        bool replacing = last != g_lastPlate.end() && p.born - last->second.tick < 3000;
        if (replacing)
            p.stackUp = last->second.stackUp;
        for (auto& [other, op] : g_plates) {
            if (other != ind && op.born) {
                const game::Identity oid = Field<game::Identity>(other, kIndIdentity);
                if (oid.type == id.type && oid.instance == id.instance) {
                    replacing = true;
                    p.stackUp = op.stackUp;
                }
            }
        }
        if (replacing)
            p.born -= static_cast<DWORD>(Cfg().np.fadeInSec * 1000.0f) + 1;
        // A name label's states, so renderers treat the plate like the game's own names (ao-vk draws those
        // after its shadows and effects). Drawn over the scene, so characters in front don't hide
        // it; walls and terrain hide it through the line-of-sight test in the update.
        if (!game::MaterialSetLabelStates(game::VisualMaterialMaterial(p.vm), true)) {
            static bool warned = false;
            if (!warned) {
                Log("[np] couldn't set the render states on the plate material");
                warned = true;
            }
        }
    }
    p.role = RoleOf(ind);
    p.id = id;
    p.kind = Classify(id);

    if (p.sprite != sprite) {
        // The game's quad: remember it to restore later, and which corner is which.
        auto* base = static_cast<uint8_t*>(sprite);
        const Vertex* v = reinterpret_cast<const Vertex*>(base + kSpriteVerts);
        float topSign = 1.0f, leftSign = -1.0f;
        float minV = 1e9f, minU = 1e9f;
        for (int i = 0; i < 4; ++i) {
            p.origVerts[i] = v[i];
            p.signX[i] = v[i].x >= 0.0f ? 1.0f : -1.0f;
            p.signY[i] = v[i].y >= 0.0f ? 1.0f : -1.0f;
            if (v[i].v < minV) {
                minV = v[i].v;
                topSign = p.signY[i];
            }
            if (v[i].u < minU) {
                minU = v[i].u;
                leftSign = p.signX[i];
            }
        }
        p.rightSign = -leftSign;
        p.origHalfW = *reinterpret_cast<float*>(base + kSpriteHalfW);
        p.origHalfH = *reinterpret_cast<float*>(base + kSpriteHalfH);
        p.upSign = topSign;
        p.sprite = sprite;
        // Until our update has sized and placed it, the quad still has the game's size and lift:
        // keep it hidden so it doesn't flash there for a frame. The game's update shows it again
        // right before ours runs.
        game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
    }

    RenderText(p, id, ind);
    {
        int pct = -1;
        uint32_t rgb = 0;
        BarChanged(p, id, &pct, &rgb);
        Upload(p, pct, rgb);
    }
    p.version = g_version.load();
    game::SwapSpriteMaterial(sprite, game::VisualMaterialMaterial(p.vm));
}

// Back to the game's own look: its material and quad, our texture freed.
void Restore(uint8_t* ind, std::unordered_map<uint8_t*, Plate>::iterator it)
{
    Plate& p = it->second;
    void* sprite = Sprite(ind);
    void* gameVm = Field<void*>(ind, kIndGameMaterial);
    if (sprite && sprite == p.sprite && gameVm) {
        auto* base = static_cast<uint8_t*>(sprite);
        Vertex* v = reinterpret_cast<Vertex*>(base + kSpriteVerts);
        for (int i = 0; i < 4; ++i)
            v[i] = p.origVerts[i];
        *reinterpret_cast<float*>(base + kSpriteHalfW) = p.origHalfW;
        *reinterpret_cast<float*>(base + kSpriteHalfH) = p.origHalfH;
        game::SwapSpriteMaterial(sprite, game::VisualMaterialMaterial(gameVm)); // releases ours
    } else if (sprite == p.sprite) {
        return; // can't restore safely; keep ours
    }
    void* vm = p.vm;
    g_plates.erase(it);
    game::DestroyVisualMaterial(vm);
}

void __fastcall Hook_Build(uint8_t* ind)
{
    g_origBuild(ind);
    if (Cfg().np.enable)
        Build(ind);
}

// The plate of another Indicator for the same character with the given role, if any.
Plate* Other(uint8_t* self, const game::Identity& id, Role role)
{
    for (auto& [ind, plate] : g_plates) {
        if (ind != self && plate.role == role) {
            const game::Identity oid = Field<game::Identity>(ind, kIndIdentity);
            if (oid.type == id.type && oid.instance == id.instance)
                return &plate;
        }
    }
    return nullptr;
}

int __fastcall Hook_Update(uint8_t* ind)
{
    g_gameThread = GetCurrentThreadId();
    const int keep = g_origUpdate(ind);
    if (!keep)
        return keep;
    const NameplateConfig& cfg = Cfg().np;
    if (WantSelf() != g_selfScanned) {
        g_selfScanned = WantSelf();
        ForceRescan(); // your own plate comes or goes at once
    }
    auto it = g_plates.find(ind);
    if (!cfg.enable) {
        if (it != g_plates.end())
            Restore(ind, it);
        return keep;
    }
    // Types switched off show the game's own tag, except while that character has a selected or
    // fight-target plate of ours (shown instead, so the name isn't there twice).
    const game::Identity tagId = Field<game::Identity>(ind, kIndIdentity);
    if (KeepsGameTag(ind, tagId)) {
        if (it != g_plates.end())
            Restore(ind, it);
        // Your own tag only exists for ShowSelf: hidden once it's off, until the rescan drops it.
        if (Other(ind, tagId, Role::Selected) || Other(ind, tagId, Role::Fighting) ||
            (RoleOf(ind) == Role::Normal && IsSelf(tagId)))
            game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
        return keep;
    }
    if (it == g_plates.end()) {
        Build(ind); // turned back on, or the builder ran before we were enabled
        it = g_plates.find(ind);
        if (it == g_plates.end())
            return keep;
    }
    Plate& p = it->second;
    void* sprite = Sprite(ind);
    if (sprite != p.sprite)
        return keep;

    const game::Identity id = Field<game::Identity>(ind, kIndIdentity);
    bool rerender = p.version != g_version.load() || p.hpPct == -2;
    if (p.version != g_version.load()) {
        p.kind = Classify(id);
        p.version = g_version.load();
    }

    // Selected and fight target are the same character: show one plate with a gold marker.
    if (p.role == Role::Fighting && Other(ind, id, Role::Selected)) {
        game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
        p.screenTick = 0;
        return keep;
    }
    if (p.role == Role::Selected) {
        const bool fight = Other(ind, id, Role::Fighting) != nullptr;
        if (fight != p.alsoFight) {
            p.alsoFight = fight;
            rerender = true;
        }
    }
    // An ordinary tag gives way to the same character's selected / fight-target plate.
    if (p.role == Role::Normal && (Other(ind, id, Role::Selected) || Other(ind, id, Role::Fighting))) {
        game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
        return keep; // footprint kept until it ages out, see the stacking loop
    }

    int pct = -1;
    uint32_t rgb = 0;
    if (rerender) {
        RenderText(p, id, ind);
        BarChanged(p, id, &pct, &rgb);
        Upload(p, pct, rgb);
    } else if (BarChanged(p, id, &pct, &rgb)) {
        Upload(p, pct, rgb);
    }

    // Attacking us: the red brackets, blinking slowly (full when the attack starts, fading to faint
    // and back). Not on the dead.
    {
        DWORD since = 0;
        int level = 0;
        if (threat::IsAttackingMe(id, &since) && p.hpPct != 0) {
            const float phase = static_cast<float>((GetTickCount() - since) % kBlinkMs) / kBlinkMs;
            const float strength = kBlinkLow + (1.0f - kBlinkLow) * 0.5f * (1.0f + std::cos(phase * 6.2831853f));
            level = std::max(1, static_cast<int>(std::lround(strength * kBlinkSteps)));
        }
        if (level != p.atkLevel) {
            p.atkLevel = level;
            Upload(p, p.hpPct, p.barRgb);
        }
    }

    // Size: the name's capitals are NaturalSize world units, clamped to MinPx..MaxPx on screen.
    game::Vec3 pos;
    if (!game::IndicatorPosition(id, &pos))
        return keep;
    const bool target = p.role != Role::Normal;
    // Classic is placed and moves as the game's tags do: only its size and sharpness differ.
    const bool classic = cfg.style == kStyleClassic;
    // The game's anchor follows the head, so it bobs with walking, running and fighting. Keep the
    // plate's height above the character's own position (its feet, which follow the ground without
    // the bob) and smooth only that; big changes (mounting, zoning, a new model) snap.
    float steady = 0.0f; // world units added to the anchor's height
    game::Vec3 feet;
    if (!classic && n3::Position(id, &feet)) {
        const float height = pos.y - feet.y;
        const DWORD now = GetTickCount();
        if (!p.headTick || std::fabs(height - p.headHeight) > kHeadSnap) {
            p.headHeight = height;
        } else {
            const float dt = std::min(0.1f, (now - p.headTick) / 1000.0f);
            p.headHeight += (height - p.headHeight) * std::min(1.0f, dt * kHeadFollow);
        }
        p.headTick = now;
        steady = p.headHeight - height;
        pos.y += steady;
    }
    const game::Vec3 view = game::ToView(pos);
    if (view.z < 0.1f)
        return keep;
    // Your own plate is hidden in first person (the camera is inside your head).
    if (p.kind == Kind::Self && std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z) < kFirstPersonRadius) {
        game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
        p.screenTick = 0;
        return keep;
    }
    const float ppu = game::PxPerUnit(view.z);
    // Your selected and fight targets are a little bigger than the plates around them.
    const float capPx = std::clamp(cfg.naturalSize * ppu, cfg.minPx, cfg.maxPx) * (target ? cfg.targetScale : 1.0f);
    const float worldPerTexel = capPx / p.nameCap / ppu;
    const float halfW = kTexW * 0.5f * worldPerTexel;
    const float halfH = kTexH * 0.5f * worldPerTexel;
    // Lift the quad so anchorRow sits at the game's anchor (above the head). Classic: the row at the
    // anchor is the one that puts the name's middle where the game's tag has it.
    const float anchorRow = classic && p.origHalfH > 0.0f
        ? p.nameMidRow + p.origHalfH * kClassicNameRise / worldPerTexel
        : static_cast<float>(p.anchorRow);
    const float lift = (anchorRow - kTexH * 0.5f) * worldPerTexel;

    // Fade: in over FadeInSec after the tag appears, and out between FadeStart and the game's
    // 30 m tag range (distance from the player), so tags don't pop.
    float alpha = 1.0f;
    if (cfg.fadeInSec > 0.0f)
        alpha = std::min(1.0f, (GetTickCount() - p.born) / (cfg.fadeInSec * 1000.0f));
    // Only ordinary tags fade with distance: your selected and fight targets stay readable as long
    // as the game keeps their indicator, e.g. a nano target beyond the 30 m tag range.
    game::Vec3 me;
    if (p.role == Role::Normal && cfg.fadeStart < kTagRange && n3::Position({kTypeCharacter, game::ClientInstance()}, &me)) {
        const float dx = pos.x - me.x, dz = pos.z - me.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        alpha *= std::clamp((kTagRange - d) / (kTagRange - cfg.fadeStart), 0.0f, 1.0f);
    }
    // Plates other than your selected / fight target are slightly see-through.
    if (p.role == Role::Normal)
        alpha *= std::clamp(cfg.otherOpacity, 0.1f, 1.0f);

    // Hidden behind walls and terrain like the game's tags, but not behind characters: the line from
    // the camera to the plate is tested against the playfield's static world (a few times a
    // second, not every frame), and the plate fades out while it is blocked. Your selected and
    // fight targets show through everything.
    {
        const DWORD now = GetTickCount();
        if (target && !classic) {
            p.inSight = true;
        } else if (!p.sightTick || now - p.sightTick >= kSightEveryMs) {
            bool known = false;
            const bool clear = game::LineOfSight(game::CameraWorldPos(), pos, &known);
            p.inSight = !known || clear;
            p.sightTick = now;
        }
        const float dt = p.sightFadeTick ? std::min(0.1f, (now - p.sightFadeTick) / 1000.0f) : 1.0f;
        p.sightFadeTick = now;
        const float step = dt / kSightFadeSec;
        p.sightAlpha = p.inSight ? std::min(1.0f, p.sightAlpha + step) : std::max(0.0f, p.sightAlpha - step);
        if (p.sightAlpha <= 0.0f) {
            game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
            p.screenTick = 0; // no footprint for stacking or clicking while hidden
            return keep;
        }
        alpha *= p.sightAlpha;
    }
    const uint32_t diffuse = (static_cast<uint32_t>(alpha * 255.0f + 0.5f) << 24) | 0xFFFFFF;

    // Screen rectangle of the plate where the game puts it (bottom edge at the anchor), and
    // stacking: a plate that overlaps a nearer one (or the selected/fight target's) moves up above
    // it. Placement uses the other plates' rectangles from their latest update.
    float sx = 0.0f, sy = 0.0f;
    const bool onScreen = game::ViewToScreen(view, &sx, &sy);
    const float pxPerTexel = capPx / p.nameCap;
    const float w2 = p.hitHalfW * pxPerTexel;
    // Visible part relative to the anchor (texture row anchorRow is at the anchor).
    const float visBelow = (anchorRow - p.visBottom) * pxPerTexel; // anchor -> visible bottom (< 0: below it)
    const float h = (p.visBottom - p.visTop) * pxPerTexel;
    const float priority = view.z; // nearer keeps its spot (selecting doesn't reorder plates)
    // Your selected and fight targets in front of the camera stay on screen: off the side, top or
    // bottom, the plate stops at the edge (its brackets included), shifted in the camera's plane.
    float edgeRightPx = 0.0f, edgeDownPx = 0.0f;
    if (target && onScreen && !classic) {
        int screenW = 0, screenH = 0;
        game::ViewportSize(&screenW, &screenH);
        const float room = (p.visTop - static_cast<float>(p.atkY + g_outline)) * pxPerTexel; // brackets around the name
        const float left = sx - w2 - room, right = sx + w2 + room;
        const float bottom = sy - visBelow + room, top = sy - visBelow - h - room;
        if (left < kEdgeMarginPx)
            edgeRightPx = kEdgeMarginPx - left;
        else if (right > screenW - kEdgeMarginPx)
            edgeRightPx = screenW - kEdgeMarginPx - right;
        if (top < kEdgeMarginPx)
            edgeDownPx = kEdgeMarginPx - top;
        else if (bottom > screenH - kEdgeMarginPx)
            edgeDownPx = screenH - kEdgeMarginPx - bottom;
        sx += edgeRightPx;
        sy += edgeDownPx;
    }
    float targetUp = 0.0f;
    if (cfg.stack && onScreen && !classic) {
        const DWORD now = GetTickCount();
        const float gap = 1.0f;
        for (int pass = 0; pass < 8; ++pass) {
            bool moved = false;
            // ghost: the footprint of a plate just removed (target change). It only keeps a plate
            // that was already stacked on it from dropping into the gap; it never starts a new push,
            // or a new target plate appearing where the old one was would be pushed away and then
            // drop back.
            auto collide = [&](const RECT& r, float qPriority, const game::Identity& qid, bool ghost) {
                if (ghost && !RecentlyStacked(qid, p.id, now))
                    return;
                if (qid.type == p.id.type && qid.instance == p.id.instance)
                    return; // never against another plate of the same character
                // Plates that are stacked stay stacked while they're within a margin of each other, so
                // characters moving about in a group don't make their plates jump at every touch.
                // Once clearly apart they're free (no timed hold: moving the camera changes the
                // overlaps all the time).
                const float margin = RecentlyStacked(qid, p.id, now) ? kStackKeepPx : 0.0f;
                const float bottom = sy - visBelow - targetUp, top = bottom - h;
                if (sx + w2 + margin <= r.left || sx - w2 - margin >= r.right ||
                    top - margin >= r.bottom || bottom + margin <= r.top)
                    return;
                // Farther plates make room for nearer ones, and an order once set is kept while the
                // two overlap (StackFirst), so it doesn't flip with small camera or NPC moves.
                if (!StackFirst(qid, p.id, qPriority - priority, now, true))
                    return;
                if (bottom <= r.top - gap)
                    return; // within the margin, but already above it
                targetUp = sy - visBelow - (static_cast<float>(r.top) - gap);
                moved = true;
            };
            for (auto& [qkey, ghost] : g_lastPlate) {
                if (ghost.hadScreen && now - ghost.tick < kGhostMs)
                    collide(ghost.screen, ghost.priority, ghost.id, true);
            }
            for (auto& [other, q] : g_plates) {
                if (other == ind || !q.screenTick || now - q.screenTick > 250)
                    continue;
                collide(q.screen, q.priority, q.id, false);
            }
            if (!moved)
                break;
        }
        // No towers: in a crowd seen from above, plates may overlap a little rather than climb far
        // away from their characters.
        targetUp = std::min(targetUp, kStackMaxPlates * h);
    }
    // Glide instead of jumping: up fairly quickly (names mustn't overlap for long), back down a
    // little slower.
    const DWORD tick = GetTickCount();
    const float dt = p.stackTick ? std::min(0.1f, (tick - p.stackTick) / 1000.0f) : 1.0f;
    p.stackTick = tick;
    const float rate = targetUp > p.stackUp ? kStackRiseRate : kStackFallRate;
    p.stackUp += (targetUp - p.stackUp) * std::min(1.0f, dt * rate);
    const float stackWorld = p.stackUp / ppu;

    auto* base = static_cast<uint8_t*>(sprite);
    Vertex* v = reinterpret_cast<Vertex*>(base + kSpriteVerts);
    for (int i = 0; i < 4; ++i) {
        v[i].x = p.signX[i] * halfW + p.rightSign * edgeRightPx / ppu;
        v[i].y = p.signY[i] * halfH + p.upSign * (lift + stackWorld + steady - edgeDownPx / ppu);
        v[i].diffuse = diffuse;
    }
    *reinterpret_cast<float*>(base + kSpriteHalfW) = halfW;
    *reinterpret_cast<float*>(base + kSpriteHalfH) = halfH;

    // Where the plate is now, for click-to-target and for the other plates' stacking.
    if (alpha > 0.3f && onScreen) {
        p.screen.left = static_cast<LONG>(sx - w2);
        p.screen.right = static_cast<LONG>(sx + w2);
        p.screen.bottom = static_cast<LONG>(sy - visBelow - p.stackUp);
        p.screen.top = static_cast<LONG>(sy - visBelow - p.stackUp - h);
        // ... and the top including the room its (aggro) brackets take above the name.
        p.screenTopFull = p.screen.top - (p.visTop - static_cast<float>(p.atkY + g_outline)) * pxPerTexel;
        p.depth = view.z;
        p.priority = priority;
        p.screenTick = GetTickCount();
    } else {
        p.screenTick = 0;
    }
    return keep;
}

// --- click-to-target ----------------------------------------------------------------------

// The plate under a client-area point, nearest first; null if none.
uint8_t* PlateAt(int x, int y)
{
    const DWORD now = GetTickCount();
    uint8_t* best = nullptr;
    float bestDepth = 1e9f;
    for (auto& [ind, p] : g_plates) {
        if (!p.screenTick || now - p.screenTick > 250)
            continue;
        if (x < p.screen.left || x > p.screen.right || y < p.screen.top || y > p.screen.bottom)
            continue;
        if (p.depth < bestDepth) {
            bestDepth = p.depth;
            best = ind;
        }
    }
    return best;
}

LRESULT CALLBACK ClickWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    hideui::HandleMessage(msg, wp, lp); // (the key still reaches the game)
    const NameplateConfig& cfg = Cfg().np;
    if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && cfg.enable && cfg.clickToTarget &&
        GetCurrentThreadId() == g_gameThread && g_setTarget && !(wp & (MK_RBUTTON | MK_SHIFT | MK_CONTROL))) {
        // Only clicks on the 3D world: none of the game's GUI windows is under the mouse.
        void* wc = g_windowController ? g_windowController() : nullptr;
        if (wc && !g_getMouseWindow(wc)) {
            if (uint8_t* ind = PlateAt(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)))) {
                const game::Identity id = Field<game::Identity>(ind, kIndIdentity);
                g_setTarget(id, false); // as a normal click (not forced)
                g_swallowUp = true;
                if (Cfg().fct.debug)
                    Log("[np] click selected %u:%u", id.type, id.instance);
                return 0;
            }
        }
    }
    if (msg == WM_LBUTTONUP && g_swallowUp) {
        g_swallowUp = false;
        return 0;
    }
    return g_unicodeWnd ? CallWindowProcW(g_prevWndProc, hwnd, msg, wp, lp)
                        : CallWindowProcA(g_prevWndProc, hwnd, msg, wp, lp);
}

} // namespace

void OnConfigChanged()
{
    g_version.fetch_add(1);
}

void Tick()
{
    HWND hwnd = game::GameWindow();
    if (!hwnd || hwnd == g_clickWnd || !g_setTarget)
        return;
    // Chain onto whatever window procedure is there now (the game's, or another mod's).
    g_unicodeWnd = IsWindowUnicode(hwnd) != FALSE;
    g_clickWnd = hwnd;
    const LONG_PTR proc = reinterpret_cast<LONG_PTR>(&ClickWndProc);
    g_prevWndProc = reinterpret_cast<WNDPROC>(g_unicodeWnd ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, proc)
                                                           : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, proc));
    Log("[np] click-to-target on window %p (previous wndproc %p)", hwnd, g_prevWndProc);
}

bool PlateTop(uint32_t type, uint32_t instance, float* top)
{
    const DWORD now = GetTickCount();
    bool found = false;
    for (auto& [ind, p] : g_plates) {
        if (p.id.type != type || p.id.instance != instance || !p.screenTick || now - p.screenTick > 250)
            continue;
        if (!found || p.screenTopFull < *top)
            *top = p.screenTopFull;
        found = true;
    }
    return found;
}

bool Install()
{
    if (!n3::Init())
        return false;
    if (HMODULE gui = GetModuleHandleA("GUI.dll"))
    {
        g_targetingInstance = reinterpret_cast<void**>(GetProcAddress(gui, "?m_pcInstance@TargetingModule_t@@0PAV1@A"));
        g_setTarget = reinterpret_cast<SetTargetFn>(GetProcAddress(gui, "?SetTarget@TargetingModule_t@@CAXABVIdentity_t@@_N@Z"));
        g_windowController = reinterpret_cast<InstanceFn>(GetProcAddress(gui, "?GetInstanceIfAny@WindowController_c@@SAPAV1@XZ"));
        g_getMouseWindow = reinterpret_cast<GetMouseWindowFn>(GetProcAddress(gui, "?GetMouseWindow@WindowController_c@@QAEPAVWindow@@XZ"));
        if (!g_setTarget || !g_windowController || !g_getMouseWindow)
            Log("[np] click-to-target exports missing - disabled");

        g_handleNametags = reinterpret_cast<uint8_t*>(GetProcAddress(gui, "?HandleNametags@TargetingModule_t@@AAEXXZ"));
        void** slot = FindImportSlot(gui, "?N3Msg_GetDynelsInVicinity@");
        if (g_handleNametags && slot && *slot) {
            g_nextVicinity = reinterpret_cast<VicinityFn>(*slot);
            if (!WritePointer(slot, reinterpret_cast<void*>(&Hook_Vicinity)))
                g_nextVicinity = nullptr;
        }
        Log("[np] own nameplate (ShowSelf) %s", g_nextVicinity ? "available" : "unavailable: GUI.dll import not found");
    }
    uint8_t* build = FindUnique(SIG_NAMETAG_BUILD, "nametag builder");
    uint8_t* update = FindUnique(SIG_NAMETAG_UPDATE, "nametag update");
    if (!build || !update)
        return false;
    const bool ok = InstallHook("nametag update", update, &Hook_Update, &g_origUpdate) &&
                    InstallHook("nametag builder", build, &Hook_Build, &g_origBuild);
    Log("[np] %s", ok ? "ready" : "disabled");
    return ok;
}

} // namespace prk::nameplates
