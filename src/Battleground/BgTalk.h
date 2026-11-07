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

#ifndef Playerbots_BgTalk_h__
#define Playerbots_BgTalk_h__

// Battleground chat of the team planner (docs/design/BG_PLAN.md §4): the bots' short calls in instance chat (#9) and what a real
// player calls there (#10). World thread: the planner calls Say, the chat hook (PlayerbotsScripts.cpp) calls HearCall

#include "Battleground/BgPlanner.h"
#include <string>
#include <vector>

class Player;

namespace Playerbots::Bg
{
/// #9: the speaker says line in its battleground's instance chat, as a player's call: only while a real player is on that team,
/// one line per bot per 20 s, one per team per 10 s, the same call (key) not again within 30 s. Playerbots.Bg.CallOuts
void Say(BgPlan& plan, uint32 team, Player* speaker, std::string const& line, std::string const& key, uint32 now);

/// The word a call uses for an objective: its first alias ("st", "ibt", "galv"), else its name in lowercase
std::string CallName(BgPlan const& plan, int8 objective);

/// The spellings of the map's objectives for the call parser, from team's side: "base" is our flag room, and the bosses shared
/// words ("general", "captain") name the enemy's first
std::vector<BgRules::Alias> AliasesOf(BgPlan const& plan, uint32 team);

/// #10: a real player's line of instance chat (CHAT_MSG_INSTANCE_CHAT or _LEADER). A call the parser understands becomes an order
/// of the player's team in the planner (Playerbots.Bg.HumanCalls). True when it did
bool HearCall(Player* player, uint32 type, std::string const& msg);
}

#endif
