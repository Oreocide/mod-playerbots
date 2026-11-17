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


#include "Battleground/BgCarts.h"
#include "Battleground/BgPlanner.h"
#include "Battleground.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapReference.h"
#include "MotionMaster.h"
#include "Player.h"
#include "StringFormat.h"
#include <array>
#include <string>
#include <unordered_map>

// Data: battleground_silvershard_mines.cpp, the world DB and DB2 of map 727 (worktrees/bg-silvershard/SILVERSHARD_DATA.md) and
// navcheck on its mmaps (worktrees/bg-silvershard/nav, 2026-11-14)
namespace Playerbots::Bg::Carts
{
namespace
{
constexpr float RoomFloorZ = 325.0f;        // the room platforms stand at z ~329, the field below at ~320.5
constexpr float RoomRadius = 45.0f;         // a start room spans about 30 yd around its start point
constexpr float EdgeReach = 2.0f;
constexpr uint32 DropTimeoutMs = 4000;
constexpr float CartZone = 25.0f;           // the control zone radius (3-D in the core; 2-D is close enough on the tracks)
constexpr uint32 ReportMs = 30000;

struct Room
{
    Position Start;
    std::array<Position, 2> Edges;          // the last room mesh point at each exit
    std::array<Position, 2> Lands;          // the field mesh point below and beyond it
    float FloorZ = 0.0f;                    // this room's own platform height and reach (0: the set's)
    float Radius = 0.0f;
};

struct RoomSet
{
    uint32 MapId;
    char const* Name;
    float FloorZ;                           // a spot above this and near a room's start is on the room's platform
    float Radius;
    Room const* Rooms;
    size_t Count;
};

std::array<Room, 2> const Rooms =
{ {
    // Alliance (world_safe_locs 4062): exits A1 (door 212939) and A2 (212940)
    { { 851.575f, 135.141f, 328.838f }, { { { 856.8f, 169.3f, 329.0f }, { 822.7f, 144.5f, 329.0f } } },
        { { { 855.0f, 174.2f, 320.6f }, { 816.5f, 146.0f, 320.6f } } } },
    // Horde (4061): exits H3 (212941) and H4 (212942)
    { { 630.17f, 230.174f, 328.838f }, { { { 634.4f, 196.3f, 329.0f }, { 660.3f, 229.9f, 329.0f } } },
        { { { 635.0f, 190.8f, 320.7f }, { 665.5f, 228.0f, 320.5f } } } },
} };

// Deephaul Ravine (2656): each team's start and spawn graveyard sit on a platform (z ~258-281) whose mesh ends at its edge ~12 yd above
// the field (navcheck, worktrees/bg-deephaul/DEEPHAUL_DATA.md, user OK 2026-11-16). Start = the platform's middle (start location and
// spawn graveyard); the Alliance platform has one edge, listed twice
std::array<Room, 4> const RavineRooms =
{ {
    // Alliance: start 3963.8 -3391.0 280.7, spawn graveyard 3963.7 -3267.2 258.5
    { { 3963.7f, -3329.1f, 270.0f }, { { { 3990.7f, -3241.9f, 258.2f }, { 3990.7f, -3241.9f, 258.2f } } },
        { { { 3993.1f, -3232.2f, 245.9f }, { 3993.1f, -3232.2f, 245.9f } } } },
    // Horde: start 4162.5 -2536.9 277.3, spawn graveyard 4162.6 -2626.3 257.4
    { { 4162.5f, -2581.6f, 268.0f }, { { { 4178.1f, -2642.1f, 258.2f }, { 4145.9f, -2641.9f, 259.3f } } },
        { { { 4176.0f, -2647.7f, 245.9f }, { 4146.1f, -2648.0f, 246.2f } } } },
    // the center graveyards stand on ledges ~30 yd above the basin; each ledge's mesh leads off it on one side only (user 2026-11-16:
    // "fix the Horde center graveyard stop"; navcheck from rings around each graveyard). Horde: 4027.3 -2785.9 239.7, east edge
    { { 4027.3f, -2785.9f, 239.7f }, { { { 4043.5f, -2789.3f, 240.1f }, { 4043.5f, -2789.3f, 240.1f } } },
        { { { 4057.7f, -2786.5f, 207.5f }, { 4057.7f, -2786.5f, 207.5f } } }, 234.0f, 20.0f },
    // Alliance: 4103.6 -3113.1 239.7, west edge
    { { 4103.6f, -3113.1f, 239.7f }, { { { 4094.1f, -3114.7f, 240.1f }, { 4094.1f, -3114.7f, 240.1f } } },
        { { { 4078.1f, -3114.7f, 210.2f }, { 4078.1f, -3114.7f, 210.2f } } }, 234.0f, 20.0f },
} };

std::array<RoomSet, 2> const RoomSets =
{ {
    { MapId, "Silvershard Mines", RoomFloorZ, RoomRadius, Rooms.data(), Rooms.size() },
    { Ravine::MapId, "Deephaul Ravine", 252.0f, 110.0f, RavineRooms.data(), RavineRooms.size() },
} };

RoomSet const* RoomsOf(uint32 mapId)
{
    for (RoomSet const& set : RoomSets)
        if (set.MapId == mapId)
            return &set;
    return nullptr;
}

// the lines' spawn points, where the next cart appears 5 s after a delivery (S:138-142), by control zone object
struct Line
{
    uint32 Zone;
    Position Spawn;
};

std::array<Line, 3> const Lines =
{ {
    { 213766, { 744.517f, 183.198f, 319.544f } },   // East
    { 213794, { 759.325f, 198.332f, 319.531f } },   // North
    { 213796, { 739.295f, 203.764f, 319.544f } },   // South
} };

struct Drop
{
    Position To;
    uint32 UntilMs = 0;
    size_t Exit = 0;
};

std::unordered_map<ObjectGuid, Drop>& Drops()
{
    static std::unordered_map<ObjectGuid, Drop> drops;  // world thread only (battleground bots act there)
    return drops;
}

/// The start room the spot is in (on the platform layer) on that map, or -1
int RoomOf(uint32 mapId, Position const& pos)
{
    RoomSet const* set = RoomsOf(mapId);
    if (!set)
        return -1;
    for (size_t i = 0; i < set->Count; ++i)
    {
        Room const& room = set->Rooms[i];
        float floor = room.FloorZ ? room.FloorZ : set->FloorZ;
        float radius = room.Radius ? room.Radius : set->Radius;
        if (pos.GetPositionZ() >= floor && pos.GetExactDist2d(room.Start) <= radius)
            return int(i);
    }
    return -1;
}
}

Step Next(Player* bot, Position const& where, Position& waypoint)
{
    uint32 now = GameTime::GetGameTimeMS();
    if (auto drop = Drops().find(bot->GetGUID()); drop != Drops().end())
    {
        if (int32(drop->second.UntilMs - now) > 0 && bot->GetExactDist2d(drop->second.To) > 1.5f)
            return Step::Busy;
        // the first matches check the drops: the bot must stand on the field near the landing point
        float off = bot->GetExactDist2d(drop->second.To);
        RoomSet const* dropped = RoomsOf(bot->GetMapId());
        if (RoomOf(bot->GetMapId(), bot->GetPosition()) >= 0 || off > 3.0f)
            TC_LOG_WARN("server.playerbots", "Playerbots: {} after the {} drop by exit {} stands at {:.1f} {:.1f} {:.1f}, {:.1f} yd from the landing point",
                bot->GetName(), dropped ? dropped->Name : "start room", drop->second.Exit, bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), off);
        Drops().erase(drop);
    }

    RoomSet const* set = RoomsOf(bot->GetMapId());
    int room = RoomOf(bot->GetMapId(), bot->GetPosition());
    if (!set || room < 0 || RoomOf(bot->GetMapId(), where) == room)
        return Step::Direct;
    // the gates are open only once the match runs (the doors go 3 s after the start)
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return Step::Busy;

    // the exit that makes edge + the rest of the way shortest
    Room const& start = set->Rooms[room];
    size_t best = 0;
    float bestCost = 0.0f;
    for (size_t i = 0; i < start.Edges.size(); ++i)
    {
        float cost = bot->GetExactDist2d(start.Edges[i]) + start.Lands[i].GetExactDist2d(where);
        if (i == 0 || cost < bestCost)
        {
            best = i;
            bestCost = cost;
        }
    }
    if (bot->GetExactDist2d(start.Edges[best]) > EdgeReach)
    {
        waypoint = start.Edges[best];
        return Step::Waypoint;
    }
    // off the ledge: a fixed drop onto the field (the navmesh has no link down)
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    bot->GetMotionMaster()->MoveJump(0, start.Lands[best], Milliseconds(900));
    Drops()[bot->GetGUID()] = { start.Lands[best], now + DropTimeoutMs, best };
    TC_LOG_INFO("server.playerbots", "Playerbots: {} drops out of the {} start room {} by exit {}", bot->GetName(), set->Name, room, best);
    return Step::Busy;
}

namespace
{
// Deephaul Ravine's carts (battleground_deephaul_ravine.cpp): each spawns at its team's end of the field (east: the Alliance's, west:
// the Horde's) and rolls toward the other team's
std::array<Line, 2> const RavineLines =
{ {
    { 415326, { 3875.0f, -3150.0f, 240.2913f } },
    { 415330, { 4250.3647f, -2751.066f, 239.46973f } },
} };
}

bool Dropping(Player const* bot)
{
    auto it = Drops().find(bot->GetGUID());
    return it != Drops().end() && int32(it->second.UntilMs - GameTime::GetGameTimeMS()) > 0;
}

bool HasCarts(uint32 mapId)
{
    return mapId == MapId || mapId == Ravine::MapId;
}

namespace
{
void ReadStateFor(BgPlan& plan, Map* map, Line const* lines, size_t lineCount, uint32 scoreAlliance, uint32 scoreHorde);
}

void ReadState(BgPlan& plan, Map* map)
{
    if (plan.MapId == Ravine::MapId)
        ReadStateFor(plan, map, RavineLines.data(), RavineLines.size(), 24958, 24959);
    else
        ReadStateFor(plan, map, Lines.data(), Lines.size(), 6437, 6438);
}

namespace
{
void ReadStateFor(BgPlan& plan, Map* map, Line const* lines, size_t lineCount, uint32 scoreAlliance, uint32 scoreHorde)
{
    // diagnostics for the test matches: who owns each cart, and each team's players in its zone (only those count; a player that is not
    // PvP-flagged or is stealthed does not)
    static std::unordered_map<uint32, uint32> lastReport;
    uint32 now = GameTime::GetGameTimeMS();
    bool report = false;
    if (uint32& last = lastReport[plan.InstanceId]; !last || now - last >= ReportMs)
    {
        last = now;
        report = true;
    }
    std::string text;
    for (Objective& objective : plan.Objectives)
    {
        Line const* line = nullptr;
        for (size_t i = 0; i < lineCount; ++i)
            if (lines[i].Zone == objective.Entry)
                line = &lines[i];
        if (!line)
            continue;
        GameObject* zone = objective.Guid.IsEmpty() ? nullptr : map->GetGameObject(objective.Guid);
        objective.Known = true;
        objective.Assaulting = TEAM_NEUTRAL;
        objective.Destroyed = false;
        if (zone && zone->IsInWorld())
        {
            objective.Owner = zone->GetControllingTeam();
            objective.Pos = zone->GetPosition();
        }
        else
        {
            objective.Owner = TEAM_NEUTRAL;
            objective.Pos = line->Spawn;
        }
        if (report)
        {
            uint32 near[2] = { 0, 0 }, counted[2] = { 0, 0 };
            for (MapReference const& ref : map->GetPlayers())
                if (Player* player = ref.GetSource(); player && player->IsAlive() && player->GetExactDist2d(objective.Pos) <= CartZone)
                {
                    TeamId team = TeamOf(player);
                    if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
                        continue;
                    ++near[team];
                    counted[team] += player->IsOutdoorPvPActive() ? 1 : 0;
                }
            text += Trinity::StringFormat("; {} {} at {:.0f} {:.0f}{}: A {} ({} count) H {} ({} count)", objective.Name,
                objective.Owner == TEAM_ALLIANCE ? "Alliance" : objective.Owner == TEAM_HORDE ? "Horde" : "neutral", objective.Pos.GetPositionX(),
                objective.Pos.GetPositionY(), zone ? "" : " (between carts)", near[TEAM_ALLIANCE], counted[TEAM_ALLIANCE], near[TEAM_HORDE], counted[TEAM_HORDE]);
        }
    }
    if (report)
        TC_LOG_INFO("server.playerbots", "Playerbots: {} instance {}: score A {} H {}{}", plan.Def ? plan.Def->Name : "carts", plan.InstanceId,
            map->GetWorldStateValue(scoreAlliance), map->GetWorldStateValue(scoreHorde), text);
}
}

bool AwayFromCart(Player const* bot, BgPlan const& plan, Assignment const& job, Position& cart)
{
    if (!bot->IsAlive() || !bot->IsInCombat() || RoomOf(bot->GetMapId(), bot->GetPosition()) >= 0)
        return false;
    int8 target = job.Objective;
    if (target < 0 || size_t(target) >= plan.Objectives.size())
    {
        TeamId us = TeamOf(bot);
        float best = 0.0f;
        for (size_t i = 0; i < plan.Objectives.size(); ++i)
            if (plan.Objectives[i].Known && plan.Objectives[i].Owner != us && (target < 0 || bot->GetExactDist2d(plan.Objectives[i].Pos) < best))
            {
                target = int8(i);
                best = bot->GetExactDist2d(plan.Objectives[i].Pos);
            }
        if (target < 0)
            return false;
    }
    cart = plan.Objectives[target].Pos;
    if (bot->GetExactDist2d(cart) <= CartZone)
        return false;
    Unit const* victim = bot->GetVictim();
    return !victim || victim->GetExactDist2d(cart) > CartZone;
}
}

namespace Playerbots::Bg::Ravine
{
Position const& CapturePoint(uint8 team)
{
    // areatrigger spawns 115 (create properties 30, Alliance) and 116 (31, Horde) on map 2656
    static Position const points[2] = { { 3919.47f, -3120.23f, 240.846f }, { 4207.9f, -2777.05f, 239.817f } };
    return points[team ? 1 : 0];
}
}
