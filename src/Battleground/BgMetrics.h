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

#ifndef Playerbots_BgMetrics_h__
#define Playerbots_BgMetrics_h__

// Battleground metrics (docs/design/BG_COORDINATOR.md §2.10, §5.2; Playerbots.Bg.Metrics). Read only: nothing here changes a plan,
// a job or a bot, so a match plays the same with the key on or off. World thread: the planner calls Sample after each plan and Close
// when it drops one. Lines go to the logger server.playerbots.bg (INFO, falls back to server.playerbots / server)

#include "Battleground/BgPlanner.h"
#include <string>
#include <vector>

class Battleground;
class Map;

namespace Playerbots::Bg::Metrics
{
/// After each plan of a match: counts deaths, bot-seconds per objective, first assaults, when half of a team's attackers first
/// massed at an objective; once a minute of play, one line per team (score, deaths, per objective assigned / dead / present)
void Sample(BgPlan const& plan, Battleground* bg, Map* map, uint32 now);

/// The planner drops the plan (why: "match over", "match gone", ...): one line for the match, and the map's totals since boot
void Close(BgPlan const& plan, Battleground* bg, char const* why);

/// .bot bg metrics: per map since boot (matches, wins per team, average play time), then each running match's last lines
std::vector<std::string> Report();
}

#endif
