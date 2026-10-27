#include "config.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
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
    "; Nano damage the game sends without saying whose (everyone's: floods in raids)\n"
    "ShowOtherHitByNano=0\n"
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
    "; 0 = Modern, 1 = Classic (the game's look - name only, corner marks on targets - sharp at any size)\n"
    "Style=0\n"
    "; Size of the name's capitals in world units; the result on screen is clamped to MinPx..MaxPx\n"
    "NaturalSize=0.14\n"
    "MinPx=11\n"
    "MaxPx=20\n"
    "ShowPlayers=1\n"
    "ShowNpcs=1\n"
    "ShowPets=1\n"
    "; 1 = your own character always has a nameplate (not in first person)\n"
    "ShowSelf=0\n"
    "ShowLevel=1\n"
    "; Org / title line\n"
    "ShowOrg=1\n"
    "ShowHealth=1\n"
    "; Health bar and level colour: 0 = classic (the game's consider colours: grey, green, yellow, red by level\n"
    "; difference), 1 = one colour for all, BarHue (0-360: 0 red, 60 yellow, 120 green, 180 cyan, 240 blue,\n"
    "; 300 magenta), 2 = by health (green at full health, through yellow, to red near death)\n"
    "BarColor=0\n"
    "BarHue=0\n"
    "; Brightness of those colours (0.3-1, 1 = the pure colours)\n"
    "BarBrightness=0.9\n"
    "; Font and weight (100-900, 400 = regular, 700 = bold). Applied at game start.\n"
    "Font=Verdana\n"
    "FontWeight=400\n"
    "; 1 = left-click a nameplate to select that character\n"
    "ClickToTarget=1\n"
    "; 1 = don't let nameplates overlap: farther ones move up above nearer ones\n"
    "Stack=1\n"
    "; Opacity of plates other than your selected / fight target (0.1-1)\n"
    "OtherOpacity=0.9\n"
    "; Dark panel behind level, name and health bar, so plates stand out on bright backgrounds (0-1, 0 = none)\n"
    "BackgroundOpacity=0.1\n"
    "; Your selected / fight target's plate is this much bigger than the others (1-2, 1.1 = 10% bigger)\n"
    "TargetScale=1.1\n"
    "; Fade in when a plate appears (s), and fade out from FadeStart metres to the game's 30 m range\n"
    "FadeInSec=0.25\n"
    "FadeStart=28\n"
    "; Extra NPCFamily values counted as pets (comma-separated). The known pet families are built in\n"
    "; (95-98 players' pets, 221 Devileet); see Debug=1 output for a character's NPCFamily.\n"
    "PetFamilies=\n"
    "\n"
    "[Bars]\n"
    "; 1 = Clarity's health/nano and target bars instead of the game's; 0 = the game's bars\n"
    "Enable=0\n"
    "; Bars for your fight target and your nano target (the nano target's is smaller while you have a fight\n"
    "; target, and takes the fight target's place when you don't)\n"
    "ShowTarget=1\n"
    "; A small bar for whom your fight target is targeting (maybe you)\n"
    "ShowTargetTarget=1\n"
    "; 1 = the bars can't be dragged\n"
    "Locked=0\n"
    "; Health just lost shows as a light segment that shrinks away\n"
    "Trail=1\n"
    "; Numbers on the bars: 0 = amount (8,412 / 12,950), 1 = percent, 2 = both\n"
    "Numbers=2\n"
    "; Size (0.6-2)\n"
    "Scale=1\n"
    "; Where the bars are (top-left, GUI units); set when you drag one. -1 = the default place.\n"
    "SelfX=-1\n"
    "SelfY=-1\n"
    "TargetX=-1\n"
    "TargetY=-1\n"
    "NanoX=-1\n"
    "NanoY=-1\n"
    "TotX=-1\n"
    "TotY=-1\n"
    "\n"
    "[Interface]\n"
    "; Key that hides the game's interface (windows, chat, bars) and shows it again; nameplates and\n"
    "; floating text stay. Ctrl, Shift, Alt and a letter, digit or F1-F12, e.g. Ctrl+Shift+U. Empty = no key.\n"
    "HideUiKey=Ctrl+Shift+U\n";

// "Ctrl+Shift+U", "Alt+F12", "Ctrl+7": modifiers and one key; vk 0 if not understood.
Hotkey ParseHotkey(const wchar_t* text)
{
    Hotkey k;
    std::wstring s(text);
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(L'+', start);
        if (end == std::wstring::npos)
            end = s.size();
        std::wstring part = s.substr(start, end - start);
        part.erase(0, part.find_first_not_of(L" \t"));
        part.erase(part.find_last_not_of(L" \t") + 1);
        for (wchar_t& ch : part)
            ch = static_cast<wchar_t>(towupper(ch));
        if (part == L"CTRL" || part == L"CONTROL") {
            k.ctrl = true;
        } else if (part == L"SHIFT") {
            k.shift = true;
        } else if (part == L"ALT") {
            k.alt = true;
        } else if (part.size() == 1 && ((part[0] >= L'A' && part[0] <= L'Z') || (part[0] >= L'0' && part[0] <= L'9'))) {
            k.vk = part[0];
        } else if (part.size() >= 2 && part[0] == L'F') {
            const int n = _wtoi(part.c_str() + 1);
            if (n >= 1 && n <= 12)
                k.vk = VK_F1 + n - 1;
        } else if (!part.empty()) {
            return {};
        }
        start = end + 1;
    }
    return k;
}

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
            c.showGroup[i] = d.showGroup[i]; // not a combat group
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
    n.style = std::clamp(static_cast<int>(ReadFloat(p, ns, L"Style", static_cast<float>(dn.style))), 0, 1);
    n.naturalSize = ReadFloat(p, ns, L"NaturalSize", dn.naturalSize);
    n.minPx = ReadFloat(p, ns, L"MinPx", dn.minPx);
    n.maxPx = ReadFloat(p, ns, L"MaxPx", dn.maxPx);
    if (n.maxPx < n.minPx)
        n.maxPx = n.minPx;
    n.showPlayers = ReadFloat(p, ns, L"ShowPlayers", dn.showPlayers) != 0.0f;
    n.showNpcs = ReadFloat(p, ns, L"ShowNpcs", dn.showNpcs) != 0.0f;
    n.showPets = ReadFloat(p, ns, L"ShowPets", dn.showPets) != 0.0f;
    n.showSelf = ReadFloat(p, ns, L"ShowSelf", dn.showSelf) != 0.0f;
    n.showLevel = ReadFloat(p, ns, L"ShowLevel", dn.showLevel) != 0.0f;
    n.showOrg = ReadFloat(p, ns, L"ShowOrg", dn.showOrg) != 0.0f;
    n.showHealth = ReadFloat(p, ns, L"ShowHealth", dn.showHealth) != 0.0f;
    n.barColor = std::clamp(static_cast<int>(ReadFloat(p, ns, L"BarColor", static_cast<float>(dn.barColor))), 0, 2);
    n.barHue = std::clamp(ReadFloat(p, ns, L"BarHue", dn.barHue), 0.0f, 360.0f);
    n.barBrightness = std::clamp(ReadFloat(p, ns, L"BarBrightness", dn.barBrightness), 0.3f, 1.0f);
    GetPrivateProfileStringW(ns, L"Font", dn.fontName, n.fontName, 64, p);
    n.fontWeight = static_cast<int>(ReadFloat(p, ns, L"FontWeight", static_cast<float>(dn.fontWeight)));
    n.clickToTarget = ReadFloat(p, ns, L"ClickToTarget", dn.clickToTarget) != 0.0f;
    n.stack = ReadFloat(p, ns, L"Stack", dn.stack) != 0.0f;
    n.otherOpacity = ReadFloat(p, ns, L"OtherOpacity", dn.otherOpacity);
    n.backgroundOpacity = ReadFloat(p, ns, L"BackgroundOpacity", dn.backgroundOpacity);
    n.targetScale = std::clamp(ReadFloat(p, ns, L"TargetScale", dn.targetScale), 1.0f, 2.0f);
    n.fadeInSec = ReadFloat(p, ns, L"FadeInSec", dn.fadeInSec);
    n.fadeStart = ReadFloat(p, ns, L"FadeStart", dn.fadeStart);
    {
        // The known pet families always apply; PetFamilies only adds to them (older Clarity.ini
        // files list the old defaults, which are already in).
        n.petFamilyCount = dn.petFamilyCount;
        std::copy(dn.petFamilies, dn.petFamilies + dn.petFamilyCount, n.petFamilies);
        wchar_t list[128];
        GetPrivateProfileStringW(ns, L"PetFamilies", L"", list, 128, p);
        for (wchar_t* cur = list; *cur && n.petFamilyCount < 16;) {
            wchar_t* end = nullptr;
            const long v = wcstol(cur, &end, 10);
            if (end == cur) {
                ++cur;
                continue;
            }
            if (std::find(n.petFamilies, n.petFamilies + n.petFamilyCount, static_cast<int>(v)) == n.petFamilies + n.petFamilyCount)
                n.petFamilies[n.petFamilyCount++] = static_cast<int>(v);
            cur = end;
        }
    }

    {
        BarsConfig& b = g_bufs[next].bars;
        const BarsConfig db;
        const wchar_t* bs = L"Bars";
        b.enable = ReadFloat(p, bs, L"Enable", db.enable) != 0.0f;
        b.showTarget = ReadFloat(p, bs, L"ShowTarget", db.showTarget) != 0.0f;
        b.locked = ReadFloat(p, bs, L"Locked", db.locked) != 0.0f;
        b.trail = ReadFloat(p, bs, L"Trail", db.trail) != 0.0f;
        b.numbers = std::clamp(static_cast<int>(ReadFloat(p, bs, L"Numbers", static_cast<float>(db.numbers))), 0, 2);
        b.scale = std::clamp(ReadFloat(p, bs, L"Scale", db.scale), 0.6f, 2.0f);
        b.selfX = ReadFloat(p, bs, L"SelfX", db.selfX);
        b.selfY = ReadFloat(p, bs, L"SelfY", db.selfY);
        b.targetX = ReadFloat(p, bs, L"TargetX", db.targetX);
        b.targetY = ReadFloat(p, bs, L"TargetY", db.targetY);
        b.nanoX = ReadFloat(p, bs, L"NanoX", db.nanoX);
        b.nanoY = ReadFloat(p, bs, L"NanoY", db.nanoY);
        b.totX = ReadFloat(p, bs, L"TotX", db.totX);
        b.totY = ReadFloat(p, bs, L"TotY", db.totY);
        b.showTargetTarget = ReadFloat(p, bs, L"ShowTargetTarget", db.showTargetTarget) != 0.0f;
    }

    {
        InterfaceConfig& ui = g_bufs[next].ui;
        wchar_t key[64];
        GetPrivateProfileStringW(L"Interface", L"HideUiKey", L"Ctrl+Shift+U", key, 64, p);
        ui.hideUi = ParseHotkey(key);
        if (key[0] && !ui.hideUi.vk)
            Log("[cfg] HideUiKey '%ls' not understood - no key", key);
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
