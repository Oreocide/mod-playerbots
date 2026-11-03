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

namespace Playerbots::Bg
{
namespace
{
// entries from the battleground scripts (battleground_arathi_basin.cpp:116-120, battleground_battle_for_gilneas.cpp:31-33,
// battleground_eye_of_the_storm.cpp:93-96, battleground_twin_peaks.cpp:50-51); the win scores from the same scripts
std::vector<MapDef> const Maps =
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
};
}

MapDef const* FindMap(uint32 mapId)
{
    for (MapDef const& def : Maps)
        if (def.MapId == mapId)
            return &def;
    return nullptr;
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
