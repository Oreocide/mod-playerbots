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

#include "RouteFollower.h"
#include "StuckDiagnosis.h"
#include "GameTime.h"
#include "Item.h"
#include "MotionMaster.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "SpellDefines.h"
#include "StringFormat.h"
#include "Taxi.h"
#include "MoveToward.h"
#include "TravelMgr.h"
#include <algorithm>
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr uint32 MaxReplans = 3;
constexpr uint32 HearthItem = 6948;
constexpr uint32 HearthSpell = 8690;
constexpr float NodeArrive = 8.0f;          // yards from a node that count as being there
constexpr float TaxiBoard = 20.0f;          // yards from a flight master node to take the flight
constexpr uint32 HearthWaitMs = 25000;
constexpr uint32 PortalWaitMs = 12000;
}

bool RouteFollower::Begin(PlayerbotAI& ai, Position3 const& goal, float arriveYards)
{
    Cancel();
    Player* bot = ai.GetBot();

    // never into a town whose creatures attack the bot
    if (sTravelMgr->IsHostileGround(goal.Map, goal.X, goal.Y, bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2))
        return false;

    Route route = sTravelMgr->PlanFor(bot, goal);
    if (!route.Found || route.Seconds > float(sPlayerbotsConfig.TravelMaxRouteMinutes) * 60.0f)
        return false;

    _route = std::move(route);
    _goal = goal;
    _arrive = arriveYards;
    _leg = 0;
    _replans = 0;
    _noHearth = false;
    _active = true;
    _virtualStarted = false;
    NextLeg();
    return true;
}

void RouteFollower::Cancel()
{
    _active = false;
    _route = Route();
    _leg = 0;
    _flying = false;
    _casting = false;
}

void RouteFollower::NextLeg()
{
    _legStarted = GameTime::GetGameTimeMS();
    _lastProgress = _legStarted;
    _bestDistance = 0.0f;
    _flying = false;
    _casting = false;
    _waitUntil = 0;
}

std::string RouteFollower::Summary() const
{
    uint32 flights = 0;
    uint32 portals = 0;
    bool hearth = false;
    for (RouteLeg const& leg : _route.Legs)
    {
        flights += leg.Type == LinkType::Taxi;
        portals += leg.Type == LinkType::Portal;
        hearth = hearth || leg.Type == LinkType::Teleport;
    }

    std::string text = Trinity::StringFormat("about {:.0f} min", std::max(1.0f, _route.Seconds / 60.0f));
    if (flights)
        text += Trinity::StringFormat(", {} flight{}", flights, flights > 1 ? "s" : "");
    if (portals)
        text += Trinity::StringFormat(", {} portal{}", portals, portals > 1 ? "s" : "");
    if (hearth)
        text += ", hearthstone";
    return text;
}

float RouteFollower::RemainingSeconds() const
{
    float seconds = 0.0f;
    for (size_t i = _leg; i < _route.Legs.size(); ++i)
        seconds += _route.Legs[i].Seconds;
    return seconds;
}

bool RouteFollower::Replan(PlayerbotAI& ai)
{
    if (++_replans > MaxReplans)
        return false;

    Route route;
    Player* bot = ai.GetBot();
    // the hearthstone is not offered again after it failed: plan without the extra link
    if (_noHearth)
    {
        Position3 start{ bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
        route = sTravelMgr->GetGraph().PlanPositions(start, _goal, sTravelMgr->OptionsFor(bot));
    }
    else
        route = sTravelMgr->PlanFor(bot, _goal);

    if (!route.Found)
        return false;

    _route = std::move(route);
    _leg = 0;
    NextLeg();
    return true;
}

RouteStatus RouteFollower::Finish(PlayerbotAI& ai, bool arrived)
{
    Cancel();
    _lastResult = arrived ? RouteStatus::Arrived : RouteStatus::Failed;
    _lastEnded = GameTime::GetGameTimeMS();
    ai.ClearChaseTarget();
    return arrived ? RouteStatus::Arrived : RouteStatus::Failed;
}

// where a leg ends: the position of its end node, or the goal for the last leg
bool RouteFollower::LegTarget(RouteLeg const& leg, Position3& position) const
{
    if (leg.To == UINT32_MAX)
    {
        position = _goal;
        return true;
    }

    TravelNode const& node = sTravelMgr->GetGraph().GetNode(leg.To);
    position = { node.Map, node.X, node.Y, node.Z };
    return true;
}

RouteStatus RouteFollower::WalkTo(PlayerbotAI& ai, Position3 const& target, float arrive, bool lastLeg)
{
    Player* bot = ai.GetBot();
    uint32 now = GameTime::GetGameTimeMS();
    if (bot->GetMapId() != target.Map)
        return RouteStatus::Failed;

    Position dest(target.X, target.Y, target.Z);
    float distance = bot->GetExactDist2d(dest);
    if (distance <= arrive)
        return RouteStatus::Arrived;

    // making headway is progress; a fight or a cast is not time lost
    // 5 yards of headway count: a bot that circles an obstacle gains a yard now and then and must not look busy forever
    if (_bestDistance <= 0.0f || distance + 5.0f < _bestDistance || bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_CASTING))
    {
        _bestDistance = distance;
        _lastProgress = now;
    }

    if (now - _lastProgress >= sPlayerbotsConfig.RpgStuckTime)
    {
        TC_LOG_INFO("server.playerbots.stuck", "Playerbots: route of {} made no headway for {} ms: map {} at ({:.1f}, {:.1f}, {:.1f}), goal ({:.1f}, {:.1f}, {:.1f}), {:.0f} yd to go; {}",
            bot->GetName(), now - _lastProgress, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), distance, DescribeStuck(bot, nullptr));
        return RouteStatus::Failed;
    }

    if (bot->IsInCombat() || bot->IsInFlight())
        return RouteStatus::Running;

    if (!IsMovingOnFoot(bot))
    {
        if (bot->GetVictim())
            bot->AttackStop();
        UpdateMountForTravel(bot, distance);
        MoveToward(bot, dest, std::min(arrive, 5.0f));
    }

    (void)lastLeg;
    return RouteStatus::Running;
}

RouteStatus RouteFollower::StepTaxi(PlayerbotAI& ai, RouteLeg const& leg)
{
    Player* bot = ai.GetBot();
    uint32 now = GameTime::GetGameTimeMS();
    TravelGraph const& graph = sTravelMgr->GetGraph();

    if (_flying)
    {
        if (bot->IsInFlight())
            return RouteStatus::Running;

        // landed
        ai.NoteExpectedMove();
        return RouteStatus::Arrived;
    }

    TravelNode const& from = graph.GetNode(leg.From);
    Position3 at{ from.Map, from.X, from.Y, from.Z };
    if (bot->GetMapId() != from.Map || bot->GetExactDist2d(Position(from.X, from.Y, from.Z)) > TaxiBoard)
        return WalkTo(ai, at, TaxiBoard - 6.0f, false);

    // flights are free for bots unless told otherwise: a bot without the gold gets it
    if (sPlayerbotsConfig.TravelFreeFlights && bot->GetMoney() < 200000)
        bot->ModifyMoney(200000);

    if (TakeTaxi(bot, from.Ref, leg.Ref))
    {
        _flying = true;
        _lastProgress = now;
        ai.NoteExpectedMove();
        return RouteStatus::Running;
    }

    // refused (a flight master it cannot use, a path it may not take): not offered again for a while
    if (leg.LinkId != UINT32_MAX)
        sTravelMgr->BlockLink(leg.LinkId);
    return RouteStatus::Failed;
}

RouteStatus RouteFollower::StepPortal(PlayerbotAI& ai, RouteLeg const& leg)
{
    Player* bot = ai.GetBot();
    uint32 now = GameTime::GetGameTimeMS();
    TravelGraph const& graph = sTravelMgr->GetGraph();
    TravelNode const& mouth = graph.GetNode(leg.From);
    TravelNode const& exit = graph.GetNode(leg.To);

    if (_waitUntil)
    {
        // teleported: the map or the place changed
        if (bot->GetMapId() == exit.Map && bot->GetExactDist2d(Position(exit.X, exit.Y, exit.Z)) < 40.0f)
            return RouteStatus::Arrived;
        return int32(now - _waitUntil) >= 0 ? RouteStatus::Failed : RouteStatus::Running;
    }

    if (bot->GetMapId() != mouth.Map || bot->GetExactDist2d(Position(mouth.X, mouth.Y, mouth.Z)) > NodeArrive)
        return WalkTo(ai, { mouth.Map, mouth.X, mouth.Y, mouth.Z }, NodeArrive - 2.0f, false);

    ai.NoteExpectedMove();
    if (bot->TeleportTo(exit.Map, exit.X, exit.Y, exit.Z, bot->GetOrientation()))
    {
        _waitUntil = now + PortalWaitMs;
        return RouteStatus::Running;
    }

    if (leg.LinkId != UINT32_MAX)
        sTravelMgr->BlockLink(leg.LinkId);
    return RouteStatus::Failed;
}

// Nobody scripts the ride: the bot waits at the dock for the trip and is placed at the other one
RouteStatus RouteFollower::StepTransport(PlayerbotAI& ai, RouteLeg const& leg)
{
    Player* bot = ai.GetBot();
    uint32 now = GameTime::GetGameTimeMS();
    TravelGraph const& graph = sTravelMgr->GetGraph();
    TravelNode const& dock = graph.GetNode(leg.From);
    TravelNode const& other = graph.GetNode(leg.To);

    if (!_waitUntil)
    {
        if (bot->GetMapId() != dock.Map || bot->GetExactDist2d(Position(dock.X, dock.Y, dock.Z)) > NodeArrive)
            return WalkTo(ai, { dock.Map, dock.X, dock.Y, dock.Z }, NodeArrive - 2.0f, false);

        // at the dock: the trip takes as long as the transport does, within the hop limits
        float seconds = std::clamp(leg.Seconds, 15.0f, float(sPlayerbotsConfig.TravelHopMaxSeconds));
        _waitUntil = now + uint32(seconds * 1000.0f);
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        return RouteStatus::Running;
    }

    if (int32(now - _waitUntil) < 0)
        return RouteStatus::Running;

    ai.NoteExpectedMove();
    if (bot->TeleportTo(other.Map, other.X, other.Y, other.Z, bot->GetOrientation()))
        return RouteStatus::Arrived;

    if (leg.LinkId != UINT32_MAX)
        sTravelMgr->BlockLink(leg.LinkId);
    return RouteStatus::Failed;
}

RouteStatus RouteFollower::StepHearth(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    uint32 now = GameTime::GetGameTimeMS();
    if (_casting)
    {
        // the cast ends with a teleport: the position jumps
        if (!bot->HasUnitState(UNIT_STATE_CASTING) && !bot->IsBeingTeleported() && int32(now - _waitUntil) >= 0)
            return RouteStatus::Failed;
        if (!bot->HasUnitState(UNIT_STATE_CASTING) && bot->GetExactDist2d(Position(_hearthFrom.X, _hearthFrom.Y)) > 100.0f)
            return RouteStatus::Arrived;
        return RouteStatus::Running;
    }

    Item* item = bot->GetItemByEntry(HearthItem);
    if (!item)
        return RouteStatus::Failed;

    ai.NoteExpectedMove();
    _hearthFrom = { bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    if (bot->CastSpell(bot, HearthSpell, CastSpellExtraArgs().SetCastItem(item)) != SPELL_CAST_OK)
        return RouteStatus::Failed;

    _casting = true;
    _waitUntil = now + HearthWaitMs;
    return RouteStatus::Running;
}

RouteStatus RouteFollower::Step(PlayerbotAI& ai)
{
    if (!_active)
        return RouteStatus::Idle;

    // a player came near while the bot was between hops: it walks from where it stands
    _virtualStarted = false;

    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsBeingTeleported())
        return RouteStatus::Running;

    // done when at the goal, wherever the route is
    if (bot->GetMapId() == _goal.Map && bot->GetExactDist2d(Position(_goal.X, _goal.Y, _goal.Z)) <= _arrive && !_flying)
        return Finish(ai, true);

    while (_leg < _route.Legs.size())
    {
        RouteLeg const& leg = _route.Legs[_leg];
        RouteStatus status = RouteStatus::Failed;
        bool last = _leg + 1 == _route.Legs.size();
        switch (leg.Type)
        {
            case LinkType::Walk:
            {
                Position3 target;
                LegTarget(leg, target);
                status = WalkTo(ai, target, last ? _arrive : NodeArrive, last);
                break;
            }
            case LinkType::Taxi:
                status = StepTaxi(ai, leg);
                break;
            case LinkType::Portal:
                status = StepPortal(ai, leg);
                break;
            case LinkType::Teleport:
                status = StepHearth(ai);
                break;
            case LinkType::Transport:
                status = StepTransport(ai, leg);
                break;
        }

        if (status == RouteStatus::Running)
            return RouteStatus::Running;

        if (status == RouteStatus::Arrived)
        {
            // a link that was walked, flown or ridden is known to work
            if (leg.LinkId != UINT32_MAX)
                sTravelMgr->LinkWorked(leg.LinkId);
            ++_leg;
            NextLeg();
            continue;
        }

        // the leg failed: plan again from here, without the hearthstone if that is what failed
        if (leg.Type == LinkType::Teleport)
            _noHearth = true;
        else if (leg.Type == LinkType::Walk && leg.LinkId != UINT32_MAX)
            sTravelMgr->BlockLink(leg.LinkId);   // no way through, or no progress: other routes first
        if (!Replan(ai))
            return Finish(ai, false);
        return RouteStatus::Running;
    }

    return Finish(ai, bot->GetMapId() == _goal.Map && bot->GetExactDist2d(Position(_goal.X, _goal.Y, _goal.Z)) <= _arrive + 40.0f);
}

// The next jump: legs are taken until their travel time reaches the minimum hop, a leg that would push it past the
// maximum starts the next hop. The wait is the stretch's travel time within the limits
void RouteFollower::PlanHop()
{
    float hopMin = float(sPlayerbotsConfig.TravelHopMinSeconds);
    float hopMax = float(sPlayerbotsConfig.TravelHopMaxSeconds);

    float sum = 0.0f;
    size_t index = _leg;
    while (index < _route.Legs.size())
    {
        float seconds = _route.Legs[index].Seconds;
        if (sum > 0.0f && sum + seconds > hopMax)
            break;

        sum += seconds;
        ++index;
        if (sum >= hopMin)
            break;
    }

    _hopLegs = std::max<size_t>(index - _leg, 1);
    _virtualLeftMs = uint32(std::clamp(sum, hopMin, hopMax) * 1000.0f);
    LegTarget(_route.Legs[std::min(_leg + _hopLegs, _route.Legs.size()) - 1], _hopTarget);
    _virtualStarted = true;
}

RouteStatus RouteFollower::StepVirtual(PlayerbotAI& ai, uint32 diffMs)
{
    if (!_active)
        return RouteStatus::Idle;

    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInFlight())
        return RouteStatus::Running;

    if (_leg >= _route.Legs.size())
        return Finish(ai, true);

    if (!_virtualStarted)
        PlanHop();

    if (diffMs < _virtualLeftMs)
    {
        _virtualLeftMs -= diffMs;
        return RouteStatus::Running;
    }

    // the stretch is over: the bot is at the end of it
    bool moved = true;
    if (bot->GetMapId() != _hopTarget.Map || bot->GetExactDist2d(Position(_hopTarget.X, _hopTarget.Y, _hopTarget.Z)) > 5.0f)
    {
        ai.NoteExpectedMove();
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        moved = bot->TeleportTo(_hopTarget.Map, _hopTarget.X, _hopTarget.Y, _hopTarget.Z, bot->GetOrientation());
    }

    if (!moved)
        return Finish(ai, false);

    _leg += _hopLegs;
    NextLeg();
    _virtualStarted = false;
    return _leg >= _route.Legs.size() ? Finish(ai, true) : RouteStatus::Running;
}
}
