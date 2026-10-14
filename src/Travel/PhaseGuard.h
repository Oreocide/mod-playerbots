/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Playerbots_PhaseGuard_h__
#define Playerbots_PhaseGuard_h__

#include "Define.h"
#include <vector>

class Player;

namespace Playerbots
{
/// Bots live in the default phase: most players are there, and quest driven phases (Cata and later) hide the
/// world a bot expects. Spawns, hubs and quests that depend on another phase are not used by bots.
namespace PhaseGuard
{
/// Phase.db2 id of the default phase, a spawn with phase 0 is in it too
constexpr uint32 DefaultPhaseId = 169;

/// Spawn columns phaseId / phaseGroup: only 0 and 169 without a group are the default phase
constexpr bool IsDefaultSpawnPhase(uint32 phaseId, uint32 phaseGroup)
{
    return phaseGroup == 0 && (phaseId == 0 || phaseId == DefaultPhaseId);
}

/// A phase every player in its area gets without doing anything: world `phase_area` grants it and no
/// condition anywhere restricts it (the new Silvermoon city lives in one). Spawns in it are what a visitor
/// normally sees, so bots may use them
bool IsSharedPhase(uint32 phaseId);

/// What bots may use: the default phase and the shared phases (no phase group)
inline bool IsUsableSpawnPhase(uint32 phaseId, uint32 phaseGroup)
{
    return phaseGroup == 0 && (phaseId == 0 || phaseId == DefaultPhaseId || IsSharedPhase(phaseId));
}

/// Loads the quests that open, close or change a phase (world `conditions` of phase and terrain swap
/// sources). Safe to call more than once, world thread
void Load();

/// A quest whose state decides which phase a player is in: accepting or finishing it may move the bot. Includes
/// the quests found at runtime (BlockQuest). Any thread
bool IsPhaseGatedQuest(uint32 questId);
uint32 GetGatedQuestCount();

/// The phases of a player that are neither the default phase, a shared phase, nor cosmetic (sorted ids): the ones
/// that change what the player sees. A bot that gets one through a quest has left the world other players are in
std::vector<uint16> ForeignPhases(Player const* player);
/// A quest that moved a bot into a phase of its own: remembered for this run and written to
/// playerbots_quest_blacklist so no bot takes it after a restart. Any thread
void BlockQuest(uint32 questId, char const* reason);
uint32 GetSharedPhaseCount();
}
}

#endif // Playerbots_PhaseGuard_h__
