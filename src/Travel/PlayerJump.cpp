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

#include "PlayerJump.h"
#include "GameTime.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementPackets.h"
#include "ObjectGuid.h"
#include "Player.h"
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace Playerbots
{
namespace
{
constexpr float JumpSpeed = 7.955547f;        // the client's jump: upward speed in yards per second (sent negative)
constexpr float Gravity = 19.29110527f;
constexpr uint32 AirMs = uint32(2.0f * JumpSpeed / Gravity * 1000.0f);   // 824 ms
constexpr uint32 GraceMs = 400;
constexpr float MinDistance = 0.8f;
constexpr float MaxDistance = 9.0f;            // a mounted run covers 14 yards a second, the arc lasts 0.82 seconds

std::mutex sLock;
std::unordered_map<ObjectGuid, uint32> sUntil;   // game time ms: end of the arc

void Broadcast(Player* bot)
{
    WorldPackets::Movement::MoveUpdate update;
    update.Status = &bot->m_movementInfo;
    bot->SendMessageToSet(update.Write(), true);
}
}

bool IsPlayerJumping(Player const* bot)
{
    std::lock_guard lock(sLock);
    auto itr = sUntil.find(bot->GetGUID());
    return itr != sUntil.end() && int32(GameTime::GetGameTimeMS() - itr->second) < int32(GraceMs);
}

bool StartPlayerJump(Player* bot, Position const& to)
{
    if (!bot->IsInWorld() || !bot->IsAlive() || bot->IsInWater() || bot->IsUnderWater() || bot->IsFalling() || bot->IsInFlight()
        || bot->GetTransport() || bot->GetVehicle() || bot->IsBeingTeleported()
        || bot->HasUnitState(UNIT_STATE_CASTING | UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED))
        return false;

    float dx = to.GetPositionX() - bot->GetPositionX();
    float dy = to.GetPositionY() - bot->GetPositionY();
    float distance = std::hypot(dx, dy);
    if (distance < MinDistance || distance > MaxDistance)
        return false;

    uint32 now = GameTime::GetGameTimeMS();
    {
        std::lock_guard lock(sLock);
        if (sUntil.size() > 4096)
            sUntil.clear();
        uint32& until = sUntil[bot->GetGUID()];
        if (int32(now - until) < int32(GraceMs))
            return false;
        until = now + AirMs;
    }

    float angle = std::atan2(dy, dx);

    // the spline of the walk ends here; from now on the clients fly the bot along the jump
    bot->StopMoving();
    bot->SetOrientation(angle);

    MovementInfo& info = bot->m_movementInfo;
    // the guid is only ever filled from a client's packet: without it the clients drop the update (a bot has no client)
    info.guid = bot->GetGUID();
    info.time = now;
    info.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), angle);
    info.AddMovementFlag(MovementFlags(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_FALLING));
    info.jump.fallTime = 0;
    info.jump.zspeed = -JumpSpeed;
    info.jump.sinAngle = std::sin(angle);
    info.jump.cosAngle = std::cos(angle);
    info.jump.xyspeed = distance / (float(AirMs) / 1000.0f);
    Broadcast(bot);

    // the landing: the bot stands at the far side, the clients are told
    Position landing(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(), angle);
    bot->m_Events.AddEventAtOffset([bot, landing]()
    {
        if (!bot->IsInWorld())
            return;

        MovementInfo& info = bot->m_movementInfo;
        info.RemoveMovementFlag(MovementFlags(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_FALLING));
        info.ResetJump();
        bot->UpdatePosition(landing.GetPositionX(), landing.GetPositionY(), landing.GetPositionZ(), landing.GetOrientation());
        bot->SetFallInformation(0, landing.GetPositionZ());
        info.time = GameTime::GetGameTimeMS();
        Broadcast(bot);
    }, Milliseconds(AirMs));

    return true;
}
}
