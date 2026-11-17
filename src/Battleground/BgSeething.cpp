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

#include "Battleground/BgSeething.h"
#include "Battleground/BgPlanner.h"
#include "Battleground.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "MovementPackets.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Transport.h"
#include <array>
#include <unordered_map>

namespace Playerbots::Bg::Seething
{
namespace
{
// the twelve fissures (creature 125253), same order as MapDef 1803's code table
constexpr std::array<Position, 12> Fissures =
{ {
    { 1343.7f, 2919.9f, 32.9f }, { 1390.2f, 2570.8f, 6.5f }, { 1257.4f, 2882.7f, 28.0f }, { 1361.7f, 2643.3f, 4.5f },
    { 1441.1f, 2700.1f, 9.5f }, { 1454.1f, 2598.4f, 15.2f }, { 1126.7f, 2781.2f, 30.9f }, { 1461.4f, 2823.2f, 31.7f },
    { 1243.2f, 2721.5f, 11.9f }, { 1339.6f, 2785.7f, 2.6f }, { 1259.2f, 2571.4f, 8.6f }, { 1113.9f, 2887.0f, 38.5f },
} };
constexpr std::array<uint32, 3> NodeEntries = { 272471, 281306, 281307 };   // the azerite capture points a fissure summons
constexpr float NodeReach = 15.0f;          // a node stands on its fissure; anything farther is not one of ours
const Position FieldMiddle = { 1380.0f, 2740.0f, 10.0f };   // no node up: drop toward the middle of the ring
constexpr float GoodDrop = 80.0f;           // a drop is taken when the mesh route from the ground below is at most this long
constexpr uint32 LapMs = 90000;             // after this long aboard any complete route will do (the ring is ~1,290 yd)
constexpr float GroundSlack = 3.0f;         // after landing: farther than this from the ground the fall was aimed at is a WARN
// the airship's own collision model is the first "ground" under a bot on its deck (match 1, 2026-11-15: height above ground 0 at
// z 318): the ground is searched from below the hull. The ships fly at z ~297 (deck ~318), the field is at z 0-40
constexpr float BelowHull = 40.0f;
constexpr float SearchCeiling = 250.0f;

struct Drop
{
    uint32 AboardSinceMs = 0;   // the first look after the start (or a revive aboard)
    bool Falling = false;
    Position Ground;            // where the fall should end
};

std::unordered_map<ObjectGuid, Drop>& Drops()
{
    static std::unordered_map<ObjectGuid, Drop> drops;
    return drops;
}

bool IsNode(uint32 entry)
{
    for (uint32 node : NodeEntries)
        if (node == entry)
            return true;
    return false;
}

/// The node to aim for: the job's, else the nearest one up, else the middle of the field
Position Target(Player const* bot, BgPlan const* plan, Assignment const* job)
{
    if (plan)
    {
        if (job && job->Objective >= 0 && size_t(job->Objective) < plan->Objectives.size() && plan->Objectives[job->Objective].Known)
            return plan->Objectives[job->Objective].Pos;
        Position const* best = nullptr;
        for (Objective const& objective : plan->Objectives)
            if (objective.Known && (!best || bot->GetExactDist2d(objective.Pos) < bot->GetExactDist2d(*best)))
                best = &objective.Pos;
        if (best)
            return *best;
    }
    return FieldMiddle;
}

/// Tell the clients the bot left the deck: a bot has no client to send it (BotTransports does the same)
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
}

void ReadState(BgPlan& plan, Map* map)
{
    std::array<ObjectGuid, Fissures.size()> up = {};
    for (auto const& [guid, go] : map->GetObjectsStore().Data.FindContainer<GameObject>())
    {
        if (!go || !go->IsInWorld() || !IsNode(go->GetEntry()) || go->GetGoState() != GO_STATE_READY || go->HasFlag(GO_FLAG_NOT_SELECTABLE))
            continue;
        size_t best = Fissures.size();
        for (size_t i = 0; i < Fissures.size(); ++i)
            if (go->GetExactDist2d(Fissures[i]) <= NodeReach && (best == Fissures.size() || go->GetExactDist2d(Fissures[i]) < go->GetExactDist2d(Fissures[best])))
                best = i;
        if (best < Fissures.size())
            up[best] = guid;
    }
    for (size_t i = 0; i < plan.Objectives.size() && i < Fissures.size(); ++i)
    {
        Objective& objective = plan.Objectives[i];
        bool wasUp = objective.Known;
        objective.Guid = up[i];
        objective.Known = !up[i].IsEmpty();
        objective.Pos = Fissures[i];
        objective.Owner = TEAM_NEUTRAL;     // a node is captured once and gone: nobody ever holds one
        objective.Destroyed = false;
        if (objective.Known != wasUp)
            TC_LOG_INFO("server.playerbots", "Playerbots: Seething Shore instance {}: {} {}", plan.InstanceId, objective.Name,
                objective.Known ? "has a node up" : "node gone");
    }
}

bool Airship(Player* bot, BgPlan const* plan, Assignment const* job)
{
    if (bot->GetMapId() != MapId)
        return false;
    uint32 now = GameTime::GetGameTimeMS();
    auto it = Drops().find(bot->GetGUID());

    // falling: wait for the fall to end, then check where it ended
    if (it != Drops().end() && it->second.Falling)
    {
        if (!bot->movespline->Finalized())
            return true;
        float off = std::fabs(bot->GetPositionZ() - it->second.Ground.GetPositionZ());
        Map* map = bot->GetMap();
        bool water = map->IsInWater(bot->GetPhaseShift(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() + 0.5f);
        if (water || off > GroundSlack || bot->GetTransport())
            TC_LOG_WARN("server.playerbots", "Playerbots: {} after the Seething Shore drop stands at {:.1f} {:.1f} {:.1f} ({:.1f} yd from the ground aimed at{}{})",
                bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), off, water ? ", in water" : "",
                bot->GetTransport() ? ", still aboard" : "");
        Drops().erase(it);
        return false;
    }

    if (!bot->GetTransport() || !bot->IsAlive())
    {
        if (it != Drops().end())
            Drops().erase(it);
        return false;
    }

    // aboard before the start: the deck holds everyone (the script sends a player who steps off back to the start)
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return true;

    Drop& drop = Drops()[bot->GetGUID()];
    if (!drop.AboardSinceMs)
        drop.AboardSinceMs = now;

    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    float ground = bot->GetMapHeight(x, y, std::min(bot->GetPositionZ() - BelowHull, SearchCeiling), true, MAX_FALL_DISTANCE);
    if (ground <= INVALID_HEIGHT || bot->GetMap()->IsInWater(bot->GetPhaseShift(), x, y, ground + 0.5f))
        return true;

    Position target = Target(bot, plan, job);
    PathGenerator path(bot);
    if (!path.CalculatePath(x, y, ground, target.GetPositionX(), target.GetPositionY(), target.GetPositionZ())
        || (path.GetPathType() & ~PATHFIND_NORMAL))
        return true;
    float length = path.GetPathLength();
    bool lap = now - drop.AboardSinceMs >= LapMs;
    if (length > GoodDrop && !lap)
        return true;

    TC_LOG_INFO("server.playerbots", "Playerbots: {} drops from its Seething Shore airship at {:.1f} {:.1f}, ground {:.1f}, {:.0f} yd to {:.1f} {:.1f}{} after {} s aboard",
        bot->GetName(), x, y, ground, length, target.GetPositionX(), target.GetPositionY(), lap ? " (lap timeout)" : "",
        (now - drop.AboardSinceMs) / 1000);
    drop.Falling = true;
    drop.Ground.Relocate(x, y, ground);
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    if (TransportBase* transport = bot->GetTransport())
        transport->RemovePassenger(bot);   // after the start the script casts the rocket parachute and no fall damage
    Broadcast(bot);
    // MoveFall would measure from the bot's spot too and find the deck under it (no fall): fall straight to the ground found above
    bot->SetFallInformation(0, bot->GetPositionZ());
    Movement::MoveSplineInit init(bot);
    init.MoveTo(x, y, ground + 0.1f, false);
    init.SetFall();
    init.Launch();
    return true;
}
}
