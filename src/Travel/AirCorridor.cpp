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

#include "AirCorridor.h"
#include "AirProfile.h"
#include "DB2Stores.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "StringFormat.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_map>

namespace Playerbots
{
namespace AirCorridor
{
namespace
{
constexpr float ProbeAboveEnds = 900.0f;      // the terrain is probed from this far above the higher end
constexpr float ProbeBelowEnds = 400.0f;      // and may be this far below the lower end
constexpr float PathStepYards = 40.0f;        // distance between the points of the path handed to the spline
constexpr size_t CacheLimit = 8192;

std::mutex CacheLock;
std::unordered_map<uint64, Plan> Cache;

// the same pair of ends within 10 yards is the same corridor
uint64 KeyOf(uint32 map, Position const& from, Position const& to)
{
    auto cell = [](float v) { return uint64(uint32(int32(std::lround(v / 10.0f)))); };
    uint64 key = map;
    for (uint64 part : { cell(from.GetPositionX()), cell(from.GetPositionY()), cell(to.GetPositionX()), cell(to.GetPositionY()) })
        key = key * 0x9E3779B97F4A7C15ull + part + 1;
    return key;
}
}

void ClearCache()
{
    std::lock_guard lock(CacheLock);
    Cache.clear();
}

Plan PlanLeg(Player* bot, Position const& from, Position const& to)
{
    Plan plan;
    Map* map = bot ? bot->GetMap() : nullptr;
    if (!map || map->Instanceable())
    {
        plan.Why = "not on a continent";
        return plan;
    }

    uint64 key = KeyOf(map->GetId(), from, to);
    {
        std::lock_guard lock(CacheLock);
        auto itr = Cache.find(key);
        if (itr != Cache.end())
            return itr->second;
    }

    float length = from.GetExactDist2d(to);
    Air::ProfileParams params;
    size_t steps = size_t(std::ceil(length / params.SampleYards));
    if (steps < 3)
    {
        plan.Why = "too short for an air leg";
        return plan;     // the length is a static fact too, but it is cheap to recompute
    }

    float dx = (to.GetPositionX() - from.GetPositionX()) / length;
    float dy = (to.GetPositionY() - from.GetPositionY()) / length;
    auto pointAt = [&](float distance) { return std::pair<float, float>(from.GetPositionX() + dx * distance, from.GetPositionY() + dy * distance); };

    // grids first: the line may only use what is loaded, plus a few grids this leg is allowed to load
    std::set<std::pair<int32, int32>> unloaded;
    for (size_t i = 0; i <= steps; ++i)
    {
        auto [x, y] = pointAt(std::min(length, float(i) * params.SampleYards));
        if (!map->IsGridLoaded(x, y))
            unloaded.insert({ int32(std::floor(x / SIZE_OF_GRIDS)), int32(std::floor(y / SIZE_OF_GRIDS)) });
    }
    if (unloaded.size() > sPlayerbotsConfig.AirMaxGridLoads)
    {
        plan.Transient = true;
        plan.Why = Trinity::StringFormat("the line crosses {} grids that are not loaded (at most {} may be loaded for it)", unloaded.size(), sPlayerbotsConfig.AirMaxGridLoads);
        return plan;
    }
    for (size_t i = 0; i <= steps && !unloaded.empty(); ++i)
    {
        auto [x, y] = pointAt(std::min(length, float(i) * params.SampleYards));
        if (!map->IsGridLoaded(x, y))
            map->LoadGrid(x, y);
    }

    float highEnd = std::max(from.GetPositionZ(), to.GetPositionZ());
    float lowEnd = std::min(from.GetPositionZ(), to.GetPositionZ());
    float probeZ = highEnd + ProbeAboveEnds;
    float searchDistance = probeZ - (lowEnd - ProbeBelowEnds);

    std::vector<float> ground;
    ground.reserve(steps + 1);
    for (size_t i = 0; i <= steps; ++i)
    {
        auto [x, y] = pointAt(std::min(length, float(i) * params.SampleYards));
        float height = map->GetHeight(bot->GetPhaseShift(), x, y, probeZ, true, searchDistance);
        if (height <= INVALID_HEIGHT)
            height = lowEnd - 50.0f;    // no terrain at all under this point (open sea past the edge): the lowest of the ends counts as the ground
        ground.push_back(height);

        // every point of the line must be an area where flying mounts work, or the mount aura is removed in the air
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(map->GetAreaId(bot->GetPhaseShift(), x, y, height + 1.0f));
        if (!area || !area->GetMountFlags().HasFlag(AreaMountFlags::AllowFlyingMounts))
        {
            plan.Why = Trinity::StringFormat("area {} at ({:.0f}, {:.0f}) does not allow flying", area ? area->ID : 0, x, y);
            std::lock_guard lock(CacheLock);
            if (Cache.size() > CacheLimit)
                Cache.clear();
            Cache[key] = plan;
            return plan;
        }
    }

    Air::Profile profile = Air::PlanProfile(from.GetPositionZ(), to.GetPositionZ(), length, ground, params);
    plan.Why = profile.Why;
    plan.Transient = profile.Transient;
    if (profile.Ok)
    {
        plan.CruiseZ = profile.CruiseZ;
        size_t points = size_t(std::ceil(length / PathStepYards));
        for (size_t i = 0; i <= points; ++i)
        {
            float distance = std::min(length, float(i) * PathStepYards);
            auto [x, y] = pointAt(distance);
            plan.Path.emplace_back(x, y, Air::ProfileZ(profile.Points, distance));
        }
        // the ends are the navmesh points themselves
        plan.Path.front() = from;
        plan.Path.back() = to;

        // two points less than a yard apart make the spline validator refuse the whole path (a length of 800.03 yd leaves the last two
        // points 0.03 apart): the point before the landing goes
        while (plan.Path.size() > 2 && plan.Path[plan.Path.size() - 2].GetExactDist(plan.Path.back()) < 1.0f)
            plan.Path.erase(plan.Path.end() - 2);

        // the ground samples are 20 yards apart: a tower between two of them is only found by a ray along the flight path itself
        for (size_t i = 1; i < plan.Path.size() && plan.Why.empty(); ++i)
        {
            Position const& a = plan.Path[i - 1];
            Position const& b = plan.Path[i];
            if (!map->isInLineOfSight(bot->GetPhaseShift(), a.GetPositionX(), a.GetPositionY(), a.GetPositionZ() + 2.0f,
                b.GetPositionX(), b.GetPositionY(), b.GetPositionZ() + 2.0f, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                plan.Why = Trinity::StringFormat("something solid between ({:.0f}, {:.0f}, {:.0f}) and ({:.0f}, {:.0f}, {:.0f})",
                    a.GetPositionX(), a.GetPositionY(), a.GetPositionZ(), b.GetPositionX(), b.GetPositionY(), b.GetPositionZ());
        }
        plan.Ok = plan.Why.empty();
        if (!plan.Ok)
            plan.Path.clear();
    }

    // only a verdict about the terrain is kept, never a path: a path is checked from where one bot stood, at its height
    if (!plan.Ok && !plan.Transient)
    {
        std::lock_guard lock(CacheLock);
        if (Cache.size() > CacheLimit)
            Cache.clear();
        Cache[key] = plan;
    }
    return plan;
}
}
}
