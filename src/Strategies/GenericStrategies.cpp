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
#include "DB2Stores.h"
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
#include "Battleground.h"
#include "GameObject.h"
#include "World/WorldActionQueue.h"
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
#include <unordered_map>
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
    constexpr float World = 40.0f;
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

        if (bot->InBattleground() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            static std::mutex repopLock;
            static std::unordered_map<ObjectGuid, uint32> repopAt;
            uint32 now = GameTime::GetGameTimeMS();
            {
                std::lock_guard lock(repopLock);
                if (repopAt[bot->GetGUID()] > now)
                    return true;
                repopAt[bot->GetGUID()] = now + 10000;
            }

            sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [](Player* player)
            {
                if (player->IsAlive() || player->HasPlayerFlag(PLAYER_FLAGS_GHOST) || !player->InBattleground())
                    return;
                player->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);
                player->BuildPlayerRepop();
                player->RepopAtGraveyard();
            });
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

        RotationRunner& rotation = ai.GetRotation();
        bool useRotation = sPlayerbotsConfig.RotationEnable && rotation.HasList(ai, "combat");
        // OOC combat-list entries (rogue Stealth) before Attack(), which flags combat and would make Stealth fail
        if (useRotation && !bot->IsInCombat())
            rotation.Run(ai, "combat", target, false);

        if (bot->GetVictim() != target)
            bot->Attack(target, !ranged);

        // server side facing check for casts, the chase movement turns the bot visibly
        if (!bot->isMoving())
            bot->SetOrientation(bot->GetAbsoluteAngle(target));

        if (role != BotRole::Healer || bot->GetPowerPct(POWER_MANA) > HealerDamageMinMana)
        {
            if (useRotation)
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

        ObjectGuid guid = corpse->GetGUID();

        // loot opcodes are world-thread only
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), guid, [guid](Player* player)
        {
            Creature* body = ObjectAccessor::GetCreature(*player, guid);
            if (!body)
                return;

            WorldSession* session = player->GetSession();
            WorldPackets::Loot::LootUnit lootUnit{ WorldPacket(CMSG_LOOT_UNIT) };
            lootUnit.Unit = guid;
            session->HandleLootOpcode(lootUnit);

            if (Loot* loot = body->GetLootForPlayer(player))
            {
                if (loot->gold)
                {
                    WorldPackets::Loot::LootMoney lootMoney{ WorldPacket(CMSG_LOOT_MONEY) };
                    session->HandleLootMoneyOpcode(lootMoney);
                }

                for (uint32 slot = 0; slot < loot->items.size() && slot < 255; ++slot)
                    if (!loot->items[slot].is_looted)
                        player->StoreLootItem(guid, uint8(slot), loot);
            }

            WorldPackets::Loot::LootRelease release{ WorldPacket(CMSG_LOOT_RELEASE) };
            release.Unit = guid;
            session->HandleLootReleaseOpcode(release);
        });
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
        MoveToward(bot, dest, 3.0f);
        return true;
    }
};

constexpr uint32 WarsongMap = 2106;
constexpr uint32 ArathiMap = 2107;
constexpr uint32 AllianceFlag = 227741;
constexpr uint32 HordeFlag = 227740;
constexpr uint32 HordeFlagAura = 23333;
constexpr uint32 AllianceFlagAura = 23335;
constexpr uint32 ArathiNodes[] = { 227420, 227522, 227536, 227538, 227544 };

GameObject* FindGo(Player* bot, uint32 entry)
{
    FindGameObjectOptions options;
    options.GameObjectId = entry;
    std::vector<GameObject*> found;
    bot->GetGameObjectListWithOptionsInGrid(found, 400.0f, options);
    return found.empty() ? nullptr : found.front();
}

bool CarriesFlag(Player const* bot)
{
    return bot->HasAura(HordeFlagAura) || bot->HasAura(AllianceFlagAura);
}

Player* FriendlyCarrier(Player* bot)
{
    for (MapReference const& ref : bot->GetMap()->GetPlayers())
    {
        Player* other = ref.GetSource();
        if (!other || other == bot || !other->IsAlive() || other->GetTeamId() != bot->GetTeamId())
            continue;
        if (CarriesFlag(other))
            return other;
    }
    return nullptr;
}

void SayBg(Player* bot, std::string const& line)
{
    static std::mutex lock;
    static std::unordered_map<ObjectGuid, uint32> nextAt;
    uint32 now = GameTime::GetGameTimeMS();
    {
        std::lock_guard guard(lock);
        if (nextAt[bot->GetGUID()] > now)
            return;
        nextAt[bot->GetGUID()] = now + 20000;
    }

    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [line](Player* player)
    {
        player->Say(line, LANG_UNIVERSAL);
    });
}

bool NearEnemy(Player* bot, PlayerbotAI& ai, Position const& where)
{
    Player* best = nullptr;
    float bestDist = 40.0f * 40.0f;
    for (MapReference const& ref : bot->GetMap()->GetPlayers())
    {
        Player* other = ref.GetSource();
        if (!other || other == bot || !other->IsAlive() || !bot->IsValidAttackTarget(other))
            continue;
        float dist = other->GetExactDistSq(where);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = other;
        }
    }
    if (!best)
        return false;
    ai.SetForcedTarget(best->GetGUID());
    return true;
}

bool PlayWarsong(Player* bot, PlayerbotAI& ai)
{
    bool alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    GameObject* ours = FindGo(bot, alliance ? AllianceFlag : HordeFlag);
    GameObject* enemyFlag = FindGo(bot, alliance ? HordeFlag : AllianceFlag);
    if (!ours || !enemyFlag)
    {
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} cannot see a Warsong flag yet", bot->GetName());
        return false;
    }

    if (CarriesFlag(bot))
    {
        SayBg(bot, "flag out");
        NearEnemy(bot, ai, ours->GetPosition());
        MoveAlongMesh(bot, ours->GetPosition(), 5.0f);
        return true;
    }

    if (Player* carrier = FriendlyCarrier(bot))
    {
        uint32 closer = 0;
        for (MapReference const& ref : bot->GetMap()->GetPlayers())
        {
            Player* other = ref.GetSource();
            if (!other || other == bot || other == carrier || !other->IsAlive() || other->GetTeamId() != bot->GetTeamId() || CarriesFlag(other))
                continue;
            if (other->GetExactDist(carrier) < bot->GetExactDist(carrier))
                ++closer;
        }
        if (closer < 2)
        {
            NearEnemy(bot, ai, carrier->GetPosition());
            MoveAlongMesh(bot, carrier->GetPosition(), 8.0f);
            return true;
        }
    }

    if (enemyFlag->GetFlagState() == FlagState::Taken)
    {
        uint32 aura = alliance ? HordeFlagAura : AllianceFlagAura;
        Player* carrier = nullptr;
        for (MapReference const& ref : bot->GetMap()->GetPlayers())
        {
            Player* other = ref.GetSource();
            if (!other || !other->IsAlive() || !other->HasAura(aura))
                continue;
            carrier = other;
            break;
        }
        if (carrier)
        {
            NearEnemy(bot, ai, carrier->GetPosition());
            MoveAlongMesh(bot, carrier->GetPosition(), 5.0f);
            return true;
        }
    }

    if (enemyFlag->GetFlagState() == FlagState::InBase)
        SayBg(bot, "inc their flag");
    NearEnemy(bot, ai, enemyFlag->GetPosition());
    MoveAlongMesh(bot, enemyFlag->GetPosition(), 5.0f);
    return true;
}

bool PlayArathi(Player* bot, PlayerbotAI& ai)
{
    GameObject* nearest = nullptr;
    float nearestDist = 0.0f;
    uint32 held = 0;
    GameObject* ours = nullptr;
    for (uint32 entry : ArathiNodes)
    {
        GameObject* node = FindGo(bot, entry);
        if (!node)
            continue;
        // Capture points are not control zones, so ownership is "can we still assault this".
        bool enemy = node->CanInteractWithCapturePoint(bot);
        if (!enemy)
        {
            ++held;
            ours = node;
            continue;
        }
        float dist = bot->GetExactDist2d(node);
        if (!nearest || dist < nearestDist)
        {
            nearest = node;
            nearestDist = dist;
        }
    }

    if (held >= 3 && ours && (bot->GetGUID().GetCounter() % 8) == 0)
    {
        SayBg(bot, "defending");
        NearEnemy(bot, ai, ours->GetPosition());
        MoveAlongMesh(bot, ours->GetPosition(), 8.0f);
        return true;
    }

    if (!nearest)
        return false;

    NearEnemy(bot, ai, nearest->GetPosition());
    if (bot->GetExactDist(nearest) < 10.0f && nearest->CanInteractWithCapturePoint(bot))
    {
        SayBg(bot, "need help");
        nearest->AssaultCapturePoint(bot);
        return true;
    }

    MoveAlongMesh(bot, nearest->GetPosition(), 5.0f);
    return true;
}

bool PlayObjective(Player* bot, PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.BgObjectives)
        return false;

    Battleground* bg = bot->GetBattleground();
    if (!bg || (bot->GetMapId() != WarsongMap && bot->GetMapId() != ArathiMap))
        return false;

    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return true;

    if (ai.GetRole() == BotRole::Healer)
    {
        Player* follow = FriendlyCarrier(bot);
        float followDist = follow ? 0.0f : 80.0f * 80.0f;
        if (!follow)
            for (MapReference const& ref : bot->GetMap()->GetPlayers())
            {
                Player* other = ref.GetSource();
                if (!other || other == bot || !other->IsAlive() || other->GetTeamId() != bot->GetTeamId())
                    continue;
                float dist = bot->GetExactDistSq(other);
                if (dist < followDist)
                {
                    followDist = dist;
                    follow = other;
                }
            }
        if (follow)
        {
            MoveAlongMesh(bot, follow->GetPosition(), 10.0f);
            return true;
        }
    }

    if (bot->GetMapId() == WarsongMap)
        return PlayWarsong(bot, ai);
    return PlayArathi(bot, ai);
}

class PvpAction : public Action
{
public:
    char const* GetName() const override { return "pvp"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (PlayObjective(bot, ai))
            return true;

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

constexpr float HostileRange = 40.0f;
constexpr float RareRange = 60.0f;
constexpr float GuardRange = 80.0f;
constexpr int32 LevelWindow = 5;

struct ReactState
{
    uint32 NextScan = 0;
    ObjectGuid Guard;
    ObjectGuid Rare;
};

std::mutex sReactLock;
std::unordered_map<ObjectGuid, ReactState> sReact;

ReactState LoadReact(ObjectGuid guid)
{
    std::lock_guard guard(sReactLock);
    auto itr = sReact.find(guid);
    return itr == sReact.end() ? ReactState{} : itr->second;
}

void SaveReact(ObjectGuid guid, ReactState const& state)
{
    std::lock_guard guard(sReactLock);
    if (sReact.size() > 4096)
        sReact.clear();
    sReact[guid] = state;
}

bool InCapital(Player const* bot)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    return zone && zone->GetFlags().HasFlag(AreaFlags::LinkedChat);
}

Player* NearestHostilePlayer(Player* bot)
{
    struct Check
    {
        Player* Bot = nullptr;
        bool operator()(Player* other) const
        {
            return other && other != Bot && other->IsAlive() && !Bot->IsInSameGroupWith(other) && Bot->IsValidAttackTarget(other);
        }
    };

    std::vector<Player*> found;
    Check check{ bot };
    Trinity::PlayerListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, HostileRange);

    Player* best = nullptr;
    for (Player* other : found)
        if (!best || bot->GetExactDistSq(other) < bot->GetExactDistSq(best))
            best = other;
    return best;
}

Creature* NearestGuard(Player* bot)
{
    struct Check
    {
        Player* Bot = nullptr;
        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && creature->IsGuard() && creature->IsFriendlyTo(Bot);
        }
    };

    std::vector<Creature*> found;
    Check check{ bot };
    Trinity::CreatureListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, GuardRange);

    Creature* best = nullptr;
    for (Creature* creature : found)
        if (!best || bot->GetExactDistSq(creature) < bot->GetExactDistSq(best))
            best = creature;
    return best;
}

Creature* NearestRare(Player* bot)
{
    struct Check
    {
        bool operator()(Creature* creature) const
        {
            if (!creature->IsAlive() || creature->IsCritter() || creature->IsTrigger() || creature->IsPet())
                return false;
            CreatureClassifications rank = creature->GetCreatureTemplate()->Classification;
            return rank == CreatureClassifications::Rare || rank == CreatureClassifications::RareElite;
        }
    };

    std::vector<Creature*> found;
    Check check;
    Trinity::CreatureListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, RareRange);

    Creature* best = nullptr;
    for (Creature* creature : found)
        if (!best || bot->GetExactDistSq(creature) < bot->GetExactDistSq(best))
            best = creature;
    return best;
}

bool WorldReactTrigger(PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.WorldReactions)
        return false;

    Player* bot = ai.GetBot();
    if (ai.GetMaster() || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->InBattleground())
        return false;

    ReactState state = LoadReact(bot->GetGUID());
    if (!state.Guard.IsEmpty() || !state.Rare.IsEmpty())
        return true;
    if (bot->duel && bot->duel->State == DUEL_STATE_CHALLENGED && bot->duel->Initiator != bot)
        return true;
    return GameTime::GetGameTimeMS() >= state.NextScan;
}

class WorldReactAction : public Action
{
public:
    char const* GetName() const override { return "world react"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        ReactState state = LoadReact(bot->GetGUID());
        uint32 now = GameTime::GetGameTimeMS();

        if (bot->duel && bot->duel->State == DUEL_STATE_CHALLENGED && bot->duel->Initiator != bot)
        {
            Player* challenger = bot->duel->Initiator;
            if (!InCapital(bot))
            {
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} declined a duel from {}, not in a city", bot->GetName(), challenger ? challenger->GetName() : "someone");
                state.NextScan = now + sPlayerbotsConfig.ScanInterval;
                SaveReact(bot->GetGUID(), state);
                return false;
            }

            ObjectGuid arbiter(*bot->m_playerData->DuelArbiter);
            TC_LOG_DEBUG("server.playerbots", "Playerbots: {} accepted a duel from {} in a city", bot->GetName(), challenger ? challenger->GetName() : "someone");
            sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [arbiter](Player* player)
            {
                if (!player->duel || player->duel->State != DUEL_STATE_CHALLENGED || player->duel->Initiator == player)
                    return;
                player->GetSession()->HandleDuelAccepted(arbiter);
            });
            state.NextScan = now + 5000;
            SaveReact(bot->GetGUID(), state);
            return true;
        }

        if (Player* hostile = NearestHostilePlayer(bot))
        {
            int32 gap = std::abs(int32(bot->GetLevel()) - int32(hostile->GetLevel()));
            if (gap <= LevelWindow)
            {
                state.Guard.Clear();
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} fights {}, levels are within {}", bot->GetName(), hostile->GetName(), LevelWindow);
                ai.SetForcedTarget(hostile->GetGUID());
                ai.Chase(hostile, ai.GetRole() == BotRole::RangedDps || ai.GetRole() == BotRole::Healer);
                state.NextScan = now + sPlayerbotsConfig.ScanInterval;
                SaveReact(bot->GetGUID(), state);
                return true;
            }

            if (Creature* guard = NearestGuard(bot))
            {
                state.Guard = guard->GetGUID();
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} runs from {} to a guard", bot->GetName(), hostile->GetName());
                if (MoveAlongMesh(bot, guard->GetPosition(), 6.0f) == MoveResult::NoPath)
                    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} has no path to the guard and does not run straight", bot->GetName());
                SaveReact(bot->GetGUID(), state);
                return true;
            }

            state.Guard.Clear();
            TC_LOG_DEBUG("server.playerbots", "Playerbots: {} will not fight {}, no guard within {} yards", bot->GetName(), hostile->GetName(), uint32(GuardRange));
        }
        else
            state.Guard.Clear();

        if (Creature* rare = NearestRare(bot))
        {
            state.Rare = rare->GetGUID();
            int32 gap = std::abs(int32(bot->GetLevel()) - int32(rare->GetLevelForTarget(bot)));
            TC_LOG_DEBUG("server.playerbots", "Playerbots: {} is looking at rare {}", bot->GetName(), rare->GetName());
            if (gap <= LevelWindow && bot->GetExactDist(rare) < 25.0f)
                ai.SetForcedTarget(rare->GetGUID());
            if (MoveAlongMesh(bot, rare->GetPosition(), 12.0f) == MoveResult::NoPath)
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} has no path to rare {} and does not run straight", bot->GetName(), rare->GetName());
            state.NextScan = now + sPlayerbotsConfig.ScanInterval;
            SaveReact(bot->GetGUID(), state);
            return true;
        }

        state.Rare.Clear();
        state.NextScan = now + sPlayerbotsConfig.ScanInterval;
        SaveReact(bot->GetGUID(), state);
        return false;
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
    r.AddTrigger("world react", 0, WorldReactTrigger);
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
    r.AddAction<WorldReactAction>("world react");
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
    AddTriggerStrategy(r, "world", { { "world react", { { "world react", Priority::World } } } });
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
