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

// Standalone test of the positioning geometry (Combat/Positioning.h), see tests/run_engine_test.sh

#include "Combat/Positioning.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
bool Near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }
}

int main()
{
    // angles
    CHECK(Near(NormalizeAngle(3.0f * Pi), Pi) || Near(NormalizeAngle(3.0f * Pi), -Pi));
    CHECK(Near(NormalizeAngle(-2.5f * Pi), -0.5f * Pi));

    // front arc: a target facing +x, a bot at +x is in front, at -x behind, at +y on the side
    Pt boss{ 0, 0 };
    CHECK(InFrontArc(boss, 0.0f, { 5, 0 }, Pi / 4.0f));
    CHECK(!InFrontArc(boss, 0.0f, { -5, 0 }, Pi / 4.0f));
    CHECK(!InFrontArc(boss, 0.0f, { 0, 5 }, Pi / 4.0f));
    CHECK(InFrontArc(boss, Pi, { -5, 0.5f }, Pi / 4.0f));                       // orientation wraps

    // behind point
    Pt behind = BehindPoint(boss, 0.0f, 3.0f);
    CHECK(Near(behind.X, -3.0f) && Near(behind.Y, 0.0f));
    CHECK(!InFrontArc(boss, 0.0f, behind, Pi / 4.0f));

    // tank face: the group is at -x, so the tank goes to +x and the boss faces +x, away from the group
    Pt tank = TankFacePoint({ 10, 0 }, { 0, 0 }, 4.0f);
    CHECK(Near(tank.X, 14.0f) && Near(tank.Y, 0.0f));

    // spread: 3 slots around bearing 0 at range 20: the middle one on the bearing, the others symmetric
    Pt mid = SpreadPoint(boss, 0.0f, 1, 3, 0.4f, 20.0f);
    Pt lo = SpreadPoint(boss, 0.0f, 0, 3, 0.4f, 20.0f);
    Pt hi = SpreadPoint(boss, 0.0f, 2, 3, 0.4f, 20.0f);
    CHECK(Near(mid.X, 20.0f) && Near(mid.Y, 0.0f));
    CHECK(Near(lo.X, hi.X) && Near(lo.Y, -hi.Y) && hi.Y > 0.0f);
    CHECK(Near(std::hypot(hi.X, hi.Y), 20.0f));
    Pt single = SpreadPoint(boss, 1.0f, 0, 1, 0.4f, 10.0f);
    CHECK(Near(single.X, std::cos(1.0f) * 10.0f) && Near(single.Y, std::sin(1.0f) * 10.0f));

    // safe spots: candidates avoid the hazard, the pick leaves it by the shortest way
    Pt bot{ 0, 0 };
    std::vector<Hazard> fire = { { { 0, 0 }, 6.0f } };
    std::vector<Pt> cands = SafeSpotCandidates(bot, fire, { 4, 8, 12 }, 16, 1.0f);
    CHECK(!cands.empty());
    for (Pt const& c : cands)
        CHECK(std::hypot(c.X, c.Y) >= 7.0f);                                    // 6 radius + 1 margin
    FleeHistory history;
    int pick = PickSafeSpot(cands, bot, fire, history, 1000, {}, 3.0f);
    CHECK(pick >= 0);
    CHECK(Near(std::hypot(cands[pick].X, cands[pick].Y), 8.0f, 0.1f));          // the 8 yd ring is the nearest clear one

    // the next flee may not turn around within 5 s
    float firstAngle = std::atan2(cands[pick].Y, cands[pick].X);
    NoteFlee(history, firstAngle, 1000);
    for (uint32_t at : { 1500u, 5500u })
    {
        int again = PickSafeSpot(cands, bot, fire, history, at, {}, 3.0f);
        CHECK(again >= 0);
        float angle = std::atan2(cands[again].Y, cands[again].X);
        float diffToReverse = std::fabs(NormalizeAngle(angle - NormalizeAngle(firstAngle + Pi)));
        if (at == 1500u)
            CHECK(diffToReverse >= Pi / 3.0f);
    }
    // after 5 s a reversal is allowed again: the same best candidate comes back
    CHECK(PickSafeSpot(cands, bot, fire, history, 6100, {}, 3.0f) == pick);

    // a spot claimed by another bot is skipped
    int claimedPick = PickSafeSpot(cands, bot, fire, FleeHistory{}, 1000, { cands[pick] }, 3.0f);
    CHECK(claimedPick >= 0 && claimedPick != pick);
    CHECK(std::hypot(cands[claimedPick].X - cands[pick].X, cands[claimedPick].Y - cands[pick].Y) >= 3.0f);

    // nothing to pick from
    CHECK(PickSafeSpot({}, bot, fire, history, 0, {}, 3.0f) == -1);

    if (Failures)
    {
        std::printf("positioning_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("positioning_test: ok\n");
    return 0;
}
