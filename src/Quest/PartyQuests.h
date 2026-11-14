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

#ifndef Playerbots_PartyQuests_h__
#define Playerbots_PartyQuests_h__

#include "Define.h"

class Player;

namespace Playerbots
{
class AiRegistry;
}

/// Quests in a party (AC parity, mod-playerbots#38; AC: SyncQuestWithPlayer, auto share quest, "share", "reward").
/// QuestTravel stays out of a bot with a master (QuestTravel.cpp: relevance 0 with a master or a group), so without this a party
/// bot never takes the master's quests and never hands in the ones shared with it.
///  * sync: when the master takes a quest from an NPC, party bots near that NPC take it too; when the master hands one in,
///    bots that finished it hand it in there; while the master talks to a quest NPC, bots hand in what they finished for it
///  * share: a bot shares a shareable quest once per party with members near it who can take it
namespace Playerbots::PartyQuests
{
struct Config
{
    bool Sync = false;      // Playerbots.Party.QuestSync
    bool Share = false;     // Playerbots.Party.QuestShare
    float Range = 30.0f;    // Playerbots.Party.QuestRange: how near the quest NPC (or the member to share with) a bot must be
};

Config const& GetConfig();
void LoadConfig();

/// PlayerScript::OnQuestStatusChange for every player: remembers what a human took or handed in, and at which NPC
void OnQuestStatusChange(Player* player, uint32 questId);

/// The "party quests" strategy, its triggers and actions, and the share / reward orders
void Register(AiRegistry& registry);
}

#endif
