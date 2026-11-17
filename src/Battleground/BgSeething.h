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

#ifndef Playerbots_BgSeething_h__
#define Playerbots_BgSeething_h__

// Seething Shore (map 1803), BG_PLAN.md phase 5.6: up to three azerite nodes are up at a time, each at one of twelve fissures; the
// team that captures one gets +100 and the node is gone (a new one comes up at a free fissure); 1500 wins
// (worktrees/bg-seething/SEETHING_DATA.md). The capture planner plays the twelve fissures as nodes that are known only while their
// node stands: nothing is ever held, so a team has no defenders. Both teams start, and come back from death, on an airship that
// circles the field 260 yd up with no navmesh link down: a bot leaves it when the ground below has a short mesh route to its node and
// falls (user OK 2026-11-15; the battleground's script gives the parachute and no fall damage).

#include "Define.h"
#include "Position.h"

class Map;
class Player;

namespace Playerbots
{
class PlayerbotAI;

namespace Bg
{
struct Assignment;
struct BgPlan;

namespace Seething
{
constexpr uint32 MapId = 1803;

/// Every plan, after the object pass: which fissures have a node up (the node's guid, READY and selectable), the rest unknown
void ReadState(BgPlan& plan, Map* map);

/// A bot on an airship, or falling from one: before the match it stays aboard; after the start it drops when the ground below has a
/// short mesh route to the node of its job (any route after a whole lap). True while the airship or the fall holds the bot (Bg::Play
/// asks before the match status check)
bool Airship(Player* bot, BgPlan const* plan, Assignment const* job);

/// The bot is in its deliberate fall from the airship: the fall rescue (PlayerbotAI::RememberStand) leaves it alone, or it puts the bot
/// straight back on the deck (2026-11-16 Seething retry: every drop was undone that way)
bool Dropping(Player const* bot);
}
}
}

#endif
