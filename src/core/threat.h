#pragma once

#include "game.h"

#include <windows.h>

// Characters attacking us, shared by floating text (which sees the attacks) and nameplates (which
// show them). Game thread only.
namespace prk::threat {

// How long a character counts as attacking us after its last attack (hit, miss or the game's
// "Attacking You!").
constexpr DWORD kMemoryMs = 8000;

void NoteAttack(const game::Identity& attacker);

// Whether it attacked us within kMemoryMs; *since = when this run of attacks began.
bool IsAttackingMe(const game::Identity& id, DWORD* since);

} // namespace prk::threat
