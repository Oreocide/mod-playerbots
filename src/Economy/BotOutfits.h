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

#ifndef Playerbots_BotOutfits_h__
#define Playerbots_BotOutfits_h__

#include "ObjectGuid.h"
#include <string>

class Player;

namespace Playerbots
{
class PlayerbotAI;
}

// Named gear sets (AC "outfit"). The core keeps a player's equipment sets in a private container with no accessor, and the guid
// of a new set only reaches the client (SMSG_EQUIPMENT_SET_ID), so the names and pieces are kept in playerbots_db_store
// (keys "outfits" and "outfit:<name>") and putting one on goes through the core's equipment set handler
// (WorldSession::HandleUseEquipmentSet), as the client's equipment manager does.
namespace Playerbots::Prof::Outfits
{
/// Chat order "outfit": list | save <name> (+name) | equip <name> (name) | delete <name> (-name) | show <name> (?name).
/// Playerbots.Outfits.Enable
bool Order(PlayerbotAI& ai, std::string const& param);

/// The item is a piece of one of the bot's outfits (kept out of disenchanting)
bool IsOutfitItem(Player const* bot, ObjectGuid item);
}

#endif // Playerbots_BotOutfits_h__
