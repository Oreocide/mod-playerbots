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

#ifndef Playerbots_LegRules_h__
#define Playerbots_LegRules_h__

// Long routes in legs (Playerbots.Move.LegYards, Playerbots.Bg.LegYards; pure, tested in tests/bg_rules_test.cpp).
// The core's smooth path fails outright once it needs 74 points of 4 yd (PathGenerator::FindSmoothPath returns DT_FAILURE at
// MAX_POINT_PATH_LENGTH, about 296 yd of route; BuildPointPath then makes a shortcut marked NOPATH, type 10), so a far target gets no
// path at all. The route is found once with the straight path (its corners only), and the bot walks it as ordinary smooth mesh paths
// of at most a leg each, corner by corner

#include <algorithm>
#include <cmath>
#include <vector>

namespace Playerbots::LegRules
{
struct Point3
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct LegGoal
{
    bool Use = false;       // false: nothing ahead worth a leg (the route is too short, or makes too little ground)
    bool Grounded = true;   // false: a point inside a long segment, its height is a guess for the caller to put on the ground
    Point3 At;
};

/// Where the next leg ends along route (route[0] is where the walker stands): the farthest corner within legYards of route length,
/// when it is at least minGain away; otherwise (the next corner lies beyond the leg) the point where legYards of route end, inside
/// that next segment (Grounded false: its height is the segment's, the caller puts it on the ground). Nothing minGain away: no leg
inline LegGoal PickLeg(std::vector<Point3> const& route, float legYards, float minGain)
{
    LegGoal leg;
    if (route.size() < 2 || legYards <= 0.0f)
        return leg;

    auto flat = [](Point3 const& a, Point3 const& b) { return std::hypot(b.X - a.X, b.Y - a.Y); };
    float walked = 0.0f;
    size_t best = 0;
    for (size_t i = 1; i < route.size(); ++i)
    {
        float next = walked + flat(route[i - 1], route[i]);
        if (next > legYards)
            break;
        walked = next;
        best = i;
    }

    if (best > 0 && flat(route[0], route[best]) >= minGain)
    {
        leg.At = route[best];
        leg.Use = true;
        return leg;
    }
    // the whole route is shorter than minGain
    if (best + 1 >= route.size())
        return leg;

    // the rest of the leg inside the next segment
    Point3 const& from = route[best];
    Point3 const& to = route[best + 1];
    float length = flat(from, to);
    if (length <= 0.0f)
        return leg;
    float t = std::min(1.0f, (legYards - walked) / length);
    Point3 at = { from.X + (to.X - from.X) * t, from.Y + (to.Y - from.Y) * t, from.Z + (to.Z - from.Z) * t };
    if (flat(route[0], at) < minGain)
        return leg;
    leg.At = at;
    leg.Grounded = false;
    leg.Use = true;
    return leg;
}
}

#endif
