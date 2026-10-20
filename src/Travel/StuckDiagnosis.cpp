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

#include "StuckDiagnosis.h"
#include "GameObject.h"
#include "Map.h"
#include "MoveSpline.h"
#include "PathRepair.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "StringFormat.h"
#include "VMapDefinitions.h"

namespace Playerbots
{
namespace
{
constexpr float EyeHeight = 1.5f;
constexpr float ObjectRange = 8.0f;

char const* Los(Map* map, PhaseShift const& phase, Position const& a, Position const& b)
{
    return map->isInLineOfSight(phase, a.GetPositionX(), a.GetPositionY(), a.GetPositionZ() + EyeHeight,
        b.GetPositionX(), b.GetPositionY(), b.GetPositionZ() + EyeHeight, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing) ? "clear" : "blocked";
}
}

PathVerdict JudgePath(Player* bot, Movement::PointsArray const& points)
{
    PathVerdict verdict;
    Map* map = bot->GetMap();
    if (!map)
        return verdict;

    constexpr float KneeHeight = 0.5f;
    constexpr size_t MaxSegments = 60;
    PhaseShift const& phase = bot->GetPhaseShift();
    size_t last = std::min(points.size(), MaxSegments + 1);
    auto describe = [&points](size_t i, char const* what)
    {
        G3D::Vector3 const& a = points[i];
        G3D::Vector3 const& b = points[i + 1];
        return Trinity::StringFormat("segment {} of {} {}: ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})", i + 1, points.size() - 1, what, a.x, a.y, a.z, b.x, b.y, b.z);
    };

    for (size_t i = 0; i + 1 < last; ++i)
    {
        G3D::Vector3 const& a = points[i];
        G3D::Vector3 const& b = points[i + 1];
        float run = std::hypot(b.x - a.x, b.y - a.y);
        float dz = std::fabs(b.z - a.z);
        if (dz > SteepMinDz && dz > run * SteepSlope)
        {
            verdict.Result = PathVerdict::Kind::Steep;
            verdict.Text = describe(i, "is a cliff");
            return verdict;
        }

        if (run < 0.5f)
            continue;

        if (SegmentBlockedAtEye(bot, a, b))
        {
            verdict.Result = PathVerdict::Kind::EyeBlock;
            verdict.Text = describe(i, "blocked at eye height");
            return verdict;
        }

        if (verdict.Result == PathVerdict::Kind::Clear
            && !map->isInLineOfSight(phase, a.x, a.y, a.z + KneeHeight, b.x, b.y, b.z + KneeHeight, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            verdict.Result = PathVerdict::Kind::KneeOnly;
    }
    return verdict;
}

std::string DescribeStuck(Player* bot, Position const* reference)
{
    Map* map = bot->GetMap();
    if (!map)
        return "no map";

    PhaseShift const& phase = bot->GetPhaseShift();
    Position here = bot->GetPosition();
    std::string text;

    if (reference)
        text += Trinity::StringFormat("los={} ", Los(map, phase, *reference, here));

    float ground = map->GetHeight(phase, here.GetPositionX(), here.GetPositionY(), here.GetPositionZ() + 2.0f, true, 60.0f);
    if (ground > INVALID_HEIGHT)
        text += Trinity::StringFormat("ground={:.1f} dz={:.1f} ", ground, here.GetPositionZ() - ground);
    else
        text += "ground=none ";

    if (reference)
    {
        PathGenerator path(bot);
        bool calculated = path.CalculatePath(reference->GetPositionX(), reference->GetPositionY(), reference->GetPositionZ(), false);
        float straight = std::max(here.GetExactDist(*reference), 0.1f);
        if (calculated)
            text += Trinity::StringFormat("back={}/{:.1f} ", uint32(path.GetPathType()), path.GetPathLength() / straight);
        else
            text += Trinity::StringFormat("back={}/none ", uint32(path.GetPathType()));
    }

    if (bot->movespline && !bot->movespline->Finalized())
    {
        G3D::Vector3 end = bot->movespline->FinalDestination();
        Position endPosition(end.x, end.y, end.z);
        text += Trinity::StringFormat("spline=moving to ({:.0f}, {:.0f}, {:.0f}) los={} ", end.x, end.y, end.z, Los(map, phase, here, endPosition));
    }
    else
        text += "spline=idle ";

    if (GameObject* object = bot->FindNearestGameObjectWithOptions(ObjectRange, { }))
        text += Trinity::StringFormat("object={} {} at {:.1f} yd", object->GetEntry(), object->GetName(), bot->GetExactDist(object));
    else
        text += "object=none";

    return text;
}
}
