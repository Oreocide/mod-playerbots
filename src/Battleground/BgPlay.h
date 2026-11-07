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

#ifndef Playerbots_BgPlay_h__
#define Playerbots_BgPlay_h__

// The bot side of the battleground team planner: an ownerless bot plays the job the planner gave it (BgPlanner.h). Called from the
// "pvp" action (Strategies/GenericStrategies.cpp PlayObjective) instead of the pilot objective code while Playerbots.Bg.Planner plays
// the map; with the key off nothing here runs

class Player;

namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;

namespace Bg
{
/// The planner plays this bot's battleground (Playerbots.Bg.Objectives, Playerbots.Bg.Planner, the map in Bg.Planner.Maps)
bool Plays(Player const* bot);
/// One step of the bot's job. False: nothing to do here, the pvp action chases the nearest enemy instead
bool Play(Player* bot, PlayerbotAI& ai);
/// The "bg leash" trigger and action (Alterac Valley boss pushes: nobody chases past the boss's leash, a wipe walks back to the
/// rally point together); the "pvp" strategy offers them in combat above the attack action
void Register(AiRegistry& registry);
}
}

#endif
