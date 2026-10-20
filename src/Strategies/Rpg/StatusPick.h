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

#ifndef Playerbots_StatusPick_h__
#define Playerbots_StatusPick_h__

#include <cstdint>
#include <utility>
#include <vector>

// The weighted status roll of AzerothCore's NewRpg (RandomChangeStatus), without core types so the near loop and the abstract
// picker of frozen bots share it and tests/rpg_pick_test.cpp can run it standalone.
namespace Playerbots
{
using StatusWeight = std::pair<uint8_t, uint32_t>;   // RpgStatus value, weight

inline uint32_t SumWeights(std::vector<StatusWeight> const& candidates)
{
    uint32_t sum = 0;
    for (StatusWeight const& candidate : candidates)
        sum += candidate.second;
    return sum;
}

/// The candidate whose cumulative weight first reaches roll (1 .. SumWeights); fallback when there are none, or roll is out of range
inline uint8_t PickWeighted(std::vector<StatusWeight> const& candidates, uint32_t roll, uint8_t fallback)
{
    uint32_t acc = 0;
    for (StatusWeight const& candidate : candidates)
    {
        acc += candidate.second;
        if (roll >= 1 && acc >= roll)
            return candidate.first;
    }
    return fallback;
}
}

#endif
