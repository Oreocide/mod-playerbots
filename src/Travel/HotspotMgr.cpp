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

#include "HotspotMgr.h"
#include "DB2Stores.h"
#include "DatabaseEnv.h"
#include "HubIndex.h"
#include "Log.h"
#include "Maps/TerrainMgr.h"
#include "ObjectAccessor.h"
#include "Phasing/PhasingHandler.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "RandomBotMgr.h"
#include "Random.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <vector>

namespace Playerbots
{
namespace
{
constexpr uint32 OccupancyRefreshMs = 5000;
constexpr uint32 PointsPerHub = 14;
constexpr uint32 SocialPointsPerHub = 60;

uint8 TeamCode(Team team)
{
    return team == ALLIANCE ? 1 : team == HORDE ? 2 : 0;
}

char const* TierName(uint8 tier)
{
    switch (tier)
    {
        case 1: return "capital";
        case 2: return "town";
        case 3: return "outpost";
        case 4: return "flight master hub";
        case 5: return "transport hub";
    }
    return "hotspot";
}

// town sizes by tier: high level bots do not hang around in starting villages
void LevelRange(HubTier tier, uint8& minLevel, uint8& maxLevel)
{
    minLevel = 1;
    maxLevel = 255;
    switch (tier)
    {
        case HubTier::Town: maxLevel = 40; break;
        case HubTier::Outpost: maxLevel = 20; break;
        case HubTier::FlightMaster: minLevel = 5; break;
        default: break;
    }
}
}

HotspotMgr* HotspotMgr::instance()
{
    static HotspotMgr instance;
    return &instance;
}

void HotspotMgr::LoadMarkerTemplates()
{
    _markerTemplates.clear();

    // the table ships with the module's world SQL, a server that has not applied it yet simply has no markers
    QueryResult result = WorldDatabase.Query("SELECT entry, tier, team, min_level, max_level, race_mask, radius, name FROM playerbots_hotspot_marker");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            MarkerTemplate marker;
            marker.Tier = uint8(std::clamp<uint32>(fields[1].GetUInt8(), 1, 5));
            marker.Team = std::min<uint8>(fields[2].GetUInt8(), 2);
            marker.MinLevel = fields[3].GetUInt8();
            marker.MaxLevel = fields[4].GetUInt8();
            marker.RaceMask = fields[5].GetUInt32();
            marker.Radius = std::max(5.0f, fields[6].GetFloat());
            marker.Name = fields[7].GetString();
            _markerTemplates[fields[0].GetUInt32()] = std::move(marker);
        }
        while (result->NextRow());
    }

    // retuning of derived hotspots by place
    _overrides.clear();
    if (QueryResult overrides = WorldDatabase.Query("SELECT map, x, y, radius, min_level, max_level, tier, team, enabled FROM playerbots_hotspot_override"))
    {
        do
        {
            Field* fields = (*overrides).Fetch();
            Override entry;
            entry.MapId = fields[0].GetUInt32();
            entry.X = fields[1].GetFloat();
            entry.Y = fields[2].GetFloat();
            entry.Radius = fields[3].GetFloat();
            entry.MinLevel = fields[4].GetUInt8();
            entry.MaxLevel = fields[5].GetUInt8();
            entry.Tier = fields[6].IsNull() ? -1 : int32(fields[6].GetUInt8());
            entry.Team = fields[7].IsNull() ? -1 : int32(fields[7].GetUInt8());
            entry.Enabled = fields[8].GetUInt8();
            _overrides.push_back(entry);
        }
        while (overrides->NextRow());
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: {} hotspot marker templates, {} hotspot overrides", _markerTemplates.size(), _overrides.size());
}

void HotspotMgr::BuildFromHubs()
{
    _registry.ClearDerived();

    uint32 added = 0;
    for (HubCluster const& cluster : HubIndex::Clusters())
    {
        // a city whose NPCs attack both teams is nobody's home, one that attacks a team is the other team's
        if (cluster.HostileToAlliance && cluster.HostileToHorde)
            continue;

        // bots only live on the maps of Playerbots.Random.Maps, every other map would keep its grids loaded for them
        if (!sRandomBotMgr->IsMapAllowed(cluster.MapId))
            continue;

        Hotspot hotspot;
        hotspot.Map = cluster.MapId;
        hotspot.Center = { cluster.Center.GetPositionX(), cluster.Center.GetPositionY(), cluster.Center.GetPositionZ() };
        hotspot.Radius = std::max(30.0f, cluster.Radius + 10.0f);
        hotspot.Team = cluster.HostileToAlliance ? 2 : cluster.HostileToHorde ? 1 : 0;
        hotspot.Tier = uint8(cluster.Tier);
        LevelRange(cluster.Tier, hotspot.MinLevel, hotspot.MaxLevel);

        // cities and towns: arrivals gather where players stand (auction house, bank, mailbox, inn)
        bool social = cluster.Tier == HubTier::Capital || cluster.Tier == HubTier::Town;
        for (Position const& point : HubIndex::ClusterPoints(cluster, social ? SocialPointsPerHub : PointsPerHub, social))
            hotspot.Points.push_back({ point.GetPositionX(), point.GetPositionY(), point.GetPositionZ() });

        // derived hubs of tiers outside Playerbots.Hotspot.AutoTiers are not places players crowd, unless an override
        // row with enabled = 2 names them
        uint8 enabled = (sPlayerbotsConfig.HotspotAutoTiers & (1u << (uint8(cluster.Tier) - 1))) ? 1 : 0;
        for (Override const& entry : _overrides)
        {
            if (entry.MapId != hotspot.Map || std::hypot(entry.X - hotspot.Center.X, entry.Y - hotspot.Center.Y) > entry.Radius)
                continue;

            hotspot.MinLevel = entry.MinLevel;
            hotspot.MaxLevel = entry.MaxLevel;
            if (entry.Tier >= 1)
                hotspot.Tier = uint8(std::min(entry.Tier, 5));
            if (entry.Team >= 0)
                hotspot.Team = uint8(std::min(entry.Team, 2));
            if (entry.Enabled != 1)
                enabled = entry.Enabled;
        }

        if (!enabled)
            continue;

        MapEntry const* map = sMapStore.LookupEntry(cluster.MapId);
        hotspot.Name = Trinity::StringFormat("{} #{} ({}, {} spots)", TierName(hotspot.Tier), cluster.Id,
            map ? map->MapName[sWorld->GetDefaultDbcLocale()] : "?", cluster.Spots);

        _registry.Add(std::move(hotspot));
        ++added;
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: {} hotspots from the hub index, {} in total", added, _registry.Count());
}

void HotspotMgr::Update(uint32 diff)
{
    _timer += diff;
    if (_timer < OccupancyRefreshMs)
        return;
    _timer = 0;

    std::vector<Hotspot> hotspots = _registry.Snapshot();
    if (hotspots.empty())
        return;

    struct Spot
    {
        uint32 Id;
        uint32 Map;
        float X;
        float Y;
        float RadiusSq;
    };
    std::vector<Spot> spots;
    spots.reserve(hotspots.size());
    float radius = sPlayerbotsConfig.HotspotOccupancyRadius;
    for (Hotspot const& hotspot : hotspots)
    {
        float reach = radius + hotspot.Radius * 0.5f;
        spots.push_back({ hotspot.Id, hotspot.Map, hotspot.Center.X, hotspot.Center.Y, reach * reach });
    }

    _occupancy.clear();
    std::shared_lock lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!player->IsInWorld())
            continue;

        uint32 mapId = player->GetMapId();
        float x = player->GetPositionX();
        float y = player->GetPositionY();
        for (Spot const& spot : spots)
        {
            if (spot.Map != mapId)
                continue;

            float dx = x - spot.X;
            float dy = y - spot.Y;
            if (dx * dx + dy * dy <= spot.RadiusSq)
                ++_occupancy[spot.Id];
        }
    }
}

uint32 HotspotMgr::GetOccupancy(Hotspot const& hotspot) const
{
    auto itr = _occupancy.find(hotspot.Id);
    return itr == _occupancy.end() ? 0 : itr->second;
}

std::optional<HotspotMgr::Destination> HotspotMgr::PickFor(Player const* bot) const
{
    HotspotRules rules;
    for (uint32 tier = 1; tier <= 5; ++tier)
    {
        rules.TierWeight[tier] = sPlayerbotsConfig.HotspotTierWeight[tier];
        rules.TierMaxOccupancy[tier] = sPlayerbotsConfig.HotspotTierMaxOccupancy[tier];
    }
    rules.SpreadRadius = sPlayerbotsConfig.HotspotSpreadRadius;

    HotspotQuery query;
    query.Level = uint8(std::min<uint32>(bot->GetLevel(), 255));
    query.Race = bot->GetRace();
    query.Team = TeamCode(Player::TeamForRace(bot->GetRace()));

    Random rng = [](uint32 maxInclusive) { return urand(0, maxInclusive); };
    Occupancy occupancy = [this](Hotspot const& hotspot) { return GetOccupancy(hotspot); };

    std::optional<Hotspot> spot = _registry.Pick(query, rules, occupancy, rng);
    if (!spot)
        return std::nullopt;

    Destination destination;
    destination.Point = HotspotRegistry::PickPoint(*spot, rules.SpreadRadius, rng);
    destination.Spot = std::move(*spot);
    return destination;
}

std::string HotspotMgr::Describe(Hotspot const& hotspot) const
{
    // the zone name needs the terrain of the map, which only a loaded map has
    uint32 zoneId = 0;
    uint32 areaId = 0;
    sTerrainMgr.GetZoneAndAreaId(PhasingHandler::GetEmptyPhaseShift(), zoneId, areaId, hotspot.Map, hotspot.Center.X, hotspot.Center.Y, hotspot.Center.Z);
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
        return Trinity::StringFormat("{} [{}]", hotspot.Name, zone->AreaName[sWorld->GetDefaultDbcLocale()]);

    return hotspot.Name;
}

void HotspotMgr::RegisterMarker(ObjectGuid guid, uint32 entry, uint32 mapId, float x, float y, float z)
{
    if (!sPlayerbotsConfig.HotspotMarkers)
        return;

    auto itr = _markerTemplates.find(entry);
    if (itr == _markerTemplates.end())
        return;

    MarkerTemplate const& marker = itr->second;
    Hotspot hotspot;
    hotspot.Map = mapId;
    hotspot.Center = { x, y, z };
    hotspot.Radius = marker.Radius;
    hotspot.Team = marker.Team;
    hotspot.Tier = marker.Tier;
    hotspot.MinLevel = marker.MinLevel;
    hotspot.MaxLevel = marker.MaxLevel;
    hotspot.RaceMask = marker.RaceMask;
    hotspot.Name = marker.Name.empty() ? Trinity::StringFormat("marker {}", entry) : marker.Name;
    hotspot.Owner = guid.GetCounter() + 1;

    // a marker whose AI was rebuilt registers again: one hotspot per marker
    _registry.RemoveOwner(hotspot.Owner);
    _registry.Add(std::move(hotspot));
}

void HotspotMgr::UnregisterMarker(ObjectGuid guid)
{
    _registry.RemoveOwner(guid.GetCounter() + 1);
}
}
