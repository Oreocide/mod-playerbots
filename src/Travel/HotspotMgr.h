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

#ifndef PlayerbotsHotspotMgr_h__
#define PlayerbotsHotspotMgr_h__

#include "Hotspots.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace Playerbots
{
/// Hotspots of the world: towns and cities derived from the service NPC index, and invisible marker NPCs
/// (entries of world.playerbots_hotspot_marker) that register themselves while loaded. Picks where a random
/// bot goes. World thread, except the marker calls that come from map threads.
class HotspotMgr
{
public:
    static HotspotMgr* instance();

    /// Marker templates from the world database (startup)
    void LoadMarkerTemplates();
    /// One hotspot per usable town of the hub index; rebuilt on each call, markers stay
    void BuildFromHubs();

    /// Counts the players and bots around each hotspot, a few times a minute
    void Update(uint32 diff);

    struct Destination
    {
        Hotspot Spot;
        HotspotPoint Point;
    };
    /// Where this bot could go: team, level and race fit, the tier is wanted and the hotspot is not crowded
    std::optional<Destination> PickFor(Player const* bot) const;

    uint32 GetOccupancy(Hotspot const& hotspot) const;
    HotspotRegistry& GetRegistry() { return _registry; }
    /// "Orgrimmar", or the hotspot's own name
    std::string Describe(Hotspot const& hotspot) const;

    /// A marker NPC came into the world / left it. Any thread
    void RegisterMarker(ObjectGuid guid, uint32 entry, uint32 mapId, float x, float y, float z);
    void UnregisterMarker(ObjectGuid guid);

private:
    struct MarkerTemplate
    {
        uint8 Tier = 2;
        uint8 Team = 0;
        uint8 MinLevel = 1;
        uint8 MaxLevel = 255;
        uint32 RaceMask = 0;
        float Radius = 40.0f;
        std::string Name;
    };

    /// Retunes the derived hotspot near a point: a city that is only for high levels, or not for bots at all
    struct Override
    {
        uint32 MapId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Radius = 0.0f;
        uint8 MinLevel = 1;
        uint8 MaxLevel = 255;
        int32 Tier = -1;     // -1 = keep
        int32 Team = -1;
        uint8 Enabled = 1;       // 0 remove, 1 keep what was derived, 2 force in
    };

    HotspotRegistry _registry;
    std::vector<Override> _overrides;
    std::unordered_map<uint32, MarkerTemplate> _markerTemplates;
    std::unordered_map<uint32, uint32> _occupancy;   // hotspot id -> players and bots near it
    uint32 _timer = 0;
};
}

#define sHotspotMgr Playerbots::HotspotMgr::instance()

#endif // PlayerbotsHotspotMgr_h__
