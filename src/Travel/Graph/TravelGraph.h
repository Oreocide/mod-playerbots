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

#ifndef Playerbots_TravelGraph_h__
#define Playerbots_TravelGraph_h__

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Places a bot can travel between and the ways to do it, with an A* planner. No core types, tested standalone.
// Nodes are cities, flight masters, boat docks and portal mouths; a link is one way to get from one to another
// (walking, a flight path, a boat, a portal, a hearthstone) with its cost in seconds and who may use it.
namespace Playerbots
{
enum class LinkType : uint8_t
{
    Walk,
    Taxi,        // flight path, Ref = taxi node id of the destination
    Transport,   // boat, zeppelin, elevator, Ref = transport entry
    Portal,      // area trigger or portal object, Ref = area trigger id
    Teleport,    // hearthstone or a teleport spell
    Air          // a flight on the bot's own flying mount along a straight line (Playerbots.Air.*), cost = distance / air speed + takeoff and landing
};

enum class NodeKind : uint8_t
{
    Hub,         // town or city
    FlightMaster,
    Dock,        // where a transport stops
    PortalMouth, // where a portal is entered or left
    Home,        // a bot's hearthstone destination
    Waypoint
};

struct TravelNode
{
    uint32_t Id = 0;
    uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    NodeKind Kind = NodeKind::Waypoint;
    std::string Name;
    uint32_t Ref = 0;        // taxi node id of a flight master, transport entry of a dock...
    uint8_t Team = 0;        // 0 open to both, 1 Alliance only, 2 Horde only, 3 closed to everyone (enemy ground)
};

struct TravelLink
{
    uint32_t From = 0;
    uint32_t To = 0;
    LinkType Type = LinkType::Walk;
    float Seconds = 0.0f;    // the whole trip over this link, waiting included
    uint32_t Ref = 0;
    uint8_t Team = 0;        // 0 both, 1 Alliance, 2 Horde
    uint8_t MinLevel = 0;
    uint32_t Gold = 0;       // copper
    uint32_t Id = 0;         // index in the graph, lets a caller remember "this link is blocked"
    uint8_t DangerAlliance = 0;   // highest level of a creature that attacks Alliance players near a walk link
    uint8_t DangerHorde = 0;
};

struct RouteOptions
{
    uint8_t Team = 0;                // 1 Alliance, 2 Horde, 0 = no team restriction
    uint8_t Level = 255;
    uint64_t Money = UINT64_MAX;     // copper the bot can spend on flights
    float RunSpeed = 7.0f;           // yards per second for the walk legs at the ends of a route
    bool AllowTaxi = true;
    bool AllowTransport = true;
    bool AllowPortal = true;
    bool AllowTeleport = true;
    bool AllowAir = false;           // only a bot that may and can fly (Playerbots.Air.*)
    float GoldSecondsPerCopper = 0.0f; // time a bot is willing to trade for gold, 0 = gold is free once affordable
    uint8_t DangerFreeLevels = 3;      // creatures this many levels above the bot or less are no reason to detour
    float DangerSecondsPerLevel = 30.0f; // each level above that makes a walk link this much longer
    uint8_t DangerBlockLevels = 12;    // creatures this far above the bot: the link is not walked at all
    std::function<bool(TravelLink const&)> Blocked;  // links that failed recently
};

struct RouteLeg
{
    uint32_t From = 0;
    uint32_t To = 0;
    LinkType Type = LinkType::Walk;
    float Seconds = 0.0f;
    uint32_t Ref = 0;
    uint32_t LinkId = 0;
};

struct Route
{
    bool Found = false;
    float Seconds = 0.0f;
    std::vector<RouteLeg> Legs;
};

struct Position3
{
    uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

/// What it costs to get from one position to every node of the graph, computed in one pass. Ranking many
/// goals (quests across the world) takes one of these per bot instead of one planner call per goal
struct Reach
{
    Position3 From;
    std::vector<float> Cost;      // planner cost of the best way to each node, infinity = no way
    std::vector<float> Seconds;   // the travel time of that way
};

class TravelGraph
{
public:
    /// Walking is slower than a straight line: roads wind, hills and buildings are in the way
    static constexpr float DetourFactor = 1.3f;
    /// Fastest way to cover distance (a flight path), keeps the A* heuristic from overestimating
    static constexpr float FastestSpeed = 45.0f;

    uint32_t AddNode(TravelNode node);                  // returns the id (the index)
    uint32_t AddLink(TravelLink link);                  // returns the link id
    /// Walk links in both directions between two nodes of one map, cost from their distance
    void AddWalkPair(uint32_t a, uint32_t b, float runSpeed = 7.0f);
    /// Links every node to its nearest neighbours of the same map (walking), call after all nodes exist
    void ConnectNeighbours(uint32_t neighbours, float maxDistance, float runSpeed = 7.0f);
    /// Call once the nodes are in: builds the lookup used by NearestNodes and the planner
    void Finalize();

    size_t NodeCount() const { return _nodes.size(); }
    size_t LinkCount() const { return _links.size(); }
    TravelNode const& GetNode(uint32_t id) const { return _nodes[id]; }
    TravelLink const& GetLink(uint32_t id) const { return _links[id]; }
    TravelLink& MutableLink(uint32_t id) { return _links[id]; }
    TravelNode& MutableNode(uint32_t id) { return _nodes[id]; }
    std::vector<uint32_t> const& LinksFrom(uint32_t node) const { return _out[node]; }

    /// Ids of up to count nodes of the map nearest to a point, within maxDistance, nearest first
    std::vector<uint32_t> NearestNodes(uint32_t map, float x, float y, uint32_t count, float maxDistance) const;

    /// Best route between two nodes
    Route Plan(uint32_t from, uint32_t to, RouteOptions const& options) const;

    /// Best route between two positions: it walks to a nearby node, rides the graph, walks from a node near the
    /// goal. extra = temporary links from the start (a hearthstone, a mount spell)
    Route PlanPositions(Position3 const& from, Position3 const& to, RouteOptions const& options, std::vector<TravelLink> const& fromExtra = {}) const;

    /// Costs from a position to all nodes (the walk to a few nearby nodes, then the graph); extra = temporary
    /// links from the start such as a hearthstone
    Reach ComputeReach(Position3 const& from, RouteOptions const& options, std::vector<TravelLink> const& fromExtra = {}) const;
    /// Cost and travel time to a goal position from a Reach; false if there is no way
    bool ReachPosition(Reach const& reach, Position3 const& goal, RouteOptions const& options, float& cost, float& seconds) const;

    static float WalkSeconds(float distance, float runSpeed);

private:
    bool Allowed(TravelLink const& link, RouteOptions const& options) const;
    bool NodeAllowed(uint32_t node, RouteOptions const& options) const;
    /// What the link costs the planner: its time, gold, and the danger along it
    float Cost(TravelLink const& link, RouteOptions const& options) const;

    std::vector<TravelNode> _nodes;
    std::vector<TravelLink> _links;
    std::vector<std::vector<uint32_t>> _out;
    std::unordered_map<uint32_t, std::vector<uint32_t>> _byMap;   // map -> node ids
    bool _finalized = false;
};
}

#endif // Playerbots_TravelGraph_h__
