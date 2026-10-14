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

#ifndef PlayerbotsBotTalents_h__
#define PlayerbotsBotTalents_h__

class Player;

namespace Playerbots
{
/// Talents for bots: Blizzard's own starter build of the specialization (the loadout the client offers as
/// "Use starter build"), applied through the same code path as that button. It spends the points the bot has at its
/// level, so it is applied again after every level and specialization change.
namespace BotTalents
{
/// true if a build was applied. The bot needs a specialization (level 10 and up) and must be in the world
bool ApplyStarterBuild(Player* bot);
}
}

#endif // PlayerbotsBotTalents_h__
