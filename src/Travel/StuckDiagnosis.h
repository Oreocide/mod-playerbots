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

#ifndef PlayerbotsStuckDiagnosis_h__
#define PlayerbotsStuckDiagnosis_h__

#include "MoveSplineInitArgs.h"
#include "Position.h"
#include <cstdint>
#include <string>

class Player;

namespace Playerbots
{
/// One line of facts for a stuck log entry (ground snap, fall rescue, a route that made no headway), to tell a gap in the mesh from
/// geometry the mesh does not know about from a floor that is simply not where the path said:
///   los=blocked|clear     line of sight between the reference spot (where the bot last stood well) and where it is now: blocked means
///                         VMAP or a game object stands between them, so a walk from one to the other cannot have been legal
///   ground=<z> dz=<d>     the terrain/VMAP floor under the bot now and how far the bot is from it
///   back=<type>/<ratio>   the mesh path from here back to the reference spot: PathType and path length over straight distance
///                         (a ratio far above 1, or NOPATH, means the mesh does not connect the two the way the bot moved)
///   spline=<state>        what the bot's movement spline is doing, with the line of sight to its end point
///   object=<entry name>   the nearest game object within 8 yards (a gate or door is not in the mesh)
/// reference may be null.
std::string DescribeStuck(Player* bot, Position const* reference);

/// Playerbots.Move.LogBlockedPaths: how a mesh path stands against the geometry the mesh does not know.
///   Steep:    a segment that rises or drops more than SteepMinDz yards at a slope no walker climbs (a cliff the mesh links)
///   EyeBlock: a segment that VMAP or a game object blocks at eye height (a wall, a board, a closed door)
///   KneeOnly: blocked at knee height but clear at eye height (stairs, curbs, a slope: mostly not a real obstacle, counted only)
struct PathVerdict
{
    enum class Kind : uint8_t { Clear, KneeOnly, EyeBlock, Steep };
    Kind Result = Kind::Clear;
    std::string Text;    // segment and positions of the first Steep or EyeBlock segment
};

constexpr float SteepMinDz = 8.0f;
constexpr float SteepSlope = 1.5f;    // rise over run (a walker climbs about 1.2)

PathVerdict JudgePath(Player* bot, Movement::PointsArray const& points);
}

#endif
