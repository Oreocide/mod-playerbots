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

#ifndef PlayerbotsRouteFollower_h__
#define PlayerbotsRouteFollower_h__

#include "Graph/TravelGraph.h"
#include "Define.h"
#include "Position.h"
#include <string>
#include <vector>


namespace Playerbots
{
class PlayerbotAI;

enum class RouteStatus : uint8
{
    Idle,       // no route
    Running,
    Arrived,
    Failed
};

/// Takes a bot along a planned route: walks the walking legs, flies the flight paths, steps through portals,
/// uses the hearthstone. A leg that makes no progress is re-planned from where the bot stands; after a few of
/// those the bot gives up. A bot that nobody can see (it is frozen) does not walk at all: it jumps ahead along the
/// route a stretch at a time, after the travel time of that stretch (2 to 5 minutes), and is placed at the goal at
/// the end. Boats and zeppelins are never boarded: a bot waits at the dock and is placed at the other one.
/// State of one bot, lives in its PlayerbotAI.
class RouteFollower
{
public:
    /// Plans from the bot's position. false if there is no route or it takes longer than Travel.MaxRouteMinutes
    bool Begin(PlayerbotAI& ai, Position3 const& goal, float arriveYards = 8.0f);
    void Cancel();
    bool IsActive() const { return _active; }
    /// How the last route ended (Arrived or Failed) and when (game time ms, 0 = never): lets whoever asked for it,
    /// which does not run while the route does, find out
    RouteStatus GetLastResult() const { return _lastResult; }
    uint32 GetLastEnded() const { return _lastEnded; }
    Position3 const& GetGoal() const { return _goal; }
    /// What the planned route looks like, for the bot's reply
    std::string Summary() const;

    /// An air leg is in progress: the bot is, or is about to be, off the ground on its own flying mount. The fall watchdog, the ground snap
    /// and the interruption check of PlayerbotAI leave it alone while this is true
    bool InAir() const { return _air.Flying; }
    /// Every tick of a bot in the air, whether or not its route is stepped (the route is not stepped in combat): the checks that put it
    /// back on the ground. True if it did.
    bool TickAir(PlayerbotAI& ai, bool frozen);
    /// Puts a bot that is in the air back on the ground: the nearer of the takeoff and the landing point (both navmesh points), mount off.
    /// Logged under server.playerbots.air, separate from the fall rescue and ground snap lines
    void RecoverAir(PlayerbotAI& ai, char const* reason);

    /// One AI tick of a bot that is moving for real
    RouteStatus Step(PlayerbotAI& ai);
    /// A bot nobody can see: lets time pass, places the bot at the goal when the (shortened) route time is over
    RouteStatus StepVirtual(PlayerbotAI& ai, uint32 diffMs);

private:
    bool Replan(PlayerbotAI& ai);
    RouteStatus Finish(PlayerbotAI& ai, bool arrived);
    bool LegTarget(RouteLeg const& leg, Position3& position) const;
    RouteStatus WalkTo(PlayerbotAI& ai, Position3 const& target, float arrive, bool lastLeg);
    RouteStatus StepTaxi(PlayerbotAI& ai, RouteLeg const& leg);
    RouteStatus StepPortal(PlayerbotAI& ai, RouteLeg const& leg);
    RouteStatus StepHearth(PlayerbotAI& ai);
    RouteStatus StepTransport(PlayerbotAI& ai, RouteLeg const& leg);
    RouteStatus StepAir(PlayerbotAI& ai, RouteLeg const& leg);
    void PlanHop();
    void NextLeg();
    float RemainingSeconds() const;

    bool _active = false;
    Position3 _goal;
    float _arrive = 8.0f;
    Route _route;
    size_t _leg = 0;
    uint32 _replans = 0;
    bool _noHearth = false;
    bool _noAir = false;       // an air leg failed: planned again without air links
    uint32 _airTries = 0;      // mount attempts of the current air leg
    struct AirState
    {
        bool Flying = false;
        Position Takeoff;
        Position Landing;
        uint32 StartedMs = 0;
    } _air;

    // progress of the current leg
    uint32 _legStarted = 0;
    uint32 _lastProgress = 0;
    float _bestDistance = 0.0f;
    bool _flying = false;
    bool _casting = false;
    uint32 _waitUntil = 0;

    Position3 _hearthFrom;
    // virtual travel: the next jump along the route
    RouteStatus _lastResult = RouteStatus::Idle;
    uint32 _lastEnded = 0;
    uint32 _virtualLeftMs = 0;
    bool _virtualStarted = false;
    size_t _hopLegs = 0;
    Position3 _hopTarget;
};
}

#endif // PlayerbotsRouteFollower_h__
