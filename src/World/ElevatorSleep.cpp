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

#include "DB2Stores.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "PlayerbotsConfig.h"
#include "ScriptMgr.h"

// Playerbots.SleepElevators
//
// An elevator (a GAMEOBJECT_TYPE_TRANSPORT game object) is created "active": every map tick the core visits all cells around it and updates every creature there,
// even when no player is anywhere near (the grid stays loaded because bots stand in it). In the scale benchmark that was the biggest steady CPU cost
// (Undercity doors and elevators, Thunder Bluff and Ironforge elevators: 340-500 ms of work per second at 200 frozen bots).
//
// setActive(false) is public API. An inactive object is only updated when the cell it stands in is covered by a player's update range, i.e. exactly when a
// player (or a simulated bot) is near it; nothing a player can see changes, an unobserved elevator and the creatures around it just do not advance while nobody
// is there. This is a world change for everybody, not only for bots. It replaces the core switch SleepIdleActiveObjects without any core edit.
//
// The script runs on the map's own thread (WorldMapScript::OnUpdate, after the map's loops), the only thread allowed to touch the map's active list.

namespace
{
constexpr uint32 SweepIntervalMs = 5000;    // grids load and unload all the time, a newly created elevator is put to sleep within this long

class playerbots_elevator_sleep : public WorldMapScript
{
public:
    explicit playerbots_elevator_sleep(uint32 mapId) : WorldMapScript("playerbots_elevator_sleep", mapId) { }

    void OnUpdate(Map* map, uint32 diff) override
    {
        if (!sPlayerbotsConfig.SleepElevators)
            return;

        _accumulated += diff;
        if (_accumulated < SweepIntervalMs)
            return;
        _accumulated = 0;

        uint32 slept = 0;
        for (auto const& [spawnId, go] : map->GetGameObjectBySpawnIdStore())
        {
            if (go->IsInWorld() && go->GetGoType() == GAMEOBJECT_TYPE_TRANSPORT && go->isActiveObject())
            {
                go->setActive(false);
                ++slept;
            }
        }

        if (slept)
        {
            _total += slept;
            TC_LOG_INFO("server.playerbots", "Playerbots: map {} put {} elevator(s) to sleep ({} on this map so far)", map->GetId(), slept, _total);
        }
    }

private:
    uint32 _accumulated = 0;
    uint32 _total = 0;
};
}

void AddSC_playerbots_elevator_sleep()
{
    // one script per continent: a WorldMapScript is bound to one map id and is not database-bound
    for (MapEntry const* entry : sMapStore)
        if (entry->IsContinent())
            new playerbots_elevator_sleep(entry->ID);
}
