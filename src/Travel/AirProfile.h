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

#ifndef Playerbots_AirProfile_h__
#define Playerbots_AirProfile_h__

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The altitude profile of an air leg (Playerbots.Air.*): a straight line between two navmesh-snapped points, a climb, a cruise above the
// highest terrain under the line, and a descent onto the landing point. Pure geometry with no core types, tested standalone
// (tests/air_profile_test.cpp). The terrain heights come from the caller (Map::GetHeight probed from above, see AirCorridor).
namespace Playerbots::Air
{
constexpr float UnknownGround = -200000.0f;   // a sample whose height could not be read (grid not loaded): the plan is refused

struct ProfileParams
{
    float SampleYards = 20.0f;        // distance between two ground samples
    float Clearance = 30.0f;          // yards between the bot and the ground once it is away from the ends
    float EndSlack = 1.0f;            // the takeoff and landing points are on the ground: the terrain may read this much above them
    float ClearancePerYard = 0.3f;    // the clearance asked for grows this much per yard of distance from the nearest end, up to Clearance
    float MaxCruiseAboveEnds = 450.0f;   // a leg that needs more height than this over its own ends is refused (a mountain wall)
    std::vector<float> ClimbSlopes = { 0.5f, 0.8f, 1.2f };   // yards up per yard along the line, the gentlest that clears is used
    std::vector<float> DescentSlopes = { 0.4f, 0.7f, 1.0f };
};

struct ProfilePoint
{
    float Distance = 0.0f;   // along the straight line from the takeoff point
    float Z = 0.0f;
};

struct Profile
{
    bool Ok = false;
    std::string Why;                     // when not Ok
    float CruiseZ = 0.0f;
    std::vector<ProfilePoint> Points;    // takeoff, top of the climb, start of the descent, landing point
    bool Transient = false;              // refused for something that is not the terrain (unknown ground), so it is not cached as unsafe
};

/// Height of the profile at a distance along the line
inline float ProfileZ(std::vector<ProfilePoint> const& points, float distance)
{
    if (points.empty())
        return 0.0f;
    if (distance <= points.front().Distance)
        return points.front().Z;
    for (size_t i = 1; i < points.size(); ++i)
        if (distance <= points[i].Distance)
        {
            float span = points[i].Distance - points[i - 1].Distance;
            float t = span > 0.0f ? (distance - points[i - 1].Distance) / span : 1.0f;
            return points[i - 1].Z + (points[i].Z - points[i - 1].Z) * t;
        }
    return points.back().Z;
}

/// ground[i] is the terrain height under the line at distance i x params.SampleYards (the last sample may be shorter than a full step:
/// it is at `length`). startZ / endZ are the heights of the navmesh-snapped takeoff and landing points.
inline Profile PlanProfile(float startZ, float endZ, float length, std::vector<float> const& ground, ProfileParams const& params = {})
{
    Profile result;
    if (length < params.SampleYards * 2.0f || ground.size() < 3)
    {
        result.Why = "too short for an air leg";
        return result;
    }

    float highest = std::max(startZ, endZ);
    for (float height : ground)
    {
        if (height <= UnknownGround)
        {
            result.Why = "terrain height unknown on part of the corridor";
            result.Transient = true;
            return result;
        }
        highest = std::max(highest, height);
    }

    float cruise = highest + params.Clearance;
    if (cruise - std::max(startZ, endZ) > params.MaxCruiseAboveEnds)
    {
        result.Why = "the terrain under the line is too high above the takeoff and landing points";
        return result;
    }

    auto sampleDistance = [&](size_t i) { return std::min(length, float(i) * params.SampleYards); };

    for (float climbSlope : params.ClimbSlopes)
    {
        for (float descentSlope : params.DescentSlopes)
        {
            float climbEnd = (cruise - startZ) / climbSlope;
            float descentStart = length - (cruise - endZ) / descentSlope;
            if (climbEnd > descentStart)
                continue;   // not enough line to climb and come down at these slopes

            std::vector<ProfilePoint> points = { { 0.0f, startZ }, { climbEnd, cruise }, { descentStart, cruise }, { length, endZ } };
            bool clear = true;
            for (size_t i = 0; i < ground.size() && clear; ++i)
            {
                float d = sampleDistance(i);
                float fromEnd = std::min(d, length - d);
                float need = std::min(params.Clearance, params.ClearancePerYard * fromEnd) - params.EndSlack;
                if (ProfileZ(points, d) - ground[i] < need)
                    clear = false;
            }
            if (!clear)
                continue;

            result.Ok = true;
            result.CruiseZ = cruise;
            result.Points = std::move(points);
            return result;
        }
    }

    result.Why = "no climb and descent slope clears the terrain under the line";
    return result;
}
}

#endif
