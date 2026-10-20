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

#ifndef Playerbots_Positioning_h__
#define Playerbots_Positioning_h__

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Where a bot should stand in a fight, on plain numbers. No core types, tested standalone (tests/positioning_test.cpp).
// Ideas from AzerothCore's SetBehind/RearFlank/TankFace/Disperse and FleeManager (candidate points, 5 s no-reversal rule,
// spots claimed by group members). Every point returned here is only a wish: the glue (Combat/GroupCombat.cpp) snaps it to
// the navmesh with BotMover::SnapToMesh before any bot moves there.
namespace Playerbots
{
constexpr float Pi = 3.14159265358979f;

struct Pt
{
    float X = 0.0f;
    float Y = 0.0f;
};

inline float NormalizeAngle(float angle)
{
    while (angle > Pi)
        angle -= 2.0f * Pi;
    while (angle < -Pi)
        angle += 2.0f * Pi;
    return angle;
}

inline Pt PointAt(Pt origin, float angle, float distance)
{
    return { origin.X + std::cos(angle) * distance, origin.Y + std::sin(angle) * distance };
}

/// The bot is inside the arc the target faces (halfArc each side of its orientation): a melee attacker standing there is hit
/// by the front attacks (cleaves, breaths)
inline bool InFrontArc(Pt target, float targetOrientation, Pt bot, float halfArc)
{
    float toBot = std::atan2(bot.Y - target.Y, bot.X - target.X);
    return std::fabs(NormalizeAngle(toBot - targetOrientation)) < halfArc;
}

/// Standing point behind the target at `distance`
inline Pt BehindPoint(Pt target, float targetOrientation, float distance)
{
    return PointAt(target, targetOrientation + Pi, distance);
}

/// Where a tank stands so the enemy turns its front away from the group: on the far side of the enemy as seen from the group
inline Pt TankFacePoint(Pt enemy, Pt groupCenter, float distance)
{
    float away = std::atan2(enemy.Y - groupCenter.Y, enemy.X - groupCenter.X);
    return PointAt(enemy, away, distance);
}

/// A ranged slot: `slotCount` bots share an arc around the target, centred on `bearing` (the direction from the target to
/// where the group stands), `spreadRad` apart, at `range`
inline Pt SpreadPoint(Pt target, float bearing, int slot, int slotCount, float spreadRad, float range)
{
    float offset = (float(slot) - float(slotCount - 1) * 0.5f) * spreadRad;
    return PointAt(target, bearing + offset, range);
}

struct Hazard
{
    Pt Center;
    float Radius = 0.0f;
};

/// The last flee directions, so a bot does not run back and forth between two hazards
struct FleeHistory
{
    struct Entry
    {
        float Angle = 0.0f;
        uint32_t AtMs = 0;
        bool Used = false;
    };
    Entry Last;
};

inline void NoteFlee(FleeHistory& history, float angle, uint32_t nowMs)
{
    history.Last = { angle, nowMs, true };
}

constexpr uint32_t NoReverseMs = 5000;
constexpr float ReverseCone = Pi / 3.0f;    // a flee within this of the opposite direction counts as reversing

/// Candidate standing points on rings around the bot, outside every hazard by `margin`
inline std::vector<Pt> SafeSpotCandidates(Pt bot, std::vector<Hazard> const& hazards, std::vector<float> const& rings, int samples,
    float margin)
{
    std::vector<Pt> out;
    for (float ring : rings)
        for (int i = 0; i < samples; ++i)
        {
            Pt p = PointAt(bot, 2.0f * Pi * float(i) / float(samples), ring);
            bool inside = std::ranges::any_of(hazards, [&](Hazard const& h)
            {
                return std::hypot(p.X - h.Center.X, p.Y - h.Center.Y) < h.Radius + margin;
            });
            if (!inside)
                out.push_back(p);
        }
    return out;
}

/// The best candidate: short moves win, standing well clear of the hazards wins, a reversal of the last flee within
/// NoReverseMs is not allowed, and spots another bot claimed (within claimRadius) are skipped. -1 if none is left
inline int PickSafeSpot(std::vector<Pt> const& candidates, Pt bot, std::vector<Hazard> const& hazards, FleeHistory const& history,
    uint32_t nowMs, std::vector<Pt> const& claimed, float claimRadius)
{
    int best = -1;
    float bestScore = 0.0f;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        Pt const& p = candidates[i];

        if (history.Last.Used && uint32_t(nowMs - history.Last.AtMs) < NoReverseMs)
        {
            float angle = std::atan2(p.Y - bot.Y, p.X - bot.X);
            float reverse = NormalizeAngle(history.Last.Angle + Pi);
            if (std::fabs(NormalizeAngle(angle - reverse)) < ReverseCone)
                continue;
        }

        if (std::ranges::any_of(claimed, [&](Pt const& c) { return std::hypot(p.X - c.X, p.Y - c.Y) < claimRadius; }))
            continue;

        float clearance = 1000.0f;
        for (Hazard const& h : hazards)
            clearance = std::min(clearance, std::hypot(p.X - h.Center.X, p.Y - h.Center.Y) - h.Radius);

        // clearance counts up to 4 yards past the hazard edge; beyond that a shorter run is better
        float score = std::min(clearance, 4.0f) - std::hypot(p.X - bot.X, p.Y - bot.Y);
        if (best < 0 || score > bestScore)
        {
            best = int(i);
            bestScore = score;
        }
    }
    return best;
}
}

#endif
