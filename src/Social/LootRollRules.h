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

#ifndef Playerbots_LootRollRules_h__
#define Playerbots_LootRollRules_h__

#include <algorithm>
#include <cstdint>

// What a bot answers when a group loot roll starts (Playerbots.Loot.Roll). No core types, tested standalone
// (tests/loot_roll_rules_test.cpp). The way a player plays it: need what is an upgrade, greed what is not but sells or may
// be wanted, pass on gear it cannot use. Ideas from AzerothCore's LootRollAction, minus its item usage tables.
namespace Playerbots
{
enum class RollChoice : uint8_t
{
    Pass,
    Greed,
    Need
};

struct RollInput
{
    bool Equippable = false;        // armor, weapon, trinket... something worn
    bool Usable = false;            // the bot may wear it: class, armor type, primary stat, level, skill
    float UpgradeMargin = 0.0f;     // score gain over the worst piece the bot wears in that slot; 0 = no gain, empty slot = its whole score
    bool SellsForMoney = true;      // the item has a vendor price
};

inline RollChoice ChooseRoll(RollInput const& input)
{
    if (input.Equippable)
    {
        if (!input.Usable)
            return RollChoice::Pass;
        return input.UpgradeMargin > 0.0f ? RollChoice::Need : RollChoice::Greed;
    }

    // materials, consumables, recipes: nobody needs them, a greed roll keeps them winnable
    return input.SellsForMoney ? RollChoice::Greed : RollChoice::Pass;
}

/// When a bot answers, so a table of bots does not vote in the same instant: 1.5 to 6 seconds, never later than 60% of the roll time
inline uint32_t RollAnswerDelayMs(float rand01, uint32_t rollTimeMs)
{
    rand01 = std::clamp(rand01, 0.0f, 1.0f);
    uint32_t delay = 1500 + uint32_t(rand01 * 4500.0f);
    uint32_t cap = rollTimeMs ? uint32_t(float(rollTimeMs) * 0.6f) : delay;
    return std::min(delay, std::max(cap, 300u));
}
}

#endif
