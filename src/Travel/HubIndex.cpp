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

// Flight masters, city services and mailboxes, built from spawn data (like AzerothCore TravelMgr's hub
// caches) and grouped into towns. Only spawns every player sees are used: default phase, no difficulty restriction.

#include "HubIndex.h"
#include "Travel/HubClusters.h"
#include "Travel/PhaseGuard.h"
#include "CreatureData.h"
#include "DB2Stores.h"
#include "GameObjectData.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "UnitDefines.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Playerbots
{
namespace
{
constexpr float CellSize = 250.0f;
constexpr uint32 LinearScanLimit = 160;   // a map with this few spots of a kind is scanned, not gridded

struct KindMap
{
    std::vector<HubSpot> Spots;
    std::unordered_map<int64, std::vector<uint32>> Cells;
};

struct Index
{
    bool Built = false;
    std::unordered_map<uint32, KindMap> ByMap[HubKindCount];
    std::vector<HubCluster> Clusters;
    std::unordered_map<uint32, std::vector<uint32>> ClustersByMap;
};

Index sIndex;

int64 CellKey(int32 cx, int32 cy)
{
    return (int64(cx) << 32) ^ int64(uint32(cy));
}

int32 CellOf(float v)
{
    return int32(std::floor(v / CellSize));
}

// every service an NPC offers, not just the first: an innkeeper that also sells food is both
uint32 KindMaskFromNpcFlag(uint64 npcflag)
{
    uint32 mask = 0;
    auto add = [&](HubKind kind) { mask |= 1u << uint8(kind); };
    if (npcflag & UNIT_NPC_FLAG_FLIGHTMASTER)
        add(HubKind::FlightMaster);
    if (npcflag & UNIT_NPC_FLAG_BANKER)
        add(HubKind::Banker);
    if (npcflag & UNIT_NPC_FLAG_AUCTIONEER)
        add(HubKind::Auctioneer);
    if (npcflag & UNIT_NPC_FLAG_INNKEEPER)
        add(HubKind::Innkeeper);
    if (npcflag & UNIT_NPC_FLAG_REPAIR)
        add(HubKind::Repair);
    if (npcflag & UNIT_NPC_FLAG_MAILBOX)
        add(HubKind::Mailbox);
    if (npcflag & (UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_VENDOR_FOOD | UNIT_NPC_FLAG_VENDOR_MASK))
        add(HubKind::Vendor);
    return mask;
}

bool IsVisibleToEveryone(uint32 phaseId, uint32 phaseGroup, std::vector<Difficulty> const& difficulties)
{
    if (!PhaseGuard::IsUsableSpawnPhase(phaseId, phaseGroup))
        return false;

    return difficulties.empty() || std::ranges::find(difficulties, DIFFICULTY_NONE) != difficulties.end();
}

HubTier TierOf(uint32 mask)
{
    auto has = [&](HubKind kind) { return (mask & (1u << uint8(kind))) != 0; };
    if (has(HubKind::Auctioneer) && has(HubKind::Banker) && has(HubKind::Innkeeper))
        return HubTier::Capital;
    if (has(HubKind::Innkeeper))
        return HubTier::Town;
    if (has(HubKind::FlightMaster))
        return HubTier::FlightMaster;
    return HubTier::Outpost;
}
}

namespace HubIndex
{
void Build()
{
    PhaseGuard::Load();
    sIndex = Index{};

    // does an NPC attack players of a race's faction? The faction of the first Human and Orc stand for the teams
    FactionTemplateEntry const* alliance = nullptr;
    FactionTemplateEntry const* horde = nullptr;
    if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(RACE_HUMAN))
        alliance = sFactionTemplateStore.LookupEntry(race->FactionID);
    if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(RACE_ORC))
        horde = sFactionTemplateStore.LookupEntry(race->FactionID);

    struct Pending
    {
        HubKind Kind;
        uint32 MapId;
        uint32 Entry;
        Position Pos;
        bool HostileToAlliance;
        bool HostileToHorde;
    };
    std::vector<Pending> pending;
    std::set<std::tuple<uint32, uint32, int32, int32, uint8>> seen; // stacked duplicates of one NPC are one spot
    uint32 phased = 0;

    auto add = [&](HubKind kind, uint32 mapId, uint32 entry, Position const& pos, bool hostileAlliance, bool hostileHorde)
    {
        if (!seen.emplace(entry, mapId, int32(pos.GetPositionX()), int32(pos.GetPositionY()), uint8(kind)).second)
            return;
        pending.push_back({ kind, mapId, entry, pos, hostileAlliance, hostileHorde });
    };

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* temp = sObjectMgr->GetCreatureTemplate(data.id);
        if (!temp)
            continue;

        uint32 mask = KindMaskFromNpcFlag(temp->npcflag);
        if (!mask)
            continue;

        MapEntry const* map = sMapStore.LookupEntry(data.mapId);
        if (!map || map->Instanceable())
            continue;

        if (!IsVisibleToEveryone(data.phaseId, data.phaseGroup, data.spawnDifficulties))
        {
            ++phased;
            continue;
        }

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(temp->faction);
        bool hostileAlliance = faction && alliance && faction->IsHostileTo(alliance);
        bool hostileHorde = faction && horde && faction->IsHostileTo(horde);
        for (uint8 kind = 0; kind < HubKindCount; ++kind)
            if (mask & (1u << kind))
                add(HubKind(kind), data.mapId, data.id, data.spawnPoint, hostileAlliance, hostileHorde);
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGameObjectData())
    {
        GameObjectTemplate const* temp = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!temp || temp->type != GAMEOBJECT_TYPE_MAILBOX)
            continue;

        MapEntry const* map = sMapStore.LookupEntry(data.mapId);
        if (!map || map->Instanceable())
            continue;

        if (!IsVisibleToEveryone(data.phaseId, data.phaseGroup, data.spawnDifficulties))
        {
            ++phased;
            continue;
        }

        add(HubKind::Mailbox, data.mapId, data.id, data.spawnPoint, false, false);
    }

    // towns and cities
    std::vector<HubPoint> points;
    points.reserve(pending.size());
    for (Pending const& spot : pending)
    {
        HubPoint point;
        point.Map = spot.MapId;
        point.X = spot.Pos.GetPositionX();
        point.Y = spot.Pos.GetPositionY();
        point.Kind = uint8(spot.Kind);
        point.HostileToAlliance = spot.HostileToAlliance;
        point.HostileToHorde = spot.HostileToHorde;
        points.push_back(point);
    }

    std::vector<int32> clusterOf(pending.size(), -1);
    for (HubGroup const& found : BuildHubClusters(points))
    {
        HubCluster cluster;
        cluster.Id = uint32(sIndex.Clusters.size());
        cluster.MapId = found.Map;
        cluster.Center = Position(found.X, found.Y, 0.0f);
        cluster.Radius = found.Radius;
        cluster.KindMask = found.KindMask;
        cluster.Spots = found.Spots;
        cluster.Tier = TierOf(found.KindMask);
        cluster.HostileToAlliance = found.HostileToAlliance * 2 >= found.Spots;
        cluster.HostileToHorde = found.HostileToHorde * 2 >= found.Spots;

        // the cluster sits at the average height of its members, a point on the ground for a walking bot
        float z = 0.0f;
        for (uint32 member : found.Members)
        {
            clusterOf[member] = int32(cluster.Id);
            z += pending[member].Pos.GetPositionZ();
        }
        cluster.Center.m_positionZ = z / float(found.Members.size());

        sIndex.ClustersByMap[cluster.MapId].push_back(cluster.Id);
        sIndex.Clusters.push_back(cluster);
    }

    for (uint32 i = 0; i < pending.size(); ++i)
    {
        Pending const& source = pending[i];
        KindMap& kindMap = sIndex.ByMap[uint8(source.Kind)][source.MapId];
        HubSpot spot;
        spot.Kind = source.Kind;
        spot.MapId = source.MapId;
        spot.Entry = source.Entry;
        spot.Pos = source.Pos;
        spot.Cluster = clusterOf[i];
        kindMap.Spots.push_back(spot);
    }

    for (auto& byMap : sIndex.ByMap)
        for (auto& [mapId, kindMap] : byMap)
            for (uint32 i = 0; i < kindMap.Spots.size(); ++i)
                kindMap.Cells[CellKey(CellOf(kindMap.Spots[i].Pos.GetPositionX()), CellOf(kindMap.Spots[i].Pos.GetPositionY()))].push_back(i);

    uint32 tiers[6] = {};
    for (HubCluster const& cluster : sIndex.Clusters)
        ++tiers[uint8(cluster.Tier)];

    sIndex.Built = true;
    TC_LOG_INFO("server.playerbots", "Playerbots: city hub index built, {} service spots in the default phase ({} phased spawns skipped), "
        "{} towns ({} capitals, {} towns, {} outposts, {} flight master hubs)", pending.size(), phased, sIndex.Clusters.size(),
        tiers[uint8(HubTier::Capital)], tiers[uint8(HubTier::Town)], tiers[uint8(HubTier::Outpost)], tiers[uint8(HubTier::FlightMaster)]);
}

bool IsBuilt()
{
    return sIndex.Built;
}

HubSpot const* Nearest(Player const* bot, HubKind kind, float maxDist)
{
    if (!bot || !sIndex.Built)
        return nullptr;

    auto mapItr = sIndex.ByMap[uint8(kind)].find(bot->GetMapId());
    if (mapItr == sIndex.ByMap[uint8(kind)].end())
        return nullptr;

    KindMap const& kindMap = mapItr->second;
    Position const here = bot->GetPosition();
    HubSpot const* best = nullptr;
    float bestDist = maxDist;

    int32 range = int32(std::ceil(maxDist / CellSize));
    bool linear = kindMap.Spots.size() <= LinearScanLimit || uint64(2 * range + 1) * uint64(2 * range + 1) > kindMap.Spots.size();
    if (linear)
    {
        for (HubSpot const& spot : kindMap.Spots)
        {
            float dist = here.GetExactDist2d(spot.Pos);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = &spot;
            }
        }
        return best;
    }

    int32 cx = CellOf(here.GetPositionX());
    int32 cy = CellOf(here.GetPositionY());
    for (int32 dx = -range; dx <= range; ++dx)
        for (int32 dy = -range; dy <= range; ++dy)
            if (auto cell = kindMap.Cells.find(CellKey(cx + dx, cy + dy)); cell != kindMap.Cells.end())
                for (uint32 index : cell->second)
                {
                    HubSpot const& spot = kindMap.Spots[index];
                    float dist = here.GetExactDist2d(spot.Pos);
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        best = &spot;
                    }
                }
    return best;
}

HubSpot const* NearestAny(Player const* bot, std::initializer_list<HubKind> kinds, float maxDist)
{
    HubSpot const* best = nullptr;
    float bestDist = maxDist;
    for (HubKind kind : kinds)
        if (HubSpot const* spot = Nearest(bot, kind, maxDist))
        {
            float dist = bot->GetExactDist2d(spot->Pos);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = spot;
            }
        }
    return best;
}

std::vector<HubCluster> const& Clusters()
{
    return sIndex.Clusters;
}

bool IsOpenTo(HubCluster const& cluster, Team team)
{
    if (team == ALLIANCE)
        return !cluster.HostileToAlliance;
    if (team == HORDE)
        return !cluster.HostileToHorde;
    return true;
}

HubCluster const* NearestCluster(uint32 mapId, Position const& from, float maxDist, Team team)
{
    auto mapItr = sIndex.ClustersByMap.find(mapId);
    if (mapItr == sIndex.ClustersByMap.end())
        return nullptr;

    HubCluster const* best = nullptr;
    float bestDist = maxDist;
    for (uint32 id : mapItr->second)
    {
        HubCluster const& cluster = sIndex.Clusters[id];
        if (team != TEAM_OTHER && !IsOpenTo(cluster, team))
            continue;

        float dist = std::max(0.0f, from.GetExactDist2d(cluster.Center) - cluster.Radius);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = &cluster;
        }
    }
    return best;
}

HubSpot const* NearestInCluster(HubCluster const& cluster, HubKind kind, Position const& from)
{
    auto mapItr = sIndex.ByMap[uint8(kind)].find(cluster.MapId);
    if (mapItr == sIndex.ByMap[uint8(kind)].end())
        return nullptr;

    HubSpot const* best = nullptr;
    float bestDist = 0.0f;
    for (HubSpot const& spot : mapItr->second.Spots)
    {
        if (spot.Cluster != int32(cluster.Id))
            continue;

        float dist = from.GetExactDist2d(spot.Pos);
        if (!best || dist < bestDist)
        {
            bestDist = dist;
            best = &spot;
        }
    }
    return best;
}

std::vector<Position> ClusterPoints(HubCluster const& cluster, uint32 maxCount, bool social)
{
    if (social)
    {
        // where people stand in a city: the more a place is used, the more often it is listed
        struct Place
        {
            HubKind Kind;
            uint32 Weight;
            uint32 MaxSpots;
        };
        static constexpr Place Places[] = { { HubKind::Auctioneer, 4, 8 }, { HubKind::Banker, 3, 8 }, { HubKind::Mailbox, 2, 8 },
            { HubKind::Innkeeper, 2, 6 }, { HubKind::FlightMaster, 2, 6 }, { HubKind::Vendor, 1, 4 }, { HubKind::Repair, 1, 3 } };

        std::vector<Position> weighted;
        for (Place const& place : Places)
        {
            auto mapItr = sIndex.ByMap[uint8(place.Kind)].find(cluster.MapId);
            if (mapItr == sIndex.ByMap[uint8(place.Kind)].end())
                continue;

            std::vector<Position> kept;
            for (HubSpot const& spot : mapItr->second.Spots)
            {
                if (spot.Cluster != int32(cluster.Id) || kept.size() >= place.MaxSpots)
                    continue;

                // the counter of an auction house is a row of NPCs: one place to stand
                if (std::ranges::none_of(kept, [&](Position const& other) { return other.GetExactDist2d(spot.Pos) < 8.0f; }))
                    kept.push_back(spot.Pos);
            }

            for (uint32 copy = 0; copy < place.Weight; ++copy)
                weighted.insert(weighted.end(), kept.begin(), kept.end());
        }

        // shops are last, they go first when there is too much
        if (maxCount && weighted.size() > maxCount)
            weighted.resize(maxCount);
        return weighted;
    }

    std::vector<Position> all;
    for (auto const& byMap : sIndex.ByMap)
    {
        auto mapItr = byMap.find(cluster.MapId);
        if (mapItr == byMap.end())
            continue;

        for (HubSpot const& spot : mapItr->second.Spots)
        {
            if (spot.Cluster != int32(cluster.Id))
                continue;

            // an NPC and the one next to it are one place to stand
            if (std::ranges::none_of(all, [&](Position const& other) { return other.GetExactDist2d(spot.Pos) < 6.0f; }))
                all.push_back(spot.Pos);
        }
    }

    if (all.size() <= maxCount || !maxCount)
        return all;

    // evenly through the list: the order follows the NPC kinds, so this mixes them
    std::vector<Position> picked;
    picked.reserve(maxCount);
    for (uint32 i = 0; i < maxCount; ++i)
        picked.push_back(all[size_t(i) * all.size() / maxCount]);
    return picked;
}
}
}
