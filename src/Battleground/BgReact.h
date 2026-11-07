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

#ifndef Playerbots_BgReact_h__
#define Playerbots_BgReact_h__

// The reactive behaviours of the team planner (docs/design/BG_PLAN.md §4, phase 4): the picture of the match, threats at our
// nodes and their retakes, falling back, and the overlay each bot follows. World thread, called from Planner::Plan like the rest
// of the planner, so nothing here is locked

#include "Battleground/BgPlanner.h"
#include <string>
#include <vector>

class Battleground;
class Map;
class Player;

namespace Playerbots::Bg
{
/// Every player of the match and the counts of each team near each objective; once per plan, after the objective states
void BuildPicture(BgPlan& plan, Map* map, uint32 now);

/// One team after its split and its targets: falling back (#8), threats and retakes (#1, #2), then the overlay every bot follows
/// (BgRules::Overlay). Runs with every key off too (it then only clears overlays). Returns the bots that got a new errand; reason
/// gets the first of them for .bot bg
uint32 ReactTeam(BgPlan& plan, uint32 team, std::vector<Player*> const& bots, std::string& reason, uint32 now);

/// A healer: a bot by its role, a player by the role of the specialization
bool IsHealer(Player const* player);

/// #4 and #15: the score outlook and the stance of both teams; once per plan, before the teams are planned
void UpdateStances(BgPlan& plan, Battleground const* bg, Map const* map, uint32 now);
/// The defence share of a team's stance (the rolled one while ScoreClock and Endgame are off)
uint32 StanceDefendPct(TeamPlan const& teamPlan);
/// Turtling on a node race map: the most attackers the team may have (bots when there is no cap)
uint32 TurtleCap(BgPlan const& plan, uint32 team, uint32 bots);

/// #10: the newest "go" or "everyone" order of the team's real players that still holds and names something the team can
/// attack (their flag room, a node it does not hold, their living boss); nullptr: none, or HumanCalls off
CallOrder const* AttackOrder(BgPlan const& plan, uint32 team, uint32 now);

/// #12 and #20, before a team's split: its skill (rolled once per match, moved against streak on the side of a real player) and
/// the style of each bot (once per match). With the keys off: skill 100, every bot an objective player
void PrepareTeam(BgPlan& plan, uint32 team, std::vector<Player*> const& members, std::vector<Player*> const& bots, int32 streak);
}

#endif
