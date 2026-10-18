/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "World/WorldActionQueue.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Playerbots
{
namespace
{
constexpr size_t MaxDepth = 256;
constexpr uint32 MaxPerTick = 8;
constexpr float MaxDistance = 10.0f;

struct Job
{
    ObjectGuid Bot;
    uint32 MapId = 0;
    ObjectGuid Target;
    std::function<void(Player*)> Step;
};

std::mutex _mutex;
std::deque<Job> _queue;
std::unordered_map<ObjectGuid, uint32> _lastVisit;
uint32 _dropped = 0;
}

WorldActionQueue* WorldActionQueue::instance()
{
    static WorldActionQueue queue;
    return &queue;
}

bool WorldActionQueue::TryVisit(ObjectGuid bot, uint32 cooldownMs)
{
    uint32 now = GameTime::GetGameTimeMS();
    std::lock_guard lock(_mutex);
    auto itr = _lastVisit.find(bot);
    if (itr != _lastVisit.end() && now - itr->second < cooldownMs)
        return false;
    _lastVisit[bot] = now;
    return true;
}

void WorldActionQueue::Enqueue(ObjectGuid bot, uint32 mapId, ObjectGuid target, std::function<void(Player*)> step)
{
    std::lock_guard lock(_mutex);
    if (_queue.size() >= MaxDepth)
    {
        ++_dropped;
        TC_LOG_DEBUG("server.playerbots", "Playerbots: world queue full, dropped a step for {}", bot.ToString());
        return;
    }
    _queue.push_back({ bot, mapId, target, std::move(step) });
}

void WorldActionQueue::Drain()
{
    std::vector<Job> batch;
    {
        std::lock_guard lock(_mutex);
        while (batch.size() < MaxPerTick && !_queue.empty())
        {
            batch.push_back(std::move(_queue.front()));
            _queue.pop_front();
        }
    }

    for (Job& job : batch)
    {
        auto drop = [&](char const* why)
        {
            std::lock_guard lock(_mutex);
            ++_dropped;
            TC_LOG_DEBUG("server.playerbots", "Playerbots: world queue dropped {} ({})", job.Bot.ToString(), why);
        };

        if (!sPlayerbotMgr->IsBotOnline(job.Bot))
        {
            drop("logged out");
            continue;
        }

        Player* bot = ObjectAccessor::FindConnectedPlayer(job.Bot);
        if (!bot || !bot->IsInWorld() || bot->GetMapId() != job.MapId || !bot->GetSession())
        {
            drop("not on the map");
            continue;
        }

        if (!job.Target.IsEmpty())
        {
            WorldObject* target = nullptr;
            if (job.Target.IsGameObject())
                target = bot->GetMap()->GetGameObject(job.Target);
            else if (job.Target.IsCreature())
                target = bot->GetMap()->GetCreature(job.Target);

            if (!target || bot->GetExactDist(target) > MaxDistance)
            {
                drop("target gone or too far");
                continue;
            }
        }

        job.Step(bot);
    }
}

uint32 WorldActionQueue::Depth() const
{
    std::lock_guard lock(_mutex);
    return uint32(_queue.size());
}

uint32 WorldActionQueue::Dropped() const
{
    std::lock_guard lock(_mutex);
    return _dropped;
}
}
