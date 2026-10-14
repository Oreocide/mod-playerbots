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

#include "HubClusters.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <unordered_map>

namespace Playerbots
{
namespace
{
using CellKey = int64_t;

CellKey Key(int32_t cx, int32_t cy)
{
    return (int64_t(cx) << 32) ^ int64_t(uint32_t(cy));
}

struct Grid
{
    float CellSize;
    std::unordered_map<CellKey, std::vector<uint32_t>> Cells;

    int32_t Cell(float v) const { return int32_t(std::floor(v / CellSize)); }

    // calls fn(index) for every indexed point within radius of (x, y), radius <= CellSize
    template <typename Fn>
    void Near(std::vector<HubPoint> const& points, float x, float y, float radius, Fn fn) const
    {
        int32_t cx = Cell(x);
        int32_t cy = Cell(y);
        float radiusSq = radius * radius;
        for (int32_t dx = -1; dx <= 1; ++dx)
            for (int32_t dy = -1; dy <= 1; ++dy)
                if (auto itr = Cells.find(Key(cx + dx, cy + dy)); itr != Cells.end())
                    for (uint32_t index : itr->second)
                    {
                        float ddx = points[index].X - x;
                        float ddy = points[index].Y - y;
                        if (ddx * ddx + ddy * ddy <= radiusSq)
                            fn(index);
                    }
    }
};
}

std::vector<HubGroup> BuildHubClusters(std::vector<HubPoint> const& points, ClusterSettings const& settings)
{
    std::vector<HubGroup> clusters;

    std::map<uint32_t, std::vector<uint32_t>> byMap;
    for (uint32_t i = 0; i < points.size(); ++i)
        byMap[points[i].Map].push_back(i);

    float cell = std::max(settings.SeedRadius, settings.ClaimRadius);
    for (auto const& [map, indexes] : byMap)
    {
        Grid grid{ cell, {} };
        for (uint32_t index : indexes)
            grid.Cells[Key(grid.Cell(points[index].X), grid.Cell(points[index].Y))].push_back(index);

        // rate every point by the points around it
        std::vector<std::pair<uint32_t, uint32_t>> rated; // (neighbours, index)
        rated.reserve(indexes.size());
        for (uint32_t index : indexes)
        {
            uint32_t neighbours = 0;
            grid.Near(points, points[index].X, points[index].Y, settings.SeedRadius, [&](uint32_t) { ++neighbours; });
            rated.emplace_back(neighbours, index);
        }

        // densest first, the index breaks ties so the result does not depend on hash order
        std::ranges::sort(rated, [](auto const& a, auto const& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });

        std::vector<bool> claimed(points.size(), false);
        for (auto const& [neighbours, seed] : rated)
        {
            if (claimed[seed])
                continue;

            std::vector<uint32_t> members;
            grid.Near(points, points[seed].X, points[seed].Y, settings.ClaimRadius, [&](uint32_t index)
            {
                if (!claimed[index])
                    members.push_back(index);
            });

            uint32_t kinds = 0;
            for (uint32_t index : members)
                kinds |= 1u << points[index].Kind;

            if (members.size() < settings.MinSpots || uint32_t(std::popcount(kinds)) < settings.MinKinds)
                continue;

            HubGroup cluster;
            cluster.Map = map;
            cluster.KindMask = kinds;
            cluster.Spots = uint32_t(members.size());
            for (uint32_t index : members)
            {
                claimed[index] = true;
                cluster.X += points[index].X;
                cluster.Y += points[index].Y;
                cluster.HostileToAlliance += points[index].HostileToAlliance ? 1 : 0;
                cluster.HostileToHorde += points[index].HostileToHorde ? 1 : 0;
            }
            cluster.X /= float(members.size());
            cluster.Y /= float(members.size());
            for (uint32_t index : members)
            {
                float dx = points[index].X - cluster.X;
                float dy = points[index].Y - cluster.Y;
                cluster.Radius = std::max(cluster.Radius, std::sqrt(dx * dx + dy * dy));
            }

            std::ranges::sort(members);
            cluster.Members = std::move(members);
            clusters.push_back(std::move(cluster));
        }
    }

    return clusters;
}
}
