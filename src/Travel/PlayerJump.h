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

#ifndef PlayerbotsPlayerJump_h__
#define PlayerbotsPlayerJump_h__

#include "Position.h"

class Player;

namespace Playerbots
{
/// The jump a player makes with the space bar, as the other clients see it. A client that jumps sends its movement flags (moving forward
/// and falling) with the jump data (upward speed, direction, horizontal speed); the server relays that to everybody around and each
/// client plays the jump animation and flies the character along the arc by itself. A bot has no client, so the same packet is made
/// from its movement info: the jump is not a spline. With the standard jump speed the arc takes 0.82 seconds and peaks 1.64 yards
/// up; the horizontal speed is chosen to land on `to`. At the end of the arc the server puts the bot there and sends the landing.
/// false if the bot cannot jump right now (mounted, swimming, falling, in a vehicle, a target too near or farther than a run can cover).
bool StartPlayerJump(Player* bot, Position const& to);

/// The bot is in the air of a StartPlayerJump (plus a short grace after landing): the mover and the fall rescue leave it alone
bool IsPlayerJumping(Player const* bot);
}

#endif
