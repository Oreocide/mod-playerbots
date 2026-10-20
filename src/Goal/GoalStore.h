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

#ifndef PlayerbotsGoalStore_h__
#define PlayerbotsGoalStore_h__

#include "ObjectGuid.h"
#include <string>

class Player;

namespace Playerbots
{
class PlayerbotAI;

/// World-table goals and the six personas. Load and reload run on the world thread.
/// A map thread copies the rows it needs and drops the lock before it paths.
namespace GoalStore
{
uint32 Load();
/// Rebuilds the snapshot. Returns how many goal rows were read (0 if the table is missing).
uint32 Reload();

/// Near bot, called when it is idle. Starts one matching goal, or leaves the caller to roll today's weights.
bool TryBegin(PlayerbotAI& ai, uint32 nowMs);

/// Far bot. Stores the same kind of goal and does not path. Returns when to check again.
/// timedOnly: only rows tied to a game event, an hour window or weekdays (Playerbots.Abstract.Enable: the AC status weights decide the rest)
uint32 OnFrozen(PlayerbotAI& ai, uint32 nowMs, bool timedOnly = false);

/// The bot holds a stored goal that has not ended yet
bool HasGoal(ObjectGuid guid, uint32 nowMs);

/// True once, when a stored quest goal has reached its end. The caller grants XP on the map thread.
bool ReadyForAbstractXp(ObjectGuid guid, uint32 nowMs);

/// Drops a bot's goal and its zone count. Safe from the AI destructor.
void Forget(ObjectGuid guid);

struct Inspect
{
    bool Known = false;
    std::string Persona;
    std::string Goal;
    std::string Kind;
    bool Far = false;
};
Inspect Describe(ObjectGuid guid);
}
}

#endif
