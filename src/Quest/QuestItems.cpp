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

#include "QuestItems.h"
#include "AiRegistry.h"
#include "CellImpl.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Playerbots::QuestItems
{
namespace
{
constexpr float SearchRange = 30.0f;
constexpr uint32 MaxTriesPerRun = 3;
constexpr uint32 RetryAfterMs = 60000;     // a target an item failed on is not tried again for this long
constexpr uint32 UsedAgainAfterMs = 20000; // nor one it worked on (most quest items work once per target)
constexpr float NonCombatPriority = 47.0f; // below looting (50), above buffs (48 is buff: just under it is fine either way)
constexpr float CombatPriority = 59.0f;    // just below attacking (60): only the current enemy, a "use when weakened" item

Config sConfig;

/// A quest item with an on-use spell that needs a creature or gameobject target, while the bot has an unfinished quest
struct Candidate
{
    Item* ItemPtr = nullptr;
    SpellInfo const* Spell = nullptr;
    bool WantsGo = false;
    std::vector<uint32> Entries;    // entries the spell's target conditions name, empty: any
};

bool HasOpenQuest(Player* bot)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 questId = bot->GetQuestSlotQuestId(slot))
            if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                return true;
    return false;
}

bool IsQuestItem(ItemTemplate const* proto)
{
    return proto->GetClass() == ITEM_CLASS_QUEST || proto->GetBonding() == BIND_QUEST || proto->GetStartQuest();
}

void CollectEntries(SpellInfo const* spell, std::vector<uint32>& entries)
{
    for (SpellEffectInfo const& effect : spell->GetEffects())
    {
        if (!effect.ImplicitTargetConditions)
            continue;
        for (Condition const& condition : *effect.ImplicitTargetConditions)
            if (!condition.NegativeCondition && condition.ConditionType == CONDITION_OBJECT_ENTRY_GUID && condition.ConditionValue2)
                entries.push_back(condition.ConditionValue2);
    }
}

std::vector<Candidate> FindCandidates(Player* bot)
{
    std::vector<Candidate> result;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!IsQuestItem(proto) || proto->GetStartQuest())
            return ItemSearchCallbackResult::Continue;   // an item that starts a quest is read, not used on things

        for (ItemEffectEntry const* effect : proto->Effects)
        {
            if (effect->TriggerType != ITEM_SPELLTRIGGER_ON_USE || effect->SpellID <= 0)
                continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(effect->SpellID), DIFFICULTY_NONE);
            if (!spell || bot->GetSpellHistory()->HasCooldown(spell->Id))
                continue;

            uint32 mask = spell->GetExplicitTargetMask();
            bool unit = (mask & TARGET_FLAG_UNIT_MASK) != 0;
            bool go = (mask & TARGET_FLAG_GAMEOBJECT_MASK) != 0;
            if (!unit && !go)
                continue;   // self, area and ground spells: no way to know where they belong

            Candidate candidate;
            candidate.ItemPtr = item;
            candidate.Spell = spell;
            candidate.WantsGo = go && !unit;
            CollectEntries(spell, candidate.Entries);
            result.push_back(std::move(candidate));
            break;
        }
        return ItemSearchCallbackResult::Continue;
    });
    return result;
}

class UseQuestItemAction final : public Action
{
public:
    char const* GetName() const override { return "use quest item"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (!sConfig.Enable || bot->IsMounted() || bot->HasUnitState(UNIT_STATE_CASTING) || !HasOpenQuest(bot))
            return false;

        uint32 now = GameTime::GetGameTimeMS();
        if (_tried.size() > 256)
            _tried.clear();

        uint32 tries = 0;
        for (Candidate const& candidate : FindCandidates(bot))
        {
            for (WorldObject* target : Targets(ai, candidate))
            {
                uint64 key = Key(candidate.ItemPtr->GetEntry(), target->GetGUID());
                if (auto itr = _tried.find(key); itr != _tried.end() && int32(itr->second - now) > 0)
                    continue;
                if (tries++ >= MaxTriesPerRun)
                    return false;

                if (bot->isMoving())
                    bot->StopMoving();
                bot->SetFacingToObject(target);
                SpellCastResult result = bot->CastSpell(target, candidate.Spell->Id, CastSpellExtraArgs().SetCastItem(candidate.ItemPtr));
                _tried[key] = now + (result == SPELL_CAST_OK ? UsedAgainAfterMs : RetryAfterMs);
                if (result == SPELL_CAST_OK)
                {
                    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} used quest item {} on {}", bot->GetName(), candidate.ItemPtr->GetEntry(), target->GetName());
                    return true;
                }
            }
        }
        return false;
    }

private:
    static uint64 Key(uint32 item, ObjectGuid const& target)
    {
        return (uint64(item) << 32) ^ uint64(target.GetCounter());
    }

    std::vector<WorldObject*> Targets(PlayerbotAI& ai, Candidate const& candidate) const
    {
        Player* bot = ai.GetBot();
        float range = std::min(SearchRange, std::max(5.0f, candidate.Spell->GetMaxRange(false, bot)));
        auto wanted = [&](uint32 entry) { return candidate.Entries.empty() || std::ranges::find(candidate.Entries, entry) != candidate.Entries.end(); };
        std::vector<WorldObject*> targets;

        // in a fight only the bot's own enemy: "use it when the beast is weakened"
        if (bot->IsInCombat())
        {
            if (Unit* victim = bot->GetVictim(); victim && !candidate.WantsGo && victim->ToCreature() && wanted(victim->GetEntry()))
                targets.push_back(victim);
            return targets;
        }

        if (candidate.WantsGo)
        {
            std::vector<GameObject*> objects;
            auto check = [&](GameObject const* go) { return go->isSpawned() && wanted(go->GetEntry()) && bot->IsWithinDist(go, range); };
            Trinity::GameObjectListSearcher searcher(bot, objects, check);
            Cell::VisitGridObjects(bot, searcher, range);
            targets.assign(objects.begin(), objects.end());
        }
        else
        {
            bool dead = candidate.Spell->IsAllowingDeadTarget();
            std::vector<Creature*> creatures;
            auto check = [&](Creature const* creature)
            {
                if (creature->IsAlive() == false && !dead)
                    return false;
                // without a condition naming the target, only creatures that matter to a quest: hostile ones or quest NPCs
                if (candidate.Entries.empty() && !creature->IsHostileTo(bot) && !creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                    return false;
                return wanted(creature->GetEntry()) && bot->IsWithinDist(creature, range);
            };
            Trinity::CreatureListSearcher searcher(bot, creatures, check);
            Cell::VisitGridObjects(bot, searcher, range);
            targets.assign(creatures.begin(), creatures.end());
        }

        std::ranges::sort(targets, [bot](WorldObject const* a, WorldObject const* b) { return bot->GetExactDistSq(a) < bot->GetExactDistSq(b); });
        std::erase_if(targets, [bot](WorldObject const* target) { return !bot->IsWithinLOSInMap(target); });
        return targets;
    }

    std::unordered_map<uint64, uint32> _tried;
};

bool QuestItemTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return sConfig.Enable && bot->IsAlive() && !bot->IsBeingTeleported() && !bot->IsInFlight() && !bot->GetVehicle() && HasOpenQuest(bot);
}

class QuestItemStrategy final : public Strategy
{
public:
    QuestItemStrategy() : Strategy("quest items") { }

    uint8 GetStates() const override { return StrategyStates::Combat | StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "quest item due", { { "use quest item", NonCombatPriority } }, StrategyStates::NonCombat });
        triggers.push_back({ "quest item due", { { "use quest item", CombatPriority } }, StrategyStates::Combat });
    }
};
}

Config const& GetConfig()
{
    return sConfig;
}

void LoadConfig()
{
    Config cfg;
    cfg.Enable = sConfigMgr->GetBoolDefault("Playerbots.Quest.UseItems", false);
    sConfig = cfg;
}

void Register(AiRegistry& registry)
{
    registry.AddTrigger("quest item due", 3000, QuestItemTrigger, IntervalMode::Gate, 1000);
    registry.AddAction<UseQuestItemAction>("use quest item");
    registry.AddStrategy<QuestItemStrategy>("quest items");
}
}
