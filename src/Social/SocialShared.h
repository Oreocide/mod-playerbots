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

#ifndef Playerbots_SocialShared_h__
#define Playerbots_SocialShared_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <string_view>

class Channel;
class Group;
class Player;
enum DuelCompleteType : uint8;

// Internal glue between the files of the social group (src/Social/Social*.cpp). Nothing outside src/Social includes it.
namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;
}

namespace Playerbots::Social
{
using SocialCommandFn = bool (*)(PlayerbotAI& ai, std::string const& param);

/// Registers a chat order the way Cmd/ChatCommands.cpp AddCommand does (action "cmd <name>" + the command table)
void AddSocialCommand(char const* name, char const* help, SocialCommandFn fn, bool whisperOnly);

bool InCapital(Player const* player);
/// A real player: in the world and not a bot session
bool IsHuman(Player const* player);
/// "tank", "healer" or "dps"
std::string RoleWord(Player const* player);
std::string ZoneName(Player const* player);
/// A random bot (no master character) that no player's party has picked up
bool IsFreeRandomBot(PlayerbotAI& ai);

// per feature registration (actions, triggers, orders), each in its own file
void RegisterChannels(AiRegistry& registry);
void RegisterInvites(AiRegistry& registry);
void RegisterDuels(AiRegistry& registry);
void RegisterWarMode(AiRegistry& registry);
void RegisterTrades(AiRegistry& registry);
void RegisterBanking(AiRegistry& registry);
void RegisterMailOrders(AiRegistry& registry);
void RegisterMageRefreshments(AiRegistry& registry);

// hooks, called by the scripts in SocialAi.cpp
void OnPlayerChannelChat(Player* player, std::string const& message, Channel* channel);   // world thread
void UpdateChannels(uint32 diff);                                                       // world thread
void OnGroupMemberAdded(Group* group, ObjectGuid guid);                                 // world thread
void OnDuelStarted(Player* first, Player* second);                                      // the duelists' map thread
void OnDuelEnded(Player* winner, Player* loser, DuelCompleteType type);                  // the duelists' map thread
}

#endif // Playerbots_SocialShared_h__
