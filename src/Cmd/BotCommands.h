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

#ifndef Playerbots_BotCommands_h__
#define Playerbots_BotCommands_h__

#include <string>

class PlayerbotAI;

// Owner commands about the bot itself, registered by ChatCommands.cpp
namespace Playerbots
{
/// spec [name]: the specializations of the class, or switch to one by name
bool SpecCommand(PlayerbotAI& ai, std::string const& param);
/// maintenance: repair and food, behind Playerbots.Command.Maintenance
bool MaintenanceCommand(PlayerbotAI& ai, std::string const& param);
/// hire: a random bot in the party joins the master's account for gold (Playerbots.Hire.Enable)
bool HireCommand(PlayerbotAI& ai, std::string const& param);
/// cs [name [trigger > action ...]]: custom strategies, see CustomStrategies
bool CustomStrategyCommand(PlayerbotAI& ai, std::string const& param);
}

#endif
