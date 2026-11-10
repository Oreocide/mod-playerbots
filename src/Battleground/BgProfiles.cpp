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

#include "Battleground/BgMode.h"

namespace Playerbots::Bg
{
// The coordinator's battlegrounds (docs/design/BG_COORDINATOR.md §4, §6). Alterac Valley comes with the Lane mode (W6) and its
// profile (W7); every other map stays on the planner's old path (backlog B1-B6)
Profile const* FindProfile(uint32 /*mapId*/)
{
    return nullptr;
}
}
