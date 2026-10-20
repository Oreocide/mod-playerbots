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

#include "Social/GroupEvents.h"
#include "BotSession.h"
#include "Engine/AiContext.h"
#include "Engine/AiRegistry.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "PartyPackets.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "QuestPackets.h"
#include "Random.h"
#include "TradeData.h"
#include "TradePackets.h"
#include "World/WorldActionQueue.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace Playerbots::GroupEvents
{
namespace
{
constexpr float ReadyCheckPriority = 70.0f;
constexpr uint32 TradeIdleCancelMs = 120000;      // an open trade nobody finishes
constexpr uint32 StrangerTradeCancelMs = 30000;   // a trade a stranger opened: left unanswered, then closed

/// The people a bot takes orders and gifts from: its master, and the players of its group
bool IsFriend(PlayerbotAI& ai, Player* other)
{
    if (!other || IsBot(other))
        return false;
    return other == ai.GetMaster() || ai.GetBot()->IsInSameGroupWith(other);
}

class AnswerReadyCheckAction final : public Action
{
public:
    char const* GetName() const override { return "answer ready check"; }

    bool Execute(PlayerbotAI& ai) override
    {
        uint32 now = GameTime::GetGameTimeMS();
        if (!_due)
        {
            _due = now + urand(1000, 4000);
            return false;
        }
        if (int32(_due - now) > 0)
            return false;
        _due = 0;

        Player* bot = ai.GetBot();
        // ready unless it is hurt or empty: a player would say no with 30% health or no mana
        bool ready = bot->IsAlive() && bot->GetHealthPct() >= 50.0f
            && (bot->GetPowerType() != POWER_MANA || !bot->GetMaxPower(POWER_MANA) || bot->GetPowerPct(POWER_MANA) >= 40.0f);

        WorldPackets::Party::ReadyCheckResponseClient packet{ WorldPacket(CMSG_READY_CHECK_RESPONSE) };
        packet.IsReady = ready;
        bot->GetSession()->HandleReadyCheckResponseOpcode(packet);
        ai.ReadyCheckAnswered() = true;
        return true;
    }

private:
    uint32 _due = 0;
};

class HandleTradeAction final : public Action
{
public:
    char const* GetName() const override { return "handle trade"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        TradeData* trade = bot->GetTradeData();
        Player* trader = trade ? trade->GetTrader() : nullptr;
        if (!trader)
        {
            Reset();
            return false;
        }

        uint32 now = GameTime::GetGameTimeMS();
        if (!_since)
            _since = now;

        // a stranger's trade is not opened; it is closed when it has waited long enough
        if (!IsFriend(ai, trader))
        {
            if (now - _since > StrangerTradeCancelMs)
                Cancel(bot);
            return false;
        }

        if (_with != trader->GetGUID())
        {
            _with = trader->GetGUID();
            _since = now;
            _acceptAt = 0;
            // the client answers the proposal by opening the window
            sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [](Player* player)
            {
                if (player->GetTradeData())
                {
                    WorldPackets::Trade::BeginTrade packet{ WorldPacket(CMSG_BEGIN_TRADE) };
                    player->GetSession()->HandleBeginTradeOpcode(packet);
                }
            });
            return true;
        }

        if (now - _since > TradeIdleCancelMs)
        {
            Cancel(bot);
            return true;
        }

        // the player has pressed accept: so does the bot, a moment later (it offers nothing; gifts and requests both end here)
        TradeData* theirs = trade->GetTraderData();
        if (theirs && theirs->IsAccepted() && !trade->IsAccepted())
        {
            if (!_acceptAt)
                _acceptAt = now + urand(800, 2000);
            else if (int32(_acceptAt - now) <= 0)
            {
                _acceptAt = 0;
                sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [](Player* player)
                {
                    if (TradeData* data = player->GetTradeData())
                    {
                        WorldPackets::Trade::AcceptTrade packet{ WorldPacket(CMSG_ACCEPT_TRADE) };
                        packet.StateIndex = data->GetServerStateIndex();
                        player->GetSession()->HandleAcceptTradeOpcode(packet);
                    }
                });
            }
            return true;
        }
        return false;
    }

private:
    void Reset()
    {
        _with.Clear();
        _since = 0;
        _acceptAt = 0;
    }

    void Cancel(Player* bot)
    {
        Reset();
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [](Player* player)
        {
            if (player->GetTradeData())
                player->TradeCancel(true);
        });
    }

    ObjectGuid _with;
    uint32 _since = 0;
    uint32 _acceptAt = 0;
};

class AcceptSharedQuestAction final : public Action
{
public:
    char const* GetName() const override { return "accept shared quest"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        uint32 now = GameTime::GetGameTimeMS();
        if (int32(_next - now) > 0)
            return false;
        _next = now + 2000;

        Player* sharer = ObjectAccessor::FindConnectedPlayer(bot->GetPlayerSharingQuest());
        int32 questId = int32(bot->GetSharedQuestID());
        if (!questId)
            return false;

        // quests offered by the master or a group player are taken, anyone else's is declined
        if (!IsFriend(ai, sharer))
        {
            bot->ClearQuestSharingInfo();
            return true;
        }

        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [questId](Player* player)
        {
            if (player->GetSharedQuestID() != uint32(questId))
                return;
            WorldPackets::Quest::QuestConfirmAccept packet{ WorldPacket(CMSG_QUEST_CONFIRM_ACCEPT) };
            packet.QuestID = questId;
            player->GetSession()->HandleQuestConfirmAccept(packet);
        });
        return true;
    }

private:
    uint32 _next = 0;
};

class GroupEventsStrategy final : public Strategy
{
public:
    GroupEventsStrategy() : Strategy("group events") { }
    uint8_t GetStates() const override { return StrategyStates::Combat | StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "event:ready check", { { "answer ready check", ReadyCheckPriority } }, StrategyStates::Combat | StrategyStates::NonCombat });
        triggers.push_back({ "event:trade", { { "handle trade", ReadyCheckPriority - 1.0f } }, StrategyStates::NonCombat });
        triggers.push_back({ "event:quest share", { { "accept shared quest", ReadyCheckPriority - 2.0f } }, StrategyStates::NonCombat });
    }
};
}

void Poll(PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.GroupEvents)
        return;

    Player* bot = ai.GetBot();
    AiContext& context = ai.GetContext();

    if (Group* group = bot->GetGroup())
    {
        if (group->IsReadyCheckStarted())
        {
            if (!ai.ReadyCheckAnswered())
                context.Raise("ready check");
        }
        else
            ai.ReadyCheckAnswered() = false;
    }

    if (TradeData* trade = bot->GetTradeData(); trade && trade->GetTrader())
        context.Raise("trade", {}, trade->GetTrader()->GetGUID().GetCounter());

    if (!bot->GetPlayerSharingQuest().IsEmpty())
        context.Raise("quest share", {}, bot->GetSharedQuestID());
}

void Register(AiRegistry& registry)
{
    registry.AddAction<AnswerReadyCheckAction>("answer ready check");
    registry.AddAction<HandleTradeAction>("handle trade");
    registry.AddAction<AcceptSharedQuestAction>("accept shared quest");
    registry.AddStrategy<GroupEventsStrategy>("group events");
}
}
