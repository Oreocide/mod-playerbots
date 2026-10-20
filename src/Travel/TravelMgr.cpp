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

#include "TravelMgr.h"
#include "Economy/FlyingMounts.h"
#include "DB2Stores.h"
#include "DangerMap.h"
#include "DatabaseEnv.h"
#include "GameObjectData.h"
#include "TransportMgr.h"
#include "GameTime.h"
#include "HubIndex.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "SpellHistory.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include <algorithm>
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr float FlightSpeed = 32.0f;         // yards per second on a flight path
constexpr float AirSpeed = 22.0f;            // yards per second of steady flight on a Master Riding mount (310% of 7)
constexpr float AirOverheadSeconds = 12.0f;  // mounting up, the climb out and the landing

// Playerbots.Air.TestBots: comma separated names, any case
bool AirListed(std::string const& name)
{
    std::string wanted = name;
    std::ranges::transform(wanted, wanted.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::string const& list = sPlayerbotsConfig.AirTestBots;
    size_t start = 0;
    while (start <= list.size())
    {
        size_t comma = list.find(',', start);
        std::string entry = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::erase(entry, ' ');
        std::ranges::transform(entry, entry.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (!entry.empty() && entry == wanted)
            return true;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return false;
}
constexpr float MountedRunSpeed = 11.2f;     // 160 percent of a run
constexpr float HearthSeconds = 15.0f;       // ten second cast and the loading screen
constexpr uint32 HearthstoneItem = 6948;
constexpr uint32 HearthstoneSpell = 8690;
constexpr uint32 NeighbourCount = 6;
constexpr float NeighbourDistance = 1500.0f;

uint8 TeamCode(Team team)
{
    return team == ALLIANCE ? 1 : team == HORDE ? 2 : 0;
}

char const* TierName(HubTier tier)
{
    switch (tier)
    {
        case HubTier::Capital: return "capital";
        case HubTier::Town: return "town";
        case HubTier::Outpost: return "outpost";
        case HubTier::FlightMaster: return "flight master hub";
        case HubTier::Transport: return "transport hub";
    }
    return "hub";
}

char const* TypeName(LinkType type)
{
    switch (type)
    {
        case LinkType::Walk: return "walk";
        case LinkType::Taxi: return "fly";
        case LinkType::Transport: return "ride";
        case LinkType::Portal: return "portal";
        case LinkType::Teleport: return "teleport";
        case LinkType::Air: return "air";
    }
    return "?";
}
}

TravelMgr* TravelMgr::instance()
{
    static TravelMgr instance;
    return &instance;
}

uint32 TravelMgr::NearestNode(uint32 mapId, float x, float y, float maxDistance) const
{
    std::vector<uint32> nearest = _graph.NearestNodes(mapId, x, y, 1, maxDistance);
    return nearest.empty() ? UINT32_MAX : nearest.front();
}

void TravelMgr::AddHubNodes()
{
    for (HubCluster const& cluster : HubIndex::Clusters())
    {
        // a standing place near the middle: the service NPC position closest to the centroid
        std::vector<Position> points = HubIndex::ClusterPoints(cluster, 60, false);
        if (points.empty())
            continue;

        Position const* best = &points.front();
        for (Position const& point : points)
            if (point.GetExactDist2d(cluster.Center) < best->GetExactDist2d(cluster.Center))
                best = &point;

        MapEntry const* map = sMapStore.LookupEntry(cluster.MapId);
        TravelNode node;
        node.Map = cluster.MapId;
        node.X = best->GetPositionX();
        node.Y = best->GetPositionY();
        node.Z = best->GetPositionZ();
        node.Kind = NodeKind::Hub;
        node.Ref = cluster.Id;
        node.Name = Trinity::StringFormat("{} #{} ({})", TierName(cluster.Tier), cluster.Id, map ? map->MapName[sWorld->GetDefaultDbcLocale()] : "?");
        _graph.AddNode(std::move(node));
    }
}

void TravelMgr::AddTaxi()
{
    for (TaxiNodesEntry const* entry : sTaxiNodesStore)
    {
        if (!entry || !entry->IsPartOfTaxiNetwork())
            continue;

        MapEntry const* map = sMapStore.LookupEntry(entry->ContinentID);
        if (!map || map->Instanceable())
            continue;

        TravelNode node;
        node.Map = entry->ContinentID;
        node.X = entry->Pos.X;
        node.Y = entry->Pos.Y;
        node.Z = entry->Pos.Z;
        node.Kind = NodeKind::FlightMaster;
        node.Ref = entry->ID;
        node.Name = entry->Name[sWorld->GetDefaultDbcLocale()];
        _nodeByTaxi[entry->ID] = _graph.AddNode(std::move(node));
    }

    uint32 links = 0;
    for (TaxiPathEntry const* path : sTaxiPathStore)
    {
        if (!path)
            continue;

        auto from = _nodeByTaxi.find(path->FromTaxiNode);
        auto to = _nodeByTaxi.find(path->ToTaxiNode);
        if (from == _nodeByTaxi.end() || to == _nodeByTaxi.end())
            continue;

        // time on the path: its length at flight speed plus the stops it makes
        float length = 0.0f;
        float delays = 0.0f;
        if (path->ID < sTaxiPathNodesByPath.size())
        {
            TaxiPathNodeList const& nodes = sTaxiPathNodesByPath[path->ID];
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                delays += float(nodes[i]->Delay) / 1000.0f;
                if (i)
                    length += std::sqrt(std::pow(nodes[i]->Loc.X - nodes[i - 1]->Loc.X, 2.0f) + std::pow(nodes[i]->Loc.Y - nodes[i - 1]->Loc.Y, 2.0f)
                        + std::pow(nodes[i]->Loc.Z - nodes[i - 1]->Loc.Z, 2.0f));
            }
        }

        // a path that shows on one team's map only is that team's
        TaxiNodesEntry const* source = sTaxiNodesStore.LookupEntry(path->FromTaxiNode);
        TaxiNodesEntry const* target = sTaxiNodesStore.LookupEntry(path->ToTaxiNode);
        bool alliance = source->GetFlags().HasFlag(TaxiNodeFlags::ShowOnAllianceMap) && target->GetFlags().HasFlag(TaxiNodeFlags::ShowOnAllianceMap);
        bool horde = source->GetFlags().HasFlag(TaxiNodeFlags::ShowOnHordeMap) && target->GetFlags().HasFlag(TaxiNodeFlags::ShowOnHordeMap);

        TravelLink link;
        link.From = from->second;
        link.To = to->second;
        link.Type = LinkType::Taxi;
        link.Seconds = length / FlightSpeed + delays + 3.0f;
        link.Ref = path->ToTaxiNode;
        link.Gold = path->Cost;
        link.Team = alliance && !horde ? 1 : horde && !alliance ? 2 : 0;
        if (!alliance && !horde && !source->GetFlags().HasFlag(TaxiNodeFlags::ShowOnAllianceMap | TaxiNodeFlags::ShowOnHordeMap))
            link.Team = 0; // the hidden hubs belong to everyone
        _graph.AddLink(link);
        ++links;
    }

    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} flight master nodes, {} flight path links", _nodeByTaxi.size(), links);
}

// Portals that join two places of the open world. Most area triggers lead into instances, which bots do not enter
// Air links join towns and flight masters of Eastern Kingdoms and Kalimdor (the continents whose steady-flight capabilities were checked in
// the client data) that are Playerbots.Air.MinLegYards to MaxLegYards apart: the nearest few in that range for each node. Which bot may use
// them, and whether the corridor is safe, is decided when a bot plans (RouteOptions::AllowAir, AirCorridor), not here
void TravelMgr::AddAirLinks()
{
    if (!sPlayerbotsConfig.AirEnable)
        return;

    float const minYards = float(sPlayerbotsConfig.AirMinLegYards);
    float const maxYards = float(sPlayerbotsConfig.AirMaxLegYards);

    std::vector<std::pair<uint32, uint32>> pairs;
    for (uint32 id = 0; id < _graph.NodeCount(); ++id)
    {
        TravelNode const& node = _graph.GetNode(id);
        if ((node.Map != 0 && node.Map != 1) || node.Team == 3 || (node.Kind != NodeKind::Hub && node.Kind != NodeKind::FlightMaster))
            continue;

        uint32 taken = 0;
        for (uint32 other : _graph.NearestNodes(node.Map, node.X, node.Y, 40, maxYards))
        {
            TravelNode const& target = _graph.GetNode(other);
            if (other == id || target.Team == 3 || (target.Kind != NodeKind::Hub && target.Kind != NodeKind::FlightMaster))
                continue;
            float distance = std::hypot(target.X - node.X, target.Y - node.Y);
            if (distance < minYards)
                continue;

            pairs.emplace_back(std::min(id, other), std::max(id, other));
            if (++taken >= sPlayerbotsConfig.AirNeighbours)
                break;
        }
    }

    std::ranges::sort(pairs);
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    for (auto const& [a, b] : pairs)
    {
        TravelNode const& from = _graph.GetNode(a);
        TravelNode const& to = _graph.GetNode(b);
        float distance = std::sqrt((to.X - from.X) * (to.X - from.X) + (to.Y - from.Y) * (to.Y - from.Y) + (to.Z - from.Z) * (to.Z - from.Z));

        TravelLink link;
        link.Type = LinkType::Air;
        link.Seconds = distance / AirSpeed + AirOverheadSeconds;
        link.From = a;
        link.To = b;
        _graph.AddLink(link);
        std::swap(link.From, link.To);
        _graph.AddLink(link);
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: {} air links between {} town pairs", pairs.size() * 2, pairs.size());
}

void TravelMgr::AddPortals()
{
    uint32 added = 0;
    for (AreaTriggerEntry const* trigger : sAreaTriggerStore)
    {
        if (!trigger)
            continue;

        AreaTriggerTeleport const* destination = sObjectMgr->GetAreaTrigger(trigger->ID);
        if (!destination)
            continue;

        MapEntry const* fromMap = sMapStore.LookupEntry(trigger->ContinentID);
        MapEntry const* toMap = sMapStore.LookupEntry(destination->Loc.GetMapId());
        if (!fromMap || !toMap || fromMap->Instanceable() || toMap->Instanceable())
            continue;

        TravelNode source;
        source.Map = trigger->ContinentID;
        source.X = trigger->Pos.X;
        source.Y = trigger->Pos.Y;
        source.Z = trigger->Pos.Z;
        source.Kind = NodeKind::PortalMouth;
        source.Ref = trigger->ID;
        source.Name = Trinity::StringFormat("portal {}", trigger->ID);
        uint32 sourceId = _graph.AddNode(std::move(source));

        TravelNode target;
        target.Map = destination->Loc.GetMapId();
        target.X = destination->Loc.GetPositionX();
        target.Y = destination->Loc.GetPositionY();
        target.Z = destination->Loc.GetPositionZ();
        target.Kind = NodeKind::PortalMouth;
        target.Ref = trigger->ID;
        target.Name = Trinity::StringFormat("portal {} exit", trigger->ID);
        uint32 targetId = _graph.AddNode(std::move(target));

        TravelLink link;
        link.From = sourceId;
        link.To = targetId;
        link.Type = LinkType::Portal;
        link.Seconds = 6.0f;
        link.Ref = trigger->ID;
        _graph.AddLink(link);
        ++added;
    }

    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} open world area trigger portals", added);
}

// Boats, zeppelins and ships that connect places. Nobody scripts the boarding: a bot walks to the dock and is
// taken across after the trip time (RouteFollower), so a transport is just a link between the nodes next to its
// docks. Transports that are not for players (gunships, ships of raids) are left out.
void TravelMgr::AddTransports()
{
    static constexpr std::string_view NotForBots[] = { "Gunship", "Sister Mercy", "Krazzworks", "Orgrim's Hammer", "Skybreaker",
        "Warbringer", "Sword of Dawn", "Attack Zeppelin", "Dragonmaw" };

    QueryResult result = WorldDatabase.Query("SELECT entry FROM transports");
    if (!result)
        return;

    uint32 transports = 0;
    uint32 links = 0;
    do
    {
        uint32 entry = (*result)[0].GetUInt32();
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(entry);
        if (!object || object->type != GAMEOBJECT_TYPE_MAP_OBJ_TRANSPORT)
            continue;

        std::string name = object->name;
        if (std::ranges::any_of(NotForBots, [&](std::string_view bad) { return name.find(bad) != std::string::npos; }))
            continue;

        uint32 pathId = object->moTransport.taxiPathID;
        if (pathId >= sTaxiPathNodesByPath.size())
            continue;

        // where it stops: the ground node next to each dock, in the order of the route
        std::vector<uint32> docks;
        for (TaxiPathNodeEntry const* node : sTaxiPathNodesByPath[pathId])
        {
            if (!(node->Flags & TAXI_PATH_NODE_FLAG_STOP))
                continue;

            uint32 dock = NearestNode(node->ContinentID, node->Loc.X, node->Loc.Y, 400.0f);
            if (dock != UINT32_MAX && (docks.empty() || docks.back() != dock) && std::ranges::find(docks, dock) == docks.end())
                docks.push_back(dock);
        }

        if (docks.size() < 2)
            continue;

        // one trip between neighbouring docks: the route's cycle shared by its stops, plus the wait for the next one
        TransportTemplate const* transportTemplate = sTransportMgr->GetTransportTemplate(entry);
        float cycle = transportTemplate && transportTemplate->TotalPathTime ? float(transportTemplate->TotalPathTime) / 1000.0f : 400.0f;
        float trip = cycle / float(docks.size() == 2 ? 2 : docks.size());
        float wait = std::min(cycle / 2.0f, 150.0f);

        auto teamOf = [&](uint32 nodeId) -> int
        {
            TravelNode const& node = _graph.GetNode(nodeId);
            HubCluster const* cluster = HubIndex::NearestCluster(node.Map, Position(node.X, node.Y, node.Z), 300.0f);
            if (!cluster)
                return 0;
            if (cluster->HostileToAlliance && !cluster->HostileToHorde)
                return 2;
            if (cluster->HostileToHorde && !cluster->HostileToAlliance)
                return 1;
            return 0;
        };

        bool linked = false;
        for (size_t i = 0; i + 1 < docks.size(); ++i)
        {
            int a = teamOf(docks[i]);
            int b = teamOf(docks[i + 1]);
            if (a && b && a != b)
                continue;   // joins an Alliance place to a Horde place: not a ride for anyone

            for (bool forward : { true, false })
            {
                TravelLink link;
                link.From = forward ? docks[i] : docks[i + 1];
                link.To = forward ? docks[i + 1] : docks[i];
                link.Type = LinkType::Transport;
                link.Seconds = trip + wait;
                link.Ref = entry;
                link.Team = uint8(a ? a : b);
                _graph.AddLink(link);
                ++links;
            }
            linked = true;
        }

        transports += linked;
    }
    while (result->NextRow());

    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} transports joined by {} links", transports, links);
}

// A town whose creatures attack a team is closed to it (Orgrimmar for an Alliance bot): its nodes are not used, so
// no route walks through it. Flight masters that only show on one team's map belong to that team
void TravelMgr::MarkTeams()
{
    for (uint32 i = 0; i < _graph.NodeCount(); ++i)
    {
        TravelNode& node = _graph.MutableNode(i);
        if (node.Kind == NodeKind::Hub)
        {
            HubCluster const& cluster = HubIndex::Clusters()[node.Ref];
            if (cluster.HostileToAlliance && cluster.HostileToHorde)
                node.Team = 3;
            else if (cluster.HostileToAlliance)
                node.Team = 2;
            else if (cluster.HostileToHorde)
                node.Team = 1;
        }
        else if (node.Kind == NodeKind::FlightMaster)
        {
            if (TaxiNodesEntry const* taxi = sTaxiNodesStore.LookupEntry(node.Ref))
            {
                bool alliance = taxi->GetFlags().HasFlag(TaxiNodeFlags::ShowOnAllianceMap);
                bool horde = taxi->GetFlags().HasFlag(TaxiNodeFlags::ShowOnHordeMap);
                node.Team = alliance && !horde ? 1 : horde && !alliance ? 2 : 0;
            }
        }
    }
}

// What a bot meets on the way: the highest level of a creature that attacks each team along every walking link
void TravelMgr::RateWalkLinks()
{
    if (!sDangerMap->IsBuilt())
        return;

    for (uint32 i = 0; i < _graph.LinkCount(); ++i)
    {
        TravelLink& link = _graph.MutableLink(i);
        if (link.Type != LinkType::Walk)
            continue;

        TravelNode const& from = _graph.GetNode(link.From);
        TravelNode const& to = _graph.GetNode(link.To);
        link.DangerAlliance = sDangerMap->LevelAlong(from.Map, from.X, from.Y, to.X, to.Y, 1);
        link.DangerHorde = sDangerMap->LevelAlong(from.Map, from.X, from.Y, to.X, to.Y, 2);
    }
}

bool TravelMgr::IsHostileGround(uint32 mapId, float x, float y, uint8 team) const
{
    if (!team)
        return false;

    for (HubCluster const& cluster : HubIndex::Clusters())
    {
        if (cluster.MapId != mapId || std::hypot(cluster.Center.GetPositionX() - x, cluster.Center.GetPositionY() - y) > cluster.Radius + 30.0f)
            continue;

        if (!HubIndex::IsOpenTo(cluster, team == 1 ? ALLIANCE : HORDE))
            return true;
    }
    return false;
}

void TravelMgr::Build()
{
    _graph = TravelGraph();
    _nodeByTaxi.clear();
    {
        std::lock_guard lock(_blockedMutex);
        _blocked.clear();
        _failures.clear();
    }

    uint32 oldMSTime = getMSTime();
    sDangerMap->Build();
    AddHubNodes();
    AddTaxi();
    AddPortals();
    _graph.Finalize();
    _graph.ConnectNeighbours(NeighbourCount, NeighbourDistance, 7.0f);
    AddTransports();
    MarkTeams();
    AddAirLinks();
    RateWalkLinks();
    _built = true;

    TC_LOG_INFO("server.playerbots", "Playerbots: travel graph built in {} ms: {}", GetMSTimeDiffToNow(oldMSTime), Summary());
    LogSampleRoutes();
}

void TravelMgr::LogSampleRoutes() const
{
    struct Sample
    {
        char const* From;
        char const* To;
        uint8 Team;
    };
    static constexpr Sample Samples[] = { { "stormwind", "orgrimmar", 1 }, { "stormwind", "orgrimmar", 2 }, { "stormwind", "silvermoon", 1 },
        { "orgrimmar", "silvermoon", 2 }, { "stormwind", "darnassus", 1 } };

    for (Sample const& sample : Samples)
    {
        std::vector<uint32> from = FindNodes(sample.From, 1);
        std::vector<uint32> to = FindNodes(sample.To, 1);
        if (from.empty() || to.empty())
        {
            TC_LOG_INFO("server.playerbots", "Playerbots: sample route {} -> {}: no such flight master node", sample.From, sample.To);
            continue;
        }

        RouteOptions options;
        options.Team = sample.Team;
        options.Level = 80;
        options.Money = 10000000;
        options.RunSpeed = MountedRunSpeed;
        Route route = _graph.Plan(from.front(), to.front(), options);
        if (!route.Found)
        {
            TC_LOG_INFO("server.playerbots", "Playerbots: sample route {} -> {} for the {}: none", _graph.GetNode(from.front()).Name,
                _graph.GetNode(to.front()).Name, sample.Team == 1 ? "Alliance" : "Horde");
            continue;
        }

        uint32 flights = 0;
        uint32 boats = 0;
        uint32 walks = 0;
        for (RouteLeg const& leg : route.Legs)
        {
            flights += leg.Type == LinkType::Taxi;
            boats += leg.Type == LinkType::Transport;
            walks += leg.Type != LinkType::Taxi && leg.Type != LinkType::Transport;
        }
        TC_LOG_INFO("server.playerbots", "Playerbots: sample route {} -> {} for the {}: {:.1f} min, {} flights, {} boats, {} other legs", _graph.GetNode(from.front()).Name,
            _graph.GetNode(to.front()).Name, sample.Team == 1 ? "Alliance" : "Horde", route.Seconds / 60.0f, flights, boats, walks);
    }
}

std::string TravelMgr::Summary() const
{
    uint32 kinds[6] = {};
    for (uint32 i = 0; i < _graph.NodeCount(); ++i)
        ++kinds[uint8(_graph.GetNode(i).Kind)];

    uint32 types[6] = {};
    for (uint32 i = 0; i < _graph.LinkCount(); ++i)
        ++types[uint8(_graph.GetLink(i).Type)];

    return Trinity::StringFormat("{} nodes ({} towns, {} flight masters, {} portal mouths), {} links ({} walk, {} flight, {} boat or zeppelin, {} portal, {} air)", _graph.NodeCount(),
        kinds[uint8(NodeKind::Hub)], kinds[uint8(NodeKind::FlightMaster)], kinds[uint8(NodeKind::PortalMouth)], _graph.LinkCount(),
        types[uint8(LinkType::Walk)], types[uint8(LinkType::Taxi)], types[uint8(LinkType::Transport)], types[uint8(LinkType::Portal)], types[uint8(LinkType::Air)]);
}

bool TravelMgr::IsBlocked(uint32 linkId) const
{
    std::lock_guard lock(_blockedMutex);
    auto itr = _blocked.find(linkId);
    return itr != _blocked.end() && int32(itr->second - GameTime::GetGameTimeMS()) > 0;
}

// Every failure in a row blocks the link twice as long as the one before, up to a day: a bridge that is gone stays
// out of the plans, a link that failed once because a bot was busy comes back soon
void TravelMgr::BlockLink(uint32 linkId, uint32 forMs)
{
    std::lock_guard lock(_blockedMutex);
    uint32& failures = _failures[linkId];
    uint64 duration = uint64(forMs) << std::min<uint32>(failures, 8);
    ++failures;
    _blocked[linkId] = GameTime::GetGameTimeMS() + uint32(std::min<uint64>(duration, 24ull * 3600ull * 1000ull));
}

void TravelMgr::LinkWorked(uint32 linkId)
{
    std::lock_guard lock(_blockedMutex);
    _failures.erase(linkId);
    _blocked.erase(linkId);
}

RouteOptions TravelMgr::OptionsFor(Player const* bot) const
{
    RouteOptions options;
    options.Team = TeamCode(Player::TeamForRace(bot->GetRace()));
    options.Level = uint8(std::min<uint32>(bot->GetLevel(), 255));
    options.Money = bot->GetMoney();
    options.RunSpeed = bot->GetLevel() >= sPlayerbotsConfig.RpgMountMinLevel ? MountedRunSpeed : 7.0f;
    options.DangerSecondsPerLevel = sPlayerbotsConfig.TravelDangerSecondsPerLevel;
    options.DangerBlockLevels = uint8(sPlayerbotsConfig.TravelDangerBlockLevels);
    options.Blocked = [this](TravelLink const& link) { return IsBlocked(link.Id); };
    // air legs: only a bot on the test list that could really take to the air (riding skill and a flying mount of its account); the
    // collection of a random bot is account wide, so "it has no flying mount" is not a reason to leave the rule out
    options.AllowAir = sPlayerbotsConfig.AirEnable && AirListed(bot->GetName()) && bot->GetSkillValue(SKILL_RIDING) >= FlyingMounts::MinRidingSkill
        && !FlyingMounts::Known(bot).empty();
    return options;
}

std::vector<TravelLink> TravelMgr::HearthLinks(Player const* bot) const
{
    std::vector<TravelLink> links;
    if (!bot->HasItemCount(HearthstoneItem) || bot->GetSpellHistory()->HasCooldown(HearthstoneSpell))
        return links;

    WorldLocation const& home = bot->m_homebind;
    uint32 node = NearestNode(home.GetMapId(), home.GetPositionX(), home.GetPositionY(), 800.0f);
    if (node == UINT32_MAX)
        return links;

    TravelLink link;
    link.From = UINT32_MAX;
    link.To = node;
    link.Type = LinkType::Teleport;
    link.Seconds = HearthSeconds + TravelGraph::WalkSeconds(std::hypot(_graph.GetNode(node).X - home.GetPositionX(), _graph.GetNode(node).Y - home.GetPositionY()), 7.0f);
    link.Ref = HearthstoneSpell;
    links.push_back(link);
    return links;
}

Route TravelMgr::PlanFor(Player const* bot, Position3 const& goal, bool noAir) const
{
    if (!_built)
        return {};

    Position3 start;
    start.Map = bot->GetMapId();
    start.X = bot->GetPositionX();
    start.Y = bot->GetPositionY();
    start.Z = bot->GetPositionZ();
    RouteOptions options = OptionsFor(bot);
    if (noAir)
        options.AllowAir = false;
    return _graph.PlanPositions(start, goal, options, HearthLinks(bot));
}

TravelMgr::BotReach TravelMgr::ReachFor(Player const* bot) const
{
    BotReach reach;
    if (!_built)
        return reach;

    reach.Options = OptionsFor(bot);
    reach.Costs = _graph.ComputeReach({ bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() }, reach.Options, HearthLinks(bot));
    reach.Valid = true;
    return reach;
}

bool TravelMgr::Estimate(BotReach const& reach, Position3 const& goal, float& cost, float& seconds) const
{
    return reach.Valid && _graph.ReachPosition(reach.Costs, goal, reach.Options, cost, seconds);
}

std::vector<std::string> TravelMgr::Describe(Route const& route) const
{
    std::vector<std::string> lines;
    if (!route.Found)
    {
        lines.push_back("no route");
        return lines;
    }

    auto name = [&](uint32 id, char const* edge) -> std::string { return id == UINT32_MAX ? edge : _graph.GetNode(id).Name; };
    for (RouteLeg const& leg : route.Legs)
        lines.push_back(Trinity::StringFormat("{} {} -> {} ({:.0f} s)", TypeName(leg.Type), name(leg.From, "start"), name(leg.To, "goal"), leg.Seconds));

    lines.push_back(Trinity::StringFormat("total {:.0f} s ({:.1f} min)", route.Seconds, route.Seconds / 60.0f));
    return lines;
}

std::vector<uint32> TravelMgr::FindNodes(std::string_view text, uint32 limit) const
{
    std::string needle(text);
    strToLower(needle);
    std::vector<uint32> found;
    for (uint32 i = 0; i < _graph.NodeCount() && found.size() < limit; ++i)
    {
        std::string name = _graph.GetNode(i).Name;
        strToLower(name);
        if (name.find(needle) != std::string::npos)
            found.push_back(i);
    }
    return found;
}
}
