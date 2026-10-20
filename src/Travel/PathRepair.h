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

#ifndef PlayerbotsPathRepair_h__
#define PlayerbotsPathRepair_h__

#include "MoveSplineInitArgs.h"
#include <cstdint>

class Player;

namespace Playerbots
{
/// Playerbots.Move.AvoidBlockedPaths: the mesh does not know game objects (a Hero's Call board, doodads) or small models, so a path can run
/// straight through them. For the first segment that VMAP or a game object blocks at eye height, a detour is looked for: points beside the
/// middle of the segment (3, 6 and 10 yards to either side), each reached and left again by mesh paths of their own that are clear. Every
/// point of the new path is still a navmesh point (no bee-line). Up to three segments are repaired per path.
/// Returns true if the path was changed; a path that could not be repaired is left as it was.
bool RepairBlockedPath(Player* bot, Movement::PointsArray& points);

/// How low a blocked segment is: Low = something between knee and waist height on level ground (a curb, a low fence, a crate: a hop clears
/// it), Tall = blocks at waist height (a hop would not, go round), Clear otherwise. A rise along the segment means stairs or a slope: Clear
enum class SegmentKind : uint8_t { Clear, Low, Tall };
SegmentKind ClassifySegment(Player* bot, G3D::Vector3 const& a, G3D::Vector3 const& b);

/// The first Low segment of a path, or points.size() if there is none. Stops at a Tall one (that is for RepairBlockedPath)
size_t FirstLowSegment(Player* bot, Movement::PointsArray const& points);

/// A segment blocked at eye height, its ends pulled in 0.3 yd so a wall corner that only touches the end does not count
bool SegmentBlockedAtEye(Player* bot, G3D::Vector3 const& a, G3D::Vector3 const& b);
}

#endif
