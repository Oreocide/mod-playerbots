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

#include "Travel/BotMover.h"
#include "GameTime.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Player.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr float ArriveMaxZ = 2.5f;          // a bot this far above/below the point is not "there" (a bridge over a road)
constexpr float CombatMinProgress = 1.0f;   // an incomplete path must get the bot at least this much closer
constexpr uint32 HoldMinMs = 500;
constexpr uint32 HoldMaxMs = 5000;

std::atomic<uint32> sCombatLaunched{ 0 };
std::atomic<uint32> sCombatNoPath{ 0 };

BotMoveState* StateOf(Player const* bot)
{
    PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
    return ai ? &ai->GetMoveState() : nullptr;
}
}

bool CanMove(Player const* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsBeingTeleported())
        return false;
    if (!bot->CanFreeMove() || bot->IsCharmed() || bot->IsPolymorphed())
        return false;
    return true;
}

bool SnapToMesh(Player* bot, Position const& wanted, Position& out, float maxXY, float maxZ)
{
    if (!bot)
        return false;

    PathGenerator path(bot);
    bool calculated = path.CalculatePath(wanted.GetPositionX(), wanted.GetPositionY(), wanted.GetPositionZ(), false);
    if (!calculated || !PathIsWalkable(path.GetPathType()))
        return false;

    // forceDest = false: the mesh decides where the path ends (the nearest reachable poly point to the wanted position)
    G3D::Vector3 const& end = path.GetActualEndPosition();
    if (std::hypot(end.x - wanted.GetPositionX(), end.y - wanted.GetPositionY()) > maxXY)
        return false;
    if (std::fabs(end.z - wanted.GetPositionZ()) > maxZ)
        return false;

    out.Relocate(end.x, end.y, end.z);
    return true;
}

MoveResult MoveCombat(Player* bot, Position const& dest, float arriveYards, MovePriority prio)
{
    if (!bot)
        return MoveResult::NoPath;

    float dist = bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
    float dz = std::fabs(bot->GetPositionZ() - dest.GetPositionZ());
    BotMoveState* state = StateOf(bot);

    if (dist <= arriveYards && dz <= ArriveMaxZ)
    {
        if (state)
            NoteStopped(*state);
        return MoveResult::Arrived;
    }

    // not a path problem: rooted, stunned, being teleported... the caller tries again next tick
    if (!CanMove(bot))
        return MoveResult::Moving;

    // a cast would be cut by the first step; only an avoid move (leaving a hazard) is worth that
    if (prio < MovePriority::Avoid && bot->HasUnitState(UNIT_STATE_CASTING))
        return MoveResult::Moving;

    uint32 now = GameTime::GetGameTimeMS();
    bool moving = IsMovingOnFoot(bot);
    if (state && DecideMove(*state, prio, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), moving, now) == MoveDecision::KeepRunning)
        return MoveResult::Moving;

    PathGenerator path(bot);
    bool calculated = path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false);
    Movement::PointsArray const& points = path.GetPath();
    if (!calculated || !PathIsWalkable(path.GetPathType()) || points.size() < 2 || path.GetPathLength() < 0.3f)
    {
        ++sCombatNoPath;
        TC_LOG_DEBUG("server.playerbots.stuck", "Playerbots: {} has no combat mesh path on map {} from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}), path type {}",
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), uint32(path.GetPathType()));
        return MoveResult::NoPath;
    }

    // a path cut short (INCOMPLETE) is a step toward the point, taken only if it really gets closer
    G3D::Vector3 const& end = points.back();
    float endGap = std::hypot(end.x - dest.GetPositionX(), end.y - dest.GetPositionY());
    bool reaches = endGap <= arriveYards && std::fabs(end.z - dest.GetPositionZ()) <= ArriveMaxZ;
    if (!reaches && dist - endGap < CombatMinProgress)
    {
        ++sCombatNoPath;
        return MoveResult::NoPath;
    }

    // Unlike MoveAlongMesh: no AttackStop (the bot keeps its target and swings when the step ends), and a running
    // walk or chase is cut when the arbitration above allowed it
    bot->GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(bot);
    init.MovebyPath(points);
    init.Launch();

    if (state)
    {
        float speed = std::max(bot->GetSpeed(bot->IsWalking() ? MOVE_WALK : MOVE_RUN), 1.0f);
        uint32 holdMs = std::clamp(uint32(path.GetPathLength() / speed * 1000.0f) + 300, HoldMinMs, HoldMaxMs);
        NoteLaunched(*state, prio, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), now, holdMs);
    }
    ++sCombatLaunched;
    return MoveResult::Moving;
}

bool MoveHeldAbove(Player const* bot, MovePriority prio)
{
    BotMoveState* state = StateOf(bot);
    if (!state || !state->Active || state->Priority <= prio)
        return false;
    // the spline ended or the hold ran out: nothing is held
    if (!IsMovingOnFoot(bot) || int32(state->ExpiresAtMs - GameTime::GetGameTimeMS()) <= 0)
        return false;
    return true;
}

void ClearMoveState(Player* bot)
{
    if (BotMoveState* state = StateOf(bot))
        NoteStopped(*state);
}
}
