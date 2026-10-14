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

#include "DangerMap.h"
#include "CreatureData.h"
#include "DB2Stores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PhaseGuard.h"
#include "Timer.h"
#include "Util.h"
#include <algorithm>
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr float CellSize = 128.0f;
constexpr float SampleStep = 64.0f;

int32 CellOf(float v)
{
    return int32(std::floor(v / CellSize));
}
}

DangerMap* DangerMap::instance()
{
    static DangerMap instance;
    return &instance;
}

void DangerMap::Build()
{
    _maps.clear();
    uint32 oldMSTime = getMSTime();

    // the first Human and Orc stand for the two teams, like in the hub index
    FactionTemplateEntry const* alliance = nullptr;
    FactionTemplateEntry const* horde = nullptr;
    if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(RACE_HUMAN))
        alliance = sFactionTemplateStore.LookupEntry(race->FactionID);
    if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(RACE_ORC))
        horde = sFactionTemplateStore.LookupEntry(race->FactionID);

    // per template: how dangerous it is, worked out once (a few thousand entries against hundreds of thousands of spawns)
    struct Danger
    {
        uint8 Level = 0;
        bool Alliance = false;
        bool Horde = false;
    };
    std::unordered_map<uint32, Danger> byEntry;
    auto dangerOf = [&](uint32 entry) -> Danger const&
    {
        auto [itr, inserted] = byEntry.try_emplace(entry);
        if (!inserted)
            return itr->second;

        CreatureTemplate const* temp = sObjectMgr->GetCreatureTemplate(entry);
        if (!temp || temp->type == CREATURE_TYPE_CRITTER || temp->type == CREATURE_TYPE_NON_COMBAT_PET
            || (temp->flags_extra & (CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER)))
            return itr->second;

        CreatureDifficulty const* difficulty = temp->GetDifficulty(DIFFICULTY_NONE);
        if (!difficulty || difficulty->ContentTuningID <= 0)
            return itr->second;

        Optional<ContentTuningLevels> levels = sDB2Manager.GetContentTuningData(uint32(difficulty->ContentTuningID), {});
        if (!levels || levels->MinLevel < 1)
            return itr->second;

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(temp->faction);
        if (!faction)
            return itr->second;

        Danger& danger = itr->second;
        danger.Level = uint8(std::clamp<int32>(int32(levels->MinLevel) + int32(difficulty->DeltaLevelMin), 1, 255));
        danger.Alliance = alliance && faction->IsHostileTo(alliance);
        danger.Horde = horde && faction->IsHostileTo(horde);
        return danger;
    };

    uint32 counted = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!PhaseGuard::IsUsableSpawnPhase(data.phaseId, data.phaseGroup))
            continue;

        MapEntry const* map = sMapStore.LookupEntry(data.mapId);
        if (!map || map->Instanceable())
            continue;

        Danger const& danger = dangerOf(data.id);
        if (!danger.Level || (!danger.Alliance && !danger.Horde))
            continue;

        Cell& cell = _maps[data.mapId][Key(CellOf(data.spawnPoint.GetPositionX()), CellOf(data.spawnPoint.GetPositionY()))];
        if (danger.Alliance)
            cell.Alliance = std::max(cell.Alliance, danger.Level);
        if (danger.Horde)
            cell.Horde = std::max(cell.Horde, danger.Level);
        ++counted;
    }

    _built = true;
    TC_LOG_INFO("server.playerbots", "Playerbots: danger map built from {} hostile spawns in {} ms", counted, GetMSTimeDiffToNow(oldMSTime));
}

uint8 DangerMap::LevelNear(uint32 mapId, float x, float y, uint8 team) const
{
    auto mapItr = _maps.find(mapId);
    if (mapItr == _maps.end())
        return 0;

    uint8 level = 0;
    int32 cx = CellOf(x);
    int32 cy = CellOf(y);
    for (int32 dx = -1; dx <= 1; ++dx)
        for (int32 dy = -1; dy <= 1; ++dy)
        {
            auto cell = mapItr->second.find(Key(cx + dx, cy + dy));
            if (cell == mapItr->second.end())
                continue;

            level = std::max(level, team == 1 ? cell->second.Alliance : team == 2 ? cell->second.Horde : std::max(cell->second.Alliance, cell->second.Horde));
        }
    return level;
}

uint8 DangerMap::LevelAlong(uint32 mapId, float x1, float y1, float x2, float y2, uint8 team) const
{
    float length = std::hypot(x2 - x1, y2 - y1);
    uint32 steps = std::max<uint32>(1, uint32(std::ceil(length / SampleStep)));
    uint8 level = 0;
    for (uint32 i = 0; i <= steps; ++i)
    {
        float t = float(i) / float(steps);
        level = std::max(level, LevelNear(mapId, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, team));
    }
    return level;
}
}
