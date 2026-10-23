#include "config.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace prk {

namespace {

// Double-buffered: the reload fills the inactive copy, then publishes it.
Config g_bufs[2];
std::atomic<int> g_cur{0};
FILETIME g_lastWrite{};

const char kDefaultIni[] =
    "; Clarity - UI improvements for Anarchy Online. Changes are picked up while the game runs.\n"
    "\n"
    "[FloatingText]\n"
    "; 0 = original floating combat text\n"
    "Enable=1\n"
    "; Text height on screen in pixels, kept constant at any camera distance\n"
    "SizePx=22\n"
    "; Font for the numbers (any installed font) and its weight 100-900. Applied at game start.\n"
    "Font=Segoe UI\n"
    "FontWeight=800\n"
    "LifetimeSec=1.6\n"
    "; Total upward drift in pixels (eased out)\n"
    "RisePx=50\n"
    "; Pop-in: start scale and duration\n"
    "PopScale=1.5\n"
    "PopSec=0.15\n"
    "; Fraction of the lifetime after which the text fades out\n"
    "FadeStart=0.6\n"
    "Outline=1\n"
    "OutlinePx=1.5\n"
    "OutlineColor=000000\n"
    "OutlineAlpha=0.85\n"
    "; Drop shadow offset in pixels, 0 = off\n"
    "ShadowPx=2\n"
    "; Critical hits (as reported by the server) are drawn CritScale x larger, glancing hits\n"
    "; GlanceScale x\n"
    "CritScale=1.6\n"
    "GlanceScale=0.85\n"
    "; Special attack, perk and taunt names (\"Fling Shot!\", \"Thunder\") are drawn ActionScale x\n"
    "; (0.55 is about the game's own size up close)\n"
    "ActionScale=0.55\n"
    "; 1 = show health drains as heals on yourself\n"
    "DrainHeals=1\n"
    "; Colours of \"Miss\" (your attack missed) and \"Evade\" (an attack on you missed); turn them\n"
    "; off with ShowYourMisses / ShowOtherMisses in [CombatLog]\n"
    "MissColor=C0C0C0\n"
    "EvadeColor=9FD4FF\n"
    "; Colour of nano points you gain or drain (ShowMeGotNano in [CombatLog])\n"
    "NanoColor=4FA8FF\n"
    "; Crit effects: pop-in start scale, white flash duration (s), sideways shake (px); 0 = off\n"
    "CritPop=2\n"
    "CritFlashSec=0.2\n"
    "CritShakePx=4\n"
    "; Texts on the same target within this window stack instead of overlapping\n"
    "StackWindowSec=0.6\n"
    "; Random sideways offset in pixels\n"
    "JitterPx=6\n"
    "; Consecutive texts on one target start side by side, alternating sides: at least this many\n"
    "; pixels apart, or their own width if wider. 0 = all start at the same spot\n"
    "SpreadPx=36\n"
    "; Sideways drift outwards while rising, in pixels. 0 = straight up\n"
    "DriftPx=0\n"
    "; 1 = show other players' special attack, perk and taunt names over them\n"
    "OthersSpecials=0\n"
    "; 1 = draw over everything, so large targets can't hide their own numbers\n"
    "OnTop=1\n"
    "; Start this many world units from the target's name-tag point (negative = lower)\n"
    "HeadOffset=-0.3\n"
    "; Keep text at least this many pixels inside the window edges\n"
    "ScreenMarginPx=16\n"
    "; Texts never start higher than this fraction of the screen height (0 = top, 1 = bottom)\n"
    "AnchorTop=0.3\n"
    "; First person: your own texts start at this fraction of the screen height\n"
    "SelfScreenY=0.6\n"
    "; 1 = show damage you take in first person (the game sends it to an off-screen text)\n"
    "SelfFirstPerson=1\n"
    "; 1 = numbers on yourself get a sign: -damage taken, +health, nano, XP and SK gained\n"
    "SelfSigns=1\n"
    "; 1 = texts move with their target; 0 = they rise from where they appeared\n"
    "FollowTarget=0\n"
    "; 1 = log every floating text with its colour index to Clarity.log\n"
    "Debug=0\n"
    "\n"
    "[Colors]\n"
    "; Override the game's floating-text colours by colour index: <index>=RRGGBB\n"
    "; (decimal index, see Debug=1 output). Known: 22 = your nano damage, 23/24 = damage you take,\n"
    "; 25 = your hits, 26/27 = others' damage, 28 = heals (green unless set here).\n"
    "; game = the game's own colour, e.g. 28=game\n"
    ";25=FFE680\n"
    "\n"
    "[CombatLog]\n"
    "; Floating numbers per combat-log category (the game's chat groups); 0 = hide\n"
    "ShowMeHitByEnvironment=1\n"
    "ShowMeHitByNano=1\n"
    "ShowYourPetHitByNano=1\n"
    "ShowYouHitOtherWithNano=1\n"
    "ShowMeHitByMonster=1\n"
    "ShowMeHitByPlayer=1\n"
    "ShowYouHitOther=1\n"
    "ShowYourPetHitByOther=1\n"
    "ShowOtherHitByOther=0\n"
    "ShowMeGotXP=1\n"
    "ShowMeGotSK=1\n"
    "ShowYourPetHitByMonster=1\n"
    "ShowYourMisses=1\n"
    "ShowOtherMisses=1\n"
    "ShowYouGaveHealth=1\n"
    "ShowMeGotHealth=1\n"
    "ShowMeGotNano=1\n"
    "ShowYouGaveNano=1\n"
    "ShowMeCastNano=1\n"
    "\n"
    "[Nameplates]\n"
    "; 0 = the game's original name tags\n"
    "Enable=1\n"
    "; Size of the name's capitals in world units; the result on screen is clamped to MinPx..MaxPx\n"
    "NaturalSize=0.14\n"
    "MinPx=11\n"
    "MaxPx=20\n"
    "ShowPlayers=1\n"
    "ShowNpcs=1\n"
    "ShowPets=1\n"
    "ShowLevel=1\n"
    "; Org / title line\n"
    "ShowOrg=1\n"
    "ShowHealth=1\n"
    "; Font and weight (100-900, 400 = regular, 700 = bold). Applied at game start.\n"
    "Font=Verdana\n"
    "FontWeight=400\n"
    "; 1 = left-click a nameplate to select that character\n"
    "ClickToTarget=1\n"
    "; 1 = don't let nameplates overlap: farther ones move up above nearer ones\n"
    "Stack=1\n"
    "; Opacity of plates other than your selected / fight target (0.1-1)\n"
    "OtherOpacity=0.9\n"
    "; Fade in when a plate appears (s), and fade out from FadeStart metres to the game's 30 m range\n"
    "FadeInSec=0.25\n"
    "FadeStart=28\n"
    "; NPCFamily values counted as pets (comma-separated)\n"
    "PetFamilies=95,96,97,98\n";

std::wstring IniPath()
{
    return ModuleDir() + L"Clarity.ini";
}

float ReadFloat(const wchar_t* path, const wchar_t* section, const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, path);
    if (!buf[0])
        return def;
    return static_cast<float>(_wtof(buf));
}

int32_t ReadRgb(const wchar_t* path, const wchar_t* section, const wchar_t* key, int32_t def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, path);
    if (!buf[0])
        return def;
    if (_wcsicmp(buf, L"game") == 0 || buf[0] == L'-')
        return -1; // the game's own colour
    const wchar_t* p = buf;
    if (*p == L'#')
        ++p;
    return static_cast<int32_t>(wcstoul(p, nullptr, 16) & 0xFFFFFF);
}

bool GetWriteTime(const std::wstring& path, FILETIME* out)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    *out = data.ftLastWriteTime;
    return true;
}

} // namespace

const std::wstring& ConfigPath()
{
    static const std::wstring path = IniPath();
    return path;
}

const Config& Cfg()
{
    return g_bufs[g_cur.load(std::memory_order_acquire)];
}

void LoadConfig()
{
    const std::wstring path = IniPath();
    // The addon used to be called PRKUI: keep the user's settings.
    const std::wstring old = ModuleDir() + L"PRKUI.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && MoveFileW(old.c_str(), path.c_str()))
        Log("[cfg] renamed PRKUI.ini to Clarity.ini");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(path.c_str(), L"w")) {
            fputs(kDefaultIni, f);
            fclose(f);
            Log("[cfg] wrote default Clarity.ini");
        }
    }
    GetWriteTime(path, &g_lastWrite);

    const wchar_t* p = path.c_str();
    const wchar_t* s = L"FloatingText";
    const int next = 1 - g_cur.load(std::memory_order_relaxed);
    FctConfig& c = g_bufs[next].fct;
    const FctConfig d;
    c.enable = ReadFloat(p, s, L"Enable", d.enable) != 0.0f;
    c.sizePx = ReadFloat(p, s, L"SizePx", d.sizePx);
    c.lifetimeSec = ReadFloat(p, s, L"LifetimeSec", d.lifetimeSec);
    c.risePx = ReadFloat(p, s, L"RisePx", d.risePx);
    c.popScale = ReadFloat(p, s, L"PopScale", d.popScale);
    c.popSec = ReadFloat(p, s, L"PopSec", d.popSec);
    c.fadeStart = ReadFloat(p, s, L"FadeStart", d.fadeStart);
    c.outline = ReadFloat(p, s, L"Outline", d.outline) != 0.0f;
    c.outlinePx = ReadFloat(p, s, L"OutlinePx", d.outlinePx);
    c.outlineRgb = static_cast<uint32_t>(ReadRgb(p, s, L"OutlineColor", d.outlineRgb));
    c.outlineAlpha = ReadFloat(p, s, L"OutlineAlpha", d.outlineAlpha);
    c.missRgb = static_cast<uint32_t>(ReadRgb(p, s, L"MissColor", d.missRgb));
    c.evadeRgb = static_cast<uint32_t>(ReadRgb(p, s, L"EvadeColor", d.evadeRgb));
    c.nanoRgb = static_cast<uint32_t>(ReadRgb(p, s, L"NanoColor", d.nanoRgb));
    c.shadowPx = ReadFloat(p, s, L"ShadowPx", d.shadowPx);
    c.critScale = ReadFloat(p, s, L"CritScale", d.critScale);
    c.glanceScale = ReadFloat(p, s, L"GlanceScale", d.glanceScale);
    c.actionScale = std::clamp(ReadFloat(p, s, L"ActionScale", d.actionScale), 0.3f, 1.5f);
    c.drainHeals = ReadFloat(p, s, L"DrainHeals", d.drainHeals) != 0.0f;
    c.critPop = ReadFloat(p, s, L"CritPop", d.critPop);
    c.critFlashSec = ReadFloat(p, s, L"CritFlashSec", d.critFlashSec);
    c.critShakePx = ReadFloat(p, s, L"CritShakePx", d.critShakePx);
    c.stackWindowSec = ReadFloat(p, s, L"StackWindowSec", d.stackWindowSec);
    c.jitterPx = ReadFloat(p, s, L"JitterPx", d.jitterPx);
    c.spreadPx = ReadFloat(p, s, L"SpreadPx", d.spreadPx);
    c.driftPx = ReadFloat(p, s, L"DriftPx", d.driftPx);
    // Per combat-log category. The old ShowOthers=1 switched on both "other" categories.
    const bool showOthers = ReadFloat(p, s, L"ShowOthers", 0.0f) != 0.0f;
    for (int i = 0; i < kCombatGroupCount; ++i) {
        if (!kCombatGroups[i].key[0]) {
            c.showGroup[i] = d.showGroup[i]; // not configurable (e.g. unknown-source nano damage: off)
            continue;
        }
        const bool otherGroup = i == 0x09;
        wchar_t key[48];
        swprintf_s(key, L"Show%s", kCombatGroups[i].key);
        c.showGroup[i] = ReadFloat(p, L"CombatLog", key, (d.showGroup[i] || (otherGroup && showOthers)) ? 1.0f : 0.0f) != 0.0f;
    }
    c.othersSpecials = ReadFloat(p, s, L"OthersSpecials", d.othersSpecials) != 0.0f;
    c.onTop = ReadFloat(p, s, L"OnTop", d.onTop) != 0.0f;
    c.headOffset = ReadFloat(p, s, L"HeadOffset", d.headOffset);
    c.screenMarginPx = ReadFloat(p, s, L"ScreenMarginPx", d.screenMarginPx);
    c.anchorTop = ReadFloat(p, s, L"AnchorTop", d.anchorTop);
    c.selfScreenY = ReadFloat(p, s, L"SelfScreenY", d.selfScreenY);
    c.selfFirstPerson = ReadFloat(p, s, L"SelfFirstPerson", d.selfFirstPerson) != 0.0f;
    c.selfSigns = ReadFloat(p, s, L"SelfSigns", d.selfSigns) != 0.0f;
    c.followTarget = ReadFloat(p, s, L"FollowTarget", d.followTarget) != 0.0f;
    GetPrivateProfileStringW(s, L"Font", d.fontName, c.fontName, 64, p);
    c.fontWeight = static_cast<int>(ReadFloat(p, s, L"FontWeight", static_cast<float>(d.fontWeight)));
    c.debug = ReadFloat(p, s, L"Debug", d.debug) != 0.0f;

    // Sanity clamps so a typo can't make text vanish or explode.
    if (c.sizePx < 6.0f) c.sizePx = 6.0f;
    if (c.sizePx > 128.0f) c.sizePx = 128.0f;
    if (c.lifetimeSec < 0.3f) c.lifetimeSec = 0.3f;
    if (c.fadeStart < 0.0f) c.fadeStart = 0.0f;
    if (c.fadeStart > 0.99f) c.fadeStart = 0.99f;
    if (c.popSec < 0.0f) c.popSec = 0.0f;

    for (int i = 0; i < kGameColorCount; ++i) {
        wchar_t key[8];
        swprintf(key, 8, L"%d", i);
        c.colors[i] = ReadRgb(p, L"Colors", key, d.colors[i]);
    }

    NameplateConfig& n = g_bufs[next].np;
    const NameplateConfig dn;
    const wchar_t* ns = L"Nameplates";
    n.enable = ReadFloat(p, ns, L"Enable", dn.enable) != 0.0f;
    n.naturalSize = ReadFloat(p, ns, L"NaturalSize", dn.naturalSize);
    n.minPx = ReadFloat(p, ns, L"MinPx", dn.minPx);
    n.maxPx = ReadFloat(p, ns, L"MaxPx", dn.maxPx);
    if (n.maxPx < n.minPx)
        n.maxPx = n.minPx;
    n.showPlayers = ReadFloat(p, ns, L"ShowPlayers", dn.showPlayers) != 0.0f;
    n.showNpcs = ReadFloat(p, ns, L"ShowNpcs", dn.showNpcs) != 0.0f;
    n.showPets = ReadFloat(p, ns, L"ShowPets", dn.showPets) != 0.0f;
    n.showLevel = ReadFloat(p, ns, L"ShowLevel", dn.showLevel) != 0.0f;
    n.showOrg = ReadFloat(p, ns, L"ShowOrg", dn.showOrg) != 0.0f;
    n.showHealth = ReadFloat(p, ns, L"ShowHealth", dn.showHealth) != 0.0f;
    GetPrivateProfileStringW(ns, L"Font", dn.fontName, n.fontName, 64, p);
    n.fontWeight = static_cast<int>(ReadFloat(p, ns, L"FontWeight", static_cast<float>(dn.fontWeight)));
    n.clickToTarget = ReadFloat(p, ns, L"ClickToTarget", dn.clickToTarget) != 0.0f;
    n.stack = ReadFloat(p, ns, L"Stack", dn.stack) != 0.0f;
    n.otherOpacity = ReadFloat(p, ns, L"OtherOpacity", dn.otherOpacity);
    n.fadeInSec = ReadFloat(p, ns, L"FadeInSec", dn.fadeInSec);
    n.fadeStart = ReadFloat(p, ns, L"FadeStart", dn.fadeStart);
    {
        wchar_t list[128];
        GetPrivateProfileStringW(ns, L"PetFamilies", L"95,96,97,98", list, 128, p);
        n.petFamilyCount = 0;
        for (wchar_t* cur = list; *cur && n.petFamilyCount < 16;) {
            wchar_t* end = nullptr;
            const long v = wcstol(cur, &end, 10);
            if (end == cur) {
                ++cur;
                continue;
            }
            n.petFamilies[n.petFamilyCount++] = static_cast<int>(v);
            cur = end;
        }
    }

    g_cur.store(next, std::memory_order_release);

    Log("[cfg] loaded: FloatingText Enable=%d SizePx=%.1f Lifetime=%.2f Rise=%.0f Outline=%d Shadow=%.1f Debug=%d",
        c.enable, c.sizePx, c.lifetimeSec, c.risePx, c.outline, c.shadowPx, c.debug);
}

bool ReloadConfigIfChanged()
{
    FILETIME now;
    if (!GetWriteTime(IniPath(), &now) || CompareFileTime(&now, &g_lastWrite) == 0)
        return false;
    LoadConfig();
    return true;
}

} // namespace prk
