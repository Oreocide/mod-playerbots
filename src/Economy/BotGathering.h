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

#ifndef Playerbots_BotGathering_h__
#define Playerbots_BotGathering_h__

#include "Define.h"

class Player;

/// Real herb and ore gathering (AC parity, mod-playerbots#38; user decision 2026-11-13, reverses the 10-07 scenery rule for
/// gathering): at the node the rpg gather step walked to, the bot casts the gathering spell for the node's lock (its own, or the
/// lowest unskilled spell of that lock type), and the loot is taken on the world thread (Prof::LootWatch). Modern nodes (type 50)
/// give every player personal loot up to the node's cap, older ones are taken whole. Off: the bot kneels or mines for show.
namespace Playerbots::BotGathering
{
struct Config
{
    bool Real = false;      // Playerbots.Rpg.GatherReal
};

Config const& GetConfig();
void LoadConfig();
/// World thread, after the spell store is loaded: the gathering spell of every lock type
void OnStartup();

enum class GatherResult : uint8
{
    Casting,        // the spell started: wait castMs, the loot is taken when it opens
    Gone,           // the node is not there now (gathered, despawned, already looted by this bot): pick another
    Cannot          // no spell for the lock or the cast was refused: play the scenery instead
};

/// At the node (rpg gather step). castMs: the cast time of the spell started
GatherResult TryGather(Player* bot, uint64 spawnId, uint32& castMs);
}

#endif
