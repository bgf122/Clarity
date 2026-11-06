#pragma once

// A round minimap ([Minimap]) in place of the game's local map: a Clarity GUI window whose view draws
// itself with the game's renderer (render_t::RenderTriangleFan, as the game's local map does), so it
// works with any renderer. The local map and the planet map, a compass, and the mission waypoint.
namespace prk::minimap {

bool Install();

} // namespace prk::minimap
