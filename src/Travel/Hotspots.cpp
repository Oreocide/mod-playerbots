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

#include "Hotspots.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace Playerbots
{
uint32_t HotspotRegistry::Add(Hotspot hotspot)
{
    std::unique_lock lock(_mutex);
    hotspot.Id = _nextId++;
    _hotspots.push_back(std::move(hotspot));
    return _hotspots.back().Id;
}

bool HotspotRegistry::Remove(uint32_t id)
{
    std::unique_lock lock(_mutex);
    return std::erase_if(_hotspots, [id](Hotspot const& h) { return h.Id == id; }) > 0;
}

void HotspotRegistry::RemoveOwner(uint64_t owner)
{
    if (!owner)
        return;

    std::unique_lock lock(_mutex);
    std::erase_if(_hotspots, [owner](Hotspot const& h) { return h.Owner == owner; });
}

void HotspotRegistry::ClearDerived()
{
    std::unique_lock lock(_mutex);
    std::erase_if(_hotspots, [](Hotspot const& h) { return h.Owner == 0; });
}

uint32_t HotspotRegistry::Count() const
{
    std::shared_lock lock(_mutex);
    return uint32_t(_hotspots.size());
}

std::vector<Hotspot> HotspotRegistry::Snapshot() const
{
    std::shared_lock lock(_mutex);
    return _hotspots;
}

std::optional<Hotspot> HotspotRegistry::Pick(HotspotQuery const& query, HotspotRules const& rules, Occupancy const& occupancy, Random const& rng) const
{
    std::shared_lock lock(_mutex);

    std::vector<std::pair<Hotspot const*, uint32_t>> candidates;
    uint64_t total = 0;
    for (Hotspot const& hotspot : _hotspots)
    {
        if (hotspot.Team && hotspot.Team != query.Team)
            continue;

        if (query.Level < hotspot.MinLevel || query.Level > hotspot.MaxLevel)
            continue;

        if (hotspot.RaceMask && (query.Race == 0 || query.Race > 32 || !(hotspot.RaceMask & (1u << (query.Race - 1)))))
            continue;

        uint8_t tier = std::min<uint8_t>(hotspot.Tier, 5);
        uint32_t weight = rules.TierWeight[tier];
        if (!weight)
            continue;

        uint32_t cap = rules.TierMaxOccupancy[tier];
        if (cap && occupancy && occupancy(hotspot) >= cap)
            continue;

        candidates.emplace_back(&hotspot, weight);
        total += weight;
    }

    if (candidates.empty())
        return std::nullopt;

    uint64_t roll = rng(uint32_t(std::min<uint64_t>(total - 1, UINT32_MAX)));
    for (auto const& [hotspot, weight] : candidates)
    {
        if (roll < weight)
            return *hotspot;
        roll -= weight;
    }

    return *candidates.back().first;
}

HotspotPoint HotspotRegistry::PickPoint(Hotspot const& hotspot, float spread, Random const& rng)
{
    HotspotPoint base = hotspot.Center;
    if (!hotspot.Points.empty())
        base = hotspot.Points[rng(uint32_t(hotspot.Points.size() - 1))];

    // uniform in the disc: the square root keeps the spots from crowding the middle
    float angle = float(rng(3599)) / 3600.0f * 2.0f * std::numbers::pi_v<float>;
    float distance = std::sqrt(float(rng(10000)) / 10000.0f) * spread;
    return { base.X + std::cos(angle) * distance, base.Y + std::sin(angle) * distance, base.Z };
}
}
