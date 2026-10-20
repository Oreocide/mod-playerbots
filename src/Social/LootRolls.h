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

#ifndef Playerbots_LootRolls_h__
#define Playerbots_LootRolls_h__

#include "Define.h"
#include "ObjectGuid.h"

class WorldPacket;

namespace Playerbots
{
/// Group loot rolls for bots (Playerbots.Loot.Roll). The bot's packet sink sees SMSG_START_LOOT_ROLL on whichever thread sends it
/// and only notes the roll; the vote itself is sent from the world thread (CMSG_LOOT_ROLL is a THREADUNSAFE opcode), after a
/// delay that spreads the bots over a few seconds
namespace LootRolls
{
/// Any thread
void OnBotPacket(ObjectGuid bot, WorldPacket const& packet);
/// World thread, every update
void Update(uint32 diff);
}
}

#endif
