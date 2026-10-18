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

#include "SpawnPolicy.h"
#include "ConditionMgr.h"
#include "DB2Stores.h"
#include "DBCEnums.h"
#include "Map.h"
#include "MapManager.h"
#include "Player.h"
#include "Travel/HubIndex.h"
#include "Travel/TravelMgr.h"
#include <iterator>

namespace Playerbots::SpawnPolicy
{
namespace
{
// the first area in the chain (area, then its parents) that says who owns it
uint32 OwnerMask(uint32 areaId)
{
    for (uint32 hops = 0; areaId && hops < 4; ++hops)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        if (!area)
            return 0;
        if (uint32 mask = area->FactionGroupMask & (FACTION_MASK_ALLIANCE | FACTION_MASK_HORDE))
            return mask;
        areaId = area->ParentAreaID;
    }
    return 0;
}

// Quarters whose AreaTable faction mask is 0, so the owner walk above cannot see them.
// Ids are from build 12.1.0.69933. Alliance is blocked from the Horde list, and the reverse.
constexpr uint32 HordeOnlyAreas[] =
{
    4616, 4666, 4676, 6634, 7589, 7893,             // Sunreaver's Sanctuary, Sunreaver pavilion, Windrunner's Sanctuary
    16083, 16084, 16085, 16086, 16618               // Silvermoon: Walk of Elders, Royal Exchange, Court of Blood, Court of the Sun, Hall of Blood
};

constexpr uint32 AllianceOnlyAreas[] =
{
    4740, 4667, 4677, 6627, 7508, 7884              // Silver Enclave, Silver Covenant pavilion, Greyfang Enclave
};

bool ListedArea(uint32 areaId, Team team)
{
    uint32 const* list = team == ALLIANCE ? HordeOnlyAreas : AllianceOnlyAreas;
    size_t count = team == ALLIANCE ? std::size(HordeOnlyAreas) : std::size(AllianceOnlyAreas);
    for (size_t i = 0; i < count; ++i)
        if (list[i] == areaId)
            return true;
    return false;
}

// the area or one of its parents is a quarter the other faction owns
bool DistrictChain(uint32 areaId, Team team)
{
    for (uint32 hops = 0; areaId && hops < 4; ++hops)
    {
        if (ListedArea(areaId, team))
            return true;
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        if (!area)
            return false;
        areaId = area->ParentAreaID;
    }
    return false;
}

// Northrend Dalaran streets still report the neutral city. These boxes are the ones AzerothCore
// measured on map 571 (z 620-680): the first is hostile to Alliance, the second to Horde.
bool DalaranStreet(uint32 mapId, float x, float y, float z, Team team)
{
    if (mapId != 571 || z < 620.0f || z > 680.0f)
        return false;

    if (team == ALLIANCE && x >= 5810.0f && x <= 6050.0f && y >= 440.0f && y <= 680.0f)
        return true;
    if (team == HORDE && x >= 5575.0f && x <= 5835.0f && y >= 590.0f && y <= 810.0f)
        return true;
    return false;
}
}

bool IsSplitDistrict(uint32 mapId, uint32 areaId, float x, float y, float z, Team team)
{
    if (team != ALLIANCE && team != HORDE)
        return false;
    return DistrictChain(areaId, team) || DalaranStreet(mapId, x, y, z, team);
}

bool IsEnemyGround(uint32 mapId, uint32 areaId, float x, float y, float z, Team team)
{
    if (team != ALLIANCE && team != HORDE)
        return false;

    // owned by exactly one faction, and not ours (Silvermoon's Horde quarters already have mask 4)
    uint32 ownMask = team == ALLIANCE ? FACTION_MASK_ALLIANCE : FACTION_MASK_HORDE;
    uint32 owner = OwnerMask(areaId);
    if (owner && !(owner & ownMask))
        return true;

    if (IsSplitDistrict(mapId, areaId, x, y, z, team))
        return true;

    // inside a town whose NPCs attack the team: needs the hub index only, not the travel graph (Playerbots.Travel.Enable)
    return HubIndex::IsBuilt() && sTravelMgr->IsHostileGround(mapId, x, y, team == ALLIANCE ? 1 : 2);
}

bool IsEnemyGroundAt(Player const* bot, uint32 mapId, Position const& pos)
{
    if (!bot)
        return false;

    // the area id needs a loaded map; without one only the town check can answer
    uint32 areaId = 0;
    if (Map* map = sMapMgr->FindMap(mapId, 0))
        areaId = map->GetAreaId(bot->GetPhaseShift(), pos);

    return IsEnemyGround(mapId, areaId, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), Player::TeamForRace(bot->GetRace()));
}

bool IsEnemyGround(Player const* bot)
{
    Map* map = bot ? bot->GetMap() : nullptr;
    if (!map)
        return false;

    // the bot's own cached area (kept up to date by the core), no terrain query per bot
    uint32 areaId = bot->GetAreaId();
    return IsEnemyGround(bot->GetMapId(), areaId, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), Player::TeamForRace(bot->GetRace()));
}

Optional<Position> SafeDistrict(Player const* bot)
{
    if (!bot || bot->GetMapId() != 571)
        return {};

    // only the Northrend city has surveyed points. Legion Dalaran and Silvermoon fall through to a normal placement.
    float z = bot->GetPositionZ();
    if (z < 620.0f || z > 680.0f)
        return {};

    Team team = Player::TeamForRace(bot->GetRace());
    if (team == ALLIANCE)
        return Position(5620.8f, 688.002f, 652.641f, 0.0f);
    if (team == HORDE)
        return Position(5987.35f, 614.434f, 651.223f, 0.0f);
    return {};
}

uint32 TeamOfGraveyard(ConditionsReference const& conditions)
{
    std::shared_ptr<std::vector<Condition>> list = conditions.Conditions.lock();
    if (!list)
        return 0;

    uint32 team = 0;
    for (Condition const& condition : *list)
    {
        if (condition.ConditionType != CONDITION_TEAM)
            continue;

        uint32 value = condition.ConditionValue1;
        if (value != ALLIANCE && value != HORDE)
            continue;
        if (condition.NegativeCondition)
            value = value == ALLIANCE ? HORDE : ALLIANCE;

        if (team && team != value)
            return 0;   // else-groups that allow both teams
        team = value;
    }
    return team;
}
}
