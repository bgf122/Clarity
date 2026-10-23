#pragma once

#include <cstdint>
#include <string>

namespace prk {

constexpr int kGameColorCount = 0x53; // entries in Gamecode's floating-text colour table
constexpr int kColorIndexHeal = 28;   // the game's colour index for heals (events 0x22/0x23)

// The game's combat-log categories (chat groups 0x42000001..0x42000018) the combat function
// files each floating number under. Index = group - 0x42000001; 0x0D-0x10 aren't combat groups.
constexpr uint32_t kCombatGroupBase = 0x42000001;
constexpr int kCombatGroupCount = 0x18;
struct CombatGroupInfo {
    const wchar_t* key; // INI key (Show<key>), empty = not a combat group
    const char* label;  // the game's name for it
    bool show;          // default
};
inline constexpr CombatGroupInfo kCombatGroups[kCombatGroupCount] = {
    {L"MeHitByEnvironment", "Me hit by environment", true},
    {L"MeHitByNano", "Me hit by nano", true},
    {L"YourPetHitByNano", "Your pet hit by nano", true},
    // Nano damage the server sends without an attacker: unknown source, always hidden (no key, so
    // it is neither read from Clarity.ini nor offered in the options tab).
    {L"", "Other hit by nano (unknown source)", false},
    {L"YouHitOtherWithNano", "You hit other with nano", true},
    {L"MeHitByMonster", "Me hit by monster", true},
    {L"MeHitByPlayer", "Me hit by player", true},
    {L"YouHitOther", "You hit other", true},
    {L"YourPetHitByOther", "Your pet hit by other", true},
    {L"OtherHitByOther", "Other hit by other", false},
    {L"MeGotXP", "Me got XP", true},
    {L"MeGotSK", "Me got SK", true},
    {L"", "", true}, {L"", "", true}, {L"", "", true}, {L"", "", true},
    {L"YourPetHitByMonster", "Your pet hit by monster", true},
    {L"YourMisses", "Your misses", true},
    {L"OtherMisses", "Other misses", true},
    {L"YouGaveHealth", "You gave health", true},
    {L"MeGotHealth", "Me got health", true},
    {L"MeGotNano", "Me got nano", true},
    {L"YouGaveNano", "You gave nano", true},
    {L"MeCastNano", "Me cast nano", true},
};

struct FctConfig {
    bool enable = true;
    float sizePx = 22.0f;        // glyph height on screen; original atlas glyphs are 12 px
    float lifetimeSec = 1.6f;
    float risePx = 50.0f;        // total rise over the lifetime, eased out
    float popScale = 1.5f;       // start scale of the pop-in
    float popSec = 0.15f;
    float fadeStart = 0.6f;      // fraction of lifetime where fade-out begins
    bool outline = true;
    float outlinePx = 1.5f;
    uint32_t outlineRgb = 0x000000;
    float outlineAlpha = 0.85f;
    float shadowPx = 2.0f;       // 0 = no drop shadow
    float critScale = 1.6f;
    float critPop = 2.0f;        // crit pop-in start scale (springs back with an overshoot)
    float critFlashSec = 0.2f;   // crits flash white for this long, 0 = off
    float critShakePx = 4.0f;    // crit sideways shake amplitude, 0 = off
    float glanceScale = 0.85f;   // glancing hits (the server's hit type 2) are drawn this much smaller
    float actionScale = 0.55f;   // special attack, perk and taunt names ("Fling Shot!", "Thunder"): ~ the game's own size
    bool drainHeals = true;      // show health drains as heals on yourself
    uint32_t missRgb = 0xC0C0C0;  // "Miss" over the target when your attack misses ([CombatLog] ShowYourMisses)
    uint32_t evadeRgb = 0x9FD4FF; // "Evade" over you when an attack on you misses ([CombatLog] ShowOtherMisses)
    uint32_t nanoRgb = 0x4FA8FF;  // nano points you gain or drain ([CombatLog] ShowMeGotNano)
    float stackWindowSec = 0.6f; // texts on the same target within this window stack upwards
    float jitterPx = 6.0f;
    float spreadPx = 36.0f;      // minimum horizontal step between the start points of texts on one target
    float driftPx = 0.0f;        // sideways drift over the lifetime, away from the centre (0 = straight up)
    bool showGroup[kCombatGroupCount]; // per combat-log category (kCombatGroups)
    bool othersSpecials = false; // special attack / perk / taunt names over other players
    bool onTop = true;           // draw over geometry (no depth test)
    float headOffset = -0.3f;    // world units relative to the target's name-tag point
    float screenMarginPx = 16.0f; // keep text this far inside the window edges
    float anchorTop = 0.3f;      // texts never start above this fraction of the screen height
    float selfScreenY = 0.6f;    // first person: own texts start here (fraction of screen height)
    bool selfFirstPerson = true; // route damage on yourself in first person to floating text
    bool selfSigns = true;       // "-" before damage you take, "+" before what you gain (on your own character)
    bool followTarget = false;   // texts move with their target; off = they stay where they appeared
    wchar_t fontName[64] = L"Segoe UI"; // glyph atlas font (read once, at the first text)
    int fontWeight = 800;
    bool debug = false;
    int32_t colors[kGameColorCount]; // RRGGBB override per game colour index, -1 = keep game colour

    FctConfig()
    {
        for (auto& c : colors)
            c = -1;
        colors[kColorIndexHeal] = 0x4CDF5A; // heals in green
        for (int i = 0; i < kCombatGroupCount; ++i)
            showGroup[i] = kCombatGroups[i].show;
    }
};

struct NameplateConfig {
    bool enable = true;
    float naturalSize = 0.14f; // world height of the name's capitals before clamping
    float minPx = 11.0f;       // name capitals on screen never smaller than this ...
    float maxPx = 20.0f;       // ... or larger than this
    bool showPlayers = true;
    bool showNpcs = true;
    bool showPets = true;
    bool showLevel = true;
    bool showOrg = true;       // org / title line
    bool showHealth = true;
    wchar_t fontName[64] = L"Verdana"; // the GUI's TrueType face; read at game start
    int fontWeight = 400;
    bool clickToTarget = true; // left-click a plate to select that character
    bool stack = true;         // move overlapping plates apart
    float otherOpacity = 0.9f; // plates other than the selected / fight target
    float fadeInSec = 0.25f;
    float fadeStart = 28.0f;   // metres from you; fully faded at the game's 30 m tag range
    int petFamilies[16] = {95, 96, 97, 98}; // NPCFamily values that are pets (98: MP pets)
    int petFamilyCount = 4;
};

struct Config {
    FctConfig fct;
    NameplateConfig np;
};

// Current settings. Written only by LoadConfig (init thread); read by the game thread.
const Config& Cfg();

// Full path of Clarity.ini.
const std::wstring& ConfigPath();

// Loads Clarity.ini from the client folder, writing a commented default file if missing.
void LoadConfig();

// Reloads if Clarity.ini changed on disk since the last load. Returns true if reloaded.
bool ReloadConfigIfChanged();

} // namespace prk
