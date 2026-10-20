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

#ifndef PlayerbotsMountVariety_h__
#define PlayerbotsMountVariety_h__

#include "Define.h"
#include <vector>

class Player;

namespace Playerbots::MountVariety
{
/// How many ground mounts a bot of this level owns: none before the riding skill, then 1 at 20, 2 at 40, 3 at 60, 4 at 80, 6 at 90.
/// A player of that level who has played a while: the race's vendor mount first, then whatever came with quests, vendors, professions
/// and drops (higher levels draw from all four, the lower ones only from quests and vendors)
uint32 CountForLevel(uint32 level);

/// The mount spells this bot owns (ground mounts from Mount.db2 that its race, class and faction may use, chosen by a hash of its guid so
/// two bots rarely own the same ones). Cached per bot until its tier changes. World or map thread
std::vector<uint32> AssignedFor(Player const* bot);

/// Teaches the bot the mounts of AssignedFor (per character, not the account collection)
void Grant(Player* bot);

/// A random one of the mounts the bot owns, 0 if none: a player does not ride the same mount every time
uint32 PickRide(Player const* bot);
}

#endif
