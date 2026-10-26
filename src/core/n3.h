#pragma once

#include "game.h"

#include <cstdint>
#include <string>

// Queries through Interfaces.dll's N3InterfaceModule_t (the GUI's own route to game data).
namespace prk::n3 {

constexpr int kNoValue = 1234567890; // N3Msg_GetSkill's "no such stat" sentinel

// Stat ids (GameData::Stat_e) used by the GUI's nametag builder.
constexpr int kStatLife = 1;      // max health
constexpr int kStatHealth = 27;
constexpr int kStatNano = 214;      // current nano
constexpr int kStatMaxNano = 221;
constexpr int kStatSide = 33;
constexpr int kStatLevel = 54;
constexpr int kStatPetMaster = 196;
constexpr int kStatOwnerInstance = 433;
constexpr int kStatNpcFamily = 455;
constexpr int kStatPetType = 512;
constexpr int kStatPetState = 671;

bool Init();

int GetSkill(const game::Identity& id, int stat);
std::string Name(const game::Identity& id);
std::string Title(const game::Identity& id); // e.g. "Supreme Commander", may be empty
std::string Org(const game::Identity& id);   // N3Msg_GetClanString, may be empty
bool IsNpc(const game::Identity& id);

// N3Msg_GetPos: world position of a dynel.
bool Position(const game::Identity& id, game::Vec3* out);

// N3Msg_Consider: level comparison with the player. Returns the Consider_e value (0 = can't
// consider) and the 0..1 fraction the GUI turns into the health-bar colour.
int Consider(const game::Identity& id, float* fraction);

// The fight target: N3Msg_GetAttackingID, whoever the client character is attacking. False if none.
bool FightTarget(game::Identity* out);

// The selected (nano) target: GUI InputConfig_t::GetCurrentTarget, where TargetingModule_t::SetTarget
// puts every selection. False if none.
bool SelectedTarget(game::Identity* out);

// GUI FontSystem_t::GetColor(ColorCode_e): the RGB the text renderer uses for a colour code.
uint32_t ColorOf(int colorCode);

constexpr int kStatFlags = 0;            // bits 22/23: name colour index
constexpr int kStatHasAlwaysLootable = 345;

} // namespace prk::n3
