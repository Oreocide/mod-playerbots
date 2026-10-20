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

#ifndef Playerbots_PotionRules_h__
#define Playerbots_PotionRules_h__

#include "Define.h"

struct ItemTemplate;

namespace Playerbots
{
enum class PotionKind : uint8
{
    None,
    Health,     // an instant heal on use
    Mana        // an instant mana energize on use
};

/// What a consumable does for a bot in a fight: an on-use spell with an instant heal or an instant mana energize, and not a food or drink
/// that regenerates over time. The one rule behind both the stock the gear factory buys and the potion the combat action drinks, so every
/// stocked item is one that will be used. `spellOut` gets the spell to cast.
PotionKind ClassifyPotion(ItemTemplate const* proto, uint32* spellOut = nullptr);
}

#endif
