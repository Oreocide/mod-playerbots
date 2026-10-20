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

#ifndef PlayerbotsTravelMgr_h__
#define PlayerbotsTravelMgr_h__

#include "Graph/TravelGraph.h"
#include "Define.h"
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Player;

namespace Playerbots
{
/// The travel graph of the world and the routes through it. Built once at startup from the hub index (towns),
/// the taxi network, area trigger portals; read-only afterwards except the short list of links that failed.
class TravelMgr
{
public:
    static TravelMgr* instance();

    /// After HubIndex::Build. World thread
    void Build();
    bool IsBuilt() const { return _built; }
    TravelGraph const& GetGraph() const { return _graph; }

    /// What this bot may use: its team and level, its money, how fast it runs (mount), links that failed lately
    RouteOptions OptionsFor(Player const* bot) const;
    /// Hearthstone as a way out of where the bot stands, empty if it has none or it is on cooldown
    std::vector<TravelLink> HearthLinks(Player const* bot) const;
    /// noAir: leave the air links out (an air leg of this route failed)
    Route PlanFor(Player const* bot, Position3 const& goal, bool noAir = false) const;

    /// Costs from where the bot stands to the whole graph: rank many goals with one pass (see Estimate)
    struct BotReach
    {
        Reach Costs;
        RouteOptions Options;
        bool Valid = false;
    };
    BotReach ReachFor(Player const* bot) const;
    /// Planner cost and travel time to a goal, false if the bot has no way there
    bool Estimate(BotReach const& reach, Position3 const& goal, float& cost, float& seconds) const;

    /// A link that did not work (no path, flight refused, boat never came): not offered for a while
    void BlockLink(uint32 linkId, uint32 forMs = 5 * 60 * 1000);
    bool IsBlocked(uint32 linkId) const;
    /// A bot finished a link: it is fine, forget earlier failures
    void LinkWorked(uint32 linkId);

    /// A place a team's bots must not walk to: inside a town whose creatures attack that team
    bool IsHostileGround(uint32 mapId, float x, float y, uint8 team) const;

    /// One line per leg, for the debug command
    std::vector<std::string> Describe(Route const& route) const;
    /// Node ids whose name contains the text
    std::vector<uint32> FindNodes(std::string_view text, uint32 limit) const;
    /// Counts of nodes and links by kind
    std::string Summary() const;

private:
    void AddHubNodes();
    void AddTaxi();
    void AddPortals();
    /// Boats and zeppelins as links between the ground nodes next to their docks (after Finalize)
    void AddTransports();
    /// Playerbots.Air.Enable: air links between towns of the continents where steady flight is checked (Eastern Kingdoms, Kalimdor)
    void AddAirLinks();
    /// Teams of the nodes (enemy towns are closed to the other team) and the danger along each walk link
    void MarkTeams();
    void RateWalkLinks();
    /// A few routes between well known places, to the log: a sanity check of the data
    void LogSampleRoutes() const;
    uint32 NearestNode(uint32 mapId, float x, float y, float maxDistance) const;

    TravelGraph _graph;
    bool _built = false;
    std::unordered_map<uint32, uint32> _nodeByTaxi;      // taxi node id -> graph node
    mutable std::mutex _blockedMutex;                    // bots fail on map threads
    std::unordered_map<uint32, uint32> _blocked;         // link id -> game time ms until it is blocked
    std::unordered_map<uint32, uint32> _failures;        // link id -> failures in a row, each one blocks the link longer
};
}

#define sTravelMgr Playerbots::TravelMgr::instance()

#endif // PlayerbotsTravelMgr_h__
