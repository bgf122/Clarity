#pragma once

namespace prk::buffs {

// The buff frame ([Buffs]): your running nanos as icons with the time left, in place of the game's NCU
// window. Runs from the game window's procedure; hooks the NCU window's opener to show and hide it instead.
bool Install();

} // namespace prk::buffs
