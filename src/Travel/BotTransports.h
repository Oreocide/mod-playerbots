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

#ifndef Playerbots_BotTransports_h__
#define Playerbots_BotTransports_h__

namespace Playerbots
{
class AiRegistry;
}

/// Party bots ride along on the master's boat, zeppelin, elevator or tram (AC parity, mod-playerbots#38; GAPS-bots G45).
/// A boat or zeppelin is boarded only while it waits at the dock. The bot walks the mesh to the dock edge, then takes the one
/// allowed straight step onto the deck, and joins the transport as a passenger (the clients are told with a movement update,
/// since a bot has no client to send one). Aboard it stands still; when the master steps off at a stop, the bot leaves the
/// transport and its follow walks on. The "board" order does the boarding at once (the spike test). Far bots never board.
namespace Playerbots::BotTransports
{
struct Config
{
    bool Board = false;     // Playerbots.Transport.Board
};

Config const& GetConfig();
void LoadConfig();
void Register(AiRegistry& registry);
}

#endif
