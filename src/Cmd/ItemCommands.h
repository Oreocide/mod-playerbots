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

#ifndef Playerbots_ItemCommands_h__
#define Playerbots_ItemCommands_h__

#include <string>

class PlayerbotAI;

// Owner commands that name items by link ("equip [Sword of Truth]"), registered by ChatCommands.cpp. Each returns false
// when it could do nothing at all (the master then hears "That did not work"), the replies say what happened
namespace Playerbots
{
bool EquipItems(PlayerbotAI& ai, std::string const& param);
bool UnequipItems(PlayerbotAI& ai, std::string const& param);
bool UseItems(PlayerbotAI& ai, std::string const& param);
bool SellItems(PlayerbotAI& ai, std::string const& param);
bool BuyItems(PlayerbotAI& ai, std::string const& param);
bool DestroyItems(PlayerbotAI& ai, std::string const& param);
}

#endif
