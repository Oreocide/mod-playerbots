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

#include "AbstractLife.h"
#include "Db/PlayerbotRepository.h"
#include "Goal/GoalStore.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "QuestTravel.h"
#include "RpgStrategy.h"
#include <algorithm>
#include <string>

namespace Playerbots
{
namespace AbstractLife
{
namespace
{
constexpr uint32 RecheckMs = 60000;    // a stored game-event goal may start while a status runs

// what a finished status paid: a quest's XP, or a share of it for a grind; nothing for the rest
uint32 XpFor(Player* bot, RpgStatus status)
{
    if (!sPlayerbotsConfig.QuestAbstractXp)
        return 0;
    if (status == RpgStatus::DoQuest)
        return QuestTravel::AbstractXp(bot);
    if (status == RpgStatus::GoGrind)
        return QuestTravel::AbstractXp(bot) * std::min<uint32>(sPlayerbotsConfig.AbstractGrindXpPercent, 100) / 100;
    return 0;
}
}

char const* StatusName(RpgStatus status)
{
    static char const* const Names[] = { "idle", "do quest", "wander npc", "travel flight", "go grind", "go camp", "rest", "wander random", "gather", "craft" };
    return uint8(status) < RpgStatusCount ? Names[uint8(status)] : "?";
}

uint32 Tick(PlayerbotAI& ai, State& state, uint32 nowMs)
{
    Player* bot = ai.GetBot();
    if (!bot || !ai.GetMasterGuid().IsEmpty())
        return nowMs + RecheckMs;

    // a game-event row (festival visits and the like) outranks the status weights while it lasts
    uint32 goalUntil = GoalStore::OnFrozen(ai, nowMs, true);
    if (GoalStore::HasGoal(bot->GetGUID(), nowMs))
    {
        state = {};
        return std::min(goalUntil, nowMs + RecheckMs);
    }

    if (state.Status != RpgStatus::Idle && int32(state.UntilMs - nowMs) > 0)
        return std::min(state.UntilMs, nowMs + RecheckMs);

    // the status that just ended pays out
    if (state.Status != RpgStatus::Idle)
        if (uint32 xp = XpFor(bot, state.Status))
        {
            bot->GiveXP(xp, nullptr);
            sPlayerbotRepository->Remember(bot->GetGUID(), "quest", "abstract", std::to_string(xp));
        }

    RpgStatus next = PickAbstractRpgStatus(ai);
    state.Status = next;
    state.UntilMs = nowMs + std::max<uint32>(RollAbstractRpgDuration(next), 1000);
    TC_LOG_DEBUG("server.playerbots", "Playerbots: frozen {} takes status {}", bot->GetName(), StatusName(next));
    return std::min(state.UntilMs, nowMs + RecheckMs);
}
}
}
