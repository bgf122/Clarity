#pragma once

// The playfield map in alien mission zones ([Effects] AlienMap). The map draws a dungeon's rooms with
// their floor textures; the alien zones' floors are small dark 32x32 textures, so their rooms are black
// on the black map. Those tiles are drawn in a solid green instead, as ao-vk's RVK_AlienMap does.
namespace prk::pfmap {

bool Install();

// The texture a dungeon map room tile is drawn with: the green one in place of an alien zone's 32x32
// floor ([Effects] AlienMap, or every tile with AlienMapTest), else `texture`. Game thread only.
void* RoomTileTexture(void* texture);

} // namespace prk::pfmap
