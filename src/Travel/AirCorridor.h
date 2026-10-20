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

#ifndef PlayerbotsAirCorridor_h__
#define PlayerbotsAirCorridor_h__

#include "Define.h"
#include "Position.h"
#include <string>
#include <vector>

class Player;

namespace Playerbots
{
/// The one place the module leaves the navmesh (user decision 2026-10-19, air legs only): a straight line between two navmesh-snapped
/// points, flown at a height that is checked against the terrain and the buildings under it. Everything about the line is read from the
/// map before the bot leaves the ground; a line that does not check out is never flown.
namespace AirCorridor
{
struct Plan
{
    bool Ok = false;
    bool Transient = false;          // refused for something about the moment (grids not loaded), not about the terrain: not cached, never blocks the link
    std::string Why;
    float CruiseZ = 0.0f;
    std::vector<Position> Path;      // takeoff point, the points of the profile every few tens of yards, landing point
};

/// Plans the leg on the bot's own map thread. The terrain is probed from far above the highest of the two ends (grid terrain only counts
/// when the probe starts above it, and a probe from the bot's height would read a ridge as clear), every sample must be in an area that
/// allows flying mounts, and at most Playerbots.Air.MaxGridLoads unloaded grids are loaded for it. A static answer (flyable or not)
/// is remembered per pair of ends.
Plan PlanLeg(Player* bot, Position const& from, Position const& to);

/// Forgets the remembered answers (config reload, tests)
void ClearCache();
}
}

#endif
