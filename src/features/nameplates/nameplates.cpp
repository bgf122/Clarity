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
#include "../../core/wndproc.h"

#include <windows.h>

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

constexpr int kTexW = 1024; // wide enough for long names at full size
constexpr int kTexH = 256;
constexpr int kPad = 8;          // texels around text pieces (outline + shadow room)
// Sizes are designed for a 40 px name font and scaled by g_scale so the texture's text is drawn
// at the size it is shown (MaxPx): no mipmaps, so heavy minification would alias.
constexpr float kShadowAlpha = 0.6f;
constexpr int kDesignNameEm = 40;
constexpr float kCapPerEm = 0.70f; // Bahnschrift cap height / em
float g_scale = 1.0f;
int S(int v)
{
    return std::max(1, static_cast<int>(std::lround(v * g_scale)));
}
int g_outline = 2, g_shadowOffset = 2, g_shadowBlur = 2;
int g_barW = 240, g_barH = 12; // the health bar: at least this wide (wider for a long name)
constexpr uint32_t kTypeCharacter = 50000;
constexpr int kStatPvpFlags = 224;   // 0xE0: bits 0x4000001 = flagged for PvP ("** name **")
constexpr int kStatNameMarks = 673;  // 0x2A1: bit 9 = "= name =" (as the game's tag builder)

enum class Kind { Player, Npc, Pet };
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
    std::vector<uint32_t> px;   // the texture as last sent: text + "attacking you" band + bar fill
    float nameCap = 30.0f;      // texels, what MinPx/MaxPx refer to
    int anchorRow = kTexH;      // texel row placed at the game's anchor (above the head)
    int barY = -1;              // -1 = no bar
    int barW = 0;               // texels: g_barW, or wider to span a long name
    int barH = 0;               // texels
    // "Attacking you" band (red, round the frame): fill and outline masks of its box, whose (0, 0) is
    // texel (atkX, atkY), and how strongly they're painted (0 = off, blinks).
    raster::Mask atkFill, atkOutline;
    int atkX = 0, atkY = 0;
    int atkLevel = 0; // 0..kBlinkSteps
    int hpPct = -1;
    DWORD barTick = 0;          // the bar last redrawn (kBarEveryMs)
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
constexpr DWORD kBarEveryMs = 100;     // a plate's health bar is redrawn at most this often
constexpr float kSightFadeSec = 0.15f; // fade out / in when a wall comes between
constexpr DWORD kBlinkMs = 1600;       // the "attacking you" brackets: one slow blink, full to faint and back
constexpr float kEdgeMarginPx = 8.0f;     // target plates kept this far inside the window edges
constexpr float kTargetRange = 80.0f; // your selected / fight target's plate is hidden beyond this (metres from you)
constexpr float kFirstPersonRadius = 1.5f; // your own plate's anchor this close to the camera = first person
constexpr int kBlinkSteps = 6;         // painted strengths (each change repaints and sends the brackets' area)
constexpr float kBlinkLow = 0.25f;     // faintest strength
raster::Font g_nameFont, g_smallFont;
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

    void Blend(int x, int y, uint32_t rgb, float alpha)
    {
        if (x < 0 || y < 0 || x >= kTexW || y >= kTexH || alpha <= 0.0f)
            return;
        alpha = std::min(alpha, 1.0f);
        const size_t i = static_cast<size_t>(y) * kTexW + x;
        r[i] = ((rgb >> 16) & 0xFF) / 255.0f * alpha + r[i] * (1.0f - alpha);
        g[i] = ((rgb >> 8) & 0xFF) / 255.0f * alpha + g[i] * (1.0f - alpha);
        b[i] = (rgb & 0xFF) / 255.0f * alpha + b[i] * (1.0f - alpha);
        a[i] = alpha + a[i] * (1.0f - alpha);
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
        // Bahnschrift (Windows' DIN), as Clarity's bars.
        const NameplateConfig& np = Cfg().np;
        g_scale = std::clamp(np.maxPx / (kDesignNameEm * kCapPerEm), 0.35f, 1.5f);
        g_outline = S(2);
        g_shadowOffset = S(2);
        g_shadowBlur = S(2);
        g_barW = S(240);
        g_barH = S(12);
        g_fontsOk = g_nameFont.Create(L"Bahnschrift", S(40), 600) && g_smallFont.Create(L"Bahnschrift", S(28), 400);
        Log("[np] text drawn at %.2fx (name %d px for MaxPx %.0f)", g_scale, S(40), np.maxPx);
        if (!g_fontsOk)
            Log("[np] font creation failed - nameplates unchanged");
    }
    return g_fontsOk;
}

void FillRect(std::vector<uint32_t>& px, int x0, int y0, int w, int h, uint32_t argb, const RECT* clip = nullptr);

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

// --- the frame (as Clarity's bars) ------------------------------------------------------------

constexpr uint32_t kTealRgb = 0x2FD3B9;

// Signed distance (texels, < 0 inside) from a point to the rectangle (x0, y0)-(x1, y1) with its
// corners cut off at 45 degrees, `cut` along each side.
float ChamferDist(float px, float py, float x0, float y0, float x1, float y1, float cut)
{
    const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    const float ax = std::fabs(px - (x0 + hx)), ay = std::fabs(py - (y0 + hy));
    return std::max(std::max(ax - hx, ay - hy), (ax + ay - (hx + hy - cut)) * 0.70710678f);
}

uint32_t Mix(uint32_t a, uint32_t b, float t)
{
    auto ch = [&](int shift) {
        const float va = static_cast<float>((a >> shift) & 0xFF), vb = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<uint32_t>(std::lround(va + (vb - va) * std::clamp(t, 0.0f, 1.0f))) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

// The part of the cut-corner rectangle between `from` and `to` texels in from its edge, antialiased,
// in rgbAt(t) (t from 0 at the top to 1 at the bottom), where keep(x, y) says.
template <typename F, typename K>
void Ring(Canvas& c, float x0, float y0, float x1, float y1, float cut, float from, float to, float alpha, F&& rgbAt, K&& keep)
{
    for (int y = std::max(0, static_cast<int>(std::floor(y0))); y < std::min(kTexH, static_cast<int>(std::ceil(y1))); ++y) {
        const uint32_t rgb = rgbAt(std::clamp((y + 0.5f - y0) / (y1 - y0), 0.0f, 1.0f));
        for (int x = std::max(0, static_cast<int>(std::floor(x0))); x < std::min(kTexW, static_cast<int>(std::ceil(x1))); ++x) {
            if (!keep(x + 0.5f, y + 0.5f))
                continue;
            const float d = ChamferDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, cut);
            const float cover = std::clamp(0.5f - (d + from), 0.0f, 1.0f) - std::clamp(0.5f - (d + to), 0.0f, 1.0f);
            c.Blend(x, y, rgb, alpha * cover);
        }
    }
}

// A line `width` texels thick from (x0, y0) to (x1, y1), antialiased.
void Line(Canvas& c, float x0, float y0, float x1, float y1, float width, uint32_t rgb, float alpha)
{
    const float r = width * 0.5f;
    const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - r)) - 1);
         y < std::min(kTexH, static_cast<int>(std::ceil(std::max(y0, y1) + r)) + 1); ++y) {
        for (int x = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - r)) - 1);
             x < std::min(kTexW, static_cast<int>(std::ceil(std::max(x0, x1) + r)) + 1); ++x) {
            const float qx = x + 0.5f - x0, qy = y + 0.5f - y0;
            const float t = len2 > 0.0f ? std::clamp((qx * dx + qy * dy) / len2, 0.0f, 1.0f) : 0.0f;
            c.Blend(x, y, rgb, alpha * std::clamp(r + 0.5f - std::hypot(qx - t * dx, qy - t * dy), 0.0f, 1.0f));
        }
    }
}

// A teal line with a soft glow along the points.
void TealPath(Canvas& c, std::initializer_list<std::pair<float, float>> points, float alpha)
{
    const std::vector<std::pair<float, float>> pts(points);
    const float core = static_cast<float>(S(2)), glow = static_cast<float>(S(5));
    for (size_t i = 1; i < pts.size(); ++i)
        Line(c, pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second, core + 2.0f * glow, kTealRgb, 0.10f * alpha);
    for (size_t i = 1; i < pts.size(); ++i)
        Line(c, pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second, core, Mix(kTealRgb, 0xFFFFFF, 0.25f), 0.8f * alpha);
}

// The bar's fill from top to bottom (t): a bright edge, a light band fading down, the colour, and
// deeper at the bottom with a bright edge again - as Clarity's bars.
uint32_t GlossRgb(uint32_t rgb, float t)
{
    if (t < 0.08f)
        return Mix(rgb, 0xFFFFFF, 0.55f);
    if (t < 0.45f)
        return Mix(rgb, 0xFFFFFF, 0.28f * (0.45f - t) / 0.37f);
    if (t < 0.85f)
        return Mix(0x000000, rgb, 1.0f - (t - 0.45f) * 0.5f);
    if (t < 0.94f)
        return Mix(0x000000, rgb, 0.78f);
    return Mix(rgb, 0xFFFFFF, 0.3f);
}

// The plate, in the style of Clarity's bars: the name centred over a thin health bar, both in a
// frame - a silver rim with cut corners, a dark gap, a thin inner ring and a translucent dark panel,
// teal at the lower corners. Your selected target's ring is white, your fight target's orange (gold
// when it's both); "attacking you" is a red band right on the rim, blinked by Repaint. The org line
// is under the frame. Every plate keeps room for the band, so a plate doesn't move when it starts
// being attacked.
void RenderText(Plate& p, const game::Identity& id, uint8_t* ind)
{
    const NameplateConfig& cfg = Cfg().np;
    Canvas c;
    // Only characters (Identity type 50000) have health; corpses ("Remains of ...") are Corpse_t
    // dynels of type 51050 and get just their name.
    const bool corpse = id.type != kTypeCharacter;
    const float cut = static_cast<float>(S(18)), rim = static_cast<float>(S(5)), gap = static_cast<float>(S(3));
    const float ring = static_cast<float>(S(4)), pad = static_cast<float>(S(18));
    const float inner = rim + gap + ring; // frame edge -> panel
    const int band = S(10);               // the "attacking you" band, outside the frame
    const float asc = g_nameFont.Ascent(), cap = asc * 0.72f;

    const float maxW = kTexW - 2.0f * (kPad + band + inner + pad);
    const std::vector<TextRun> name = FitRuns(g_nameFont, NameRuns(id, ind), maxW);
    const float nameW = RunsWidth(g_nameFont, name);

    // The health bar under the name: as wide as the name, at least g_barW.
    bool bar = false;
    if (cfg.showHealth && !corpse) {
        const int life = n3::GetSkill(id, n3::kStatLife);
        bar = life > 0 && life != n3::kNoValue;
    }
    p.barW = std::min(std::max(g_barW, static_cast<int>(std::ceil(nameW))), static_cast<int>(maxW));
    p.barH = g_barH;

    // The frame, symmetric about the texture's centre (where name and bar are centred).
    const float contentW = bar ? std::max(nameW, static_cast<float>(p.barW)) : nameW;
    const float x0 = std::floor((kTexW - contentW) * 0.5f - inner - pad), x1 = kTexW - x0;
    const float y0 = static_cast<float>(kPad + band);
    const float capTop = y0 + inner + pad;
    const float baseline = capTop + cap;
    p.barY = bar ? static_cast<int>(std::lround(baseline + S(12))) : -1;
    const float y1 = std::ceil((bar ? static_cast<float>(p.barY + p.barH) : baseline) + pad + inner);
    const float h = y1 - y0;

    const bool target = p.role != Role::Normal;
    uint32_t ringRgb = 0x3C4045;
    if (p.role == Role::Fighting)
        ringRgb = 0xFF8A1A;
    else if (p.role == Role::Selected)
        ringRgb = p.alsoFight ? 0xFFD21F : 0xF2F4F6;
    const float ringIn = inner + (target ? static_cast<float>(S(2)) : 0.0f); // a target's ring a little wider
    auto all = [](float, float) { return true; };
    auto flat = [](uint32_t rgb) { return [rgb](float) { return rgb; }; };
    // A dark line round the outside; the silver rim, light at the top, with a bright line along it;
    // the dark gap; the ring; the panel.
    Ring(c, x0, y0, x1, y1, cut, 0.0f, 1.0f, 1.0f, flat(0x0A0B0D), all);
    Ring(c, x0, y0, x1, y1, cut, 1.0f, rim, 1.0f, [](float t) { return Mix(0xD3D7DB, 0x8C9196, t); }, all);
    Ring(c, x0, y0, x1, y1, cut, 1.0f, 1.0f + static_cast<float>(S(2)), 0.45f, flat(0xFFFFFF),
         [&](float, float y) { return y < y0 + h * 0.5f; });
    Ring(c, x0, y0, x1, y1, cut, rim, rim + gap, 0.9f, flat(0x0B0C0E), all);
    Ring(c, x0, y0, x1, y1, cut, rim + gap, ringIn, 1.0f, flat(ringRgb), all);
    // The panel (Background), a little more solid on target plates.
    if (cfg.background)
        Ring(c, x0, y0, x1, y1, cut, ringIn, 1e6f, target ? 0.75f : 0.55f, [](float t) { return Mix(0x1A1D20, 0x0E1012, t); }, all);
    // Teal along the panel's lower corners, less of it on the right.
    const float tl = inner + S(2), ic = std::max(0.0f, cut - tl * 0.5858f);
    const float w = x1 - x0;
    TealPath(c, {{x0 + tl, y0 + h * 0.55f}, {x0 + tl, y1 - tl - ic}, {x0 + tl + ic, y1 - tl}, {x0 + w * 0.22f, y1 - tl}}, 1.0f);
    TealPath(c, {{x1 - tl, y1 - tl - ic - S(8)}, {x1 - tl, y1 - tl - ic}, {x1 - tl - ic, y1 - tl}, {x1 - w * 0.12f, y1 - tl}}, 0.7f);

    // The bar's silver bezel and its track (Repaint draws the fill over the track).
    if (bar) {
        const float bx0 = static_cast<float>((kTexW - p.barW) / 2), by0 = static_cast<float>(p.barY);
        const float bx1 = bx0 + p.barW, by1 = by0 + p.barH;
        const float bezel = static_cast<float>(S(5)), dark = static_cast<float>(S(2));
        Ring(c, bx0 - bezel, by0 - bezel, bx1 + bezel, by1 + bezel, static_cast<float>(S(3)), 0.0f, 1e6f, 1.0f,
             [](float t) { return Mix(0x9AA0A5, 0x55595D, t); }, all);
        Ring(c, bx0 - dark, by0 - dark, bx1 + dark, by1 + dark, static_cast<float>(S(2)), 0.0f, 1e6f, 1.0f, flat(0x050607), all);
        Ring(c, bx0, by0, bx1, by1, 0.0f, 0.0f, 1e6f, 1.0f, [](float t) { return Mix(0x0B0D10, 0x17191D, t); }, all);
    }
    DrawRuns(c, g_nameFont, name, (kTexW - nameW) * 0.5f, capTop - (asc - cap));

    // "Attacking you": a red band round the rim, its corners cut parallel to the frame's, with a
    // dark outer edge; Repaint paints it (blinking) from these masks.
    {
        const float bx0 = x0 - band, by0 = y0 - band, bx1 = x1 + band, by1 = y1 + band, bcut = cut + band * 0.5858f;
        p.atkX = static_cast<int>(std::floor(bx0)) - 1;
        p.atkY = static_cast<int>(std::floor(by0)) - 1;
        const int mw = static_cast<int>(std::ceil(bx1)) + 1 - p.atkX, mh = static_cast<int>(std::ceil(by1)) + 1 - p.atkY;
        p.atkFill = raster::Mask(mw, mh);
        p.atkOutline = raster::Mask(mw, mh);
        for (int my = 0; my < mh; ++my) {
            for (int mx = 0; mx < mw; ++mx) {
                const float d = ChamferDist(p.atkX + mx + 0.5f, p.atkY + my + 0.5f, bx0, by0, bx1, by1, bcut);
                const float outer = std::clamp(0.5f - d, 0.0f, 1.0f);
                const float in1 = std::clamp(0.5f - (d + 1.0f), 0.0f, 1.0f);
                const float frame = std::clamp(0.5f - (d + band), 0.0f, 1.0f);
                p.atkOutline.a[static_cast<size_t>(my) * mw + mx] = outer - in1;
                p.atkFill.a[static_cast<size_t>(my) * mw + mx] = in1 - frame;
            }
        }
    }

    // Below the band room, the org line. The row under it sits at the game's anchor: the plate's
    // bottom edge, the same for a character's normal, selected and attacking plate.
    float visHalfW = (x1 - x0) * 0.5f;
    float y = y1 + band + S(4);
    y = OrgLine(c, id, y, &visHalfW);
    p.anchorRow = static_cast<int>(std::ceil(y)) + 2;
    // Footprint for stacking and clicking: the frame's top down to the plate's bottom edge.
    p.hitHalfW = visHalfW;
    p.visTop = y0;
    p.visBottom = static_cast<float>(p.anchorRow - 2);
    p.text = c.ToArgb();
    p.nameCap = std::max(1.0f, cap);
    p.hpPct = -1;
    p.barRgb = 0;
}

// Within the texture and, if given, within clip.
RECT ClipRect(const RECT* clip)
{
    RECT r{0, 0, kTexW, kTexH};
    if (clip) {
        r.left = std::max(r.left, clip->left);
        r.top = std::max(r.top, clip->top);
        r.right = std::min(r.right, clip->right);
        r.bottom = std::min(r.bottom, clip->bottom);
    }
    return r;
}

void FillRect(std::vector<uint32_t>& px, int x0, int y0, int w, int h, uint32_t argb, const RECT* clip)
{
    const RECT c = ClipRect(clip);
    for (int y = std::max<int>(c.top, y0); y < std::min<int>(c.bottom, y0 + h); ++y)
        for (int x = std::max<int>(c.left, x0); x < std::min<int>(c.right, x0 + w); ++x)
            px[static_cast<size_t>(y) * kTexW + x] = argb;
}

// Straight-alpha "over" of one colour through a mask onto ARGB pixels, touching only the mask's
// box (and clip).
void OverMask(std::vector<uint32_t>& px, int x0, int y0, const raster::Mask& m, uint32_t rgb, float alpha,
              const RECT* clip = nullptr)
{
    const RECT c = ClipRect(clip);
    for (int y = 0; y < m.h; ++y) {
        const int ty = y0 + y;
        if (ty < c.top || ty >= c.bottom)
            continue;
        for (int x = 0; x < m.w; ++x) {
            const int tx = x0 + x;
            const float sa = m.a[static_cast<size_t>(y) * m.w + x] * alpha;
            if (tx < c.left || tx >= c.right || sa <= 0.0f)
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

// The texels each changing part covers.
RECT BarArea(const Plate& p)
{
    if (p.barY < 0)
        return {};
    const int x0 = (kTexW - p.barW) / 2;
    return {x0, p.barY, x0 + p.barW, p.barY + p.barH};
}

RECT BracketArea(const Plate& p)
{
    if (p.atkOutline.a.empty())
        return {};
    return {p.atkX, p.atkY, p.atkX + p.atkOutline.w, p.atkY + p.atkOutline.h};
}

// Repaints `area` of the plate (from the text, then the "attacking you" band and the bar's fill over
// it) and sends just that part of the texture. The fill is glossy, in the bar colour, over the track.
void Repaint(Plate& p, RECT area)
{
    area = ClipRect(&area);
    if (area.right <= area.left || area.bottom <= area.top)
        return;
    if (p.px.size() != p.text.size())
        p.px = p.text;
    for (int y = area.top; y < area.bottom; ++y) {
        const size_t row = static_cast<size_t>(y) * kTexW;
        std::copy(p.text.begin() + row + area.left, p.text.begin() + row + area.right, p.px.begin() + row + area.left);
    }
    std::vector<uint32_t>& px = p.px;
    if (p.atkLevel > 0 && !p.atkFill.a.empty()) {
        const float strength = static_cast<float>(p.atkLevel) / kBlinkSteps;
        OverMask(px, p.atkX, p.atkY, p.atkOutline, 0x000000, 0.9f * strength, &area);
        OverMask(px, p.atkX, p.atkY, p.atkFill, 0xFF2A2A, strength, &area);
    }
    const int tenths = p.hpPct;
    if (p.barY >= 0 && tenths >= 0) {
        const int x0 = (kTexW - p.barW) / 2, fill = p.barW * tenths / 1000;
        for (int row = 0; row < p.barH; ++row) {
            const float t = (row + 0.5f) / p.barH;
            FillRect(px, x0, p.barY + row, p.barW, 1, 0xFF000000 | Mix(0x0B0D10, 0x17191D, t), &area);
            FillRect(px, x0, p.barY + row, fill, 1, 0xFF000000 | GlossRgb(p.barRgb, t), &area);
        }
    }
    if (!game::ModifyVisualMaterial(p.vm, px.data(), kTexW, area))
        game::WriteTexture(game::VisualMaterialTexture(p.vm), px.data(), kTexW, kTexH);
}

// New bar values: the whole plate after a new layout (`full`), else just the bar.
// `tenths` is 0..1000 (percent x 10), -1 = unknown.
void Upload(Plate& p, int tenths, uint32_t barRgb, bool full)
{
    p.hpPct = tenths;
    p.barRgb = barRgb;
    if (full || p.px.size() != p.text.size()) {
        p.px = p.text;
        Repaint(p, RECT{0, 0, kTexW, kTexH});
        return;
    }
    Repaint(p, BarArea(p));
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

// Current bar state for the plate; returns false if nothing needs redrawing.
bool BarChanged(const Plate& p, const game::Identity& id, int* tenths, uint32_t* rgb)
{
    if (p.barY < 0)
        return false;
    *tenths = HealthTenths(id);
    *rgb = HealthBarRgb(id, *tenths);
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

// You (when you target yourself) count as a player.
Kind Classify(const game::Identity& id)
{
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
        Upload(p, pct, rgb, true);
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
        if (Other(ind, tagId, Role::Selected) || Other(ind, tagId, Role::Fighting))
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
        Upload(p, pct, rgb, true);
    } else if (GetTickCount() - p.barTick >= kBarEveryMs && BarChanged(p, id, &pct, &rgb)) {
        Upload(p, pct, rgb, false); // (at most every kBarEveryMs: in raids health changes all the time)
        p.barTick = GetTickCount();
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
            Repaint(p, BracketArea(p));
        }
    }

    // Size: the name's capitals are NaturalSize world units, clamped to MinPx..MaxPx on screen.
    game::Vec3 pos;
    if (!game::IndicatorPosition(id, &pos))
        return keep;
    const bool target = p.role != Role::Normal;
    // The game's anchor follows the head, so it bobs with walking, running and fighting. Keep the
    // plate's height above the character's own position (its feet, which follow the ground without
    // the bob) and smooth only that; big changes (mounting, zoning, a new model) snap.
    float steady = 0.0f; // world units added to the anchor's height
    game::Vec3 feet;
    if (n3::Position(id, &feet)) {
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
    // Your own plate (you targeted yourself) is hidden in first person (the camera is inside your head).
    if (IsSelf(id) && std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z) < kFirstPersonRadius) {
        game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
        p.screenTick = 0;
        return keep;
    }
    // Your selected and fight targets: nothing beyond kTargetRange of you.
    game::Vec3 me;
    const bool haveMe = n3::Position({kTypeCharacter, game::ClientInstance()}, &me);
    if (target && haveMe) {
        const float dx = pos.x - me.x, dy = pos.y - me.y, dz = pos.z - me.z;
        if (dx * dx + dy * dy + dz * dz > kTargetRange * kTargetRange) {
            game::VisualSpriteSetInvisible(Field<void*>(ind, kIndSprite));
            p.screenTick = 0;
            return keep;
        }
    }
    const float ppu = game::PxPerUnit(view.z);
    // Your selected and fight targets are a little bigger than the plates around them.
    const float capPx = std::clamp(cfg.naturalSize * ppu, cfg.minPx, cfg.maxPx) * (target ? cfg.targetScale : 1.0f);
    const float worldPerTexel = capPx / p.nameCap / ppu;
    const float halfW = kTexW * 0.5f * worldPerTexel;
    const float halfH = kTexH * 0.5f * worldPerTexel;
    // Lift the quad so anchorRow sits at the game's anchor (above the head).
    const float anchorRow = static_cast<float>(p.anchorRow);
    const float lift = (anchorRow - kTexH * 0.5f) * worldPerTexel;

    // Fade: in over FadeInSec after the tag appears, and out between FadeStart and the game's
    // 30 m tag range (distance from the player), so tags don't pop.
    float alpha = 1.0f;
    if (cfg.fadeInSec > 0.0f)
        alpha = std::min(1.0f, (GetTickCount() - p.born) / (cfg.fadeInSec * 1000.0f));
    // Only ordinary tags fade with distance: your selected and fight targets stay readable as long
    // as the game keeps their indicator (up to kTargetRange), e.g. a nano target beyond the 30 m tag range.
    if (p.role == Role::Normal && cfg.fadeStart < kTagRange && haveMe) {
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
        if (target) {
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
    // bottom, the plate stops at the edge (its "attacking you" band included), shifted in the camera's plane.
    float edgeRightPx = 0.0f, edgeDownPx = 0.0f;
    if (target && onScreen) {
        int screenW = 0, screenH = 0;
        game::ViewportSize(&screenW, &screenH);
        const float room = (p.visTop - static_cast<float>(p.atkY + g_outline)) * pxPerTexel; // the band round the frame
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
    if (cfg.stack && onScreen) {
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

bool HandleClick(HWND /*hwnd*/, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result)
{
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
                *result = 0;
                return true;
            }
        }
    }
    if (msg == WM_LBUTTONUP && g_swallowUp) {
        g_swallowUp = false;
        *result = 0;
        return true;
    }
    return false;
}

} // namespace

void OnConfigChanged()
{
    g_version.fetch_add(1);
}

uint32_t HealthBarRgb(const game::Identity& id, int tenths)
{
    // Classic: the game's consider colours (by level difference, grey for those it can't rate). Or one
    // colour for everyone, or by health: green at full health, yellow at half, red at none.
    const NameplateConfig& cfg = Cfg().np;
    uint32_t rgb;
    if (cfg.barColor == 1) {
        rgb = HueRgb(cfg.barHue);
    } else if (cfg.barColor == 2) {
        const int health = tenths >= 0 ? tenths : HealthTenths(id);
        rgb = HueRgb(120.0f * std::clamp(health, 0, 1000) / 1000.0f);
    } else {
        bool considerable = false;
        rgb = ConsiderRgb(id, &considerable);
        if (!considerable)
            return 0xAAAAAA; // the game's grey for those it can't rate (e.g. other players), as it is
    }
    return Soften(rgb);
}

uint32_t ConsiderTextRgb(const game::Identity& id)
{
    bool considerable = false;
    const uint32_t rgb = ConsiderRgb(id, &considerable);
    return considerable ? rgb : 0xFFFFFF;
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
        else
            wndproc::Add(&HandleClick);

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
