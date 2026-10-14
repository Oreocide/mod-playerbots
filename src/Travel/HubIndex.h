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

#ifndef PlayerbotsHubIndex_h__
#define PlayerbotsHubIndex_h__

#include "Define.h"
#include "Position.h"
#include "SharedDefines.h"
#include <initializer_list>
#include <vector>

class Player;

namespace Playerbots
{
enum class HubKind : uint8
{
    FlightMaster,
    Banker,
    Auctioneer,
    Innkeeper,
    Vendor,
    Repair,
    Mailbox
};

constexpr uint32 HubKindCount = 7;

struct HubSpot
{
    HubKind Kind = HubKind::Vendor;
    uint32 MapId = 0;
    uint32 Entry = 0;
    Position Pos;
    int32 Cluster = -1;   // HubIndex::Clusters() index, -1 = a service NPC outside any town
};

/// What a town or city offers decides how bots use it: 1 capital (auction house, bank and inn), 2 town (inn),
/// 3 outpost (shops only), 4 flight master hub. 5 (transport hub) is added by the travel graph
enum class HubTier : uint8
{
    Capital = 1,
    Town = 2,
    Outpost = 3,
    FlightMaster = 4,
    Transport = 5
};

struct HubCluster
{
    uint32 Id = 0;
    uint32 MapId = 0;
    Position Center;
    float Radius = 0.0f;
    uint32 KindMask = 0;           // bit per HubKind
    uint32 Spots = 0;
    HubTier Tier = HubTier::Outpost;
    bool HostileToAlliance = false; // most of its NPCs attack that team: not a place for its bots
    bool HostileToHorde = false;

    bool Has(HubKind kind) const { return (KindMask & (1u << uint8(kind))) != 0; }
};

/// Service NPCs of the open world in the default phase, and the towns they form. Built once, read-only after
namespace HubIndex
{
void Build();
bool IsBuilt();

HubSpot const* Nearest(Player const* bot, HubKind kind, float maxDist = 4000.0f);
HubSpot const* NearestAny(Player const* bot, std::initializer_list<HubKind> kinds, float maxDist = 4000.0f);

std::vector<HubCluster> const& Clusters();
/// The cluster closest to a point (its edge, not its centre), nullptr if none within maxDist
HubCluster const* NearestCluster(uint32 mapId, Position const& from, float maxDist, Team team = TEAM_OTHER);
/// Whether bots of a team can use the cluster (its NPCs do not attack them)
bool IsOpenTo(HubCluster const& cluster, Team team);
/// Nearest spot of a kind inside one cluster
HubSpot const* NearestInCluster(HubCluster const& cluster, HubKind kind, Position const& from);
/// Up to maxCount positions of service NPCs of a cluster, spread over it (ground a bot can stand on).
/// social = where players stand around: auction house, bank, mailbox, inn and flight master spots are listed
/// several times (the more, the more bots arrive there), shops once
std::vector<Position> ClusterPoints(HubCluster const& cluster, uint32 maxCount, bool social = false);
}
}

#endif
