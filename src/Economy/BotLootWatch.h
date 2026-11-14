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

#ifndef Playerbots_BotLootWatch_h__
#define Playerbots_BotLootWatch_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <vector>

class Player;

// Loot that opens some time after the bot's action: a disenchant (the cast ends, then the loot window opens) and a fishing cast (the
// bobber splashes up to 20 s later, the player clicks it, the loot opens). The loot opcodes are THREADUNSAFE and the cast supervisor
// keeps the AI from ticking while a channel runs, so the watches are run by the playerbots_professions world script on the world
// thread (after the map updates), never by the AI. Map threads only add watches.
namespace Playerbots::Prof::LootWatch
{
/// World thread: takes every item of the loot of `owner` (a creature, gameobject or item) that the bot may take, then its money,
/// then closes it (an item's loot that is empty destroys the item, as for a player). false if the bot has no such loot open
bool CollectLoot(Player* bot, ObjectGuid owner);

/// Any thread: when a loot of `owner` opens for the bot within timeoutMs, it is collected
void Expect(Player* bot, ObjectGuid owner, uint32 timeoutMs);

/// Any thread: the bot just cast fishing. The bobber summoned by one of `bobberSpells` is watched: when it splashes the bot uses it
/// after a human reaction time and loots the catch (or the fishing pool's). The watch ends by itself after timeoutMs
void WatchBobber(Player* bot, std::vector<uint32> bobberSpells, uint32 timeoutMs);

/// Any thread: a fishing watch of this bot is still running
bool IsFishing(ObjectGuid bot);

/// World thread: runs the watches
void Update();

/// Watches running now (all bots), for .bot status lines
uint32 Count();
}

#endif // Playerbots_BotLootWatch_h__
