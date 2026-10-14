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

#ifndef PlayerbotsDangerMap_h__
#define PlayerbotsDangerMap_h__

#include "Define.h"
#include <unordered_map>

namespace Playerbots
{
/// How dangerous the open world is for a walking bot: per map and 128 yard cell, the highest level of a creature
/// that attacks each team (civilians, critters, triggers and creatures of other phases do not count). The level of a
/// creature is the lowest level its content tuning gives it, so a scaling zone only counts from the level it starts at.
/// Built once at startup from the creature spawns, read-only afterwards.
class DangerMap
{
public:
    static DangerMap* instance();

    void Build();
    bool IsBuilt() const { return _built; }

    /// Highest level near a point for a team (1 Alliance, 2 Horde): the cell of the point and its neighbours
    uint8 LevelNear(uint32 mapId, float x, float y, uint8 team) const;
    /// The highest along a straight stretch, sampled every 64 yards
    uint8 LevelAlong(uint32 mapId, float x1, float y1, float x2, float y2, uint8 team) const;

private:
    struct Cell
    {
        uint8 Alliance = 0;
        uint8 Horde = 0;
    };

    static int64 Key(int32 cx, int32 cy) { return (int64(cx) << 32) ^ int64(uint32(cy)); }

    std::unordered_map<uint32, std::unordered_map<int64, Cell>> _maps;
    bool _built = false;
};
}

#define sDangerMap Playerbots::DangerMap::instance()

#endif // PlayerbotsDangerMap_h__
