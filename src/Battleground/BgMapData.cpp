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

#include "Battleground/BgMapData.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <algorithm>

namespace Playerbots::Bg
{
namespace
{
// entries from the battleground scripts (battleground_arathi_basin.cpp:116-120, battleground_battle_for_gilneas.cpp:31-33,
// battleground_eye_of_the_storm.cpp:93-96, battleground_twin_peaks.cpp:50-51); the win scores from the same scripts. Alterac
// Valley's nodes come from playerbots_bg_node (LoadNodes); its score is the reinforcements (battleground_alterac_valley.cpp
// AV_WS_ALLIANCE_REINFORCEMENTS 3127, AV_WS_HORDE_REINFORCEMENTS 3128, AV_WS_MAX_REINFORCEMENTS 3136)
std::vector<MapDef> Maps =
{
    { 2106, "Warsong Gulch", MapKind::Flag, {}, { 227741, 227740 }, 3 },
    { 726, "Twin Peaks", MapKind::Flag, {}, { 227741, 227740 }, 3 },
    { 2107, "Arathi Basin", MapKind::Capture,
        {
            { 227420, "Stables", 1.0f },
            { 227522, "Blacksmith", 1.5f },
            { 227536, "Farm", 1.0f },
            { 227538, "Gold Mine", 1.0f },
            { 227544, "Lumber Mill", 1.0f },
        }, { 0, 0 }, 1500 },
    { 761, "Battle for Gilneas", MapKind::Capture,
        {
            { 228050, "Lighthouse", 1.0f },
            { 228052, "Waterworks", 1.5f },
            { 228053, "Mines", 1.0f },
        }, { 0, 0 }, 1500 },
    { 566, "Eye of the Storm", MapKind::Eye,
        {
            { 184080, "Blood Elf Tower", 1.0f },
            { 184081, "Fel Reaver Ruins", 1.0f },
            { 184082, "Mage Tower", 1.0f },
            { 184083, "Draenei Ruins", 1.0f },
        }, { 208977, 208977 }, 1500 },
    { 30, "Alterac Valley", MapKind::Valley, {}, { 0, 0 }, 0, { 3127, 3128 }, 3136 },
};

MapDef* Lookup(uint32 mapId)
{
    for (MapDef& def : Maps)
        if (def.MapId == mapId)
            return &def;
    return nullptr;
}

bool ParseKind(std::string const& text, NodeKind& kind)
{
    static std::pair<char const*, NodeKind> const Kinds[] =
    {
        { "capture", NodeKind::Capture }, { "burn", NodeKind::Burn }, { "kill_captain", NodeKind::KillCaptain },
        { "kill_general", NodeKind::KillGeneral }, { "rally", NodeKind::Rally }, { "gate", NodeKind::Gate },
    };
    for (auto const& [name, value] : Kinds)
        if (text == name)
        {
            kind = value;
            return true;
        }
    return false;
}
}

char const* NodeKindName(NodeKind kind)
{
    switch (kind)
    {
        case NodeKind::Capture: return "capture";
        case NodeKind::Burn: return "burn";
        case NodeKind::KillCaptain: return "kill_captain";
        case NodeKind::KillGeneral: return "kill_general";
        case NodeKind::Rally: return "rally";
        case NodeKind::Gate: return "gate";
    }
    return "?";
}

MapDef const* FindMap(uint32 mapId)
{
    MapDef const* def = Lookup(mapId);
    // a Valley map is only played with its rows
    if (def && def->Kind == MapKind::Valley && def->Nodes.empty())
        return nullptr;
    return def;
}

MapDef* FindMapForEdit(uint32 mapId)
{
    return const_cast<MapDef*>(FindMap(mapId));
}

uint32 LoadNodes()
{
    for (MapDef& def : Maps)
        if (def.Kind == MapKind::Valley)
            def.Nodes.clear();

    //                                                 0    1     2     3     4     5  6  7  8       9               10         11
    QueryResult result = WorldDatabase.Query("SELECT map, node, kind, side, name, x, y, z, radius, creature_entry, ws_a_ctrl, ws_a_assault, "
    //   12         13            14        15        16           17            18         19     20       21
        "ws_h_ctrl, ws_h_assault, ws_alive, requires, ally_weight, horde_weight, min_group, rally, leash_r, aliases FROM playerbots_bg_node ORDER BY map, node");
    if (!result)
    {
        TC_LOG_INFO("server.playerbots", "Playerbots: playerbots_bg_node is empty, Alterac Valley is not planned");
        return 0;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 mapId = fields[0].GetUInt32();
        MapDef* def = Lookup(mapId);
        if (!def || def->Kind != MapKind::Valley)
        {
            TC_LOG_ERROR("server.playerbots", "Playerbots: playerbots_bg_node row {} {}: map {} is not one the planner reads rows for, skipped",
                mapId, fields[1].GetString(), mapId);
            continue;
        }

        NodeDef node;
        node.Key = fields[1].GetString();
        if (!ParseKind(fields[2].GetString(), node.Kind))
        {
            TC_LOG_ERROR("server.playerbots", "Playerbots: playerbots_bg_node row {} {}: unknown kind '{}', skipped", mapId, node.Key, fields[2].GetString());
            continue;
        }
        node.Side = fields[3].GetUInt8();
        node.Name = fields[4].GetString();
        if (node.Name.empty())
            node.Name = node.Key;
        node.Pos.Relocate(fields[5].GetFloat(), fields[6].GetFloat(), fields[7].GetFloat());
        node.Radius = fields[8].GetFloat();
        node.CreatureEntry = fields[9].GetUInt32();
        node.WsAllianceControl = fields[10].GetUInt32();
        node.WsAllianceAssault = fields[11].GetUInt32();
        node.WsHordeControl = fields[12].GetUInt32();
        node.WsHordeAssault = fields[13].GetUInt32();
        node.WsAlive = fields[14].GetUInt32();
        node.Requires = BgRules::ParseRequires(fields[15].GetString());
        node.Weights[0] = fields[16].GetFloat();
        node.Weights[1] = fields[17].GetFloat();
        node.MinGroup = fields[18].GetUInt8();
        node.RallyKey = fields[19].GetString();
        node.LeashRadius = fields[20].GetFloat();
        node.Aliases = fields[21].GetString();
        def->Nodes.push_back(std::move(node));
        ++count;
    } while (result->NextRow());

    for (MapDef& def : Maps)
    {
        if (def.Kind != MapKind::Valley)
            continue;
        auto find = [&def](std::string const& key) -> int
        {
            for (size_t i = 0; i < def.Nodes.size(); ++i)
                if (def.Nodes[i].Key == key)
                    return int(i);
            return -1;
        };
        for (NodeDef& node : def.Nodes)
        {
            if (!node.RallyKey.empty())
            {
                int rally = find(node.RallyKey);
                if (rally >= 0 && def.Nodes[rally].Kind == NodeKind::Rally)
                    node.Rally = int8(rally);
                else
                    TC_LOG_ERROR("server.playerbots", "Playerbots: playerbots_bg_node {} {}: rally '{}' is not a rally row", def.MapId, node.Key, node.RallyKey);
            }
            if (node.IsBoss() && node.Rally < 0)
                TC_LOG_ERROR("server.playerbots", "Playerbots: playerbots_bg_node {} {}: a boss without a rally row is never pushed", def.MapId, node.Key);
            for (std::string const& key : node.Requires.Keys)
                if (find(key) < 0)
                    TC_LOG_ERROR("server.playerbots", "Playerbots: playerbots_bg_node {} {}: requires the unknown node '{}'", def.MapId, node.Key, key);
        }
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: loaded {} battleground objective rows (playerbots_bg_node)", count);
    return count;
}

uint32 OpenSpellForLock(uint32 lockId)
{
    switch (lockId)
    {
        case 2178: return 156098;   // "Capturing", 6 s: type 42 capture points, the Eye of the Storm flag
        case 1479: return 21651;    // "Opening", 8 s: Alterac Valley banners (phase 3)
        default: return 0;
    }
}
}
