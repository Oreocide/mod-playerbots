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

// Trades, ported from AzerothCore's TradeAction (the "trade" order: the bot opens a trade with its master holding the named
// items and gold) and RpgTradeUsefulAction (a bot in town hands an item it cannot use to a bot that can). All trade opcodes
// go through the world queue. The master's side: the existing "handle trade" action (Social/GroupEvents.cpp) answers the
// opened trade and presses accept after the master does; the items are put in a moment later, once the window is open.

#include "BotSession.h"
#include "Cmd/CommandText.h"
#include "Engine/AiRegistry.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "Social/SocialConfig.h"
#include "Social/SocialShared.h"
#include "Social/SocialText.h"
#include "TradeData.h"
#include "TradePackets.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"
#include <atomic>
#include <map>
#include <mutex>

namespace Playerbots::Social
{
namespace
{
constexpr uint32 FillDelayMs = 2000;      // the window is open by then ("handle trade" begins it on the bot's next tick)
constexpr uint32 FillExpireMs = 30000;

struct PendingTrade
{
    ObjectGuid With;
    std::vector<ObjectGuid> Items;
    uint64 Money = 0;
    uint32 DueMs = 0;
};

std::mutex g_tradeLock;
std::map<ObjectGuid, PendingTrade> g_pendingTrades;   // bot -> what goes into the trade it opened
std::atomic<uint32> g_pendingCount{ 0 };            // size of g_pendingTrades, read without the lock by the trigger

std::pair<uint8, uint8> PositionOf(Item const* item)
{
    return { item->GetBagSlot(), item->GetSlot() };
}

void SetTradeItems(Player* bot, std::vector<ObjectGuid> const& items, uint64 money)
{
    WorldSession* session = bot->GetSession();
    uint8 tradeSlot = 0;
    for (ObjectGuid const& guid : items)
    {
        if (tradeSlot >= TRADE_SLOT_TRADED_COUNT)
            break;
        Item* item = bot->GetItemByGuid(guid);
        if (!item || !item->CanBeTraded(false, true))
            continue;
        WorldPackets::Trade::SetTradeItem packet{ WorldPacket(CMSG_SET_TRADE_ITEM) };
        packet.TradeSlot = tradeSlot++;
        auto [bag, slot] = PositionOf(item);
        packet.PackSlot = bag;
        packet.ItemSlotInPack = slot;
        session->HandleSetTradeItemOpcode(packet);
    }
    if (money)
    {
        WorldPackets::Trade::SetTradeGold gold{ WorldPacket(CMSG_SET_TRADE_GOLD) };
        gold.Coinage = std::min<uint64>(money, bot->GetMoney());
        session->HandleSetTradeGoldOpcode(gold);
    }
}

void Initiate(Player* bot, ObjectGuid with)
{
    WorldPackets::Trade::InitiateTrade packet{ WorldPacket(CMSG_INITIATE_TRADE) };
    packet.Guid = with;
    bot->GetSession()->HandleInitiateTradeOpcode(packet);
}

// ---- the "trade" order ----

bool TradeOrder(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    if (!master)
        return false;

    ItemSelection selection = ParseItemSelection(param);
    uint64 money = 0;
    for (std::string const& word : selection.Words)
        money += SocialText::ParseMoneyWord(word);

    std::vector<ObjectGuid> items;
    for (uint32 itemId : selection.Items)
    {
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            if (item->GetEntry() == itemId && item->CanBeTraded(false, true) && std::ranges::find(items, item->GetGUID()) == items.end())
            {
                items.push_back(item->GetGUID());
                return ItemSearchCallbackResult::Stop;
            }
            return ItemSearchCallbackResult::Continue;
        });
    }

    if (items.empty() && !money)
    {
        ai.TellMaster("trade <item links> [5g 20s]: I open a trade with you holding them.");
        return true;
    }
    if (money > bot->GetMoney())
    {
        ai.TellMaster("I only have " + SocialText::FormatMoney(bot->GetMoney()) + ".");
        money = bot->GetMoney();
    }
    if (bot->GetTradeData())
    {
        ai.TellMaster("I am already trading.");
        return true;
    }

    {
        std::lock_guard lock(g_tradeLock);
        g_pendingTrades[bot->GetGUID()] = { master->GetGUID(), items, money, GameTime::GetGameTimeMS() + FillDelayMs };
        g_pendingCount.store(uint32(g_pendingTrades.size()), std::memory_order_relaxed);
    }
    ObjectGuid with = master->GetGUID();
    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [with](Player* player)
    {
        if (!player->GetTradeData())
            Initiate(player, with);
    });
    return true;
}

bool TradeFillDue(PlayerbotAI& ai)
{
    // every bot asks twice a second: no lock while no trade order is pending anywhere
    if (!g_pendingCount.load(std::memory_order_relaxed))
        return false;
    Player* bot = ai.GetBot();
    std::lock_guard lock(g_tradeLock);
    auto itr = g_pendingTrades.find(bot->GetGUID());
    if (itr == g_pendingTrades.end())
        return false;
    uint32 now = GameTime::GetGameTimeMS();
    TradeData* trade = bot->GetTradeData();
    if (!trade || int32(now - itr->second.DueMs) > int32(FillExpireMs))
    {
        if (int32(now - itr->second.DueMs) > int32(FillExpireMs))
        {
            g_pendingTrades.erase(itr);
            g_pendingCount.store(uint32(g_pendingTrades.size()), std::memory_order_relaxed);
        }
        return false;
    }
    return trade->GetTrader() && trade->GetTrader()->GetGUID() == itr->second.With && int32(now - itr->second.DueMs) >= 0;
}

class TradeFillAction final : public Action
{
public:
    char const* GetName() const override { return "social trade fill"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        PendingTrade pending;
        {
            std::lock_guard lock(g_tradeLock);
            auto itr = g_pendingTrades.find(bot->GetGUID());
            if (itr == g_pendingTrades.end())
                return false;
            pending = std::move(itr->second);
            g_pendingTrades.erase(itr);
            g_pendingCount.store(uint32(g_pendingTrades.size()), std::memory_order_relaxed);
        }
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [pending](Player* player)
        {
            TradeData* trade = player->GetTradeData();
            if (trade && trade->GetTrader() && trade->GetTrader()->GetGUID() == pending.With)
                SetTradeItems(player, pending.Items, pending.Money);
        });
        return true;
    }
};

// ---- gifts between bots (AC "rpg trade useful") ----

bool IsUpgradeFor(Player* receiver, Item const* item)
{
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto->IsWeapon() && !proto->IsArmor())
        return false;
    if (receiver->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;
    uint8 slot = receiver->FindEquipSlot(item, NULL_SLOT, true);
    if (slot == NULL_SLOT)
        return false;
    Item* worn = receiver->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    return !worn || worn->GetItemLevel(receiver) + 5 <= item->GetTemplate()->GetBaseItemLevel();
}

bool GiftDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.TradeGiftEnable || !IsFreeRandomBot(ai))
        return false;
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsInCombat() || bot->GetTradeData() || !InCapital(bot) || !ai.IsSimulated())
        return false;
    return urand(1, 100) <= sSocialConfig.TradeGiftPct;
}

class GiftAction final : public Action
{
public:
    char const* GetName() const override { return "social gift"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();

        // what the bot carries, could trade, and cannot use itself
        std::vector<Item*> spare;
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if ((proto->IsWeapon() || proto->IsArmor()) && item->CanBeTraded(false, true) && !IsUpgradeFor(bot, item)
                && proto->GetQuality() >= ITEM_QUALITY_UNCOMMON)
                spare.push_back(item);
            return ItemSearchCallbackResult::Continue;
        });
        if (spare.empty())
            return false;

        std::vector<Player*> nearby;
        auto check = [bot](Player* other)
        {
            return other != bot && IsBot(other) && other->IsAlive() && !other->IsInCombat() && !other->GetTradeData()
                && other->GetTeam() == bot->GetTeam() && !other->GetGroup();
        };
        Trinity::PlayerListSearcher searcher(bot, nearby, check);
        Cell::VisitWorldObjects(bot, searcher, sSocialConfig.TradeGiftRange);

        for (Item* item : spare)
            for (Player* other : nearby)
            {
                if (!IsUpgradeFor(other, item))
                    continue;
                ObjectGuid itemGuid = item->GetGUID();
                ObjectGuid receiverGuid = other->GetGUID();
                std::string itemName = item->GetTemplate()->GetName(DEFAULT_LOCALE);
                // the queue resolves only creature and gameobject targets: the receiver is looked up in the step
                sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [itemGuid, receiverGuid](Player* giver)
                {
                    Player* receiver = ObjectAccessor::GetPlayer(*giver, receiverGuid);
                    if (!receiver || giver->GetTradeData() || receiver->GetTradeData())
                        return;
                    Initiate(giver, receiverGuid);
                    if (!giver->GetTradeData() || !receiver->GetTradeData())
                        return;
                    WorldPackets::Trade::BeginTrade begin{ WorldPacket(CMSG_BEGIN_TRADE) };
                    receiver->GetSession()->HandleBeginTradeOpcode(begin);
                    SetTradeItems(giver, { itemGuid }, 0);
                    for (Player* side : { giver, receiver })
                    {
                        TradeData* data = side->GetTradeData();
                        if (!data)
                            return;
                        WorldPackets::Trade::AcceptTrade accept{ WorldPacket(CMSG_ACCEPT_TRADE) };
                        accept.StateIndex = data->GetServerStateIndex();
                        side->GetSession()->HandleAcceptTradeOpcode(accept);
                    }
                    // whatever did not go through (a full bag) is not left open
                    if (giver->GetTradeData())
                        giver->TradeCancel(true);
                });
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} hands {} to {}", bot->GetName(), itemName, other->GetName());
                return true;
            }
        return false;
    }
};
}

void RegisterTrades(AiRegistry& registry)
{
    registry.AddTrigger("social trade fill due", 500, TradeFillDue, IntervalMode::Gate);
    registry.AddAction<TradeFillAction>("social trade fill");
    registry.AddTrigger("social gift due", sSocialConfig.TradeGiftCheckMs, GiftDue, IntervalMode::Gate, 5000);
    registry.AddAction<GiftAction>("social gift");
    AddSocialCommand("trade", "trade <item links> [5g 20s]: I open a trade with you holding them; accept and I accept too.", TradeOrder, true);
}
}
