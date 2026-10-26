#include "n3.h"

#include "log.h"

#include <windows.h>

namespace prk::n3 {

namespace {

// std::string as laid out by the game's MSVC 2010 runtime (release): 16-byte small buffer or
// heap pointer, then size and capacity. Heap memory belongs to msvcr100.
struct GameString {
    union {
        char buf[16];
        char* ptr;
    } bx;
    uint32_t size;
    uint32_t res;

    const char* c_str() const { return res >= 16 ? bx.ptr : bx.buf; }
};
static_assert(sizeof(GameString) == 24, "VS2010 std::string layout");

using InstanceFn = void*(__cdecl*)();
using GetSkillFn = int(__thiscall*)(void* self, const game::Identity& id, int stat, int mode, const game::Identity& other);
using GetNameFn = const char*(__thiscall*)(void* self, const game::Identity& id, const game::Identity& other);
using GetTitleFn = GameString*(__thiscall*)(void* self, GameString* ret, const game::Identity& id);
using GetClanFn = const GameString*(__thiscall*)(void* self, const game::Identity& id);
using IsNpcFn = bool(__thiscall*)(void* self, const game::Identity& id);
using CrtDeleteFn = void(__cdecl*)(void*);
using ConsiderFn = int(__thiscall*)(void* self, const game::Identity& id, float& fraction);
using FontSystemFn = void*(__cdecl*)();
using GetColorFn = int(__thiscall*)(void* self, int code);
using GetPosFn = bool(__thiscall*)(void* self, const game::Identity& id, game::Vec3& out);
using GetAttackingFn = bool(__thiscall*)(void* self, game::Identity& out);
using CurrentTargetFn = game::Identity*(__thiscall*)(void* self, game::Identity* ret);
using TargetTargetFn = game::Identity*(__thiscall*)(void* self, game::Identity* ret, game::Identity& target);
TargetTargetFn g_targetTarget;
GetPosFn g_getPos;
GetAttackingFn g_getAttacking;
InstanceFn g_inputConfig;
CurrentTargetFn g_currentTarget;
ConsiderFn g_consider;
FontSystemFn g_fontSystem;
GetColorFn g_getColor;

InstanceFn g_instance;
GetSkillFn g_getSkill;
GetNameFn g_getName;
GetTitleFn g_getTitle;
GetClanFn g_getClan;
IsNpcFn g_isNpc;
CrtDeleteFn g_crtDelete;

template <typename T>
bool Resolve(T& fn, const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
    if (!fn)
        Log("[n3] missing export %s!%s", module, name);
    return fn != nullptr;
}

void* Module()
{
    return g_instance ? g_instance() : nullptr;
}

} // namespace

bool Init()
{
    bool ok = true;
    ok &= Resolve(g_instance, "Interfaces.dll", "?GetInstance@N3InterfaceModule_t@@SAPAV1@XZ");
    ok &= Resolve(g_getSkill, "Interfaces.dll", "?N3Msg_GetSkill@N3InterfaceModule_t@@QBEHABVIdentity_t@@W4Stat_e@GameData@@H0@Z");
    ok &= Resolve(g_getName, "Interfaces.dll", "?N3Msg_GetName@N3InterfaceModule_t@@QBEPBDABVIdentity_t@@0@Z");
    ok &= Resolve(g_getTitle, "Interfaces.dll",
                  "?N3Msg_GetTitleName@N3InterfaceModule_t@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@ABVIdentity_t@@@Z");
    ok &= Resolve(g_getClan, "Interfaces.dll",
                  "?N3Msg_GetClanString@N3InterfaceModule_t@@QBEPBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@ABVIdentity_t@@@Z");
    ok &= Resolve(g_isNpc, "Interfaces.dll", "?N3Msg_IsNpc@N3InterfaceModule_t@@QBE_NABVIdentity_t@@@Z");
    ok &= Resolve(g_crtDelete, "msvcr100.dll", "??3@YAXPAX@Z");
    ok &= Resolve(g_getPos, "Interfaces.dll", "?N3Msg_GetPos@N3InterfaceModule_t@@QBE_NABVIdentity_t@@AAVVector3_t@@@Z");
    ok &= Resolve(g_consider,"Interfaces.dll", "?N3Msg_Consider@N3InterfaceModule_t@@QBE?AW4Consider_e@@ABVIdentity_t@@AAM@Z");
    ok &= Resolve(g_fontSystem, "GUI.dll", "?GetInstance@FontSystem_t@@SAPAV1@XZ");
    ok &= Resolve(g_getColor, "GUI.dll", "?GetColor@FontSystem_t@@QBEHW4ColorCode_e@@@Z");
    // Targets (the bars): optional, FightTarget / SelectedTarget report none without them.
    Resolve(g_getAttacking, "Interfaces.dll", "?N3Msg_GetAttackingID@N3InterfaceModule_t@@QBE_NAAVIdentity_t@@@Z");
    Resolve(g_inputConfig, "GUI.dll", "?GetInstance@InputConfig_t@@SAPAV1@XZ");
    Resolve(g_currentTarget, "GUI.dll", "?GetCurrentTarget@InputConfig_t@@QBE?AVIdentity_t@@XZ");
    Resolve(g_targetTarget, "Interfaces.dll", "?N3Msg_GetTargetTarget@N3InterfaceModule_t@@QAE?AVIdentity_t@@AAV2@@Z");
    return ok;
}

int GetSkill(const game::Identity& id, int stat)
{
    void* m = Module();
    const game::Identity none{0, 0};
    return m ? g_getSkill(m, id, stat, 2, none) : kNoValue;
}

std::string Name(const game::Identity& id)
{
    void* m = Module();
    const game::Identity none{0, 0};
    const char* s = m ? g_getName(m, id, none) : nullptr;
    return s ? s : "";
}

std::string Title(const game::Identity& id)
{
    void* m = Module();
    if (!m)
        return {};
    GameString ret{};
    g_getTitle(m, &ret, id);
    std::string out(ret.c_str(), ret.size);
    if (ret.res >= 16 && ret.bx.ptr)
        g_crtDelete(ret.bx.ptr); // allocated by the game's runtime
    return out;
}

std::string Org(const game::Identity& id)
{
    void* m = Module();
    const GameString* s = m ? g_getClan(m, id) : nullptr;
    return s ? std::string(s->c_str(), s->size) : std::string();
}

bool IsNpc(const game::Identity& id)
{
    void* m = Module();
    return m && g_isNpc(m, id);
}

bool Position(const game::Identity& id, game::Vec3* out)
{
    void* m = Module();
    return m && g_getPos(m, id, *out);
}

int Consider(const game::Identity& id, float* fraction)
{
    void* m = Module();
    float f = 0.0f;
    const int c = m ? g_consider(m, id, f) : 0;
    *fraction = f;
    return c;
}

bool FightTarget(game::Identity* out)
{
    void* m = Module();
    game::Identity id{0, 0};
    if (!m || !g_getAttacking || !g_getAttacking(m, id) || (id.type == 0 && id.instance == 0))
        return false;
    *out = id;
    return true;
}

bool SelectedTarget(game::Identity* out)
{
    void* ic = g_inputConfig ? g_inputConfig() : nullptr;
    if (!ic || !g_currentTarget)
        return false;
    game::Identity id{0, 0};
    g_currentTarget(ic, &id);
    if (id.type == 0 && id.instance == 0)
        return false;
    *out = id;
    return true;
}

bool TargetOf(const game::Identity& target, game::Identity* out)
{
    void* m = Module();
    if (!m || !g_targetTarget)
        return false;
    game::Identity in = target, id{0, 0};
    g_targetTarget(m, &id, in);
    if (id.type == 0 && id.instance == 0)
        return false;
    *out = id;
    return true;
}

uint32_t ColorOf(int colorCode)
{
    void* fs = g_fontSystem ? g_fontSystem() : nullptr;
    return fs ? static_cast<uint32_t>(g_getColor(fs, colorCode)) & 0xFFFFFF : 0xFFFFFF;
}

} // namespace prk::n3
