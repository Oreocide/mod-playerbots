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

#ifndef Playerbots_GenericStrategies_h__
#define Playerbots_GenericStrategies_h__

#include "Engine.h"
#include <array>
#include <memory>
#include <string_view>

namespace Playerbots
{
/// Strategies every new bot starts with
inline constexpr std::array<std::string_view, 11> DefaultStrategies = { "death", "teleport", "combat", "loot", "consume", "follow", "travel", "buff", "threat", "wait for attack", "group events" };

/// Registers the generic actions, triggers, values and strategies with the AiRegistry (once, thread safe)
void EnsureAiRegistered();

/// Strategy by registry name (death, teleport, combat, loot, consume, follow, stay, wander, rpg, quest, pvp,
/// later the class strategies). nullptr for unknown names
std::unique_ptr<Strategy> CreateStrategy(std::string_view name);
}

#endif // Playerbots_GenericStrategies_h__
