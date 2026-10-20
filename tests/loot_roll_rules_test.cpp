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

// Standalone test of the loot roll rules (Social/LootRollRules.h), see tests/run_engine_test.sh

#include "Social/LootRollRules.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    // gear: need an upgrade, greed a side-grade, pass what cannot be worn
    CHECK(ChooseRoll({ true, true, 12.0f, true }) == RollChoice::Need);
    CHECK(ChooseRoll({ true, true, 0.0f, true }) == RollChoice::Greed);
    CHECK(ChooseRoll({ true, false, 50.0f, true }) == RollChoice::Pass);       // plate on a mage, whatever the score says

    // not gear: greed when it sells, pass when it is worthless
    CHECK(ChooseRoll({ false, false, 0.0f, true }) == RollChoice::Greed);
    CHECK(ChooseRoll({ false, false, 0.0f, false }) == RollChoice::Pass);

    // answers are spread out, bounded, and early enough for a short roll
    CHECK(RollAnswerDelayMs(0.0f, 60000) == 1500);
    CHECK(RollAnswerDelayMs(1.0f, 60000) == 6000);
    CHECK(RollAnswerDelayMs(0.5f, 60000) == 3750);
    CHECK(RollAnswerDelayMs(1.0f, 5000) == 3000);                               // 60% of a 5 s roll
    CHECK(RollAnswerDelayMs(1.0f, 100) == 300);                                 // never below 300 ms
    CHECK(RollAnswerDelayMs(2.0f, 60000) == 6000);                              // out of range input is clamped

    if (Failures)
    {
        std::printf("loot_roll_rules_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("loot_roll_rules_test: ok\n");
    return 0;
}
