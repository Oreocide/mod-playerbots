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

#ifndef Playerbots_BotCrafting_h__
#define Playerbots_BotCrafting_h__

#include "Define.h"
#include <string>
#include <vector>

class Player;

namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;
struct TriggerNode;
}

// What a bot does with its professions and its bags (AC "craft random item", "disenchant random item", "enchant random item",
// "use random recipe", "open items", "unlock items"), each behind its own Playerbots.* key (Economy/ProfessionsConfig.h). Only bots a
// player can see (PlayerbotAI::IsSimulated) do any of it; far bots never touch their bags here.
namespace Playerbots::Prof
{
/// The rpg "craft" activity may be rolled: the bot has a recipe it can pay for, or it is a blacksmith in a capital (who works at a
/// forge for show). Playerbots.Craft.AnyProfession
bool WantsToCraft(Player* bot);

/// The rpg "craft" activity for every production profession, used instead of the blacksmith-only one while
/// Playerbots.Craft.AnyProfession is on: pick a recipe the bags pay for, walk to the station it needs (or make one, a cooking fire),
/// make it a few times, then go idle. Keeps its state in the bot's RpgInfo: Target = station, NodeEntry = recipe, NodeSpawn = casts
/// left, TaxiDestNode = spell that makes the station first
bool DoCraftAny(PlayerbotAI& ai, uint32 now);

/// Actions "disenchant gear", "enchant gear", "learn recipe", "open items" and their triggers
void RegisterCraftingAi(AiRegistry& registry);

/// Their nodes in the "professions" strategy (non-combat, above the rpg routine and questing: one short stop)
std::vector<TriggerNode> CraftingTriggerNodes();

/// Chat order "open": open the lockboxes and clams in the bags now (a rogue picks one locked box per order)
bool OpenItemsOrder(PlayerbotAI& ai, std::string const& param);
}

#endif // Playerbots_BotCrafting_h__
