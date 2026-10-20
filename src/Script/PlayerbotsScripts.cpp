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

#include "ActivityMgr.h"
#include "BotSession.h"
#include "Strategies/Rpg/Ambient.h"
#include "Chat.h"
#include "Group.h"
#include "Player.h"
#include "Cmd/ChatCommands.h"
#include "Social/WhisperFallback.h"
#include "DB2Stores.h"
#include "StringFormat.h"
#include "Db/PlayerbotRepository.h"
#include "Goal/GoalStore.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "Perf/BotPerfReporter.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "RandomBotMgr.h"
#include "RemoteControl.h"
#include "Rotation/RotationMgr.h"
#include "Social/BotSocialMgr.h"
#include "Travel/HotspotMgr.h"
#include "World/WorldActionQueue.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "GameTime.h"
#include "Util.h"
#include "WorldSession.h"
#include <sstream>

using namespace Playerbots;

class playerbots_world : public WorldScript
{
public:
    playerbots_world() : WorldScript("playerbots_world")
    {
        // OnConfigLoad(false) runs before scripts are registered
        sPlayerbotsConfig.Load();
    }

    // Data the module reads once, after the databases are loaded. This hook went missing in 60ee8ad (the repository, the hotspot
    // marker templates and the rotation scripts were never loaded afterwards: the repository answers nothing before its Load)
    void OnStartup() override
    {
        sPlayerbotRepository->Load();
        sHotspotMgr->LoadMarkerTemplates();
        if (sPlayerbotsConfig.RotationEnable)
            sRotationMgr->Load();
        sRemoteControl->Start();
    }

    void OnConfigLoad(bool reload) override
    {
        sPlayerbotsConfig.Load();
        if (reload && sPlayerbotsConfig.GoalEnable)
            GoalStore::Load();
    }

    void OnUpdate(uint32 diff) override
    {
        static bool goalsLoaded = false;
        if (!goalsLoaded)
        {
            goalsLoaded = true;
            if (sPlayerbotsConfig.GoalEnable)
                GoalStore::Load();
        }

        sWorldActionQueue->Drain();
        sPlayerbotMgr->TickOtherMapBots(diff);
        sRemoteControl->Update();
        sPlayerbotMgr->NoteWorldTick(diff);
        sRandomBotMgr->Update(diff);
        sPlayerbotMgr->Update(diff);
        sActivityMgr->Update(diff);
        sBotSocialMgr->Update(diff);
        sBotPerfReporter->Update(diff);
    }

    void OnShutdown() override
    {
        sRemoteControl->Stop();
        sActivityMgr->Shutdown();
        sRandomBotMgr->Shutdown();
        sPlayerbotMgr->LogoutAll();
    }
};

class playerbots_player : public PlayerScript
{
public:
    playerbots_player() : PlayerScript("playerbots_player") { }

    // game masters are told how many random bots the server is going to log in
    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        WorldSession* session = player->GetSession();
        if (IsBotSession(session) || session->GetSecurity() < SEC_GAMEMASTER)
            return;

        if (!sPlayerbotsConfig.Enabled || !sPlayerbotsConfig.RandomEnabled)
            return;

        ChatHandler handler(session);
        size_t ready = sRandomBotMgr->GetCharacterCount();
        size_t online = sPlayerbotMgr->GetRandomBotCount();
        uint32 target = sPlayerbotsConfig.RandomCount;

        handler.PSendSysMessage("|cff00ff00Playerbots:|r %u random bots will be online (%u are online now, %u characters ready%s).",
            target, uint32(online), uint32(ready), ready < target ? ", the rest are created over time" : "");

        if (sPlayerbotsConfig.RandomOnlyWithPlayers && online == 0)
            handler.PSendSysMessage("|cff00ff00Playerbots:|r the first bots log in %u seconds after the first player, %u every %u ms.",
                uint32(sPlayerbotsConfig.RandomPlayerLoginDelay), uint32(sPlayerbotsConfig.RandomLoginBatch), uint32(sPlayerbotsConfig.RandomLoginDelay));

        if (sPlayerbotsConfig.RandomMaxGrids)
            handler.PSendSysMessage("|cff00ff00Playerbots:|r the bots share at most %u map grids (%s).",
                uint32(sPlayerbotsConfig.RandomMaxGrids), sPlayerbotsConfig.RandomMaps.empty() ? "all maps" : ("maps " + sPlayerbotsConfig.RandomMaps).c_str());
    }

    void OnTextEmote(Player* player, uint32 textEmote, uint32 /*emoteNum*/, ObjectGuid /*guid*/) override
    {
        Ambient::ReturnTextEmote(player, textEmote, GameTime::GetGameTimeMS());
    }

    void OnLogout(Player* player) override
    {
        if (!IsBot(player))
            sPlayerbotMgr->RemoveBotsOf(player->GetGUID());
    }

    // whisper from the master to one of its bots
    void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Player* receiver) override
    {
        if (!receiver || !IsBot(receiver))
            return;

        PlayerbotAI* ai = sPlayerbotMgr->GetAI(receiver);
        if (!ai || IsBot(player))
            return;

        // a random bot that does not follow this player only tells how to get it to listen
        if (ai->GetMasterGuid() != player->GetGUID())
        {
            if (!CallWhisperFallback(receiver, player, msg))
                sBotSocialMgr->OnWhisper(receiver, player);
            return;
        }

        // the AI is gone after a leave, remember the guid first
        ObjectGuid bot = receiver->GetGUID();
        if (HandleBotChat(*ai, player, msg, false).Leave)
            sPlayerbotMgr->RemoveBot(bot);
    }

    // party chat of a player is a command for the bots of that group that follow this player
    void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        if (IsBot(player))
            return;

        std::vector<ObjectGuid> leaving;
        for (GroupReference const& ref : group->GetMembers())
        {
            Player* member = ref.GetSource();
            if (!member || member == player || !IsBot(member))
                continue;

            PlayerbotAI* ai = sPlayerbotMgr->GetAI(member);
            if (!ai || ai->GetMasterGuid() != player->GetGUID())
                continue;

            if (HandleBotChat(*ai, player, msg, true).Leave)
                leaving.push_back(member->GetGUID());
        }

        for (ObjectGuid bot : leaving)
            sPlayerbotMgr->RemoveBot(bot);
    }
};

// The AI of the bots on a world map runs from that map's update, on the map's thread. Dungeons and battlegrounds cannot
// get a map script of their own (theirs are bound to the database), their bots are ticked from the world update.
class playerbots_map : public WorldMapScript
{
public:
    explicit playerbots_map(uint32 mapId) : WorldMapScript(Trinity::StringFormat("playerbots_map_{}", mapId).c_str(), mapId) { }

    void OnUpdate(Map* map, uint32 diff) override
    {
        sPlayerbotMgr->TickMapBots(map, diff);
    }
};

void AddSC_playerbots_scripts()
{
    for (MapEntry const* entry : sMapStore)
        if (entry->IsWorldMap())
            new playerbots_map(entry->ID);

    new playerbots_world();
    new playerbots_player();
}
