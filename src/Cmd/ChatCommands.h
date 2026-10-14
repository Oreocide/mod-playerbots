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

#ifndef Playerbots_ChatCommands_h__
#define Playerbots_ChatCommands_h__

#include "ChatParser.h"
#include <string>

class Player;

namespace Playerbots
{
class PlayerbotAI;

/// The commands a master can give a bot in chat, filled by EnsureAiRegistered()
ChatCommandTable& GetChatCommands();
void RegisterChatCommands();

struct ChatOutcome
{
    bool Known = false;   // at least one command of the message was understood
    bool Leave = false;   // the bot was asked to log out, the caller does it (the AI is gone afterwards)
};

/// A whisper (group = false) or party chat (group = true) of `speaker`, already known to be allowed to command
/// this bot. Runs the chain of commands that pass the @filters for this bot and answers the speaker
ChatOutcome HandleBotChat(PlayerbotAI& ai, Player* speaker, std::string text, bool group);
}

#endif // Playerbots_ChatCommands_h__
