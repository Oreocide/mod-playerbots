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

#include "MountVariety.h"
#include "ConditionMgr.h"
#include "DB2Stores.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "RaceMounts.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Util.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace Playerbots::MountVariety
{
namespace
{
// Mount.db2 SourceTypeEnum: 0 drop, 1 quest, 2 vendor, 3 profession, 5 achievement. Store, promotion, trading card and
// trading post mounts are not what a character earned in the world
constexpr int8 SourceDrop = 0;
constexpr int8 SourceQuest = 1;
constexpr int8 SourceVendor = 2;
constexpr int8 SourceProfession = 3;

struct PoolEntry
{
    uint32 Spell = 0;
    int8 Source = 0;
    uint32 Condition = 0;
};

// every ground mount of the client data a bot may own, built once
std::vector<PoolEntry> const& Pool()
{
    static std::vector<PoolEntry> pool;
    static std::once_flag once;
    std::call_once(once, []
    {
        for (MountEntry const* mount : sMountStore)
        {
            if (!mount->SourceSpellID || (mount->SourceTypeEnum != SourceDrop && mount->SourceTypeEnum != SourceQuest
                && mount->SourceTypeEnum != SourceVendor && mount->SourceTypeEnum != SourceProfession))
                continue;

            std::string name = mount->Name[LOCALE_enUS];
            strToLower(name);
            if (name.empty() || name.find("test") != std::string::npos || name.find("dnt") != std::string::npos || name.find("[ph]") != std::string::npos)
                continue;

            SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(mount->SourceSpellID), DIFFICULTY_NONE);
            if (!spell || !spell->HasAura(SPELL_AURA_MOUNTED))
                continue;

            bool ground = false;
            if (DB2Manager::MountTypeXCapabilitySet const* capabilities = sDB2Manager.GetMountCapabilities(mount->MountTypeID))
                for (MountTypeXCapabilityEntry const* link : *capabilities)
                    if (MountCapabilityEntry const* capability = sMountCapabilityStore.LookupEntry(link->MountCapabilityID))
                        ground = ground || (capability->Flags & MOUNT_CAPABILITY_FLAG_GROUND);
            if (!ground)
                continue;

            pool.push_back({ uint32(mount->SourceSpellID), mount->SourceTypeEnum, mount->PlayerConditionID });
        }
        TC_LOG_INFO("server.playerbots", "Playerbots: mount variety pool of {} ground mounts", pool.size());
    });
    return pool;
}

uint32 Mix(uint32 a, uint32 b)
{
    uint32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

struct Cached
{
    uint32 Count = 0;
    std::vector<uint32> Spells;
};

std::mutex sLock;
std::unordered_map<ObjectGuid, Cached> sCache;
}

uint32 CountForLevel(uint32 level)
{
    if (level < 20)
        return 0;
    if (level < 40)
        return 1;
    if (level < 60)
        return 2;
    if (level < 80)
        return 3;
    return level < 90 ? 4 : 6;
}

std::vector<uint32> AssignedFor(Player const* bot)
{
    uint32 count = CountForLevel(bot->GetLevel());
    if (!count)
        return { };

    {
        std::lock_guard lock(sLock);
        auto itr = sCache.find(bot->GetGUID());
        if (itr != sCache.end() && itr->second.Count == count)
            return itr->second.Spells;
    }

    // the lower levels draw from quests and vendors, from 60 on professions and drops too
    bool wide = bot->GetLevel() >= 60;
    uint32 seed = uint32(bot->GetGUID().GetCounter());

    std::vector<std::pair<uint32, uint32>> ranked;   // (hash, spell)
    for (PoolEntry const& entry : Pool())
    {
        if (!wide && entry.Source != SourceQuest && entry.Source != SourceVendor)
            continue;
        if (entry.Condition && !ConditionMgr::IsPlayerMeetingCondition(bot, entry.Condition))
            continue;
        ranked.emplace_back(Mix(seed, entry.Spell), entry.Spell);
    }
    std::ranges::sort(ranked);

    std::vector<uint32> spells;
    // the race's own vendor mount stays the first one
    if (uint32 racial = RaceMounts::PickKnown(bot))
        spells.push_back(racial);
    else if (std::span<uint32 const> list = RaceMounts::For(bot->GetRace()); !list.empty())
        spells.push_back(list[seed % list.size()]);

    for (auto const& [hash, spell] : ranked)
    {
        if (spells.size() >= count)
            break;
        if (std::ranges::find(spells, spell) == spells.end())
            spells.push_back(spell);
    }

    std::lock_guard lock(sLock);
    if (sCache.size() > 8192)
        sCache.clear();
    sCache[bot->GetGUID()] = { count, spells };
    return spells;
}

void Grant(Player* bot)
{
    for (uint32 spell : AssignedFor(bot))
        if (!bot->HasSpell(spell))
            bot->LearnSpell(spell, false);
}

uint32 PickRide(Player const* bot)
{
    std::vector<uint32> known;
    for (uint32 spell : AssignedFor(bot))
        if (bot->HasSpell(spell))
            known.push_back(spell);

    return known.empty() ? 0 : known[urand(0, uint32(known.size() - 1))];
}
}
