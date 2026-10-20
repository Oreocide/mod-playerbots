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
#include <functional>
#include <string>

/// Bot sessions are headless sessions of the core (WorldSession::SetHeadless): sessions without client connection that this
/// module drives. Packets the core sends to a bot go to the packet sink given at creation (null = dropped); the module reads
/// the few it needs (dungeon finder proposals, loot rolls, character creation results) there.
namespace Playerbots
{
inline bool IsBotSession(WorldSession const* session)
{
    return session && session->IsHeadless();
}

inline bool IsBot(Player const* player)
{
    return player && IsBotSession(player->GetSession());
}

using BotPacketSink = std::function<void(WorldPacket const&)>;

/// Builds a headless session and starts loading its account data. `onInitialized` runs on the world thread, from the session's
/// Update, once that is done: the place to start a login (WorldSession::LoginHeadlessPlayer) or character creation.
/// `packetSink` can run on any thread
WorldSession* CreateBotSession(uint32 accountId, std::string accountName, uint32 bnetAccountId, std::string email, uint8 expansion,
    std::string os, Minutes timezoneOffset, uint32 build, ClientBuild::VariantId variant, LocaleConstant locale,
    BotPacketSink packetSink, std::function<void(WorldSession*)> onInitialized);
}

#endif // PlayerbotsBotSession_h__
