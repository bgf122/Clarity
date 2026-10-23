// How the original works (offsets checked on the client build):
//  - Gamecode CreateFloatingText(dynel, text, colorIdx) creates effect 0x2F5A, a
//    _GfxControlFont_t, then SetColor(colorTable[idx]) and SetText(text).
//  - SetText builds one DiaBill_n::Sprite_t per character (offsets in camera-facing space,
//    sized 12 px * scale(0.01) world units) and hands the array to a GfxVisualDiaBill.
//  - Process moves the visual to locator + (0, speed * age, 0) each frame.
// Because sizes are in world units, the text shrinks with camera distance.
//
// What we do: after SetText we give the visual our own sprite array (shadow + outline
// layers + the glyphs) and rewrite it every Process so the glyphs are SizePx tall on
// screen, with the rise/fade/pop done in pixels instead of world units.
#include "fct.h"

#include "atlas.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/n3.h"
#include "../../core/signatures.h"
#include "../../core/threat.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace prk::fct {

namespace {

// DiaBill_n::Sprite_t (0x20 bytes), layout from GfxVisualDiaBill render
// and _GfxControlFont_t::SetText.
struct Sprite {
    float x, y, z; // offset from the visual, in camera right/up/forward axes
    float w, h;    // world units
    uint32_t argb;
    uint32_t frame; // (atlasX << 24) | (atlasY << 16) | (glyphW << 8) | 12
    uint8_t visible;
    uint8_t pad[3];
};
static_assert(sizeof(Sprite) == 0x20, "Sprite_t layout");

// _GfxControlFont_t fields
constexpr int kAge = 0x0C;        // float, seconds since creation
constexpr int kDuration = 0x10;   // float, lifetime
constexpr int kTerminated = 0x14; // byte
constexpr int kCount = 0x30;      // int, sprite count (= strlen)
constexpr int kVisual = 0x44;     // GfxVisualDiaBill*
constexpr int kSprites = 0x48;    // Sprite*
constexpr int kSpeed = 0xB0;      // float, rise speed (world units / s)
// GfxVisualDiaBill fields (set by GfxVisualDiaBill::Init)
constexpr int kVisCount = 0x1A8;
constexpr int kVisSprites = 0x1AC;

template <typename T>
T& Field(uint8_t* obj, int off)
{
    return *reinterpret_cast<T*>(obj + off);
}

struct Layer {
    float dx, dy; // pixel offset (y up)
    bool tinted;  // true = outline/shadow colour, false = glyph colour
    float alpha;
};

struct Text {
    void* visual = nullptr;
    Sprite* gameSprites = nullptr;
    int count = 0;
    std::vector<Sprite> base; // glyphs as the game built them
    std::vector<Sprite> draw; // what the visual renders: layers * count
    std::vector<Layer> layers;
    void* dynel = nullptr;
    bool hasId = false;
    game::Identity id{};
    float headUp = 1.5f;     // world units from the visual (locator) up to the anchor; game default
    bool anchored = false;   // anchorWorld is set: the text stays where it appeared (FollowTarget off)
    game::Vec3 anchorWorld{};
    char label[16] = {};     // start of the text, for debug logging
    bool placeLogged = false;
    float widthRatio = 0.0f; // text width / glyph height
    const atlas::Atlas* atlas = nullptr; // high-res glyphs in use (else the game font + layers)
    std::vector<uint8_t> chars;          // the text, for atlas layout
    uint32_t fillArgb = 0xFFFFFFFF;      // the game's colour for this text
    bool crit = false;
    bool glancing = false;
    int stackSlot = 0;
    float jitterPx = 0.0f;
    int spreadSlot = 0;    // horizontal start slot: 0, +1, -1, +2, -2 (alternating sides per target)
    float driftDir = 0.0f; // -1 / +1: which way the text drifts while rising
    uint32_t outlineRgb = 0;
    DWORD lastSeen = 0;
};

struct Pending {
    bool active = false;
    void* dynel = nullptr;
    int colorIdx = -1;
    int hitType = 0;
    int32_t rgb = -1; // our own colour for the text (misses), -1 = the game's / [Colors]
};

// Hit types from AttackInfoIIR_t (+0x30), passed by the game as the combat function's 7th
// argument ([ebp+0x20]); it appends "Critical hit!" / "Glancing hit." to the chat line on these.
constexpr int kHitGlancing = 2;
constexpr int kHitCritical = 4;
constexpr int kCombatHitTypeSlot = 0x20;

struct Retired {
    DWORD at;
    std::vector<Sprite> sprites;
};

using CreateFn = void(__stdcall*)(void* dynel, const char* text, int colorIdx);
using ProcessFn = void(__thiscall*)(uint8_t* self);
using SetTextFn = void(__thiscall*)(uint8_t* self, const char* text);

using CharActionFn = void(__thiscall*)(void* self, int action, int* p20, int chatGroup, int* p28, void* str);

CreateFn g_origCreate = nullptr;
CreateFn g_createGame = nullptr; // CreateFloatingText's own address (enters our hook)
CharActionFn g_origCharAction = nullptr;

constexpr int kActionDrain = 0xD0;
constexpr int kActionNanoPool = 0x8C; // "Feedback_IncreasedNanoPool": p28[1] = nano points (client 0x1005DC96)
constexpr int kStatHealth = 0x1B;
constexpr uint32_t kTypeCharacter = 50000; // Identity type of characters (n3Dynel_t +0x14)
constexpr int kDynelIsClientChar = 0x140;  // n3Dynel_t: set for the client's own character
constexpr int kColorHeal = kColorIndexHeal; // the game's colour index for heals (event 0x22/0x23)
ProcessFn g_origProcess = nullptr;
SetTextFn g_origSetText = nullptr;

// All of the below is only touched on the game thread (inside the hooks).
uint8_t* g_selfRouteJump = nullptr; // the jne in the self-damage routing, see SIG_SELF_DAMAGE_ROUTE

// Combat-message function (client 0x10012BD5): it keeps the chat group of the
// event in [ebp+0x10] (written once per case, read when the chat line is sent), so when it calls
// CreateFloatingText we can tell whose damage it is. Captured by Hook_CreateEntry.
uint32_t g_callerEbp = 0;
uint32_t g_callerRet = 0;
uint32_t g_combatCallRet = 0; // return address of its CreateFloatingText call
constexpr int kCombatGroupSlot = 0x10;
constexpr uint32_t kGroupYourMisses = 0x42000012; // chat groups (combat-log categories)
constexpr uint32_t kGroupOtherMisses = 0x42000013;
constexpr uint32_t kGroupMeGotNano = 0x42000016;

// The combat-message function itself (same client 0x10012BD5), __thiscall with 8 stack arguments
// (ret 0x20). Several of its cases send only the chat line and leave through the exit that skips
// the floating text; hooked to add text for those.
using CombatFn = void(__thiscall*)(void* self, int event, uint8_t* target, int amount, uint8_t* attacker,
                                   int special, int damageType, int hitType, int extra);
CombatFn g_origCombat = nullptr;
constexpr int kEventMissed = 0x3B;  // MissedAttackInfoIIR_t: "You tried to hit X, but missed!" and the reverse
constexpr int kEventGotNano = 0x44; // "Feedback_GotNanoIncreaseFrom": target = the giver, amount = nano points
constexpr int kEventYouHealed = 0x41;   // "Feedback_YouHealed": target = who you healed, amount = health
constexpr int kEventYouGaveNano = 0x43; // "Feedback_PlayerIncreasedNano": target = who got it, amount = nano
constexpr uint32_t kGroupYouGaveHealth = 0x42000014;
constexpr uint32_t kGroupYouGaveNano = 0x42000017;
constexpr int kColorMiss = 0;       // the game colour index passed along; our own colour replaces it
constexpr int kStatCurrentNano = 214;
int32_t g_nextRgb = -1;            // colour for the next CreateFloatingText we make, see Pending::rgb
uint8_t* g_combatAttacker = nullptr; // while the combat function runs: its attacker argument
                                     // ([ebp+0x14] is overwritten before its floating-text call)
char g_nextSign = 0;               // sign for the next one we make ('+' for our gains), see SelfSigns

struct Spread {
    DWORD last = 0;
    unsigned next = 0;
};
std::unordered_map<void*, Spread> g_spread; // per target dynel

Pending g_pending;
std::unordered_map<uint8_t*, Text> g_texts;
std::vector<Retired> g_retired;
unsigned g_leaked = 0;

float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float EaseOutCubic(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// Overshoots past 1 and settles back (the "spring" of a crit pop).
float EaseOutBack(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

uint32_t LerpRgb(uint32_t from, uint32_t to, float t)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float a = static_cast<float>((from >> shift) & 0xFF);
        const float b = static_cast<float>((to >> shift) & 0xFF);
        out |= static_cast<uint32_t>(a + (b - a) * Clamp01(t) + 0.5f) << shift;
    }
    return out;
}

uint32_t WithAlpha(uint32_t rgb, float a)
{
    return (static_cast<uint32_t>(Clamp01(a) * 255.0f + 0.5f) << 24) | (rgb & 0xFFFFFF);
}


// The visual may still render our array for a frame after we stop tracking it, so
// arrays are hidden and kept alive for a while before being freed.
void Retire(std::unordered_map<uint8_t*, Text>::iterator it)
{
    Text& t = it->second;
    for (Sprite& s : t.draw)
        s.visible = 0;
    if (!t.draw.empty())
        g_retired.push_back({GetTickCount(), std::move(t.draw)});
    g_texts.erase(it);
}

void CollectGarbage()
{
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < g_retired.size();) {
        if (now - g_retired[i].at > 3000) {
            g_retired[i] = std::move(g_retired.back());
            g_retired.pop_back();
        } else {
            ++i;
        }
    }
    for (auto it = g_spread.begin(); it != g_spread.end();) {
        if (now - it->second.last > 10000)
            it = g_spread.erase(it);
        else
            ++it;
    }
    // Effects deleted without a final Process (zone change etc.). Their visual is gone too,
    // but we can't prove it, so the array is leaked rather than risk a use-after-free.
    for (auto it = g_texts.begin(); it != g_texts.end();) {
        if (now - it->second.lastSeen > 10000) {
            std::vector<Sprite> leak = std::move(it->second.draw);
            new std::vector<Sprite>(std::move(leak));
            ++g_leaked;
            it = g_texts.erase(it);
        } else {
            ++it;
        }
    }
}

void Register(uint8_t* self, const char* text)
{
    const FctConfig& cfg = Cfg().fct;
    auto* visual = Field<uint8_t*>(self, kVisual);
    auto* sprites = Field<Sprite*>(self, kSprites);
    const int count = Field<int>(self, kCount);
    if (!visual || !sprites || count <= 0 || count > 256)
        return;

    if (Field<Sprite*>(visual, kVisSprites) != sprites || Field<int>(visual, kVisCount) != count) {
        static bool warned = false;
        if (!warned) {
            Log("[fct] unexpected GfxVisualDiaBill layout (sprites %p/%p count %d/%d) - leaving text unchanged",
                Field<Sprite*>(visual, kVisSprites), sprites, Field<int>(visual, kVisCount), count);
            warned = true;
        }
        return;
    }

    Text t;
    t.visual = visual;
    t.gameSprites = sprites;
    t.count = count;
    t.base.assign(sprites, sprites + count);
    t.dynel = g_pending.active ? g_pending.dynel : nullptr;
    t.hasId = game::IdentityOf(t.dynel, &t.id);
    strncpy_s(t.label, text, _TRUNCATE);
    for (char& c : t.label)
        if (c == '\n')
            c = ' ';
    t.outlineRgb = cfg.outlineRgb;
    t.lastSeen = GetTickCount();

    float minX = 0.0f, maxX = 0.0f, glyphH = 0.0f;
    bool any = false;
    for (const Sprite& s : t.base) {
        if (!s.visible)
            continue;
        minX = any ? (s.x - s.w * 0.5f < minX ? s.x - s.w * 0.5f : minX) : s.x - s.w * 0.5f;
        maxX = any ? (s.x + s.w * 0.5f > maxX ? s.x + s.w * 0.5f : maxX) : s.x + s.w * 0.5f;
        glyphH = s.h;
        any = true;
    }
    if (!any || glyphH <= 0.0f)
        return;
    t.widthRatio = (maxX - minX) / glyphH;

    if (cfg.onTop && !game::DiaBillDisableDepthTest(visual)) {
        static bool warned = false;
        if (!warned) {
            Log("[fct] couldn't disable depth test on the text visual - OnTop has no effect");
            warned = true;
        }
    }

    const int idx = g_pending.active ? g_pending.colorIdx : -1;
    if (idx >= 0 && idx < kGameColorCount && cfg.colors[idx] >= 0) {
        for (Sprite& s : t.base)
            s.argb = 0xFF000000u | static_cast<uint32_t>(cfg.colors[idx]);
    }
    if (g_pending.active && g_pending.rgb >= 0) {
        for (Sprite& s : t.base)
            s.argb = 0xFF000000u | static_cast<uint32_t>(g_pending.rgb);
    }

    // The attack's hit type as the server sent it (AttackInfo +0x30): only auto attacks carry it.
    t.crit = g_pending.active && g_pending.hitType == kHitCritical;
    t.glancing = g_pending.active && g_pending.hitType == kHitGlancing;

    // Stack above texts on the same target that are still young.
    if (t.dynel) {
        for (auto& [other, ot] : g_texts) {
            if (ot.dynel == t.dynel && Field<float>(other, kAge) < cfg.stackWindowSec)
                t.stackSlot = (ot.stackSlot + 1 > t.stackSlot) ? ot.stackSlot + 1 : t.stackSlot;
        }
        if (t.stackSlot > 4)
            t.stackSlot = 4;
    }
    if (cfg.jitterPx > 0.0f)
        t.jitterPx = (static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f) * cfg.jitterPx;

    // Horizontal separation: consecutive texts on the same target start in slots 0, +1, -1, +2,
    // -2, ... side by side (Update spaces them by their width), and drift outwards with DriftPx.
    {
        static const int kSlots[] = {0, 1, -1, 2, -2};
        auto& spread = g_spread[t.dynel];
        const DWORD now = GetTickCount();
        if (now - spread.last > 1500)
            spread.next = 0;
        spread.last = now;
        const int slot = kSlots[spread.next++ % 5];
        t.spreadSlot = slot;
        t.driftDir = slot > 0 ? 1.0f : (slot < 0 ? -1.0f : (rand() & 1 ? 1.0f : -1.0f));
    }

    // The game colours every glyph the same (SetColor before SetText).
    for (const Sprite& s : t.base) {
        if (s.visible) {
            t.fillArgb = s.argb;
            break;
        }
    }

    // High-res glyphs: all outline cells first, then all fill cells (so an outline never
    // covers the neighbouring glyph's fill).
    const atlas::Atlas* glyphAtlas = atlas::Get(game::DiaBillMaterial(visual));
    if (glyphAtlas) {
        for (const char* p = text; *p; ++p)
            t.chars.push_back(static_cast<uint8_t>(*p));
        int glyphs = 0;
        for (uint8_t c : t.chars)
            glyphs += (c < 128 && glyphAtlas->glyphs[c].present) ? 1 : 0;
        if (glyphs > 0 && game::SwapDiaBillMaterial(visual, glyphAtlas->material)) {
            t.atlas = glyphAtlas;
            t.draw.assign(static_cast<size_t>(glyphs) * 2, Sprite{});
            int i = 0;
            for (uint8_t c : t.chars) {
                if (c >= 128 || !glyphAtlas->glyphs[c].present)
                    continue;
                t.draw[i].frame = glyphAtlas->glyphs[c].outlineFrame;
                t.draw[glyphs + i].frame = glyphAtlas->glyphs[c].fillFrame;
                t.draw[i].visible = t.draw[glyphs + i].visible = 1;
                ++i;
            }
        }
    }

    if (!t.atlas) {
        // Fallback: the game's 12 px font, outline and shadow faked with offset copies.
        if (cfg.shadowPx > 0.0f)
            t.layers.push_back({cfg.shadowPx, -cfg.shadowPx, true, cfg.outlineAlpha * 0.8f});
        if (cfg.outline && cfg.outlinePx > 0.0f) {
            const float o = cfg.outlinePx;
            const float d = o * 0.7071f;
            const float offs[8][2] = {{o, 0}, {-o, 0}, {0, o}, {0, -o}, {d, d}, {-d, d}, {d, -d}, {-d, -d}};
            for (auto& off : offs)
                t.layers.push_back({off[0], off[1], true, cfg.outlineAlpha});
        }
        t.layers.push_back({0.0f, 0.0f, false, 1.0f});

        t.draw.resize(t.layers.size() * count);
        for (size_t i = 0; i < t.draw.size(); ++i)
            t.draw[i] = t.base[i % count];
    }

    // Our own rise in pixels replaces the world-space one; lifetime from the INI.
    Field<float>(self, kSpeed) = 0.0f;
    Field<float>(self, kDuration) = cfg.lifetimeSec * (t.crit ? 1.35f : 1.0f);

    if (cfg.debug)
        Log("[fct] '%s' colorIdx=%d dynel=%p id=%u:%u crit=%d slot=%d", text, idx, t.dynel,
            t.hasId ? t.id.type : 0, t.hasId ? t.id.instance : 0, t.crit, t.stackSlot);

    auto [it, inserted] = g_texts.insert_or_assign(self, std::move(t));
    game::DiaBillInit(visual, it->second.draw.data(), static_cast<int>(it->second.draw.size()));
}

constexpr float kSelfRadius = 1.5f;  // anchor this close to the camera = our own character in 1st person
constexpr float kPinnedDepth = 4.0f; // depth the pinned first-person texts are placed at

void Hide(Text& t)
{
    for (Sprite& s : t.draw)
        s.visible = 0;
}

void Update(uint8_t* self, Text& t)
{
    const FctConfig& cfg = Cfg().fct;

    game::Vec3 pos;
    if (!game::WorldPosition(t.visual, &pos))
        return;

    // Anchor above the target's head (its name-tag point), not the game's fixed 1.5 units,
    // so big targets don't swallow their own numbers. Keeps the last height if the target
    // is gone (the text finishes where it was).
    game::Vec3 head{};
    const bool headOk = t.hasId && game::IndicatorPosition(t.id, &head);
    if (headOk) {
        float up = head.y - pos.y + cfg.headOffset;
        t.headUp = up < 0.3f ? 0.3f : (up > 50.0f ? 50.0f : up);
    }
    // Everything below is in camera space (x right, y up, z = depth), which is also the space
    // DiaBill sprite offsets are in.
    const game::Vec3 posView = game::ToView(pos);
    int screenW, screenH;
    game::ViewportSize(&screenW, &screenH);

    // The start point above the target. Unless FollowTarget is on, it is fixed where the text
    // appeared, so the text rises from the spot of the hit while the target moves on.
    game::Vec3 anchorWorld{pos.x, pos.y + t.headUp, pos.z};
    if (!cfg.followTarget) {
        if (!t.anchored) {
            t.anchorWorld = anchorWorld;
            t.anchored = true;
        }
        anchorWorld = t.anchorWorld;
    }
    game::Vec3 anchorView = game::ToView(anchorWorld);
    float sx = 0.0f, sy = 0.0f;
    const char* mode = "head";
    const float dist = std::sqrt(anchorView.x * anchorView.x + anchorView.y * anchorView.y + anchorView.z * anchorView.z);
    if (dist < kSelfRadius) {
        // First person: the camera is inside this character. Pin to a fixed screen spot.
        sx = screenW * 0.5f;
        sy = screenH * cfg.selfScreenY;
        anchorView = game::ScreenToView(sx, sy, kPinnedDepth);
        mode = "pinned";
    } else if (!game::ViewToScreen(anchorView, &sx, &sy)) {
        Hide(t); // behind the camera
        return;
    } else if (sy < screenH * cfg.anchorTop) {
        // Never start above the AnchorTop line: move the start point straight down on screen
        // (same depth, so the size stays the same).
        sy = screenH * cfg.anchorTop;
        anchorView = game::ScreenToView(sx, sy, anchorView.z);
        mode = "clamped";
    }
    if (cfg.debug && !t.placeLogged) {
        t.placeLogged = true;
        Log("[fct] place '%s': %s headOk=%d head.y=%.2f pos=(%.1f %.1f %.1f) headUp=%.2f depth=%.1f screen=(%.0f %.0f) of %dx%d",
            t.label, mode, headOk, head.y, pos.x, pos.y, pos.z, t.headUp, anchorView.z, sx, sy, screenW, screenH);
    }
    const float wpp = 1.0f / game::PxPerUnit(anchorView.z);
    // Sprite offsets are relative to the visual.
    const game::Vec3 offset{anchorView.x - posView.x, anchorView.y - posView.y, anchorView.z - posView.z};

    const float age = Field<float>(self, kAge);
    const float dur = Field<float>(self, kDuration);
    const float life = dur > 0.0f ? Clamp01(age / dur) : 1.0f;

    // Pop-in; crits pop bigger and longer and spring back with an overshoot.
    float pop = 1.0f;
    const float popSec = t.crit ? cfg.popSec * 1.8f : cfg.popSec;
    if (popSec > 0.0f && age < popSec) {
        const float p = age / popSec;
        pop = t.crit ? cfg.critPop + (1.0f - cfg.critPop) * EaseOutBack(p)
                     : cfg.popScale + (1.0f - cfg.popScale) * EaseOutCubic(p);
    }

    const float sizePx = cfg.sizePx * (t.crit ? cfg.critScale : (t.glancing ? cfg.glanceScale : 1.0f));
    const float alpha = life < cfg.fadeStart ? 1.0f : 1.0f - (life - cfg.fadeStart) / (1.0f - cfg.fadeStart);
    // With horizontal spread doing most of the separation, stacking needs only half a line.
    const float risePx = cfg.risePx * EaseOutCubic(life) + t.stackSlot * sizePx * (cfg.spreadPx > 0.0f ? 0.5f : 1.15f);

    const float hPx = sizePx * pop; // cap height on screen

    // Text width for the screen clamp.
    float halfWPx = 0.0f;
    float pxPerTexel = 0.0f;
    float totalAdvance = 0.0f; // texels
    if (t.atlas) {
        pxPerTexel = hPx / t.atlas->capH;
        for (uint8_t c : t.chars)
            totalAdvance += (c < 128 && t.atlas->glyphs[c].present) ? t.atlas->glyphs[c].advance : t.atlas->spaceAdvance;
        halfWPx = totalAdvance * pxPerTexel * 0.5f;
    } else {
        halfWPx = t.widthRatio * hPx * 0.5f;
    }

    // Keep the whole text inside the window: e.g. a big target's head above the screen top.
    const float margin = cfg.screenMarginPx;
    float upPx = risePx;       // baseline / bottom edge, pixels above the anchor
    // Start slots are at least SpreadPx apart, and at least the text's own width (unscaled by the
    // pop-in) plus a gap, so long numbers and crits don't start on top of each other.
    constexpr float kSpreadGapPx = 6.0f;
    const float spreadStep = cfg.spreadPx > 0.0f ? std::max(cfg.spreadPx, 2.0f * halfWPx / pop + kSpreadGapPx) : 0.0f;
    float rightPx = t.jitterPx + t.spreadSlot * spreadStep + t.driftDir * cfg.driftPx * EaseOutCubic(life); // centre, right of the anchor
    // Crits shake sideways briefly (~12 Hz, decaying).
    constexpr float kShakeSec = 0.35f;
    if (t.crit && cfg.critShakePx > 0.0f && age < kShakeSec)
        rightPx += std::sin(age * 75.0f) * cfg.critShakePx * (1.0f - age / kShakeSec);
    // Only for targets that are horizontally on screen; texts of targets off to the side
    // shouldn't pile up along the edges.
    if (sx >= 0.0f && sx <= static_cast<float>(screenW)) {
        const float top = sy - upPx - hPx;
        const float bottom = sy - upPx;
        if (top < margin)
            upPx -= margin - top;
        else if (bottom > screenH - margin)
            upPx += bottom - (screenH - margin);
        const float left = sx + rightPx - halfWPx;
        const float right = sx + rightPx + halfWPx;
        if (left < margin)
            rightPx += margin - left;
        else if (right > screenW - margin)
            rightPx -= right - (screenW - margin);
    }

    if (t.atlas) {
        // Lay the glyphs out ourselves from the atlas metrics (texels), scaled to pixels, then
        // to world units. Outline cells occupy the first half of the array, fills the second.
        const atlas::Atlas& a = *t.atlas;
        const size_t n = t.draw.size() / 2;
        const float cellCenterAboveBaseline = a.baseline - a.cellH * 0.5f; // texels
        const float yPx = upPx + cellCenterAboveBaseline * pxPerTexel;
        const uint32_t edgeArgb = WithAlpha(0xFFFFFF, alpha); // white vertex colour keeps the black outline
        // Crits flash white and fade to the game colour.
        uint32_t fillRgb = t.fillArgb;
        if (t.crit && cfg.critFlashSec > 0.0f && age < cfg.critFlashSec)
            fillRgb = LerpRgb(0xFFFFFF, t.fillArgb, age / cfg.critFlashSec);
        const uint32_t fillArgb = WithAlpha(fillRgb, static_cast<float>(t.fillArgb >> 24) / 255.0f * alpha);
        float pen = -totalAdvance * 0.5f;
        size_t i = 0;
        for (uint8_t c : t.chars) {
            if (c >= 128 || !a.glyphs[c].present) {
                pen += a.spaceAdvance;
                continue;
            }
            const atlas::Glyph& g = a.glyphs[c];
            const float centerPx = (pen - g.originX + g.cellW * 0.5f) * pxPerTexel;
            for (int pass = 0; pass < 2; ++pass) {
                Sprite& d = t.draw[pass * n + i];
                d.x = offset.x + (centerPx + rightPx) * wpp;
                d.y = offset.y + yPx * wpp;
                d.z = offset.z;
                d.w = g.cellW * pxPerTexel * wpp;
                d.h = a.cellH * pxPerTexel * wpp;
                d.argb = pass == 0 ? edgeArgb : fillArgb;
                d.visible = 1;
            }
            pen += g.advance;
            ++i;
        }
        return;
    }

    // Fallback: the game's glyphs, 12 px * game scale, all the same height.
    float baseH = 0.0f;
    for (const Sprite& s : t.base) {
        if (s.visible) {
            baseH = s.h;
            break;
        }
    }
    if (baseH <= 0.0f)
        return;
    const float k = hPx * wpp / baseH;
    const float hWorld = baseH * k;

    for (size_t l = 0; l < t.layers.size(); ++l) {
        const Layer& layer = t.layers[l];
        for (int i = 0; i < t.count; ++i) {
            const Sprite& b = t.base[i];
            Sprite& d = t.draw[l * t.count + i];
            d = b;
            if (!b.visible)
                continue;
            d.x = offset.x + b.x * k + (rightPx + layer.dx) * wpp;
            d.y = offset.y + hWorld * 0.5f + (upPx + layer.dy) * wpp;
            d.z = offset.z;
            d.w = b.w * k;
            d.h = b.h * k;
            const uint32_t rgb = layer.tinted ? t.outlineRgb : b.argb;
            const float la = (layer.tinted ? 1.0f : static_cast<float>(b.argb >> 24) / 255.0f) * layer.alpha * alpha;
            d.argb = WithAlpha(rgb, la);
        }
    }
}

// The sign for a number on our own character, by the combat-log group it was filed under.
char SignOf(uint32_t group)
{
    switch (group) {
    case 0x42000001: // me hit by environment
    case 0x42000002: // me hit by nano
    case 0x42000006: // me hit by monster
    case 0x42000007: // me hit by player
        return '-';
    case 0x4200000B: // me got XP
    case 0x4200000C: // me got SK
    case 0x42000015: // me got health
    case kGroupMeGotNano:
        return '+';
    default:
        return 0;
    }
}

bool IsNumber(const char* text)
{
    if (!text || !*text)
        return false;
    for (const char* c = text; *c; ++c)
        if (*c < '0' || *c > '9')
            return false;
    return true;
}

// The names the game shows over whoever does them: special attacks ("Fling Shot!", "Burst!",
// "Backstab!", colour 13), perks and actions ("Thunder", "Righteous Fury", colour 3) and taunts
// ("Aggro", "Challenger!", colour 2).
bool IsActionName(const char* text, int colorIdx)
{
    return (colorIdx == 13 || colorIdx == 3 || colorIdx == 2) && !IsNumber(text);
}

// Another player's character (not ours, not an NPC).
bool IsOtherPlayer(void* dynel)
{
    if (!dynel || Field<uint8_t>(static_cast<uint8_t*>(dynel), kDynelIsClientChar))
        return false;
    game::Identity id;
    return game::IdentityOf(dynel, &id) && id.type == kTypeCharacter && !n3::IsNpc(id);
}

void __stdcall Hook_Create(void* dynel, const char* text, int colorIdx)
{
    const FctConfig& cfg = Cfg().fct;
    char sign = g_nextSign;
    g_nextSign = 0;
    if (cfg.enable && !cfg.othersSpecials && IsActionName(text, colorIdx) && IsOtherPlayer(dynel)) {
        if (cfg.debug)
            Log("[fct] hidden (other player's action): '%s'", text);
        return;
    }
    // Attacks on us, for the nameplates' marker: the game's "Attacking You!" over a character,
    // and damage on us with a known attacker (below).
    if (dynel && strcmp(text, "Attacking You!") == 0) {
        game::Identity id;
        if (game::IdentityOf(dynel, &id))
            threat::NoteAttack(id);
    }
    if (g_combatCallRet && g_callerRet == g_combatCallRet) {
        const uint32_t group = *reinterpret_cast<const uint32_t*>(g_callerEbp + kCombatGroupSlot);
        if (SignOf(group) == '-' && g_combatAttacker && !Field<uint8_t>(g_combatAttacker, kDynelIsClientChar)) {
            game::Identity id; // "me hit by monster / player / nano" (environment has no attacker)
            if (game::IdentityOf(g_combatAttacker, &id))
                threat::NoteAttack(id);
        }
    }
    if (cfg.enable && g_combatCallRet && g_callerRet == g_combatCallRet) {
        const uint32_t group = *reinterpret_cast<const uint32_t*>(g_callerEbp + kCombatGroupSlot);
        if (!sign)
            sign = SignOf(group);
        const uint32_t idx = group - kCombatGroupBase;
        if (idx < static_cast<uint32_t>(kCombatGroupCount) && !cfg.showGroup[idx]) {
            if (cfg.debug)
                Log("[fct] hidden (%s): '%s'", kCombatGroups[idx].label, text);
            return;
        }
    }

    int hitType = 0;
    if (g_combatCallRet && g_callerRet == g_combatCallRet)
        hitType = *reinterpret_cast<const int*>(g_callerEbp + kCombatHitTypeSlot);

    // "-123" / "+558" on our own character, so damage taken and gains read apart at a glance.
    char signed_[24];
    if (cfg.enable && cfg.selfSigns && sign && dynel && IsNumber(text) && strlen(text) < sizeof(signed_) - 1 &&
        Field<uint8_t>(static_cast<uint8_t*>(dynel), kDynelIsClientChar)) {
        sprintf_s(signed_, "%c%s", sign, text);
        text = signed_;
    }

    g_pending = {true, dynel, colorIdx, hitType, g_nextRgb};
    g_nextRgb = -1;
    g_origCreate(dynel, text, colorIdx);
    g_pending.active = false;
}

// Our own character's dynel, or null.
void* MyDynel()
{
    const uint32_t me = game::ClientInstance();
    void* dynel = me ? game::DynelById({kTypeCharacter, me}) : nullptr;
    return dynel && Field<uint8_t>(static_cast<uint8_t*>(dynel), kDynelIsClientChar) ? dynel : nullptr;
}

// A text of ours in our own colour, through the CreateFloatingText hook like the game's calls.
void CreateOwnText(void* dynel, const char* text, int colorIdx, uint32_t rgb)
{
    g_nextRgb = static_cast<int32_t>(rgb & 0xFFFFFF);
    g_createGame(dynel, text, colorIdx);
    g_nextRgb = -1;
}

// Whether a chat group's floating text is on ([CombatLog]); logs the hidden text in debug mode.
bool GroupShown(uint32_t group, const char* text)
{
    const FctConfig& cfg = Cfg().fct;
    if (cfg.showGroup[group - kCombatGroupBase])
        return true;
    if (cfg.debug)
        Log("[fct] hidden (%s): '%s'", kCombatGroups[group - kCombatGroupBase].label, text);
    return false;
}

// Nano points gained, over our own character in the nano colour (chat group "Me got nano").
void ShowNanoGain(int amount)
{
    void* me = amount > 0 ? MyDynel() : nullptr;
    if (!me)
        return;
    char buf[16];
    sprintf_s(buf, "%d", amount);
    if (GroupShown(kGroupMeGotNano, buf)) {
        g_nextSign = '+';
        CreateOwnText(me, buf, kColorHeal, Cfg().fct.nanoRgb);
    }
}

// The combat function's cases that make no floating text of their own:
//  - misses: "Miss" over the target when your attack misses (chat group "Your misses") and
//    "Evade" over you when an attack on you misses ("Other misses");
//  - nano points someone gave you ("Me got nano").
void __fastcall Hook_Combat(void* self, void* /*edx*/, int event, uint8_t* target, int amount, uint8_t* attacker,
                            int special, int damageType, int hitType, int extra)
{
    g_combatAttacker = attacker;
    g_origCombat(self, event, target, amount, attacker, special, damageType, hitType, extra);
    g_combatAttacker = nullptr;

    // A miss on us is an attack too (the nameplates' marker).
    if (event == kEventMissed && amount && target && attacker && Field<uint8_t>(target, kDynelIsClientChar) &&
        !Field<uint8_t>(attacker, kDynelIsClientChar)) {
        game::Identity id;
        if (game::IdentityOf(attacker, &id))
            threat::NoteAttack(id);
    }

    const FctConfig& cfg = Cfg().fct;
    if (cfg.debug) {
        const bool targetMe = target && Field<uint8_t>(target, kDynelIsClientChar);
        const bool attackerMe = attacker && Field<uint8_t>(attacker, kDynelIsClientChar);
        Log("[fct] combat event 0x%X amount=%d target=%p%s attacker=%p%s special=%d dmgType=%d hit=%d",
            event, amount, target, targetMe ? "(me)" : "", attacker, attackerMe ? "(me)" : "", special, damageType, hitType);
    }
    if (!cfg.enable || !amount || !target)
        return; // (with no amount the game itself shows nothing, not even the chat line)
    if (event == kEventGotNano) {
        ShowNanoGain(amount);
        return;
    }
    if (event == kEventYouHealed || event == kEventYouGaveNano) {
        // Health or nano you gave someone, over them. On yourself the game already shows it.
        if (amount < 0 || Field<uint8_t>(target, kDynelIsClientChar))
            return;
        char buf[16];
        sprintf_s(buf, "%d", amount);
        if (event == kEventYouHealed) {
            if (GroupShown(kGroupYouGaveHealth, buf))
                g_createGame(target, buf, kColorHeal); // the heal colour ([Colors] 28, green by default)
        } else if (GroupShown(kGroupYouGaveNano, buf)) {
            CreateOwnText(target, buf, kColorHeal, cfg.nanoRgb);
        }
        return;
    }
    if (event != kEventMissed)
        return;
    const char* text;
    uint32_t group, rgb;
    if (attacker && Field<uint8_t>(attacker, kDynelIsClientChar)) {
        text = "Miss";
        group = kGroupYourMisses;
        rgb = cfg.missRgb;
    } else if (Field<uint8_t>(target, kDynelIsClientChar)) {
        text = "Evade";
        group = kGroupOtherMisses;
        rgb = cfg.evadeRgb;
    } else {
        return;
    }
    if (GroupShown(group, text))
        CreateOwnText(target, text, kColorMiss, rgb);
}

// Drains: the game prints "You drained %d points of health/nano from the target." and sets the
// stat directly, so no number is ever made. Show one on our character: health like the game's
// own heals, nano in the nano colour.
void __fastcall Hook_CharAction(void* self, void* /*edx*/, int action, int* p20, int chatGroup, int* p28, void* str)
{
    g_origCharAction(self, action, p20, chatGroup, p28, str);

    const FctConfig& cfg = Cfg().fct;
    if (cfg.debug)
        Log("[fct] character action 0x%X group=0x%X p20=(%d, %d) p28=(%d, %d)", action, chatGroup, p20 ? p20[0] : 0,
            p20 ? p20[1] : 0, p28 ? p28[0] : 0, p28 ? p28[1] : 0);
    // "You increased your nanopool with %d points." (nano programs, items, perks): chat only.
    if (cfg.enable && action == kActionNanoPool && p28 && p28[1] > 0) {
        ShowNanoGain(p28[1]);
        return;
    }
    if (!cfg.enable || !cfg.drainHeals || action != kActionDrain || !p20 || !p28)
        return;
    if (p28[0] != kStatHealth && p28[0] != kStatCurrentNano)
        return;
    const uint32_t me = game::ClientInstance();
    if (!me || static_cast<uint32_t>(p20[1]) != me || p20[0] <= 0)
        return;
    if (cfg.debug)
        Log("[fct] drain %s %d", p28[0] == kStatHealth ? "heal" : "nano", p20[0]);
    if (p28[0] == kStatCurrentNano) {
        ShowNanoGain(p20[0]);
        return;
    }
    void* meDynel = MyDynel();
    if (!meDynel)
        return;
    char buf[16];
    sprintf_s(buf, "%d", p20[0]);
    g_nextSign = '+';
    g_createGame(meDynel, buf, kColorHeal); // through our hook, like the game's own calls
}

// Entry for the CreateFloatingText hook: records the caller's EBP and return address (untouched
// at this point) before continuing into Hook_Create with the original stack.
__declspec(naked) void Hook_CreateEntry()
{
    __asm {
        mov g_callerEbp, ebp
        mov eax, [esp]
        mov g_callerRet, eax
        jmp Hook_Create
    }
}

void __fastcall Hook_SetText(uint8_t* self, void* /*edx*/, const char* text)
{
    // SetText destroys the old visual; anything we gave it is no longer referenced.
    auto it = g_texts.find(self);
    if (it != g_texts.end())
        Retire(it);

    g_origSetText(self, text);

    if (Cfg().fct.enable)
        Register(self, text);
}

void __fastcall Hook_Process(uint8_t* self, void* /*edx*/)
{
    g_origProcess(self);

    static unsigned calls = 0;
    if ((++calls & 63) == 0)
        CollectGarbage();

    auto it = g_texts.find(self);
    if (it == g_texts.end())
        return;
    Text& t = it->second;
    if (Field<uint8_t>(self, kTerminated) || Field<void*>(self, kVisual) != t.visual ||
        Field<Sprite*>(self, kSprites) != t.gameSprites) {
        Retire(it);
        return;
    }
    t.lastSeen = GetTickCount();
    Update(self, t);
}

void SetSelfRoute(bool floatingText)
{
    if (!g_selfRouteJump)
        return;
    const uint8_t want = floatingText ? 0xEB : 0x75; // jmp / jne (original)
    if (*g_selfRouteJump == want)
        return;
    DWORD old;
    if (!VirtualProtect(g_selfRouteJump, 1, PAGE_EXECUTE_READWRITE, &old))
        return;
    *g_selfRouteJump = want;
    VirtualProtect(g_selfRouteJump, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_selfRouteJump, 1);
    Log("[fct] first-person self damage -> %s", floatingText ? "floating text" : "game default");
}

} // namespace

void OnConfigChanged()
{
    const FctConfig& cfg = Cfg().fct;
    SetSelfRoute(cfg.enable && cfg.selfFirstPerson);
}

bool Install()
{
    uint8_t* create = FindUnique(SIG_FCT_CREATE, "CreateFloatingText");
    uint8_t* process = FindUnique(SIG_FONT_PROCESS, "_GfxControlFont_t::Process");
    uint8_t* setText = FindUnique(SIG_FONT_SETTEXT, "_GfxControlFont_t::SetText");
    if (!create || !process || !setText)
        return false;

    // Process and SetText first: Create only adds metadata.
    bool ok = InstallHook("_GfxControlFont_t::Process", process, &Hook_Process, &g_origProcess) &&
              InstallHook("_GfxControlFont_t::SetText", setText, &Hook_SetText, &g_origSetText);
    if (ok) {
        // The combat function's call to CreateFloatingText sits at a fixed spot after the
        // self-damage branch: route+0x38 is "call CreateFloatingText", returning to route+0x3D.
        if (uint8_t* route = FindUnique(SIG_SELF_DAMAGE_ROUTE, "combat floating-text call")) {
            const uint8_t* call = route + 0x38;
            if (call[0] == 0xE8 && call + 5 + *reinterpret_cast<const int32_t*>(call + 1) == create)
                g_combatCallRet = reinterpret_cast<uint32_t>(call + 5);
            else
                Log("[fct] combat call not where expected - others' damage filter off");
        }
        InstallHook("CreateFloatingText", create, &Hook_CreateEntry, &g_origCreate);
        g_createGame = reinterpret_cast<CreateFn>(create);
        if (uint8_t* charAction = FindUnique(SIG_CHAR_ACTION, "CharacterAction handler"))
            InstallHook("CharacterAction handler", charAction, &Hook_CharAction, &g_origCharAction);
        if (uint8_t* combat = FindUnique(SIG_COMBAT_MESSAGE, "combat-message function"))
            InstallHook("combat-message function", combat, &Hook_Combat, &g_origCombat);
    }
    if (ok) {
        // Optional: damage on yourself in first person goes to an off-screen 2D text in the
        // original client; flipping one jump sends it through floating text instead.
        if (uint8_t* route = FindUnique(SIG_SELF_DAMAGE_ROUTE, "self damage routing")) {
            if (route[16] == 0x75 || route[16] == 0xEB)
                g_selfRouteJump = route + 16;
            else
                Log("[fct] self damage routing: unexpected byte %02X, not patched", route[16]);
        }
        OnConfigChanged();
    }
    Log("[fct] %s", ok ? "ready" : "disabled");
    return ok;
}

} // namespace prk::fct
