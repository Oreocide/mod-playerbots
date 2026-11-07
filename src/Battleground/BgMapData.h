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

// The objectives of the battlegrounds the planner plays. The five pilot maps are code tables (phase 1); Alterac Valley reads its
// objective graph from world.playerbots_bg_node (phase 3, docs/design/BG_PLAN.md §2 "Objective graph")

#include "Battleground/BgRules.h"
#include "Define.h"
#include "Position.h"
#include <string>
#include <vector>

namespace Playerbots::Bg
{
enum class MapKind : uint8
{
    Flag,       // Warsong Gulch, Twin Peaks: two flags, three captures win
    Capture,    // Arathi Basin, Battle for Gilneas: type 42 capture points, 1500 points win
    Eye,        // Eye of the Storm: four control-zone towers and one flag in the middle
    Valley      // Alterac Valley: graveyards, towers and bosses from playerbots_bg_node, state from world states
};

/// playerbots_bg_node.kind
enum class NodeKind : uint8
{
    Capture,        // a node held while owned (graveyards; the code tables' capture points and towers)
    Burn,           // a tower: assaulted, then destroyed when the assault runs out
    KillCaptain,
    KillGeneral,
    Rally,          // where the attackers gather before a boss
    Gate            // where the team waits while the gates are closed
};

char const* NodeKindName(NodeKind kind);

struct NodeDef
{
    uint32 Entry = 0;               // code tables: the game object of the node
    std::string Name;
    float Weight = 1.0f;            // code tables: share of the defenders this node gets while it is held

    // playerbots_bg_node rows (Valley maps)
    std::string Key;
    NodeKind Kind = NodeKind::Capture;
    uint8 Side = 0;                 // 0 none, 1 Alliance, 2 Horde (see the SQL file)
    Position Pos;
    float Radius = 0.0f;
    uint32 CreatureEntry = 0;
    uint32 WsAllianceControl = 0;
    uint32 WsAllianceAssault = 0;
    uint32 WsHordeControl = 0;
    uint32 WsHordeAssault = 0;
    uint32 WsAlive = 0;
    BgRules::Requirement Requires;
    float Weights[2] = { 0.0f, 0.0f };  // [TEAM_ALLIANCE], [TEAM_HORDE]
    uint8 MinGroup = 0;
    std::string RallyKey;
    int8 Rally = -1;                // index of the rally row in MapDef::Nodes
    float LeashRadius = 0.0f;
    std::string Aliases;

    bool IsBoss() const { return Kind == NodeKind::KillCaptain || Kind == NodeKind::KillGeneral; }
    bool IsNode() const { return Kind == NodeKind::Capture || Kind == NodeKind::Burn; }
};

struct MapDef
{
    uint32 MapId;
    char const* Name;
    MapKind Kind;
    std::vector<NodeDef> Nodes;
    uint32 FlagEntries[2];      // Flag maps: [0] the Alliance flag, [1] the Horde flag. Eye: the one flag in both
    uint32 WinScore;            // Battleground::GetTeamScore reaches this to win
    uint32 ScoreWs[2] = { 0, 0 };   // the score is in these world states instead (Alterac Valley reinforcements)
    uint32 ScoreMaxWs = 0;          // the starting score
};

/// The planned definition of the map, nullptr when there is none (a Valley map without rows in playerbots_bg_node)
MapDef const* FindMap(uint32 mapId);
/// For .bot bg rally set: the same definition, to move one row in place
MapDef* FindMapForEdit(uint32 mapId);
/// Reads playerbots_bg_node into the Valley maps. World thread, at startup and on .bot bg reload: the caller drops every plan in
/// the same call, plans index these rows. Returns the rows loaded
uint32 LoadNodes();

/// The spell a player casts to open a battleground object with this lock (the client casts it on a click): 0 = Use it directly
uint32 OpenSpellForLock(uint32 lockId);
}

#endif
