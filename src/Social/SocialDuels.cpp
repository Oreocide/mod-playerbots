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

// Duels, ported from AzerothCore: the "start duel" strategy of a quarter of the free random bots and its RpgDuelTrigger /
// RpgDuelAction (cast Duel, 7266, on a player of a fitting level where duels are allowed), AcceptDuelAction (accept at 90%
// health or more, cancel below) and the "duel" strategy's "attack duel opponent". The fight itself is the "combat" strategy:
// at the start of the countdown's end each bot is pointed at its opponent, at the end the target is dropped.

#include "BotSession.h"
#include "DB2Stores.h"
#include "Engine/AiRegistry.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "SocialMgr.h"
#include "Social/SocialConfig.h"
#include "Social/SocialShared.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"
#include <map>
#include <mutex>

namespace Playerbots::Social
{
namespace
{
constexpr uint32 DuelSpell = 7266;
constexpr float AcceptHealthPct = 90.0f;      // AC AcceptDuelAction / RpgDuelTrigger
constexpr uint32 PairCooldownMs = 300000;     // the same two do not duel again for 5 minutes

std::mutex g_duelLock;
std::map<ObjectGuid, uint32> g_humanChallenged;                        // human -> last challenge by any bot
std::map<std::pair<ObjectGuid, ObjectGuid>, uint32> g_pairChallenged;  // (bot, target) -> last challenge

bool AllowsDuels(Player const* player)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
    return !area || area->GetFlags().HasFlag(AreaFlags::AllowDueling);
}

bool IsStarter(Player const* bot)
{
    // AC AiFactory: "let 25% of free bots start duels" (rolled at each strategy reset there; stable by guid here)
    return bot->GetGUID().GetCounter() * 2246822519u % 100 < sSocialConfig.DuelStarterPct;
}

bool DuelChallenged(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->duel || bot->duel->State != DUEL_STATE_CHALLENGED || bot->duel->Initiator == bot)
        return false;
    // a challenge from one of our bots is always answered; any other one only with AcceptAnywhere (the "world" strategy
    // accepts in capitals by itself)
    return sSocialConfig.DuelAcceptAnywhere || (sSocialConfig.DuelEnable && IsBot(bot->duel->Initiator));
}

class AnswerDuelAction final : public Action
{
public:
    char const* GetName() const override { return "social answer duel"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (!bot->duel)
            return false;
        bool accept = bot->GetHealthPct() >= AcceptHealthPct && !bot->IsInCombat();
        ObjectGuid arbiter(*bot->m_playerData->DuelArbiter);
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [accept, arbiter](Player* player)
        {
            if (!player->duel || player->duel->State != DUEL_STATE_CHALLENGED || player->duel->Initiator == player)
                return;
            if (accept)
                player->GetSession()->HandleDuelAccepted(arbiter);
            else
                player->GetSession()->HandleDuelCancelled();
        });
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} {} a duel", bot->GetName(), accept ? "accepts" : "declines");
        return true;
    }
};

bool DuelDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.DuelEnable || !IsFreeRandomBot(ai))
        return false;
    Player* bot = ai.GetBot();
    // AC RpgDuelTrigger: level 3 or more, 90% health or more, nothing hostile around
    if (!IsStarter(bot) || bot->duel || bot->GetLevel() < 3 || !bot->IsAlive() || bot->IsInCombat() || bot->GetHealthPct() < AcceptHealthPct
        || bot->GetGroup() || bot->IsMounted() || bot->InBattleground() || bot->GetMap()->Instanceable() || !ai.IsSimulated())
        return false;
    return urand(1, 100) <= sSocialConfig.DuelChallengePct && AllowsDuels(bot);
}

class ChallengeAction final : public Action
{
public:
    char const* GetName() const override { return "social challenge"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(DuelSpell, DIFFICULTY_NONE);
        if (!spell)
            return false;
        float range = std::min(sSocialConfig.DuelRange, std::max(5.0f, spell->GetMaxRange(false)));
        uint32 now = GameTime::GetGameTimeMS();

        std::vector<Player*> found;
        auto check = [bot](Player* other)
        {
            return other && other != bot && other->IsAlive() && !other->duel && !other->IsInCombat() && !other->IsMounted()
                && other->GetTeam() == bot->GetTeam() && !bot->IsInSameGroupWith(other) && !other->IsBeingTeleported();
        };
        Trinity::PlayerListSearcher searcher(bot, found, check);
        Cell::VisitWorldObjects(bot, searcher, range);

        Player* pick = nullptr;
        for (Player* other : found)
        {
            // AC: the target at most 3 levels above, at most 10 below; duels allowed where it stands; not ignoring the bot
            if (other->GetLevel() > bot->GetLevel() + 3 || bot->GetLevel() > other->GetLevel() + 10 || !AllowsDuels(other))
                continue;
            if (!other->GetSocial() || other->GetSocial()->HasIgnore(bot->GetGUID(), bot->GetSession()->GetAccountGUID()))
                continue;
            bool human = IsHuman(other);
            {
                std::lock_guard lock(g_duelLock);
                uint32 pair = g_pairChallenged[{ bot->GetGUID(), other->GetGUID() }];
                if (pair && now - pair < PairCooldownMs)
                    continue;
                if (human && sSocialConfig.DuelHumanCooldownMs)
                    if (uint32 last = g_humanChallenged[other->GetGUID()]; last && now - last < sSocialConfig.DuelHumanCooldownMs)
                        continue;
            }
            if (human && !other->IsInWorld())
                continue;
            if (!pick || urand(0, 1))
                pick = other;
        }
        if (!pick)
            return false;

        {
            std::lock_guard lock(g_duelLock);
            g_pairChallenged[{ bot->GetGUID(), pick->GetGUID() }] = now;
            if (IsHuman(pick))
                g_humanChallenged[pick->GetGUID()] = now;
        }
        bot->SetFacingToObject(pick);
        bot->SetSelection(pick->GetGUID());
        SpellCastResult result = bot->CastSpell(pick, DuelSpell, false);
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} challenges {} to a duel (cast result {})", bot->GetName(), pick->GetName(), uint32(result));
        return result == SPELL_CAST_OK;
    }
};

void PointAt(Player* bot, Player* opponent)
{
    if (!IsBot(bot))
        return;
    if (PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot))
    {
        ai->SetForcedTarget(opponent->GetGUID());
        bot->SetSelection(opponent->GetGUID());
    }
}

void LetGo(Player* bot, Player* opponent)
{
    if (!bot || !IsBot(bot))
        return;
    if (PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot))
    {
        if (opponent && ai->GetForcedTarget() == opponent->GetGUID())
            ai->ClearForcedTarget();
        ai->ClearChaseTarget();
    }
    if (opponent && bot->GetVictim() == opponent)
        bot->AttackStop();
}
}

void OnDuelStarted(Player* first, Player* second)
{
    if (!first || !second || (!sSocialConfig.DuelEnable && !sSocialConfig.DuelAcceptAnywhere))
        return;
    // the "duel" strategy of AC: "attack duel opponent"
    PointAt(first, second);
    PointAt(second, first);
}

void OnDuelEnded(Player* winner, Player* loser, DuelCompleteType /*type*/)
{
    LetGo(winner, loser);
    LetGo(loser, winner);
}

void RegisterDuels(AiRegistry& registry)
{
    registry.AddTrigger("social duel challenged", 1000, DuelChallenged, IntervalMode::Gate, 1500);
    registry.AddAction<AnswerDuelAction>("social answer duel");
    registry.AddTrigger("social duel due", sSocialConfig.DuelCheckMs, DuelDue, IntervalMode::Gate, 2000);
    registry.AddAction<ChallengeAction>("social challenge");
}
}
