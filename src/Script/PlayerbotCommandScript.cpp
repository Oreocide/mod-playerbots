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

#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Engine/PerfMonitor.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "RBAC.h"
#include "Rotation/RotationMgr.h"
#include "Travel/HotspotMgr.h"
#include "Travel/TravelMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include <algorithm>

using namespace Trinity::ChatCommands;
using namespace Playerbots;

namespace
{
// custom permission (1000+ range, must stay below 1024 to be a valid RBACPermissions value),
// created and granted to "Role: Player Commands" by data/sql/db-auth
constexpr rbac::RBACPermissions RBAC_PERM_COMMAND_BOT = static_cast<rbac::RBACPermissions>(1010);

char const* GetAddBotError(AddBotResult result)
{
    switch (result)
    {
        case AddBotResult::Disabled: return "Playerbots are disabled.";
        case AddBotResult::NotFound: return "No character with that name.";
        case AddBotResult::IsSelf: return "You can't add yourself.";
        case AddBotResult::WrongAccount: return "Only characters of your own account can be bots.";
        case AddBotResult::AlreadyOnline: return "That character is already online.";
        case AddBotResult::TooManyBots: return "You have reached the bot limit.";
        default: return "";
    }
}
}

class playerbots_commandscript : public CommandScript
{
public:
    playerbots_commandscript() : CommandScript("playerbots_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable hotspotCommandTable =
        {
            { "list", HandleHotspotListCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "go",   HandleHotspotGoCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
        };

        static ChatCommandTable rotationCommandTable =
        {
            { "reload", HandleRotationReloadCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "info",   HandleRotationInfoCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };

        static ChatCommandTable travelCommandTable =
        {
            { "info",  HandleTravelInfoCommand,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "nodes", HandleTravelNodesCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "route", HandleTravelRouteCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
        };

        static ChatCommandTable botCommandTable =
        {
            { "add",     HandleBotAddCommand,    RBAC_PERM_COMMAND_BOT, Console::No },
            { "remove",  HandleBotRemoveCommand, RBAC_PERM_COMMAND_BOT, Console::No },
            { "list",    HandleBotListCommand,   RBAC_PERM_COMMAND_BOT, Console::No },
            { "perf",    HandleBotPerfCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "hotspot", hotspotCommandTable },
            { "travel",  travelCommandTable },
            { "rotation", rotationCommandTable },
        };

        static ChatCommandTable commandTable =
        {
            { "bot", botCommandTable },
        };

        return commandTable;
    }

    // .bot add <character>
    static bool HandleBotAddCommand(ChatHandler* handler, std::string name)
    {
        AddBotResult result = sPlayerbotMgr->AddBot(handler->GetPlayer(), name);
        if (result != AddBotResult::Ok)
        {
            handler->SendSysMessage(GetAddBotError(result));
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Logging in %s...", name.c_str());
        return true;
    }

    // .bot remove [character], all bots without a name
    static bool HandleBotRemoveCommand(ChatHandler* handler, Optional<std::string> name)
    {
        Player* master = handler->GetPlayer();
        if (!name)
        {
            sPlayerbotMgr->RemoveBotsOf(master->GetGUID());
            handler->SendSysMessage("Removing all your bots.");
            return true;
        }

        std::string characterName = *name;
        ObjectGuid guid = normalizePlayerName(characterName) ? sCharacterCache->GetCharacterGuidByName(characterName) : ObjectGuid::Empty;
        Player* bot = guid.IsEmpty() ? nullptr : ObjectAccessor::FindConnectedPlayer(guid);
        PlayerbotAI* ai = bot ? sPlayerbotMgr->GetAI(bot) : nullptr;
        if (!ai || ai->GetMasterGuid() != master->GetGUID())
        {
            handler->SendSysMessage("That character is not one of your bots.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        sPlayerbotMgr->RemoveBot(guid);
        handler->PSendSysMessage("Removing %s.", characterName.c_str());
        return true;
    }

    // .bot list
    static bool HandleBotListCommand(ChatHandler* handler)
    {
        Player* master = handler->GetPlayer();
        std::vector<Player*> bots = sPlayerbotMgr->GetBotsOf(master->GetGUID());
        handler->PSendSysMessage("Bots: %u / %u", uint32(bots.size()), sPlayerbotsConfig.MaxBotsPerPlayer);
        for (Player* bot : bots)
        {
            PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
            handler->PSendSysMessage("  %s - level %u %s, %.0f%% health", bot->GetName().c_str(), uint32(bot->GetLevel()),
                ai ? GetRoleName(ai->GetRole()) : "?", bot->GetHealthPct());
        }
        return true;
    }

    // .bot rotation reload
    static bool HandleRotationReloadCommand(ChatHandler* handler)
    {
        uint32 count = sRotationMgr->Load();
        std::shared_ptr<RotationTable const> table = sRotationMgr->GetTable();
        handler->PSendSysMessage("%u rotations from %u files in %s, %u problems", count, table ? table->Files : 0, sRotationMgr->GetPath().c_str(),
            table ? uint32(table->Errors.size()) : 0u);
        if (table)
            for (size_t i = 0; i < table->Errors.size() && i < 8; ++i)
                handler->SendSysMessage(table->Errors[i]);
        return true;
    }

    // .bot rotation info [specialization id]: the rotations loaded, or the lists of one
    static bool HandleRotationInfoCommand(ChatHandler* handler, Optional<uint32> spec)
    {
        std::shared_ptr<RotationTable const> table = sRotationMgr->GetTable();
        if (!table)
        {
            handler->SendSysMessage("No rotations loaded.");
            return true;
        }

        if (!spec)
        {
            std::string ids;
            for (auto const& [id, set] : table->Sets)
                ids += Trinity::StringFormat("{} ({}) ", id, set->Name);
            handler->PSendSysMessage("%u rotations: %s", uint32(table->Sets.size()), ids.c_str());
            return true;
        }

        auto itr = table->Sets.find(*spec);
        if (itr == table->Sets.end())
        {
            handler->SendSysMessage("No rotation for that specialization.");
            return true;
        }

        for (auto const& [name, list] : itr->second->Lists)
            handler->PSendSysMessage("%s: %u entries", name.c_str(), uint32(list.Entries.size()));
        return true;
    }

    // .bot travel info
    static bool HandleTravelInfoCommand(ChatHandler* handler)
    {
        if (!sTravelMgr->IsBuilt())
        {
            handler->SendSysMessage("The travel graph is not built (Playerbots.Travel.Enable, random bots enabled).");
            return true;
        }

        handler->PSendSysMessage("Travel graph: %s", sTravelMgr->Summary().c_str());
        return true;
    }

    // .bot travel nodes <text>
    static bool HandleTravelNodesCommand(ChatHandler* handler, Tail text)
    {
        std::vector<uint32> found = sTravelMgr->FindNodes(std::string(text), 20);
        for (uint32 id : found)
        {
            TravelNode const& node = sTravelMgr->GetGraph().GetNode(id);
            handler->PSendSysMessage("#%u map %u (%.0f, %.0f, %.0f) %s", id, node.Map, node.X, node.Y, node.Z, node.Name.c_str());
        }
        handler->PSendSysMessage("%u nodes shown", uint32(found.size()));
        return true;
    }

    // .bot travel route <node id | hotspot id>: from where you stand, for your own character
    static bool HandleTravelRouteCommand(ChatHandler* handler, uint32 nodeId)
    {
        if (!sTravelMgr->IsBuilt() || nodeId >= sTravelMgr->GetGraph().NodeCount())
        {
            handler->SendSysMessage("Unknown node, find one with .bot travel nodes <text>.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        TravelNode const& target = sTravelMgr->GetGraph().GetNode(nodeId);
        Route route = sTravelMgr->PlanFor(handler->GetPlayer(), { target.Map, target.X, target.Y, target.Z });
        for (std::string const& line : sTravelMgr->Describe(route))
            handler->SendSysMessage(line);
        return true;
    }

    // .bot hotspot list [tier]
    static bool HandleHotspotListCommand(ChatHandler* handler, Optional<uint32> tier)
    {
        std::vector<Hotspot> hotspots = sHotspotMgr->GetRegistry().Snapshot();
        std::ranges::sort(hotspots, [](Hotspot const& a, Hotspot const& b) { return a.Tier != b.Tier ? a.Tier < b.Tier : a.Id < b.Id; });

        uint32 shown = 0;
        uint32 total = 0;
        uint32 markers = 0;
        for (Hotspot const& hotspot : hotspots)
        {
            if (tier && hotspot.Tier != *tier)
                continue;

            ++total;
            markers += hotspot.Owner ? 1 : 0;
            if (shown >= 25)
                continue;

            ++shown;
            handler->PSendSysMessage("#%u tier %u %s level %u-%u map %u, %u near: %s%s", hotspot.Id, uint32(hotspot.Tier),
                hotspot.Team == 1 ? "Alliance" : hotspot.Team == 2 ? "Horde" : "both", uint32(hotspot.MinLevel), uint32(hotspot.MaxLevel),
                hotspot.Map, sHotspotMgr->GetOccupancy(hotspot), sHotspotMgr->Describe(hotspot).c_str(), hotspot.Owner ? " (marker)" : "");
        }

        handler->PSendSysMessage("%u hotspots (%u markers)%s%s", total, markers, shown < total ? ", showing the first 25, filter with a tier: " : "",
            shown < total ? ".bot hotspot list <1-5>" : "");
        return true;
    }

    // .bot hotspot go <id>
    static bool HandleHotspotGoCommand(ChatHandler* handler, uint32 id)
    {
        for (Hotspot const& hotspot : sHotspotMgr->GetRegistry().Snapshot())
        {
            if (hotspot.Id != id)
                continue;

            HotspotPoint point = hotspot.Points.empty() ? hotspot.Center : hotspot.Points.front();
            handler->GetPlayer()->TeleportTo(hotspot.Map, point.X, point.Y, point.Z, 0.0f);
            return true;
        }

        handler->SendSysMessage("No hotspot with that id.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // .bot perf [reset]
    static bool HandleBotPerfCommand(ChatHandler* handler, Optional<std::string> option)
    {
        if (option && *option == "reset")
        {
            sPlayerbotsPerf.Reset();
            handler->SendSysMessage("Playerbots timings cleared.");
            return true;
        }

        if (!sPlayerbotsConfig.PerfMonEnabled)
        {
            handler->SendSysMessage("Timing is off, set Playerbots.PerfMon.Enable = 1 and restart.");
            return true;
        }

        std::vector<PerfMonitor::Entry> entries = sPlayerbotsPerf.Snapshot();
        if (entries.empty())
        {
            handler->SendSysMessage("Nothing timed yet.");
            return true;
        }

        handler->SendSysMessage("Playerbots timings (slowest total first):");
        uint32 shown = 0;
        for (PerfMonitor::Entry const& entry : entries)
        {
            handler->PSendSysMessage("  %-20s calls %llu  avg %.1f us  max %u us  total %.1f ms", entry.Name.c_str(),
                static_cast<unsigned long long>(entry.Calls), double(entry.TotalUs) / double(entry.Calls ? entry.Calls : 1),
                entry.MaxUs, double(entry.TotalUs) / 1000.0);
            if (++shown >= 15)
                break;
        }
        return true;
    }
};

void AddSC_playerbots_commandscript()
{
    new playerbots_commandscript();
}
