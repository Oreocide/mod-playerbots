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

// Character bank and guild bank, ported from AzerothCore's BankAction ("bank": list, deposit, "-" withdraw, at a banker) and
// GuildBankAction ("gb": deposit at a guild vault), plus two habits of players in town: a bot that stands at a banker (the
// rpg life walks bots to bankers) stores the reagents and trade goods it carries, and a guilded bot next to a guild vault
// puts a share of its spare gold in. Every bank opcode runs on the world queue after a banker activation, like a client.

#include "Bag.h"
#include "BankPackets.h"
#include "Cmd/CommandText.h"
#include "Creature.h"
#include "Engine/AiRegistry.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Guild.h"
#include "GuildPackets.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Random.h"
#include "Social/SocialConfig.h"
#include "Social/SocialShared.h"
#include "Social/SocialText.h"
#include "Travel/MoveToward.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"
#include <map>
#include <mutex>

namespace Playerbots::Social
{
namespace
{
constexpr float BankerReach = 5.0f;           // a bot this close to a banker uses it
constexpr float OrderReach = 10.0f;           // a bank or guild bank order works this close to one
constexpr float VaultSearch = 15.0f;          // a guild vault this close is walked to
constexpr uint32 GuildBankCooldownMs = 900000;

std::mutex g_bankLock;
std::map<ObjectGuid, uint32> g_nextBank;      // bot -> earliest next deposit at a banker
std::map<ObjectGuid, uint32> g_nextVault;     // bot -> earliest next guild bank deposit

Creature* NearestBanker(Player* bot, float range)
{
    Creature* best = nullptr;
    auto check = [bot, range](Creature* creature)
    {
        return creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_BANKER) && !creature->IsHostileTo(bot) && bot->IsWithinDistInMap(creature, range);
    };
    std::vector<Creature*> found;
    Trinity::CreatureListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, range);
    for (Creature* creature : found)
        if (!best || bot->GetExactDistSq(creature) < bot->GetExactDistSq(best))
            best = creature;
    return best;
}

GameObject* NearestVault(Player* bot, float range)
{
    GameObject* best = nullptr;
    auto check = [bot, range](GameObject* go)
    {
        return go->GetGoType() == GAMEOBJECT_TYPE_GUILD_BANK && go->isSpawned() && bot->IsWithinDistInMap(go, range);
    };
    std::vector<GameObject*> found;
    Trinity::GameObjectListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, range);
    for (GameObject* go : found)
        if (!best || bot->GetExactDistSq(go) < bot->GetExactDistSq(best))
            best = go;
    return best;
}

/// What a player keeps in the bank rather than the bags: reagents and trade goods (not quest items, not what it wears)
bool BankWorthy(Item const* item)
{
    ItemTemplate const* proto = item->GetTemplate();
    return (proto->GetClass() == ITEM_CLASS_TRADE_GOODS || proto->GetClass() == ITEM_CLASS_REAGENT) && proto->GetBonding() != BIND_QUEST
        && !proto->GetStartQuest();
}

/// World thread, the bot next to `banker`: opens the bank like a client, buys the first tab if asked to, then stores each item
void Deposit(Player* bot, ObjectGuid banker, std::vector<ObjectGuid> const& items, bool buyTab)
{
    WorldSession* session = bot->GetSession();
    WorldPackets::Bank::BankerActivate activate{ WorldPacket(CMSG_BANKER_ACTIVATE) };
    activate.Banker = banker;
    activate.InteractionType = PlayerInteractionType::CharacterBanker;
    session->HandleBankerActivateOpcode(activate);

    if (buyTab && !bot->GetCharacterBankTabCount())
    {
        WorldPackets::Bank::BuyBankTab buy{ WorldPacket(CMSG_BUY_ACCOUNT_BANK_TAB) };
        buy.Banker = banker;
        buy.BankType = BankType::Character;
        session->HandleBuyBankTab(buy);
    }

    for (ObjectGuid const& guid : items)
    {
        Item* item = bot->GetItemByGuid(guid);
        if (!item || bot->IsBankPos(item->GetBagSlot(), item->GetSlot()))
            continue;
        WorldPackets::Bank::AutoBankItem packet{ WorldPacket(CMSG_AUTOBANK_ITEM) };
        packet.BankType = BankType::Character;
        packet.Bag = item->GetBagSlot();
        packet.Slot = item->GetSlot();
        session->HandleAutoBankItemOpcode(packet);
    }
}

void Withdraw(Player* bot, ObjectGuid banker, std::vector<ObjectGuid> const& items)
{
    WorldSession* session = bot->GetSession();
    WorldPackets::Bank::BankerActivate activate{ WorldPacket(CMSG_BANKER_ACTIVATE) };
    activate.Banker = banker;
    activate.InteractionType = PlayerInteractionType::CharacterBanker;
    session->HandleBankerActivateOpcode(activate);

    for (ObjectGuid const& guid : items)
    {
        Item* item = bot->GetItemByGuid(guid);
        if (!item || !bot->IsBankPos(item->GetBagSlot(), item->GetSlot()))
            continue;
        WorldPackets::Bank::AutoStoreBankItem packet{ WorldPacket(CMSG_AUTOSTORE_BANK_ITEM) };
        packet.Bag = item->GetBagSlot();
        packet.Slot = item->GetSlot();
        session->HandleAutoStoreBankItemOpcode(packet);
    }
}

// ---- habits ----

bool BankDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.BankEnable || !IsFreeRandomBot(ai))
        return false;
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || !InCapital(bot) || !ai.IsSimulated())
        return false;
    {
        std::lock_guard lock(g_bankLock);
        if (auto itr = g_nextBank.find(bot->GetGUID()); itr != g_nextBank.end() && int32(itr->second - GameTime::GetGameTimeMS()) > 0)
            return false;
    }
    return NearestBanker(bot, BankerReach) != nullptr;
}

class BankAction final : public Action
{
public:
    char const* GetName() const override { return "social bank"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        {
            std::lock_guard lock(g_bankLock);
            g_nextBank[bot->GetGUID()] = GameTime::GetGameTimeMS() + sSocialConfig.BankCooldownMs + urand(0, 60000);
        }
        Creature* banker = NearestBanker(bot, BankerReach);
        if (!banker)
            return false;

        std::vector<ObjectGuid> items;
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            if (BankWorthy(item))
                items.push_back(item->GetGUID());
            return ItemSearchCallbackResult::Continue;
        });
        if (items.empty())
            return false;

        bot->SetFacingToObject(banker);
        ObjectGuid bankerGuid = banker->GetGUID();
        bool buyTab = sSocialConfig.BankBuyTab;
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), bankerGuid, [bankerGuid, items, buyTab](Player* player)
        {
            Deposit(player, bankerGuid, items, buyTab);
        });
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} puts {} stacks in the bank", bot->GetName(), items.size());
        return true;
    }
};

uint64 SpareGold(Player const* bot)
{
    uint64 money = bot->GetMoney();
    if (money <= sSocialConfig.GuildBankKeepCopper)
        return 0;
    return (money - sSocialConfig.GuildBankKeepCopper) * sSocialConfig.GuildBankDepositPct / 100;
}

bool GuildBankDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.GuildBankEnable || !IsFreeRandomBot(ai))
        return false;
    Player* bot = ai.GetBot();
    if (!bot->GetGuild() || !bot->IsAlive() || bot->IsInCombat() || !InCapital(bot) || !ai.IsSimulated() || SpareGold(bot) < 10000)
        return false;
    {
        std::lock_guard lock(g_bankLock);
        if (auto itr = g_nextVault.find(bot->GetGUID()); itr != g_nextVault.end() && int32(itr->second - GameTime::GetGameTimeMS()) > 0)
            return false;
    }
    return NearestVault(bot, VaultSearch) != nullptr;
}

class GuildBankAction final : public Action
{
public:
    char const* GetName() const override { return "social guild bank"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        GameObject* vault = NearestVault(bot, VaultSearch);
        if (!vault)
            return false;

        if (!bot->IsWithinDistInMap(vault, 4.0f))
        {
            if (MoveAlongMesh(bot, vault->GetPosition(), 3.0f) != MoveResult::NoPath)
                return true;
            std::lock_guard lock(g_bankLock);
            g_nextVault[bot->GetGUID()] = GameTime::GetGameTimeMS() + GuildBankCooldownMs;
            return false;
        }

        {
            std::lock_guard lock(g_bankLock);
            g_nextVault[bot->GetGUID()] = GameTime::GetGameTimeMS() + GuildBankCooldownMs + urand(0, 300000);
        }
        uint64 money = SpareGold(bot);
        if (!money)
            return false;
        bot->StopMoving();
        bot->SetFacingToObject(vault);
        ObjectGuid vaultGuid = vault->GetGUID();
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), vaultGuid, [vaultGuid, money](Player* player)
        {
            WorldPackets::Guild::GuildBankDepositMoney deposit{ WorldPacket(CMSG_GUILD_BANK_DEPOSIT_MONEY) };
            deposit.Banker = vaultGuid;
            deposit.Money = money;
            player->GetSession()->HandleGuildBankDepositMoney(deposit);
        });
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} puts {} in the guild bank", bot->GetName(), SocialText::FormatMoney(money));
        return true;
    }
};

// ---- orders ----

std::string ItemList(Player* bot, ItemSearchLocation where, size_t max)
{
    std::map<uint32, uint32> counts;
    bot->ForEachItem(where, [&](Item* item)
    {
        if (where != ItemSearchLocation::Bank || bot->IsBankPos(item->GetBagSlot(), item->GetSlot()))
            counts[item->GetEntry()] += item->GetCount();
        return ItemSearchCallbackResult::Continue;
    });
    std::string out;
    size_t shown = 0;
    for (auto const& [itemId, count] : counts)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;
        if (shown++ == max)
        {
            out += " and " + std::to_string(counts.size() - max) + " more";
            break;
        }
        out += (out.empty() ? "" : ", ") + SocialText::ItemLink(itemId, proto->GetQuality(), proto->GetBaseItemLevel(), proto->GetName(DEFAULT_LOCALE), count);
    }
    return out;
}

bool BankOrder(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    bool withdraw = std::ranges::any_of(selection.Words, [](std::string const& word)
    {
        return word == "-" || word.starts_with("-") || word == "take" || word == "withdraw";
    });

    if (selection.Items.empty())
    {
        std::string list = ItemList(bot, ItemSearchLocation::Bank, 15);
        ai.TellMaster(list.empty() ? "My bank is empty." : "In my bank: " + list + ".");
        return true;
    }

    Creature* banker = NearestBanker(bot, OrderReach);
    if (!banker)
    {
        ai.TellMaster("There is no banker next to me.");
        return true;
    }

    std::vector<ObjectGuid> items;
    for (uint32 itemId : selection.Items)
        bot->ForEachItem(withdraw ? ItemSearchLocation::Bank : ItemSearchLocation::Inventory, [&](Item* item)
        {
            bool inBank = bot->IsBankPos(item->GetBagSlot(), item->GetSlot());
            if (item->GetEntry() == itemId && inBank == withdraw && std::ranges::find(items, item->GetGUID()) == items.end())
                items.push_back(item->GetGUID());
            return ItemSearchCallbackResult::Continue;
        });
    if (items.empty())
    {
        ai.TellMaster(withdraw ? "I have none of them in the bank." : "I have none of them in my bags.");
        return true;
    }

    ObjectGuid bankerGuid = banker->GetGUID();
    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), bankerGuid, [bankerGuid, items, withdraw](Player* player)
    {
        if (withdraw)
            Withdraw(player, bankerGuid, items);
        else
            Deposit(player, bankerGuid, items, false);
    });
    ai.TellMaster(withdraw ? "Taking them out of the bank." : "Putting them in the bank.");
    return true;
}

bool GuildBankOrder(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    Guild* guild = bot->GetGuild();
    if (!guild)
    {
        ai.TellMaster("I am not in a guild.");
        return true;
    }

    ItemSelection selection = ParseItemSelection(param);
    uint64 money = 0;
    for (std::string const& word : selection.Words)
        money += SocialText::ParseMoneyWord(word);
    if (selection.Items.empty() && !money)
    {
        ai.TellMaster("gb <item links> [5g]: I put them in the guild bank at a guild vault. The guild bank holds "
            + SocialText::FormatMoney(guild->GetBankMoney()) + ".");
        return true;
    }

    GameObject* vault = NearestVault(bot, OrderReach);
    if (!vault)
    {
        ai.TellMaster("There is no guild vault next to me.");
        return true;
    }

    std::vector<std::pair<uint8, uint8>> positions;
    for (uint32 itemId : selection.Items)
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            std::pair<uint8, uint8> position{ item->GetBagSlot(), item->GetSlot() };
            if (item->GetEntry() == itemId && !item->IsSoulBound() && std::ranges::find(positions, position) == positions.end())
            {
                positions.push_back(position);
                return ItemSearchCallbackResult::Stop;
            }
            return ItemSearchCallbackResult::Continue;
        });

    money = std::min<uint64>(money, bot->GetMoney());
    ObjectGuid vaultGuid = vault->GetGUID();
    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), vaultGuid, [vaultGuid, positions, money](Player* player)
    {
        WorldSession* session = player->GetSession();
        if (money)
        {
            WorldPackets::Guild::GuildBankDepositMoney deposit{ WorldPacket(CMSG_GUILD_BANK_DEPOSIT_MONEY) };
            deposit.Banker = vaultGuid;
            deposit.Money = money;
            session->HandleGuildBankDepositMoney(deposit);
        }
        // the first tab, a free slot found by the guild (NULL_SLOT); a guild without a bought tab refuses
        for (auto const& [bag, slot] : positions)
        {
            WorldPackets::Guild::AutoGuildBankItem deposit{ WorldPacket(CMSG_AUTO_GUILD_BANK_ITEM) };
            deposit.Banker = vaultGuid;
            deposit.BankTab = 0;
            deposit.BankSlot = NULL_SLOT;
            if (bag != INVENTORY_SLOT_BAG_0)
                deposit.ContainerSlot = bag;
            deposit.ContainerItemSlot = slot;
            session->HandleAutoGuildBankItem(deposit);
        }
    });
    ai.TellMaster("Putting it in the guild bank.");
    return true;
}
}

void RegisterBanking(AiRegistry& registry)
{
    registry.AddTrigger("social bank due", 5000, BankDue, IntervalMode::Gate, 2000);
    registry.AddAction<BankAction>("social bank");
    registry.AddTrigger("social guild bank due", 10000, GuildBankDue, IntervalMode::Gate, 5000);
    registry.AddAction<GuildBankAction>("social guild bank");
    AddSocialCommand("bank", "bank [item links | - item links]: what is in my bank, or put in / take out at a banker.", BankOrder, true);
    AddSocialCommand("gb", "gb <item links> [5g]: put them in the guild bank at a guild vault.", GuildBankOrder, true);
}
}
