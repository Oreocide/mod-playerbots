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

#ifndef Playerbots_MovePriority_h__
#define Playerbots_MovePriority_h__

#include <cmath>
#include <cstdint>

// Per-bot movement arbitration for BotMover. No core types, tested standalone (tests/bot_mover_test.cpp).
// Idea from AzerothCore's MovementPriority/LastMovement values: a move that is already running toward the same
// place is not relaunched, and a lower-priority caller cannot cut into a running higher-priority move.
namespace Playerbots
{
enum class MovePriority : uint8_t
{
    None = 0,
    Travel = 10,    // walking a route or an rpg destination
    Follow = 20,    // keeping up with the master
    Chase = 30,     // the core chase generator toward the target
    Position = 35,  // stepping behind the target, spreading, keeping range: must not be undone by the chase generator
    Avoid = 40      // leaving a hazard or fleeing: nothing below this may cut in
};

struct BotMoveState
{
    MovePriority Priority = MovePriority::None;
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    uint32_t ExpiresAtMs = 0;   // game time ms; a move not renewed by then no longer holds its priority
    bool Active = false;
};

enum class MoveDecision : uint8_t
{
    Launch,     // start (or restart) a path to the requested place
    KeepRunning // the running move already serves this request, or outranks it
};

constexpr float SameDestYards = 1.0f;

inline bool SameDest(BotMoveState const& s, float x, float y, float z)
{
    return std::fabs(s.X - x) <= SameDestYards && std::fabs(s.Y - y) <= SameDestYards && std::fabs(s.Z - z) <= 2.5f;
}

/// `moving` = a spline is running now. `now` and `ExpiresAtMs` are game time ms (wrap-safe for the gaps used here).
inline MoveDecision DecideMove(BotMoveState const& state, MovePriority wanted, float x, float y, float z, bool moving, uint32_t now)
{
    bool holds = state.Active && moving && int32_t(state.ExpiresAtMs - now) > 0;
    if (!holds)
        return MoveDecision::Launch;
    if (wanted < state.Priority)
        return MoveDecision::KeepRunning;       // outranked
    if (SameDest(state, x, y, z))
        return MoveDecision::KeepRunning;       // duplicate request for the move that is running
    return MoveDecision::Launch;                // same or higher priority, new place: cut in
}

/// Records a launched move. `holdMs` is how long it keeps its priority without being renewed.
inline void NoteLaunched(BotMoveState& state, MovePriority prio, float x, float y, float z, uint32_t now, uint32_t holdMs)
{
    state.Priority = prio;
    state.X = x;
    state.Y = y;
    state.Z = z;
    state.ExpiresAtMs = now + holdMs;
    state.Active = true;
}

inline void NoteStopped(BotMoveState& state)
{
    state.Active = false;
    state.Priority = MovePriority::None;
}
}

#endif
