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

#ifndef Playerbots_BgCoordinator_h__
#define Playerbots_BgCoordinator_h__

// The battleground coordinator path (docs/design/BG_COORDINATOR.md §2): with Playerbots.Bg.Coordinator = 1 on a map that has a
// profile (BgMode.h FindProfile), the planner hands the per-team planning of a match to the coordinator. Its state lives in its own
// tables keyed by match, not in BgPlan, so the planner's structures and old path stay as they are. World thread, like the planner

#include "Battleground/BgPlanner.h"
#include <string>
#include <vector>

class Battleground;
class Map;
class Player;

namespace Playerbots::Bg
{
class Coordinator
{
public:
    /// True when the coordinator plans this match. Decided at the match's first plan and kept until it closes, so a config reload in
    /// the middle of a match never hands its jobs from one path to the other
    static bool Coordinates(BgPlan const& plan);
    /// The per-team planning of a coordinated match (instead of the planner's PlanTeam / PlanValleyTeam); the planner has read the
    /// state, the roster and the events before
    static void Plan(Planner& planner, BgPlan& plan, Battleground* bg, Map* map, std::vector<Player*> const (&members)[2], std::string const& reason,
        uint32 now);
    /// The planner drops the match's plan
    static void Close(BgPlan const& plan);
    /// .bot bg squads: per team the phase, the general's push gate, the fallback share and every task with its bots
    static std::vector<std::string> Report(BgPlan const& plan);
    /// A coordinated captain squad's own push on that boss (§3.3): its rally state and puller. False when the boss has none (the bot side
    /// then reads the team's push as before)
    static bool SquadPush(BgPlan const& plan, uint32 team, int8 boss, BgRules::RallyState& state, ObjectGuid& puller);
    /// Bots that took the fallback task, and all assignments, since the match began (Playerbots.Bg.Metrics)
    static void FallbackCounts(BgPlan const& plan, uint32 team, uint32& fallbacks, uint32& assignments);
};
}

#endif
