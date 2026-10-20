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

#include "ItemCommands.h"
#include "CommandText.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "DB2Stores.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellPackets.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
std::string NameOf(uint32 itemId)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    return proto ? proto->GetName(sWorld->GetDefaultDbcLocale()) : Trinity::StringFormat("item {}", itemId);
}

std::string Money(uint64 copper)
{
    return Trinity::StringFormat("{}g {}s {}c", copper / 10000, (copper / 100) % 100, copper % 100);
}

bool IsGreyWord(std::string const& word)
{
    return word == "grey" || word == "greys" || word == "gray" || word == "grays" || word == "junk";
}

/// The vendor the master has selected, if this bot can talk to it now
Creature* SelectedVendor(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    // the master's target on this bot's map (a whisper can come from another one)
    Creature* vendor = master ? ObjectAccessor::GetCreature(*bot, master->GetTarget()) : nullptr;
    if (!vendor || !vendor->IsVendor())
    {
        ai.TellMaster("Select the vendor first.");
        return nullptr;
    }

    if (!bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_VENDOR, UNIT_NPC_FLAG_2_NONE))
    {
        ai.TellMaster(Trinity::StringFormat("I need to stand next to {} ({:.0f} yards away).", vendor->GetName(), bot->GetDistance(vendor)));
        return nullptr;
    }

    return vendor;
}

/// The count asked for in a command ("buy [Water] 5"), 1 if none
uint32 CountIn(ItemSelection const& selection)
{
    for (std::string const& word : selection.Words)
        if (!word.empty() && std::ranges::all_of(word, [](unsigned char c) { return std::isdigit(c); }) && word.size() < 4)
            return std::max<uint32>(1, uint32(std::stoul(word)));
    return 1;
}

Item* InBags(Player* bot, uint32 itemId)
{
    return bot->GetItemByEntry(itemId, ItemSearchLocation::Inventory);
}
}

bool EquipItems(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    if (selection.Items.empty())
    {
        ai.TellMaster("Link the items to equip.");
        return true;
    }

    if (bot->IsInCombat())
    {
        ai.TellMaster("Not in combat.");
        return false;
    }

    uint32 equipped = 0;
    for (uint32 itemId : selection.Items)
    {
        Item* item = InBags(bot, itemId);
        if (!item)
        {
            ai.TellMaster(Trinity::StringFormat("I don't have {} in my bags.", NameOf(itemId)));
            continue;
        }

        uint16 dest = 0;
        InventoryResult result = bot->CanEquipItem(NULL_SLOT, dest, item, true);
        if (result != EQUIP_ERR_OK)
        {
            ai.TellMaster(Trinity::StringFormat("I can't equip {}.", NameOf(itemId)));
            continue;
        }

        bot->SwapItem(item->GetPos(), dest);
        ++equipped;
    }

    if (equipped)
        ai.TellMaster(Trinity::StringFormat("Equipped {} {}.", equipped, equipped == 1 ? "item" : "items"));
    return equipped > 0;
}

bool UnequipItems(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    if (selection.Items.empty())
    {
        ai.TellMaster("Link the items to take off.");
        return true;
    }

    if (bot->IsInCombat())
    {
        ai.TellMaster("Not in combat.");
        return false;
    }

    uint32 removed = 0;
    for (uint32 itemId : selection.Items)
    {
        Item* item = bot->GetItemByEntry(itemId, ItemSearchLocation::Equipment);
        if (!item)
        {
            ai.TellMaster(Trinity::StringFormat("I'm not wearing {}.", NameOf(itemId)));
            continue;
        }

        ItemPosCountVec dest;
        if (bot->CanUnequipItem(item->GetPos(), false) != EQUIP_ERR_OK
            || bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
        {
            ai.TellMaster(Trinity::StringFormat("I can't take off {}.", NameOf(itemId)));
            continue;
        }

        bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        bot->StoreItem(dest, item, true);
        ++removed;
    }

    if (removed)
        ai.TellMaster(Trinity::StringFormat("Took off {} {}.", removed, removed == 1 ? "item" : "items"));
    return removed > 0;
}

bool UseItems(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    if (selection.Items.empty())
    {
        ai.TellMaster("Link the item to use.");
        return true;
    }

    Player* master = ai.GetMaster();
    Unit* selected = master ? ObjectAccessor::GetUnit(*bot, master->GetTarget()) : nullptr;

    bool used = false;
    for (uint32 itemId : selection.Items)
    {
        Item* item = bot->GetItemByEntry(itemId, ItemSearchLocation::Default);
        if (!item)
        {
            ai.TellMaster(Trinity::StringFormat("I don't have {}.", NameOf(itemId)));
            continue;
        }

        SpellInfo const* spellInfo = nullptr;
        for (ItemEffectEntry const* effect : item->GetTemplate()->Effects)
            if (effect->TriggerType == ITEM_SPELLTRIGGER_ON_USE && effect->SpellID > 0)
                if ((spellInfo = sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE)))
                    break;

        if (!spellInfo || bot->CanUseItem(item) != EQUIP_ERR_OK)
        {
            ai.TellMaster(Trinity::StringFormat("I can't use {}.", NameOf(itemId)));
            continue;
        }

        Unit* target = spellInfo->NeedsExplicitUnitTarget() ? selected : bot;
        if (!target)
        {
            ai.TellMaster(Trinity::StringFormat("{} needs a target, select one.", NameOf(itemId)));
            continue;
        }

        if (bot->CastSpell(target, spellInfo->Id, CastSpellExtraArgs().SetCastItem(item)) == SPELL_CAST_OK)
            used = true;
        else
            ai.TellMaster(Trinity::StringFormat("{} does not work right now.", NameOf(itemId)));
    }

    return used;
}

bool SellItems(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    bool greys = std::ranges::any_of(selection.Words, IsGreyWord);
    if (selection.Items.empty() && !greys)
    {
        ai.TellMaster("Link the items to sell, or say \"sell grey\".");
        return true;
    }

    Creature* vendor = SelectedVendor(ai);
    if (!vendor)
        return false;

    std::vector<Item*> stacks;
    for (uint32 itemId : selection.Items)
        if (Item* item = InBags(bot, itemId); item && std::ranges::find(stacks, item) == stacks.end())
            stacks.push_back(item);

    if (greys)
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            if (item->GetTemplate()->GetQuality() == ITEM_QUALITY_POOR && std::ranges::find(stacks, item) == stacks.end())
                stacks.push_back(item);
            return ItemSearchCallbackResult::Continue;
        });

    uint64 before = bot->GetMoney();
    uint32 sold = 0;
    for (Item* item : stacks)
    {
        if (bot->CanSellItemToVendor(item, item->GetCount()))
            continue;

        if (!bot->SellItemToVendor(item, item->GetCount()))
            ++sold;
    }

    if (!sold)
    {
        ai.TellMaster("Nothing to sell.");
        return false;
    }

    ai.TellMaster(Trinity::StringFormat("Sold {} {} for {}.", sold, sold == 1 ? "stack" : "stacks", Money(bot->GetMoney() - before)));
    return true;
}

bool BuyItems(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    if (selection.Items.empty())
    {
        ai.TellMaster("Link the items to buy, and a count if more than one.");
        return true;
    }

    Creature* vendor = SelectedVendor(ai);
    if (!vendor)
        return false;

    VendorItemData const* offers = vendor->GetVendorItems();
    uint32 count = CountIn(selection);
    uint32 bought = 0;
    for (uint32 itemId : selection.Items)
    {
        uint32 slot = 0;
        for (; offers && slot < offers->GetItemCount(); ++slot)
            if (VendorItem const* offer = offers->GetItem(slot); offer->item == itemId && offer->Type == ITEM_VENDOR_TYPE_ITEM)
                break;

        if (!offers || slot >= offers->GetItemCount())
        {
            ai.TellMaster(Trinity::StringFormat("{} does not sell {}.", vendor->GetName(), NameOf(itemId)));
            continue;
        }

        uint32 had = bot->GetItemCount(itemId);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, itemId, count, NULL_BAG, NULL_SLOT);
        if (bot->GetItemCount(itemId) > had)
            ++bought;
        else
            ai.TellMaster(Trinity::StringFormat("I can't buy {}.", NameOf(itemId)));
    }

    if (bought)
        ai.TellMaster(Trinity::StringFormat("Bought {}.", bought == 1 ? NameOf(selection.Items.front()) : Trinity::StringFormat("{} items", bought)));
    return bought > 0;
}

bool DestroyItems(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    if (selection.Items.empty())
    {
        ai.TellMaster("Link the items to destroy.");
        return true;
    }

    uint32 destroyed = 0;
    for (uint32 itemId : selection.Items)
    {
        Item* item = InBags(bot, itemId);
        if (!item)
        {
            ai.TellMaster(Trinity::StringFormat("I don't have {}.", NameOf(itemId)));
            continue;
        }

        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        ++destroyed;
    }

    if (destroyed)
        ai.TellMaster(Trinity::StringFormat("Destroyed {} {}.", destroyed, destroyed == 1 ? "stack" : "stacks"));
    return destroyed > 0;
}
}
