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

#include "Battleground/BgIsle.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgPlanner.h"
#include "Battleground/BgPlay.h"
#include "Engine/AiRegistry.h"
#include "PlayerbotAI.h"
#include "Travel/MoveToward.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Vehicle.h"
#include <array>
#include <cmath>
#include <list>
#include <unordered_map>

// Data: worktrees/bg-ioc/IOC_DATA.md (2026-11-13), from battleground_isle_of_conquest.cpp, isle_of_conquest.h and the world DB of map 628.
// Gates: type 33 destructible buildings, 600001 hit points, hurt only by SPELL_EFFECT_GAMEOBJECT_DAMAGE (siege vehicles, Seaforium).
// Their "open" world state turns 1 when they fall (BG_IC_GATE_*_WS_OPEN). Teleporters: type 22 spell casters (66548 out, 66549 in)
// on each gate's side of the keep, usable from the start.

namespace Playerbots::Bg::Isle
{
namespace
{
constexpr uint32 DemolisherEntry = 34775;
constexpr uint32 RamSpell = 67441;          // cone 7 yd, 4000 gameobject damage, 4 s cooldown
constexpr uint32 BoulderSpell = 67440;      // trajectory at a spot, 2000 gameobject damage, 3 s cooldown
constexpr float PortalReach = 3.0f;
constexpr float GateStep = 3.0f;            // this close to a gate's inside or outside point, the bot steps through it
constexpr float CornerSkip = 6.0f;          // a corner the bot stands at is not a waypoint again
constexpr float VehicleSearch = 80.0f;       // the Workshop's Demolishers stand within this of its banner
constexpr float DriverReach = 250.0f;        // a bot this close to the Workshop may go and drive one
constexpr float BoardReach = 4.0f;
constexpr float RamArrive = 3.5f;
constexpr uint32 PortalRetryMs = 6000;      // used a teleporter and still inside after this: a failed use
constexpr uint32 PortalFailures = 3;        // failed uses in a row before the bot walks out as the mesh says (logged)
constexpr uint32 DriveStuckMs = 60000;      // a driver that got no closer for this long leaves the vehicle

struct Gate
{
    uint32 Entry;
    int32 WsOpen;
    Position Center;
    Position Inside;
    Position Outside;
};

struct Portal
{
    uint32 Entry;
    Position Pos;
};

struct Keep
{
    TeamId Owner;
    float MinX, MaxX, MinY, MaxY;
    std::array<Gate, 3> Gates;          // front, west, east
    std::array<Portal, 3> Leave;        // inside: out through the side of the gate with the same index
    std::array<Portal, 3> Enter;        // outside the gate with the same index: in
    std::array<Position, 2> Corners;    // just outside the two front corners: the way round the keep
    Position RamSpot;                   // where an enemy Demolisher rams the front gate from
};

// the back of each keep is cliff and sea, so a keep is a box open to the map's middle through its front and two side gates
std::array<Keep, 2> const Keeps =
{ {
    {
        TEAM_ALLIANCE, 180.0f, 413.5f, -903.3f, -762.8f,
        { {
            { 195698, 4323, { 413.5f, -834.0f, 48.5f }, { 402.0f, -834.0f, 48.9f }, { 424.0f, -834.0f, 48.5f } },
            { 195699, 4324, { 351.6f, -762.8f, 48.9f }, { 351.6f, -774.0f, 48.9f }, { 351.6f, -752.0f, 48.9f } },
            { 195700, 4325, { 351.0f, -903.3f, 48.9f }, { 351.0f, -892.0f, 48.9f }, { 351.0f, -914.0f, 48.9f } },
        } },
        { { { 195316, { 397.1f, -859.4f, 48.9f } }, { 195316, { 326.3f, -777.4f, 49.0f } }, { 195316, { 323.5f, -888.4f, 48.9f } } } },
        { { { 195315, { 425.7f, -857.1f, 48.5f } }, { 195315, { 324.6f, -749.1f, 49.4f } }, { 195315, { 311.9f, -914.0f, 48.8f } } } },
        { { { 428.0f, -748.0f, 48.0f }, { 428.0f, -918.0f, 48.0f } } },
        { 419.5f, -834.0f, 48.5f },
    },
    {
        TEAM_HORDE, 1150.9f, 1400.0f, -851.2f, -676.9f,
        { {
            { 195494, 4322, { 1150.9f, -762.6f, 47.5f }, { 1162.0f, -762.6f, 48.6f }, { 1140.0f, -762.6f, 47.5f } },
            { 195495, 4321, { 1217.9f, -676.9f, 47.6f }, { 1217.9f, -688.0f, 48.9f }, { 1217.9f, -666.0f, 47.6f } },
            { 195496, 4320, { 1218.7f, -851.2f, 48.3f }, { 1218.7f, -840.0f, 48.9f }, { 1218.7f, -862.0f, 48.3f } },
        } },
        { { { 195313, { 1158.8f, -746.2f, 48.6f } }, { 195313, { 1235.5f, -683.9f, 49.3f } }, { 195313, { 1233.3f, -844.5f, 48.9f } } } },
        { { { 195314, { 1143.2f, -779.6f, 48.6f } }, { 195314, { 1236.5f, -669.4f, 48.3f } }, { 195314, { 1235.1f, -857.9f, 48.9f } } } },
        { { { 1136.0f, -662.0f, 47.0f }, { 1136.0f, -866.0f, 47.0f } } },
        { 1144.9f, -762.6f, 47.5f },
    },
} };

int KeepOf(Position const& pos)
{
    for (size_t i = 0; i < Keeps.size(); ++i)
    {
        Keep const& keep = Keeps[i];
        if (pos.GetPositionX() >= keep.MinX && pos.GetPositionX() <= keep.MaxX && pos.GetPositionY() >= keep.MinY && pos.GetPositionY() <= keep.MaxY)
            return int(i);
    }
    return -1;
}

int KeepOfTeam(TeamId team)
{
    return team == TEAM_ALLIANCE ? 0 : team == TEAM_HORDE ? 1 : -1;
}

bool GateOpen(Map const* map, Gate const& gate)
{
    return map->GetWorldStateValue(gate.WsOpen) == 1;
}

bool AnyGateOpen(Map const* map, Keep const& keep)
{
    for (Gate const& gate : keep.Gates)
        if (GateOpen(map, gate))
            return true;
    return false;
}

/// The segment a-b crosses the keep's box (Liang-Barsky on the 2d box)
bool Crosses(Position const& a, Position const& b, Keep const& keep)
{
    float x0 = a.GetPositionX(), y0 = a.GetPositionY();
    float dx = b.GetPositionX() - x0, dy = b.GetPositionY() - y0;
    float t0 = 0.0f, t1 = 1.0f;
    float const p[4] = { -dx, dx, -dy, dy };
    float const q[4] = { x0 - keep.MinX, keep.MaxX - x0, y0 - keep.MinY, keep.MaxY - y0 };
    for (int i = 0; i < 4; ++i)
    {
        if (p[i] == 0.0f)
        {
            if (q[i] < 0.0f)
                return false;
            continue;
        }
        float t = q[i] / p[i];
        if (p[i] < 0.0f)
            t0 = std::max(t0, t);
        else
            t1 = std::min(t1, t);
        if (t0 > t1)
            return false;
    }
    return true;
}

/// Both ends outside the keeps: the first corner to go round a keep the straight way crosses, false when none is in the way
bool Round(Position const& from, Position const& to, Position& out)
{
    for (Keep const& keep : Keeps)
    {
        if (!Crosses(from, to, keep))
            continue;
        float best = 0.0f;
        bool found = false;
        for (Position const& corner : keep.Corners)
        {
            if (from.GetExactDist2d(corner) < CornerSkip || Crosses(from, corner, keep))
                continue;
            // a corner from which the rest of the way is clear wins over one that needs the other corner too
            float cost = from.GetExactDist2d(corner) + corner.GetExactDist2d(to) + (Crosses(corner, to, keep) ? 1000.0f : 0.0f);
            if (!found || cost < best)
            {
                best = cost;
                out = corner;
                found = true;
            }
        }
        if (found)
            return true;
    }
    return false;
}

/// From outside: to `where`, round any keep in the way
Position Around(Position const& from, Position const& where)
{
    Position corner;
    return Round(from, where, corner) ? corner : where;
}

struct PortalUse
{
    uint32 LastMs = 0;
    int KeepIndex = -1;
    uint32 Failures = 0;
    uint32 WalkUntilMs = 0;
};

std::unordered_map<ObjectGuid, PortalUse>& PortalUses()
{
    static std::unordered_map<ObjectGuid, PortalUse> uses;     // world thread only (battleground bots act there)
    return uses;
}

/// The teleporter at `portal` takes the bot through the keep wall. False: not in reach of it yet (the caller walks on), or its object
/// is not there
bool UsePortal(Player* bot, Portal const& portal, int keepIndex, uint32 now)
{
    GameObject* go = bot->FindNearestGameObject(portal.Entry, 10.0f);
    if (!go || !go->IsAtInteractDistance(bot, nullptr))
        return false;
    PortalUse& use = PortalUses()[bot->GetGUID()];
    if (use.LastMs && now - use.LastMs < PortalRetryMs)
        return true;                    // a use is running (the teleport comes a moment later)
    if (use.LastMs && now - use.LastMs < 20000 && use.KeepIndex == keepIndex && KeepOf(*bot) == keepIndex)
        ++use.Failures;                 // used one a moment ago and still on the same side
    else
        use.Failures = 0;
    use.LastMs = now;
    use.KeepIndex = keepIndex;
    if (use.Failures >= PortalFailures)
    {
        use.WalkUntilMs = now + 60000;
        use.Failures = 0;
        TC_LOG_WARN("server.playerbots", "Playerbots: {} could not use the Isle of Conquest teleporter {} at {:.0f} {:.0f} three times: walks as the mesh says for a minute",
            bot->GetName(), portal.Entry, portal.Pos.GetPositionX(), portal.Pos.GetPositionY());
        return false;
    }
    if (bot->IsMounted())
        bot->CancelMountAura();
    bot->StopMoving();
    go->Use(bot);
    TC_LOG_INFO("server.playerbots", "Playerbots: {} uses the Isle of Conquest teleporter {} at {:.0f} {:.0f} (failed uses before: {})", bot->GetName(),
        portal.Entry, portal.Pos.GetPositionX(), portal.Pos.GetPositionY(), use.Failures);
    return true;
}

/// The portal of the list that makes from -> portal -> exit -> where shortest (the exit is the gate side of the same index)
size_t BestPortal(std::array<Portal, 3> const& portals, std::array<Position, 3> const& exits, Position const& from, Position const& where)
{
    size_t best = 0;
    float bestCost = 0.0f;
    for (size_t i = 0; i < portals.size(); ++i)
    {
        float cost = from.GetExactDist2d(portals[i].Pos) + exits[i].GetExactDist2d(where);
        if (i == 0 || cost < bestCost)
        {
            best = i;
            bestCost = cost;
        }
    }
    return best;
}

// ---- siege ----

struct Driver
{
    ObjectGuid Vehicle;
    uint32 SinceMs = 0;
    uint32 NextRamMs = 0;
    uint32 NextBoulderMs = 0;
    uint32 LegMs = 0;
    uint32 BestMs = 0;                  // when it last got closer to the ram spot
    float Best = 0.0f;
    uint32 Casts = 0;
    uint32 BoardMs = 0;                 // the last EnterVehicle call; not seated a few seconds later = a refused board
    uint32 BoardTries = 0;
    uint32 BenchedUntilMs = 0;          // refused three times: no driving for a while
};

std::unordered_map<ObjectGuid, Driver>& Drivers()
{
    static std::unordered_map<ObjectGuid, Driver> drivers;      // world thread only
    return drivers;
}

int8 RowIndex(BgPlan const& plan, char const* key)
{
    if (!plan.Def)
        return NoObjective;
    for (size_t i = 0; i < plan.Def->Nodes.size() && i < plan.Objectives.size(); ++i)
        if (plan.Def->Nodes[i].Key == key)
            return int8(i);
    return NoObjective;
}

bool FreeDriverSeat(Creature const* vehicle, int8* seatOut = nullptr)
{
    Vehicle const* kit = vehicle->GetVehicleKit();
    if (!kit)
        return false;
    for (auto const& [id, seat] : kit->Seats)
        if (seat.SeatInfo && seat.SeatInfo->HasFlag(VEHICLE_SEAT_FLAG_CAN_CONTROL) && seat.IsEmpty())
        {
            if (seatOut)
                *seatOut = id;
            return true;
        }
    return false;
}

/// Drivers of the team on this map: seated in a Demolisher, or on the way to one
uint32 TeamDrivers(Player const* bot, TeamId team)
{
    uint32 count = 0;
    for (auto const& [guid, driver] : Drivers())
    {
        Player* other = ObjectAccessor::FindPlayer(guid);
        if (other && other != bot && other->IsAlive() && other->GetMap() == bot->GetMap() && TeamOf(other) == team)
            ++count;
    }
    return count;
}

bool Reserved(ObjectGuid vehicle, ObjectGuid except)
{
    for (auto const& [guid, driver] : Drivers())
        if (guid != except && driver.Vehicle == vehicle)
            if (Player* other = ObjectAccessor::FindPlayer(guid); other && other->IsAlive() && other->GetMapId() == MapId)
                return true;
    return false;
}

/// A free Demolisher of our side at the Workshop (searched around the Workshop, not the bot: the attackers move on as soon as it falls)
Creature* FindDemolisher(Player* bot, Position const& workshop)
{
    std::list<Creature*> found;
    bot->GetCreatureListWithEntryInGrid(found, DemolisherEntry, bot->GetExactDist2d(workshop) + VehicleSearch);
    Creature* best = nullptr;
    for (Creature* vehicle : found)
    {
        if (!vehicle->IsAlive() || !vehicle->GetVehicleKit() || vehicle->GetExactDist2d(workshop) > VehicleSearch || bot->IsHostileTo(vehicle)
            || !FreeDriverSeat(vehicle) || Reserved(vehicle->GetGUID(), bot->GetGUID()))
            continue;
        if (!best || bot->GetExactDist2d(vehicle) < bot->GetExactDist2d(best))
            best = vehicle;
    }
    return best;
}

bool DriveToward(Creature* base, Position const& goal)
{
    // in legs along the vehicle's own mesh route (the core turns a path of 74 points or more into a straight line)
    Position target = goal;
    if (base->GetExactDist2d(goal) > 80.0f)
        if (Optional<Position> leg = NextMeshLeg(base, goal, 80.0f))
            target = *leg;
    PathGenerator path(base);
    if (!path.CalculatePath(target.GetPositionX(), target.GetPositionY(), target.GetPositionZ()) || (path.GetPathType() & ~PATHFIND_NORMAL)
        || path.GetPath().size() < 2)
        return false;
    Movement::MoveSplineInit init(base);
    init.MovebyPath(path.GetPath());
    init.SetWalk(false);
    init.Launch();
    return true;
}

void LeaveVehicle(Player* bot, char const* why)
{
    TC_LOG_INFO("server.playerbots", "Playerbots: {} leaves its Isle of Conquest vehicle: {}", bot->GetName(), why);
    Drivers().erase(bot->GetGUID());
    bot->ExitVehicle();
}

/// One step of a seated bot: drive to the enemy front gate and ram it; leave once it is down
bool DriveSeated(Player* bot)
{
    Creature* base = bot->GetVehicleCreatureBase();
    if (!base)
        return false;
    if (base->GetEntry() != DemolisherEntry || !base->GetVehicleKit() || base->GetCharmerGUID() != bot->GetGUID())
    {
        LeaveVehicle(bot, "not the driver of a Demolisher");
        return true;
    }

    int enemy = KeepOfTeam(TeamOf(bot) == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE);
    if (enemy < 0)
        return false;
    Keep const& keep = Keeps[enemy];
    Gate const& gate = keep.Gates[0];
    uint32 now = GameTime::GetGameTimeMS();
    Driver& driver = Drivers()[bot->GetGUID()];
    if (driver.Vehicle != base->GetGUID() || !driver.SinceMs)
    {
        driver = Driver();
        driver.Vehicle = base->GetGUID();
        driver.SinceMs = now;
        driver.BestMs = now;
        driver.Best = base->GetExactDist2d(keep.RamSpot);
    }
    if (GateOpen(bot->GetMap(), gate))
    {
        LeaveVehicle(bot, "the enemy front gate is down");
        return true;
    }

    float dist = base->GetExactDist2d(keep.RamSpot);
    if (dist + 2.0f < driver.Best)
    {
        driver.Best = dist;
        driver.BestMs = now;
    }
    if (dist > RamArrive)
    {
        if (now - driver.BestMs > DriveStuckMs)
        {
            LeaveVehicle(bot, "it got no closer to the gate for a minute");
            return true;
        }
        if (base->movespline->Finalized() || now - driver.LegMs > 8000)
        {
            driver.LegMs = now;
            if (!DriveToward(base, keep.RamSpot))
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} finds no mesh path for its Demolisher to {:.0f} {:.0f}", bot->GetName(),
                    keep.RamSpot.GetPositionX(), keep.RamSpot.GetPositionY());
        }
        return true;
    }

    // at the gate: face it, ram whenever the ram is ready, throw a boulder in between
    if (!base->movespline->Finalized())
        base->StopMoving();
    // turn only when it is off: every SetFacingTo sends a movement packet to the clients around
    float facing = base->GetAbsoluteAngle(gate.Center);
    float off = std::fabs(Position::NormalizeOrientation(facing - base->GetOrientation()));
    if (std::min(off, float(2 * M_PI) - off) > 0.2f)
        base->SetFacingTo(facing);
    CastSpellExtraArgs args(TRIGGERED_NONE);
    args.SetOriginalCaster(bot->GetGUID());
    uint32 spellId = 0;
    if (int32(driver.NextRamMs - now) <= 0)
    {
        spellId = RamSpell;
        driver.NextRamMs = now + 4100;
    }
    else if (int32(driver.NextBoulderMs - now) <= 0)
    {
        spellId = BoulderSpell;
        driver.NextBoulderMs = now + 3100;
    }
    if (!spellId)
        return true;
    SpellCastResult result = base->CastSpell(gate.Center, spellId, args);
    ++driver.Casts;
    if (driver.Casts == 1 || driver.Casts % 20 == 0 || result != SPELL_CAST_OK)
    {
        GameObject const* go = base->FindNearestGameObject(gate.Entry, 40.0f);
        TC_LOG_INFO("server.playerbots", "Playerbots: {} casts {} at the gate {} from {:.1f} yd: result {}, gate health {} (cast {})", bot->GetName(),
            spellId, gate.Entry, base->GetExactDist2d(gate.Center), uint32(result), go ? go->GetGOValue()->Building.Health : 0u, driver.Casts);
    }
    if (result != SPELL_CAST_OK)
    {
        if (spellId == RamSpell)
            driver.NextRamMs = now + 1000;
        else
            driver.NextBoulderMs = now + 1000;
    }
    return true;
}

bool VehicleTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return bot->GetMapId() == MapId && bot->IsAlive() && bot->GetVehicleCreatureBase() && !ai.GetMaster();
}

class VehicleAction : public Action
{
public:
    char const* GetName() const override { return "bg vehicle"; }

    bool Execute(PlayerbotAI& ai) override { return DriveSeated(ai.GetBot()); }
};

/// The bot's fight is on the other side of a shut keep wall: no player gets there, so neither does the bot
bool WallTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (bot->GetMapId() != MapId || ai.GetMaster() || bot->GetVehicleBase())
        return false;
    Unit* target = bot->GetVictim();
    if (!target)
        return false;
    int mine = KeepOf(*bot), theirs = KeepOf(*target);
    if (mine == theirs)
        return false;
    Map const* map = bot->GetMap();
    return (mine >= 0 && !AnyGateOpen(map, Keeps[mine])) || (theirs >= 0 && !AnyGateOpen(map, Keeps[theirs]));
}

class WallAction : public Action
{
public:
    char const* GetName() const override { return "bg wall"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Unit* target = bot->GetVictim();
        if (!target)
            return false;
        ai.IgnoreEnemy(target->GetGUID(), 15000);
        ai.ClearForcedTarget();
        bot->AttackStop();
        bot->StopMoving();
        // on with its job (through the teleporter when that is the way)
        Play(bot, ai);
        return true;
    }
};
}

Step Next(Player* bot, Position const& where, Position& waypoint)
{
    Map const* map = bot->GetMap();
    uint32 now = GameTime::GetGameTimeMS();
    if (auto use = PortalUses().find(bot->GetGUID()); use != PortalUses().end() && int32(use->second.WalkUntilMs - now) > 0)
        return Step::Direct;

    int from = KeepOf(*bot);
    int to = KeepOf(where);
    if (from >= 0 && from == to)
        return Step::Direct;

    if (from >= 0)
    {
        // leaving keep `from`: through a broken gate (the one that makes the way shortest), else its owner's team by the teleporter
        // on the side of the target. Nobody else is inside a keep whose gates all stand
        Keep const& keep = Keeps[from];
        Gate const* best = nullptr;
        float bestCost = 0.0f;
        for (Gate const& gate : keep.Gates)
        {
            if (!GateOpen(map, gate))
                continue;
            float cost = bot->GetExactDist2d(gate.Inside) + gate.Outside.GetExactDist2d(where);
            if (!best || cost < bestCost)
            {
                best = &gate;
                bestCost = cost;
            }
        }
        if (best)
        {
            waypoint = bot->GetExactDist2d(best->Inside) > GateStep ? best->Inside : best->Outside;
            return Step::Waypoint;
        }
        if (TeamOf(bot) != keep.Owner)
            return Step::Direct;
        std::array<Position, 3> exits = { keep.Enter[0].Pos, keep.Enter[1].Pos, keep.Enter[2].Pos };
        Portal const& portal = keep.Leave[BestPortal(keep.Leave, exits, *bot, where)];
        if (bot->GetExactDist2d(portal.Pos) > PortalReach || !UsePortal(bot, portal, from, now))
        {
            if (auto use = PortalUses().find(bot->GetGUID()); use != PortalUses().end() && int32(use->second.WalkUntilMs - now) > 0)
                return Step::Direct;
            waypoint = portal.Pos;
            return Step::Waypoint;
        }
        return Step::Portal;
    }

    if (to >= 0)
    {
        // entering keep `to`: through a broken gate, else (our own keep) by the teleporter outside the gate nearest to us
        Keep const& keep = Keeps[to];
        Gate const* best = nullptr;
        float bestCost = 0.0f;
        for (Gate const& gate : keep.Gates)
        {
            if (!GateOpen(map, gate))
                continue;
            float cost = bot->GetExactDist2d(gate.Outside) + gate.Inside.GetExactDist2d(where);
            if (!best || cost < bestCost)
            {
                best = &gate;
                bestCost = cost;
            }
        }
        if (best)
        {
            waypoint = bot->GetExactDist2d(best->Outside) > GateStep ? Around(*bot, best->Outside) : best->Inside;
            return Step::Waypoint;
        }
        if (TeamOf(bot) != keep.Owner)
            return Step::Blocked;
        std::array<Position, 3> exits = { keep.Leave[0].Pos, keep.Leave[1].Pos, keep.Leave[2].Pos };
        Portal const& portal = keep.Enter[BestPortal(keep.Enter, exits, *bot, where)];
        if (bot->GetExactDist2d(portal.Pos) > PortalReach || !UsePortal(bot, portal, -1, now))
        {
            waypoint = Around(*bot, portal.Pos);
            return Step::Waypoint;
        }
        return Step::Portal;
    }

    Position corner;
    if (Round(*bot, where, corner))
    {
        waypoint = corner;
        return Step::Waypoint;
    }
    return Step::Direct;
}

DriveStep Drive(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job, Position& walkTo)
{
    if (bot->GetMapId() != MapId)
        return DriveStep::None;
    if (bot->GetVehicleBase())
        return DriveStep::Busy;     // the "bg vehicle" action drives it

    ObjectGuid const guid = bot->GetGUID();
    TeamId us = TeamOf(bot);
    // drivers that left the map (the match ended, they left the battleground) are dropped now and then
    if (Drivers().size() > 64)
        std::erase_if(Drivers(), [](auto const& entry)
        {
            Player* other = ObjectAccessor::FindPlayer(entry.first);
            return !other || other->GetMapId() != MapId;
        });
    int enemy = KeepOfTeam(us == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE);
    int8 workshop = RowIndex(plan, "workshop");
    // an attacker near the Workshop, or one of its own guards, as players at the Workshop jump into the siege engines
    Position const shop = workshop != NoObjective ? plan.Objectives[workshop].Pos : Position();
    bool role = job.Base == Job::Attack || (job.Base == Job::Defend && job.Objective == workshop);
    bool wanted = sBgConfig.IsleDrivers && enemy >= 0 && workshop != NoObjective && ai.GetRole() != BotRole::Healer && role
        && bot->GetExactDist2d(shop) <= DriverReach && plan.Objectives[workshop].Owner == us && !GateOpen(bot->GetMap(), Keeps[enemy].Gates[0]);
    auto it = Drivers().find(guid);
    uint32 const now = GameTime::GetGameTimeMS();
    if (it != Drivers().end() && int32(it->second.BenchedUntilMs - now) > 0)
        return DriveStep::None;
    if (!wanted || (it == Drivers().end() && TeamDrivers(bot, us) >= sBgConfig.IsleDrivers))
    {
        if (it != Drivers().end())
            Drivers().erase(it);
        return DriveStep::None;
    }

    Creature* vehicle = nullptr;
    if (it != Drivers().end() && !it->second.Vehicle.IsEmpty())
    {
        vehicle = ObjectAccessor::GetCreature(*bot, it->second.Vehicle);
        if (vehicle && (!vehicle->IsAlive() || !FreeDriverSeat(vehicle)))
            vehicle = nullptr;
    }
    if (!vehicle)
        vehicle = FindDemolisher(bot, shop);
    if (!vehicle)
    {
        Drivers().erase(guid);
        return DriveStep::None;
    }
    Driver& driver = Drivers()[guid];
    driver.Vehicle = vehicle->GetGUID();

    if (bot->GetExactDist2d(vehicle) > BoardReach)
    {
        walkTo = vehicle->GetPosition();
        return DriveStep::Walk;
    }
    int8 seat = -1;
    if (!FreeDriverSeat(vehicle, &seat))
        return DriveStep::None;
    // a board a moment ago that did not seat the bot was refused (a shapeshifted bot cannot ride, 2026-11-13 IoC match 3: a Guardian druid
    // tried ~60 times): three of those and the bot leaves driving to others for a while
    if (driver.BoardMs && now - driver.BoardMs < 3000)
        return DriveStep::Busy;
    if (driver.BoardMs && ++driver.BoardTries >= 3)
    {
        TC_LOG_INFO("server.playerbots", "Playerbots: {} could not board a Demolisher three times: leaves driving to others for 2 minutes", bot->GetName());
        driver = Driver();
        driver.BenchedUntilMs = now + 120000;
        return DriveStep::None;
    }
    if (bot->IsMounted())
        bot->CancelMountAura();
    // shapeshift forms (bear, cat, travel, moonkin, ghost wolf) refuse the ride spell: players drop the form to board
    bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
    bot->StopMoving();
    ai.ClearForcedTarget();
    bot->EnterVehicle(vehicle, seat);
    driver.BoardMs = now;
    if (!driver.BoardTries)
        TC_LOG_INFO("server.playerbots", "Playerbots: {} boards a Demolisher at {:.0f} {:.0f} to ram the {} front gate", bot->GetName(),
            vehicle->GetPositionX(), vehicle->GetPositionY(), us == TEAM_ALLIANCE ? "Horde" : "Alliance");
    return DriveStep::Busy;
}

void Register(AiRegistry& registry)
{
    registry.AddTrigger("bg vehicle", 0, VehicleTrigger);
    registry.AddAction<VehicleAction>("bg vehicle");
    registry.AddTrigger("bg wall", 0, WallTrigger);
    registry.AddAction<WallAction>("bg wall");
}
}
