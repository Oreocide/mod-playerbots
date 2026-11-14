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

// Public channel lines, ported from AzerothCore's SuggestWhatToDoAction / SuggestDungeonAction / SuggestTradeAction,
// BroadcastHelper (the channel routing and the per-message chances) and ChatReplyAction (answers to LFG/LFM and WTB).
// A bot that no player's party picked up posts in General, Trade or LookingForGroup now and then, only while a player is in
// its zone and in that channel; answers go to the player's channel or a whisper, a few seconds later.

#include "Channel.h"
#include "Containers.h"
#include "DB2Stores.h"
#include "Engine/AiRegistry.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFGMgr.h"
#include "Map.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "Random.h"
#include "Social/BotSocialMgr.h"
#include "Social/SocialConfig.h"
#include "Social/SocialRephrase.h"
#include "Social/SocialShared.h"
#include "Social/SocialText.h"
#include "World.h"
#include "World/WorldActionQueue.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace Playerbots::Social
{
namespace
{
// ChatChannels.db2 ids (12.1): the zone channels and the global LookingForGroup
constexpr uint32 GeneralChannel = 1;
constexpr uint32 TradeChannel = 2;
constexpr uint32 LfgChannel = 26;

constexpr uint32 ReplyCooldownMs = 25000;     // AC ChatReplyAction: "last said" chat + urand(5, 25) s

std::atomic<ChannelRephraseHook> g_rephrase = nullptr;

std::mutex g_lastSaidLock;
std::map<ObjectGuid, uint32> g_nextPost;      // bot -> earliest next suggestion (map threads)
std::map<ObjectGuid, uint32> g_nextReply;     // bot -> earliest next answer (world thread)
std::map<std::pair<uint32, uint32>, uint32> g_zonePost;  // (zone, channel) -> last bot post (world thread)

struct PendingLine
{
    ObjectGuid Bot;
    uint32 ChannelId = 0;
    std::string Text;
    uint32 DueMs = 0;
};
std::vector<PendingLine> g_pending;           // world thread

using SocialText::Category;

std::string FillLine(Category category, SocialText::Placeholders const& values)
{
    auto const& lines = SocialText::Lines(category);
    return SocialText::Fill(lines[urand(0, uint32(lines.size() - 1))], values);
}

uint32 SellMultiplierPct(Player const* bot)
{
    // AC RandomPlayerbotMgr::GetSellMultiplier: urand(80, 250) per bot; stable here (by guid)
    return 80 + uint32(bot->GetGUID().GetCounter() * 2654435761u % 171);
}

std::string ItemLinkFor(ItemTemplate const* proto, uint32 count)
{
    char const* name = proto->GetName(sWorld->GetDefaultDbcLocale());
    return SocialText::ItemLink(proto->GetId(), proto->GetQuality(), proto->GetBaseItemLevel(), name ? name : "item", count);
}

std::string QuestLinkFor(Player const* bot, Quest const* quest)
{
    return SocialText::QuestLink(quest->GetQuestId(), bot->GetQuestLevel(quest), quest->GetLogTitle());
}

std::vector<uint32> LogQuests(Player const* bot, bool incompleteOnly)
{
    std::vector<uint32> quests;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 id = bot->GetQuestSlotQuestId(slot);
        if (!id)
            continue;
        QuestStatus status = bot->GetQuestStatus(id);
        if (!incompleteOnly || status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            quests.push_back(id);
    }
    return quests;
}

/// Normal dungeons of the finder whose level range holds the bot's level (AC used a fixed list of 3.3.5 names)
std::vector<std::string> DungeonsForLevel(Player const* bot)
{
    std::vector<std::string> names;
    int8 faction = bot->GetTeam() == ALLIANCE ? 1 : 0;
    LocaleConstant locale = sWorld->GetDefaultDbcLocale();
    for (LFGDungeonsEntry const* dungeon : sLFGDungeonsStore)
    {
        if (dungeon->TypeID != lfg::LFG_TYPE_DUNGEON || dungeon->DifficultyID != DIFFICULTY_NORMAL)
            continue;
        if (dungeon->Faction != -1 && dungeon->Faction != faction)
            continue;
        Optional<ContentTuningLevels> levels = sDB2Manager.GetContentTuningData(dungeon->ContentTuningID, {});
        if (!levels || bot->GetLevel() < uint32(std::max<int16>(1, levels->MinLevel)) || (levels->MaxLevel > 0 && bot->GetLevel() > uint32(levels->MaxLevel)))
            continue;
        if (char const* name = dungeon->Name[locale]; name && *name)
            names.emplace_back(name);
    }
    std::ranges::sort(names);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

/// A player of the bot's map and zone (from the manager's once a second snapshot)
bool HumanInZone(Player const* bot)
{
    auto humans = sPlayerbotMgr->GetNearbyHumans();
    if (!humans)
        return false;
    for (PlayerbotMgr::NearbyHuman const& human : *humans)
        if (human.MapId == bot->GetMapId() && human.ZoneId == bot->GetZoneId())
            return true;
    return false;
}

/// Channel::IsOn is private: ask the player's side
bool InChannel(Player const* player, Channel const* channel)
{
    return std::ranges::find(player->GetJoinedChannels(), channel) != player->GetJoinedChannels().end();
}

Channel* JoinedChannel(Player const* player, uint32 channelId)
{
    for (Channel* channel : player->GetJoinedChannels())
        if (channel && channel->GetChannelId() == channelId)
            return channel;
    return nullptr;
}

/// World thread: the channel instance of `channelId` a player of the bot's zone and team is in, with the bot joined to it
/// (LookingForGroup is not joined on a zone change like General and Trade). nullptr = no player there to read the line
Channel* ChannelWithAudience(Player* bot, uint32 channelId)
{
    auto humans = sPlayerbotMgr->GetNearbyHumans();
    if (!humans)
        return nullptr;

    Channel* own = JoinedChannel(bot, channelId);
    for (PlayerbotMgr::NearbyHuman const& entry : *humans)
    {
        if (entry.MapId != bot->GetMapId() || entry.ZoneId != bot->GetZoneId())
            continue;
        Player* human = ObjectAccessor::FindConnectedPlayer(entry.Guid);
        if (!human || !IsHuman(human) || human->GetTeam() != bot->GetTeam())
            continue;
        if (own)
        {
            if (InChannel(human, own))
                return own;
            continue;
        }
        if (channelId != LfgChannel)
            continue;
        if (Channel* theirs = JoinedChannel(human, LfgChannel))
        {
            theirs->JoinChannel(bot);
            if (InChannel(bot, theirs))
                return theirs;
        }
    }
    return nullptr;
}

/// World thread: says the line in the channel if a player is still there to read it
void PostNow(Player* bot, uint32 channelId, std::string const& text, bool zoneCap)
{
    if (text.empty())
        return;
    Channel* channel = ChannelWithAudience(bot, channelId);
    if (!channel)
        return;

    uint32 now = GameTime::GetGameTimeMS();
    // one bot post per zone across General, Trade and LookingForGroup (user 2026-11-13: one line per 20 s per zone)
    std::pair<uint32, uint32> key{ bot->GetZoneId(), 0 };
    if (zoneCap && sSocialConfig.ZoneMinGapMs)
    {
        auto itr = g_zonePost.find(key);
        if (itr != g_zonePost.end() && now - itr->second < sSocialConfig.ZoneMinGapMs)
            return;
    }
    g_zonePost[key] = now;
    channel->Say(bot->GetGUID(), text, LANG_UNIVERSAL);
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} posted in channel {}: {}", bot->GetName(), channelId, text);
}

/// Any thread: through the rephrase hook (if a module set one), then on the world thread
void QueuePost(Player* bot, uint32 channelId, Category category, std::string text, bool zoneCap)
{
    ObjectGuid guid = bot->GetGUID();
    uint32 mapId = bot->GetMapId();
    ChannelDeliver deliver = [guid, mapId, channelId, zoneCap](std::string final)
    {
        if (final.empty())
            return;
        sWorldActionQueue->Enqueue(guid, mapId, ObjectGuid::Empty, [channelId, zoneCap, final = std::move(final)](Player* player)
        {
            PostNow(player, channelId, final, zoneCap);
        });
    };

    if (ChannelRephraseHook hook = g_rephrase.load())
        hook(guid, SocialText::CategoryName(category), text, std::move(deliver));
    else
        deliver(std::move(text));
}

/// AC BroadcastToChannelWithGlobalChance: the channels in order, each with its chance; the first that rolls takes the line
uint32 RouteChannel(std::initializer_list<std::pair<uint32, uint32>> routes)
{
    for (auto const& [channel, chance] : routes)
        if (urand(1, 100) <= chance)
            return channel;
    return 0;
}

bool Roll30000(uint32 chance)
{
    return chance && urand(1, 30000) <= chance;
}

SocialText::Placeholders BasePlaceholders(Player const* bot)
{
    return { { "%my_role", RoleWord(bot) }, { "%zone_name", ZoneName(bot) }, { "%area_name", ZoneName(bot) },
        { "%my_level", std::to_string(bot->GetLevel()) } };
}

bool SuggestQuest(Player* bot)
{
    std::vector<uint32> quests = LogQuests(bot, true);
    if (quests.empty() || !Roll30000(sSocialConfig.ChanceQuest))
        return false;
    Quest const* quest = sObjectMgr->GetQuestTemplate(quests[urand(0, uint32(quests.size() - 1))]);
    if (!quest)
        return false;
    SocialText::Placeholders values = BasePlaceholders(bot);
    values.push_back({ "%quest_link", QuestLinkFor(bot, quest) });
    // AC: { LOOKING_FOR_GROUP 50, GUILD 50, WORLD 50, GENERAL 100 } (guild chat is mod-ollama's, there is no world channel)
    uint32 channel = RouteChannel({ { LfgChannel, 50 }, { GeneralChannel, 100 } });
    QueuePost(bot, channel, Category::SuggestQuest, FillLine(Category::SuggestQuest, values), true);
    return true;
}

bool SuggestSomething(Player* bot)
{
    if (!Roll30000(sSocialConfig.ChanceSomething))
        return false;
    // AC: { GUILD 10, WORLD 70, GENERAL 100 }
    QueuePost(bot, GeneralChannel, Category::SuggestSomething, FillLine(Category::SuggestSomething, BasePlaceholders(bot)), true);
    return true;
}

bool SuggestInstance(Player* bot)
{
    if (!sSocialConfig.ChannelDungeons || !Roll30000(sSocialConfig.ChanceInstance))
        return false;
    std::vector<std::string> dungeons = DungeonsForLevel(bot);
    if (dungeons.empty())
        return false;
    SocialText::Placeholders values = BasePlaceholders(bot);
    values.push_back({ "%instance_name", dungeons[urand(0, uint32(dungeons.size() - 1))] });
    uint32 channel = RouteChannel({ { LfgChannel, 50 }, { GeneralChannel, 100 } });
    QueuePost(bot, channel, Category::SuggestInstance, FillLine(Category::SuggestInstance, values), true);
    return true;
}

bool SuggestSell(Player* bot)
{
    if (!Roll30000(sSocialConfig.ChanceSell))
        return false;

    // AC SuggestTradeAction: a quality roll, then trade goods of that quality (or lower) that are not bound
    uint32 roll = urand(0, 100);
    uint32 quality = roll > 95 ? ITEM_QUALITY_LEGENDARY : roll > 90 ? ITEM_QUALITY_EPIC : roll > 75 ? ITEM_QUALITY_RARE
        : roll > 50 ? ITEM_QUALITY_UNCOMMON : ITEM_QUALITY_NORMAL;
    std::map<uint32, uint32> counts;
    for (; quality > ITEM_QUALITY_POOR && counts.empty(); --quality)
    {
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (proto->GetQuality() == quality && proto->GetClass() == ITEM_CLASS_TRADE_GOODS && proto->GetBonding() == BIND_NONE && !item->IsSoulBound())
                counts[proto->GetId()] += item->GetCount();
            return ItemSearchCallbackResult::Continue;
        });
    }
    if (counts.empty())
        return false;

    auto pick = counts.begin();
    std::advance(pick, urand(0, uint32(counts.size() - 1)));
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(pick->first);
    if (!proto || !proto->GetSellPrice())
        return false;
    uint64 price = uint64(proto->GetSellPrice()) * pick->second * SellMultiplierPct(bot) / 100;
    SocialText::Placeholders values = BasePlaceholders(bot);
    values.push_back({ "%item_formatted_link", ItemLinkFor(proto, pick->second) });
    values.push_back({ "%item_link", ItemLinkFor(proto, 1) });
    values.push_back({ "%item_count", std::to_string(pick->second) });
    values.push_back({ "%cost_gold", SocialText::FormatMoney(std::max<uint64>(1, price)) });
    // AC: { TRADE 90, GENERAL 100 }
    uint32 channel = RouteChannel({ { TradeChannel, 90 }, { GeneralChannel, 100 } });
    QueuePost(bot, channel, Category::SuggestSell, FillLine(Category::SuggestSell, values), true);
    return true;
}

bool ChannelDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.ChannelEnable || !IsFreeRandomBot(ai))
        return false;
    Player* bot = ai.GetBot();
    // AC SuggestWhatToDoAction::isUseful: a random bot outside groups, instances and battlegrounds
    if (!bot->IsAlive() || bot->GetGroup() || bot->GetMap()->Instanceable() || bot->InBattleground() || !ai.IsSimulated())
        return false;
    // AC "often": one roll in five
    if (urand(1, sSocialConfig.ChannelOneIn) != 1)
        return false;
    {
        std::lock_guard lock(g_lastSaidLock);
        auto itr = g_nextPost.find(bot->GetGUID());
        if (itr != g_nextPost.end() && int32(itr->second - GameTime::GetGameTimeMS()) > 0)
            return false;
    }
    return HumanInZone(bot);
}

class ChannelPostAction final : public Action
{
public:
    char const* GetName() const override { return "social channel post"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        {
            // AC: "last said" = now + urand(1, 60) s, and the next suggestion waits 30 s past it
            std::lock_guard lock(g_lastSaidLock);
            uint32 gap = (sSocialConfig.ChannelMinGapSec + urand(1, std::max<uint32>(1, sSocialConfig.ChannelExtraGapSec))) * IN_MILLISECONDS;
            g_nextPost[bot->GetGUID()] = GameTime::GetGameTimeMS() + gap;
        }

        // AC picks one suggestion at random (its reputation, materials and toxic ones have no lines or a chance of 0); the
        // dungeon and trade suggestions are AC's separate actions on the same trigger, folded in here
        switch (urand(0, 3))
        {
            case 0: return SuggestQuest(bot);
            case 1: return SuggestSomething(bot);
            case 2: return SuggestInstance(bot);
            default: return SuggestSell(bot);
        }
    }
};
}

void SetChannelRephraseHook(ChannelRephraseHook hook)
{
    g_rephrase.store(hook);
}

ChannelRephraseHook GetChannelRephraseHook()
{
    return g_rephrase.load();
}

void OnPlayerChannelChat(Player* player, std::string const& message, Channel* channel)
{
    if (!sSocialConfig.ReplyEnable)
        return;

    bool lfg = SocialText::StartsWithWord(message, "LFG") || SocialText::StartsWithWord(message, "LFM");
    bool wtb = !lfg && SocialText::StartsWithWord(message, "WTB");
    if (!lfg && !wtb)
        return;
    std::set<uint32> wanted = SocialText::LinkIds(message, lfg ? "quest" : "item");
    if (wanted.empty())
        return;

    struct Candidate
    {
        Player* Bot;
        std::string Links;
        uint64 Price = 0;
    };
    std::vector<Candidate> candidates;
    uint32 now = GameTime::GetGameTimeMS();
    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI* ai)
    {
        if (ai->IsFarBot() || bot == player || bot->GetTeam() != player->GetTeam() || !InChannel(bot, channel) || !bot->IsAlive())
            return;
        if (auto itr = g_nextReply.find(bot->GetGUID()); itr != g_nextReply.end() && int32(itr->second - now) > 0)
            return;

        Candidate candidate{ bot, {}, 0 };
        if (lfg)
        {
            // AC HandleLFGQuestsReply: the quests of the line the bot also has
            for (uint32 questId : LogQuests(bot, false))
                if (wanted.contains(questId))
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                        candidate.Links += QuestLinkFor(bot, quest);
        }
        else
        {
            // AC HandleWTBItemsReply: the items of the line the bot carries and could trade
            std::map<uint32, uint32> counts;
            bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
            {
                if (wanted.contains(item->GetEntry()) && !item->IsSoulBound())
                    counts[item->GetEntry()] += item->GetCount();
                return ItemSearchCallbackResult::Continue;
            });
            for (auto const& [itemId, count] : counts)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
                {
                    candidate.Links += ItemLinkFor(proto, count);
                    candidate.Price += uint64(proto->GetSellPrice()) * count * SellMultiplierPct(bot) / 100;
                }
        }
        if (!candidate.Links.empty())
            candidates.push_back(std::move(candidate));
    });

    if (candidates.empty())
        return;
    Trinity::Containers::RandomShuffle(candidates);
    if (sSocialConfig.ReplyMaxBots && candidates.size() > sSocialConfig.ReplyMaxBots)
        candidates.resize(sSocialConfig.ReplyMaxBots);

    uint32 channelId = channel->GetChannelId();
    for (Candidate const& candidate : candidates)
    {
        Player* bot = candidate.Bot;
        g_nextReply[bot->GetGUID()] = now + urand(5000, ReplyCooldownMs);

        SocialText::Placeholders values = BasePlaceholders(bot);
        values.push_back({ "%other_name", player->GetName() });
        values.push_back({ "%quest_links", candidate.Links });
        values.push_back({ "%formatted_item_links", candidate.Links });
        values.push_back({ "%cost_gold", SocialText::FormatMoney(std::max<uint64>(1, candidate.Price)) });

        // AC: a General or Trade line gets an answer in the channel or a whisper (one in two); a LookingForGroup line only a whisper
        bool inChannel = channelId != LfgChannel && urand(0, 1);
        if (inChannel)
        {
            Category category = lfg ? Category::ReplyLfgChannel : Category::ReplyWtbChannel;
            uint32 delay = urand(sSocialConfig.ReplyMinDelayMs, sSocialConfig.ReplyMaxDelayMs);
            g_pending.push_back({ bot->GetGUID(), channelId, FillLine(category, values), now + delay });
        }
        else
        {
            Category category = lfg ? Category::ReplyLfgWhisper : Category::ReplyWtbWhisper;
            sBotSocialMgr->QueueWhisper(bot->GetGUID(), player->GetGUID(), FillLine(category, values));
        }
    }
}

void UpdateChannels(uint32 /*diff*/)
{
    if (g_pending.empty())
        return;
    uint32 now = GameTime::GetGameTimeMS();
    std::vector<PendingLine> due;
    for (auto itr = g_pending.begin(); itr != g_pending.end();)
    {
        if (int32(itr->DueMs - now) <= 0)
        {
            due.push_back(std::move(*itr));
            itr = g_pending.erase(itr);
        }
        else
            ++itr;
    }
    for (PendingLine& line : due)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(line.Bot);
        if (!bot || !bot->IsInWorld())
            continue;
        Category category = line.Text.find("|Hquest:") != std::string::npos ? Category::ReplyLfgChannel : Category::ReplyWtbChannel;
        QueuePost(bot, line.ChannelId, category, std::move(line.Text), false);
    }
}

void RegisterChannels(AiRegistry& registry)
{
    registry.AddTrigger("social channel due", sSocialConfig.ChannelCheckMs, ChannelDue, IntervalMode::Gate, 500);
    registry.AddAction<ChannelPostAction>("social channel post");
}
}
