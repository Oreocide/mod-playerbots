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

#include "Ambient.h"
#include "BotSession.h"
#include "ChatPackets.h"
#include "GameObject.h"
#include "GameObjectPackets.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Db/PlayerbotRepository.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "RpgInfo.h"
#include "SharedDefines.h"
#include "Travel/MoveToward.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

namespace Playerbots::Ambient
{
namespace
{
constexpr float HopSpeed = 9.0f;           // yards per second along the ground
constexpr float HopMinHeight = 0.5f;
constexpr float HopMaxHeight = 1.2f;
constexpr float HopMaxStepZ = 0.8f;        // the landing point must be level with the bot
}

bool TryHop(Player* bot, float minDist, float maxDist)
{
    Map* map = bot->GetMap();
    if (!map || !bot->IsAlive() || bot->IsMounted() || bot->IsInWater() || bot->IsUnderWater() || bot->IsInCombat() || bot->IsFalling()
        || bot->IsInFlight() || bot->IsBeingTeleported() || !bot->IsOutdoors()
        || bot->HasUnitState(UNIT_STATE_CASTING | UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED))
        return false;

    float dist = frand(std::min(minDist, maxDist), std::max(minDist, maxDist));
    Position target = bot->GetFirstCollisionPosition(dist, 0.0f);   // straight ahead, shortened where something is in the way
    if (bot->GetExactDist2d(target) < std::min(minDist, 1.0f) || std::fabs(target.GetPositionZ() - bot->GetPositionZ()) > HopMaxStepZ)
        return false;

    if (!bot->IsWithinLOS(target.GetPositionX(), target.GetPositionY(), target.GetPositionZ()))
        return false;

    // the landing point must be connected to where the bot stands by the navmesh, with no detour
    PathGenerator path(bot);
    bool ok = path.CalculatePath(target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(), false);
    uint32 type = path.GetPathType();
    if (!ok || (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_INCOMPLETE)) || !(type & PATHFIND_NORMAL)
        || path.GetPathLength() > bot->GetExactDist2d(target) * 1.6f + 1.0f)
        return false;

    bot->GetMotionMaster()->MoveJump(EVENT_JUMP, target, HopSpeed, HopMinHeight, HopMaxHeight);
    return true;
}

void WalkingHop(PlayerbotAI& ai, uint32 now)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();
    uint32 elapsed = std::min<uint32>(now - rpg.LastHopRoll, 1000);
    rpg.LastHopRoll = now;

    float chancePerSecond = sPlayerbotsConfig.RpgHopChance;
    if (chancePerSecond <= 0.0f || now < rpg.NextHopAt || !IsMovingOnFoot(bot))
        return;

    // not in the last few yards of a walk (the arrival check wants the bot at the target, not in the air)
    if (rpg.HasDest && bot->GetExactDist2d(rpg.Dest) < 8.0f)
        return;

    if (!roll_chance(chancePerSecond * float(elapsed) / 10.0f))   // roll_chance_f takes a percent
        return;

    if (TryHop(bot, 2.0f, 3.5f))
        rpg.NextHopAt = now + sPlayerbotsConfig.RpgHopMinIntervalMs;
}

void IdleHop(PlayerbotAI& ai, uint32 now)
{
    RpgInfo& rpg = ai.GetRpg();
    if (sPlayerbotsConfig.RpgIdleHopChance <= 0.0f || now < rpg.NextHopAt || !roll_chance(sPlayerbotsConfig.RpgIdleHopChance * 100.0f))
        return;

    if (TryHop(ai.GetBot(), 0.8f, 1.6f))
        rpg.NextHopAt = now + sPlayerbotsConfig.RpgHopMinIntervalMs;
}

bool GreetNearbyPlayer(PlayerbotAI& ai, uint32 now)
{
    RpgInfo& rpg = ai.GetRpg();
    Player* bot = ai.GetBot();
    if (!sPlayerbotsConfig.RpgGreetEmotes || now < rpg.NextGreetAt)
        return false;

    auto humans = sPlayerbotMgr->GetNearbyHumans();
    if (!humans)
        return false;

    float range = sPlayerbotsConfig.RpgGreetDistance;
    PlayerbotMgr::NearbyHuman const* nearest = nullptr;
    float nearestSq = range * range;
    for (PlayerbotMgr::NearbyHuman const& human : *humans)
    {
        if (human.MapId != bot->GetMapId())
            continue;
        float dx = human.X - bot->GetPositionX();
        float dy = human.Y - bot->GetPositionY();
        float distSq = dx * dx + dy * dy;
        if (distSq <= nearestSq)
        {
            nearestSq = distSq;
            nearest = &human;
        }
    }
    if (!nearest)
        return false;

    // someone is close: the bot may greet them, and whether it does or not it waits a while before asking again
    rpg.NextGreetAt = now + sPlayerbotsConfig.RpgGreetCooldownMs + urand(0, 60000);
    if (!roll_chance(sPlayerbotsConfig.RpgGreetChance * 100.0f))
        return false;

    Player* human = ObjectAccessor::GetPlayer(*bot, nearest->Guid);
    if (!human || !human->IsInWorld() || !human->IsAlive() || !bot->IsWithinLOSInMap(human))
        return false;

    static constexpr int32 Greetings[] = { TEXT_EMOTE_WAVE, TEXT_EMOTE_HELLO, TEXT_EMOTE_GREET, TEXT_EMOTE_BOW };
    bot->SetFacingToObject(human);
    WorldPackets::Chat::CTextEmote emote{ WorldPacket(CMSG_SEND_TEXT_EMOTE) };
    emote.Target = human->GetGUID();
    emote.EmoteID = Greetings[urand(0, std::size(Greetings) - 1)];
    emote.SoundIndex = -1;
    bot->GetSession()->HandleTextEmoteOpcode(emote);
    return true;
}

void ReturnTextEmote(Player* sender, uint32 emoteId, uint32 now)
{
    if (!sPlayerbotsConfig.ReactReturnEmotes || !sender || IsBot(sender) || !sender->IsAlive())
        return;

    static constexpr uint32 Friendly[] = { TEXT_EMOTE_WAVE, TEXT_EMOTE_HELLO, TEXT_EMOTE_GREET, TEXT_EMOTE_BOW, TEXT_EMOTE_CHEER,
        TEXT_EMOTE_LAUGH, TEXT_EMOTE_SALUTE, TEXT_EMOTE_THANK, TEXT_EMOTE_WELCOME };
    if (std::find(std::begin(Friendly), std::end(Friendly), emoteId) == std::end(Friendly))
        return;

    float range = sPlayerbotsConfig.RpgGreetDistance;
    std::vector<Player*> nearby;
    sender->GetPlayerListInGrid(nearby, range, true);

    Player* nearest = nullptr;
    float nearestDist = range;
    for (Player* bot : nearby)
    {
        if (!IsBot(bot) || bot->IsInCombat() || bot->GetMap() != sender->GetMap())
            continue;
        PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
        if (!ai || ai->GetRpg().RepliedTo(sender->GetGUID(), now))
            continue;
        float dist = bot->GetExactDist(sender);
        if (dist < nearestDist && bot->IsWithinLOSInMap(sender))
        {
            nearestDist = dist;
            nearest = bot;
        }
    }
    if (!nearest)
        return;

    PlayerbotAI* ai = sPlayerbotMgr->GetAI(nearest);
    ai->GetRpg().NoteReply(sender->GetGUID(), now, sPlayerbotsConfig.RpgGreetCooldownMs);
    sPlayerbotRepository->Remember(nearest->GetGUID(), "person", sender->GetName(), "emote");
    nearest->SetFacingToObject(sender);
    WorldPackets::Chat::CTextEmote emote{ WorldPacket(CMSG_SEND_TEXT_EMOTE) };
    emote.Target = sender->GetGUID();
    emote.EmoteID = int32(emoteId);
    emote.SoundIndex = -1;
    nearest->GetSession()->HandleTextEmoteOpcode(emote);
}

bool OnChair(Player const* bot)
{
    switch (bot->GetStandState())
    {
        case UNIT_STAND_STATE_SIT_CHAIR:
        case UNIT_STAND_STATE_SIT_LOW_CHAIR:
        case UNIT_STAND_STATE_SIT_MEDIUM_CHAIR:
        case UNIT_STAND_STATE_SIT_HIGH_CHAIR:
            return true;
        default:
            return false;
    }
}

bool TryChair(PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.ReactChairs)
        return false;

    Player* bot = ai.GetBot();
    if (OnChair(bot))
        return true;

    FindGameObjectOptions options;
    options.GameObjectType = GAMEOBJECT_TYPE_CHAIR;
    std::vector<GameObject*> chairs;
    bot->GetGameObjectListWithOptionsInGrid(chairs, 15.0f, options);

    GameObject* best = nullptr;
    float bestDist = 15.0f;
    for (GameObject* chair : chairs)
    {
        if (!chair)
            continue;
        float dist = bot->GetExactDist2d(chair);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = chair;
        }
    }
    if (!best)
        return false;

    if (bot->GetGameObjectIfCanInteractWith(best->GetGUID(), GAMEOBJECT_TYPE_CHAIR))
    {
        WorldPackets::GameObject::GameObjUse use{ WorldPacket(CMSG_GAME_OBJ_USE) };
        use.Guid = best->GetGUID();
        bot->GetSession()->HandleGameObjectUseOpcode(use);
        return OnChair(bot);
    }

    if (!IsMovingOnFoot(bot))
        MoveAlongMesh(bot, best->GetPosition(), 2.0f);
    return true;
}
}
