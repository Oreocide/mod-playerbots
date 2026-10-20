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

#include "PathRepair.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Player.h"
#include "VMapDefinitions.h"
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr float EyeHeight = 1.5f;
constexpr float KneeHeight = 0.5f;
constexpr float WaistHeight = 1.5f;     // what a jump clears: the standard jump of every race rises 1.64 yd, so an obstacle below 1.5 is jumped and anything higher is gone round
constexpr float LevelDz = 0.35f;        // a segment that rises more is stairs or a slope
constexpr float MaxHopRun = 6.0f;
constexpr float EndTrim = 0.3f;
constexpr size_t MaxSegments = 60;
constexpr int MaxRepairs = 3;
constexpr float Offsets[] = { 3.0f, -3.0f, 6.0f, -6.0f, 10.0f, -10.0f };
constexpr float SnapXY = 1.5f;      // a detour point the mesh only reaches this close to
constexpr float SnapZ = 2.0f;

bool Walkable(PathGenerator const& path)
{
    uint32 type = path.GetPathType();
    if (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_INCOMPLETE))
        return false;
    return (type & PATHFIND_NORMAL) != 0 && path.GetPath().size() >= 2;
}

bool Clear(Player* bot, Movement::PointsArray const& points)
{
    for (size_t i = 0; i + 1 < points.size() && i < MaxSegments; ++i)
        if (ClassifySegment(bot, points[i], points[i + 1]) == SegmentKind::Tall)
            return false;
    return true;
}

bool Detour(Player* bot, G3D::Vector3 const& a, G3D::Vector3 const& b, Movement::PointsArray& out)
{
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float length = std::hypot(dx, dy);
    if (length < 0.5f)
        return false;

    float px = -dy / length;
    float py = dx / length;
    float mx = (a.x + b.x) * 0.5f;
    float my = (a.y + b.y) * 0.5f;
    float mz = (a.z + b.z) * 0.5f;

    for (float offset : Offsets)
    {
        float cx = mx + px * offset;
        float cy = my + py * offset;

        PathGenerator first(bot);
        if (!first.CalculatePath(a.x, a.y, a.z, cx, cy, mz, false) || !Walkable(first))
            continue;

        G3D::Vector3 const& middle = first.GetActualEndPosition();
        if (std::hypot(middle.x - cx, middle.y - cy) > SnapXY || std::fabs(middle.z - mz) > SnapZ + std::fabs(b.z - a.z) * 0.5f)
            continue;

        PathGenerator second(bot);
        if (!second.CalculatePath(middle.x, middle.y, middle.z, b.x, b.y, b.z, false) || !Walkable(second))
            continue;

        // not a long way round: the two halves stay within a few yards of going through the offset point
        if (first.GetPathLength() + second.GetPathLength() > length + 4.0f * std::fabs(offset) + 6.0f)
            continue;

        Movement::PointsArray candidate = first.GetPath();
        candidate.insert(candidate.end(), second.GetPath().begin() + 1, second.GetPath().end());
        if (!Clear(bot, candidate))
            continue;

        out = std::move(candidate);
        return true;
    }
    return false;
}
}

namespace
{
bool BlockedAt(Player* bot, G3D::Vector3 const& a, G3D::Vector3 const& b, float height)
{
    Map* map = bot->GetMap();
    if (!map)
        return false;

    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 0.5f + 2.0f * EndTrim)
        return false;

    float t0 = EndTrim / length;
    float t1 = 1.0f - t0;
    return !map->isInLineOfSight(bot->GetPhaseShift(),
        a.x + dx * t0, a.y + dy * t0, a.z + dz * t0 + height,
        a.x + dx * t1, a.y + dy * t1, a.z + dz * t1 + height, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
}
}

bool SegmentBlockedAtEye(Player* bot, G3D::Vector3 const& a, G3D::Vector3 const& b)
{
    return BlockedAt(bot, a, b, EyeHeight);
}

SegmentKind ClassifySegment(Player* bot, G3D::Vector3 const& a, G3D::Vector3 const& b)
{
    if (BlockedAt(bot, a, b, WaistHeight))
        return SegmentKind::Tall;

    if (std::fabs(b.z - a.z) > LevelDz || std::hypot(b.x - a.x, b.y - a.y) > MaxHopRun)
        return SegmentKind::Clear;

    return BlockedAt(bot, a, b, KneeHeight) ? SegmentKind::Low : SegmentKind::Clear;
}

size_t FirstLowSegment(Player* bot, Movement::PointsArray const& points)
{
    for (size_t i = 0; i + 1 < points.size() && i < MaxSegments; ++i)
    {
        SegmentKind kind = ClassifySegment(bot, points[i], points[i + 1]);
        if (kind == SegmentKind::Low)
            return i;
        if (kind == SegmentKind::Tall)
            break;
    }
    return points.size();
}

bool RepairBlockedPath(Player* bot, Movement::PointsArray& points)
{
    bool changed = false;
    for (int repair = 0; repair < MaxRepairs; ++repair)
    {
        size_t blocked = points.size();
        for (size_t i = 0; i + 1 < points.size() && i < MaxSegments; ++i)
            if (ClassifySegment(bot, points[i], points[i + 1]) == SegmentKind::Tall)
            {
                blocked = i;
                break;
            }

        if (blocked + 1 >= points.size())
            return changed;

        Movement::PointsArray around;
        if (!Detour(bot, points[blocked], points[blocked + 1], around))
            return changed;

        Movement::PointsArray repaired(points.begin(), points.begin() + blocked);
        repaired.insert(repaired.end(), around.begin(), around.end());
        repaired.insert(repaired.end(), points.begin() + blocked + 2, points.end());
        points = std::move(repaired);
        changed = true;
    }
    return changed;
}
}
