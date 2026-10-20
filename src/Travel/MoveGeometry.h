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

#ifndef Playerbots_MoveGeometry_h__
#define Playerbots_MoveGeometry_h__

#include <algorithm>
#include <cmath>
#include <vector>

// Pure geometry for BotMover's fallbacks (Playerbots.Move.Fallbacks). No core types, tested standalone
// (tests/move_geometry_test.cpp). Ideas from AzerothCore's MoveFarTo (cone-sampled stepping stones when the path to a
// far point is blocked) and SearchForBestPath (try several heights of one destination).
namespace Playerbots
{
struct Point2
{
    float X = 0.0f;
    float Y = 0.0f;
};

/// Candidate stepping stones toward a destination, best guess first: for each distance the straight line, then the
/// bearings rotated by spreadRad, 2 x spreadRad... alternating left and right. distances x (1 + 2 x sides) points
inline std::vector<Point2> ConeStones(float fromX, float fromY, float toX, float toY, std::vector<float> const& distances,
    float spreadRad, int sides)
{
    std::vector<Point2> stones;
    float bearing = std::atan2(toY - fromY, toX - fromX);
    for (float distance : distances)
    {
        stones.push_back({ fromX + std::cos(bearing) * distance, fromY + std::sin(bearing) * distance });
        for (int side = 1; side <= sides; ++side)
            for (float sign : { 1.0f, -1.0f })
            {
                float angle = bearing + sign * spreadRad * float(side);
                stones.push_back({ fromX + std::cos(angle) * distance, fromY + std::sin(angle) * distance });
            }
    }
    return stones;
}

/// Heights of one point worth trying as a destination: the given one first, then the others that are at least
/// tolerance away from every one already kept (invalid heights are skipped by the caller)
inline std::vector<float> DistinctHeights(std::vector<float> const& heights, float tolerance)
{
    std::vector<float> kept;
    for (float height : heights)
    {
        bool duplicate = std::ranges::any_of(kept, [&](float other) { return std::fabs(other - height) < tolerance; });
        if (!duplicate)
            kept.push_back(height);
    }
    return kept;
}

/// Which of the stones is the best next step: it must be closer to the goal than the bot by at least minProgress, and the
/// closest to the goal wins. -1 if none qualifies. Strictly decreasing distance, so repeating it cannot oscillate
inline int BestStone(std::vector<Point2> const& stones, float botX, float botY, float goalX, float goalY, float minProgress)
{
    float now = std::hypot(goalX - botX, goalY - botY);
    int best = -1;
    float bestDist = now - minProgress;
    for (size_t i = 0; i < stones.size(); ++i)
    {
        float d = std::hypot(goalX - stones[i].X, goalY - stones[i].Y);
        if (d <= bestDist)
        {
            bestDist = d;
            best = int(i);
        }
    }
    return best;
}

/// The part of a walking path that ends `standoff` yards (2D) from the target instead of on it: walks back from the end of the
/// path to the first point whose distance to the target reaches standoff, interpolating inside that segment (V has x, y, z:
/// G3D::Vector3). Distance to the target, not length cut from the end: an NPC behind a counter has a path that already ends 2-3
/// yd short of it, and that path is returned untouched (it never gets shorter than the mesh allows). Empty when what is left
/// is shorter than minLength, i.e. the bot is already as close as it should get and does not move.
template <typename V>
std::vector<V> TrimPathToStandoff(std::vector<V> const& points, float targetX, float targetY, float standoff, float minLength = 0.5f)
{
    auto distance = [&](V const& p) { return std::hypot(p.x - targetX, p.y - targetY); };
    auto lengthOf = [](std::vector<V> const& path)
    {
        float total = 0.0f;
        for (size_t i = 1; i < path.size(); ++i)
            total += std::hypot(path[i].x - path[i - 1].x, path[i].y - path[i - 1].y);
        return total;
    };

    if (points.size() < 2)
        return {};
    if (distance(points.back()) >= standoff)
        return points;

    // the last index whose distance to the target still reaches standoff
    int keep = -1;
    for (int i = int(points.size()) - 2; i >= 0; --i)
        if (distance(points[i]) >= standoff)
        {
            keep = i;
            break;
        }
    if (keep < 0)
        return {};

    // the crossing inside segment keep .. keep + 1 (bisection: the distance to a point is not linear along the segment)
    V const& a = points[keep];
    V const& b = points[keep + 1];
    float lo = 0.0f;     // distance(a + lo (b - a)) >= standoff
    float hi = 1.0f;     // distance(a + hi (b - a)) < standoff
    for (int step = 0; step < 24; ++step)
    {
        float mid = (lo + hi) * 0.5f;
        V p = a;
        p.x = a.x + (b.x - a.x) * mid;
        p.y = a.y + (b.y - a.y) * mid;
        if (distance(p) >= standoff)
            lo = mid;
        else
            hi = mid;
    }

    std::vector<V> trimmed(points.begin(), points.begin() + keep + 1);
    V cut = a;
    cut.x = a.x + (b.x - a.x) * lo;
    cut.y = a.y + (b.y - a.y) * lo;
    cut.z = a.z + (b.z - a.z) * lo;
    trimmed.push_back(cut);
    if (lengthOf(trimmed) < minLength)
        return {};
    return trimmed;
}
}

#endif
