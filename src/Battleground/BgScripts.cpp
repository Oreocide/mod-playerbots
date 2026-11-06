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

// The battleground team planner's scripts: the world script that loads Playerbots.Bg.* and plans every match after the map updates,
// and the ".bot bg" command (merged into the .bot table of PlayerbotCommandScript by the command loader)

#include "Battleground/BgConfig.h"
#include "Battleground/BgPlanner.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BotSession.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

using namespace Trinity::ChatCommands;
using namespace Playerbots::Bg;

namespace
{
class playerbots_bg_world : public WorldScript
{
public:
    playerbots_bg_world() : WorldScript("playerbots_bg_world")
    {
        // OnConfigLoad(false) runs before scripts are registered
        sBgConfig.Load();
    }

    void OnConfigLoad(bool /*reload*/) override
    {
        sBgConfig.Load();
    }

    // after the map updates (World::Update waits for them), in the world thread like the battleground bots
    void OnUpdate(uint32 /*diff*/) override
    {
        sBgPlanner.Update();
    }
};

char const* TeamLabel(uint32 team)
{
    return team == TEAM_ALLIANCE ? "Alliance" : "Horde";
}

char const* OwnerLabel(TeamId team)
{
    return team == TEAM_ALLIANCE ? "Alliance" : team == TEAM_HORDE ? "Horde" : "neutral";
}

char const* FlagStateLabel(uint8 state)
{
    switch (FlagState(state))
    {
        case FlagState::InBase: return "in base";
        case FlagState::Taken: return "taken";
        case FlagState::Dropped: return "dropped";
        case FlagState::Respawning: return "respawning";
        default: return "not seen";
    }
}

std::string Ago(uint32 now, uint32 then)
{
    return then ? Trinity::StringFormat("{} s ago", (now - then) / 1000) : "never";
}

Battleground* FindBg(BgPlan const& plan)
{
    Map* map = sMapMgr->FindMap(plan.MapId, plan.InstanceId);
    BattlegroundMap* bgMap = map ? map->ToBattlegroundMap() : nullptr;
    return bgMap ? bgMap->GetBG() : nullptr;
}

// Team sizes of any battleground, planned or not: inside, invited and still on the way, waiting in its queue (Playerbots.Bg.FillToMax)
void SendTeamSizes(ChatHandler* handler, Battleground const* bg)
{
    uint32 humans[2] = { 0, 0 };
    for (auto const& [guid, member] : bg->GetPlayers())
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid); player && !Playerbots::IsBot(player))
            ++humans[member.Team == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE];

    uint32 queued[2] = { 0, 0 };
    BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(bg->GetQueueID());
    for (uint32 type = 0; type < BG_QUEUE_GROUP_TYPES_COUNT; ++type)
        for (GroupQueueInfo const* info : queue.m_QueuedGroups[bg->GetBracketId()][type])
            if (!info->IsInvitedToBGInstanceGUID)
                queued[type == BG_QUEUE_PREMADE_ALLIANCE || type == BG_QUEUE_NORMAL_ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE] += uint32(info->Players.size());

    std::string teams;
    for (uint32 team = 0; team < 2; ++team)
    {
        Team side = team == TEAM_ALLIANCE ? ALLIANCE : HORDE;
        uint32 inside = bg->GetPlayersCountByTeam(side);
        uint32 invited = bg->GetInvitedCount(side);
        teams += Trinity::StringFormat("{}{} {} in ({} human), {} on the way, {} queued", team ? "; " : "", TeamLabel(team), inside, humans[team],
            invited > inside ? invited - inside : 0, queued[team]);
    }

    char const* status = bg->GetStatus() == STATUS_WAIT_JOIN ? "preparation" : bg->GetStatus() == STATUS_IN_PROGRESS ? "in progress"
        : bg->GetStatus() == STATUS_WAIT_LEAVE ? "ending" : "waiting";
    handler->PSendSysMessage("  %s (map %u, instance %u, %s): %s; max %u per team, Playerbots.Bg.FillToMax %s",
        bg->GetName(), bg->GetMapId(), bg->GetInstanceID(), status, teams.c_str(), bg->GetMaxPlayersPerTeam(), sBgConfig.FillToMax ? "on" : "off");
}

class playerbots_bg_commandscript : public CommandScript
{
public:
    playerbots_bg_commandscript() : CommandScript("playerbots_bg_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable bgCommandTable =
        {
            { "list",  HandleBgListCommand,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "roles", HandleBgRolesCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "",      HandleBgCommand,      rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };

        // "bot bg" lands in the .bot table of PlayerbotCommandScript (ChatCommandNode::LoadCommandsIntoMap splits the name)
        static ChatCommandTable commandTable =
        {
            { "bot bg", bgCommandTable },
        };
        return commandTable;
    }

    // the plan of the game master's own battleground, else of the instance id given, else the only one
    static BgPlan const* Pick(ChatHandler* handler, Optional<uint32> instanceId)
    {
        std::vector<BgPlan const*> plans = sBgPlanner.List();
        if (instanceId)
        {
            for (BgPlan const* plan : plans)
                if (plan->InstanceId == *instanceId)
                    return plan;
            handler->PSendSysMessage("No planned battleground with instance id %u.", *instanceId);
            return nullptr;
        }
        if (Player* player = handler->GetPlayer())
            if (BgPlan const* plan = sBgPlanner.Find(player->GetMapId(), player->GetInstanceId()))
                return plan;
        if (plans.size() == 1)
            return plans.front();
        if (plans.empty())
            handler->PSendSysMessage("No battleground is planned (Playerbots.Bg.Planner %s, maps in Playerbots.Bg.Planner.Maps).",
                sBgConfig.Planner ? "on" : "off");
        else
            handler->PSendSysMessage("%u battlegrounds are planned: .bot bg list, then .bot bg <instance id>.", uint32(plans.size()));
        return nullptr;
    }

    static bool HandleBgListCommand(ChatHandler* handler)
    {
        std::vector<BgPlan const*> plans = sBgPlanner.List();
        handler->PSendSysMessage("Planned battlegrounds: %u (Playerbots.Bg.Planner %s)", uint32(plans.size()), sBgConfig.Planner ? "on" : "off");
        for (BgPlan const* plan : plans)
            handler->PSendSysMessage("  instance %u: %s (map %u), %u plans, Alliance %u bots + %u others defend %u%%, Horde %u bots + %u others defend %u%%",
                plan->InstanceId, plan->Def ? plan->Def->Name : "?", plan->MapId, plan->Ticks,
                plan->Teams[0].Assignable, plan->Teams[0].Others, plan->Teams[0].DefendPct,
                plan->Teams[1].Assignable, plan->Teams[1].Others, plan->Teams[1].DefendPct);

        handler->SendSysMessage("Running battlegrounds:");
        sMapMgr->DoForAllMaps([handler](Map* map)
        {
            if (BattlegroundMap* bgMap = map->ToBattlegroundMap())
                if (Battleground const* bg = bgMap->GetBG(); bg && bg->isBattleground())
                    SendTeamSizes(handler, bg);
        });
        return true;
    }

    static bool HandleBgCommand(ChatHandler* handler, Optional<uint32> instanceId)
    {
        // the game master's own battleground first, also when it is not planned (Alterac Valley before phase 3)
        if (Player* player = handler->GetPlayer(); player && !instanceId)
            if (Battleground const* own = player->GetBattleground(); own && own->isBattleground())
                SendTeamSizes(handler, own);

        BgPlan const* plan = Pick(handler, instanceId);
        if (!plan)
            return true;

        uint32 now = GameTime::GetGameTimeMS();
        Battleground* bg = FindBg(*plan);
        handler->PSendSysMessage("%s (map %u, instance %u): %s, %u plans every %u ms",
            plan->Def ? plan->Def->Name : "?", plan->MapId, plan->InstanceId,
            bg ? (bg->GetStatus() == STATUS_IN_PROGRESS ? Trinity::StringFormat("in progress {} s", bg->GetElapsedTime() / 1000).c_str() : "not started") : "gone",
            plan->Ticks, sBgConfig.TickMs);
        handler->PSendSysMessage("  captures: cast %u, used directly %u%s, gave up for now %u%s",
            plan->CaptureCasts, plan->CaptureFallbacks,
            plan->CaptureFallbacks ? Trinity::StringFormat(" (last cast result {})", plan->LastCastFailure).c_str() : "",
            plan->CaptureGiveUps, plan->CaptureGiveUps ? Trinity::StringFormat(" (last cast result {})", plan->LastGiveUpResult).c_str() : "");
        if (bg)
            handler->PSendSysMessage("  score: Alliance %u, Horde %u (win at %u); last event: %s, %s", bg->GetTeamScore(TEAM_ALLIANCE), bg->GetTeamScore(TEAM_HORDE),
                plan->Def ? plan->Def->WinScore : 0, plan->LastEvent.c_str(), Ago(now, plan->LastEventMs).c_str());

        for (uint32 team = 0; team < 2; ++team)
        {
            TeamPlan const& teamPlan = plan->Teams[team];
            uint32 counts[5] = { 0, 0, 0, 0, 0 };
            Map* map = sMapMgr->FindMap(plan->MapId, plan->InstanceId);
            for (auto const& [guid, job] : plan->Jobs)
                if (Player* player = map ? ObjectAccessor::GetPlayer(map, guid) : nullptr)
                    if (uint32(TeamOf(player)) == team)
                        ++counts[uint32(job.Now)];

            std::string targets;
            for (int8 target : teamPlan.AttackTargets)
                targets += (targets.empty() ? "" : ", ") + ObjectiveName(*plan, target);
            std::string defended;
            for (int8 target : teamPlan.DefendTargets)
                defended += (defended.empty() ? "" : ", ") + ObjectiveName(*plan, target);

            handler->PSendSysMessage("  %s: seed %016llx, defend %u%% -> %u of %u bots (+%u humans or owned bots)",
                TeamLabel(team), (unsigned long long)teamPlan.Seed, teamPlan.DefendPct, teamPlan.WantDefenders, teamPlan.Assignable, teamPlan.Others);
            handler->PSendSysMessage("    jobs: attack %u, defend %u, carrier %u, escort %u, none %u",
                counts[uint32(Job::Attack)], counts[uint32(Job::Defend)], counts[uint32(Job::Carrier)], counts[uint32(Job::Escort)], counts[uint32(Job::None)]);
            handler->PSendSysMessage("    attacking: %s; defending: %s", targets.empty() ? "-" : targets.c_str(), defended.empty() ? "-" : defended.c_str());
            handler->PSendSysMessage("    last change: %s, %u jobs or objectives, %s (%u changes in all)",
                teamPlan.LastReason.c_str(), teamPlan.LastChanged, Ago(now, teamPlan.LastReasonMs).c_str(), teamPlan.Reassigns);
        }

        if (!plan->Objectives.empty() && plan->Def && plan->Def->Kind != MapKind::Flag)
        {
            std::string line;
            for (Objective const& objective : plan->Objectives)
            {
                line += Trinity::StringFormat("{}{}: {}", line.empty() ? "" : ", ", objective.Name, objective.Known ? OwnerLabel(objective.Owner) : "not seen");
                if (objective.Assaulting != TEAM_NEUTRAL)
                    line += Trinity::StringFormat(" (assault by {})", OwnerLabel(objective.Assaulting));
            }
            handler->PSendSysMessage("  objectives: %s", line.c_str());
        }
        for (uint32 i = 0; i < 2; ++i)
        {
            FlagInfo const& flag = plan->Flags[i];
            if (!flag.Entry || (i == 1 && plan->Def && plan->Def->Kind == MapKind::Eye))
                continue;
            Player* carrier = nullptr;
            if (Map* map = sMapMgr->FindMap(plan->MapId, plan->InstanceId))
                carrier = flag.Carrier.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(map, flag.Carrier);
            handler->PSendSysMessage("  flag %u: %s%s%s", flag.Entry, FlagStateLabel(flag.State), carrier ? " by " : "", carrier ? carrier->GetName().c_str() : "");
        }
        return true;
    }

    static bool HandleBgRolesCommand(ChatHandler* handler, Optional<uint32> instanceId)
    {
        BgPlan const* plan = Pick(handler, instanceId);
        if (!plan)
            return true;

        Map* map = sMapMgr->FindMap(plan->MapId, plan->InstanceId);
        if (!map)
            return true;
        uint32 now = GameTime::GetGameTimeMS();
        for (uint32 team = 0; team < 2; ++team)
        {
            handler->PSendSysMessage("%s:", TeamLabel(team));
            for (auto const& [guid, job] : plan->Jobs)
            {
                Player* player = ObjectAccessor::GetPlayer(map, guid);
                if (!player || uint32(TeamOf(player)) != team)
                    continue;
                Player* follow = job.Follow.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(map, job.Follow);
                handler->PSendSysMessage("  %s: %s %s%s%s, job %u s%s", player->GetName().c_str(), Playerbots::BgRules::JobName(job.Now), ObjectiveName(*plan, job.Objective).c_str(),
                    follow ? ", with " : "", follow ? follow->GetName().c_str() : "", (now - job.SinceMs) / 1000,
                    player->IsAlive() ? "" : ", dead");
            }
        }
        return true;
    }
};
}

void AddSC_playerbots_bg()
{
    new playerbots_bg_world();
    new playerbots_bg_commandscript();
}
