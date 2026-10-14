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

#include "RotationFacts.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "RotationRunner.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>

namespace Playerbots
{
namespace
{
struct PowerName
{
    char const* Name;
    Powers Power;
};

constexpr PowerName PowerNames[] = { { "mana", POWER_MANA }, { "rage", POWER_RAGE }, { "focus", POWER_FOCUS }, { "energy", POWER_ENERGY },
    { "combo_points", POWER_COMBO_POINTS }, { "runes", POWER_RUNES }, { "runic_power", POWER_RUNIC_POWER }, { "soul_shards", POWER_SOUL_SHARDS },
    { "astral_power", POWER_LUNAR_POWER }, { "holy_power", POWER_HOLY_POWER }, { "maelstrom", POWER_MAELSTROM }, { "chi", POWER_CHI },
    { "insanity", POWER_INSANITY }, { "arcane_charges", POWER_ARCANE_CHARGES }, { "fury", POWER_FURY }, { "pain", POWER_PAIN },
    { "essence", POWER_ESSENCE } };

bool FindPower(std::string_view name, Powers& power)
{
    for (PowerName const& entry : PowerNames)
        if (name == entry.Name)
        {
            power = entry.Power;
            return true;
        }
    return false;
}

std::vector<std::string> Split(std::string const& text)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        size_t dot = text.find('.', start);
        parts.push_back(text.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return parts;
}

// "buff.battle_shout.remains" -> scope "buff", spell "battle_shout", tail "remains"; the spell may itself hold no dots
bool SplitAuraFact(std::vector<std::string> const& parts, std::string& scope, std::string& spell, std::string& tail)
{
    size_t index = 0;
    scope = parts[0];
    if (parts[0] == "target" && parts.size() >= 3 && parts[1] == "buff")
    {
        scope = "target.buff";
        index = 1;
    }
    else if (parts[0] != "buff" && parts[0] != "debuff")
        return false;

    if (parts.size() < index + 2)
        return false;

    spell = parts[index + 1];
    tail = parts.size() > index + 2 ? parts[index + 2] : std::string();
    return parts.size() <= index + 3 && (tail.empty() || tail == "remains" || tail == "stacks");
}
}

bool IsKnownRotationFact(std::string const& name)
{
    static constexpr std::string_view Flags[] = { "in_combat", "moving", "mounted", "solo", "pet", "pet.alive", "target.boss", "target.elite",
        "target.casting", "target.moving", "target.in_melee", "target.targeting_me", "target.is_player", "role.tank", "role.healer", "role.dps" };
    static constexpr std::string_view Numbers[] = { "self.hp", "self.power", "self.power.value", "target.hp", "target.distance", "target.level_diff",
        "enemies", "enemies.target", "enemies.ranged", "group.size", "group.lowest_hp", "group.injured" };

    if (std::ranges::find(Flags, name) != std::end(Flags) || std::ranges::find(Numbers, name) != std::end(Numbers))
        return true;

    std::vector<std::string> parts = Split(name);
    Powers power;
    if (parts[0] == "self" && parts.size() >= 2 && parts.size() <= 3 && FindPower(parts[1], power) && (parts.size() == 2 || parts[2] == "pct"))
        return true;

    std::string scope, spell, tail;
    if (SplitAuraFact(parts, scope, spell, tail))
        return true;

    if (parts.size() == 3 && ((parts[0] == "cooldown" && (parts[2] == "ready" || parts[2] == "remains")) || (parts[0] == "spell" && parts[2] == "known")))
        return true;

    return false;
}

bool BotFacts::AuraFact(std::string const& name, double* number, bool* flag) const
{
    std::string scope, spell, tail;
    if (!SplitAuraFact(Split(name), scope, spell, tail))
        return false;

    Player* bot = _ai.GetBot();
    Unit* holder = scope == "buff" ? static_cast<Unit*>(bot) : _target;
    if (!holder)
    {
        // no target: no aura on it
        if (number)
            *number = 0.0;
        if (flag)
            *flag = false;
        return true;
    }

    bool numeric = !spell.empty() && std::ranges::all_of(spell, [](unsigned char c) { return std::isdigit(c); });
    uint32 wantedId = numeric ? uint32(std::strtoul(spell.c_str(), nullptr, 10)) : 0;
    std::string wantedName = NormalizeSpellName(spell);

    Aura const* found = nullptr;
    for (auto const& [id, application] : holder->GetAppliedAuras())
    {
        Aura const* aura = application->GetBase();
        if (!aura)
            continue;

        if (numeric ? aura->GetId() != wantedId : NormalizeSpellName(aura->GetSpellInfo()->SpellName ? aura->GetSpellInfo()->SpellName->Str[LOCALE_enUS] : "") != wantedName)
            continue;

        // own debuffs only: another player's dot is not ours to keep up
        if (scope == "debuff" && aura->GetCasterGUID() != bot->GetGUID())
            continue;

        found = aura;
        break;
    }

    if (tail == "remains")
    {
        if (number)
            *number = !found ? 0.0 : found->GetDuration() < 0 ? 99999.0 : double(found->GetDuration()) / 1000.0;
    }
    else if (tail == "stacks")
    {
        if (number)
            *number = found ? double(found->GetStackAmount()) : 0.0;
    }
    else if (flag)
        *flag = found != nullptr;
    else if (number)
        *number = found ? 1.0 : 0.0;

    return true;
}

bool BotFacts::Number(std::string const& name, double& value) const
{
    Player* bot = _ai.GetBot();

    if (name == "self.hp") { value = bot->GetHealthPct(); return true; }
    if (name == "self.power") { value = bot->GetMaxPower(bot->GetPowerType()) ? bot->GetPowerPct(bot->GetPowerType()) : 0.0; return true; }
    if (name == "self.power.value") { value = double(bot->GetPower(bot->GetPowerType())); return true; }
    if (name == "target.hp") { value = _target ? _target->GetHealthPct() : 0.0; return true; }
    if (name == "target.distance") { value = _target ? double(bot->GetExactDist(_target)) : 9999.0; return true; }
    if (name == "target.level_diff") { value = _target ? double(int32(_target->GetLevelForTarget(bot)) - int32(bot->GetLevel())) : 0.0; return true; }
    if (name == "enemies") { value = double(_runner.EnemiesAround(_ai, bot, 0)); return true; }
    if (name == "enemies.target") { value = double(_runner.EnemiesAround(_ai, _target, 1)); return true; }
    if (name == "enemies.ranged") { value = double(_runner.EnemiesAround(_ai, bot, 2)); return true; }

    if (name == "group.size" || name == "group.lowest_hp" || name == "group.injured")
    {
        uint32 size = 1;
        double lowest = bot->GetHealthPct();
        uint32 injured = bot->GetHealthPct() < 80.0f ? 1 : 0;
        if (Group* group = bot->GetGroup())
        {
            size = 0;
            lowest = 100.0;
            injured = 0;
            for (GroupReference const& ref : group->GetMembers())
            {
                Player* member = ref.GetSource();
                if (!member || !member->IsAlive() || !member->IsInMap(bot))
                    continue;
                ++size;
                lowest = std::min<double>(lowest, member->GetHealthPct());
                injured += member->GetHealthPct() < 80.0f ? 1 : 0;
            }
        }
        value = name == "group.size" ? double(size) : name == "group.lowest_hp" ? lowest : double(injured);
        return true;
    }

    std::vector<std::string> parts = Split(name);
    Powers power;
    if (parts[0] == "self" && parts.size() >= 2 && FindPower(parts[1], power))
    {
        value = parts.size() == 3 && parts[2] == "pct" ? (bot->GetMaxPower(power) ? double(bot->GetPowerPct(power)) : 0.0) : double(bot->GetPower(power));
        return true;
    }

    if (parts[0] == "cooldown" && parts.size() == 3 && parts[2] == "remains")
    {
        value = 0.0;
        for (uint32 id : _runner.Resolve(bot, parts[1]))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE))
                value = std::max(value, double(std::chrono::duration_cast<Milliseconds>(bot->GetSpellHistory()->GetRemainingCooldown(info)).count()) / 1000.0);
        return true;
    }

    return AuraFact(name, &value, nullptr);
}

bool BotFacts::Flag(std::string const& name, bool& value) const
{
    Player* bot = _ai.GetBot();

    if (name == "in_combat") { value = bot->IsInCombat(); return true; }
    if (name == "moving") { value = bot->isMoving(); return true; }
    if (name == "mounted") { value = bot->IsMounted(); return true; }
    if (name == "solo") { value = !bot->GetGroup(); return true; }
    if (name == "pet") { value = bot->GetPet() != nullptr; return true; }
    if (name == "pet.alive") { value = bot->GetPet() && bot->GetPet()->IsAlive(); return true; }
    if (name == "role.tank") { value = _ai.GetRole() == BotRole::Tank; return true; }
    if (name == "role.healer") { value = _ai.GetRole() == BotRole::Healer; return true; }
    if (name == "role.dps") { value = _ai.GetRole() == BotRole::MeleeDps || _ai.GetRole() == BotRole::RangedDps; return true; }

    if (name == "target.boss")
    {
        Creature const* creature = _target ? _target->ToCreature() : nullptr;
        value = creature && (creature->isWorldBoss() || creature->IsDungeonBoss());
        return true;
    }
    if (name == "target.elite")
    {
        Creature const* creature = _target ? _target->ToCreature() : nullptr;
        value = creature && creature->IsElite();
        return true;
    }
    if (name == "target.casting") { value = _target && _target->IsNonMeleeSpellCast(false, false, true); return true; }
    if (name == "target.moving") { value = _target && _target->isMoving(); return true; }
    if (name == "target.in_melee") { value = _target && bot->GetExactDist(_target) <= bot->GetMeleeRange(_target); return true; }
    if (name == "target.targeting_me") { value = _target && _target->GetVictim() == bot; return true; }
    if (name == "target.is_player") { value = _target && _target->IsPlayer(); return true; }

    std::vector<std::string> parts = Split(name);
    if (parts[0] == "cooldown" && parts.size() == 3 && parts[2] == "ready")
    {
        std::vector<uint32> const& ids = _runner.Resolve(bot, parts[1]);
        value = !ids.empty();
        for (uint32 id : ids)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE); info && !bot->GetSpellHistory()->IsReady(info))
                value = false;
        return true;
    }
    if (parts[0] == "spell" && parts.size() == 3 && parts[2] == "known")
    {
        value = !_runner.Resolve(bot, parts[1]).empty();
        return true;
    }

    return AuraFact(name, nullptr, &value);
}
}
