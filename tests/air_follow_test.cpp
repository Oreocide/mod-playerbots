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

// Standalone test of the air follow geometry (Travel/AirFollowGeometry.h), see tests/run_engine_test.sh

#include "Travel/AirFollowGeometry.h"
#include <cmath>
#include <cstdio>

using namespace Playerbots::AirFollow;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    // every bot's place is 6 to 16 yd from the master, behind it, within 4 yd above or below, and stable
    for (uint64_t seed = 1; seed <= 200; ++seed)
    {
        float facing = 0.7f;
        Vec offset = FormationOffset(seed, facing);
        float radius = std::hypot(offset.X, offset.Y);
        CHECK(radius >= 6.0f - 0.01f && radius <= 16.0f + 0.01f);
        CHECK(std::fabs(offset.Z) <= 4.0f + 0.01f);
        // behind: the dot product with the facing direction is negative
        CHECK(offset.X * std::cos(facing) + offset.Y * std::sin(facing) < 0.0f);
        Vec again = FormationOffset(seed, facing);
        CHECK(again.X == offset.X && again.Y == offset.Y && again.Z == offset.Z);
    }
    // bots differ: 40 raid members do not all hover in one place
    Vec a = FormationOffset(1, 0.0f), b = FormationOffset(2, 0.0f);
    CHECK(a.X != b.X || a.Y != b.Y);

    // a trail along x: the bot far behind flies through the points ahead of it, thinned by spacing, the newest last
    std::vector<Vec> trail;
    for (int i = 0; i <= 20; ++i)
        trail.push_back({ float(i) * 10.0f, 0.0f, 100.0f });
    std::vector<Vec> path = TrailAhead(trail, { -30.0f, 0.0f, 100.0f }, 40.0f, 8);
    CHECK(!path.empty() && path.size() <= 8);
    CHECK(path.back().X == 200.0f);
    for (size_t i = 1; i < path.size(); ++i)
        CHECK(path[i].X > path[i - 1].X);
    // a bot next to the newest point has only that one to fly to
    std::vector<Vec> near = TrailAhead(trail, { 195.0f, 0.0f, 100.0f }, 40.0f, 8);
    CHECK(near.size() == 1 && near[0].X == 200.0f);
    // a bot mid-trail skips what is behind it
    std::vector<Vec> mid = TrailAhead(trail, { 100.0f, 0.0f, 100.0f }, 40.0f, 8);
    CHECK(!mid.empty() && mid.front().X > 100.0f && mid.back().X == 200.0f);
    // the limit keeps the newest
    std::vector<Vec> few = TrailAhead(trail, { -30.0f, 0.0f, 100.0f }, 10.0f, 3);
    CHECK(few.size() <= 3 && few.back().X == 200.0f);
    // empty and single-point trails
    CHECK(TrailAhead({}, { 0, 0, 0 }, 10.0f, 4).empty());
    std::vector<Vec> one = TrailAhead({ { 5.0f, 5.0f, 5.0f } }, { 0, 0, 0 }, 10.0f, 4);
    CHECK(one.size() == 1 && one[0].X == 5.0f);

    if (Failures)
    {
        std::printf("air_follow_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("air_follow_test: ok\n");
    return 0;
}
