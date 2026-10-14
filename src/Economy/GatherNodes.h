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

#ifndef PlayerbotsGatherNodes_h__
#define PlayerbotsGatherNodes_h__

#include "Define.h"
#include <functional>

class Player;

namespace Playerbots::GatherNodes
{
struct Node
{
    uint64 SpawnId = 0;
    uint32 Entry = 0;
    uint32 Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    uint32 Skill = 0;    // skill line the lock is checked against (expansion line)
    uint16 Required = 0; // skill value the lock asks for
    bool Herb = false;   // else ore
};

/// Herb and ore spawns of the world (default phase, open world maps), built once at startup
void Build();
uint32 Count();

/// Nearest node on the bot's map within maxDist that satisfies accept
Node const* Nearest(Player const* bot, float maxDist, std::function<bool(Node const&)> const& accept);

/// Skill line and requirement of a gathering lock, false when the lock is not a herb or ore lock
bool ReadLock(uint32 lockId, uint32& skill, uint16& required, bool& herb);
}

#endif
