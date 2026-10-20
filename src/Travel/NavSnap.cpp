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

#include "NavSnap.h"
#include "DetourNavMeshQuery.h"
#include "MMapDefines.h"
#include "MMapManager.h"
#include "Map.h"
#include "PhasingHandler.h"
#include "Player.h"

namespace Playerbots
{
bool SnapPointToNavMesh(Player const* bot, Position const& wanted, Position& out, float horizontal, float vertical)
{
    Map* map = bot ? bot->GetMap() : nullptr;
    if (!map)
        return false;

    uint32 meshMapId = PhasingHandler::GetTerrainMapId(bot->GetPhaseShift(), map->GetId(), map->GetTerrain(), wanted.GetPositionX(), wanted.GetPositionY());
    dtNavMeshQuery const* query = MMAP::MMapManager::instance()->GetNavMeshQuery(meshMapId, map->GetId(), map->GetInstanceId());
    if (!query)
        return false;

    // the mesh stores (y, z, x)
    float point[3] = { wanted.GetPositionY(), wanted.GetPositionZ(), wanted.GetPositionX() };
    float extents[3] = { horizontal, vertical, horizontal };
    dtQueryFilter filter;
    filter.setIncludeFlags(NAV_GROUND | NAV_GROUND_STEEP);
    filter.setExcludeFlags(NAV_EMPTY);

    dtPolyRef polygon = 0;
    float closest[3] = { 0.0f, 0.0f, 0.0f };
    if (!dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &polygon, closest)) || !polygon)
        return false;

    out = Position(closest[2], closest[0], closest[1], wanted.GetOrientation());
    return true;
}
}
