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

#ifndef Playerbots_SocialRephrase_h__
#define Playerbots_SocialRephrase_h__

#include "ObjectGuid.h"
#include <functional>
#include <string>
#include <string_view>

// Hand-off point for another module (mod-ollama) to rephrase a bot's public channel line before it is posted, like
// AzerothCore's g_BroadcastRephraseHook. Without a hook the template line goes out as it is.
namespace Playerbots::Social
{
/// Posts the final text for the bot (any thread; the post itself runs on the world thread). An empty text drops the line.
using ChannelDeliver = std::function<void(std::string text)>;

/// category: the AzerothCore text group of the line ("suggest_quest", "suggest_sell", "response_lfg_quests_channel"...).
/// The hook must call deliver exactly once, now or later, from any thread.
using ChannelRephraseHook = void (*)(ObjectGuid bot, std::string_view category, std::string const& text, ChannelDeliver deliver);

void SetChannelRephraseHook(ChannelRephraseHook hook);
ChannelRephraseHook GetChannelRephraseHook();
}

#endif // Playerbots_SocialRephrase_h__
