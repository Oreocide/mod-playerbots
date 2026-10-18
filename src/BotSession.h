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

#ifndef PlayerbotsBotSession_h__
#define PlayerbotsBotSession_h__

#include "ClientBuildInfo.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "WorldSession.h"
#include <string>

/// Bot sessions on a stock WorldSession: no core patch, only public API.
///
/// A bot session is a WorldSession built without a socket. Everything a client would normally drive is driven here:
///  - Packets sent to the bot are dropped by WorldSession::SendPacket ("Prevented sending ... to non existent socket"
///    is logged at ERROR level for each; mute it with `Logger.network.opcode` in worldserver.conf). The module
///    never needs them: invites, dungeon finder proposals and character creation results are polled from game state.
///  - The idle check at the top of WorldSession::Update dereferences the realm socket once the idle timeout passed, so
///    the queue flag (which makes IsConnectionIdle() false) is set around every Update.
///  - WorldSession::Update returns false for a session without socket. The result is meaningless here.
namespace Playerbots
{
/// A session of a real client always has a remote address, a bot session never does (WorldSession::m_Address is only
/// set from the realm socket). Immutable after construction, so it is safe from every thread
inline bool IsBotSession(WorldSession const* session)
{
    return session && session->GetRemoteAddress().empty();
}

inline bool IsBot(Player const* player)
{
    return player && IsBotSession(player->GetSession());
}

/// Builds a socketless session and starts loading its account data (WorldSession::InitializeSession).
/// Account data is ready once UpdateBotSession returned true
WorldSession* CreateBotSession(uint32 accountId, std::string accountName, uint32 bnetAccountId, std::string email, uint8 expansion,
    std::string os, Minutes timezoneOffset, uint32 build, ClientBuild::VariantId variant, LocaleConstant locale);

/// Runs WorldSession::Update. Returns true once the account data of the session is loaded.
/// World thread only
bool UpdateBotSession(WorldSession* session, uint32 diff);

/// Starts the login of a character of the session's account with the stock handlers (character select -> enter world).
/// Only after UpdateBotSession returned true. False if the stock login handler did not accept the request
bool StartBotLogin(WorldSession* session, ObjectGuid character);

/// Deleting a session logs its player out and saves it, which sets every character of the account offline. Bots of a
/// player share the player's account, so those flags are set again a moment later. `master` is the player that
/// stays online (empty for random bots, which have an account of their own)
void RepairOnlineFlags(uint32 accountId, ObjectGuid master);
}

#endif // PlayerbotsBotSession_h__
