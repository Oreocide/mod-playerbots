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

#ifndef PlayerbotsAirFollow_h__
#define PlayerbotsAirFollow_h__

#include "ObjectGuid.h"

class Player;

namespace Playerbots
{
class PlayerbotAI;

/// Party and raid bots follow a master who flies (Playerbots.Air.Follow). The master's own flight is the route: its positions are recorded
/// as a trail, and each bot flies along that trail to a stable place around the master, so the bots inherit the clearance the master
/// has (the one place besides AirCorridor where a bot leaves the navmesh, user decision 2026-10-19). When the master lands the bots descend
/// onto navmesh points next to it. All of it runs on the bot's map thread, from the bot's follow action.
namespace AirFollow
{
/// The bot is in the air (or taking off, or landing) following its master or landing for a fight: the ground checks of PlayerbotAI leave it alone
bool Active(ObjectGuid bot);

/// The follow action of a bot with a master. True if the bot is handled here (the master is in the air, or the bot is still up and
/// landing); false = ordinary follow
bool Update(PlayerbotAI& ai, Player* master);

/// A bot that is in the air and under attack: lands on the ground below it before it fights (a bot that dismounts at cruise height falls).
/// True while it is still coming down, false once it is on the ground (or cannot be landed: the caller then dismounts as before)
bool EmergencyLand(PlayerbotAI& ai);

/// Every tick of a bot that is in the air following (first thing in PlayerbotAI::Update, before the follow action could drop it): a bot
/// whose master is gone (logged out, left the group, another map, an instance) or that froze is put on the ground at once, mount off
void Tick(PlayerbotAI& ai, bool frozen);

void Forget(ObjectGuid bot);
}
}

#endif
