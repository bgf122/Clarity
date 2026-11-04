#pragma once

// Scenery effects the game would show that Clarity leaves out ([Effects] Hide). Gamecode's effect
// definitions (Setupf/gfxtweak.bin, by id) are created through _EffectHandler_t::CreateEffect2(id,
// RConnector&): the effects meshes carry ("eff_universal_<id>" sub-meshes, queued by DisplaySystem and
// popped in _EffectHandler_t::RunFunction) and the playfield's start list. A hidden id is never created.
namespace prk::effects {

bool Install();

} // namespace prk::effects
