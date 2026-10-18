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

#ifndef PlayerbotsAmbient_h__
#define PlayerbotsAmbient_h__

#include "Define.h"

class Player;

namespace Playerbots
{
class PlayerbotAI;

/// The small things a player does that are not a job: a hop now and then, a wave at someone standing close.
/// All of it runs on the bot's own map thread, only for bots that run their full AI (a player can see them).
namespace Ambient
{
/// A short hop forward, only on level mesh ground (outdoors, not mounted, not swimming, not in combat).
/// false when the bot cannot hop here.
bool TryHop(Player* bot, float minDist, float maxDist);

/// While walking: rolls the hop chance for the time since the last call
void WalkingHop(PlayerbotAI& ai, uint32 now);

/// Standing around: now and then a hop on the spot
void IdleHop(PlayerbotAI& ai, uint32 now);

/// A wave, hello, greeting or bow (a text emote, like the client sends) at a player within Rpg.GreetDistance, at most once
/// per Rpg.GreetCooldown. true when it did.
bool GreetNearbyPlayer(PlayerbotAI& ai, uint32 now);

/// The nearest bot within greet range answers a player's text emote once per sender per cooldown. Map thread (the emote opcode is INPLACE).
void ReturnTextEmote(Player* sender, uint32 emoteId, uint32 now);

/// A resting bot sits on a chair within 15 yards, through the gameobject-use opcode. true while seated or walking to one.
bool TryChair(PlayerbotAI& ai);
}
}

#endif
