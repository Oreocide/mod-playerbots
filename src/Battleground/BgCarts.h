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


#ifndef Playerbots_BgCarts_h__
#define Playerbots_BgCarts_h__

// Silvershard Mines (map 727), BG_PLAN.md phase 5.5: three mine carts roll along fixed tracks at 2 yd/s; a team owns a cart by having
// more players within 25 yd of it (a control zone object that moves with the cart), scores +1 per owned cart every 2 s and +150 when
// an owned cart reaches its depot; 1500 wins (worktrees/bg-silvershard/SILVERSHARD_DATA.md). The capture planner plays the carts as
// nodes (their control zone objects, owner from GetControllingTeam): attackers go to carts we do not own, defenders stay with ours.
// Each start room is a platform 8.5 yd above the field with no navmesh link down (tools/bg navcheck, 2026-11-14): a bot leaves it by
// a fixed drop at one of its two exits (user OK 2026-11-14). Track switches are not used yet.

#include "Define.h"
#include "Position.h"

class Map;
class Player;

namespace Playerbots
{
namespace Bg
{
struct Assignment;
struct BgPlan;

namespace Carts
{
constexpr uint32 MapId = 727;

enum class Step : uint8
{
    Direct,     // outside the start rooms: walk to the target
    Waypoint,   // in a start room: walk to the edge of its nearest exit first
    Busy        // dropping out of the room: nothing else moves the bot
};

/// Where a bot on map 727 that wants to reach `where` goes first (Bg::Go asks before every move)
Step Next(Player* bot, Position const& where, Position& waypoint);

/// Every plan, after the object pass: each cart's owner and position from its control zone object (the line's spawn point while
/// the next cart is on its way)
void ReadState(BgPlan& plan, Map* map);

/// The cart of the bot's job (or the nearest one its team does not hold) when a fight pulled the bot out of its control zone: the "bg
/// carrier" leash walks it back, in combat too (only who stands in the zone counts)
bool AwayFromCart(Player const* bot, BgPlan const& plan, Assignment const& job, Position& cart);

/// A map whose carts this file reads: Silvershard Mines (727) and Deephaul Ravine (2656)
bool HasCarts(uint32 mapId);
}

// Deephaul Ravine (map 2656, worktrees/bg-deephaul/DEEPHAUL_DATA.md): Silvershard's cart play on two carts, plus Eye of the Storm's crystal
// play with the capture at the team's own point (areatrigger 30 Alliance, 31 Horde) instead of a tower
namespace Ravine
{
constexpr uint32 MapId = 2656;

/// Where our crystal carrier scores: the capture point of team 0 (Alliance) or 1 (Horde)
Position const& CapturePoint(uint8 team);
}
}
}

#endif
