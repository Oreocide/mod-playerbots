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
#include "RotationFacts.h"
#include "RotationMgr.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr uint32 MaxFailedCasts = 4;     // spells tried and refused in one turn
constexpr float TargetRange = 40.0f;      // allies the bot may pick
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
        if (!target || !target->IsAlive())
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
                    return true;
                break;
            }

            if (++failures >= MaxFailedCasts)
                return cast;
        }
    }

    return cast;
}
}
