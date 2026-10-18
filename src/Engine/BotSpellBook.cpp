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

#include "BotSpellBook.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Util.h"
#include <algorithm>
#include <string>

namespace Playerbots
{
namespace
{
// Spells with a longer cooldown are saved for the player to decide on
constexpr uint32 MaxCooldown = 2 * MINUTE * IN_MILLISECONDS;
// Long casts make the bot look stuck
constexpr uint32 MaxCastTime = 4 * IN_MILLISECONDS;

bool IsDamageEffect(SpellEffectInfo const& effect)
{
    switch (effect.Effect)
    {
        case SPELL_EFFECT_SCHOOL_DAMAGE:
        case SPELL_EFFECT_HEALTH_LEECH:
        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
        case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
            return true;
        default:
            break;
    }

    return effect.IsAura(SPELL_AURA_PERIODIC_DAMAGE) || effect.IsAura(SPELL_AURA_PERIODIC_LEECH)
        || effect.IsAura(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
}

bool IsHealEffect(SpellEffectInfo const& effect)
{
    return effect.IsEffect(SPELL_EFFECT_HEAL) || effect.IsEffect(SPELL_EFFECT_HEAL_PCT) || effect.IsAura(SPELL_AURA_PERIODIC_HEAL);
}

bool IsCandidate(SpellInfo const* spellInfo)
{
    if (spellInfo->IsPassive() || spellInfo->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
        return false;

    if (std::max(spellInfo->RecoveryTime, spellInfo->CategoryRecoveryTime) > MaxCooldown)
        return false;

    if (spellInfo->CalcCastTime() > MaxCastTime)
        return false;

    // reagents are not managed by the bot
    return std::ranges::none_of(spellInfo->Reagent, [](int32 reagent) { return reagent > 0; });
}

bool IsJunkName(char const* name)
{
    if (!name || !*name)
        return false;

    std::string lower(name);
    strToLower(lower);
    for (std::string_view marker : { "dummy", "test", "dnt", "obsolete", "unused", "rks ", "[ph]", "(ph)", "placeholder" })
        if (lower.find(marker) != std::string::npos)
            return true;

    return false;
}

// Cooldown-gated spells first (they are only rarely available), then faster casts
bool SortOrder(SpellInfo const* left, SpellInfo const* right)
{
    uint32 leftCooldown = std::max(left->RecoveryTime, left->CategoryRecoveryTime);
    uint32 rightCooldown = std::max(right->RecoveryTime, right->CategoryRecoveryTime);
    if (leftCooldown != rightCooldown)
        return leftCooldown > rightCooldown;

    return left->CalcCastTime() < right->CalcCastTime();
}
}

bool BotSpellBook::IsJunk(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return true;

    if (IsJunkName(spellInfo->SpellName ? spellInfo->SpellName->Str[LOCALE_enUS] : nullptr))
        return true;

    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        if (effect.TriggerSpell && !sSpellMgr->GetSpellInfo(effect.TriggerSpell, DIFFICULTY_NONE))
            return true;

    return false;
}

void BotSpellBook::StripStartingAuras(Player* bot)
{
    if (!bot)
        return;

    // Character-create auras meant for the starting room. Random bots keep them after GiveLevel + teleport.
    // 80653 Gnome Irradiated (New Tinkertown), 73523 Undead Rigor Mortis (Deathknell), 155882 GiveLevel spam
    static constexpr uint32 StartingAuras[] = { 80653, 73523, 155882 };
    for (uint32 spellId : StartingAuras)
        if (bot->HasAura(spellId))
            bot->RemoveAurasDueToSpell(spellId);
}

void BotSpellBook::StripJunk(Player* bot)
{
    if (!bot)
        return;

    StripStartingAuras(bot);

    std::vector<uint32> junk;
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED)
            continue;

        if (IsJunk(sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE)))
            junk.push_back(spellId);
    }

    for (auto const& [id, application] : bot->GetAppliedAuras())
    {
        if (!application)
            continue;

        Aura* aura = application->GetBase();
        if (!aura)
            continue;

        if (IsJunk(aura->GetSpellInfo()))
            junk.push_back(aura->GetId());
    }

    std::ranges::sort(junk);
    junk.erase(std::ranges::unique(junk).begin(), junk.end());
    for (uint32 spellId : junk)
    {
        bot->RemoveAurasDueToSpell(spellId);
        bot->RemoveSpell(spellId, false, false, true);
    }
}

void BotSpellBook::Refresh(Player const* bot)
{
    uint32 specId = uint32(bot->GetPrimarySpecialization());
    if (specId == _specId && bot->GetLevel() == _level && bot->GetSpellMap().size() == _spellCount)
        return;

    _specId = specId;
    _level = bot->GetLevel();
    _spellCount = bot->GetSpellMap().size();
    _damage.clear();
    _heal.clear();

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || !playerSpell.active || playerSpell.disabled)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spellInfo || IsJunk(spellInfo) || !IsCandidate(spellInfo))
            continue;

        bool damage = false;
        bool heal = false;
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            damage = damage || IsDamageEffect(effect);
            heal = heal || IsHealEffect(effect);
        }

        if (damage && !spellInfo->IsPositive())
            _damage.push_back(spellInfo);
        else if (heal && spellInfo->IsPositive())
            _heal.push_back(spellInfo);
    }

    std::ranges::sort(_damage, SortOrder);
    std::ranges::sort(_heal, SortOrder);
}

bool BotSpellBook::CastFirstUsable(Player* bot, Unit* target, std::vector<SpellInfo const*> const& spells, uint32 maxAttempts)
{
    if (bot->IsNonMeleeSpellCast(false, false, true))
        return false;

    uint32 attempts = 0;
    for (SpellInfo const* spellInfo : spells)
    {
        if (!bot->GetSpellHistory()->IsReady(spellInfo))
            continue;

        if (spellInfo->IsAutoRepeatRangedSpell() && bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            continue;

        if (target != bot && !bot->IsWithinDistInMap(target, spellInfo->GetMaxRange(spellInfo->IsPositive(), bot)))
            continue;

        if (bot->CastSpell(target, spellInfo->Id) == SPELL_CAST_OK)
            return true;

        if (++attempts >= maxAttempts)
            break;
    }

    return false;
}
}
