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

// Standalone test of the hub clustering (Travel/HubClusters), see tests/run_engine_test.sh

#include "Travel/HubClusters.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

void Town(std::vector<HubPoint>& points, uint32_t map, float x, float y)
{
    // innkeeper, banker, auctioneer and two vendors inside 40 yards
    points.push_back({ map, x, y, 0 });
    points.push_back({ map, x + 10, y, 1 });
    points.push_back({ map, x, y + 15, 2 });
    points.push_back({ map, x - 20, y, 3 });
    points.push_back({ map, x + 5, y - 30, 3 });
}
}

int main()
{
    std::vector<HubPoint> points;
    Town(points, 0, 1000, 1000);          // town A
    Town(points, 0, 3000, 1000);          // town B, far away on the same map
    Town(points, 1, 1000, 1000);          // same coordinates on another map
    points.push_back({ 0, 5000, 5000, 3 });       // lone vendor
    points.push_back({ 0, 5010, 5000, 3 });
    points.push_back({ 0, 5020, 5000, 3 });      // three vendors of one kind: not a town

    auto clusters = BuildHubClusters(points);
    CHECK(clusters.size() == 3);

    int onMap0 = 0;
    for (HubGroup const& c : clusters)
    {
        CHECK(c.Spots == 5);
        CHECK(c.Radius > 0.0f && c.Radius < 60.0f);
        CHECK(c.KindMask == 0b1111);
        if (c.Map == 0)
            ++onMap0;
    }
    CHECK(onMap0 == 2);

    // a point inside a town's claim radius belongs to it, one beyond starts nothing on its own
    points.push_back({ 0, 1100, 1000, 4 });
    clusters = BuildHubClusters(points);
    bool found = false;
    for (HubGroup const& c : clusters)
        if (c.Map == 0 && c.X > 900 && c.X < 1200 && c.Spots == 6 && (c.KindMask & (1u << 4)))
            found = true;
    CHECK(found);

    // hostile members are counted per faction
    std::vector<HubPoint> hostile;
    Town(hostile, 0, 0, 0);
    hostile[0].HostileToAlliance = true;
    hostile[1].HostileToAlliance = true;
    hostile[2].HostileToHorde = true;
    auto h = BuildHubClusters(hostile);
    CHECK(h.size() == 1 && h[0].HostileToAlliance == 2 && h[0].HostileToHorde == 1);

    // the order of the input does not change the result
    std::vector<HubPoint> reversed(points.rbegin(), points.rend());
    CHECK(BuildHubClusters(reversed).size() == BuildHubClusters(points).size());

    // settings
    ClusterSettings strict;
    strict.MinSpots = 6;
    CHECK(BuildHubClusters(points, strict).size() == 1);   // only the town that grew to 6

    CHECK(BuildHubClusters({}).empty());

    if (Failures)
        std::printf("%d check(s) failed\n", Failures);
    else
        std::printf("hub cluster tests passed\n");
    return Failures ? 1 : 0;
}
