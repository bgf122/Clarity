// Leaving out scenery effects ([Effects] HideShipSmoke, Hide).
//
// The alien ship over the ICC Shuttleport (playfield 4582): ai_alien_lander.abiff and five
// ai_effect_ship_smoke.abiff markers carry the effect 12530, a _GfxControlGlobalSmoke_t - 64 large,
// bright, alpha-blended smoke sprites each, the glowing columns under the ship. Many layers of them
// cover much of the screen there, which is what makes the GPU work hard.
//
// Scenery effects are made by _EffectHandler_t::CreateEffect2(int id, RConnector&) (Gamecode export)
// from _EffectHandler_t::RunFunction (every frame, game thread): the effects of FXS-spawned meshes
// (DisplaySystem's attractor stack; RunFunction keeps their handles and DeleteEffect()s them when the
// mesh goes) and the playfield's start list (statics placed in the zone, as at the Shuttleport).
//
// Switching live: an effect we may hide is still created, and we keep a reference to its connector.
// Hidden: we delete the effect; shown again: we create it anew on the same connector. When the game
// deletes the original handle, or the playfield changes, our current one goes too. Handles are looked
// up in a map, so deleting one that is already gone does nothing.
#include "effects.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/hooks.h"
#include "../../core/log.h"

#include <windows.h>

#include <algorithm>
#include <set>
#include <vector>

namespace prk::effects {

namespace {

constexpr int kShipSmoke = 12530;

using CreateFn = unsigned(__thiscall*)(void* self, int id, void* connector);
using DeleteFn = void(__thiscall*)(void* self, unsigned handle);
using RunFn = void(__thiscall*)(void* self);
using RefFn = void(__thiscall*)(const void* resource);
CreateFn g_origCreate = nullptr;
DeleteFn g_origDelete = nullptr;
RunFn g_origRun = nullptr;
RefFn g_addRef = nullptr;  // RResource_t::AddRefRResource
RefFn g_release = nullptr; // RResource_t::ReleaseRResource

struct Tracked {
    int id;
    unsigned original;     // the handle the game holds
    unsigned current;      // what is running now (0 = hidden)
    void* connector;       // a reference we hold
    const void* playfield; // the zone it belongs to
};
std::vector<Tracked> g_tracked; // game thread only
std::set<int> g_seen;           // ids already logged (Debug)
bool g_inCreate = false;        // our own CreateEffect2 calls

bool Listed(int id)
{
    const EffectsConfig& cfg = Cfg().effects;
    return std::find(cfg.hide, cfg.hide + cfg.hideCount, id) != cfg.hide + cfg.hideCount;
}

bool Switchable(int id)
{
    return id == kShipSmoke || Listed(id);
}

bool Hidden(int id)
{
    return (id == kShipSmoke && Cfg().effects.hideShipSmoke) || Listed(id);
}

unsigned __fastcall Hook_Create(void* self, void* /*edx*/, int id, void* connector)
{
    if (g_inCreate)
        return g_origCreate(self, id, connector);
    if (Cfg().effects.debug && g_seen.insert(id).second)
        Log("[effects] scenery effect %d%s", id, Hidden(id) ? " (hidden)" : "");
    const unsigned handle = g_origCreate(self, id, connector);
    if (!handle || !connector || !Switchable(id))
        return handle;
    g_addRef(connector);
    Tracked t{id, handle, handle, connector, game::CurrentPlayfield()};
    if (Hidden(id)) {
        g_origDelete(self, handle);
        t.current = 0;
    }
    g_tracked.push_back(t);
    return handle;
}

void Drop(void* self, std::vector<Tracked>::iterator it)
{
    if (it->current && it->current != it->original)
        g_origDelete(self, it->current); // ours
    g_release(it->connector);
    g_tracked.erase(it);
}

void __fastcall Hook_Delete(void* self, void* /*edx*/, unsigned handle)
{
    for (auto it = g_tracked.begin(); it != g_tracked.end(); ++it) {
        if (it->original == handle) {
            Drop(self, it);
            break;
        }
    }
    g_origDelete(self, handle);
}

// Each frame, before the game's: hide or show the tracked effects as the settings say.
void __fastcall Hook_Run(void* self, void* /*edx*/)
{
    const void* playfield = game::CurrentPlayfield();
    int hidden = 0, shown = 0;
    for (size_t i = 0; i < g_tracked.size();) {
        if (g_tracked[i].playfield != playfield) { // left the zone
            Drop(self, g_tracked.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        Tracked& t = g_tracked[i++];
        const bool hide = Hidden(t.id);
        if (hide && t.current) {
            g_origDelete(self, t.current);
            t.current = 0;
            ++hidden;
        } else if (!hide && !t.current) {
            g_inCreate = true;
            t.current = g_origCreate(self, t.id, t.connector);
            g_inCreate = false;
            if (!t.current)
                t.current = ~0u; // couldn't be made: don't try every frame
            ++shown;
        }
    }
    if (hidden || shown)
        Log("[effects] %d hidden, %d shown", hidden, shown);
    g_origRun(self);
}

template <typename T>
bool Resolve(T& fn, HMODULE module, const char* name)
{
    fn = module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
    if (!fn)
        Log("[effects] missing export %s - effects not filtered", name);
    return fn != nullptr;
}

} // namespace

bool Install()
{
    HMODULE gamecode = GetModuleHandleA("Gamecode.dll");
    HMODULE randy = GetModuleHandleA("randy31.dll");
    void* create = nullptr;
    void* del = nullptr;
    void* run = nullptr;
    if (!Resolve(create, gamecode, "?CreateEffect2@_EffectHandler_t@@QAEIHAAVRConnector@@@Z") ||
        !Resolve(del, gamecode, "?DeleteEffect@_EffectHandler_t@@QAEXI@Z") ||
        !Resolve(run, gamecode, "?RunFunction@_EffectHandler_t@@UAEXXZ") ||
        !Resolve(g_addRef, randy, "?AddRefRResource@RResource_t@@QBEXXZ") ||
        !Resolve(g_release, randy, "?ReleaseRResource@RResource_t@@QBEXXZ"))
        return false;
    if (!InstallHook("_EffectHandler_t::DeleteEffect", del, &Hook_Delete, &g_origDelete) ||
        !InstallHook("_EffectHandler_t::CreateEffect2", create, &Hook_Create, &g_origCreate) ||
        !InstallHook("_EffectHandler_t::RunFunction", run, &Hook_Run, &g_origRun))
        return false;
    Log("[effects] ready");
    return true;
}

} // namespace prk::effects
