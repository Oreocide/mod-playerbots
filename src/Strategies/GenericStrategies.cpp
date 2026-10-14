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

#include "GenericStrategies.h"
#include "Cmd/ChatCommands.h"
#include "Engine/AiContext.h"
#include "Engine/AiRegistry.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridDefines.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "Item.h"
#include "Loot.h"
#include "LootPackets.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "QuestTravel.h"
#include "Log.h"
#include "Random.h"
#include "Rpg/RpgStrategy.h"
#include "Travel/MoveToward.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_set>

namespace Playerbots
{
namespace
{
namespace Priority
{
    constexpr float Revive = 100.0f;
    constexpr float Teleport = 90.0f;
    constexpr float Heal = 80.0f;
    constexpr float Consume = 75.0f;
    constexpr float Attack = 60.0f;
    constexpr float Travel = 55.0f;
    constexpr float Buff = 48.0f;
    constexpr float Pvp = 30.0f;
    constexpr float Loot = 50.0f;
    constexpr float Idle = 10.0f;
}

constexpr float HealRange = 40.0f;
constexpr float SelfHealThreshold = 40.0f;     // non-healers only heal themselves, below this
constexpr float HealerDamageMinMana = 60.0f;   // healers only deal damage above this mana percentage
constexpr uint32 MaxCastAttempts = 3;
constexpr float LootRange = 25.0f;             // the core allows 30 yards
constexpr float EatBelowHealthPct = 70.0f;
constexpr float DrinkBelowManaPct = 50.0f;

bool IsSameInstance(Player const* bot, Player const* master)
{
    return bot->GetMapId() == master->GetMapId() && bot->GetInstanceId() == master->GetInstanceId();
}

// ---- situation checks shared by triggers, values and actions ----

Unit* FindHealTarget(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive())
        return nullptr;

    if (ai.GetRole() != BotRole::Healer)
        return bot->GetHealthPct() < SelfHealThreshold ? bot : nullptr;

    Unit* best = nullptr;
    auto consider = [&](Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !unit->IsInMap(bot) || !bot->IsWithinDistInMap(unit, HealRange))
            return;

        if (unit->GetHealthPct() >= sPlayerbotsConfig.HealThreshold)
            return;

        if (!best || unit->GetHealthPct() < best->GetHealthPct())
            best = unit;
    };

    consider(bot);
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference const& ref : group->GetMembers())
            consider(ref.GetSource());
    }
    else
        consider(ai.GetMaster());

    return best;
}

bool NeedsHealth(Player const* bot) { return bot->GetHealthPct() < EatBelowHealthPct; }
bool NeedsMana(Player const* bot)
{
    return bot->GetPowerType() == POWER_MANA && bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < DrinkBelowManaPct;
}

bool IsConsuming(Player const* bot)
{
    return bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
}

// Food restores health (SPELL_AURA_MOD_REGEN), drinks mana (SPELL_AURA_MOD_POWER_REGEN)
Item* FindConsumable(Player* bot, SpellInfo const** foundSpell = nullptr)
{
    bool wantFood = NeedsHealth(bot);
    bool wantDrink = NeedsMana(bot);
    if (!wantFood && !wantDrink)
        return nullptr;

    Item* best = nullptr;
    SpellInfo const* bestSpell = nullptr;
    int32 bestLevel = -1;

    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (itemTemplate->GetClass() != ITEM_CLASS_CONSUMABLE || itemTemplate->GetSubClass() != ITEM_SUBCLASS_FOOD_DRINK)
            return ItemSearchCallbackResult::Continue;

        if (itemTemplate->GetBaseRequiredLevel() > int32(bot->GetLevel()) || itemTemplate->GetBaseRequiredLevel() < bestLevel)
            return ItemSearchCallbackResult::Continue;

        for (ItemEffectEntry const* effect : itemTemplate->Effects)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE);
            if (!spellInfo)
                continue;

            bool food = spellInfo->HasAura(SPELL_AURA_MOD_REGEN);
            bool drink = spellInfo->HasAura(SPELL_AURA_MOD_POWER_REGEN);
            if ((wantFood && food) || (wantDrink && drink))
            {
                best = item;
                bestSpell = spellInfo;
                bestLevel = itemTemplate->GetBaseRequiredLevel();
                break;
            }
        }

        return ItemSearchCallbackResult::Continue;
    });

    if (foundSpell)
        *foundSpell = bestSpell;
    return best;
}

// ---- values ----

// The enemy the bot fights now, computed once per tick (the attack trigger and action both ask)
class CombatTargetValue final : public CalculatedValue<ObjectGuid>
{
protected:
    ObjectGuid Calculate(PlayerbotAI& ai) override
    {
        Unit* target = ai.FindCombatTarget();
        return target ? target->GetGUID() : ObjectGuid::Empty;
    }
};

class HealTargetValue final : public CalculatedValue<ObjectGuid>
{
protected:
    ObjectGuid Calculate(PlayerbotAI& ai) override
    {
        if (ai.GetSpellBook().GetHealSpells().empty())
            return ObjectGuid::Empty;

        Unit* target = FindHealTarget(ai);
        return target ? target->GetGUID() : ObjectGuid::Empty;
    }
};

class CorpseCheck
{
public:
    CorpseCheck(Player* bot, std::unordered_set<ObjectGuid> const& ignored) : _bot(bot), _ignored(ignored) { }

    bool operator()(Creature const* creature) const
    {
        if (creature->IsAlive() || _ignored.contains(creature->GetGUID()) || !_bot->IsWithinDist(creature, LootRange))
            return false;

        if (!_bot->isAllowedToLoot(creature))
            return false;

        Loot const* loot = creature->GetLootForPlayer(_bot);
        return loot && !loot->isLooted();
    }

private:
    Player* _bot;
    std::unordered_set<ObjectGuid> const& _ignored;
};

// Nearest lootable corpse. An empty search is not repeated for Playerbots.ScanInterval
class NearestCorpseValue final : public CalculatedValue<ObjectGuid>
{
protected:
    ObjectGuid Calculate(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        uint32 now = GameTime::GetGameTimeMS();
        if (_nextScan && now < _nextScan)
            return ObjectGuid::Empty;

        std::vector<Creature*> corpses;
        CorpseCheck check(bot, ai.GetLootedCorpses());
        Trinity::CreatureListSearcher searcher(bot, corpses, check);
        Cell::VisitGridObjects(bot, searcher, LootRange);

        Creature* best = nullptr;
        for (Creature* corpse : corpses)
            if (!best || bot->GetExactDistSq(corpse) < bot->GetExactDistSq(best))
                best = corpse;

        _nextScan = best ? 0 : now + sPlayerbotsConfig.ScanInterval;
        return best ? best->GetGUID() : ObjectGuid::Empty;
    }

private:
    uint32 _nextScan = 0;
};

// ---- triggers ----

bool IsDeadTrigger(PlayerbotAI& ai) { return !ai.GetBot()->IsAlive(); }

bool MasterAwayTrigger(PlayerbotAI& ai)
{
    // a bot told to go somewhere is meant to be away from its master
    if (ai.GetRoute().IsActive())
        return false;

    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    if (!master || !bot->IsAlive() || bot->IsInCombat() || master->IsBeingTeleported() || master->IsInFlight())
        return false;

    return !IsSameInstance(bot, master) || !bot->IsWithinDistInMap(master, sPlayerbotsConfig.TeleportDistance);
}

bool NeedsHealTrigger(PlayerbotAI& ai)
{
    return !ai.GetContext().GetValue<ObjectGuid>("heal target", ai).IsEmpty();
}

bool HasCombatTargetTrigger(PlayerbotAI& ai)
{
    return ai.GetBot()->IsAlive() && ai.SelectCombatTarget();
}

bool CorpseToLootTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || bot->IsBeingTeleported())
        return false;

    return !ai.GetContext().GetValue<ObjectGuid>("nearest corpse", ai).IsEmpty();
}

bool ShouldConsumeTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_CASTING))
        return false;

    Player* master = ai.GetMaster();
    if (master && (master->IsInCombat() || !bot->IsWithinDistInMap(master, 30.0f)))
        return false;

    // keep still while the food or drink is working
    if (IsConsuming(bot))
        return NeedsHealth(bot) || NeedsMana(bot);

    if (bot->isMoving())
        return false;

    return FindConsumable(bot) != nullptr;
}

bool FollowMasterTrigger(PlayerbotAI& ai)
{
    Player* master = ai.GetMaster();
    return ai.GetBot()->IsAlive() && master && IsSameInstance(ai.GetBot(), master);
}

bool IdleAloneTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return !ai.GetMaster() && bot->IsAlive() && !bot->IsInCombat() && !bot->IsBeingTeleported() && !bot->HasUnitState(UNIT_STATE_CASTING);
}

// In battlegrounds and arenas ownerless bots hunt the nearest enemy player
bool PvpTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return !ai.GetMaster() && bot->IsAlive() && !bot->IsBeingTeleported() && (bot->InBattleground() || bot->InArena());
}

bool IsAliveTrigger(PlayerbotAI& ai) { return ai.GetBot()->IsAlive(); }

bool TravelingTrigger(PlayerbotAI& ai) { return ai.GetRoute().IsActive(); }

// Out of combat, standing still, and the rotation of the specialization has buffs to keep up
bool NeedsBuffTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return sPlayerbotsConfig.RotationEnable && bot->IsAlive() && !bot->IsInCombat() && !bot->IsMounted() && !bot->IsInFlight() && !bot->isMoving()
        && !bot->HasUnitState(UNIT_STATE_CASTING) && !ai.GetRoute().IsActive() && ai.GetRotation().HasList(ai, "buffs");
}

Player* FindPvpEnemy(Player* bot)
{
    Player* best = nullptr;
    for (MapReference const& ref : bot->GetMap()->GetPlayers())
    {
        Player* other = ref.GetSource();
        if (!other || other == bot || !other->IsAlive() || !bot->IsValidAttackTarget(other) || !bot->CanSeeOrDetect(other))
            continue;

        if (!best || bot->GetExactDistSq(other) < bot->GetExactDistSq(best))
            best = other;
    }

    return best;
}

// ---- actions ----

class ReviveAction : public Action
{
public:
    char const* GetName() const override { return "revive"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (bot->IsResurrectRequested())
        {
            bot->ResurrectUsingRequestData();
            return true;
        }

        Player* master = ai.GetMaster();
        if (sPlayerbotsConfig.AutoResurrect && (!master || (master->IsAlive() && !master->IsInCombat())))
        {
            bot->ResurrectPlayer(0.5f);
            bot->SpawnCorpseBones();
        }

        return true; // nothing else to do while dead
    }
};

class TeleportToMasterAction : public Action
{
public:
    char const* GetName() const override { return "teleport to master"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Player* master = ai.GetMaster();
        if (!master)
            return false;

        bot->GetMotionMaster()->Clear();
        return bot->TeleportTo(master->GetMapId(), master->GetPositionX(), master->GetPositionY(), master->GetPositionZ(),
            master->GetOrientation(), TELE_TO_NONE, master->GetInstanceId());
    }
};

class HealAction : public Action
{
public:
    char const* GetName() const override { return "heal"; }

    bool IsPossible(PlayerbotAI& ai) override { return !ai.GetSpellBook().GetHealSpells().empty(); }

    bool Execute(PlayerbotAI& ai) override
    {
        if (sPlayerbotsConfig.RotationEnable && ai.GetRotation().HasList(ai, "heal"))
            return ai.GetRotation().Run(ai, "heal", nullptr, true);

        Unit* target = ObjectAccessor::GetUnit(*ai.GetBot(), ai.GetContext().GetValue<ObjectGuid>("heal target", ai));
        return target && BotSpellBook::CastFirstUsable(ai.GetBot(), target, ai.GetSpellBook().GetHealSpells(), MaxCastAttempts);
    }
};

class AttackAction : public Action
{
public:
    char const* GetName() const override { return "attack"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Unit* target = ai.SelectCombatTarget();
        if (!target)
            return false;

        BotRole role = ai.GetRole();
        bool ranged = role == BotRole::RangedDps || role == BotRole::Healer;

        ai.Chase(target, ranged);
        if (bot->GetVictim() != target)
            bot->Attack(target, !ranged);

        // server side facing check for casts, the chase movement turns the bot visibly
        if (!bot->isMoving())
            bot->SetOrientation(bot->GetAbsoluteAngle(target));

        if (role != BotRole::Healer || bot->GetPowerPct(POWER_MANA) > HealerDamageMinMana)
        {
            RotationRunner& rotation = ai.GetRotation();
            if (sPlayerbotsConfig.RotationEnable && rotation.HasList(ai, "combat"))
            {
                // an interrupt or a defensive first, then the rotation of the specialization
                if (!rotation.Run(ai, "interrupt", target, true) && !rotation.Run(ai, "defensive", target, true))
                    rotation.Run(ai, "combat", target, true);
            }
            else
                BotSpellBook::CastFirstUsable(bot, target, ai.GetSpellBook().GetDamageSpells(), MaxCastAttempts);
        }

        return true;
    }
};

class LootAction : public Action
{
public:
    char const* GetName() const override { return "loot"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Creature* corpse = ObjectAccessor::GetCreature(*bot, ai.GetContext().GetValue<ObjectGuid>("nearest corpse", ai));
        if (!corpse)
            return false;

        std::unordered_set<ObjectGuid>& looted = ai.GetLootedCorpses();
        if (looted.size() > 128)
            looted.clear();
        looted.insert(corpse->GetGUID()); // whatever happens below, do not retry (e.g. full bags)

        WorldSession* session = bot->GetSession();
        ObjectGuid guid = corpse->GetGUID();

        WorldPackets::Loot::LootUnit lootUnit{ WorldPacket(CMSG_LOOT_UNIT) };
        lootUnit.Unit = guid;
        session->HandleLootOpcode(lootUnit);

        if (Loot* loot = corpse->GetLootForPlayer(bot))
        {
            if (loot->gold)
            {
                WorldPackets::Loot::LootMoney lootMoney{ WorldPacket(CMSG_LOOT_MONEY) };
                session->HandleLootMoneyOpcode(lootMoney);
            }

            for (uint32 slot = 0; slot < loot->items.size() && slot < 255; ++slot)
                if (!loot->items[slot].is_looted)
                    bot->StoreLootItem(guid, uint8(slot), loot);
        }

        WorldPackets::Loot::LootRelease release{ WorldPacket(CMSG_LOOT_RELEASE) };
        release.Unit = guid;
        session->HandleLootReleaseOpcode(release);
        return true;
    }
};

class ConsumeAction : public Action
{
public:
    char const* GetName() const override { return "eat/drink"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (IsConsuming(bot))
            return true;

        SpellInfo const* spellInfo = nullptr;
        Item* item = FindConsumable(bot, &spellInfo);
        if (!item)
            return false;

        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        return bot->CastSpell(bot, spellInfo->Id, CastSpellExtraArgs().SetCastItem(item)) == SPELL_CAST_OK;
    }
};

class FollowAction : public Action
{
public:
    char const* GetName() const override { return "follow"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* master = ai.GetMaster();
        if (!master)
            return false;

        if (ai.GetBot()->GetVictim())
            ai.GetBot()->AttackStop();

        UpdateMountForTravel(ai.GetBot(), ai.GetBot()->GetExactDist2d(master));
        ai.Follow(master);
        return true;
    }
};

class WanderAction : public Action
{
public:
    char const* GetName() const override { return "wander"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (!ai.HasHome())
            ai.SetHome(bot->GetPosition());

        if (IsMovingOnFoot(bot))
            return true;

        if (bot->GetVictim())
            bot->AttackStop();

        float radius = std::max(5.0f, sPlayerbotsConfig.RandomWanderRadius);
        Position dest = bot->GetPosition();
        bot->MovePositionToFirstCollision(dest, frand(radius * 0.25f, radius), frand(-float(M_PI), float(M_PI)));
        dest.m_positionZ = GetStandZ(bot, dest.GetPositionX(), dest.GetPositionY(), ai.GetHome().GetPositionZ());
        MoveToward(bot, dest, 3.0f);
        return true;
    }
};

class PvpAction : public Action
{
public:
    char const* GetName() const override { return "pvp"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Player* enemy = FindPvpEnemy(bot);
        if (!enemy)
            return false;

        BotRole role = ai.GetRole();
        ai.Chase(enemy, role == BotRole::RangedDps || role == BotRole::Healer);
        if (bot->GetExactDist(enemy) < 40.0f)
            ai.SetForcedTarget(enemy->GetGUID());
        return true;
    }
};

// Keeps up the buffs of the specialization's rotation
class BuffAction : public Action
{
public:
    char const* GetName() const override { return "buff"; }

    bool Execute(PlayerbotAI& ai) override { return ai.GetRotation().Run(ai, "buffs", nullptr, false); }
};

// Follows the route the bot was given (Travel/RouteFollower)
class TravelAction : public Action
{
public:
    char const* GetName() const override { return "travel"; }

    bool Execute(PlayerbotAI& ai) override
    {
        switch (ai.GetRoute().Step(ai))
        {
            case RouteStatus::Arrived:
                ai.TellMaster("I'm there.");
                ai.GetRpg().ChangeToIdle();
                return true;
            case RouteStatus::Failed:
                ai.TellMaster("I could not get there.");
                ai.GetRpg().ChangeToIdle();
                return true;
            case RouteStatus::Running:
                return true;
            case RouteStatus::Idle:
                break;
        }
        return false;
    }
};

class StayAction : public Action
{
public:
    char const* GetName() const override { return "stay"; }

    bool Execute(PlayerbotAI& ai) override
    {
        if (ai.GetBot()->GetVictim())
            ai.GetBot()->AttackStop();

        ai.Stay();
        return true;
    }
};

/// A strategy that is only a list of trigger -> action offers
class TriggerStrategy final : public Strategy
{
public:
    TriggerStrategy(std::string name, std::vector<TriggerNode> nodes) : Strategy(std::move(name)), _nodes(std::move(nodes)) { }

    void InitTriggers(std::vector<TriggerNode>& triggers) override { triggers = _nodes; }

private:
    std::vector<TriggerNode> _nodes;
};

void AddTriggerStrategy(AiRegistry& registry, char const* name, std::vector<TriggerNode> nodes)
{
    registry.AddStrategy(name, [name = std::string(name), nodes = std::move(nodes)](std::string const&) -> std::unique_ptr<Strategy>
    {
        return std::make_unique<TriggerStrategy>(name, nodes);
    });
}

void RegisterGenericAi()
{
    AiRegistry::SetErrorSink([](std::string const& message) { TC_LOG_ERROR("server.playerbots", "Playerbots: {}", message); });

    AiRegistry& r = sPlayerbotsAi;

    r.AddValue("combat target", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<CombatTargetValue>(); });
    r.AddValue("heal target", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<HealTargetValue>(); });
    r.AddValue("nearest corpse", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<NearestCorpseValue>(); });

    r.AddTrigger("dead", 0, IsDeadTrigger);
    r.AddTrigger("master away", 0, MasterAwayTrigger);
    r.AddTrigger("needs heal", 0, NeedsHealTrigger);
    r.AddTrigger("has combat target", 0, HasCombatTargetTrigger);
    r.AddTrigger("corpse to loot", 0, CorpseToLootTrigger);
    r.AddTrigger("should consume", 0, ShouldConsumeTrigger);
    r.AddTrigger("follow master", 0, FollowMasterTrigger);
    r.AddTrigger("idle alone", 0, IdleAloneTrigger);
    r.AddTrigger("pvp zone", 0, PvpTrigger);
    r.AddTrigger("alive", 0, IsAliveTrigger);
    r.AddTrigger("traveling", 0, TravelingTrigger);
    r.AddTrigger("needs buff", 2000, NeedsBuffTrigger);

    r.AddAction<ReviveAction>("revive");
    r.AddAction<TeleportToMasterAction>("teleport to master");
    r.AddAction<HealAction>("heal");
    r.AddAction<AttackAction>("attack");
    r.AddAction<LootAction>("loot");
    r.AddAction<ConsumeAction>("eat/drink");
    r.AddAction<FollowAction>("follow");
    r.AddAction<WanderAction>("wander");
    r.AddAction<PvpAction>("pvp");
    r.AddAction<StayAction>("stay");
    r.AddAction<TravelAction>("travel");
    r.AddAction<BuffAction>("buff");

    AddTriggerStrategy(r, "death", { { "dead", { { "revive", Priority::Revive } } } });
    AddTriggerStrategy(r, "teleport", { { "master away", { { "teleport to master", Priority::Teleport } } } });
    AddTriggerStrategy(r, "combat", { { "needs heal", { { "heal", Priority::Heal } } }, { "has combat target", { { "attack", Priority::Attack } } } });
    AddTriggerStrategy(r, "loot", { { "corpse to loot", { { "loot", Priority::Loot } } } });
    AddTriggerStrategy(r, "consume", { { "should consume", { { "eat/drink", Priority::Consume } } } });
    AddTriggerStrategy(r, "follow", { { "follow master", { { "follow", Priority::Idle } } } });
    AddTriggerStrategy(r, "stay", { { "alive", { { "stay", Priority::Idle } } } });
    AddTriggerStrategy(r, "wander", { { "idle alone", { { "wander", Priority::Idle } } } });
    AddTriggerStrategy(r, "travel", { { "traveling", { { "travel", Priority::Travel } } } });
    AddTriggerStrategy(r, "buff", { { "needs buff", { { "buff", Priority::Buff } } } });
    AddTriggerStrategy(r, "pvp", { { "pvp zone", { { "pvp", Priority::Pvp } } } });
    r.AddStrategy("rpg", [](std::string const&) { return CreateRpgStrategy(); });
    r.AddStrategy("quest", [](std::string const&) { return CreateQuestStrategy(); });

    RegisterChatCommands();
}
}

void EnsureAiRegistered()
{
    static std::once_flag once;
    std::call_once(once, RegisterGenericAi);
}

std::unique_ptr<Strategy> CreateStrategy(std::string_view name)
{
    EnsureAiRegistered();
    return sPlayerbotsAi.CreateStrategy(name);
}
}
