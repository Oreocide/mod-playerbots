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

#ifndef PlayerbotsNavSnap_h__
#define PlayerbotsNavSnap_h__

#include "Position.h"

class Map;
class Player;

namespace Playerbots
{
/// The nearest point of the ground navmesh to a wanted point, without a path from anywhere (BotMover::SnapToMesh needs one, and a landing
/// point is hundreds of yards from the bot). Only for air legs: the takeoff and the landing of a flight are always navmesh points.
/// False if the navmesh of the map is not loaded there or no walkable polygon is within horizontal / vertical yards.
bool SnapPointToNavMesh(Player const* bot, Position const& wanted, Position& out, float horizontal = 4.0f, float vertical = 6.0f);
}

#endif
