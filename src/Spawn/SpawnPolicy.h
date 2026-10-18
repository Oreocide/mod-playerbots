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

#ifndef PlayerbotsSpawnPolicy_h__
#define PlayerbotsSpawnPolicy_h__

#include "Optional.h"
#include "Position.h"
#include "SharedDefines.h"

class Player;
struct ConditionsReference;

namespace Playerbots::SpawnPolicy
{
/// Whether a point is ground that attacks bots of this team: an area or zone that only the other faction owns
/// (AreaTable.FactionGroupMask, the area first and its parents when the area has none), or the inside of a town whose
/// NPCs attack the team. Contested and neutral ground is never enemy ground.
/// Uses the map's terrain data: call from the world thread or the bot's own map thread.
bool IsEnemyGround(Player const* bot);

/// The same for a point the bot is about to be sent to (its area comes from the loaded map, if any)
bool IsEnemyGroundAt(Player const* bot, uint32 mapId, Position const& pos);

/// The same for a point that is not where a bot stands (spot lists): the area id and the team are given.
/// z is used for the Northrend Dalaran street boxes (0 skips the box and still checks the area id).
bool IsEnemyGround(uint32 mapId, uint32 areaId, float x, float y, float z, Team team);

/// The other faction's quarter of a split sanctuary (area id, or the Northrend Dalaran street box).
/// Does not ask whether the town's NPCs attack the team; hub picks use this on top of their own cluster check.
bool IsSplitDistrict(uint32 mapId, uint32 areaId, float x, float y, float z, Team team);

/// A known safe side of a split city the bot is standing in, when it is on the wrong side.
/// Empty when the bot is not in one of those cities, or we have no surveyed point for that city.
Optional<Position> SafeDistrict(Player const* bot);

/// The only team a graveyard serves according to its conditions (CONDITION_TEAM; the negative form means the other team),
/// 0 when it has none or they disagree. Graveyards have no team column any more, the faction is a condition.
uint32 TeamOfGraveyard(ConditionsReference const& conditions);
}

#endif
