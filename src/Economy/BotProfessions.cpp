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
#include "DatabaseEnv.h"
#include "Player.h"
#include "Log.h"
#include "SharedDefines.h"
#include "StringFormat.h"
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

bool CanLearn(uint8 race, uint8 classId, uint32 skill)
{
    return sDB2Manager.GetSkillRaceClassInfo(skill, race, classId) != nullptr;
}

bool CanLearn(Player const* bot, uint32 skill)
{
    return CanLearn(bot->GetRace(), bot->GetClass(), skill);
}

// the same character always draws the same pair
Pair const& PairFor(ObjectGuid guid)
{
    return Pairs[(guid.GetCounter() * 2654435761u >> 8) % Pairs.size()];
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

void SeedOffline(ObjectGuid guid)
{
    // A skill raised on a bot that is in the world completes achievements one by one, and the core announces each to the guild and to
    // every player in range: a new bot raises dozens at once, a levelling bot a tier at a time (Maintain used to follow level * 8).
    // Skills loaded from the database complete nothing, so before its session starts loading a character gets every profession it
    // has, or would draw, at 900. Maintain then has nothing left to raise in the world.
    std::string primaries;
    for (uint32 skill : Primaries)
        primaries += (primaries.empty() ? "" : ",") + std::to_string(skill);

    QueryResult character = CharacterDatabase.PQuery("SELECT race, class FROM characters WHERE guid = {}", guid.GetCounter());
    if (!character)
        return;

    uint8 race = (*character)[0].GetUInt8();
    uint8 classId = (*character)[1].GetUInt8();

    std::unordered_map<uint32, uint32> held;
    if (QueryResult rows = CharacterDatabase.PQuery("SELECT skill, value FROM character_skills WHERE guid = {} AND skill IN ({}, {}, {})",
        guid.GetCounter(), uint32(SKILL_COOKING), uint32(SKILL_FISHING), primaries))
    {
        do
            held[(*rows)[0].GetUInt32()] = (*rows)[1].GetUInt32();
        while (rows->NextRow());
    }

    // a character with a primary profession keeps its own, a fresh one draws the pair
    std::vector<uint32> roots;
    bool hasPrimary = std::ranges::any_of(Primaries, [&](uint32 skill) { return held.contains(skill); });
    if (hasPrimary)
    {
        for (uint32 skill : Primaries)
            if (held.contains(skill))
                roots.push_back(skill);
    }
    else
    {
        Pair const& pair = PairFor(guid);
        for (uint32 skill : { pair.First, pair.Second })
            if (CanLearn(race, classId, skill))
                roots.push_back(skill);
    }

    for (uint32 skill : { uint32(SKILL_COOKING), uint32(SKILL_FISHING) })
        if (CanLearn(race, classId, skill))
            roots.push_back(skill);

    constexpr uint32 value = 900;
    bool complete = std::ranges::all_of(roots, [&](uint32 root) { auto itr = held.find(root); return itr != held.end() && itr->second >= value; });
    if (complete)
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    for (uint32 root : roots)
    {
        std::vector<uint32> lines{ root };
        for (uint32 child : ChildrenOf(root))
            lines.push_back(child);

        // the children are not checked, like Raise (bots that Raise already served load these lines without errors); the update
        // keeps the profession slot of a line the character already has
        for (uint32 line : lines)
            trans->Append(Trinity::StringFormat("INSERT INTO character_skills (guid, skill, value, max, professionSlot) VALUES ({0}, {1}, {2}, {2}, -1) "
                "ON DUPLICATE KEY UPDATE value = GREATEST(value, {2}), max = GREATEST(max, {2})", guid.GetCounter(), line, value).c_str());
    }
    // synchronous: the login query holder is queued right after this and must see the rows
    CharacterDatabase.DirectCommitTransaction(trans);
    TC_LOG_DEBUG("server.playerbots", "Playerbots: professions topped up for {}", guid.ToString());
}

void Maintain(Player* bot)
{
    if (!bot || bot->GetLevel() < 10)
        return;

    uint16 value = SkillFor(bot);

    bool hasPrimary = std::ranges::any_of(Primaries, [bot](uint32 skill) { return bot->HasSkill(skill); });
    if (!hasPrimary)
    {
        Pair const& pair = PairFor(bot->GetGUID());
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
