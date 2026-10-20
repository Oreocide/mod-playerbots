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

// Standalone test of the air leg altitude profile (Travel/AirProfile.h), see tests/run_engine_test.sh

#include "Travel/AirProfile.h"
#include <cmath>
#include <cstdio>

using namespace Playerbots::Air;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

std::vector<float> Flat(float length, float height, float step = 20.0f)
{
    std::vector<float> ground;
    for (float d = 0.0f; d < length; d += step)
        ground.push_back(height);
    ground.push_back(height);
    return ground;
}
}

int main()
{
    // flat ground 800 yd: climbs to ground + clearance, cruises, comes down
    Profile flat = PlanProfile(100.0f, 100.0f, 800.0f, Flat(800.0f, 100.0f));
    CHECK(flat.Ok && std::fabs(flat.CruiseZ - 130.0f) < 0.01f && flat.Points.size() == 4);
    if (flat.Ok)
    {
        CHECK(std::fabs(flat.Points.front().Z - 100.0f) < 0.01f && std::fabs(flat.Points.back().Z - 100.0f) < 0.01f);
        CHECK(std::fabs(flat.Points.back().Distance - 800.0f) < 0.01f);
    }
    else
        return 1;

    // a ridge in the middle raises the whole cruise, never below ridge + clearance
    std::vector<float> ridge = Flat(800.0f, 100.0f);
    ridge[20] = 250.0f;
    Profile over = PlanProfile(100.0f, 100.0f, 800.0f, ridge);
    CHECK(over.Ok && over.CruiseZ >= 280.0f - 0.01f);
    for (size_t i = 5; i + 5 < ridge.size(); ++i)    // away from the ends (100 yd and more): the full clearance
        CHECK(ProfileZ(over.Points, std::min(800.0f, float(i) * 20.0f)) - ridge[i] >= 30.0f - 0.01f);

    // a wall right after takeoff needs a steeper climb than the gentlest slope
    std::vector<float> wall = Flat(800.0f, 100.0f);
    wall[3] = 140.0f;      // 60 yd out, 40 yd higher than the takeoff
    Profile steep = PlanProfile(100.0f, 100.0f, 800.0f, wall);
    CHECK(steep.Ok);
    CHECK(ProfileZ(steep.Points, 60.0f) - 140.0f >= 17.0f);
    CHECK(steep.Ok && steep.Points[1].Distance < 800.0f / 2.0f);
    // the gentlest slope that clears is used: flat ground climbs at 0.5
    CHECK(flat.Ok && std::fabs(flat.Points[1].Distance - 60.0f) < 0.01f);

    // a mountain wall far above both ends is refused (not cached as a transient)
    std::vector<float> mountain = Flat(800.0f, 100.0f);
    mountain[20] = 900.0f;
    Profile refused = PlanProfile(100.0f, 100.0f, 800.0f, mountain);
    CHECK(!refused.Ok && !refused.Transient);

    // unknown ground (a grid that is not loaded) is a transient refusal, not a verdict on the terrain
    std::vector<float> unknown = Flat(800.0f, 100.0f);
    unknown[10] = UnknownGround;
    Profile notLoaded = PlanProfile(100.0f, 100.0f, 800.0f, unknown);
    CHECK(!notLoaded.Ok && notLoaded.Transient);

    // too short a line, and a landing point far below the takeoff
    CHECK(!PlanProfile(100.0f, 100.0f, 30.0f, Flat(30.0f, 100.0f)).Ok);
    Profile down = PlanProfile(300.0f, 100.0f, 900.0f, Flat(900.0f, 100.0f));
    CHECK(!down.Ok || (std::fabs(down.Points.front().Z - 300.0f) < 0.01f && std::fabs(down.Points.back().Z - 100.0f) < 0.01f));

    // a line too short to climb and descend at any slope is refused
    std::vector<float> highEnds = Flat(120.0f, 100.0f);
    highEnds[3] = 400.0f;
    CHECK(!PlanProfile(100.0f, 100.0f, 120.0f, highEnds).Ok);

    // ProfileZ clamps outside the line
    CHECK(std::fabs(ProfileZ(flat.Points, -5.0f) - 100.0f) < 0.01f && std::fabs(ProfileZ(flat.Points, 5000.0f) - 100.0f) < 0.01f);

    if (Failures)
    {
        std::printf("air_profile_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("air_profile_test: ok\n");
    return 0;
}
