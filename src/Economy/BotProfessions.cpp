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

#include "Economy/BotProfessions.h"
#include "DB2Stores.h"
#include "Player.h"
#include "SharedDefines.h"
#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

namespace Playerbots::BotProfessions
{
namespace
{
struct Pair
{
    uint32 First;
    uint32 Second;
};

// weighted by repetition: what players really combine
constexpr std::array<Pair, 14> Pairs =
{{
    { SKILL_HERBALISM, SKILL_ALCHEMY }, { SKILL_HERBALISM, SKILL_ALCHEMY },
    { SKILL_MINING, SKILL_BLACKSMITHING }, { SKILL_MINING, SKILL_ENGINEERING }, { SKILL_MINING, SKILL_JEWELCRAFTING },
    { SKILL_SKINNING, SKILL_LEATHERWORKING }, { SKILL_SKINNING, SKILL_LEATHERWORKING },
    { SKILL_TAILORING, SKILL_ENCHANTING }, { SKILL_HERBALISM, SKILL_INSCRIPTION },
    { SKILL_MINING, SKILL_SKINNING }, { SKILL_HERBALISM, SKILL_SKINNING }, { SKILL_HERBALISM, SKILL_MINING },
    { SKILL_MINING, SKILL_BLACKSMITHING }, { SKILL_TAILORING, SKILL_ENCHANTING }
}};

constexpr uint32 Primaries[] =
{
    SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_HERBALISM, SKILL_INSCRIPTION,
    SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING
};

bool CanLearn(Player const* bot, uint32 skill)
{
    return sDB2Manager.GetSkillRaceClassInfo(skill, bot->GetRace(), bot->GetClass()) != nullptr;
}

uint16 SkillFor(Player const* bot)
{
    // generous at the top: a gathering lock demands up to a few hundred points, a bot at the level cap passes them all
    uint32 level = bot->GetLevel();
    return uint16(level >= 70 ? 900 : level * 8);
}

// the expansion lines of a profession (Legion Herbalism, Khaz Algar Mining...): gathering locks are checked against them
std::vector<uint32> const& ChildrenOf(uint32 root)
{
    static std::unordered_map<uint32, std::vector<uint32>> children = []
    {
        std::unordered_map<uint32, std::vector<uint32>> result;
        for (SkillLineEntry const* line : sSkillLineStore)
            if (line->ParentSkillLineID)
                result[line->ParentSkillLineID].push_back(line->ID);
        return result;
    }();

    static std::vector<uint32> const none;
    auto itr = children.find(root);
    return itr != children.end() ? itr->second : none;
}

void Raise(Player* bot, uint32 skill, uint16 value)
{
    if (!CanLearn(bot, skill))
        return;

    if (bot->GetSkillValue(skill) < value || bot->GetMaxSkillValue(skill) < value)
        bot->SetSkill(skill, 1, value, value);

    for (uint32 child : ChildrenOf(skill))
        if (bot->GetSkillValue(child) < value || bot->GetMaxSkillValue(child) < value)
            bot->SetSkill(child, 1, value, value);
}
}

bool CanGather(Player const* bot)
{
    return bot->HasSkill(SKILL_HERBALISM) || bot->HasSkill(SKILL_MINING);
}

void Maintain(Player* bot)
{
    if (!bot || bot->GetLevel() < 10)
        return;

    uint16 value = SkillFor(bot);

    bool hasPrimary = std::ranges::any_of(Primaries, [bot](uint32 skill) { return bot->HasSkill(skill); });
    if (!hasPrimary)
    {
        // the same character always draws the same pair
        Pair const& pair = Pairs[(bot->GetGUID().GetCounter() * 2654435761u >> 8) % Pairs.size()];
        for (uint32 skill : { pair.First, pair.Second })
            if (!CanLearn(bot, skill))
                continue;
            else
                Raise(bot, skill, value);
    }
    else
    {
        for (uint32 skill : Primaries)
            if (bot->HasSkill(skill))
                Raise(bot, skill, value);
    }

    // most players cook and fish a bit
    Raise(bot, SKILL_COOKING, value);
    Raise(bot, SKILL_FISHING, value);
}
}
