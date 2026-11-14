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

#include "Economy/BotGathering.h"
#include "Loot/BotLoot.h"
#include "Quest/PartyQuests.h"
#include "Quest/QuestItems.h"
#include "Travel/BotTransports.h"
#include "ScriptMgr.h"

// Scripts of the AC parity groups (mod-playerbots#38): their config keys and the hooks they listen to
namespace
{
class playerbots_acparity_world : public WorldScript
{
public:
    playerbots_acparity_world() : WorldScript("playerbots_acparity_world") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        Playerbots::BotLooting::LoadConfig();
        Playerbots::PartyQuests::LoadConfig();
        Playerbots::QuestItems::LoadConfig();
        Playerbots::BotTransports::LoadConfig();
        Playerbots::BotGathering::LoadConfig();
    }

    void OnStartup() override
    {
        Playerbots::BotLooting::OnStartup();
        Playerbots::BotGathering::OnStartup();
    }
};

class playerbots_acparity_player : public PlayerScript
{
public:
    playerbots_acparity_player() : PlayerScript("playerbots_acparity_player") { }

    void OnQuestStatusChange(Player* player, uint32 questId) override
    {
        Playerbots::PartyQuests::OnQuestStatusChange(player, questId);
    }
};
}

void AddSC_playerbots_acparity()
{
    new playerbots_acparity_world();
    new playerbots_acparity_player();
}
