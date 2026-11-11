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
class Player;

namespace Playerbots::Bg::Metrics
{
/// After each plan of a match: counts deaths, bot-seconds per objective, first assaults, when half of a team's attackers first
/// massed at an objective; every Playerbots.Bg.Metrics.LineSeconds (60) of play, one line per team (score, deaths, per objective
/// assigned / dead / present)
void Sample(BgPlan const& plan, Battleground* bg, Map* map, uint32 now);

/// The planner drops the plan (why: "match over", "match gone", ...): one line for the match, and the map's totals since boot
void Close(BgPlan const& plan, Battleground* bg, char const* why);

/// .bot bg metrics: per map since boot (matches, wins per team, average play time), then each running match's last lines
std::vector<std::string> Report();

/// Playerbots.Bg.Trace (a node row, "" = off): every world tick while the match runs, up to Trace.Bots bots with an attack job on that
/// objective are watched, and one line per bot is written every Trace.Ms: its job and task, position and distance to the objective,
/// last action, movement (generator, spline destination, splines launched and restarted before their end), Go calls, the forced
/// target and the combat target with their distances, the team's rally. World thread, like the bots' own AI here; read only
void Trace(BgPlan const& plan, Battleground* bg, Map* map, uint32 now);

/// Bg::Go reports each call (noPath: it found no mesh path) so the trace can count them per line; nothing for an untraced bot
void NoteGo(Player const* bot, Position const& where, bool noPath);
}

#endif
