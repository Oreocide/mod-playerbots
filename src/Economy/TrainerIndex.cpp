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

#include "TrainerIndex.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <array>
#include <unordered_map>

namespace Playerbots::TrainerIndex
{
namespace
{
struct Row
{
    uint32 SpellId = 0;
    uint32 MoneyCost = 0;
    uint32 ReqSkillLine = 0;
    uint32 ReqSkillRank = 0;
    std::array<uint32, 3> ReqAbility = { };
    uint8 ReqLevel = 0;
};

// built once at startup, read-only after: safe from every map thread
std::unordered_map<uint32, std::vector<Row>> sTrainers;

// the same questions as the core's Trainer::GetSpellState, so a spell listed here is one the core will teach
bool LooksLearnable(Player const* bot, Row const& row)
{
    if (bot->HasSpell(row.SpellId) || row.ReqLevel > bot->GetLevel() || row.MoneyCost > bot->GetMoney())
        return false;

    if (!bot->IsSpellFitByClassAndRace(row.SpellId))
        return false;

    if (row.ReqSkillLine && bot->GetBaseSkillValue(row.ReqSkillLine) < row.ReqSkillRank)
        return false;

    for (uint32 ability : row.ReqAbility)
        if (ability && !bot->HasSpell(ability))
            return false;

    // a trainer entry is usually a "learn X" spell: known once the bot has every spell it teaches
    SpellInfo const* info = sSpellMgr->GetSpellInfo(row.SpellId, DIFFICULTY_NONE);
    if (!info)
        return false;

    // Never a spell that grants a skill line: those are the professions themselves (Alchemy, Archaeology, Fishing...) and the
    // riding ranks. Which professions a bot has is decided by BotProfessions, and riding by RaceMounts::Grant; a trainer
    // must not hand out a third primary profession.
    if (info->HasEffect(SPELL_EFFECT_SKILL))
        return false;

    bool teaches = false;
    bool knowsAll = true;
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL))
        {
            SpellInfo const* learned = sSpellMgr->GetSpellInfo(effect.TriggerSpell, DIFFICULTY_NONE);
            if (learned && learned->HasEffect(SPELL_EFFECT_SKILL))
                return false;

            teaches = true;
            if (!bot->HasSpell(effect.TriggerSpell))
                knowsAll = false;
        }

    return !(teaches && knowsAll);
}
}

void Build()
{
    // read by every map thread afterwards: built once, never rebuilt while bots are online
    static bool built = false;
    if (built)
        return;
    built = true;

    sTrainers.clear();
    QueryResult result = WorldDatabase.Query("SELECT TrainerId, SpellId, MoneyCost, ReqSkillLine, ReqSkillRank, ReqAbility1, ReqAbility2, ReqAbility3, ReqLevel FROM trainer_spell");
    if (!result)
    {
        TC_LOG_INFO("server.playerbots", "Playerbots: no trainer spells in the world database");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        Row row;
        row.SpellId = fields[1].GetUInt32();
        row.MoneyCost = fields[2].GetUInt32();
        row.ReqSkillLine = fields[3].GetUInt32();
        row.ReqSkillRank = fields[4].GetUInt32();
        row.ReqAbility = { fields[5].GetUInt32(), fields[6].GetUInt32(), fields[7].GetUInt32() };
        row.ReqLevel = fields[8].GetUInt8();
        sTrainers[fields[0].GetUInt32()].push_back(row);
        ++count;
    } while (result->NextRow());

    for (auto& [trainerId, rows] : sTrainers)
        std::ranges::sort(rows, [](Row const& a, Row const& b) { return a.MoneyCost != b.MoneyCost ? a.MoneyCost < b.MoneyCost : a.SpellId < b.SpellId; });

    TC_LOG_INFO("server.playerbots", "Playerbots: {} trainer spells of {} trainers indexed", count, sTrainers.size());
}

std::vector<uint32> Learnable(Player const* bot, uint32 trainerId, uint32 maxCount)
{
    std::vector<uint32> spells;
    auto trainer = sTrainers.find(trainerId);
    if (!bot || trainer == sTrainers.end())
        return spells;

    std::vector<Row const*> candidates;
    for (Row const& row : trainer->second)
        if (LooksLearnable(bot, row))
            candidates.push_back(&row);

    // a random pick, not the cheapest ones: if the core ever refuses a spell for a reason this index cannot see, the same
    // cheap refusals must not use up the visit every time. A bot pays for each spell in turn.
    uint64 spent = 0;
    while (!candidates.empty() && spells.size() < maxCount)
    {
        size_t index = urand(0, uint32(candidates.size() - 1));
        Row const* row = candidates[index];
        candidates[index] = candidates.back();
        candidates.pop_back();
        if (spent + row->MoneyCost > bot->GetMoney())
            continue;

        spent += row->MoneyCost;
        spells.push_back(row->SpellId);
    }
    return spells;
}

bool HasLearnable(Player const* bot, uint32 trainerId)
{
    auto trainer = sTrainers.find(trainerId);
    if (!bot || trainer == sTrainers.end())
        return false;

    return std::ranges::any_of(trainer->second, [bot](Row const& row) { return LooksLearnable(bot, row); });
}
}
