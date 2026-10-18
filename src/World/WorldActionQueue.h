/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef PlayerbotsWorldActionQueue_h__
#define PlayerbotsWorldActionQueue_h__

#include "ObjectGuid.h"
#include <functional>

class Player;

namespace Playerbots
{
/// Steps that must run on the world thread (THREADUNSAFE opcodes: mail, auction). Map threads only enqueue.
/// Drain() runs from WorldScript::OnUpdate, after map threads have finished the tick.
class WorldActionQueue
{
public:
    static WorldActionQueue* instance();

    /// True if this bot has not started a visit of this kind within cooldownMs. Records the visit when it returns true.
    bool TryVisit(ObjectGuid bot, uint32 cooldownMs);

    void Enqueue(ObjectGuid bot, uint32 mapId, ObjectGuid target, std::function<void(Player*)> step);
    void Drain();

    uint32 Depth() const;
    uint32 Dropped() const;
};
}

#define sWorldActionQueue Playerbots::WorldActionQueue::instance()

#endif
