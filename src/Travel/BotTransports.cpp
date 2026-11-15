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

#include "BotTransports.h"
#include "AiRegistry.h"
#include "Cmd/ChatCommands.h"
#include "Cmd/ChatParser.h"
#include "Cmd/CommandText.h"
#include "Config.h"
#include "Util.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "TransportMgr.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementPackets.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Transport.h"
#include "Travel/MoveToward.h"
#include "Travel/NavSnap.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Playerbots::BotTransports
{
namespace
{
constexpr float MasterReach = 40.0f;      // a master farther than this on a transport is not followed aboard
constexpr float DeckStepYards = 12.0f;    // the last stretch from the dock onto the deck, the one move that is not a mesh path
constexpr float BoardYards = 2.5f;        // close enough to the master's spot on the deck to count as aboard
constexpr uint32 DeckStepTimeoutMs = 8000;
constexpr float BoardPriority = 70.0f;    // below eating (75) and healing (80), above everything that walks
constexpr float RidePriority = 65.0f;

Config sConfig;

/// Tells the clients around where the bot stands, transport included. A bot's movement status is never refreshed (no client), and
/// clients ignore a status without the unit's guid: the guid is filled for this packet alone (see Travel/PlayerJump.cpp)
void Broadcast(Player* bot)
{
    MovementInfo& info = bot->m_movementInfo;
    info.time = GameTime::GetGameTimeMS();
    info.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());
    info.guid = bot->GetGUID();
    WorldPackets::Movement::MoveUpdate update;
    update.Status = &info;
    bot->SendMessageToSet(update.Write(), true);
    info.guid.Clear();
}

GameObject* TransportObject(Player* player)
{
    TransportBase* base = player->GetTransport();
    if (!base)
        return nullptr;
    return player->GetMap()->GetGameObject(base->GetTransportGUID());
}

/// How much longer the transport waits at the stop it is paused at, 0 while it moves. A boat or zeppelin pauses by its path
/// (TransportTemplate segments, the way Transport::Update finds WaitingOnPauseWaypoint); Transport::IsStopped is only the flag of
/// a transport asked to stop (allowstopping), never set at a dock
uint32 PauseLeftMs(Transport const* transport)
{
    TransportTemplate const* path = sTransportMgr->GetTransportTemplate(transport->GetEntry());
    if (!path || !path->TotalPathTime)
        return 0;
    uint32 time = transport->GetTimer() % path->TotalPathTime;
    TransportPathLeg const* leg = path->GetLegForTime(time);
    if (!leg || leg->Segments.empty())
        return 0;
    for (auto segment = leg->Segments.begin(); segment != std::prev(leg->Segments.end()); ++segment)
    {
        if (time < segment->SegmentEndArrivalTimestamp)
            break;
        if (time < segment->SegmentEndArrivalTimestamp + segment->Delay)
            return segment->SegmentEndArrivalTimestamp + segment->Delay - time;
    }
    return 0;
}

/// Standing still at a stop: paused by its path, or stopped on request (an allowstopping transport)
bool Docked(Transport const* transport)
{
    return transport->IsStopped() || PauseLeftMs(transport) > 0;
}

/// Where a bot riding alone stands on the deck: above the transport's origin (over the water for a boat), at the floor its hull
/// model gives there (the dynamic tree; a probe from DeckProbeYards above, so the keel below is not it), the origin if none is found
constexpr float DeckProbeYards = 15.0f;
Position DeckSpot(Player const* bot, Transport const* transport)
{
    Position spot = transport->GetPosition();
    float probeZ = spot.GetPositionZ() + DeckProbeYards;
    float floor = bot->GetMap()->GetGameObjectFloor(bot->GetPhaseShift(), spot.GetPositionX(), spot.GetPositionY(), probeZ, DeckProbeYards + 5.0f);
    if (floor > spot.GetPositionZ() - 2.0f && floor < probeZ)
        spot.m_positionZ = floor;
    return spot;
}

/// Boats and zeppelins are boarded only while they wait at the dock; elevators and trams any time the master stands near
bool Boardable(GameObject* go)
{
    if (!go || !go->ToTransportBase())
        return false;
    if (Transport* transport = go->ToTransport())
        return Docked(transport);
    return true;
}

void Board(Player* bot, GameObject* go)
{
    TransportBase* base = go->ToTransportBase();
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    base->AddPassenger(bot, base->GetPositionOffsetTo(bot->GetPosition()));
    Broadcast(bot);
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} boarded {}", bot->GetName(), go->GetName());
}

void Leave(Player* bot)
{
    if (TransportBase* base = bot->GetTransport())
    {
        base->RemovePassenger(bot);
        Broadcast(bot);
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} left its transport", bot->GetName());
    }
}

// ---- random bots riding alone (Playerbots.Transport.RideAlone) ----
constexpr float DockReach = 80.0f;          // a stopped transport this close to its stop at the dock is the one to board
constexpr float StopSearch = 400.0f;        // the travel graph places a dock node up to this far from the stop (TravelMgr::AddTransports)
constexpr float PierStepYards = 30.0f;      // riding alone: the mesh ends on the pier, the hull's middle is up to this far (2D) off it
constexpr float PierVertical = 6.0f;        // a pier point is at most this far above or below the deck
constexpr float WaitLift = 8.0f;            // the pier by a stop (a boat's origin is at the waterline): searched this high above it, as far up and down
constexpr uint32 StepOffTimeoutMs = 8000;
constexpr uint32 StepOffResendMs = 1000;     // a step off the deck that is not moving after this is sent again
constexpr float StepOffArrive = 2.0f;        // the step off ends this close (2D) to its pier point

/// The pier point nearest a spot by the water: the ground navmesh only (a water polygon is never a pier), within PierStepYards around
/// it and `vertical` yards above or below. A riding bot waits there, steps aboard from there and steps off to it
Optional<Position> PierNear(Player const* bot, Position const& spot, float vertical)
{
    Position pier;
    if (!SnapPointToNavMesh(bot, spot, pier, PierStepYards, vertical) || pier.GetExactDist2d(spot) > PierStepYards)
        return {};
    return pier;
}

std::unordered_map<uint32, std::vector<ObjectGuid::LowType>> sSpawnsByEntry;    // OnStartup, read-only afterwards

enum class RidePhase : uint8
{
    Waiting,    // at the dock, for the transport
    Boarding,   // walking onto the deck
    Aboard,
    SteppingOff,    // off the deck at the far dock: the straight step to the pier
    Arrived     // stepped off at the far dock (or gave up aboard): the route's next step goes on
};

struct Ride
{
    RidePhase Phase = RidePhase::Waiting;
    uint32 Entry = 0;
    ObjectGuid Transport;
    WorldLocation To;
    Position Stop;              // where the transport stands at the near dock (its path's stop node)
    Position FarStop;           // and at the far one
    uint32 MaxMs = 0;           // the wait for it, and a third of the longest ride
    uint32 Since = 0;           // when the current phase began
    uint32 DeckStepAt = 0;
    uint32 LastLog = 0;         // the waiting line (every 15 s, for the headless checks)
    int32 LastWalk = -1;        // MoveResult of the last walk towards the pier, -1 none
    Optional<Position> WaitAt;  // the pier point by the near stop, once found
    Optional<Position> OffTo;   // the pier point a bot stepping off walks to
    uint32 OffMoveAt = 0;       // when that step was last sent
};

struct Declined
{
    uint32 Entry = 0;
    uint32 Until = 0;
};

std::mutex sRidesLock;          // map threads of different maps step their bots at the same time
std::unordered_map<ObjectGuid, Ride> sRides;
std::unordered_map<ObjectGuid, Declined> sDeclined;     // a transport the bot could not get onto: the dock wait for a while

Optional<Ride> GetRide(ObjectGuid bot)
{
    std::lock_guard lock(sRidesLock);
    auto itr = sRides.find(bot);
    return itr == sRides.end() ? Optional<Ride>{} : Optional<Ride>(itr->second);
}

void SetRide(ObjectGuid bot, Ride const& ride)
{
    std::lock_guard lock(sRidesLock);
    sRides[bot] = ride;
}

void EndRide(ObjectGuid bot)
{
    std::lock_guard lock(sRidesLock);
    sRides.erase(bot);
}

/// Gives up on riding this entry for a while, so a boarding that fails is not tried again at every step
void Decline(ObjectGuid bot, uint32 entry, uint32 forMs)
{
    std::lock_guard lock(sRidesLock);
    sRides.erase(bot);
    sDeclined[bot] = { entry, GameTime::GetGameTimeMS() + forMs };
}

bool IsDeclined(ObjectGuid bot, uint32 entry)
{
    std::lock_guard lock(sRidesLock);
    auto itr = sDeclined.find(bot);
    if (itr == sDeclined.end())
        return false;
    if (itr->second.Entry == entry && int32(itr->second.Until - GameTime::GetGameTimeMS()) > 0)
        return true;
    sDeclined.erase(itr);
    return false;
}

/// Riding alone: time to step off? At the far dock, or at any stop once the ride took three times its bound (a missed dock)
bool DueOff(Player* bot, Ride const& ride)
{
    GameObject* go = TransportObject(bot);
    Transport* transport = go ? go->ToTransport() : nullptr;
    if (!transport || !Docked(transport))
        return false;
    bool farDock = bot->GetMapId() == ride.To.GetMapId() && transport->GetExactDist2d(ride.FarStop) <= DockReach;
    return farDock || GameTime::GetGameTimeMS() - ride.Since > ride.MaxMs * 3;
}

/// Off the deck: the straight step to the nearest pier point (the deck step the other way, GAPS-bots G45 as widened 2026-11-15), or
/// where the bot stands when no pier is near (the route's next walk replans)
void StepOff(Player* bot, Ride& ride)
{
    Optional<Position> pier = PierNear(bot, bot->GetPosition(), PierVertical);
    float dist = pier ? bot->GetExactDist(*pier) : -1.0f;
    Leave(bot);
    ride.Since = GameTime::GetGameTimeMS();
    ride.OffTo = pier;
    if (pier)
    {
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, *pier, false);
        ride.OffMoveAt = ride.Since;
        ride.Phase = RidePhase::SteppingOff;
    }
    else
        ride.Phase = RidePhase::Arrived;
    TC_LOG_INFO("server.playerbots", "Playerbots: {} steps off at ({:.1f}, {:.1f}, {:.1f}): {}", bot->GetName(), bot->GetPositionX(),
        bot->GetPositionY(), bot->GetPositionZ(), pier ? Trinity::StringFormat("to the pier {:.1f} yd away", dist) : std::string("no pier within reach"));
}

/// Where a transport of this entry stands at the stop nearest `at`, and how long it pauses there. The stops are the pauses of its
/// path (TransportTemplate segments with a delay, at the time the transport arrives): a stop node of the taxi path that opens a leg
/// is no pause, and the dock node of the travel graph can be a town node far from the pier
Optional<Position> StopNear(uint32 entry, WorldLocation const& at, uint32* pauseMs = nullptr)
{
    TransportTemplate const* path = sTransportMgr->GetTransportTemplate(entry);
    if (!path || !path->TotalPathTime)
        return {};
    Optional<Position> best;
    float bestDist = StopSearch;
    for (TransportPathLeg const& leg : path->PathLegs)
    {
        if (leg.MapId != at.GetMapId() || leg.Segments.empty())
            continue;
        for (auto segment = leg.Segments.begin(); segment != std::prev(leg.Segments.end()); ++segment)
        {
            if (!segment->Delay)
                continue;
            Optional<Position> pos = path->ComputePosition(segment->SegmentEndArrivalTimestamp, nullptr, nullptr);
            if (!pos)
                continue;
            float dist = at.GetExactDist2d(*pos);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = *pos;
                if (pauseMs)
                    *pauseMs = segment->Delay;
            }
        }
    }
    return best;
}

/// One whole cycle of the transport's route, 0 if unknown
uint32 CycleMs(uint32 entry)
{
    TransportTemplate const* transportTemplate = sTransportMgr->GetTransportTemplate(entry);
    return transportTemplate ? transportTemplate->TotalPathTime : 0;
}

/// The transport of this entry (a boat has one spawn per route). Its guid, not the object: a boat that crosses to another map is not
/// on the bot's map for part of its cycle, so it is found again at every step
ObjectGuid TransportGuid(uint32 entry)
{
    auto itr = sSpawnsByEntry.find(entry);
    if (itr == sSpawnsByEntry.end() || itr->second.empty())
        return ObjectGuid::Empty;
    return ObjectGuid::Create<HighGuid::Transport>(itr->second.front());
}

class BoardTransportAction final : public Action
{
public:
    char const* GetName() const override { return "board transport"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Player* master = ai.GetMaster();
        GameObject* go = master ? TransportObject(master) : nullptr;
        if (!go || !Boardable(go) || bot->GetTransport() == go->ToTransportBase())
            return false;

        if (bot->GetTransport())
            Leave(bot);

        uint32 now = GameTime::GetGameTimeMS();
        Position spot = master->GetPosition();
        float dist = bot->GetExactDist(spot);

        if (dist <= BoardYards)
        {
            Board(bot, go);
            _deckStepAt = 0;
            return true;
        }

        // on the dock: the deck step, a straight move of a few yards (the deck has no navmesh). Allowed exception to
        // mmaps-only movement (GAPS-bots G45): only onto a transport, never off one, never for far bots
        if (dist <= DeckStepYards)
        {
            if (!_deckStepAt || now - _deckStepAt > DeckStepTimeoutMs)
            {
                _deckStepAt = now;
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, spot, false);
            }
            else if (!bot->isMoving())
                Board(bot, go);     // the step ended a little short (the master moved on the deck)
            return true;
        }

        _deckStepAt = 0;
        MoveResult result = MoveAlongMesh(bot, spot, DeckStepYards - 1.0f);
        return result != MoveResult::NoPath;
    }

private:
    uint32 _deckStepAt = 0;
};

bool BoardTrigger(PlayerbotAI& ai)
{
    if (!sConfig.Board)
        return false;
    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    if (!master || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->IsInFlight() || bot->GetVehicle() || !bot->IsInMap(master))
        return false;
    GameObject* go = TransportObject(master);
    if (!go || bot->GetTransport() == go->ToTransportBase() || !Boardable(go))
        return false;
    return bot->GetExactDist(master) <= MasterReach;
}

class RideTransportAction final : public Action
{
public:
    char const* GetName() const override { return "ride transport"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        GameObject* go = TransportObject(bot);
        if (!go)
            return false;

        Player* master = ai.GetMaster();
        if (!master)
        {
            // riding alone: off at the far dock, or at any stop once the ride took three times its bound (a missed dock)
            Optional<Ride> ride = GetRide(bot->GetGUID());
            if (!ride || ride->Phase != RidePhase::Aboard)
                return false;
            if (!DueOff(bot, *ride))
                return true;
            StepOff(bot, *ride);
            SetRide(bot->GetGUID(), *ride);
            return false;
        }
        // IsInMap first: a master on another map belongs to another map thread (MapUpdate.Threads > 1)
        bool masterAboard = master && bot->IsInMap(master) && master->GetTransport() == bot->GetTransport();
        if (!masterAboard && Boardable(go))
        {
            // the master stepped off at a stop: off too, the follow walks on from the deck
            Leave(bot);
            return false;
        }
        return true;    // stand still on deck: the transport carries the bot, no follow or walk while aboard
    }
};

bool RideTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || !bot->GetTransport() || !TransportObject(bot))
        return false;
    if (ai.GetMaster())
        return sConfig.Board;
    // a random bot riding alone: stands still aboard (nothing else may move a passenger) until the far dock
    Optional<Ride> ride = GetRide(bot->GetGUID());
    return ride && ride->Phase == RidePhase::Aboard;
}

class TransportStrategy final : public Strategy
{
public:
    TransportStrategy() : Strategy("transports") { }

    uint8 GetStates() const override { return StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "master on transport", { { "board transport", BoardPriority } }, StrategyStates::NonCombat });
        triggers.push_back({ "on transport", { { "ride transport", RidePriority } }, StrategyStates::NonCombat });
    }
};

// "board": the spike order (and the manual way): board the transport the master stands on, now, whatever the key says
bool BoardCommand(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    GameObject* go = master ? TransportObject(master) : nullptr;
    if (!go)
    {
        ai.TellMaster("You are not on a transport.");
        return true;
    }
    if (!bot->IsInMap(master) || bot->GetExactDist(master) > DeckStepYards)
    {
        ai.TellMaster("Too far from you to step on.");
        return true;
    }
    if (bot->GetTransport())
        Leave(bot);
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    Board(bot, go);
    ai.TellMaster(std::string("Aboard ") + go->GetName() + ".");
    return true;
}

class OrderAction final : public Action
{
public:
    using Fn = bool (*)(PlayerbotAI& ai, std::string const& param);
    OrderAction(std::string name, Fn fn) : _name(std::move(name)), _fn(fn) { }
    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI& ai) override { return _fn(ai, GetParam()); }

private:
    std::string _name;
    Fn _fn;
};
}

Config const& GetConfig()
{
    return sConfig;
}

void LoadConfig()
{
    Config cfg;
    cfg.Board = sConfigMgr->GetBoolDefault("Playerbots.Transport.Board", false);
    cfg.RideAlone = sConfigMgr->GetBoolDefault("Playerbots.Transport.RideAlone", false);
    std::string testBots = sConfigMgr->GetStringDefault("Playerbots.Transport.RideTestBots", "");
    for (std::string_view name : Trinity::Tokenize(testBots, ',', false))
    {
        std::string trimmed(name);
        std::erase(trimmed, ' ');
        if (!trimmed.empty())
            cfg.RideTestBots.insert(ToLower(trimmed));
    }
    sConfig = cfg;
}

void OnStartup()
{
    sSpawnsByEntry.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT guid, entry FROM transports"))
        do
            sSpawnsByEntry[(*result)[1].GetUInt32()].push_back((*result)[0].GetUInt64());
        while (result->NextRow());
}

bool IsRiding(ObjectGuid bot)
{
    std::lock_guard lock(sRidesLock);
    return sRides.contains(bot);
}

RideStep RideAlone(PlayerbotAI& ai, uint32 entry, WorldLocation const& from, WorldLocation const& to, uint32 maxMs)
{
    Player* bot = ai.GetBot();
    ObjectGuid guid = bot->GetGUID();
    uint32 now = GameTime::GetGameTimeMS();
    Optional<Ride> current = GetRide(guid);

    // a ride left over from a route that was given up before boarding (a fight, a new plan) is not this one
    if (current && current->Phase != RidePhase::Aboard && current->Phase != RidePhase::SteppingOff && current->Phase != RidePhase::Arrived
        && (current->Entry != entry || bot->GetMapId() != from.GetMapId()
            || (bot->GetExactDist2d(from.GetPositionX(), from.GetPositionY()) > StopSearch && bot->GetExactDist2d(current->Stop) > DockReach + DeckStepYards)))
    {
        EndRide(guid);
        current.reset();
    }

    if (!current)
    {
        // only where a human sees it: a far bot keeps the cheap dock wait (a test bot rides anyway, for headless checks)
        if (!sConfig.RideAlone || ai.GetMaster() || !ai.IsSimulated() || bot->IsInCombat() || bot->GetTransport() || IsDeclined(guid, entry))
            return RideStep::NotMine;
        if (ai.IsFarBot() && (sConfig.RideTestBots.empty() || !sConfig.RideTestBots.contains(ToLower(bot->GetName()))))
            return RideStep::NotMine;
        ObjectGuid transportGuid = TransportGuid(entry);
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(entry);
        if (transportGuid.IsEmpty() || !object)
        {
            TC_LOG_INFO("server.playerbots", "Playerbots: {} finds no spawn of transport {}, waits at the dock instead", bot->GetName(), entry);
            Decline(guid, entry, maxMs * 2);
            return RideStep::NotMine;
        }
        uint32 pauseMs = 0;
        uint32 farPauseMs = 0;
        Optional<Position> stop = StopNear(entry, from, &pauseMs);
        Optional<Position> farStop = StopNear(entry, to, &farPauseMs);
        if (!stop || !farStop)
        {
            TC_LOG_INFO("server.playerbots", "Playerbots: {} finds no stop of {} near the docks, waits at the dock instead", bot->GetName(), object->name);
            Decline(guid, entry, maxMs * 2);
            return RideStep::NotMine;
        }
        // the wait is bounded by the transport's own cycle (the route's trip time is capped by Travel.HopMaxSeconds)
        uint32 cycle = CycleMs(entry);
        Ride ride;
        ride.Entry = entry;
        ride.Transport = transportGuid;
        ride.To = to;
        ride.Stop = *stop;
        ride.FarStop = *farStop;
        ride.MaxMs = std::max(maxMs, cycle + 60000);
        TC_LOG_INFO("server.playerbots", "Playerbots: {} waits at the dock for {} (map {} to map {}, cycle {} s, stop {:.0f} yd from the dock node, pauses {} s there and {} s at the far one)",
            bot->GetName(), object->name, from.GetMapId(), to.GetMapId(), cycle / 1000, from.GetExactDist2d(*stop), pauseMs / 1000, farPauseMs / 1000);
        ride.Since = now;
        SetRide(guid, ride);
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        return RideStep::Running;
    }

    Ride ride = *current;
    if (ride.Phase == RidePhase::Aboard)
    {
        // off the deck by other means (a teleport, a death): the route goes on from wherever the bot is
        if (!bot->GetTransport())
        {
            EndRide(guid);
            return RideStep::Arrived;
        }
        // the ride action holds the bot still and steps it off; the same check here, whichever of the two runs
        if (!DueOff(bot, ride))
            return RideStep::Running;
        StepOff(bot, ride);
        SetRide(guid, ride);
        if (ride.Phase == RidePhase::SteppingOff)
            return RideStep::Running;
    }
    if (ride.Phase == RidePhase::SteppingOff)
    {
        // MovePoint starts its spline at the bot's next motion update, so "not moving" right after the step means "not yet"
        // (ride test 6: every bot was called Arrived on the deck and never left it): done at the pier or at the timeout; a step
        // that stopped or never started is sent again once a second
        float left = ride.OffTo ? bot->GetExactDist2d(*ride.OffTo) : 0.0f;
        if (left > StepOffArrive && now - ride.Since < StepOffTimeoutMs)
        {
            if (!IsMovingOnFoot(bot) && now - ride.OffMoveAt >= StepOffResendMs)
            {
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, *ride.OffTo, false);
                ride.OffMoveAt = now;
                SetRide(guid, ride);
            }
            return RideStep::Running;
        }
        if (left > StepOffArrive)
            TC_LOG_INFO("server.playerbots", "Playerbots: {} stops {:.1f} yd short of the pier after {} ms", bot->GetName(), left, now - ride.Since);
        ride.Phase = RidePhase::Arrived;
    }
    if (ride.Phase == RidePhase::Arrived)
    {
        // stepped off at the far dock: on along the mesh; the deck has none, so a bot that cannot path from it waits for the
        // route's next walk to replan (never a teleport in front of a human)
        EndRide(guid);
        MoveResult walk = bot->GetMapId() == to.GetMapId() ? MoveAlongMesh(bot, to, 2.0f) : MoveResult::NoPath;
        TC_LOG_INFO("server.playerbots", "Playerbots: {} stepped off on map {} at ({:.1f}, {:.1f}, {:.1f}), {:.0f} yd from the dock node, {}",
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            bot->GetMapId() == to.GetMapId() ? bot->GetExactDist(to) : -1.0f, walk == MoveResult::NoPath ? "no path off the deck" : "walks on");
        return RideStep::Arrived;
    }

    // waiting or boarding: it did not come within twice the trip (a boat that crosses to another map is not on this one for most
    // of its cycle, so it is waited for, not given up), or a fight started: the dock wait instead
    Transport* transport = bot->GetMap()->GetTransport(ride.Transport);
    if (now - ride.Since > ride.MaxMs || bot->IsInCombat())
    {
        TC_LOG_INFO("server.playerbots", "Playerbots: {} gives up on transport {} ({}), waits at the dock instead", bot->GetName(), entry,
            bot->IsInCombat() ? "a fight" : "it did not come in time");
        Decline(guid, entry, ride.MaxMs);
        return RideStep::NotMine;
    }
    if (!transport || !Docked(transport) || transport->GetExactDist2d(ride.Stop) > DockReach)
    {
        // wait on the pier by the stop, not at the town node: a docked transport leaves again within a minute or so. The pier is the
        // ground mesh nearest the stop above the water; without one, as near the stop as the mesh goes
        if (!ride.WaitAt)
            ride.WaitAt = PierNear(bot, Position(ride.Stop.GetPositionX(), ride.Stop.GetPositionY(), ride.Stop.GetPositionZ() + WaitLift), WaitLift);
        Position waitAt = ride.WaitAt ? *ride.WaitAt : ride.Stop;
        float arrive = ride.WaitAt ? 3.0f : DeckStepYards + 5.0f;
        if (ride.Phase != RidePhase::Waiting || (!IsMovingOnFoot(bot) && bot->GetExactDist2d(waitAt) > arrive + 2.0f))
            ride.LastWalk = int32(MoveAlongMesh(bot, waitAt, arrive));
        if (now - ride.LastLog >= 15000)
        {
            ride.LastLog = now;
            TC_LOG_INFO("server.playerbots", "Playerbots: {} at the dock for {}: {} {:.0f} yd from its stop, paused {} s more; the bot {:.0f} yd from the stop, {}, last walk {}",
                bot->GetName(), entry, transport ? "the transport is here," : "the transport is not on this map,",
                transport ? transport->GetExactDist2d(ride.Stop) : -1.0f, transport ? PauseLeftMs(transport) / 1000 : 0,
                bot->GetExactDist2d(ride.Stop), ride.WaitAt ? Trinity::StringFormat("the pier {:.0f} yd from the stop", ride.WaitAt->GetExactDist2d(ride.Stop)) : std::string("no pier"), ride.LastWalk);
        }
        ride.Phase = RidePhase::Waiting;
        ride.DeckStepAt = 0;
        SetRide(guid, ride);
        return RideStep::Running;
    }

    // the deck spot, reached the way a party bot steps aboard (BoardTransportAction): the mesh to the pier, then the deck step. The
    // pier edge can be farther than DeckStepYards from the hull's middle: from where the mesh ends, the step is up to PierStepYards
    Position spot = DeckSpot(bot, transport);
    float dist = bot->GetExactDist(spot);
    float dist2d = bot->GetExactDist2d(spot);
    Optional<Position> pier = PierNear(bot, spot, PierVertical);
    if (dist <= BoardYards || (ride.DeckStepAt && !bot->isMoving() && dist <= DeckStepYards))
    {
        Board(bot, transport);
        TC_LOG_INFO("server.playerbots", "Playerbots: {} rides {}, deck offset ({:.1f}, {:.1f}, {:.1f})", bot->GetName(), transport->GetName(),
            bot->m_movementInfo.transport.pos.GetPositionX(), bot->m_movementInfo.transport.pos.GetPositionY(), bot->m_movementInfo.transport.pos.GetPositionZ());
        ride.Phase = RidePhase::Aboard;
        ride.Since = now;
        SetRide(guid, ride);
        return RideStep::Running;
    }
    if (ride.Phase != RidePhase::Boarding)
        ride.LastWalk = -1;     // the waiting walk's result is not the boarding walk's
    if (ride.Phase != RidePhase::Boarding || now - ride.LastLog >= 5000)
    {
        ride.LastLog = now;
        TC_LOG_INFO("server.playerbots", "Playerbots: {} boards {}: {:.1f} yd from the deck spot ({:.1f} 2D, bot z {:.1f}, deck z {:.1f}, origin z {:.1f}, {}), paused {} s more, moving {}, deck step {}",
            bot->GetName(), transport->GetName(), dist, dist2d, bot->GetPositionZ(), spot.GetPositionZ(), transport->GetPositionZ(),
            pier ? Trinity::StringFormat("pier {:.1f} yd from the bot, {:.1f} from the deck spot", bot->GetExactDist(*pier), pier->GetExactDist2d(spot)) : std::string("no pier"),
            PauseLeftMs(transport) / 1000, IsMovingOnFoot(bot) ? "yes" : "no", ride.DeckStepAt ? "started" : "no");
    }
    ride.Phase = RidePhase::Boarding;

    auto deckStep = [&]()
    {
        ride.DeckStepAt = now;
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, spot, false);
    };
    if (ride.DeckStepAt || dist2d <= DeckStepYards)
    {
        if (!ride.DeckStepAt || now - ride.DeckStepAt > DeckStepTimeoutMs)
            deckStep();
        SetRide(guid, ride);
        return RideStep::Running;
    }
    // from the pier: the mesh to the pier point nearest the deck spot, the deck step from there. Swimming out to the hull only where
    // no pier is near or the mesh does not reach it
    if (pier && IsMovingOnFoot(bot) && ride.LastWalk != int32(MoveResult::NoPath))
    {
        SetRide(guid, ride);
        return RideStep::Running;
    }
    if (pier && ride.LastWalk != int32(MoveResult::NoPath))
    {
        MoveResult toPier = MoveAlongMesh(bot, *pier, 2.0f);
        ride.LastWalk = int32(toPier);
        if (toPier == MoveResult::Arrived)
            deckStep();
        if (toPier != MoveResult::NoPath)
        {
            SetRide(guid, ride);
            return RideStep::Running;
        }
    }
    if (!IsMovingOnFoot(bot))
    {
        MoveResult walk = MoveAlongMesh(bot, spot, DeckStepYards - 1.0f);
        if (walk == MoveResult::NoPath && dist2d <= PierStepYards)
            deckStep();     // the mesh ends at the pier edge: the step from there
        else if (walk == MoveResult::NoPath)
        {
            TC_LOG_INFO("server.playerbots", "Playerbots: {} has no path to the deck of {} ({:.1f} yd 2D from the deck spot, {:.1f} from the stop, bot z {:.1f}, deck z {:.1f}), waits at the dock instead",
                bot->GetName(), transport->GetName(), dist2d, bot->GetExactDist2d(ride.Stop), bot->GetPositionZ(), spot.GetPositionZ());
            Decline(guid, entry, maxMs * 2);
            return RideStep::NotMine;
        }
    }
    SetRide(guid, ride);
    return RideStep::Running;
}

bool IsRideTestBot(Player const* bot)
{
    return !sConfig.RideTestBots.empty() && sConfig.RideTestBots.contains(ToLower(bot->GetName()));
}

void ForgetRide(ObjectGuid bot)
{
    std::lock_guard lock(sRidesLock);
    sRides.erase(bot);
    sDeclined.erase(bot);
}

void Register(AiRegistry& registry)
{
    registry.AddTrigger("master on transport", 500, BoardTrigger);
    registry.AddTrigger("on transport", 1000, RideTrigger);
    registry.AddAction<BoardTransportAction>("board transport");
    registry.AddAction<RideTransportAction>("ride transport");
    registry.AddStrategy<TransportStrategy>("transports");

    std::string action = "cmd board";
    registry.AddAction(action, [action](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<OrderAction>(action, BoardCommand); });
    ChatCommandInfo info;
    info.Action = action;
    info.Help = "board: step onto the transport you stand on (boat, zeppelin, elevator, tram).";
    info.Persist = false;
    info.WhisperOnly = true;
    GetChatCommands().Add("board", info);
}
}
