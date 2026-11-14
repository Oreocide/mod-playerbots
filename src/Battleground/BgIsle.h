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

#ifndef Playerbots_BgIsle_h__
#define Playerbots_BgIsle_h__

// Isle of Conquest (map 628), BG_PLAN.md phase 5.1/5.2: what the Valley planner cannot read from playerbots_bg_node rows. Both teams
// start inside a keep whose three gates stay shut until siege engines break one; the navmesh is built without gameobjects, so it runs
// straight through a shut gate (tools/bg navcheck, 2026-11-13). A bot leaves its sealed keep by the keep's teleporters, enters a keep
// only through a broken gate (its own also by the teleporters), and goes round a keep it does not need. Siege: an attacker of the team
// that holds the Workshop drives a Demolisher to the enemy front gate and rams it.

#include "Define.h"
#include "Position.h"

class Player;

namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;

namespace Bg
{
struct Assignment;
struct BgPlan;

namespace Isle
{
constexpr uint32 MapId = 628;

enum class Step : uint8
{
    Direct,     // nothing in the way: walk to the target
    Waypoint,   // walk to the waypoint first (a gate, a teleporter, a keep corner)
    Portal,     // a keep teleporter was used this step: nothing else moves the bot
    Blocked     // the target is inside an enemy keep with all its gates standing
};

/// Where a bot on map 628 that wants to reach `where` goes first (Bg::Go asks before every move)
Step Next(Player* bot, Position const& where, Position& waypoint);

enum class DriveStep : uint8
{
    None,       // not a driver: play the job
    Walk,       // walk to `walkTo` (the Demolisher it is going to board)
    Busy        // boarding or seated: the vehicle code moves it
};

/// An attacker of a team that holds the Workshop, while the enemy's front gate stands, boards a free Demolisher (at most
/// Playerbots.Bg.Isle.Drivers per team) and rams the gate. Called from Bg::Play for every bot on map 628
DriveStep Drive(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job, Position& walkTo);

/// "bg vehicle" (a seated bot drives and fires, nothing else runs) and "bg wall" (a fight across a shut keep wall is dropped)
void Register(AiRegistry& registry);
}
}
}

#endif
