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

#ifndef Playerbots_BgMapData_h__
#define Playerbots_BgMapData_h__

// The objectives of the battlegrounds the planner plays (phase 1: the five pilot maps, code tables). Alterac Valley and the
// bigger maps get a world table later (docs/design/BG_PLAN.md §2 "Objective graph")

#include "Define.h"
#include <vector>

namespace Playerbots::Bg
{
enum class MapKind : uint8
{
    Flag,       // Warsong Gulch, Twin Peaks: two flags, three captures win
    Capture,    // Arathi Basin, Battle for Gilneas: type 42 capture points, 1500 points win
    Eye         // Eye of the Storm: four control-zone towers and one flag in the middle
};

struct NodeDef
{
    uint32 Entry;
    char const* Name;
    float Weight;               // share of the defenders this node gets while it is held
};

struct MapDef
{
    uint32 MapId;
    char const* Name;
    MapKind Kind;
    std::vector<NodeDef> Nodes;
    uint32 FlagEntries[2];      // Flag maps: [0] the Alliance flag, [1] the Horde flag. Eye: the one flag in both
    uint32 WinScore;            // Battleground::GetTeamScore reaches this to win
};

MapDef const* FindMap(uint32 mapId);

/// The spell a player casts to open a battleground object with this lock (the client casts it on a click): 0 = Use it directly
uint32 OpenSpellForLock(uint32 lockId);
}

#endif
