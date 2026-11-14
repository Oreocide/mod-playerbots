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

#include "BotLoot.h"
#include "AiRegistry.h"
#include "CellImpl.h"
#include "Cmd/ChatCommands.h"
#include "Cmd/ChatParser.h"
#include "Cmd/CommandText.h"
#include "Config.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Loot.h"
#include "LootPackets.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Travel/MoveToward.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_set>

namespace Playerbots::BotLooting
{
namespace
{
constexpr float SearchRange = 25.0f;        // the core loots corpses within 30 yards (AELootCreatureCheck::LootDistance)
constexpr float CorpseReach = 2.5f;         // where a player stands to loot
constexpr float ChestReach = 3.0f;
constexpr uint32 JobTimeoutMs = 25000;      // a walk, a cast and a hold together
constexpr uint32 OpenWaitMs = 3000;         // the world queue opens the loot after the map tick
constexpr uint32 TakeWaitMs = 2000;
constexpr size_t MaxRemembered = 128;

Config sConfig;

// Spells that open a lock of a type without any skill ("Opening" and its kin), the lowest id per type. Built at startup
std::array<uint32, 64> sGenericOpenSpell{};

bool IsFreeOpenType(uint32 lockType)
{
    switch (lockType)
    {
        case LOCKTYPE_OPEN:
        case LOCKTYPE_TREASURE:
        case LOCKTYPE_QUICK_OPEN:
        case LOCKTYPE_OPEN_TINKERING:
        case LOCKTYPE_OPEN_KNEELING:
        case LOCKTYPE_OPEN_ATTACKING:
            return true;
        default:
            return false;
    }
}

bool OpensLockType(SpellInfo const* info, uint32 lockType)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.Effect == SPELL_EFFECT_OPEN_LOCK && uint32(effect.MiscValue) == lockType)
            return true;
    return false;
}

void BuildOpenSpells()
{
    sGenericOpenSpell.fill(0);
    sSpellMgr->ForEachSpellInfo([](SpellInfo const* info)
    {
        if (info->Difficulty != DIFFICULTY_NONE || info->IsPassive() || info->RequiresSpellFocus)
            return;
        for (int32 reagent : info->Reagent)
            if (reagent > 0)
                return;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_OPEN_LOCK)
                continue;
            uint32 type = uint32(effect.MiscValue);
            if (type < sGenericOpenSpell.size() && IsFreeOpenType(type) && (!sGenericOpenSpell[type] || info->Id < sGenericOpenSpell[type]))
                sGenericOpenSpell[type] = info->Id;
        }
    });
}

template <typename Pred>
uint32 FindKnownSpell(Player* bot, Pred pred)
{
    uint32 fallback = 0;
    for (auto const& [id, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled || !known.active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE);
        if (!info)
            continue;
        int match = pred(info);
        if (match == 2)
            return id;
        if (match == 1 && !fallback)
            fallback = id;
    }
    return fallback;
}

/// A known skinning-effect spell for the skill the corpse needs (skinning, or herbalism/mining for some creatures)
uint32 KnownSkinSpell(Player* bot, uint32 skill)
{
    return FindKnownSpell(bot, [skill](SpellInfo const* info)
    {
        if (!info->HasEffect(SPELL_EFFECT_SKINNING))
            return 0;
        return IsPartOfSkillLine(skill, info->Id) ? 2 : 1;
    });
}

enum class OpenWay : uint8
{
    None,
    Use,    // no lock, or the bot carries the key
    Cast    // an opening spell with a cast bar ("Opening", a rogue's Pick Lock)
};

struct ChestOpen
{
    OpenWay Way = OpenWay::None;
    uint32 Spell = 0;
};

ChestOpen HowToOpen(Player* bot, GameObject const* go)
{
    uint32 lockId = go->GetGOInfo()->GetLockId();
    if (!lockId)
        return { OpenWay::Use, 0 };

    LockEntry const* lock = sLockStore.LookupEntry(lockId);
    if (!lock)
        return { OpenWay::Use, 0 };

    for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
    {
        switch (lock->Type[i])
        {
            case LOCK_KEY_ITEM:
                if (lock->Index[i] > 0 && bot->HasItemCount(uint32(lock->Index[i])))
                    return { OpenWay::Use, 0 };
                break;
            case LOCK_KEY_SKILL:
            {
                uint32 type = uint32(lock->Index[i]);
                if (type == LOCKTYPE_LOCKPICKING)
                {
                    if (uint32 spell = FindKnownSpell(bot, [type](SpellInfo const* info) { return OpensLockType(info, type) ? 2 : 0; }))
                        return { OpenWay::Cast, spell };
                    break;
                }
                if (IsFreeOpenType(type))
                {
                    if (type < sGenericOpenSpell.size() && sGenericOpenSpell[type])
                        return { OpenWay::Cast, sGenericOpenSpell[type] };
                    return { OpenWay::Use, 0 };
                }
                break;
            }
            default:
                break;
        }
    }
    return {};
}

// ---- loot filter ("ll") ----

enum class FilterMode : uint8
{
    All,        // everything, like a player who loots all (AC "gray")
    Normal,     // no grey junk (AC "normal")
    Quest,      // quest items and money
    Skill       // quest items, trade goods and reagents, money
};

struct LootPrefs
{
    FilterMode Mode = FilterMode::All;
    std::vector<uint32> Always;
    std::vector<uint32> Never;
};

char const* ModeName(FilterMode mode)
{
    switch (mode)
    {
        case FilterMode::Normal: return "normal";
        case FilterMode::Quest: return "quest";
        case FilterMode::Skill: return "skill";
        default: return "all";
    }
}

bool WantsItem(LootPrefs const& prefs, LootItem const& item)
{
    if (item.type != LootItemType::Item)
        return true;    // currencies and tracking quests
    if (std::ranges::find(prefs.Never, item.itemid) != prefs.Never.end())
        return false;
    if (std::ranges::find(prefs.Always, item.itemid) != prefs.Always.end())
        return true;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
    if (!proto)
        return false;

    bool quest = item.needs_quest || proto->GetStartQuest() || proto->GetClass() == ITEM_CLASS_QUEST;
    switch (prefs.Mode)
    {
        case FilterMode::Normal:
            return quest || proto->GetQuality() != ITEM_QUALITY_POOR;
        case FilterMode::Quest:
            return quest;
        case FilterMode::Skill:
            return quest || proto->GetClass() == ITEM_CLASS_TRADE_GOODS || proto->GetClass() == ITEM_CLASS_REAGENT;
        default:
            return true;
    }
}

LootPrefs GetPrefs(PlayerbotAI& ai)
{
    return ai.GetContext().GetValue<LootPrefs>("loot prefs", ai);
}

ManualValue<LootPrefs>* PrefsValue(PlayerbotAI& ai)
{
    return dynamic_cast<ManualValue<LootPrefs>*>(ai.GetContext().FindValue<LootPrefs>("loot prefs"));
}

// ---- world thread steps (loot opcodes are THREADUNSAFE) ----

/// Takes the money of every open loot and the items the filter allows, then closes every loot the bot has open. The open
/// of one corpse opens all corpses around it (area loot), so closing only the one the bot walked to would leave the rest
/// open and the looting flag on
void TakeAndRelease(Player* player, LootPrefs const& prefs)
{
    WorldSession* session = player->GetSession();

    bool money = false;
    for (auto const& [lootGuid, loot] : player->GetAELootView())
        if (loot && loot->gold)
            money = true;
    if (money)
    {
        WorldPackets::Loot::LootMoney lootMoney{ WorldPacket(CMSG_LOOT_MONEY) };
        session->HandleLootMoneyOpcode(lootMoney);
    }

    std::vector<std::pair<ObjectGuid, Loot*>> open(player->GetAELootView().begin(), player->GetAELootView().end());
    for (auto const& [lootGuid, loot] : open)
    {
        if (!loot)
            continue;
        ObjectGuid owner = loot->GetOwnerGUID();
        for (uint32 slot = 0; slot < loot->items.size() && slot < 255; ++slot)
        {
            LootItem const& item = loot->items[slot];
            if (item.is_looted || item.is_blocked || !WantsItem(prefs, item))
                continue;
            player->StoreLootItem(owner, uint8(slot), loot);
        }
    }

    // release by owner: the handler looks the loot up again, a loot freed by an earlier release is never touched
    std::vector<ObjectGuid> owners;
    for (auto const& [lootGuid, loot] : open)
        if (loot)
            owners.push_back(loot->GetOwnerGUID());
    for (ObjectGuid owner : owners)
    {
        WorldPackets::Loot::LootRelease release{ WorldPacket(CMSG_LOOT_RELEASE) };
        release.Unit = owner;
        session->HandleLootReleaseOpcode(release);
    }
}

void OpenCorpse(Player* player, ObjectGuid corpse)
{
    if (!ObjectAccessor::GetCreature(*player, corpse))
        return;
    WorldPackets::Loot::LootUnit lootUnit{ WorldPacket(CMSG_LOOT_UNIT) };
    lootUnit.Unit = corpse;
    player->GetSession()->HandleLootOpcode(lootUnit);
}

// ---- target search ----

class LootAction;
LootAction* GetLootAction(PlayerbotAI& ai);

enum class Kind : uint8
{
    Corpse,
    Skin,
    Chest
};

bool NearMaster(PlayerbotAI& ai, WorldObject const* target)
{
    Player* master = ai.GetMaster();
    return !master || master->GetMapId() != target->GetMapId() || master->GetExactDist(target) <= sConfig.MasterLeash;
}

class LootAction final : public Action
{
public:
    char const* GetName() const override { return "loot"; }

    bool Busy() const { return _phase != Phase::None; }
    /// A fight began while the window was open: take what is there and close it now
    void Abort(PlayerbotAI& ai) { Finish(ai); }
    bool Skinned(ObjectGuid guid) const { return _skinned.contains(guid); }
    bool ChestDone(ObjectGuid guid) const { return _chests.contains(guid); }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        uint32 now = GameTime::GetGameTimeMS();

        if (_phase != Phase::None && now - _started > JobTimeoutMs)
            Finish(ai);

        if (_phase == Phase::None && !Begin(ai, now))
            return false;

        WorldObject* target = Resolve(bot);
        if (!target)
        {
            Finish(ai);
            return false;
        }

        switch (_phase)
        {
            case Phase::Approach:
                return Approach(ai, target, now);
            case Phase::Opening:
            case Phase::Casting:
                if (!bot->GetAELootView().empty())
                {
                    _phase = Phase::Hold;
                    _until = now + RollHold();
                    return true;
                }
                if (_phase == Phase::Casting && bot->HasUnitState(UNIT_STATE_CASTING))
                    return true;
                if (now > _until)
                    Finish(ai);
                return true;
            case Phase::Hold:
                if (now < _until)
                    return true;
                Take(ai);
                _phase = Phase::Taking;
                _until = now + TakeWaitMs;
                return true;
            case Phase::Taking:
                if (bot->GetAELootView().empty() || now > _until)
                    Finish(ai);
                return true;
            default:
                Finish(ai);
                return false;
        }
    }

private:
    enum class Phase : uint8
    {
        None,
        Approach,
        Opening,    // the world queue opens a corpse or uses a chest
        Casting,    // skinning, or an opening spell on a chest
        Hold,       // the window is open
        Taking      // the world queue takes and closes
    };

    bool Begin(PlayerbotAI& ai, uint32 now)
    {
        ObjectGuid guid = ai.GetContext().GetValue<ObjectGuid>("nearest corpse", ai);
        if (guid.IsEmpty())
            return false;

        Player* bot = ai.GetBot();
        _target = guid;
        _started = now;
        _phase = Phase::Approach;
        if (guid.IsGameObject())
        {
            _kind = Kind::Chest;
            Remember(_chests, guid);
        }
        else
        {
            Creature* creature = ObjectAccessor::GetCreature(*bot, guid);
            Loot const* loot = creature ? creature->GetLootForPlayer(bot) : nullptr;
            if (loot && !loot->isLooted())
            {
                _kind = Kind::Corpse;
                std::unordered_set<ObjectGuid>& looted = ai.GetLootedCorpses();
                if (looted.size() > MaxRemembered)
                    looted.clear();
                looted.insert(guid);    // whatever happens below, never retried (full bags)
            }
            else
            {
                _kind = Kind::Skin;
                Remember(_skinned, guid);
            }
        }
        return true;
    }

    WorldObject* Resolve(Player* bot) const
    {
        if (_target.IsGameObject())
            return ObjectAccessor::GetGameObject(*bot, _target);
        return ObjectAccessor::GetCreature(*bot, _target);
    }

    bool Approach(PlayerbotAI& ai, WorldObject* target, uint32 now)
    {
        Player* bot = ai.GetBot();

        // Playerbots.Loot.Visible = 0: the old way for corpses, from where the bot stands and all at once
        if (!sConfig.Visible && _kind == Kind::Corpse)
        {
            ObjectGuid corpse = _target;
            LootPrefs prefs = GetPrefs(ai);
            sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), corpse, [corpse, prefs](Player* player)
            {
                OpenCorpse(player, corpse);
                TakeAndRelease(player, prefs);
            });
            Finish(ai);
            return true;
        }

        float reach = _kind == Kind::Chest ? ChestReach : CorpseReach;
        if (bot->GetExactDist(target) > reach + 1.0f)
        {
            if (MoveAlongMesh(bot, target->GetPosition(), reach, 1.0f) == MoveResult::NoPath)
                Finish(ai);
            return true;
        }

        if (bot->isMoving())
            bot->StopMoving();
        bot->SetFacingToObject(target);

        switch (_kind)
        {
            case Kind::Corpse:
            {
                ObjectGuid corpse = _target;
                sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), corpse, [corpse](Player* player) { OpenCorpse(player, corpse); });
                _phase = Phase::Opening;
                _until = now + OpenWaitMs;
                return true;
            }
            case Kind::Skin:
            {
                Creature* creature = target->ToCreature();
                uint32 skill = creature ? uint32(creature->GetCreatureDifficulty()->GetRequiredLootSkill()) : 0;
                uint32 spell = skill ? KnownSkinSpell(bot, skill) : 0;
                if (!spell || bot->CastSpell(creature, spell) != SPELL_CAST_OK)
                {
                    Finish(ai);
                    return false;
                }
                _phase = Phase::Casting;
                _until = now + 8000;
                return true;
            }
            case Kind::Chest:
            {
                GameObject* go = target->ToGameObject();
                ChestOpen how = go ? HowToOpen(bot, go) : ChestOpen{};
                if (how.Way == OpenWay::Cast)
                {
                    if (bot->CastSpell(go, how.Spell) != SPELL_CAST_OK)
                    {
                        Finish(ai);
                        return false;
                    }
                    _phase = Phase::Casting;
                    _until = now + 8000;
                    return true;
                }
                if (how.Way == OpenWay::Use)
                {
                    ObjectGuid chest = _target;
                    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), chest, [chest](Player* player)
                    {
                        if (GameObject* object = ObjectAccessor::GetGameObject(*player, chest))
                            if (object->getLootState() == GO_READY && object->isSpawned())
                                object->Use(player);
                    });
                    _phase = Phase::Opening;
                    _until = now + OpenWaitMs;
                    return true;
                }
                Finish(ai);
                return false;
            }
        }
        return false;
    }

    void Take(PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        LootPrefs prefs = GetPrefs(ai);
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), _target, [prefs](Player* player) { TakeAndRelease(player, prefs); });
    }

    // retail holds the window a median 1.0 s (p10 0, p90 2.0 s, 651 loots in the 11-2026 captures)
    static uint32 RollHold()
    {
        if (sConfig.InstantPct && urand(1, 100) <= sConfig.InstantPct)
            return urand(0, 150);
        return urand(sConfig.HoldMinMs, std::max(sConfig.HoldMinMs, sConfig.HoldMaxMs));
    }

    void Finish(PlayerbotAI& ai)
    {
        // a window still open (the corpse despawned during the hold, the job timed out) is closed, or the looting flag stays on
        Player* bot = ai.GetBot();
        if (_phase != Phase::Taking && _phase != Phase::None && !bot->GetAELootView().empty())
        {
            LootPrefs prefs = GetPrefs(ai);
            sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), _target, [prefs](Player* player) { TakeAndRelease(player, prefs); });
        }
        _phase = Phase::None;
        _target.Clear();
    }

    static void Remember(std::unordered_set<ObjectGuid>& set, ObjectGuid guid)
    {
        if (set.size() > MaxRemembered)
            set.clear();
        set.insert(guid);
    }

    Phase _phase = Phase::None;
    Kind _kind = Kind::Corpse;
    ObjectGuid _target;
    uint32 _started = 0;
    uint32 _until = 0;
    std::unordered_set<ObjectGuid> _skinned;
    std::unordered_set<ObjectGuid> _chests;
};

LootAction* GetLootAction(PlayerbotAI& ai)
{
    return dynamic_cast<LootAction*>(ai.GetContext().GetAction("loot"));
}

class CreatureLootCheck
{
public:
    CreatureLootCheck(PlayerbotAI& ai, LootAction const* action) : _ai(ai), _bot(ai.GetBot()), _action(action) { }

    bool operator()(Creature const* creature) const
    {
        if (creature->IsAlive() || !_bot->IsWithinDist(creature, SearchRange) || !NearMaster(_ai, creature))
            return false;

        Loot const* loot = creature->GetLootForPlayer(_bot);
        if (loot && !loot->isLooted())
            return !_ai.GetLootedCorpses().contains(creature->GetGUID()) && _bot->isAllowedToLoot(creature);

        // skinning: the corpse is empty (or never had loot for this bot) and nobody skinned it yet
        if (!sConfig.Skin || (_action && _action->Skinned(creature->GetGUID())))
            return false;
        if (!creature->HasUnitFlag(UNIT_FLAG_SKINNABLE) || creature->HasUnitFlag3(UNIT_FLAG3_ALREADY_SKINNED))
            return false;
        if (loot && loot->loot_type == LOOT_SKINNING)
            return false;
        uint32 skill = uint32(creature->GetCreatureDifficulty()->GetRequiredLootSkill());
        return skill && _bot->HasSkill(skill) && KnownSkinSpell(_bot, skill);
    }

private:
    PlayerbotAI& _ai;
    Player* _bot;
    LootAction const* _action;
};

class ChestCheck
{
public:
    ChestCheck(PlayerbotAI& ai, LootAction const* action) : _ai(ai), _bot(ai.GetBot()), _action(action) { }

    bool operator()(GameObject const* go) const
    {
        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST || !go->isSpawned() || go->getLootState() != GO_READY || go->GetGoState() != GO_STATE_READY)
            return false;
        if (!_bot->IsWithinDist(go, SearchRange) || !NearMaster(_ai, go) || (_action && _action->ChestDone(go->GetGUID())))
            return false;
        if (go->HasFlag(GO_FLAG_NOT_SELECTABLE) || (go->HasFlag(GO_FLAG_INTERACT_COND) && !go->ActivateToQuest(_bot)))
            return false;

        GameObjectTemplate const* info = go->GetGOInfo();
        if (info->chest.DungeonEncounter || (!info->GetLootId() && !info->chest.chestPersonalLoot))
            return false;
        if (Loot const* loot = go->GetLootForPlayer(_bot); loot && loot->isLooted())
            return false;
        return HowToOpen(_bot, go).Way != OpenWay::None && _bot->IsWithinLOSInMap(go);
    }

private:
    PlayerbotAI& _ai;
    Player* _bot;
    LootAction const* _action;
};

/// Nearest corpse to loot or skin, or chest to open. An empty search is not repeated for Playerbots.ScanInterval
class LootTargetValue final : public CalculatedValue<ObjectGuid>
{
protected:
    ObjectGuid Calculate(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        uint32 now = GameTime::GetGameTimeMS();
        if (_nextScan && now < _nextScan)
            return ObjectGuid::Empty;

        LootAction const* action = GetLootAction(ai);
        WorldObject* best = nullptr;

        std::vector<Creature*> corpses;
        CreatureLootCheck creatureCheck(ai, action);
        Trinity::CreatureListSearcher creatureSearcher(bot, corpses, creatureCheck);
        Cell::VisitGridObjects(bot, creatureSearcher, SearchRange);
        for (Creature* corpse : corpses)
            if (!best || bot->GetExactDistSq(corpse) < bot->GetExactDistSq(best))
                best = corpse;

        // chests only in the open world: a dungeon's chests belong to the group's run
        if (sConfig.Chests && !bot->GetMap()->Instanceable())
        {
            std::vector<GameObject*> chests;
            ChestCheck chestCheck(ai, action);
            Trinity::GameObjectListSearcher chestSearcher(bot, chests, chestCheck);
            Cell::VisitGridObjects(bot, chestSearcher, SearchRange);
            for (GameObject* chest : chests)
                if (!best || bot->GetExactDistSq(chest) < bot->GetExactDistSq(best))
                    best = chest;
        }

        _nextScan = best ? 0 : now + sPlayerbotsConfig.ScanInterval;
        return best ? best->GetGUID() : ObjectGuid::Empty;
    }

private:
    uint32 _nextScan = 0;
};

bool LootTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->IsInFlight())
        return false;

    if (LootAction const* action = GetLootAction(ai); action && action->Busy())
        return true;

    if (bot->IsMounted() || bot->GetVehicle())
        return false;

    return !ai.GetContext().GetValue<ObjectGuid>("nearest corpse", ai).IsEmpty();
}

/// A fight began while a loot window was open (the "loot" node is out of combat only, so the hold would never end): take and
/// close at once, like a player who closes the window to fight
class ReleaseLootAction final : public Action
{
public:
    char const* GetName() const override { return "release loot"; }

    uint32 ReleasedAt() const { return _releasedAt; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        _releasedAt = GameTime::GetGameTimeMS();
        if (LootAction* action = GetLootAction(ai); action && action->Busy())
            action->Abort(ai);
        else
        {
            LootPrefs prefs = GetPrefs(ai);
            sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [prefs](Player* player) { TakeAndRelease(player, prefs); });
        }
        return false;   // the fight goes on in this same tick
    }

private:
    uint32 _releasedAt = 0;
};

bool LootWindowOpenTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsInCombat() || bot->GetAELootView().empty())
        return false;
    auto const* release = dynamic_cast<ReleaseLootAction const*>(ai.GetContext().GetAction("release loot"));
    return !release || GameTime::GetGameTimeMS() - release->ReleasedAt() > 2000;
}

constexpr float LootPriority = 50.0f;           // GenericStrategies Priority::Loot
constexpr float ReleasePriority = 61.0f;        // just above attacking (60)

class LootStrategy final : public Strategy
{
public:
    LootStrategy() : Strategy("loot") { }

    uint8 GetStates() const override { return StrategyStates::Combat | StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "corpse to loot", { { "loot", LootPriority } }, StrategyStates::NonCombat });
        triggers.push_back({ "loot window open", { { "release loot", ReleasePriority } }, StrategyStates::Combat });
    }
};

// ---- "ll" order ----

std::string DescribePrefs(LootPrefs const& prefs)
{
    std::string text = std::string("Loot: ") + ModeName(prefs.Mode);
    if (!prefs.Always.empty())
        text += ", always " + std::to_string(prefs.Always.size()) + " item(s)";
    if (!prefs.Never.empty())
        text += ", never " + std::to_string(prefs.Never.size()) + " item(s)";
    return text + ".";
}

bool LootListCommand(PlayerbotAI& ai, std::string const& param)
{
    ManualValue<LootPrefs>* value = PrefsValue(ai);
    if (!value)
        return false;

    LootPrefs prefs = value->Get(ai);
    std::string text(Trim(param));
    std::string lower = ToLower(text);

    if (lower.empty() || lower == "?")
    {
        ai.TellMaster(DescribePrefs(prefs));
        return true;
    }

    if (lower == "reset")
        prefs = LootPrefs{};
    else if (lower == "all" || lower == "gray" || lower == "grey")
        prefs.Mode = FilterMode::All;
    else if (lower == "normal" || lower == "disenchant")
        prefs.Mode = FilterMode::Normal;
    else if (lower == "quest")
        prefs.Mode = FilterMode::Quest;
    else if (lower == "skill")
        prefs.Mode = FilterMode::Skill;
    else if (lower[0] == '+' || lower[0] == '-')
    {
        bool always = lower[0] == '+';
        ItemSelection selection = ParseItemSelection(std::string_view(text).substr(1));
        if (selection.Items.empty())
        {
            ai.TellMaster("Link the items after + or -.");
            return true;
        }
        std::vector<uint32>& add = always ? prefs.Always : prefs.Never;
        std::vector<uint32>& drop = always ? prefs.Never : prefs.Always;
        for (uint32 item : selection.Items)
        {
            std::erase(drop, item);
            if (std::ranges::find(add, item) == add.end())
                add.push_back(item);
        }
    }
    else
    {
        ai.TellMaster("ll [all|normal|quest|skill|reset] or ll +<items> / ll -<items>.");
        return true;
    }

    value->Set(prefs);
    ai.TellMaster(DescribePrefs(prefs));
    return true;
}

class OrderAction final : public Action
{
public:
    using Fn = bool (*)(PlayerbotAI& ai, std::string const& param);
    OrderAction(std::string name, Fn fn) : _name(std::move(name)), _fn(fn) { }
    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI& ai) override { return _fn(ai, GetParam()); }

private:
    std::string _name;
    Fn _fn;
};

void AddOrder(AiRegistry& registry, char const* name, char const* help, OrderAction::Fn fn, bool persist)
{
    std::string action = std::string("cmd ") + name;
    registry.AddAction(action, [action, fn](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<OrderAction>(action, fn); });
    ChatCommandInfo info;
    info.Action = action;
    info.Help = help;
    info.Persist = persist;
    info.WhisperOnly = true;    // changes what the bot keeps: never from a party chat line ("use the door" rule)
    GetChatCommands().Add(name, info);
}
}

Config const& GetConfig()
{
    return sConfig;
}

void LoadConfig()
{
    Config cfg;
    cfg.Visible = sConfigMgr->GetBoolDefault("Playerbots.Loot.Visible", false);
    cfg.HoldMinMs = sConfigMgr->GetIntDefault("Playerbots.Loot.HoldMinMs", 500);
    cfg.HoldMaxMs = sConfigMgr->GetIntDefault("Playerbots.Loot.HoldMaxMs", 2000);
    cfg.InstantPct = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Loot.InstantPct", 10));
    cfg.Skin = sConfigMgr->GetBoolDefault("Playerbots.Loot.Skin", false);
    cfg.Chests = sConfigMgr->GetBoolDefault("Playerbots.Loot.Chests", false);
    cfg.MasterLeash = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Loot.MasterLeash", 30.0f));
    sConfig = cfg;
}

void OnStartup()
{
    BuildOpenSpells();
    uint32 found = uint32(std::ranges::count_if(sGenericOpenSpell, [](uint32 id) { return id != 0; }));
    TC_LOG_INFO("server.playerbots", "Playerbots: loot knows {} opening spells for unskilled locks", found);
}

void Register(AiRegistry& registry)
{
    registry.AddValue("nearest corpse", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<LootTargetValue>(); });
    registry.AddValue("loot prefs", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<ManualValue<LootPrefs>>(); });
    registry.AddTrigger("corpse to loot", 0, LootTrigger);
    registry.AddAction<LootAction>("loot");
    registry.AddTrigger("loot window open", 0, LootWindowOpenTrigger);
    registry.AddAction<ReleaseLootAction>("release loot");
    registry.AddStrategy<LootStrategy>("loot");     // replaces the generic "loot" strategy: adds the release in a fight

    AddOrder(registry, "ll", "ll [all|normal|quest|skill|reset], ll +<items> (always), ll -<items> (never): what I loot.", LootListCommand, false);
}
}
