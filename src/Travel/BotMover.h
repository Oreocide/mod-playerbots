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

#ifndef Playerbots_BotMover_h__
#define Playerbots_BotMover_h__

#include "Position.h"
#include "Travel/MovePriority.h"
#include "Travel/MoveToward.h"

class Player;
class Unit;

// BotMover: the one way a bot moves under its own decision (design: docs/design/FEASIBILITY.md section d, BEHAVIORS.md
// "Movement"). Every launched move is a navmesh path (PathGenerator NORMAL, or INCOMPLETE followed in segments); never a raw
// point, never a straight spline, never a Z-snap teleport.
//   * travel mode  = MoveAlongMesh (MoveToward.h): walking routes and rpg destinations, mounts, 5 yd arrival, 5 yd of progress per
//                    segment, never cuts into a running walk.
//   * combat mode  = MoveCombat (below): short repositioning moves during a fight (behind the target, spread, step out of a
//                    hazard). Keeps attacking, arrives within 1 yd, may cut into a running walk, obeys MovePriority.
// The core chase and follow generators (MoveChase, MoveFollow) also path through PathGenerator + MovebyPath and are allowed.
namespace Playerbots
{
/// The bot is free to start a move: alive, in the world, not rooted/stunned/confused/feared/distracted/in flight/charmed,
/// not polymorphed, not being teleported.
bool CanMove(Player const* bot);

/// Snaps a wanted standing point to the navmesh near the bot: path with forceDest = false and take GetActualEndPosition().
/// False if there is no walkable path, or the mesh end is more than maxXY yards (2D) or maxZ yards (height) from the wanted
/// point. For points within a path's reach (~300 yd); farther points come back false (INCOMPLETE end is far from the target).
/// Never invents a Z from VMAP/height queries. Gap G9 workaround.
bool SnapToMesh(Player* bot, Position const& wanted, Position& out, float maxXY = 3.0f, float maxZ = 2.5f);

/// Combat mode move to dest along the mesh. Moving: a path is running or was just launched, or the bot cannot move right now
/// (retry next tick). Arrived: within arriveYards (and 2.5 yd in height). NoPath: the mesh has no walkable path (or no
/// progress); pick another point. A lower `prio` than the running move's is not applied (Moving).
MoveResult MoveCombat(Player* bot, Position const& dest, float arriveYards = 1.0f, MovePriority prio = MovePriority::Position);

/// A running move of higher priority than `prio` holds the bot: the core chase/follow generators must not be re-issued over it
bool MoveHeldAbove(Player const* bot, MovePriority prio);

/// Forgets the held move (the bot was interrupted, teleported or stopped by something else)
void ClearMoveState(Player* bot);
}

#endif
