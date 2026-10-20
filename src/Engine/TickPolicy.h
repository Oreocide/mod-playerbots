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

#ifndef Playerbots_TickPolicy_h__
#define Playerbots_TickPolicy_h__

#include <algorithm>
#include <cstdint>

// How long a bot waits before its next AI tick (Playerbots.DynamicTick.*). No core types, tested standalone
// (tests/tick_policy_test.cpp). Idea from AzerothCore's GetReactDelay: decide often while fighting, rarely while
// idle, and let an action say how long its own effect takes (a global cooldown, a move).
namespace Playerbots
{
enum class TickKind : uint8_t
{
    Combat,
    Idle,   // alive, not fighting
    Dead
};

struct TickPolicy
{
    uint32_t CombatMs = 300;
    uint32_t IdleMinMs = 1000;
    uint32_t IdleMaxMs = 2500;
    uint32_t DeadMs = 2000;
    uint32_t FloorMs = 100;     // never faster than this, whatever an action asks
};

/// requestedMs: the delay the last action asked for (0 = none). rand01 in [0, 1)
inline uint32_t NextTickDelay(TickPolicy const& policy, TickKind kind, uint32_t requestedMs, float rand01)
{
    uint32_t delay = 0;
    switch (kind)
    {
        case TickKind::Combat:
            delay = policy.CombatMs;
            break;
        case TickKind::Dead:
            delay = policy.DeadMs;
            break;
        case TickKind::Idle:
        {
            uint32_t lo = std::min(policy.IdleMinMs, policy.IdleMaxMs);
            uint32_t hi = std::max(policy.IdleMinMs, policy.IdleMaxMs);
            rand01 = std::clamp(rand01, 0.0f, 0.9999f);
            delay = lo + uint32_t(float(hi - lo) * rand01);
            break;
        }
    }

    // an action that knows how long its effect lasts replaces the default; the slowest idle tick bounds it
    if (requestedMs)
        delay = std::min(requestedMs, std::max(policy.IdleMaxMs, policy.CombatMs));

    return std::max(delay, policy.FloorMs);
}
}

#endif
