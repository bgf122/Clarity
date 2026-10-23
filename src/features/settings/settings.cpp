#include "settings.h"

#include "../../core/config.h"
#include "../../core/hooks.h"
#include "../../core/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace prk::settings {

namespace {

// --- the settings shown on the page -------------------------------------------------------

struct Item {
    const char* var;       // DValue name
    const wchar_t* key;    // key in Clarity.ini
    const char* label;
    bool isBool;
    bool FctConfig::*boolField;
    float FctConfig::*floatField;
    bool NameplateConfig::*npBool;
    float NameplateConfig::*npFloat;
    double min, max;
    const char* fmt;       // printf for the slider label (value * scale), XML-escaped
    double scale;
    const char* section;   // heading printed before this item, or nullptr
    int group = -1;        // combat-log category index (kCombatGroups), instead of a field

    const wchar_t* IniSection() const
    {
        if (group >= 0)
            return L"CombatLog";
        return npBool || npFloat ? L"Nameplates" : L"FloatingText";
    }
    double Current() const
    {
        const Config& c = Cfg();
        if (group >= 0)
            return c.fct.showGroup[group] ? 1.0 : 0.0;
        if (boolField)
            return c.fct.*boolField ? 1.0 : 0.0;
        if (floatField)
            return c.fct.*floatField;
        if (npBool)
            return c.np.*npBool ? 1.0 : 0.0;
        return c.np.*npFloat;
    }
};

#define B(field) true, &FctConfig::field, nullptr, nullptr, nullptr, 0, 1, nullptr, 1
#define F(field, lo, hi, fmt, scale) false, nullptr, &FctConfig::field, nullptr, nullptr, lo, hi, fmt, scale
#define NB(field) true, nullptr, nullptr, &NameplateConfig::field, nullptr, 0, 1, nullptr, 1
#define NF(field, lo, hi, fmt, scale) false, nullptr, nullptr, nullptr, &NameplateConfig::field, lo, hi, fmt, scale
#define G true, nullptr, nullptr, nullptr, nullptr, 0, 1, nullptr, 1

const Item kItems[] = {
    {"CLR_FctEnable", L"Enable", "Improved floating combat text (off = the game's original)", B(enable), "Floating combat text"},
    {"CLR_FctSizePx", L"SizePx", "Text size:", F(sizePx, 10, 60, "%.0f px", 1), nullptr},
    {"CLR_FctLifetime", L"LifetimeSec", "Time on screen:", F(lifetimeSec, 0.5, 4, "%.1f s", 1), nullptr},
    {"CLR_FctRisePx", L"RisePx", "Upward drift:", F(risePx, 0, 150, "%.0f px", 1), nullptr},
    {"CLR_FctFadeStart", L"FadeStart", "Start fading at:", F(fadeStart, 0.1, 0.95, "%.0f%% of the time", 100), nullptr},
    {"CLR_FctSpreadPx", L"SpreadPx", "Sideways spacing between hits on one target:", F(spreadPx, 0, 80, "%.0f px", 1), nullptr},
    {"CLR_FctDriftPx", L"DriftPx", "Sideways drift:", F(driftPx, 0, 60, "%.0f px", 1), nullptr},
    {"CLR_FctAnchorTop", L"AnchorTop", "Never start higher than (from the top of the screen):", F(anchorTop, 0.05, 0.9, "%.0f%%", 100), nullptr},
    {"CLR_FctOnTop", L"OnTop", "Draw numbers on top of everything", B(onTop), nullptr},

    {"CLR_FctCritScale", L"CritScale", "Critical hit size:", F(critScale, 1, 2.5, "%.2fx", 1), "Critical and glancing hits"},
    {"CLR_FctCritShake", L"CritShakePx", "Critical hit shake:", F(critShakePx, 0, 12, "%.0f px", 1), nullptr},
    {"CLR_FctGlanceScale", L"GlanceScale", "Glancing hit size:", F(glanceScale, 0.5, 1, "%.2fx", 1), nullptr},
    {"CLR_FctActionScale", L"ActionScale", "Special attack / perk name size:", F(actionScale, 0.4, 1, "%.2fx", 1), nullptr},

    {"CLR_LogYouHitOther", L"ShowYouHitOther", "You hit other", G, "Combat log categories", 0x07},
    {"CLR_LogYouHitOtherWithNano", L"ShowYouHitOtherWithNano", "You hit other with nano", G, nullptr, 0x04},
    {"CLR_LogMeHitByMonster", L"ShowMeHitByMonster", "Me hit by monster", G, nullptr, 0x05},
    {"CLR_LogMeHitByPlayer", L"ShowMeHitByPlayer", "Me hit by player", G, nullptr, 0x06},
    {"CLR_LogMeHitByNano", L"ShowMeHitByNano", "Me hit by nano", G, nullptr, 0x01},
    {"CLR_LogMeHitByEnvironment", L"ShowMeHitByEnvironment", "Me hit by environment", G, nullptr, 0x00},
    {"CLR_LogYourPetHitByOther", L"ShowYourPetHitByOther", "Your pet hit by other", G, nullptr, 0x08},
    {"CLR_LogYourPetHitByMonster", L"ShowYourPetHitByMonster", "Your pet hit by monster", G, nullptr, 0x10},
    {"CLR_LogYourPetHitByNano", L"ShowYourPetHitByNano", "Your pet hit by nano", G, nullptr, 0x02},
    {"CLR_LogOtherHitByOther", L"ShowOtherHitByOther", "Other hit by other", G, nullptr, 0x09},
    {"CLR_LogYourMisses", L"ShowYourMisses", "Your misses", G, nullptr, 0x11},
    {"CLR_LogOtherMisses", L"ShowOtherMisses", "Other misses", G, nullptr, 0x12},
    {"CLR_LogYouGaveHealth", L"ShowYouGaveHealth", "You gave health", G, nullptr, 0x13},
    {"CLR_LogMeGotHealth", L"ShowMeGotHealth", "Me got health", G, nullptr, 0x14},
    {"CLR_LogYouGaveNano", L"ShowYouGaveNano", "You gave nano", G, nullptr, 0x16},
    {"CLR_LogMeGotNano", L"ShowMeGotNano", "Me got nano", G, nullptr, 0x15},
    {"CLR_LogMeGotXP", L"ShowMeGotXP", "Me got XP", G, nullptr, 0x0A},
    {"CLR_LogMeGotSK", L"ShowMeGotSK", "Me got SK", G, nullptr, 0x0B},
    {"CLR_LogMeCastNano", L"ShowMeCastNano", "Me cast nano", G, nullptr, 0x17},
    {"CLR_FctOthersSpecials", L"OthersSpecials", "Show other players' special attacks and perks", B(othersSpecials), "Other"},
    {"CLR_FctSelfFirstPerson", L"SelfFirstPerson", "Show damage you take in first person", B(selfFirstPerson), nullptr},
    {"CLR_FctDrainHeals", L"DrainHeals", "Show health drains as heals on you", B(drainHeals), nullptr},
    {"CLR_FctSelfSigns", L"SelfSigns", "Show - before damage you take and + before gains", B(selfSigns), nullptr},
    {"CLR_FctFollowTarget", L"FollowTarget", "Numbers follow their target as it moves", B(followTarget), nullptr},

    {"CLR_NpEnable", L"Enable", "Modern nameplates (off = the game's original)", NB(enable), "Nameplates"},
    {"CLR_NpNaturalSize", L"NaturalSize", "Size in the world (name capitals):", NF(naturalSize, 0.05, 0.4, "%.2f m", 1), nullptr},
    {"CLR_NpMinPx", L"MinPx", "Smallest on screen:", NF(minPx, 6, 30, "%.0f px", 1), nullptr},
    {"CLR_NpMaxPx", L"MaxPx", "Largest on screen:", NF(maxPx, 8, 48, "%.0f px", 1), nullptr},
    {"CLR_NpOtherOpacity", L"OtherOpacity", "Opacity of plates other than your target:", NF(otherOpacity, 0.1, 1, "%.0f%%", 100), nullptr},
    {"CLR_NpBackgroundOpacity", L"BackgroundOpacity", "Background behind the plate:", NF(backgroundOpacity, 0, 1, "%.0f%%", 100), nullptr},
    {"CLR_NpTargetScale", L"TargetScale", "Size of your target's plate:", NF(targetScale, 1, 2, "%.0f%%", 100), nullptr},
    {"CLR_NpStack", L"Stack", "Don't let nameplates overlap (farther ones move up)", NB(stack), nullptr},
    {"CLR_NpClickToTarget", L"ClickToTarget", "Left-click a nameplate to select that character", NB(clickToTarget), nullptr},
    {"CLR_NpShowLevel", L"ShowLevel", "Show level", NB(showLevel), nullptr},
    {"CLR_NpShowOrg", L"ShowOrg", "Show title and organization", NB(showOrg), nullptr},
    {"CLR_NpShowHealth", L"ShowHealth", "Show health bar", NB(showHealth), nullptr},
    {"CLR_NpShowPlayers", L"ShowPlayers", "Nameplates for players", NB(showPlayers), nullptr},
    {"CLR_NpShowNpcs", L"ShowNpcs", "Nameplates for NPCs and monsters", NB(showNpcs), nullptr},
    {"CLR_NpShowPets", L"ShowPets", "Nameplates for pets", NB(showPets), nullptr},
    {"CLR_NpShowSelf", L"ShowSelf", "Nameplate for your own character", NB(showSelf), nullptr},
};
constexpr int kItemCount = sizeof(kItems) / sizeof(kItems[0]);

#undef B
#undef F
#undef NB
#undef NF
#undef G

// Latest value per item from the GUI, written to the INI by Flush() on the init thread.
std::atomic<bool> g_dirty[kItemCount];
std::atomic<double> g_value[kItemCount];

// --- Utils.dll (DistributedValue) -----------------------------------------------------------

using StringCtorFn = void*(__thiscall*)(void* self, const char* s);
using StringDtorFn = void(__thiscall*)(void* self);
using StringCStrFn = const char*(__thiscall*)(const void* self);
using VariantDoubleFn = void*(__thiscall*)(void* self, double v);
using VariantBoolFn = void*(__thiscall*)(void* self, bool v);
using VariantDtorFn = void(__thiscall*)(void* self);
using VariantGetTypeFn = int(__thiscall*)(const void* self);
using VariantAsDoubleFn = double(__thiscall*)(const void* self);
using VariantAsBoolFn = bool(__thiscall*)(const void* self);
using AddVariableNodeFn = void(__cdecl*)(const void* node, bool overwrite, bool keepDefault);
using DValueCtorFn = void*(__thiscall*)(void* self, const void* name);
using ObserveFn = void(__thiscall*)(void* self, const void* name);
using CrtNewFn = void*(__cdecl*)(size_t);

StringCtorFn StringCtor;
StringDtorFn StringDtor;
StringCStrFn StringCStr;
VariantDoubleFn VariantDouble;
VariantBoolFn VariantBool;
VariantDtorFn VariantDtor;
VariantGetTypeFn VariantGetType;
VariantAsDoubleFn VariantAsDouble;
VariantAsBoolFn VariantAsBool;
AddVariableNodeFn AddVariableNode;
DValueCtorFn DValueCtor;
ObserveFn Observe;
CrtNewFn CrtNew;

// ValueNode_c (0x70 bytes): String name +0, Variant value +0x20, persist +0x30, hasMin +0x31,
// hasMax +0x32, Variant min +0x38, max +0x48, category +0x58, observer vector +0x5C.
constexpr size_t kValueNodeSize = 0x70;
constexpr size_t kStringSize = 0x20;
constexpr size_t kDistributedValueSize = 0x50;
constexpr int kVariantBool = 6;

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[settings] missing export %s!%s", module, name);
    return fn != nullptr;
}

bool ResolveAll()
{
    bool ok = true;
    ok &= Resolve(StringCtor, "Utils.dll", "??0String@@QAE@PBD@Z");
    ok &= Resolve(StringDtor, "Utils.dll", "??1String@@QAE@XZ");
    ok &= Resolve(StringCStr, "Utils.dll", "?c_str@String@@QBEPBDXZ");
    ok &= Resolve(VariantDouble, "Utils.dll", "??0Variant@@QAE@N@Z");
    ok &= Resolve(VariantBool, "Utils.dll", "??0Variant@@QAE@_N@Z");
    ok &= Resolve(VariantDtor, "Utils.dll", "??1Variant@@QAE@XZ");
    ok &= Resolve(VariantGetType, "Utils.dll", "?GetType@Variant@@QBE?AW4type_code@@XZ");
    ok &= Resolve(VariantAsDouble, "Utils.dll", "?AsDouble@Variant@@QBENXZ");
    ok &= Resolve(VariantAsBool, "Utils.dll", "?AsBool@Variant@@QBE_NXZ");
    ok &= Resolve(AddVariableNode, "Utils.dll", "?AddVariable@DistributedValue_c@@SAXABVValueNode_c@1@_N1@Z");
    ok &= Resolve(DValueCtor, "Utils.dll", "??0DistributedValue_c@@QAE@ABVString@@@Z");
    ok &= Resolve(Observe, "Utils.dll", "?Observe@DistributedValue_c@@QAEXABVString@@@Z");
    ok &= Resolve(CrtNew, "msvcr100.dll", "??2@YAPAXI@Z");
    return ok;
}

void Register(const Item& item, double value)
{
    alignas(8) uint8_t node[kValueNodeSize] = {};
    StringCtor(node, item.var);
    if (item.isBool) {
        VariantBool(node + 0x20, value != 0.0);
    } else {
        VariantDouble(node + 0x20, value);
        VariantDouble(node + 0x38, item.min);
        VariantDouble(node + 0x48, item.max);
        node[0x31] = 1; // hasMin
        node[0x32] = 1; // hasMax
    }
    node[0x30] = 0; // persist: Clarity.ini is the store
    AddVariableNode(node, true, true);
    if (!item.isBool) {
        VariantDtor(node + 0x48);
        VariantDtor(node + 0x38);
    }
    VariantDtor(node + 0x20);
    StringDtor(node);
}

// --- change observer --------------------------------------------------------------------------

// DistributedValue_c vtable slot 1: DValueChanged(String const& name, DValueCategory_e, Variant const&),
// called by SetDValue (with the DValue lock held) whenever an observed value changes.
void __fastcall OnDValueChanged(void* /*self*/, void* /*edx*/, const void* name, int /*category*/, const void* value)
{
    const char* n = StringCStr(name);
    if (!n || strncmp(n, "CLR_", 4) != 0)
        return;
    for (int i = 0; i < kItemCount; ++i) {
        if (strcmp(n, kItems[i].var) == 0) {
            const double v = VariantGetType(value) == kVariantBool ? (VariantAsBool(value) ? 1.0 : 0.0) : VariantAsDouble(value);
            g_value[i].store(v);
            g_dirty[i].store(true);
            return;
        }
    }
}

void* g_observerVtable[8];

bool CreateObserver()
{
    void* obj = CrtNew(kDistributedValueSize);
    if (!obj)
        return false;
    memset(obj, 0, kDistributedValueSize);
    alignas(8) uint8_t empty[kStringSize] = {};
    StringCtor(empty, "");
    DValueCtor(obj, empty);
    StringDtor(empty);

    // Same object, our DValueChanged: copy its vtable and replace slot 1.
    void** vtable = *static_cast<void***>(obj);
    for (int i = 0; i < 8; ++i)
        g_observerVtable[i] = vtable[i];
    g_observerVtable[1] = reinterpret_cast<void*>(&OnDValueChanged);
    *static_cast<void***>(obj) = g_observerVtable;

    for (const Item& item : kItems) {
        alignas(8) uint8_t name[kStringSize] = {};
        StringCtor(name, item.var);
        Observe(obj, name);
        StringDtor(name);
    }
    return true;
}

// --- the options tab XML ------------------------------------------------------------------

// One page of the options tab: the window turns "Clarity/<page>" labels into a Clarity folder
// with a page per label (OptionWindow loader splits labels on '/').
void AppendPage(std::string& x, const char* page, const char* scrollName, bool nameplates)
{
    x += "\r\n  <ScrollView h_alignment=\"LEFT\" label=\"Clarity/";
    x += page;
    x += "\" v_scrollbar_mode=\"auto\" scroll_client=\"";
    x += scrollName;
    x += "\" max_size=\"Point(16000,-1)\">\r\n    <ScrollViewChild name=\"";
    x += scrollName;
    x += "\">\r\n"
         "      <View h_alignment=\"LEFT\" view_layout=\"vertical\" max_size=\"Point(16000,-1)\" layout_borders=\"Rect(0,0,10,0)\">\r\n"
         "        <TextView value=\"Clarity: ";
    x += page;
    x += "\" font=\"HUGE\" layout_borders=\"Rect(0,0,0,5)\" />\r\n"
         "        <TextView value=\"Changes apply at once. Settings are saved to Clarity.ini (fonts and colours are set there).\" font=\"LARGE\" layout_borders=\"Rect(0,0,0,10)\" />\r\n";
    for (const Item& item : kItems) {
        if ((item.npBool || item.npFloat) != nameplates)
            continue;
        if (item.section && !nameplates) {
            x += "        <TextView value=\"";
            x += item.section;
            x += "\" font=\"LARGE\" layout_borders=\"Rect(0,10,0,3)\" />\r\n";
        }
        if (item.isBool) {
            x += "        <OptionCheckBox label=\"";
            x += item.label;
            x += "\" layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"";
            x += item.var;
            x += "\"/>\r\n";
        } else {
            char scale[32];
            sprintf_s(scale, "%g", item.scale);
            x += "        <OptionSlider label=\"";
            x += item.label;
            x += "\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"";
            x += item.var;
            x += "\" value_fmt=\"&lt;font color=#70C4D0&gt;";
            x += item.fmt;
            x += "&lt;/font&gt;\" value_scale=\"";
            x += scale;
            x += "\"/>\r\n";
        }
    }
    x += "        <VLayoutSpacer/>\r\n"
         "      </View>\r\n"
         "    </ScrollViewChild>\r\n"
         "  </ScrollView>\r\n";
}

std::string TabXml()
{
    std::string x;
    AppendPage(x, "Floating Combat Text", "clarity_fct_scroll", false);
    AppendPage(x, "Nameplates", "clarity_np_scroll", true);
    return x;
}

// --- Root.xml overlay (msvcr100 CreateFileA/W import slots) -------------------------------

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

CreateFileAFn g_nextA = nullptr; // what the slot held before us (AOReloaded's hook or kernel32)
CreateFileWFn g_nextW = nullptr;
CreateFileWFn g_realCreateFileW = nullptr; // for our own temp file, bypassing every hook
thread_local bool t_inHook = false;
std::atomic<unsigned> g_tempCounter{0};

// The options window's root page: OptionPanel\Root.xml, or another root*.xml there - MerkUI with
// ao-vk points the game at its own OptionPanel\RootRenderer.xml (its Root.xml plus Renderer pages).
// Overlay only adds the tab to a complete page (one with </root>) that doesn't have it yet.
bool IsRootXml(std::string path)
{
    for (char& c : path)
        c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    static const char kFolder[] = "\\optionpanel\\";
    const size_t slash = path.rfind('\\');
    if (slash == std::string::npos || slash + 1 < sizeof(kFolder) - 1)
        return false;
    const std::string folder = path.substr(slash + 1 - (sizeof(kFolder) - 1), sizeof(kFolder) - 1);
    const std::string file = path.substr(slash + 1);
    return folder == kFolder && file.size() >= 8 && file.compare(0, 4, "root") == 0 &&
           file.compare(file.size() - 4, 4, ".xml") == 0;
}

// Reads what the previous layer opened, appends our tab, and returns a temp-file handle with
// the result. On any problem the original handle is returned untouched.
HANDLE Overlay(HANDLE h)
{
    if (h == INVALID_HANDLE_VALUE || !g_realCreateFileW)
        return h;
    const DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 8u * 1024 * 1024)
        return h;
    std::string content(size, '\0');
    DWORD read = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(h, content.data(), size, &read, nullptr) || read != size) {
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        return h;
    }
    const size_t root = content.find("</root>");
    if (root == std::string::npos || content.find("label=\"Clarity/") != std::string::npos) {
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        return h;
    }
    content.insert(root, TabXml());

    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf_s(path, L"%sClarity-%lu-%u.tmp", dir, GetCurrentProcessId(), g_tempCounter.fetch_add(1));
    HANDLE out = g_realCreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        return h;
    }
    DWORD written = 0;
    if (!WriteFile(out, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
        written != content.size()) {
        CloseHandle(out);
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        return h;
    }
    SetFilePointer(out, 0, nullptr, FILE_BEGIN);
    CloseHandle(h);
    Log("[settings] options tab added to Root.xml (%lu -> %u bytes)", size, static_cast<unsigned>(content.size()));
    return out;
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                               DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_nextA(name, access, share, sa, disp, flags, tmpl);
    if (t_inHook || !name || !IsRootXml(name))
        return h;
    t_inHook = true;
    h = Overlay(h);
    t_inHook = false;
    return h;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                               DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_nextW(name, access, share, sa, disp, flags, tmpl);
    if (t_inHook || !name)
        return h;
    char narrow[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr) || !IsRootXml(narrow))
        return h;
    t_inHook = true;
    h = Overlay(h);
    t_inHook = false;
    return h;
}

// Patches CreateFileA/W in one module's import table, remembering the previous slot values.
int PatchImports(HMODULE module)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    int patched = 0;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const char* fn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            void* hook = nullptr;
            if (strcmp(fn, "CreateFileA") == 0 && !g_nextA) {
                g_nextA = reinterpret_cast<CreateFileAFn>(slots->u1.Function);
                hook = reinterpret_cast<void*>(&Hook_CreateFileA);
            } else if (strcmp(fn, "CreateFileW") == 0 && !g_nextW) {
                g_nextW = reinterpret_cast<CreateFileWFn>(slots->u1.Function);
                hook = reinterpret_cast<void*>(&Hook_CreateFileW);
            }
            if (!hook)
                continue;
            // msvcr100's import table shares a page with its code: keep that page executable.
            if (WritePointer(&slots->u1.Function, hook))
                ++patched;
        }
    }
    return patched;
}

} // namespace

bool Install()
{
    if (!ResolveAll())
        return false;

    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    g_realCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(kb ? kb : GetModuleHandleA("kernel32.dll"), "CreateFileW"));

    for (const Item& item : kItems)
        Register(item, item.Current());
    if (!CreateObserver()) {
        Log("[settings] observer creation failed - options tab disabled");
        return false;
    }

    // The options window loads OptionPanel/Root.xml with fopen, i.e. through msvcr100's
    // CreateFileA. AOReloaded patched that slot earlier; we take its value as "next".
    HMODULE crt = GetModuleHandleA("msvcr100.dll");
    const int patched = crt ? PatchImports(crt) : 0;
    Log("[settings] %d DValues registered, msvcr100 CreateFile slots patched: %d (next A=%p W=%p)", kItemCount,
        patched, g_nextA, g_nextW);
    return patched > 0;
}

void Flush()
{
    bool any = false;
    for (int i = 0; i < kItemCount; ++i) {
        if (!g_dirty[i].exchange(false))
            continue;
        const Item& item = kItems[i];
        const double v = g_value[i].load();
        wchar_t text[32];
        if (item.isBool)
            swprintf_s(text, L"%d", v != 0.0 ? 1 : 0);
        else
            swprintf_s(text, L"%.3g", v);
        WritePrivateProfileStringW(item.IniSection(), item.key, text, ConfigPath().c_str());
        any = true;
    }
    if (any)
        Log("[settings] saved changes to Clarity.ini");
}

} // namespace prk::settings
