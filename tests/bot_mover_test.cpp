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

// Standalone test of the movement arbitration (Travel/MovePriority.h), see tests/run_engine_test.sh

#include "Travel/MovePriority.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    BotMoveState s;

    // nothing running: always launch
    CHECK(DecideMove(s, MovePriority::Travel, 1, 2, 3, false, 1000) == MoveDecision::Launch);

    // a running travel move toward the same place is not relaunched
    NoteLaunched(s, MovePriority::Travel, 10, 20, 5, 1000, 2000);
    CHECK(DecideMove(s, MovePriority::Travel, 10.4f, 20.4f, 5.5f, true, 1500) == MoveDecision::KeepRunning);
    // another place at the same priority cuts in
    CHECK(DecideMove(s, MovePriority::Travel, 30, 20, 5, true, 1500) == MoveDecision::Launch);
    // a higher priority toward another place cuts in
    CHECK(DecideMove(s, MovePriority::Chase, 30, 20, 5, true, 1500) == MoveDecision::Launch);

    // a position move is not undone by a chase, but an avoid move outranks it
    NoteLaunched(s, MovePriority::Position, 1, 1, 0, 2000, 1500);
    CHECK(DecideMove(s, MovePriority::Chase, 9, 9, 0, true, 2100) == MoveDecision::KeepRunning);
    CHECK(DecideMove(s, MovePriority::Avoid, 9, 9, 0, true, 2100) == MoveDecision::Launch);

    // a lower priority cannot cut into a running higher one
    NoteLaunched(s, MovePriority::Avoid, 0, 0, 0, 5000, 1500);
    CHECK(DecideMove(s, MovePriority::Chase, 50, 50, 0, true, 5200) == MoveDecision::KeepRunning);
    CHECK(DecideMove(s, MovePriority::Travel, 50, 50, 0, true, 5200) == MoveDecision::KeepRunning);
    // an equal priority to another place does
    CHECK(DecideMove(s, MovePriority::Avoid, 50, 50, 0, true, 5200) == MoveDecision::Launch);

    // the hold expires: a stale high-priority move no longer blocks
    CHECK(DecideMove(s, MovePriority::Travel, 50, 50, 0, true, 6600) == MoveDecision::Launch);

    // the spline already ended: nothing to protect
    CHECK(DecideMove(s, MovePriority::Travel, 50, 50, 0, false, 5200) == MoveDecision::Launch);

    // stopped state launches
    NoteStopped(s);
    CHECK(DecideMove(s, MovePriority::None, 1, 1, 1, true, 5200) == MoveDecision::Launch);

    // expiry survives the 32-bit game time wrap
    NoteLaunched(s, MovePriority::Chase, 0, 0, 0, 0xFFFFFF00u, 1000);
    CHECK(DecideMove(s, MovePriority::Travel, 9, 9, 0, true, 0xFFFFFF80u) == MoveDecision::KeepRunning);
    CHECK(DecideMove(s, MovePriority::Travel, 9, 9, 0, true, 0x00000500u) == MoveDecision::Launch);

    if (Failures)
    {
        std::printf("bot_mover_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("bot_mover_test: ok\n");
    return 0;
}
