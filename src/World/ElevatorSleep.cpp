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

#include "Creature.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "ScriptMgr.h"
#include "TypeContainerVisitor.h"
#include "WorldSession.h"
#include <vector>

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

// Playerbots.SleepBattlefields
//
// Everything a battlefield spawns is active (Battlefield::SpawnCreature, SpawnGameObject, capture points by their type), so every map tick updates its cells with
// nobody there. In the 2026-10-30 audit Tol Barad and Wintergrasp took 19% of the map thread on empty ground.
//
// While no awake player is in the zone they leave the active list. The battle timers run in Battlefield::Update and do not need them. An active object is also what
// keeps its grid loaded and these spawns are not in the database, an unloaded grid would lose them for the battlefield: the grid is pinned first, and stays pinned
// (the lock is one flag shared with Warm.Pin). An awake player or simulated bot in the zone gets everything back at the next sweep.
class BattlefieldObjectSleeper
{
public:
    BattlefieldObjectSleeper(Map& map, uint32 zoneId, std::vector<ObjectGuid>& slept) : _map(map), _zoneId(zoneId), _slept(slept) { }

    void Visit(std::unordered_map<ObjectGuid, Creature*>& creatures)
    {
        for (auto const& [guid, creature] : creatures)
            if (!creature->IsPet() && !creature->IsSummon() && creature->GetCharmerOrOwnerGUID().IsEmpty())
                Sleep(creature);
    }

    void Visit(std::unordered_map<ObjectGuid, GameObject*>& gameObjects)
    {
        for (auto const& [guid, gameObject] : gameObjects)
            if (gameObject->GetGoType() != GAMEOBJECT_TYPE_TRANSPORT && gameObject->GetOwnerGUID().IsEmpty())
                Sleep(gameObject);
    }

    template<class ObjectType>
    void Visit(std::unordered_map<ObjectGuid, ObjectType*>&) { }

private:
    void Sleep(WorldObject* object)
    {
        if (!object->IsInWorld() || !object->isActiveObject() || (_zoneId && object->GetZoneId() != _zoneId))
            return;

        _map.SetUnloadLock(Trinity::ComputeGridCoord(object->GetPositionX(), object->GetPositionY()), true);
        object->setActive(false);
        _slept.push_back(object->GetGUID());
    }

    Map& _map;
    uint32 _zoneId;
    std::vector<ObjectGuid>& _slept;
};

class playerbots_battlefield_sleep : public WorldMapScript
{
public:
    // zoneId 0: the whole map
    playerbots_battlefield_sleep(uint32 mapId, uint32 zoneId) : WorldMapScript("playerbots_battlefield_sleep", mapId), _zoneId(zoneId) { }

    void OnUpdate(Map* map, uint32 diff) override
    {
        bool const enabled = sPlayerbotsConfig.SleepBattlefields;
        if (!enabled && _slept.empty())
            return;

        _accumulated += diff;
        if (_accumulated < SweepIntervalMs)
            return;
        _accumulated = 0;

        if (enabled && !HasAwakePlayer(map))
            Sleep(map);
        else
            Wake(map);
    }

private:
    bool HasAwakePlayer(Map const* map) const
    {
        for (MapReference const& ref : map->GetPlayers())
            if (Player const* player = ref.GetSource(); player && !player->GetSession()->IsDormant() && (!_zoneId || player->GetZoneId() == _zoneId))
                return true;

        return false;
    }

    void Sleep(Map* map)
    {
        size_t const before = _slept.size();
        BattlefieldObjectSleeper worker(*map, _zoneId, _slept);
        TypeContainerVisitor<BattlefieldObjectSleeper, MapStoredObjectTypesContainer> visitor(worker);
        visitor.Visit(map->GetObjectsStore());

        if (_slept.size() != before)
            TC_LOG_INFO("server.playerbots", "Playerbots: map {} put {} battlefield object(s) to sleep", map->GetId(), _slept.size() - before);
    }

    void Wake(Map* map)
    {
        if (_slept.empty())
            return;

        uint32 woken = 0;
        for (ObjectGuid const& guid : _slept)
        {
            WorldObject* object = nullptr;
            if (guid.IsGameObject())
                object = map->GetGameObject(guid);
            else
                object = map->GetCreature(guid);

            if (object)
            {
                object->setActive(true);
                ++woken;
            }
        }

        TC_LOG_INFO("server.playerbots", "Playerbots: map {} woke {} battlefield object(s)", map->GetId(), woken);
        _slept.clear();
    }

    uint32 _zoneId;
    uint32 _accumulated = 0;
    std::vector<ObjectGuid> _slept;
};
}

void AddSC_playerbots_elevator_sleep()
{
    // one script per continent: a WorldMapScript is bound to one map id and is not database-bound
    for (MapEntry const* entry : sMapStore)
        if (entry->IsContinent())
            new playerbots_elevator_sleep(entry->ID);

    new playerbots_battlefield_sleep(571, 4197);    // Wintergrasp
    new playerbots_battlefield_sleep(732, 0);       // Tol Barad
}
