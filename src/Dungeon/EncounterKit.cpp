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

#include "Dungeon/EncounterKit.h"
#include "Combat/Positioning.h"
#include "Creature.h"
#include "Engine/ContextStrategies.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Travel/BotMover.h"
#include "Engine/AiRegistry.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "Spell.h"
#include "SpellInfo.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace Playerbots::Dungeon
{
void RegisterUtgardeKeep(AiRegistry& registry);
void RegisterAzjolNerub(AiRegistry& registry);
void RegisterAhnKahet(AiRegistry& registry);
void RegisterUtgardePinnacle(AiRegistry& registry);
void RegisterNexus(AiRegistry& registry);

namespace
{
struct Rule
{
    std::string Strategy;
    TargetRule Fn = nullptr;
};

// Written once at startup (RegisterAll), read-only afterwards, so map threads read it without a lock
std::unordered_map<uint32, Rule>& Rules()
{
    static std::unordered_map<uint32, Rule> rules;
    return rules;
}
}

void RegisterTargetRule(uint32 mapId, char const* strategy, TargetRule rule)
{
    Rules()[mapId] = { strategy, rule };
}

Unit* EncounterTarget(PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.DungeonEnable)
        return nullptr;

    auto const& rules = Rules();
    auto itr = rules.find(ai.GetBot()->GetMapId());
    if (itr == rules.end() || !itr->second.Fn || !ai.GetEngine().HasStrategy(itr->second.Strategy))
        return nullptr;

    return itr->second.Fn(ai);
}

Creature* FindBoss(Player* bot, std::initializer_list<uint32> entries, float range)
{
    Creature* best = nullptr;
    float bestDist = range;
    for (uint32 entry : entries)
        if (Creature* creature = bot->FindNearestCreature(entry, range, true))
            if (float dist = bot->GetExactDist(creature); !best || dist < bestDist)
            {
                best = creature;
                bestDist = dist;
            }
    return best;
}

bool GroupMemberHasAura(Player* bot, uint32 spellId)
{
    if (bot->HasAura(spellId))
        return true;

    if (Group* group = bot->GetGroup())
        for (GroupReference const& ref : group->GetMembers())
            if (Player* member = ref.GetSource(); member && member->IsInMap(bot) && member->HasAura(spellId))
                return true;
    return false;
}

bool IsCasting(Unit const* unit, std::initializer_list<uint32> spellIds)
{
    if (!unit)
        return false;

    for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        if (Spell const* spell = unit->GetCurrentSpell(type))
            if (std::ranges::find(spellIds, spell->GetSpellInfo()->Id) != spellIds.end())
                return true;
    return false;
}

bool HasAnyAura(Unit const* unit, std::initializer_list<uint32> spellIds)
{
    if (!unit)
        return false;
    return std::ranges::any_of(spellIds, [unit](uint32 id) { return unit->HasAura(id); });
}

Unit* FirstAttacker(PlayerbotAI& ai, std::initializer_list<uint32> entries)
{
    Player* bot = ai.GetBot();
    std::vector<ObjectGuid> guids = ai.GetContext().GetValue<std::vector<ObjectGuid>>("attackers", ai);
    for (uint32 entry : entries)
        for (ObjectGuid const& guid : guids)
            if (guid.GetEntry() == entry)
                if (Unit* unit = ObjectAccessor::GetUnit(*bot, guid); unit && ai.IsValidEnemy(unit))
                    return unit;
    return nullptr;
}

bool IsInFrontOf(Unit const* boss, Unit const* who, float halfArc)
{
    return InFrontArc({ boss->GetPositionX(), boss->GetPositionY() }, boss->GetOrientation(), { who->GetPositionX(), who->GetPositionY() }, halfArc);
}

bool StepAway(PlayerbotAI& ai, float x, float y, float minDist, MovePriority priority)
{
    Player* bot = ai.GetBot();
    Pt me{ bot->GetPositionX(), bot->GetPositionY() };
    if (std::hypot(me.X - x, me.Y - y) >= minDist)
        return false;

    std::vector<Hazard> hazards = { { { x, y }, minDist } };
    std::vector<Pt> candidates = SafeSpotCandidates(me, hazards, { minDist + 2.0f, minDist + 5.0f }, 16, 0.5f);
    uint32 now = GameTime::GetGameTimeMS();
    int pick = PickSafeSpot(candidates, me, hazards, ai.GetFleeHistory(), now, {}, 2.0f);
    if (pick < 0)
        return false;

    Position snapped;
    if (!SnapToMesh(bot, Position(candidates[pick].X, candidates[pick].Y, bot->GetPositionZ()), snapped, 3.0f, 2.5f))
        return false;
    if (MoveCombat(bot, snapped, 1.0f, priority) == MoveResult::NoPath)
        return false;

    NoteFlee(ai.GetFleeHistory(), std::atan2(candidates[pick].Y - me.Y, candidates[pick].X - me.X), now);
    return true;
}

bool StepBehind(PlayerbotAI& ai, Unit const* boss, float behindYards, MovePriority priority)
{
    Player* bot = ai.GetBot();
    Pt where{ boss->GetPositionX(), boss->GetPositionY() };
    Position snapped;
    if (!SnapToMesh(bot, Position(BehindPoint(where, boss->GetOrientation(), behindYards).X, BehindPoint(where, boss->GetOrientation(), behindYards).Y, boss->GetPositionZ()),
        snapped, 3.0f, 2.5f))
        return false;
    return MoveCombat(bot, snapped, 1.0f, priority) != MoveResult::NoPath;
}

namespace
{
class FnAction final : public Action
{
public:
    FnAction(std::string name, ActionFn fn) : _name(std::move(name)), _fn(std::move(fn)) { }
    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI& ai) override { return _fn(ai); }

private:
    std::string _name;
    ActionFn _fn;
};

class FnMultiplier final : public Multiplier
{
public:
    explicit FnMultiplier(MultiplierFn fn) : _fn(std::move(fn)) { }
    float GetValue(PlayerbotAI& ai, Action* action) override { return _fn(ai, action); }

private:
    MultiplierFn _fn;
};

class DungeonStrategy final : public Strategy
{
public:
    explicit DungeonStrategy(DungeonDef const& def) : Strategy(def.Name), _def(def) { }
    uint8_t GetStates() const override { return StrategyStates::Combat; }
    void InitTriggers(std::vector<TriggerNode>& triggers) override { triggers = _def.Triggers; }
    void InitMultipliers(std::vector<std::unique_ptr<Multiplier>>& multipliers) override
    {
        for (MultiplierFn const& fn : _def.Multipliers)
            multipliers.push_back(std::make_unique<FnMultiplier>(fn));
    }

private:
    DungeonDef _def;
};
}

void AddAction(AiRegistry& registry, std::string name, ActionFn fn)
{
    std::string key = name;
    registry.AddAction(std::move(key), [name, fn](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<FnAction>(name, fn); });
}

void RegisterDungeon(AiRegistry& registry, DungeonDef def)
{
    registry.AddStrategy(def.Name, [def](std::string const&) -> std::unique_ptr<Strategy> { return std::make_unique<DungeonStrategy>(def); });

    if (def.Target)
        RegisterTargetRule(def.MapId, def.Name, def.Target);

    uint32 mapId = def.MapId;
    std::string name = def.Name;
    sContextStrategies.Register({ [mapId](BotContext const& context)
    {
        return sPlayerbotsConfig.DungeonEnable && context.Place == PlaceKind::Dungeon && context.MapId == mapId && context.InGroup;
    }, { name } });
}

void RegisterAll(AiRegistry& registry)
{
    RegisterUtgardeKeep(registry);
    RegisterAzjolNerub(registry);
    RegisterAhnKahet(registry);
    RegisterUtgardePinnacle(registry);
    RegisterNexus(registry);
}
}
