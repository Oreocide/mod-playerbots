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

#include "PhaseGuard.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "DB2Stores.h"
#include "Player.h"
#include "PhaseShift.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

namespace Playerbots
{
namespace PhaseGuard
{
namespace
{
std::unordered_set<uint32> sGatedQuests;
std::unordered_set<uint32> sSharedPhases;
std::unordered_set<uint32> sRuntimeBlocked;   // quests found while the server runs
std::shared_mutex sRuntimeMutex;
}

void Load()
{
    sGatedQuests.clear();
    sSharedPhases.clear();

    // CONDITION_SOURCE_TYPE_TERRAIN_SWAP (25) and _PHASE (26); quest conditions: rewarded 8, taken 9, none 14,
    // complete 28, state 47. A phase condition may also refer to a shared condition group (negative reference)
    QueryResult result = WorldDatabase.Query(
        "SELECT DISTINCT ConditionValue1 FROM conditions WHERE ConditionTypeOrReference IN (8, 9, 14, 28, 47) AND ("
        "SourceTypeOrReferenceId IN (25, 26) OR SourceTypeOrReferenceId IN ("
        "SELECT -ConditionTypeOrReference FROM (SELECT ConditionTypeOrReference FROM conditions "
        "WHERE SourceTypeOrReferenceId IN (25, 26) AND ConditionTypeOrReference < 0) refs))");

    if (result)
    {
        do
            sGatedQuests.insert((*result)[0].GetUInt32());
        while (result->NextRow());
    }

    // phases that phase_area gives to everyone in an area and that no condition restricts anywhere
    if (QueryResult shared = WorldDatabase.Query("SELECT DISTINCT PhaseId FROM phase_area WHERE PhaseId NOT IN "
        "(SELECT SourceGroup FROM conditions WHERE SourceTypeOrReferenceId = 26)"))
    {
        do
            sSharedPhases.insert((*shared)[0].GetUInt32());
        while (shared->NextRow());
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: {} quests change or gate a phase and are left alone, {} phases are given to every player of their area and count as the default",
        sGatedQuests.size(), sSharedPhases.size());
}

bool IsPhaseGatedQuest(uint32 questId)
{
    if (sGatedQuests.contains(questId))
        return true;

    std::shared_lock lock(sRuntimeMutex);
    return sRuntimeBlocked.contains(questId);
}

std::vector<uint16> ForeignPhases(Player const* player)
{
    std::vector<uint16> foreign;
    for (PhaseShift::PhaseRef const& phase : player->GetPhaseShift().GetPhases())
    {
        if (phase.Id == DefaultPhaseId || IsSharedPhase(phase.Id))
            continue;

        // a cosmetic phase only adds scenery, everyone can see the same things
        if (PhaseEntry const* entry = sPhaseStore.LookupEntry(phase.Id); entry && entry->GetFlags().HasFlag(PhaseEntryFlags::Cosmetic))
            continue;

        foreign.push_back(phase.Id);
    }

    std::ranges::sort(foreign);
    return foreign;
}

void BlockQuest(uint32 questId, char const* reason)
{
    {
        std::unique_lock lock(sRuntimeMutex);
        if (!sRuntimeBlocked.insert(questId).second)
            return;
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: quest {} moved a bot into a phase of its own ({}), bots skip it from now on", questId, reason);
    CharacterDatabase.PExecute("INSERT IGNORE INTO playerbots_quest_blacklist (quest_id, reason) VALUES ({}, '{}')", questId, reason);
}

bool IsSharedPhase(uint32 phaseId)
{
    return sSharedPhases.contains(phaseId);
}

uint32 GetSharedPhaseCount()
{
    return uint32(sSharedPhases.size());
}

uint32 GetGatedQuestCount()
{
    return uint32(sGatedQuests.size());
}
}
}
