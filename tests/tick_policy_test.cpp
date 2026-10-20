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

// Standalone test of the tick and cast policies (Engine/TickPolicy.h, Engine/CastPolicy.h), see tests/run_engine_test.sh

#include "Engine/CastPolicy.h"
#include "Engine/TickPolicy.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    TickPolicy p;

    // combat: fixed, idle: spread over the range, dead: slow
    CHECK(NextTickDelay(p, TickKind::Combat, 0, 0.5f) == 300);
    CHECK(NextTickDelay(p, TickKind::Idle, 0, 0.0f) == 1000);
    CHECK(NextTickDelay(p, TickKind::Idle, 0, 0.5f) == 1750);
    CHECK(NextTickDelay(p, TickKind::Idle, 0, 1.0f) < 2500);      // rand clamped below 1
    CHECK(NextTickDelay(p, TickKind::Dead, 0, 0.3f) == 2000);

    // an action's own delay replaces the default, bounded below by the floor and above by the slowest tick
    CHECK(NextTickDelay(p, TickKind::Combat, 1500, 0.0f) == 1500);
    CHECK(NextTickDelay(p, TickKind::Combat, 20, 0.0f) == 100);
    CHECK(NextTickDelay(p, TickKind::Idle, 60000, 0.0f) == 2500);

    // a swapped range still works
    TickPolicy swapped;
    swapped.IdleMinMs = 3000;
    swapped.IdleMaxMs = 1000;
    uint32_t d = NextTickDelay(swapped, TickKind::Idle, 0, 0.25f);
    CHECK(d >= 1000 && d <= 3000);

    // cast supervision
    CastView v;
    CHECK(JudgeCast(v) == CastVerdict::Continue);                 // no unit target: self cast

    v.HasUnitTarget = true;
    CHECK(JudgeCast(v) == CastVerdict::Continue);                 // a live enemy

    v.TargetAlive = false;
    CHECK(JudgeCast(v) == CastVerdict::Cancel);                   // target died
    v.SpellAllowsDeadTarget = true;
    CHECK(JudgeCast(v) == CastVerdict::Continue);                 // a resurrection wants a dead target

    CastView heal;
    heal.HasUnitTarget = true;
    heal.TargetFriendly = true;
    heal.SpellIsDirectHeal = true;
    heal.TargetHealthPct = 100.0f;
    heal.RemainingMs = 1500;
    CHECK(JudgeCast(heal) == CastVerdict::Cancel);                // direct heal on a full target
    heal.RemainingMs = 100;
    CHECK(JudgeCast(heal) == CastVerdict::Continue);              // nearly done: let it land
    heal.RemainingMs = 1500;
    heal.TargetHealthPct = 80.0f;
    CHECK(JudgeCast(heal) == CastVerdict::Continue);
    heal.TargetHealthPct = 100.0f;
    heal.SpellIsDirectHeal = false;
    CHECK(JudgeCast(heal) == CastVerdict::Continue);              // a HoT may go on a full target
    heal.SpellIsDirectHeal = true;
    heal.Channeled = true;
    CHECK(JudgeCast(heal) == CastVerdict::Continue);              // channels are not cancelled for hp

    if (Failures)
    {
        std::printf("tick_policy_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("tick_policy_test: ok\n");
    return 0;
}
