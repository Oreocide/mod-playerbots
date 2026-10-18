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
#include "DB2Stores.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "CollectionMgr.h"
#include "Economy/BotProfessions.h"
#include "Economy/RaceMounts.h"
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
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace Playerbots
{
namespace
{
constexpr uint32 MaxScan = 200;       // candidates inspected per slot before giving up
constexpr int32 GearLevelSlack = 8;   // gear more than this many levels below the bot is replaced
constexpr uint32 FoodStack = 20;

// Armor slots the class's armor type governs (cloaks, jewelry, trinkets and held items are the same for everyone)
bool IsGovernedSlotType(uint8 inventoryType)
{
    switch (inventoryType)
    {
        case INVTYPE_HEAD: case INVTYPE_SHOULDERS: case INVTYPE_CHEST: case INVTYPE_ROBE: case INVTYPE_WAIST:
        case INVTYPE_LEGS: case INVTYPE_FEET: case INVTYPE_WRISTS: case INVTYPE_HANDS: case INVTYPE_SHIELD:
            return true;
        default:
            return false;
    }
}

// The armor type of the class (ChrClasses.ArmorTypeMask: a bit per item armor subclass; a warrior wears plate, a mage
// cloth) - never a lower type, and no shield for a class without the shield bit
bool FitsClassArmor(Player const* bot, ItemTemplate const* proto)
{
    if (!sPlayerbotsConfig.GearClassArmorOnly || !proto->IsArmor() || !IsGovernedSlotType(proto->GetInventoryType()))
        return true;

    ChrClassesEntry const* chrClass = sChrClassesStore.LookupEntry(bot->GetClass());
    if (!chrClass || !chrClass->ArmorTypeMask)
        return true;

    return (chrClass->ArmorTypeMask & (1u << proto->GetSubClass())) != 0;
}

int32 PrimaryItemMod(Stats primary)
{
    switch (primary)
    {
        case STAT_AGILITY: return ITEM_MOD_AGILITY;
        case STAT_INTELLECT: return ITEM_MOD_INTELLECT;
        default: return ITEM_MOD_STRENGTH;
    }
}

bool HasForeignPrimary(ItemTemplate const* proto, int32 primaryMod)
{
    for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        if (proto->GetStatPercentEditor(i) <= 0)
            continue;

        int32 stat = proto->GetStatModifierBonusStat(i);
        if (stat != ITEM_MOD_STRENGTH && stat != ITEM_MOD_AGILITY && stat != ITEM_MOD_INTELLECT)
            continue;
        if (stat != primaryMod)
            return true;
    }
    return false;
}

bool AllowedWeapon(Stats primary, uint32 subClass)
{
    auto has = [subClass](std::initializer_list<uint32> subs)
    {
        return std::find(subs.begin(), subs.end(), subClass) != subs.end();
    };

    switch (primary)
    {
        case STAT_AGILITY:
            return has({ ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_FIST_WEAPON, ITEM_SUBCLASS_WEAPON_SWORD,
                ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_BOW, ITEM_SUBCLASS_WEAPON_GUN, ITEM_SUBCLASS_WEAPON_CROSSBOW,
                ITEM_SUBCLASS_WEAPON_WARGLAIVES, ITEM_SUBCLASS_WEAPON_POLEARM, ITEM_SUBCLASS_WEAPON_SWORD2, ITEM_SUBCLASS_WEAPON_AXE2 });
        case STAT_INTELLECT:
            return has({ ITEM_SUBCLASS_WEAPON_STAFF, ITEM_SUBCLASS_WEAPON_WAND, ITEM_SUBCLASS_WEAPON_DAGGER,
                ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_SWORD });
        default:
            return has({ ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_AXE2, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_MACE2,
                ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_SWORD2, ITEM_SUBCLASS_WEAPON_POLEARM, ITEM_SUBCLASS_WEAPON_FIST_WEAPON });
    }
}

bool FitsWeapon(Player const* bot, ItemTemplate const* proto)
{
    if (proto->GetInventoryType() == INVTYPE_HOLDABLE)
        return bot->GetPrimaryStat() == STAT_INTELLECT;
    if (!proto->IsWeapon())
        return true;
    return AllowedWeapon(bot->GetPrimaryStat(), proto->GetSubClass());
}

// Retail budget: GetRandomPropertyPoints * StatPercentEditor * 0.0001, the same split Item::GetItemStatValue uses.
// Weights are shared: the spec only changes which stat is primary. A per-spec table is not seeded.
float ScoreItem(Player const* bot, ItemTemplate const* proto, bool penalizeIllegal)
{
    if (!proto)
        return -1.f;

    int32 primaryMod = PrimaryItemMod(bot->GetPrimaryStat());
    bool illegal = HasForeignPrimary(proto, primaryMod) || !FitsWeapon(bot, proto) || !FitsClassArmor(bot, proto);
    if (penalizeIllegal && illegal)
        return -1.f;

    uint32 itemLevel = proto->GetBaseItemLevel();
    float points = GetRandomPropertyPoints(itemLevel, proto->GetQuality(), proto->GetInventoryType(), proto->GetSubClass());
    float score = float(itemLevel) * 0.5f;
    for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        int32 percent = proto->GetStatPercentEditor(i);
        if (percent <= 0)
            continue;

        float amount = points * percent * 0.0001f;
        int32 stat = proto->GetStatModifierBonusStat(i);
        float weight = 0.15f;
        if (stat == primaryMod)
            weight = 1.f;
        else if (stat == ITEM_MOD_STAMINA)
            weight = 0.5f;
        else if (stat == ITEM_MOD_CRIT_RATING || stat == ITEM_MOD_HASTE_RATING || stat == ITEM_MOD_VERSATILITY || stat == ITEM_MOD_MASTERY_RATING)
            weight = 0.4f;
        score += amount * weight;
    }

    if (proto->IsWeapon())
        score += proto->GetDPS(itemLevel) * 10.f;
    return score;
}

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
    RaceMounts::Validate();
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

    // Armor of a type the class cannot wear (cloth on a warrior): gear again, unless that was tried lately and no
    // item of the right type existed for this level
    uint32 now = GameTime::GetGameTimeMS();
    {
        std::lock_guard lock(_wrongTypeLock);
        auto retry = _wrongTypeRetryAt.find(bot->GetGUID());
        if (retry != _wrongTypeRetryAt.end() && now < retry->second)
            return false;
    }

    int32 primaryMod = PrimaryItemMod(bot->GetPrimaryStat());
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!FitsClassArmor(bot, proto) || !FitsWeapon(bot, proto) || HasForeignPrimary(proto, primaryMod))
                return true;
        }

    return false;
}

bool GearFactory::EquipSlot(Player* bot, Group const& group, uint8 slot, uint32 excludedItem, GearOptions const& options) const
{
    int32 level = bot->GetLevel();
    uint32 qualityCap = options.QualityCap ? options.QualityCap : sPlayerbotsConfig.RandomGearMaxQuality;

    // Candidates are sorted from high to low required level, skip the ones the bot is too low for
    auto first = std::ranges::partition_point(group.Candidates, [level](Candidate const& c) { return c.RequiredLevel > level; });

    // strict pass: class armor, a weapon the primary stat uses, and no second primary stat. Wrong items are skipped
    // without counting against MaxScan. If nothing legal exists the second pass takes what the old rule took.
    auto search = [&](bool strict) -> std::pair<uint32, float>
    {
        uint32 bestId = 0;
        float bestScore = -1.f;
        uint32 scanned = 0;
        uint32 iterations = 0;
        for (auto itr = first; itr != group.Candidates.end() && scanned < MaxScan && iterations < MaxScan * 8; ++itr, ++iterations)
        {
            ItemTemplate const* proto = itr->Proto;
            if (proto->GetId() == excludedItem)
                continue;
            if (proto->GetQuality() > qualityCap)
                continue;
            if (options.ItemLevelCap && proto->GetBaseItemLevel() > options.ItemLevelCap)
                continue;
            if (strict && (!FitsClassArmor(bot, proto) || !FitsWeapon(bot, proto) || HasForeignPrimary(proto, PrimaryItemMod(bot->GetPrimaryStat()))))
                continue;

            ++scanned;
            if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (!proto->IsUsableByLootSpecialization(bot, false))
                continue;

            uint16 dest = 0;
            if (bot->CanEquipNewItem(slot, dest, proto->GetId(), true) != EQUIP_ERR_OK)
                continue;

            float score = ScoreItem(bot, proto, strict);
            if (score > bestScore)
            {
                bestScore = score;
                bestId = proto->GetId();
            }
        }
        return { bestId, bestScore };
    };

    auto [itemId, bestScore] = search(true);
    if (!itemId)
    {
        std::tie(itemId, bestScore) = search(false);
        if (itemId)
        {
            std::lock_guard lock(_wrongTypeLock);
            _wrongTypeRetryAt[bot->GetGUID()] = GameTime::GetGameTimeMS() + 60 * 60 * 1000;
            TC_LOG_DEBUG("server.playerbots", "Playerbots: no legal item for {} (level {}, slot {}), took a fallback", bot->GetName(), bot->GetLevel(), uint32(slot));
        }
    }

    if (!itemId)
        return false;

    if (Item const* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
    {
        float wornScore = ScoreItem(bot, worn->GetTemplate(), true);
        if (wornScore >= bestScore && wornScore >= 0.f)
            return false;
    }

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
    Equip(bot, GearOptions{});
}

GearFactory::GearArgKind GearFactory::ParseGearArg(std::string const& arg, GearOptions& options) const
{
    options = {};
    if (arg.empty())
        return GearArgKind::Ok;

    std::string text = arg;
    strToLower(text);
    if (text == "reset")
    {
        options.Reset = true;
        return GearArgKind::Ok;
    }
    if (text == "match")
        return GearArgKind::Match;
    if (text == "white")
        options.QualityCap = ITEM_QUALITY_NORMAL;
    else if (text == "green")
        options.QualityCap = ITEM_QUALITY_UNCOMMON;
    else if (text == "blue")
        options.QualityCap = ITEM_QUALITY_RARE;
    else if (text == "purple" || text == "epic")
        options.QualityCap = ITEM_QUALITY_EPIC;
    else if (!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        options.ItemLevelCap = uint32(std::strtoul(text.c_str(), nullptr, 10));
    else
        return GearArgKind::Bad;

    return options.ItemLevelCap == 0 && options.QualityCap == 0 ? GearArgKind::Bad : GearArgKind::Ok;
}

uint32 GearFactory::Equip(Player* bot, GearOptions const& options) const
{
    if (!_built || !bot || !bot->IsInWorld())
        return 0;

    if (options.Reset)
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD)
                if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

    uint32 equipped = 0;
    bool twoHanded = false;
    for (Group const& group : _groups)
    {
        uint32 previous = 0;
        for (uint8 slot : group.Slots)
        {
            if (slot == EQUIPMENT_SLOT_OFFHAND && twoHanded)
                continue;

            if (EquipSlot(bot, group, slot, previous, options))
            {
                ++equipped;
                if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    previous = item->GetEntry();
                    if (slot == EQUIPMENT_SLOT_MAINHAND)
                        twoHanded = IsTwoHanded(item->GetTemplate());
                }
            }
            else if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                previous = item->GetEntry();
                if (slot == EQUIPMENT_SLOT_MAINHAND)
                    twoHanded = IsTwoHanded(item->GetTemplate());
            }
        }
    }

    bot->AutoUnequipOffhandIfNeed();
    TC_LOG_DEBUG("server.playerbots", "Playerbots: equipped {} items on {} (level {})", equipped, bot->GetName(), bot->GetLevel());
    return equipped;
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

    // a player of this level rides: riding skill, and a vendor mount of the bot's own race (a goblin gets a trike, not the
    // Alliance horse) in the collection
    RaceMounts::Grant(bot);

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
