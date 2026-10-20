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

#include "RotationRunner.h"
#include "DB2Stores.h"
#include "Group.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "RotationFacts.h"
#include "RotationMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <vector>
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr uint32 MaxFailedCasts = 4;     // spells tried and refused in one turn
constexpr uint32 AfterCastDelayMs = 750;      // DynamicTick: wait after an instant on-GCD cast (half a typical global cooldown)
constexpr float TargetRange = 40.0f;      // allies the bot may pick

// "magic", "curse", "poison", "disease", "enrage"; anything else = the four kinds a player can dispel
uint32 DispelKindMask(std::string_view kind)
{
    if (kind == "magic")
        return 1u << DISPEL_MAGIC;
    if (kind == "curse")
        return 1u << DISPEL_CURSE;
    if (kind == "poison")
        return 1u << DISPEL_POISON;
    if (kind == "disease")
        return 1u << DISPEL_DISEASE;
    if (kind == "enrage")
        return 1u << DISPEL_ENRAGE;
    return DISPEL_ALL_MASK;
}

/// An aura of a dispel kind on the unit: harmful ones (on an ally) or beneficial ones (on an enemy)
bool HasDispellableAura(Unit const* unit, uint32 mask, bool harmful)
{
    for (auto const& [id, application] : unit->GetAppliedAuras())
    {
        Aura const* aura = application->GetBase();
        if (!aura || !aura->GetSpellInfo()->Dispel || !(mask & (1u << aura->GetSpellInfo()->Dispel)))
            continue;
        if (application->IsPositive() != !harmful)
            continue;
        return true;
    }
    return false;
}

bool HasAuraNamed(Unit const* unit, std::string const& normalizedName)
{
    for (auto const& [id, application] : unit->GetAppliedAuras())
    {
        Aura const* aura = application->GetBase();
        if (aura && aura->GetSpellInfo()->SpellName && NormalizeSpellName(aura->GetSpellInfo()->SpellName->Str[LOCALE_enUS]) == normalizedName)
            return true;
    }
    return false;
}

/// Enemies the bot can pick among: its target, then whatever attacks it or a group member
std::vector<Unit*> CandidateEnemies(PlayerbotAI& ai, Unit* enemy)
{
    Player* bot = ai.GetBot();
    std::vector<Unit*> list;
    auto add = [&](Unit* unit)
    {
        if (unit && unit->IsAlive() && bot->IsWithinDistInMap(unit, TargetRange) && bot->IsValidAttackTarget(unit) && std::ranges::find(list, unit) == list.end())
            list.push_back(unit);
    };
    add(enemy);
    for (Unit* attacker : bot->getAttackers())
        add(attacker);
    if (Group* group = bot->GetGroup())
        for (GroupReference const& ref : group->GetMembers())
            if (Player* member = ref.GetSource(); member && member != bot && member->IsInMap(bot))
                for (Unit* attacker : member->getAttackers())
                    add(attacker);
    return list;
}
}

std::string NormalizeSpellName(std::string_view name)
{
    std::string result(name);
    for (char& c : result)
        c = c == '_' ? ' ' : char(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

void RotationRunner::InvalidateIfNeeded(Player* bot)
{
    uint32 spec = uint32(bot->GetPrimarySpecialization());
    if (spec != _spec || bot->GetLevel() != _level || bot->GetSpellMap().size() != _spellCount)
    {
        _spells.clear();
        _known.clear();
        _spec = spec;
        _level = bot->GetLevel();
        _spellCount = bot->GetSpellMap().size();
    }
}

std::vector<uint32> const& RotationRunner::Resolve(Player* bot, std::string const& name)
{
    InvalidateIfNeeded(bot);

    std::string key = NormalizeSpellName(name);
    auto [itr, inserted] = _spells.try_emplace(key);
    if (!inserted)
        return itr->second;

    bool numeric = !key.empty() && std::ranges::all_of(key, [](unsigned char c) { return std::isdigit(c); });
    if (numeric)
    {
        uint32 id = uint32(std::strtoul(key.c_str(), nullptr, 10));
        if (bot->HasSpell(id))
            itr->second.push_back(id);
        return itr->second;
    }

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || !playerSpell.active || playerSpell.disabled)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || info->IsPassive() || !info->SpellName || NormalizeSpellName(info->SpellName->Str[LOCALE_enUS]) != key)
            continue;

        itr->second.push_back(spellId);
    }

    return itr->second;
}

bool RotationRunner::Knows(Player* bot, std::string const& name)
{
    InvalidateIfNeeded(bot);

    std::string key = NormalizeSpellName(name);
    auto [itr, inserted] = _known.try_emplace(key, false);
    if (!inserted)
        return itr->second;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || !playerSpell.active || playerSpell.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (info && info->SpellName && NormalizeSpellName(info->SpellName->Str[LOCALE_enUS]) == key)
        {
            itr->second = true;
            break;
        }
    }
    return itr->second;
}

uint32 RotationRunner::EnemiesAround(PlayerbotAI& ai, Unit const* target, uint8 which)
{
    if (_countTick != ai.GetTick())
    {
        _countTick = ai.GetTick();
        std::ranges::fill(_counted, false);
    }

    if (_counted[which])
        return _counts[which];

    Player* bot = ai.GetBot();
    float radius = which == 2 ? 30.0f : 8.0f;
    Unit const* around = which == 1 ? target : bot;
    uint32 count = 0;
    if (around)
    {
        std::vector<Unit*> units;
        Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(around, bot, radius);
        Trinity::UnitListSearcher searcher(around, units, check);
        Cell::VisitAllObjects(around, searcher, radius);
        for (Unit* unit : units)
            if (unit->IsAlive() && unit->IsInCombat() && bot->IsValidAttackTarget(unit))
                ++count;
    }

    _counted[which] = true;
    _counts[which] = count;
    return count;
}

Unit* RotationRunner::ResolveTarget(PlayerbotAI& ai, std::string const& what, Unit* enemy) const
{
    Player* bot = ai.GetBot();
    if (what == "enemy" || what == "target")
        return enemy;
    if (what == "self")
        return bot;
    if (what == "pet")
        return bot->GetPet();
    if (what == "master")
        return ai.GetMaster();

    if (what == "lowest_ally" || what == "tank")
    {
        Unit* best = nullptr;
        auto consider = [&](Player* member)
        {
            if (!member || !member->IsAlive() || !member->IsInMap(bot) || !bot->IsWithinDistInMap(member, TargetRange))
                return;

            if (what == "tank")
            {
                ChrSpecializationEntry const* spec = member->GetPrimarySpecializationEntry();
                if (!spec || spec->GetRole() != ChrSpecializationRole::Tank)
                    return;
                if (!best)
                    best = member;
                return;
            }

            if (!best || member->GetHealthPct() < best->GetHealthPct())
                best = member;
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

    // an enemy casting something: the current target first, then anything that attacks the bot
    if (what == "casting_enemy")
    {
        if (enemy && enemy->IsNonMeleeSpellCast(false, false, true))
            return enemy;
        for (Unit* attacker : bot->getAttackers())
            if (attacker->IsAlive() && attacker->IsNonMeleeSpellCast(false, false, true) && bot->IsWithinDistInMap(attacker, TargetRange))
                return attacker;
        return nullptr;
    }

    // the group utility targets are off until Playerbots.Rotation.Utility
    if (!sPlayerbotsConfig.RotationUtility
        && (what == "dead_ally" || what == "lowest_mana_ally" || what == "cc_target" || what == "enemy_healer" || what.starts_with("ally_without_buff.")
            || what.starts_with("dispellable_ally") || what.starts_with("dispellable_enemy")))
        return nullptr;

    // group members: the bot first, then the others in range
    auto forEachAlly = [&](auto&& visit)
    {
        visit(bot);
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource(); member && member != bot && member->IsInMap(bot) && bot->IsWithinDistInMap(member, TargetRange))
                    visit(member);
        }
        else if (Player* master = ai.GetMaster(); master && master->IsInMap(bot) && bot->IsWithinDistInMap(master, TargetRange))
            visit(master);
    };

    // a dead group member to resurrect (the spell decides whether it may be cast in combat); unreleased bodies first
    if (what == "dead_ally")
    {
        Unit* best = nullptr;
        forEachAlly([&](Player* member)
        {
            if (member->IsAlive() || member->IsResurrectRequested())
                return;
            if (!best || (best->ToPlayer()->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !member->HasPlayerFlag(PLAYER_FLAGS_GHOST)))
                best = member;
        });
        return best;
    }

    // the ally with the least mana (innervate, power infusion)
    if (what == "lowest_mana_ally")
    {
        Unit* best = nullptr;
        forEachAlly([&](Player* member)
        {
            if (!member->IsAlive() || member->GetMaxPower(POWER_MANA) == 0)
                return;
            if (!best || member->GetPowerPct(POWER_MANA) < best->GetPowerPct(POWER_MANA))
                best = member;
        });
        return best;
    }

    // a living ally who lacks a buff (a group buff): "ally_without_buff.arcane_intellect"
    if (what.starts_with("ally_without_buff."))
    {
        std::string buff = NormalizeSpellName(what.substr(18));
        Unit* found = nullptr;
        forEachAlly([&](Player* member)
        {
            if (!found && member->IsAlive() && !HasAuraNamed(member, buff))
                found = member;
        });
        return found;
    }

    // an ally with a harmful aura of a dispel kind: "dispellable_ally", "dispellable_ally.magic|curse|poison|disease"
    if (what.starts_with("dispellable_ally"))
    {
        uint32 mask = DispelKindMask(what.size() > 17 ? std::string_view(what).substr(17) : std::string_view());
        Unit* found = nullptr;
        forEachAlly([&](Player* member)
        {
            if (!found && member->IsAlive() && HasDispellableAura(member, mask, true))
                found = member;
        });
        return found;
    }

    // an enemy with a beneficial aura of a dispel kind (purge, spellsteal, tranquilizing shot): "dispellable_enemy[.magic|enrage]"
    if (what.starts_with("dispellable_enemy"))
    {
        uint32 mask = DispelKindMask(what.size() > 18 ? std::string_view(what).substr(18) : std::string_view());
        if (mask == DISPEL_ALL_MASK)
            mask = 1u << DISPEL_MAGIC;
        for (Unit* candidate : CandidateEnemies(ai, enemy))
            if (HasDispellableAura(candidate, mask, false))
                return candidate;
        return nullptr;
    }

    // an enemy that is not the main target and not crowd controlled yet, when there are at least two (polymorph, shackle, hex)
    if (what == "cc_target")
    {
        for (Unit* candidate : CandidateEnemies(ai, enemy))
            if (candidate != enemy && !candidate->HasBreakableByDamageCrowdControlAura())
                return candidate;
        return nullptr;
    }

    // an enemy that is casting a heal (interrupt it first)
    if (what == "enemy_healer")
    {
        for (Unit* candidate : CandidateEnemies(ai, enemy))
            if (Spell const* spell = candidate->GetCurrentSpell(CURRENT_GENERIC_SPELL); spell && spell->GetSpellInfo()->HasEffect(SPELL_EFFECT_HEAL))
                return candidate;
        return nullptr;
    }

    // an enemy that attacks a group member instead of the tank (taunt it)
    if (what == "loose_enemy")
    {
        if (Group* group = bot->GetGroup())
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource(); member && member != bot && member->IsAlive())
                    for (Unit* attacker : member->getAttackers())
                        if (attacker->IsAlive() && attacker->GetVictim() != bot && bot->IsWithinDistInMap(attacker, 30.0f) && bot->IsValidAttackTarget(attacker))
                            return attacker;
        return nullptr;
    }

    return nullptr;
}

bool RotationRunner::HasList(PlayerbotAI& ai, std::string const& list) const
{
    std::shared_ptr<RotationTable const> table = sRotationMgr->GetTable();
    if (!table)
        return false;

    auto set = table->Sets.find(uint32(ai.GetBot()->GetPrimarySpecialization()));
    return set != table->Sets.end() && set->second->Lists.contains(list);
}

bool RotationRunner::Run(PlayerbotAI& ai, std::string const& listName, Unit* enemy, bool inCombat)
{
    Player* bot = ai.GetBot();
    std::shared_ptr<RotationTable const> table = sRotationMgr->GetTable();
    if (!table)
        return false;

    auto setItr = table->Sets.find(uint32(bot->GetPrimarySpecialization()));
    if (setItr == table->Sets.end())
        return false;

    auto listItr = setItr->second->Lists.find(listName);
    if (listItr == setItr->second->Lists.end())
        return false;

    // a cast is on its way: the turn is taken
    if (bot->IsNonMeleeSpellCast(false, false, true))
        return true;

    bool cast = false;
    uint32 failures = 0;
    for (RotationEntry const& entry : listItr->second.Entries)
    {
        // out of combat only the buffs list and entries marked ooc run, in combat everything but the buffs
        bool allowed = inCombat ? listName != "buffs" : (listName == "buffs" || entry.OutOfCombat);
        if (!allowed)
            continue;

        Unit* target = ResolveTarget(ai, entry.Target, enemy);
        if (!target || target->IsAlive() == (entry.Target == "dead_ally"))
            continue;

        for (uint32 spellId : Resolve(bot, entry.Spell))
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            if (!spellInfo || !bot->GetSpellHistory()->IsReady(spellInfo))
                continue;

            if (!entry.OffGcd && bot->GetSpellHistory()->HasGlobalCooldown(spellInfo))
                continue;

            if (entry.Condition)
            {
                BotFacts facts(ai, *const_cast<RotationRunner*>(this), target);
                if (!Evaluate(*entry.Condition, facts))
                    continue;
            }

            if (target != bot)
            {
                bool positive = spellInfo->IsPositive();
                if (!bot->IsWithinDistInMap(target, spellInfo->GetMaxRange(positive, bot)) || !bot->IsWithinLOSInMap(target))
                    continue;
            }

            // casts with a cast time are not started on the run
            if (bot->isMoving() && spellInfo->CalcCastTime() > 0 && !spellInfo->HasAttribute(SPELL_ATTR5_ALLOW_ACTIONS_DURING_CHANNEL))
                continue;

            if (target != bot && !bot->isMoving() && !bot->HasInArc(float(M_PI), target))
                bot->SetFacingToObject(target);

            if (bot->CastSpell(target, spellId) == SPELL_CAST_OK)
            {
                cast = true;
                if (!entry.OffGcd)
                {
                    // Playerbots.DynamicTick: nothing on the global cooldown can be cast before it ends, so the next
                    // decision waits for the cast, or about half a global cooldown (interrupts and defensives are off it)
                    int32 castTime = spellInfo->CalcCastTime();
                    ai.RequestDelay(castTime > 0 ? uint32(castTime) + 50 : AfterCastDelayMs);
                    return true;
                }
                break;
            }

            if (++failures >= MaxFailedCasts)
                return cast;
        }
    }

    return cast;
}
}
