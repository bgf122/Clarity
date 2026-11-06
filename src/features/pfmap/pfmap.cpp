// The playfield map's rooms in alien mission zones ([Effects] AlienMap, AlienMapTest).
//
// How the game draws a dungeon's map (GUI.dll): N3's n3GroundRenderer_t::CreateDungeonRoom sends, per
// room, the first floor texture of each tile (the playfield tilemap's texture table; an id with the top
// bit set is a general texture, RDB type 1010004, otherwise a map tile, 1010009). MapData_c (GUI
// 0x10042CC4) loads them, and PFMapGroundRenderer_c's room loop (0x10046E28) draws every tile with
// 0x10046871(viewport, RTexture_t*, uv rect, screen rect, ARGB) - a quad, texture x colour x 2
// (MODULATE2X; rooms shown but not yet coloured get 0xFF808080, i.e. the texture as it is).
//
// The alien zones' floors are 32x32 textures (normal map tiles are 64 px and up), dark, so the rooms
// can't be told from the black map. ao-vk's RVK_AlienMap draws exactly those (32x32) as solid
// (0.10, 0.32, 0.14). Clarity does the same in the GUI, so it works with any renderer: the room loop's
// call to the tile function is pointed at ours, which gives 32x32 tiles a small solid green texture of
// our own (4x4, so ao-vk's own rule never takes it), a little darker than ao-vk's. The game's colour
// still applies on top.
// Nothing else calls through that spot: the outdoor map and the map markers are drawn elsewhere.
#include "pfmap.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/signatures.h"

#include <windows.h>

#include <cstdint>
#include <set>
#include <utility>

namespace prk::pfmap {

namespace {

// __thiscall(view, viewport, texture, uv rect, screen rect, ARGB), ret 0x14.
using DrawTileFn = void(__thiscall*)(void* view, void* viewport, void* texture, const void* uv, const void* rect,
                                     uint32_t argb);
DrawTileFn g_drawTile = nullptr;

constexpr int kAlienTile = 32; // ao-vk's test: the alien zones' floor textures are 32x32
constexpr int kGreenSize = 4;
constexpr uint32_t kGreen = 0xFF143E1B; // 3/4 of ao-vk's (0.10, 0.32, 0.14)

// Game thread only.
void* g_green = nullptr;
bool g_greenFailed = false;
std::set<std::pair<int, int>> g_seen; // tile sizes already logged (Debug)

void* Green()
{
    if (!g_green && !g_greenFailed) {
        uint32_t pixels[kGreenSize * kGreenSize];
        for (uint32_t& p : pixels)
            p = kGreen;
        g_green = game::CreateTextureARGB("clarity_pfmap_green", kGreenSize, kGreenSize);
        if (!g_green || !game::WriteTexture(g_green, pixels, kGreenSize, kGreenSize)) {
            g_green = nullptr; // (never freed: the game's CRT owns it)
            g_greenFailed = true;
            Log("[pfmap] couldn't make the green texture - alien maps unchanged");
        }
    }
    return g_green;
}

void __fastcall Hook_DrawTile(void* view, void* /*edx*/, void* viewport, void* texture, const void* uv,
                              const void* rect, uint32_t argb)
{
    g_drawTile(view, viewport, RoomTileTexture(texture), uv, rect, argb);
}

} // namespace

void* RoomTileTexture(void* texture)
{
    const EffectsConfig& cfg = Cfg().effects;
    int w = 0, h = 0;
    if (texture && (cfg.alienMap || cfg.alienMapTest || cfg.debug) && game::TextureSize(texture, &w, &h)) {
        const bool alien = w == kAlienTile && h == kAlienTile;
        if (cfg.debug && g_seen.insert({w, h}).second)
            Log("[pfmap] map room tiles of %dx%d%s", w, h, alien ? " (alien)" : "");
        if (cfg.alienMapTest || (cfg.alienMap && alien))
            if (void* green = Green())
                return green;
    }
    return texture;
}

bool Install()
{
    uint8_t* site = FindUnique(SIG_PFMAP_ROOM_TILE, "map room-tile call");
    if (!site)
        return false;
    uint8_t* call = site + 8;
    if (call[0] != 0xE8) {
        Log("[pfmap] map room-tile call not where expected - alien maps unchanged");
        return false;
    }
    uint8_t* next = call + 5;
    g_drawTile = reinterpret_cast<DrawTileFn>(next + *reinterpret_cast<const int32_t*>(call + 1));
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&Hook_DrawTile) - next);
    if (!WritePointer(call + 1, reinterpret_cast<void*>(static_cast<intptr_t>(rel)))) {
        Log("[pfmap] couldn't patch the map room-tile call - alien maps unchanged");
        return false;
    }
    Log("[pfmap] ready (tile function %p)", reinterpret_cast<void*>(g_drawTile));
    return true;
}

} // namespace prk::pfmap
