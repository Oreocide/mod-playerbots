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

#ifndef Playerbots_Hotspots_h__
#define Playerbots_Hotspots_h__

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

// Places random bots gather: a town or city, or the spot of an invisible marker NPC. No core types, tested
// standalone. Hotspots come from two sources, hubs derived from the world data and markers placed by hand,
// and are picked by tier weight like AzerothCore's population hotspots.
namespace Playerbots
{
struct HotspotPoint
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct Hotspot
{
    uint32_t Id = 0;
    uint32_t Map = 0;
    HotspotPoint Center;
    std::vector<HotspotPoint> Points;  // positions known to be on the ground (service NPCs), empty = the centre
    float Radius = 40.0f;              // the area that counts as "in" the hotspot
    uint8_t Team = 0;                  // 0 both, 1 Alliance, 2 Horde
    uint8_t Tier = 1;                  // 1 capital .. 5 transport hub
    uint8_t MinLevel = 1;
    uint8_t MaxLevel = 255;
    uint32_t RaceMask = 0;             // bit (race id - 1), 0 = every race
    std::string Name;
    uint64_t Owner = 0;                // marker NPC guid, 0 for hubs derived from data
};

struct HotspotRules
{
    uint32_t TierWeight[6] = { 0, 5, 2, 1, 2, 1 };          // index = tier, 0 = never
    uint32_t TierMaxOccupancy[6] = { 0, 75, 30, 10, 20, 15 }; // 0 = no cap
    float SpreadRadius = 15.0f;
};

struct HotspotQuery
{
    uint8_t Level = 1;
    uint8_t Race = 0;
    uint8_t Team = 0;                  // 1 Alliance, 2 Horde
};

using Occupancy = std::function<uint32_t(Hotspot const&)>;
using Random = std::function<uint32_t(uint32_t maxInclusive)>;   // [0, max]

class HotspotRegistry
{
public:
    uint32_t Add(Hotspot hotspot);
    bool Remove(uint32_t id);
    /// Everything a marker NPC registered (it is gone or unloaded)
    void RemoveOwner(uint64_t owner);
    /// Hubs derived from data are rebuilt, markers stay
    void ClearDerived();

    uint32_t Count() const;
    std::vector<Hotspot> Snapshot() const;

    /// A hotspot a bot may go to: team, level and race fit, the tier has a weight and is not full. A copy,
    /// markers can unregister while the caller uses it
    std::optional<Hotspot> Pick(HotspotQuery const& query, HotspotRules const& rules, Occupancy const& occupancy, Random const& rng) const;

    /// A ground position inside the hotspot, a service NPC spot spread by up to spread yards
    static HotspotPoint PickPoint(Hotspot const& hotspot, float spread, Random const& rng);

private:
    mutable std::shared_mutex _mutex;      // markers register from map threads, bots pick from the world thread
    std::vector<Hotspot> _hotspots;
    uint32_t _nextId = 1;
};
}

#endif // Playerbots_Hotspots_h__
