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

#include "Economy/GatherNodes.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PhaseGuard.h"
#include "Player.h"
#include "RandomBotMgr.h"
#include "SharedDefines.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Playerbots::GatherNodes
{
namespace
{
std::unordered_map<uint32, std::vector<Node>> sNodes;
uint32 sCount = 0;

uint32 RootOf(uint32 skill)
{
    SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
    return line && line->ParentSkillLineID ? uint32(line->ParentSkillLineID) : skill;
}
}

bool ReadLock(uint32 lockId, uint32& skill, uint16& required, bool& herb)
{
    LockEntry const* lock = sLockStore.LookupEntry(lockId);
    if (!lock)
        return false;

    for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
    {
        if (lock->Type[j] != LOCK_KEY_SKILL)
            continue;

        SkillType type = SkillByLockType(LockType(lock->Index[j]));
        uint32 root = type == SKILL_NONE ? 0 : RootOf(type);
        if (root != SKILL_HERBALISM && root != SKILL_MINING)
            continue;

        skill = type;
        required = lock->Skill[j];
        herb = root == SKILL_HERBALISM;
        return true;
    }

    return false;
}

void Build()
{
    sNodes.clear();
    sCount = 0;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGameObjectData())
    {
        if (!PhaseGuard::IsUsableSpawnPhase(data.phaseId, data.phaseGroup))
            continue;
        if (!data.spawnDifficulties.empty() && std::ranges::find(data.spawnDifficulties, DIFFICULTY_NONE) == data.spawnDifficulties.end())
            continue;
        if (!sRandomBotMgr->IsMapAllowed(data.mapId))
            continue;

        GameObjectTemplate const* temp = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!temp || (temp->type != GAMEOBJECT_TYPE_CHEST && temp->type != GAMEOBJECT_TYPE_GATHERING_NODE))
            continue;

        uint32 lockId = temp->GetLockId();
        Node node;
        if (!lockId || !ReadLock(lockId, node.Skill, node.Required, node.Herb))
            continue;

        MapEntry const* map = sMapStore.LookupEntry(data.mapId);
        if (!map || map->Instanceable())
            continue;

        node.SpawnId = spawnId;
        node.Entry = data.id;
        node.Map = data.mapId;
        node.X = data.spawnPoint.GetPositionX();
        node.Y = data.spawnPoint.GetPositionY();
        node.Z = data.spawnPoint.GetPositionZ();
        sNodes[data.mapId].push_back(node);
        ++sCount;
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: {} herb and ore nodes known to gatherers", sCount);
}

uint32 Count()
{
    return sCount;
}

Node const* Nearest(Player const* bot, float maxDist, std::function<bool(Node const&)> const& accept)
{
    auto itr = sNodes.find(bot->GetMapId());
    if (itr == sNodes.end())
        return nullptr;

    Node const* best = nullptr;
    float bestSq = maxDist * maxDist;
    for (Node const& node : itr->second)
    {
        float dx = node.X - bot->GetPositionX();
        float dy = node.Y - bot->GetPositionY();
        float distSq = dx * dx + dy * dy;
        if (distSq >= bestSq || !accept(node))
            continue;

        bestSq = distSq;
        best = &node;
    }

    return best;
}
}
