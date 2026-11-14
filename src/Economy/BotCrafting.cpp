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

#include "BotCrafting.h"
#include "AiRegistry.h"
#include "BotLootWatch.h"
#include "BotOutfits.h"
#include "Containers.h"
#include "DB2Stores.h"
#include "Engine.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ProfessionsConfig.h"
#include "ProfessionsRules.h"
#include "Random.h"
#include "RpgInfo.h"
#include "Spawn/SpawnPolicy.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellPackets.h"
#include "StringFormat.h"
#include "Travel/MoveToward.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Playerbots::Prof
{
namespace
{
// above the rpg routine (11) and questing (20): a short stop to tidy the bags, the way a player does it between pulls
constexpr float BagRelevance = 22.0f;

constexpr std::array<uint32, 9> ProductionSkills =
{
    SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING,
    SKILL_LEATHERWORKING, SKILL_TAILORING, SKILL_COOKING
};

bool IsProductionSkill(uint32 skill)
{
    return std::ranges::find(ProductionSkills, skill) != ProductionSkills.end();
}

// Retail recipes sit on an expansion skill line ("Dragon Isles Blacksmithing") whose parent is the profession itself
uint32 BaseProfessionOf(uint32 spellId)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        uint32 line = itr->second->SkillLine;
        SkillLineEntry const* entry = sSkillLineStore.LookupEntry(line);
        uint32 base = entry && entry->ParentSkillLineID ? entry->ParentSkillLineID : line;
        if (IsProductionSkill(base))
            return base;
    }
    return 0;
}

/// Casts of this spell the bags pay for (0: a reagent is missing, or it costs a currency)
uint32 AffordableCasts(Player const* bot, SpellInfo const* info, uint32 cap)
{
    if (!info->ReagentsCurrency.empty())
        return 0;
    std::vector<std::pair<uint32, uint32>> haveNeed;
    for (size_t i = 0; i < info->Reagent.size(); ++i)
        if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0)
            haveNeed.emplace_back(bot->GetItemCount(uint32(info->Reagent[i])), uint32(info->ReagentCount[i]));
    if (haveNeed.empty())
        return 0;       // AC: a recipe without reagents is not a "craft" (salvage, recraft, profession utilities)
    return Rules::AffordableCasts(haveNeed, cap);
}

bool MakesSomething(SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
            return true;
        if (effect.Effect == SPELL_EFFECT_CREATE_LOOT || effect.Effect == SPELL_EFFECT_CREATE_RANDOM_ITEM)
            return true;
    }
    return false;
}

struct Recipe
{
    uint32 SpellId = 0;
    uint32 Focus = 0;       // SpellFocusObject the recipe needs, 0 = none
    uint32 Casts = 0;
};

std::vector<Recipe> CraftableRecipes(Player* bot)
{
    std::vector<Recipe> recipes;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || info->IsPassive() || !MakesSomething(info))
            continue;
        uint32 base = BaseProfessionOf(spellId);
        if (!base || !bot->HasSkill(base))
            continue;
        uint32 casts = AffordableCasts(bot, info, sProfConfig.CraftMaxCasts);
        if (!casts)
            continue;
        recipes.push_back({ spellId, info->RequiresSpellFocus, casts });
    }
    return recipes;
}

bool InCapital(Player const* bot)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    return zone && zone->GetFlags().HasFlag(AreaFlags::LinkedChat);
}

// Anvil, Forge, Black Anvil, Black Forge (SpellFocusObject, build 12.1.0.69933), the blacksmith's show visit as before
bool IsForgeFocus(uint32 focus)
{
    return focus == 1 || focus == 3 || focus == 543 || focus == 623;
}

GameObject* FindStation(Player* bot, std::function<bool(uint32)> const& accepts)
{
    FindGameObjectOptions options;
    options.GameObjectType = GAMEOBJECT_TYPE_SPELL_FOCUS;
    std::vector<GameObject*> objects;
    bot->GetGameObjectListWithOptionsInGrid(objects, sProfConfig.CraftStationYards, options);

    Team team = Player::TeamForRace(bot->GetRace());
    GameObject* best = nullptr;
    float bestDist = sProfConfig.CraftStationYards;
    for (GameObject* go : objects)
    {
        if (!go || !accepts(go->GetGOInfo()->GetSpellFocusType()))
            continue;
        if (SpawnPolicy::IsSplitDistrict(bot->GetMapId(), go->GetAreaId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), team))
            continue;
        float dist = bot->GetExactDist2d(go);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = go;
        }
    }
    return best;
}

/// A known spell that puts down a station of this focus (a cooking fire) and that the bags pay for
uint32 StationSpell(Player const* bot, uint32 focus)
{
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || info->IsPassive())
            continue;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_TRANS_DOOR && effect.Effect != SPELL_EFFECT_SUMMON_OBJECT_WILD)
                continue;
            GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(uint32(effect.MiscValue));
            if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_SPELL_FOCUS || goInfo->GetSpellFocusType() != focus)
                continue;
            bool reagentsOk = true;
            for (size_t i = 0; i < info->Reagent.size(); ++i)
                if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0 && !bot->HasItemCount(uint32(info->Reagent[i]), uint32(info->ReagentCount[i])))
                    reagentsOk = false;
            if (reagentsOk && info->ReagentsCurrency.empty())
                return spellId;
        }
    }
    return 0;
}

uint32 KnownSpellWithEffect(Player const* bot, SpellEffects effectName, Optional<int32> miscValue = {})
{
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || info->IsPassive())
            continue;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == effectName && (!miscValue || effect.MiscValue == *miscValue))
                return spellId;
    }
    return 0;
}

// ---- per-bot timers (map threads of different maps touch this at the same time) ----

enum class Chore : uint8
{
    Disenchant,
    Enchant,
    Recipe,
    Lockbox,
    Count
};

std::mutex TimerLock;
std::unordered_map<ObjectGuid, std::array<uint32, size_t(Chore::Count)>> NextChore;

/// true once per interval per bot and chore (the first check after a login waits a random part of the interval)
bool ChoreDue(Player const* bot, Chore chore, uint32 intervalMs)
{
    uint32 now = GameTime::GetGameTimeMS();
    std::lock_guard guard(TimerLock);
    auto [itr, inserted] = NextChore.try_emplace(bot->GetGUID());
    if (inserted)
        for (uint32& at : itr->second)
            at = now + urand(intervalMs / 4, intervalMs);
    uint32& next = itr->second[size_t(chore)];
    if (int32(now - next) < 0)
        return false;
    next = now + intervalMs;
    return true;
}

/// Room for what a box or a disenchant gives (a full bag would leave the loot behind)
bool HasRoom(Player const* bot)
{
    return bot->GetFreeInventorySlotCount(ItemSearchLocation::Inventory) >= 2;
}

bool FreeToTidy(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return bot->IsAlive() && !bot->IsInCombat() && bot->getAttackers().empty() && !bot->isMoving() && !bot->IsInFlight()
        && !bot->IsBeingTeleported() && !bot->HasUnitState(UNIT_STATE_CASTING) && !bot->GetVehicle() && ai.IsSimulated();
}

// ---- disenchanting ----

bool Upgrade(Player* bot, Item* item)
{
    ItemTemplate const* proto = item->GetTemplate();
    if (bot->CanUseItem(item) != EQUIP_ERR_OK || !proto->IsUsableByLootSpecialization(bot, false))
        return false;
    uint16 dest = 0;
    if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
        return false;
    Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(dest & 255));
    return !worn || item->GetItemLevel(bot) > worn->GetItemLevel(bot);
}

Item* FindDisenchantable(Player* bot)
{
    Item* found = nullptr;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 itemClass = proto->GetClass();
        if (itemClass != ITEM_CLASS_WEAPON && itemClass != ITEM_CLASS_ARMOR)
            return ItemSearchCallbackResult::Continue;
        uint32 quality = item->GetQuality();
        if (quality < ITEM_QUALITY_UNCOMMON || quality > sProfConfig.DisenchantMaxQuality || !item->IsSoulBound()
            || proto->HasFlag(ITEM_FLAG_NO_DISENCHANT) || !item->GetDisenchantLootId())
            return ItemSearchCallbackResult::Continue;
        if (Outfits::IsOutfitItem(bot, item->GetGUID()) || Upgrade(bot, item))
            return ItemSearchCallbackResult::Continue;
        found = item;
        return ItemSearchCallbackResult::Stop;
    });
    return found;
}

class DisenchantAction final : public Action
{
public:
    char const* GetName() const override { return "disenchant gear"; }
    bool IsUseful(PlayerbotAI& ai) override { return sProfConfig.Disenchant && FreeToTidy(ai) && HasRoom(ai.GetBot()); }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        uint32 spell = KnownSpellWithEffect(bot, SPELL_EFFECT_DISENCHANT);
        Item* item = spell ? FindDisenchantable(bot) : nullptr;
        if (!item)
            return false;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell, DIFFICULTY_NONE);
        ObjectGuid itemGuid = item->GetGUID();
        if (bot->CastSpell(item, spell) != SPELL_CAST_OK)
            return false;
        LootWatch::Expect(bot, itemGuid, (info ? info->CalcCastTime() : 0) + 5000);
        return true;
    }
};

bool DisenchantDueTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return sProfConfig.Disenchant && bot->HasSkill(SKILL_ENCHANTING) && FreeToTidy(ai) && HasRoom(bot)
        && ChoreDue(bot, Chore::Disenchant, sProfConfig.DisenchantIntervalMs);
}

// ---- enchanting own gear ----

class EnchantAction final : public Action
{
public:
    char const* GetName() const override { return "enchant gear"; }
    bool IsUseful(PlayerbotAI& ai) override { return sProfConfig.Enchant && FreeToTidy(ai); }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        std::vector<Item*> bare;
        bot->ForEachItem(ItemSearchLocation::Equipment, [&](Item* item)
        {
            if (!item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))
                bare.push_back(item);
            return ItemSearchCallbackResult::Continue;
        });
        if (bare.empty())
            return false;

        std::vector<std::pair<uint32, Item*>> options;
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (known.state == PLAYERSPELL_REMOVED || known.disabled)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            if (!info || info->IsPassive() || !info->HasEffect(SPELL_EFFECT_ENCHANT_ITEM) || !AffordableCasts(bot, info, 1))
                continue;
            for (Item* item : bare)
                if (item->IsFitToSpellRequirements(info))
                    options.emplace_back(spellId, item);
        }
        if (options.empty())
            return false;

        auto const& [spell, item] = options[urand(0, uint32(options.size() - 1))];
        return bot->CastSpell(item, spell) == SPELL_CAST_OK;
    }
};

bool EnchantDueTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return sProfConfig.Enchant && bot->HasSkill(SKILL_ENCHANTING) && FreeToTidy(ai) && ChoreDue(bot, Chore::Enchant, sProfConfig.EnchantIntervalMs);
}

// ---- recipes ----

struct RecipeItem
{
    Item* ItemPtr = nullptr;
    uint32 UseSpell = 0;
};

RecipeItem FindLearnableRecipe(Player* bot)
{
    RecipeItem found;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->GetClass() != ITEM_CLASS_RECIPE || bot->CanUseItem(item) != EQUIP_ERR_OK)
            return ItemSearchCallbackResult::Continue;
        uint32 useSpell = 0;
        bool unknown = false;
        for (ItemEffectEntry const* effect : item->GetEffects())
        {
            if (effect->SpellID <= 0)
                continue;
            if (effect->TriggerType == ITEM_SPELLTRIGGER_ON_LEARN && !bot->HasSpell(uint32(effect->SpellID)))
                unknown = true;
            else if (effect->TriggerType == ITEM_SPELLTRIGGER_ON_USE && !useSpell)
                useSpell = uint32(effect->SpellID);
        }
        if (!unknown || !useSpell)
            return ItemSearchCallbackResult::Continue;
        found = { item, useSpell };
        return ItemSearchCallbackResult::Stop;
    });
    return found;
}

class LearnRecipeAction final : public Action
{
public:
    char const* GetName() const override { return "learn recipe"; }
    bool IsUseful(PlayerbotAI& ai) override { return sProfConfig.Recipes && FreeToTidy(ai); }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        RecipeItem recipe = FindLearnableRecipe(bot);
        if (!recipe.ItemPtr)
            return false;
        return bot->CastSpell(bot, recipe.UseSpell, CastSpellExtraArgs().SetCastItem(recipe.ItemPtr)) == SPELL_CAST_OK;
    }
};

bool RecipeDueTrigger(PlayerbotAI& ai)
{
    return sProfConfig.Recipes && FreeToTidy(ai) && ChoreDue(ai.GetBot(), Chore::Recipe, sProfConfig.RecipesIntervalMs);
}

// ---- lockboxes and clams ----

struct OpenPlan
{
    Item* Open = nullptr;       // has loot, not locked (or picked already)
    Item* Pick = nullptr;       // locked, and the bot can pick it
    uint32 PickSpell = 0;
};

/// A lock a rogue's Pick Lock opens (not a key lock)
bool PickableLock(uint32 lockId)
{
    LockEntry const* lock = sLockStore.LookupEntry(lockId);
    if (!lock)
        return false;
    for (size_t i = 0; i < lock->Type.size(); ++i)
        if (lock->Type[i] == LOCK_KEY_SKILL && lock->Index[i] == LOCKTYPE_LOCKPICKING)
            return true;
    return false;
}

OpenPlan FindOpenable(Player* bot)
{
    OpenPlan plan;
    uint32 pickSpell = 0;
    bool pickLooked = false;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto->HasFlag(ITEM_FLAG_HAS_LOOT) || item->IsWrapped())
            return ItemSearchCallbackResult::Continue;
        if (!proto->GetLockID() || !item->IsLocked())
        {
            plan.Open = item;
            return ItemSearchCallbackResult::Stop;
        }
        if (!plan.Pick && PickableLock(proto->GetLockID()))
        {
            if (!pickLooked)
            {
                pickLooked = true;
                pickSpell = KnownSpellWithEffect(bot, SPELL_EFFECT_OPEN_LOCK, int32(LOCKTYPE_LOCKPICKING));
            }
            if (pickSpell)
            {
                plan.Pick = item;
                plan.PickSpell = pickSpell;
            }
        }
        return ItemSearchCallbackResult::Continue;
    });
    return plan;
}

/// World thread: open the item at its current place and take its loot
void QueueOpen(Player* bot, Item* item)
{
    ObjectGuid itemGuid = item->GetGUID();
    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [itemGuid](Player* player)
    {
        Item* box = player->GetItemByGuid(itemGuid);
        if (!box || !player->IsAlive() || player->IsInCombat())
            return;
        WorldPackets::Spells::OpenItem open{ WorldPacket(CMSG_OPEN_ITEM) };
        open.Slot = box->GetBagSlot();
        open.PackSlot = box->GetSlot();
        player->GetSession()->HandleOpenItemOpcode(open);
        LootWatch::CollectLoot(player, itemGuid);
    });
}

class OpenItemsAction final : public Action
{
public:
    char const* GetName() const override { return "open items"; }
    bool IsUseful(PlayerbotAI& ai) override { return sProfConfig.Lockbox && FreeToTidy(ai) && HasRoom(ai.GetBot()); }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        OpenPlan plan = FindOpenable(bot);
        if (plan.Open)
        {
            QueueOpen(bot, plan.Open);
            return true;
        }
        if (plan.Pick)
            return bot->CastSpell(plan.Pick, plan.PickSpell) == SPELL_CAST_OK;
        return false;
    }
};

bool LockboxDueTrigger(PlayerbotAI& ai)
{
    return sProfConfig.Lockbox && FreeToTidy(ai) && HasRoom(ai.GetBot()) && ChoreDue(ai.GetBot(), Chore::Lockbox, sProfConfig.LockboxIntervalMs);
}
}

// ---- the rpg craft activity ----

bool WantsToCraft(Player* bot)
{
    if (InCapital(bot) && bot->HasSkill(SKILL_BLACKSMITHING))
        return true;
    for (uint32 skill : ProductionSkills)
        if (bot->HasSkill(skill))
            return !CraftableRecipes(bot).empty();
    return false;
}

bool DoCraftAny(PlayerbotAI& ai, uint32 now)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();

    auto finish = [&]()
    {
        if (rpg.Working)
            bot->SetEmoteState(EMOTE_ONESHOT_NONE);
        rpg.Working = false;
        rpg.ChangeToIdle();
        rpg.Started = now;
        return true;
    };

    if (!rpg.HasDest)
    {
        std::vector<Recipe> recipes = CraftableRecipes(bot);
        Trinity::Containers::RandomShuffle(recipes);

        bool planned = false;
        for (Recipe const& recipe : recipes)
        {
            if (!recipe.Focus)
            {
                rpg.SetDest(bot->GetPosition());
                planned = true;
            }
            else if (GameObject* station = FindStation(bot, [&](uint32 focus) { return focus == recipe.Focus; }))
            {
                rpg.Target = station->GetGUID();
                rpg.SetDest(station->GetPosition());
                planned = true;
            }
            else if (uint32 maker = StationSpell(bot, recipe.Focus))
            {
                rpg.TaxiDestNode = maker;
                rpg.SetDest(bot->GetPosition());
                planned = true;
            }
            if (planned)
            {
                rpg.NodeEntry = recipe.SpellId;
                rpg.NodeSpawn = urand(1, recipe.Casts);
                break;
            }
        }

        // nothing the bags pay for: a blacksmith in a capital still works at a forge for a while, as before
        if (!planned)
        {
            GameObject* forge = InCapital(bot) && bot->HasSkill(SKILL_BLACKSMITHING) ? FindStation(bot, IsForgeFocus) : nullptr;
            if (!forge)
                return finish();
            rpg.Target = forge->GetGUID();
            rpg.SetDest(forge->GetPosition());
            rpg.NodeEntry = 0;
            rpg.NodeSpawn = 0;
        }
        rpg.LastProgress = now;
    }

    if (bot->GetExactDist2d(rpg.Dest) > 3.5f)
    {
        // a station is walked up to, not into (the forge's centre is inside its model)
        MoveResult result = MoveAlongMesh(bot, rpg.Dest, 3.0f, rpg.Target.IsEmpty() ? 0.0f : 2.0f);
        if (result == MoveResult::NoPath)
            return finish();
        if (result == MoveResult::Moving)
            return true;
    }

    if (!rpg.Working)
    {
        if (bot->IsMounted())
            bot->CancelMountAura();
        bot->StopMoving();
        if (GameObject* station = rpg.Target.IsEmpty() ? nullptr : bot->GetMap()->GetGameObject(rpg.Target))
        {
            bot->SetFacingTo(bot->GetAbsoluteAngle(station));
            // the hammering of a blacksmith with nothing to make; a real recipe shows its own cast
            if (!rpg.NodeEntry && IsForgeFocus(station->GetGOInfo()->GetSpellFocusType()))
                bot->SetEmoteState(EMOTE_STATE_WORK);
        }
        rpg.Working = true;
        rpg.WaitUntil = now + urand(rpg.NodeEntry ? 1000 : 3000, rpg.NodeEntry ? 2500 : 6000);
        return true;
    }

    if (bot->HasUnitState(UNIT_STATE_CASTING))
        return true;

    // a cooking fire first, where the recipe needs one and none is near
    if (rpg.TaxiDestNode)
    {
        uint32 maker = rpg.TaxiDestNode;
        rpg.TaxiDestNode = 0;
        if (bot->CastSpell(bot, maker) != SPELL_CAST_OK)
            return finish();
        rpg.WaitUntil = now + urand(2500, 4000);
        return true;
    }

    if (rpg.NodeEntry && rpg.NodeSpawn > 0)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(rpg.NodeEntry, DIFFICULTY_NONE);
        if (!info || !AffordableCasts(bot, info, 1) || bot->CastSpell(bot, rpg.NodeEntry) != SPELL_CAST_OK)
            return finish();
        --rpg.NodeSpawn;
        rpg.WaitUntil = now + info->CalcCastTime() + urand(800, 2200);
        return true;
    }

    // the show visit at a forge lasts as long as the rpg status does; a real recipe is done once its casts are
    if (!rpg.NodeEntry && !rpg.Target.IsEmpty())
        return true;
    return finish();
}

void RegisterCraftingAi(AiRegistry& registry)
{
    registry.AddTrigger("disenchant due", 2000, DisenchantDueTrigger);
    registry.AddTrigger("enchant due", 2000, EnchantDueTrigger);
    registry.AddTrigger("recipe due", 2000, RecipeDueTrigger);
    registry.AddTrigger("lockbox due", 2000, LockboxDueTrigger);
    registry.AddAction<DisenchantAction>("disenchant gear");
    registry.AddAction<EnchantAction>("enchant gear");
    registry.AddAction<LearnRecipeAction>("learn recipe");
    registry.AddAction<OpenItemsAction>("open items");
}

std::vector<TriggerNode> CraftingTriggerNodes()
{
    return {
        { "disenchant due", { { "disenchant gear", BagRelevance } }, StrategyStates::NonCombat },
        { "enchant due", { { "enchant gear", BagRelevance } }, StrategyStates::NonCombat },
        { "recipe due", { { "learn recipe", BagRelevance } }, StrategyStates::NonCombat },
        { "lockbox due", { { "open items", BagRelevance } }, StrategyStates::NonCombat }
    };
}

bool OpenItemsOrder(PlayerbotAI& ai, std::string const& /*param*/)
{
    Player* bot = ai.GetBot();
    if (!sProfConfig.Lockbox)
    {
        ai.TellMaster("Opening boxes is off (Playerbots.Lockbox.Enable).");
        return true;
    }
    if (bot->IsInCombat())
    {
        ai.TellMaster("Not in a fight.");
        return true;
    }

    uint32 opened = 0;
    Item* toPick = nullptr;
    uint32 pickSpell = 0;
    std::vector<Item*> boxes;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->HasFlag(ITEM_FLAG_HAS_LOOT) && !item->IsWrapped())
            boxes.push_back(item);
        return ItemSearchCallbackResult::Continue;
    });
    for (Item* box : boxes)
    {
        if (!box->GetTemplate()->GetLockID() || !box->IsLocked())
        {
            QueueOpen(bot, box);
            ++opened;
        }
        else if (!toPick && PickableLock(box->GetTemplate()->GetLockID()))
        {
            if (!pickSpell)
                pickSpell = KnownSpellWithEffect(bot, SPELL_EFFECT_OPEN_LOCK, int32(LOCKTYPE_LOCKPICKING));
            if (pickSpell)
                toPick = box;
        }
    }

    if (toPick && bot->CastSpell(toPick, pickSpell) == SPELL_CAST_OK)
        ai.TellMaster(Trinity::StringFormat("Opening {} and picking a locked one, say open again after.", opened));
    else if (opened)
        ai.TellMaster(Trinity::StringFormat("Opening {}.", opened));
    else
        ai.TellMaster(boxes.empty() ? "Nothing to open." : "Only locked ones left, and I can't pick locks.");
    return true;
}
}
