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

#ifndef Playerbots_AirFollowGeometry_h__
#define Playerbots_AirFollowGeometry_h__

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// The geometry of party and raid bots following a flying master (Travel/AirFollow): where each bot hovers around the master and which
// points of the master's own flight path it flies along. No core types, tested standalone (tests/air_follow_test.cpp).
namespace Playerbots::AirFollow
{
struct Vec
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

inline float Dist2d(Vec a, Vec b) { return std::hypot(a.X - b.X, a.Y - b.Y); }
inline float Dist3d(Vec a, Vec b) { return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z)); }

/// Where a following bot hovers relative to its master: a stable place for that bot (the seed is its guid), 6 to 16 yd away, mostly behind
/// the master (within about 75 degrees of straight behind), a few yards above or below it, so a raid spreads out like a flock.
inline Vec FormationOffset(uint64_t seed, float masterOrientation, float minRadius = 6.0f, float maxRadius = 16.0f)
{
    auto unit = [seed](uint64_t salt)
    {
        uint64_t h = seed * 0x9E3779B97F4A7C15ull + salt * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 32;
        return float(h & 0xFFFFFFu) / float(0x1000000u);
    };

    float radius = minRadius + (maxRadius - minRadius) * unit(1);
    float angle = masterOrientation + 3.14159265f + (unit(2) - 0.5f) * 2.6f;
    return { std::cos(angle) * radius, std::sin(angle) * radius, (unit(3) - 0.5f) * 8.0f };
}

/// The points of the master's trail a bot flies through to reach it: from the trail point nearest to the bot to the newest one, at least
/// `spacing` yd apart, at most maxPoints (the newest is always kept). The trail is where the master really flew, so it is clear.
inline std::vector<Vec> TrailAhead(std::vector<Vec> const& trail, Vec bot, float spacing, size_t maxPoints)
{
    std::vector<Vec> out;
    if (trail.empty() || maxPoints == 0)
        return out;

    size_t nearest = 0;
    float best = Dist3d(bot, trail[0]);
    for (size_t i = 1; i < trail.size(); ++i)
        if (float d = Dist3d(bot, trail[i]); d < best)
        {
            best = d;
            nearest = i;
        }

    Vec last = bot;
    for (size_t i = nearest + 1; i + 1 < trail.size(); ++i)
        if (Dist3d(last, trail[i]) >= spacing)
        {
            out.push_back(trail[i]);
            last = trail[i];
        }
    if (nearest + 1 < trail.size() || trail.size() == 1)
        out.push_back(trail.back());

    // too many: every k-th, the newest stays
    while (out.size() > maxPoints)
    {
        std::vector<Vec> thinned;
        for (size_t i = 0; i < out.size(); i += 2)
            thinned.push_back(out[i]);
        if (thinned.back().X != out.back().X || thinned.back().Y != out.back().Y || thinned.back().Z != out.back().Z)
            thinned.push_back(out.back());
        out = std::move(thinned);
    }
    return out;
}
}

#endif
