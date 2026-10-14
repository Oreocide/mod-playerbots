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

#ifndef Playerbots_HubClusters_h__
#define Playerbots_HubClusters_h__

#include <cstdint>
#include <vector>

// Groups service NPCs (innkeepers, bankers, vendors...) into towns and cities (HubIndex turns a group into a HubCluster). No core types, tested standalone.
namespace Playerbots
{
struct HubPoint
{
    uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    uint8_t Kind = 0;                 // bit index in the cluster's KindMask
    bool HostileToAlliance = false;   // the NPC attacks Alliance players
    bool HostileToHorde = false;
};

struct HubGroup
{
    uint32_t Map = 0;
    float X = 0.0f;                   // centroid
    float Y = 0.0f;
    float Radius = 0.0f;              // furthest member from the centroid
    uint32_t KindMask = 0;
    uint32_t Spots = 0;
    uint32_t HostileToAlliance = 0;   // members hostile to a faction
    uint32_t HostileToHorde = 0;
    std::vector<uint32_t> Members;    // indexes into the input
};

struct ClusterSettings
{
    float SeedRadius = 100.0f;        // neighbours counted to rate a seed
    float ClaimRadius = 150.0f;       // members of a cluster are within this of its seed
    uint32_t MinSpots = 3;
    uint32_t MinKinds = 2;            // a lone vendor row is not a town
};

/// Densest first: each seed claims the unclaimed points within ClaimRadius. A group that is too small or has
/// too few kinds of service is dropped
std::vector<HubGroup> BuildHubClusters(std::vector<HubPoint> const& points, ClusterSettings const& settings = {});
}

#endif // Playerbots_HubClusters_h__
