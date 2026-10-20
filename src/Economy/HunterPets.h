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


#ifndef PlayerbotsHunterPets_h__
#define PlayerbotsHunterPets_h__

class Player;

namespace Playerbots::HunterPets
{
/// A hunter of level 10 or more with no hunter pet in its stable gets a tamed beast of its own level, summoned beside it
/// (what the core does for a created tamed pet). Cheap when there is nothing to do. Returns true when a pet was granted.
bool Grant(Player* bot);
}

#endif
