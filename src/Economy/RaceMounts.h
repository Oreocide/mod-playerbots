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

#ifndef PlayerbotsRaceMounts_h__
#define PlayerbotsRaceMounts_h__

#include "Define.h"
#include <span>

class Player;

namespace Playerbots::RaceMounts
{
/// The ground mounts a character of this race rides: the race's own vendor mounts (a goblin rides a trike, a dwarf a ram),
/// allied races use their cousin race's, anything else the basic mounts of its faction
std::span<uint32 const> For(uint8 race);

/// The mount a random bot rides: its race's list rotated by the guid, the first one it knows. 0 = none known.
/// Never any other known mount: collections are account wide, so a Horde bot on a mixed account knows the Alliance horse
uint32 PickKnown(Player const* bot);

/// Riding skill by level (apprentice from 10, journeyman from 20), and from Rpg.MountMinLevel one mount of the race
/// in the collection. Cheap when there is nothing to do.
void Grant(Player* bot);

/// Logs list entries that are not ground mounts in the client data (startup check)
void Validate();
}

#endif
