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

// Standalone test of the movement fallback geometry (Travel/MoveGeometry.h), see tests/run_engine_test.sh

#include "Travel/MoveGeometry.h"
#include <cmath>
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }
}

int main()
{
    // stones toward +x: 2 distances x (1 + 2 x 2 sides) points, the straight one first for each distance
    std::vector<Point2> stones = ConeStones(0, 0, 100, 0, { 25.0f, 50.0f }, 0.5f, 2);
    CHECK(stones.size() == 10);
    CHECK(Near(stones[0].X, 25.0f) && Near(stones[0].Y, 0.0f));
    CHECK(Near(stones[5].X, 50.0f) && Near(stones[5].Y, 0.0f));
    CHECK(stones[1].Y > 0.0f && stones[2].Y < 0.0f);                       // left then right
    CHECK(std::fabs(stones[3].Y) > std::fabs(stones[1].Y));                // wider second side

    // the bearing follows the goal, not the x axis
    std::vector<Point2> north = ConeStones(10, 10, 10, 200, { 20.0f }, 0.5f, 1);
    CHECK(Near(north[0].X, 10.0f) && Near(north[0].Y, 30.0f));

    // heights: duplicates within tolerance are dropped, the first is kept
    std::vector<float> h = DistinctHeights({ 10.0f, 10.8f, 25.0f, 24.9f, 10.0f, -5.0f }, 1.5f);
    CHECK(h.size() == 3 && Near(h[0], 10.0f) && Near(h[1], 25.0f) && Near(h[2], -5.0f));
    CHECK(DistinctHeights({}, 1.0f).empty());

    // best stone: the one nearest the goal that makes at least minProgress
    std::vector<Point2> cand = { { 20, 0 }, { 40, 5 }, { 35, -30 } };
    CHECK(BestStone(cand, 0, 0, 100, 0, 5.0f) == 1);
    CHECK(BestStone(cand, 0, 0, 100, 0, 70.0f) == -1);                     // nothing gets 70 yd closer
    CHECK(BestStone({}, 0, 0, 100, 0, 5.0f) == -1);
    // a stone that is farther from the goal than the bot never qualifies
    CHECK(BestStone({ { -10, 0 } }, 0, 0, 100, 0, 1.0f) == -1);

    // stand-off: a straight walk to an NPC at x = 50 that ends on it stops 4 yd short, height interpolated
    struct P3 { float x, y, z; };
    std::vector<P3> walk = { { 0, 0, 0 }, { 50, 0, 10 } };
    std::vector<P3> cut = TrimPathToStandoff(walk, 50.0f, 0.0f, 4.0f);
    CHECK(cut.size() == 2 && Near(cut.back().x, 46.0f, 0.05f) && Near(cut.back().z, 9.2f, 0.05f));
    // the last segment only is cut: the middle points stay
    std::vector<P3> bent = { { 0, 0, 0 }, { 20, 0, 0 }, { 50, 0, 0 } };
    cut = TrimPathToStandoff(bent, 50.0f, 0.0f, 4.0f);
    CHECK(cut.size() == 3 && Near(cut[1].x, 20.0f) && Near(cut.back().x, 46.0f, 0.05f));
    // the cut can fall in an earlier segment (a short last segment)
    std::vector<P3> shortEnd = { { 0, 0, 0 }, { 40, 0, 0 }, { 49, 0, 0 }, { 50, 0, 0 } };
    cut = TrimPathToStandoff(shortEnd, 50.0f, 0.0f, 6.0f);
    CHECK(cut.size() == 3 && Near(cut.back().x, 44.0f, 0.05f));
    // an NPC behind a counter: the mesh already ends 3 yd short, and a 2 yd stand-off leaves the path as it is
    std::vector<P3> counter = { { 0, 0, 0 }, { 47, 0, 0 } };
    cut = TrimPathToStandoff(counter, 50.0f, 0.0f, 2.0f);
    CHECK(cut.size() == 2 && Near(cut.back().x, 47.0f));
    // a stand-off equal to the gap is not a trim either
    cut = TrimPathToStandoff(counter, 50.0f, 0.0f, 3.0f);
    CHECK(cut.size() == 2 && Near(cut.back().x, 47.0f));
    // the whole walk lies inside the stand-off (or what is left is under 0.5 yd): no move
    CHECK(TrimPathToStandoff(std::vector<P3>{ { 48, 0, 0 }, { 50, 0, 0 } }, 50.0f, 0.0f, 4.0f).empty());
    CHECK(TrimPathToStandoff(std::vector<P3>{ { 0, 0, 0 }, { 50, 0, 0 } }, 50.0f, 0.0f, 49.8f).empty());
    CHECK(TrimPathToStandoff(std::vector<P3>{ { 0, 0, 0 } }, 50.0f, 0.0f, 4.0f).empty());
    // a path that curves around: the walk back finds the last point at the stand-off, not the nearest one to the end
    std::vector<P3> curve = { { 0, 0, 0 }, { 30, 30, 0 }, { 50, 3, 0 }, { 50, 0, 0 } };
    cut = TrimPathToStandoff(curve, 50.0f, 0.0f, 5.0f);
    CHECK(cut.size() == 3 && Near(std::hypot(cut.back().x - 50.0f, cut.back().y), 5.0f, 0.05f));

    if (Failures)
    {
        std::printf("move_geometry_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("move_geometry_test: ok\n");
    return 0;
}
