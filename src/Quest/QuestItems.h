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

#ifndef Playerbots_QuestItems_h__
#define Playerbots_QuestItems_h__

namespace Playerbots
{
class AiRegistry;
}

/// Quest items (AC parity, mod-playerbots#38; AC "use random quest item"): a bot with an unfinished quest uses a quest item from
/// its bags on a creature or gameobject near it that the item's spell accepts (the target conditions of the spell pick the entry
/// when it has them; otherwise hostile creatures and quest NPCs are tried, nearest first, three casts at most every few seconds).
/// In a fight only on the bot's own enemy ("use it when the beast is weakened"). Self, area and ground items are never guessed.
namespace Playerbots::QuestItems
{
struct Config
{
    bool Enable = false;    // Playerbots.Quest.UseItems
};

Config const& GetConfig();
void LoadConfig();
void Register(AiRegistry& registry);
}

#endif
