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
#include "CharacterPackets.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"

namespace Playerbots
{
WorldSession* CreateBotSession(uint32 accountId, std::string accountName, uint32 bnetAccountId, std::string email, uint8 expansion,
    std::string os, Minutes timezoneOffset, uint32 build, ClientBuild::VariantId variant, LocaleConstant locale)
{
    WorldSession* session = new WorldSession(accountId, std::move(accountName), bnetAccountId, std::move(email), nullptr, SEC_PLAYER, expansion,
        0, std::move(os), timezoneOffset, build, variant, locale, 0, false);

    session->LoadPermissions();
    session->InitializeSession();
    return session;
}

bool UpdateBotSession(WorldSession* session, uint32 diff)
{
    WorldSessionFilter filter(session);

    // IsConnectionIdle() is false while the session counts as queued, whatever its timeout is
    session->SetInQueue(true);
    session->Update(diff, filter);
    session->SetInQueue(false);

    // With a player this pushes the idle timeout. Without one it does nothing, so the timeout stays at 0 (expired)
    // until InitializeSessionCallback sets a future one: that is how a finished initialisation shows
    session->ResetTimeOutTime(true);
    bool ready = !session->IsConnectionIdle();

    // The map updates the sessions of its players too, from its own thread and between two calls of this function.
    // Once the account data is there the session stays "queued", so the idle check can never reach the null socket
    if (ready)
        session->SetInQueue(true);
    return ready;
}

bool StartBotLogin(WorldSession* session, ObjectGuid character)
{
    // The stock handler stores the character as "loading" first and then checks it against the character list the client
    // asked for before (never asked here, so it rejects it with a log line and returns). HandleContinuePlayerLogin is
    // what a client triggers after reconnecting to the instance socket: it only needs the "loading" state, and the
    // account is checked again in Player::LoadFromDB. Both are public handlers of the stock WorldSession
    WorldPackets::Character::PlayerLogin login{ WorldPacket(CMSG_PLAYER_LOGIN) };
    login.Guid = character;
    session->HandlePlayerLoginOpcode(login);
    if (!session->PlayerLoading())
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: the stock login handler did not accept {} (WorldSession::HandlePlayerLoginOpcode changed?)", character.ToString());
        return false;
    }

    session->HandleContinuePlayerLogin();
    return session->PlayerLoading();
}

void RepairOnlineFlags(uint32 accountId, ObjectGuid master)
{
    LoginDatabase.PExecute("UPDATE account SET online = 1 WHERE id = {};", accountId);
    CharacterDatabase.PExecute("UPDATE characters SET online = 1 WHERE guid = {};", master.GetCounter());
}
}
