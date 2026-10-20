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

#ifndef Playerbots_GroupEvents_h__
#define Playerbots_GroupEvents_h__

namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;

/// Things that happen to a bot in a group and that a client answers: a ready check, a trade a player opens, a quest a player shares
/// (Playerbots.Group.Events). Bots receive no packets for these, so the bot's own tick polls the game state and raises an event
/// (AiContext::Raise): "ready check", "trade", "quest share". The strategy "group events" answers them through the same handlers
/// a client uses; the THREADUNSAFE ones (trade, quest confirm) go through the world queue.
namespace GroupEvents
{
/// Once per AI tick, from the bot's map thread
void Poll(PlayerbotAI& ai);
void Register(AiRegistry& registry);
}
}

#endif
