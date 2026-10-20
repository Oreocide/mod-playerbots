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

// Standalone test of the weighted status roll (Strategies/Rpg/StatusPick.h), see tests/run_engine_test.sh

#include "Strategies/Rpg/StatusPick.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    // AC default weights: DoQuest 60, WanderNpc 20, GoGrind 15, Rest 5 (statuses 1, 2, 4, 6)
    std::vector<StatusWeight> list = { { 1, 60 }, { 2, 20 }, { 4, 15 }, { 6, 5 } };
    CHECK(SumWeights(list) == 100);
    CHECK(PickWeighted(list, 1, 0) == 1);
    CHECK(PickWeighted(list, 60, 0) == 1);       // the first weight is 1..60
    CHECK(PickWeighted(list, 61, 0) == 2);
    CHECK(PickWeighted(list, 80, 0) == 2);
    CHECK(PickWeighted(list, 81, 0) == 4);
    CHECK(PickWeighted(list, 95, 0) == 4);
    CHECK(PickWeighted(list, 96, 0) == 6);
    CHECK(PickWeighted(list, 100, 0) == 6);

    // every roll lands on the candidate that owns it, and each one gets exactly its weight of the rolls
    uint32_t counts[8] = {};
    for (uint32_t roll = 1; roll <= 100; ++roll)
        ++counts[PickWeighted(list, roll, 0)];
    CHECK(counts[1] == 60 && counts[2] == 20 && counts[4] == 15 && counts[6] == 5 && counts[0] == 0);

    // nothing to pick from, or a roll outside the range: the fallback (Rest) comes back
    CHECK(PickWeighted({}, 1, 6) == 6);
    CHECK(SumWeights({}) == 0);
    CHECK(PickWeighted(list, 0, 6) == 6);
    CHECK(PickWeighted(list, 101, 6) == 6);

    // one candidate takes every roll
    CHECK(PickWeighted({ { 3, 7 } }, 1, 0) == 3 && PickWeighted({ { 3, 7 } }, 7, 0) == 3);

    if (Failures)
    {
        std::printf("rpg_pick_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("rpg_pick_test: ok\n");
    return 0;
}
