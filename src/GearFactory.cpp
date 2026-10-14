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

#include "GearFactory.h"
#include "BotSpellBook.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "CollectionMgr.h"
#include "Economy/BotProfessions.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"
#include "Util.h"
#include "World.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
constexpr uint32 MaxScan = 200;       // candidates inspected per slot before giving up
constexpr uint32 PickAmongBest = 3;   // the bot takes a random one of the best N usable items
constexpr int32 GearLevelSlack = 8;   // gear more than this many levels below the bot is replaced
constexpr uint32 FoodStack = 20;

bool IsTestItem(char const* name)
{
    if (!name || !*name)
        return true;

    std::string lower(name);
    strToLower(lower);
    for (std::string_view marker : { "test", "dummy", "rks ", "deprecated", "[ph]", "(ph)", "placeholder", "zzold", "npc equip", "monster -", "dnt", "obsolete", "unused", "qa " })
        if (lower.find(marker) != std::string::npos)
            return true;

    return false;
}

bool IsTwoHanded(ItemTemplate const* proto)
{
    switch (proto->GetInventoryType())
    {
        case INVTYPE_2HWEAPON:
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
            return true;
        default:
            return false;
    }
}
}

GearFactory* GearFactory::instance()
{
    static GearFactory instance;
    return &instance;
}

void GearFactory::Build()
{
    _built = false;
    _groups.clear();
    _food.clear();
    _drink.clear();
    _bags.clear();

    _groups = {
        { { INVTYPE_HEAD },                                                            { EQUIPMENT_SLOT_HEAD }, {} },
        { { INVTYPE_NECK },                                                            { EQUIPMENT_SLOT_NECK }, {} },
        { { INVTYPE_SHOULDERS },                                                       { EQUIPMENT_SLOT_SHOULDERS }, {} },
        { { INVTYPE_CHEST, INVTYPE_ROBE },                                             { EQUIPMENT_SLOT_CHEST }, {} },
        { { INVTYPE_WAIST },                                                           { EQUIPMENT_SLOT_WAIST }, {} },
        { { INVTYPE_LEGS },                                                            { EQUIPMENT_SLOT_LEGS }, {} },
        { { INVTYPE_FEET },                                                            { EQUIPMENT_SLOT_FEET }, {} },
        { { INVTYPE_WRISTS },                                                          { EQUIPMENT_SLOT_WRISTS }, {} },
        { { INVTYPE_HANDS },                                                           { EQUIPMENT_SLOT_HANDS }, {} },
        { { INVTYPE_FINGER },                                                          { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 }, {} },
        { { INVTYPE_TRINKET },                                                         { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 }, {} },
        { { INVTYPE_CLOAK },                                                           { EQUIPMENT_SLOT_BACK }, {} },
        // Main hand first: a two handed weapon decides whether the off hand is filled
        { { INVTYPE_WEAPON, INVTYPE_2HWEAPON, INVTYPE_WEAPONMAINHAND, INVTYPE_RANGED, INVTYPE_RANGEDRIGHT }, { EQUIPMENT_SLOT_MAINHAND }, {} },
        { { INVTYPE_SHIELD, INVTYPE_WEAPONOFFHAND, INVTYPE_HOLDABLE, INVTYPE_WEAPON },  { EQUIPMENT_SLOT_OFFHAND }, {} },
    };

    // InventoryType -> group indices
    auto groupsFor = [this](uint8 type)
    {
        std::vector<Group*> result;
        for (Group& group : _groups)
            if (std::ranges::find(group.InventoryTypes, type) != group.InventoryTypes.end())
                result.push_back(&group);
        return result;
    };

    uint32 maxQuality = sPlayerbotsConfig.RandomGearMaxQuality;
    uint32 expansion = sWorld->getIntConfig(CONFIG_EXPANSION);

    for (auto const& [id, proto] : sObjectMgr->GetItemTemplateStore())
    {
        // plain bags a vendor sells, nothing quest or profession bound
        if (proto.GetClass() == ITEM_CLASS_CONTAINER && proto.GetSubClass() == ITEM_SUBCLASS_CONTAINER)
        {
            if (proto.GetQuality() <= ITEM_QUALITY_UNCOMMON && proto.GetBuyPrice() > 0 && proto.GetContainerSlots() >= 6
                && !proto.GetStartQuest() && proto.GetHolidayID() == HOLIDAY_NONE && !IsTestItem(proto.GetName(DEFAULT_LOCALE))
                && proto.GetBonding() != BIND_ON_ACQUIRE && proto.GetBonding() != BIND_QUEST)
                _bags.push_back({ id, proto.GetBaseRequiredLevel(), proto.GetContainerSlots() });
            continue;
        }

        if (proto.GetClass() == ITEM_CLASS_CONSUMABLE && proto.GetSubClass() == ITEM_SUBCLASS_FOOD_DRINK)
        {
            if (proto.IsConjuredConsumable() || proto.GetBuyPrice() == 0 || proto.GetRequiredSkill() || proto.GetStartQuest()
                || proto.GetHolidayID() != HOLIDAY_NONE || proto.GetQuality() > ITEM_QUALITY_UNCOMMON || IsTestItem(proto.GetName(DEFAULT_LOCALE)))
                continue;

            bool food = false, drink = false;
            for (ItemEffectEntry const* effect : proto.Effects)
                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE))
                {
                    food |= spell->HasAura(SPELL_AURA_MOD_REGEN);
                    drink |= spell->HasAura(SPELL_AURA_MOD_POWER_REGEN);
                }

            if (food != drink)
                (food ? _food : _drink).push_back({ id, proto.GetBaseRequiredLevel() });
            continue;
        }

        if (!proto.IsWeapon() && !proto.IsArmor())
            continue;

        if (proto.GetQuality() < ITEM_QUALITY_NORMAL || proto.GetQuality() > maxQuality)
            continue;

        if (proto.GetBaseRequiredLevel() <= 0 || proto.GetRequiredExpansion() > expansion)
            continue;

        if (proto.GetRequiredSkill() || proto.GetRequiredSpell() || proto.GetRequiredReputationFaction() || proto.GetStartQuest()
            || proto.GetHolidayID() != HOLIDAY_NONE || proto.GetDuration() || proto.GetBonding() == BIND_QUEST
            || proto.HasFlag(ITEM_FLAG_IS_BOUND_TO_ACCOUNT) || proto.GetArtifactID())
            continue;

        if (proto.IsWeapon() && (proto.GetSubClass() == ITEM_SUBCLASS_WEAPON_MISCELLANEOUS || proto.GetSubClass() == ITEM_SUBCLASS_WEAPON_FISHING_POLE))
            continue;

        if (proto.IsArmor())
        {
            if (proto.GetSubClass() == ITEM_SUBCLASS_ARMOR_COSMETIC)
                continue;

            // "miscellaneous" armor is only real gear as jewelry or held items
            if (proto.GetSubClass() == ITEM_SUBCLASS_ARMOR_MISCELLANEOUS
                && proto.GetInventoryType() != INVTYPE_NECK && proto.GetInventoryType() != INVTYPE_FINGER
                && proto.GetInventoryType() != INVTYPE_TRINKET && proto.GetInventoryType() != INVTYPE_HOLDABLE)
                continue;
        }

        if (IsTestItem(proto.GetName(DEFAULT_LOCALE)))
            continue;

        bool junkEffect = false;
        for (ItemEffectEntry const* effect : proto.Effects)
            if (effect->SpellID && BotSpellBook::IsJunk(sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE)))
            {
                junkEffect = true;
                break;
            }
        if (junkEffect)
            continue;

        for (Group* group : groupsFor(proto.GetInventoryType()))
            group->Candidates.push_back({ &proto, proto.GetBaseRequiredLevel(), proto.GetQuality(), proto.GetBaseItemLevel() });
    }

    for (Group& group : _groups)
        std::ranges::sort(group.Candidates, [](Candidate const& a, Candidate const& b)
        {
            if (a.RequiredLevel != b.RequiredLevel)
                return a.RequiredLevel > b.RequiredLevel;
            if (a.Quality != b.Quality)
                return a.Quality > b.Quality;
            if (a.ItemLevel != b.ItemLevel)
                return a.ItemLevel > b.ItemLevel;
            return a.Proto->GetId() < b.Proto->GetId();
        });

    auto byLevel = [](Consumable const& a, Consumable const& b)
    {
        return a.RequiredLevel != b.RequiredLevel ? a.RequiredLevel > b.RequiredLevel : a.ItemId < b.ItemId;
    };
    std::ranges::sort(_food, byLevel);
    std::ranges::sort(_drink, byLevel);
    std::ranges::sort(_bags, [](BagItem const& a, BagItem const& b) { return a.Slots != b.Slots ? a.Slots > b.Slots : a.ItemId < b.ItemId; });

    size_t total = 0;
    for (Group const& group : _groups)
        total += group.Candidates.size();

    _built = true;
    TC_LOG_INFO("server.playerbots", "Playerbots: gear factory knows {} equippable items, {} food and {} drink items", total, _food.size(), _drink.size());
}

bool GearFactory::NeedsGear(Player const* bot) const
{
    if (!_built)
        return false;

    // Core slots only, a missing ring or trinket is normal at low level
    static constexpr uint8 core[] = { EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_MAINHAND };
    int32 level = bot->GetLevel();
    for (uint8 slot : core)
    {
        Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            // Only complain if we know a usable item exists (cheap check: any item at all for this level range)
            if (level >= 5)
                return true;
            continue;
        }

        if (item->GetTemplate()->GetBaseRequiredLevel() + GearLevelSlack < level)
            return true;
    }

    return false;
}

bool GearFactory::EquipSlot(Player* bot, Group const& group, uint8 slot, uint32 excludedItem) const
{
    int32 level = bot->GetLevel();

    // Candidates are sorted from high to low required level, skip the ones the bot is too low for
    auto first = std::ranges::partition_point(group.Candidates, [level](Candidate const& c) { return c.RequiredLevel > level; });

    std::vector<uint32> usable;
    uint32 scanned = 0;
    for (auto itr = first; itr != group.Candidates.end() && scanned < MaxScan && usable.size() < PickAmongBest; ++itr, ++scanned)
    {
        ItemTemplate const* proto = itr->Proto;
        if (proto->GetId() == excludedItem)
            continue;

        if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;

        if (!proto->IsUsableByLootSpecialization(bot, false))
            continue;

        uint16 dest = 0;
        if (bot->CanEquipNewItem(slot, dest, proto->GetId(), true) != EQUIP_ERR_OK)
            continue;

        usable.push_back(proto->GetId());
    }

    if (usable.empty())
        return false;

    uint32 itemId = usable[urand(0, uint32(usable.size() - 1))];
    uint16 dest = 0;
    if (bot->CanEquipNewItem(slot, dest, itemId, true) != EQUIP_ERR_OK)
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);

    // Replace what is there: the upgrade path (swap) would try to store the old item in the bags
    if (bot->GetItemByPos(dest >> 8, dest & 255))
        bot->DestroyItem(dest >> 8, dest & 255, true);

    // A two handed weapon does not fit with an off hand
    if (slot == EQUIPMENT_SLOT_MAINHAND && proto && IsTwoHanded(proto) && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
        bot->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);

    return bot->EquipNewItem(dest, itemId, ItemContext::NONE, true) != nullptr;
}

void GearFactory::Equip(Player* bot) const
{
    if (!_built || !bot || !bot->IsInWorld())
        return;

    uint32 equipped = 0;
    bool twoHanded = false;
    for (Group const& group : _groups)
    {
        uint32 previous = 0;
        for (uint8 slot : group.Slots)
        {
            // Off hand is left alone for two handed weapons
            if (slot == EQUIPMENT_SLOT_OFFHAND && twoHanded)
                continue;

            if (EquipSlot(bot, group, slot, previous))
            {
                ++equipped;
                if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    previous = item->GetEntry();
                    if (slot == EQUIPMENT_SLOT_MAINHAND)
                        twoHanded = IsTwoHanded(item->GetTemplate());
                }
            }
        }
    }

    bot->AutoUnequipOffhandIfNeed();
    TC_LOG_DEBUG("server.playerbots", "Playerbots: equipped {} items on {} (level {})", equipped, bot->GetName(), bot->GetLevel());
}

uint32 GearFactory::PickConsumable(std::vector<Consumable> const& list, uint32 level)
{
    // Sorted from high to low required level, take the first that fits
    auto itr = std::ranges::find_if(list, [level](Consumable const& c) { return c.RequiredLevel <= int32(level); });
    return itr == list.end() ? 0 : itr->ItemId;
}

void GearFactory::Supply(Player* bot) const
{
    if (!_built || !bot || !bot->IsInWorld())
        return;

    auto topUp = [bot](uint32 itemId)
    {
        if (!itemId || bot->GetItemCount(itemId) >= FoodStack / 2)
            return;

        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, FoodStack) == EQUIP_ERR_OK)
            bot->StoreNewItem(dest, itemId, true);
    };

    // a player of this level rides: the faction's basic vendor mount (Chestnut Mare / Brown Wolf) into the collection
    if (bot->GetLevel() >= sPlayerbotsConfig.RpgMountMinLevel && bot->GetSession())
    {
        uint32 mount = Player::TeamForRace(bot->GetRace()) == ALLIANCE ? 6648 : 6654;
        if (!bot->HasSpell(mount))
            bot->GetSession()->GetCollectionMgr()->AddMount(mount, MOUNT_STATUS_NONE);
    }

    topUp(PickConsumable(_food, bot->GetLevel()));
    if (bot->GetMaxPower(POWER_MANA) > 0)
        topUp(PickConsumable(_drink, bot->GetLevel()));
}

void GearFactory::Provision(Player* bot) const
{
    if (!_built || !bot || !bot->IsInWorld())
        return;

    BotProfessions::Maintain(bot);

    // four bags like a player who has been out of the starting zone for a while (the backpack alone before level 5)
    if (bot->GetLevel() >= 5)
    {
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;

            for (BagItem const& bag : _bags)
            {
                if (bag.RequiredLevel > int32(bot->GetLevel()) || bag.Slots > 16)
                    continue;

                uint16 dest = (INVENTORY_SLOT_BAG_0 << 8) | slot;
                if (bot->CanEquipNewItem(slot, dest, bag.ItemId, true) != EQUIP_ERR_OK)
                    continue;

                bot->EquipNewItem(dest, bag.ItemId, ItemContext::NONE, true);
                break;
            }
        }
    }

    // pocket money grows with the level, a bot that sits under half of it is topped up to a varied amount
    uint64 level = bot->GetLevel();
    uint64 typical = level * level * 500;
    if (bot->GetMoney() < typical / 2)
        bot->SetMoney(typical * urand(60, 140) / 100);
}
}
