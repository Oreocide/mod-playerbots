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

#ifndef Playerbots_BotFishing_h__
#define Playerbots_BotFishing_h__

#include <string>
#include <vector>

namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;
struct TriggerNode;
}

// Fishing (AC "go fishing", "use bobber", "master fishing"), Playerbots.Fishing.*: a bot with the skill that has nothing better to do
// near water (an idle moment of its rpg routine, or its master fishing next to it) walks to a shore on the navmesh, faces the water,
// fishes a few casts and loots what it catches (real loot, pools included). The bobber is clicked by the world-thread loot watch
// (BotLootWatch.h): the cast supervisor keeps the AI asleep while the channel runs. Only bots a player can see fish.
namespace Playerbots::Prof::Fishing
{
/// Trigger "fishing due" and action "fish"
void RegisterFishingAi(AiRegistry& registry);

/// Its node in the "professions" strategy (non-combat, above the rpg routine, questing and following)
std::vector<TriggerNode> FishingTriggerNodes();

/// Chat order "fish": fish here (next to the master) for a while; "fish stop" ends it
bool Order(PlayerbotAI& ai, std::string const& param);
}

#endif // Playerbots_BotFishing_h__
