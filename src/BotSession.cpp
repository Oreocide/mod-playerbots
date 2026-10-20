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

#include "BotSession.h"
#include "PlayerbotsConfig.h"
#include "WorldPacket.h"

namespace Playerbots
{
WorldSession* CreateBotSession(uint32 accountId, std::string accountName, uint32 bnetAccountId, std::string email, uint8 expansion,
    std::string os, Minutes timezoneOffset, uint32 build, ClientBuild::VariantId variant, LocaleConstant locale,
    BotPacketSink packetSink, std::function<void(WorldSession*)> onInitialized)
{
    WorldSession* session = new WorldSession(accountId, std::move(accountName), bnetAccountId, std::move(email), nullptr, SEC_PLAYER, expansion,
        0, std::move(os), timezoneOffset, build, variant, locale, 0, false);

    session->SetHeadless(std::move(packetSink));
    session->SetHeadlessLoadsVisibilityGrids(!sPlayerbotsConfig.NoVisibilityGridLoad);
    session->LoadPermissions();
    session->InitializeSession([session, onInitialized = std::move(onInitialized)]
    {
        if (onInitialized)
            onInitialized(session);
    });
    return session;
}
}
