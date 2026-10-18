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

#ifndef PlayerbotsWhisperFallback_h__
#define PlayerbotsWhisperFallback_h__

#include <functional>
#include <string>

class Player;

namespace Playerbots
{
/// Another module (an LLM chat module) can take over whispers to bots that are not playerbots commands.
/// The fallback runs on the world thread, where the whisper of a player is processed. It returns true when the whisper
/// is dealt with, then playerbots does not answer it ("Unknown command", or the hint random bots give).
/// Set it once while the scripts are registered (AddSC_*), it is read-only afterwards and needs no locking.
using WhisperFallback = std::function<bool(Player* bot, Player* from, std::string const& text)>;

void SetWhisperFallback(WhisperFallback fallback);

/// False when no fallback is set or it did not take the whisper.
bool CallWhisperFallback(Player* bot, Player* from, std::string const& text);
}

#endif
