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

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <unordered_set>

class Player;

class WorldLocation;

namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;
}

/// Party bots ride along on the master's boat, zeppelin, elevator or tram (AC parity, mod-playerbots#38; GAPS-bots G45).
/// A boat or zeppelin is boarded only while it waits at the dock. The bot walks the mesh to the dock edge, then takes the one
/// allowed straight step onto the deck, and joins the transport as a passenger (the clients are told with a movement update,
/// since a bot has no client to send one). Aboard it stands still; when the master steps off at a stop, the bot leaves the
/// transport and its follow walks on. The "board" order does the boarding at once (the spike test). Far bots never board.
///
/// A random bot on a route over a boat or zeppelin link (Playerbots.Transport.RideAlone) rides it for real while a human can
/// see it: it waits at the dock, steps aboard when the transport stops there, and steps off at the far dock. Far bots, and any
/// bot the transport is not found for, keep the route's dock wait (Travel/RouteFollower StepTransport).
namespace Playerbots::BotTransports
{
struct Config
{
    bool Board = false;     // Playerbots.Transport.Board
    bool RideAlone = false; // Playerbots.Transport.RideAlone
    std::unordered_set<std::string> RideTestBots;    // Playerbots.Transport.RideTestBots: lowercase names that ride as if a human were near
};

Config const& GetConfig();
void LoadConfig();
/// World thread, after the transports are created: which transport spawns carry each transport entry
void OnStartup();

enum class RideStep : uint8
{
    NotMine,    // not ridden for real: the caller keeps its dock wait
    Running,
    Arrived     // stepped off at the far dock
};

/// RouteFollower::StepTransport for a bot at the dock `from` (or already riding): ride the transport `entry` to the dock `to`.
/// maxMs bounds the wait at the dock and, three times over, the ride
RideStep RideAlone(PlayerbotAI& ai, uint32 entry, WorldLocation const& from, WorldLocation const& to, uint32 maxMs);
bool IsRiding(ObjectGuid bot);
/// ~PlayerbotAI: a bot that logs out forgets its ride
void ForgetRide(ObjectGuid bot);
/// On Playerbots.Transport.RideTestBots: the population rules leave it where a test put it (RandomBotMgr relocation)
bool IsRideTestBot(Player const* bot);

void Register(AiRegistry& registry);
}

#endif
