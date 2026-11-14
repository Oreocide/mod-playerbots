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

#include "PartyQuests.h"
#include "AiRegistry.h"
#include "BotSession.h"
#include "Cmd/ChatCommands.h"
#include "Cmd/ChatParser.h"
#include "Cmd/CommandText.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Strategies/Rpg/NpcDispatch.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"
#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Playerbots::PartyQuests
{
namespace
{
constexpr uint32 EventLifetimeMs = 30000;
constexpr size_t MaxEventsPerPlayer = 16;
constexpr uint32 NpcRescanMs = 5000;
constexpr float SyncPriority = 52.0f;      // above looting (50): the master waits at the NPC
constexpr float SharePriority = 20.0f;     // above following (10), below everything that matters

Config sConfig;

// ---- what humans took and handed in (filled on map threads by the quest hook, read by their party bots) ----

struct QuestEvent
{
    uint32 Seq = 0;
    uint32 QuestId = 0;
    bool Rewarded = false;
    ObjectGuid Npc;
    uint32 Stamp = 0;
};

std::mutex sEventLock;
std::unordered_map<ObjectGuid, std::deque<QuestEvent>> sEvents;
uint32 sNextSeq = 1;

std::vector<QuestEvent> EventsAfter(ObjectGuid player, uint32 seq)
{
    std::vector<QuestEvent> result;
    uint32 now = GameTime::GetGameTimeMS();
    std::lock_guard lock(sEventLock);
    auto itr = sEvents.find(player);
    if (itr == sEvents.end())
        return result;
    for (QuestEvent const& event : itr->second)
        if (event.Seq > seq && now - event.Stamp <= EventLifetimeMs)
            result.push_back(event);
    return result;
}

bool HasEventsAfter(ObjectGuid player, uint32 seq)
{
    uint32 now = GameTime::GetGameTimeMS();
    std::lock_guard lock(sEventLock);
    auto itr = sEvents.find(player);
    if (itr == sEvents.end())
        return false;
    return std::ranges::any_of(itr->second, [&](QuestEvent const& event) { return event.Seq > seq && now - event.Stamp <= EventLifetimeMs; });
}

/// The quest NPC (creature or gameobject) a player has open right now, empty if none
ObjectGuid QuestNpcOf(Player* player)
{
    InteractionData& data = player->PlayerTalkClass->GetInteractionData();
    if (data.Type != PlayerInteractionType::QuestGiver)
        return ObjectGuid::Empty;
    return data.SourceGuid;
}

// ---- turning in and taking ----

WorldObject* FindQuestNpc(Player* bot, ObjectGuid guid)
{
    if (guid.IsGameObject())
        return ObjectAccessor::GetGameObject(*bot, guid);
    if (guid.IsCreatureOrVehicle())
        return ObjectAccessor::GetCreature(*bot, guid);
    return nullptr;
}

bool TryAccept(Player* bot, WorldObject* npc, Quest const* quest)
{
    if (!quest || bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE || !npc->hasQuest(quest->GetQuestId()))
        return false;
    if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
        return false;

    if (Creature* creature = npc->ToCreature())
        BeginInteraction(bot, creature, 3000);
    bot->AddQuestAndCheckCompletion(quest, npc);
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} took quest {} with its party at {}", bot->GetName(), quest->GetQuestId(), npc->GetName());
    return true;
}

/// The reward the master asked for ("reward <item>"), else the best usable piece, else the most valuable
bool ChooseReward(Player* bot, Quest const* quest, std::vector<uint32> const& wanted, LootItemType& type, uint32& rewardId)
{
    type = LootItemType::Item;
    rewardId = 0;
    if (!quest->GetRewChoiceItemsCount())
        return true;

    for (uint32 want : wanted)
        for (uint32 i = 0; i < quest->RewardChoiceItemId.size(); ++i)
            if (quest->RewardChoiceItemId[i] == want && bot->CanRewardQuest(quest, quest->RewardChoiceItemType[i], want, false))
            {
                type = quest->RewardChoiceItemType[i];
                rewardId = want;
                return true;
            }

    uint32 bestScore = 0;
    bool found = false;
    for (uint32 i = 0; i < quest->RewardChoiceItemId.size(); ++i)
    {
        uint32 id = quest->RewardChoiceItemId[i];
        if (!id || !bot->CanRewardQuest(quest, quest->RewardChoiceItemType[i], id, false))
            continue;

        uint32 score = 1;
        if (quest->RewardChoiceItemType[i] == LootItemType::Item)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
            {
                if ((proto->IsWeapon() || proto->IsArmor()) && proto->IsUsableByLootSpecialization(bot, false) && bot->CanUseItem(proto) == EQUIP_ERR_OK)
                    score += 1000000;
                score += proto->GetSellPrice() / 100;
            }

        if (!found || score > bestScore)
        {
            found = true;
            bestScore = score;
            type = quest->RewardChoiceItemType[i];
            rewardId = id;
        }
    }
    return found;
}

std::vector<uint32> RewardPrefs(PlayerbotAI& ai)
{
    return ai.GetContext().GetValue<std::vector<uint32>>("reward prefs", ai);
}

bool TryReward(PlayerbotAI& ai, WorldObject* npc, Quest const* quest)
{
    Player* bot = ai.GetBot();
    if (!quest || bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_COMPLETE || !npc->hasInvolvedQuest(quest->GetQuestId()))
        return false;

    LootItemType type = LootItemType::Item;
    uint32 rewardId = 0;
    if (!ChooseReward(bot, quest, RewardPrefs(ai), type, rewardId) || !bot->CanRewardQuest(quest, type, rewardId, false))
        return false;

    if (Creature* creature = npc->ToCreature())
        BeginInteraction(bot, creature, 3000);
    bot->RewardQuest(quest, type, rewardId, npc);
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} handed in quest {} with its party at {}", bot->GetName(), quest->GetQuestId(), npc->GetName());
    return true;
}

// ---- strategy ----

class PartyQuestSyncAction final : public Action
{
public:
    char const* GetName() const override { return "party quest sync"; }

    uint32 LastSeq() const { return _lastSeq; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Player* master = ai.GetMaster();
        // the master's quest log and open NPC are read below: only on the bot's map (its map thread, MapUpdate.Threads > 1)
        if (!master || !sConfig.Sync || !bot->IsInMap(master))
            return false;

        bool did = false;
        for (QuestEvent const& event : EventsAfter(master->GetGUID(), _lastSeq))
        {
            _lastSeq = std::max(_lastSeq, event.Seq);
            WorldObject* npc = FindQuestNpc(bot, event.Npc);
            if (!npc || bot->GetExactDist(npc) > sConfig.Range)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(event.QuestId);
            did |= event.Rewarded ? TryReward(ai, npc, quest) : TryAccept(bot, npc, quest);
        }

        // the master stands at a quest NPC: hand in what the bot finished for it, take what the master already has from it
        uint32 now = GameTime::GetGameTimeMS();
        ObjectGuid npcGuid = QuestNpcOf(master);
        if (!npcGuid.IsEmpty() && (npcGuid != _lastNpc || now >= _nextNpcScan))
        {
            _lastNpc = npcGuid;
            _nextNpcScan = now + NpcRescanMs;
            if (WorldObject* npc = FindQuestNpc(bot, npcGuid); npc && bot->GetExactDist(npc) <= sConfig.Range)
            {
                for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                    if (uint32 questId = bot->GetQuestSlotQuestId(slot))
                        did |= TryReward(ai, npc, sObjectMgr->GetQuestTemplate(questId));
                for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                    if (uint32 questId = master->GetQuestSlotQuestId(slot))
                        did |= TryAccept(bot, npc, sObjectMgr->GetQuestTemplate(questId));
            }
        }
        return did;
    }

private:
    uint32 _lastSeq = 0;
    ObjectGuid _lastNpc;
    uint32 _nextNpcScan = 0;
};

PartyQuestSyncAction* GetSyncAction(PlayerbotAI& ai)
{
    return dynamic_cast<PartyQuestSyncAction*>(ai.GetContext().GetAction("party quest sync"));
}

bool PartyQuestTrigger(PlayerbotAI& ai)
{
    if (!sConfig.Sync)
        return false;
    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    if (!master || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || master->GetMapId() != bot->GetMapId())
        return false;

    PartyQuestSyncAction const* action = GetSyncAction(ai);
    uint32 seq = action ? action->LastSeq() : 0;
    return HasEventsAfter(master->GetGUID(), seq) || !QuestNpcOf(master).IsEmpty();
}

/// A member near the bot who has not got the quest and can take it
bool SomeoneCanTake(Player* bot, Quest const* quest)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;
    for (GroupReference const& ref : group->GetMembers())
    {
        Player* member = ref.GetSource();
        if (!member || member == bot || !member->IsInMap(bot) || bot->GetExactDist(member) > sConfig.Range)
            continue;
        if (member->GetQuestStatus(quest->GetQuestId()) == QUEST_STATUS_NONE && member->CanTakeQuest(quest, false))
            return true;
    }
    return false;
}

void PushToParty(Player* bot, uint32 questId)
{
    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [questId](Player* player)
    {
        if (!player->GetGroup() || player->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE)
            return;
        WorldPackets::Quest::PushQuestToParty push{ WorldPacket(CMSG_PUSH_QUEST_TO_PARTY) };
        push.QuestID = questId;
        player->GetSession()->HandlePushQuestToParty(push);
    });
}

bool Shareable(Quest const* quest)
{
    return quest && quest->HasFlag(QUEST_FLAGS_SHARABLE);
}

class ShareQuestsAction final : public Action
{
public:
    char const* GetName() const override { return "share quests"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Group* group = bot->GetGroup();
        if (!group || !sConfig.Share)
            return false;

        if (group->GetGUID() != _group)
        {
            _group = group->GetGUID();
            _shared.clear();
        }

        // one quest at a time, like a player clicking Share
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = bot->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!Shareable(quest) || _shared.contains(questId) || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            if (!SomeoneCanTake(bot, quest))
                continue;
            _shared.insert(questId);
            PushToParty(bot, questId);
            return true;
        }
        return false;
    }

private:
    ObjectGuid _group;
    std::unordered_set<uint32> _shared;
};

bool ShareDueTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Group* group = bot->GetGroup();
    return sConfig.Share && group && !group->isBGGroup() && !group->isBFGroup() && bot->IsAlive() && !bot->IsInCombat();
}

class PartyQuestStrategy final : public Strategy
{
public:
    PartyQuestStrategy() : Strategy("party quests") { }

    uint8 GetStates() const override { return StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "party quest event", { { "party quest sync", SyncPriority } }, StrategyStates::NonCombat });
        triggers.push_back({ "quest share due", { { "share quests", SharePriority } }, StrategyStates::NonCombat });
    }
};

// ---- orders ----

std::vector<uint32> ParseQuestLinks(std::string_view text)
{
    std::vector<uint32> ids;
    constexpr std::string_view Tag = "|Hquest:";
    size_t pos = 0;
    while ((pos = text.find(Tag, pos)) != std::string_view::npos)
    {
        pos += Tag.size();
        uint32 id = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
            id = id * 10 + uint32(text[pos++] - '0');
        if (id)
            ids.push_back(id);
    }
    return ids;
}

bool ShareCommand(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    if (!bot->GetGroup())
    {
        ai.TellMaster("I am not in a group.");
        return true;
    }

    std::vector<uint32> wanted = ParseQuestLinks(param);
    uint32 count = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || (!wanted.empty() && std::ranges::find(wanted, questId) == wanted.end()))
            continue;
        if (!Shareable(sObjectMgr->GetQuestTemplate(questId)))
            continue;
        PushToParty(bot, questId);
        ++count;
    }
    ai.TellMaster(count ? "Sharing " + std::to_string(count) + " quest(s)." : std::string("Nothing I can share."));
    return true;
}

bool RewardCommand(PlayerbotAI& ai, std::string const& param)
{
    auto* value = dynamic_cast<ManualValue<std::vector<uint32>>*>(ai.GetContext().FindValue<std::vector<uint32>>("reward prefs"));
    if (!value)
        return false;

    std::string lower = ToLower(Trim(param));
    if (lower == "reset")
    {
        value->Set({});
        ai.TellMaster("I pick quest rewards myself again.");
        return true;
    }

    ItemSelection selection = ParseItemSelection(param);
    if (selection.Items.empty())
    {
        ai.TellMaster("reward <item link>: the reward I take when I hand in a quest that offers it (reward reset: my own pick).");
        return true;
    }

    std::vector<uint32> prefs = value->Get(ai);
    for (uint32 item : selection.Items)
    {
        std::erase(prefs, item);
        prefs.insert(prefs.begin(), item);
    }
    if (prefs.size() > 16)
        prefs.resize(16);
    value->Set(prefs);

    // at a quest NPC with the master: hand in now
    bool handedIn = false;
    if (Player* master = ai.GetMaster())
        if (WorldObject* npc = FindQuestNpc(ai.GetBot(), QuestNpcOf(master)); npc && ai.GetBot()->GetExactDist(npc) <= sConfig.Range)
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (uint32 questId = ai.GetBot()->GetQuestSlotQuestId(slot))
                    handedIn |= TryReward(ai, npc, sObjectMgr->GetQuestTemplate(questId));

    ai.TellMaster(handedIn ? "Done." : "I will take that reward when I hand the quest in.");
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

void AddOrder(AiRegistry& registry, char const* name, char const* help, OrderAction::Fn fn)
{
    std::string action = std::string("cmd ") + name;
    registry.AddAction(action, [action, fn](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<OrderAction>(action, fn); });
    ChatCommandInfo info;
    info.Action = action;
    info.Help = help;
    info.Persist = false;
    info.WhisperOnly = true;    // hands quests around and in: never from a party chat line ("use the door" rule)
    GetChatCommands().Add(name, info);
}
}

Config const& GetConfig()
{
    return sConfig;
}

void LoadConfig()
{
    Config cfg;
    cfg.Sync = sConfigMgr->GetBoolDefault("Playerbots.Party.QuestSync", false);
    cfg.Share = sConfigMgr->GetBoolDefault("Playerbots.Party.QuestShare", false);
    cfg.Range = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Party.QuestRange", 30.0f));
    sConfig = cfg;
}

void OnQuestStatusChange(Player* player, uint32 questId)
{
    if (!sConfig.Sync || !player || IsBot(player) || !player->GetGroup())
        return;

    ObjectGuid npc = QuestNpcOf(player);
    if (npc.IsEmpty())
        return;     // progress in the field, or a quest pushed without an NPC: nothing for the party to copy

    QuestStatus status = player->GetQuestStatus(questId);
    bool rewarded = status == QUEST_STATUS_REWARDED || (status == QUEST_STATUS_NONE && player->IsQuestRewarded(questId));
    if (!rewarded && status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
        return;

    QuestEvent event;
    event.QuestId = questId;
    event.Rewarded = rewarded;
    event.Npc = npc;
    event.Stamp = GameTime::GetGameTimeMS();

    std::lock_guard lock(sEventLock);
    event.Seq = sNextSeq++;
    std::deque<QuestEvent>& events = sEvents[player->GetGUID()];
    events.push_back(event);
    while (events.size() > MaxEventsPerPlayer || (!events.empty() && event.Stamp - events.front().Stamp > EventLifetimeMs))
        events.pop_front();
}

void Register(AiRegistry& registry)
{
    registry.AddValue("reward prefs", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<ManualValue<std::vector<uint32>>>(); });
    registry.AddTrigger("party quest event", 1000, PartyQuestTrigger);
    registry.AddTrigger("quest share due", 10000, ShareDueTrigger, IntervalMode::Gate, 5000);
    registry.AddAction<PartyQuestSyncAction>("party quest sync");
    registry.AddAction<ShareQuestsAction>("share quests");
    registry.AddStrategy<PartyQuestStrategy>("party quests");

    AddOrder(registry, "share", "share [quest links]: share my quests with the group (all I can share without links).", ShareCommand);
    AddOrder(registry, "reward", "reward <item link>: the quest reward to take when I hand a quest in (reward reset: my own pick).", RewardCommand);
}
}
