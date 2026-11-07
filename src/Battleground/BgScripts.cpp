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
#include "Battleground/BgMapData.h"
#include "Battleground/BgPlanner.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BotSession.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"

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

    // world.playerbots_bg_node (Alterac Valley's objective graph); .bot bg reload reads it again
    void OnStartup() override
    {
        LoadNodes();
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

constexpr uint32 MarkerEntry = 1;                   // "Waypoint (Only GM can see it)", the core's .wp show marker
constexpr Milliseconds MarkerLifetime = 5min;

// Valley maps: a node, a tower or a boss in one word or two
std::string ValleyState(BgPlan const& plan, size_t i)
{
    NodeDef const& row = plan.Def->Nodes[i];
    Objective const& objective = plan.Objectives[i];
    if (row.IsBoss())
        return objective.Alive ? "alive" : "dead";
    if (objective.Destroyed)
        return Trinity::StringFormat("destroyed by {}", OwnerLabel(objective.Owner));
    std::string state = OwnerLabel(objective.Owner);
    if (objective.Assaulting != TEAM_NEUTRAL)
        state += Trinity::StringFormat(" (assault by {})", OwnerLabel(objective.Assaulting));
    return state;
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
        static ChatCommandTable rallyCommandTable =
        {
            { "set", HandleBgRallySetCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
        };

        static ChatCommandTable bgCommandTable =
        {
            { "list",   HandleBgListCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "roles",  HandleBgRolesCommand,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "rally",  rallyCommandTable },
            { "show",   HandleBgShowCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "reload", HandleBgReloadCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "",       HandleBgCommand,       rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
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
        Map* planMap = sMapMgr->FindMap(plan->MapId, plan->InstanceId);
        if (bg && plan->Def && plan->Def->Kind == MapKind::Valley)
            handler->PSendSysMessage("  reinforcements: Alliance %u, Horde %u; last event: %s, %s", TeamScore(*plan, bg, planMap, TEAM_ALLIANCE),
                TeamScore(*plan, bg, planMap, TEAM_HORDE), plan->LastEvent.c_str(), Ago(now, plan->LastEventMs).c_str());
        else if (bg)
            handler->PSendSysMessage("  score: Alliance %u, Horde %u (win at %u); last event: %s, %s", bg->GetTeamScore(TEAM_ALLIANCE), bg->GetTeamScore(TEAM_HORDE),
                plan->Def ? plan->Def->WinScore : 0, plan->LastEvent.c_str(), Ago(now, plan->LastEventMs).c_str());

        for (uint32 team = 0; team < 2; ++team)
        {
            TeamPlan const& teamPlan = plan->Teams[team];
            uint32 counts[5] = { 0, 0, 0, 0, 0 };
            Map* map = planMap;
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

            // Valley maps: the push on a boss, e.g. "Captain Galvangar: gathering 9/12 at the rally point, 15 on the push, 74 s left"
            if (teamPlan.RallyObjective != NoObjective)
            {
                using Playerbots::BgRules::RallyState;
                Playerbots::BgRules::Rally const& rally = teamPlan.Rally;
                std::string detail;
                if (rally.State == RallyState::Gathering)
                {
                    uint32 elapsed = now - rally.GatherStartMs, timeout = sBgConfig.RallyTimeoutSeconds * 1000;
                    detail = Trinity::StringFormat(" {}/{} at the rally point, {} s left", teamPlan.RallyPresent, rally.Need,
                        elapsed < timeout ? (timeout - elapsed) / 1000 : 0);
                }
                else if (rally.State == RallyState::Charging || rally.State == RallyState::Fighting)
                    detail = Trinity::StringFormat(" ({} charged)", rally.Charged);
                Player* puller = map && !teamPlan.Puller.IsEmpty() ? ObjectAccessor::GetPlayer(map, teamPlan.Puller) : nullptr;
                handler->PSendSysMessage("    push: %s, %s%s; %u on the push, %u s in this state, gather restarts %u%s%s%s",
                    ObjectiveName(*plan, teamPlan.RallyObjective).c_str(), Playerbots::BgRules::RallyStateName(rally.State), detail.c_str(),
                    teamPlan.RallyOnPush, (now - rally.SinceMs) / 1000, rally.Restarts, rally.DefendersPulled ? " (defenders called in)" : "",
                    puller ? ", puller " : "", puller ? puller->GetName().c_str() : "");
            }
        }

        if (!plan->Objectives.empty() && plan->Def && plan->Def->Kind == MapKind::Valley)
        {
            std::string nodes, bosses;
            for (size_t i = 0; i < plan->Objectives.size() && i < plan->Def->Nodes.size(); ++i)
            {
                NodeDef const& row = plan->Def->Nodes[i];
                if (!row.IsNode() && !row.IsBoss())
                    continue;
                std::string& line = row.IsBoss() ? bosses : nodes;
                line += Trinity::StringFormat("{}{}: {}", line.empty() ? "" : ", ", row.Name, ValleyState(*plan, i));
            }
            handler->PSendSysMessage("  nodes: %s", nodes.c_str());
            handler->PSendSysMessage("  bosses: %s", bosses.c_str());
        }
        else if (!plan->Objectives.empty() && plan->Def && plan->Def->Kind != MapKind::Flag)
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

    // .bot bg rally set <node>: the game master's position becomes the row's (any row of the map: a rally point, a gate, a node)
    static bool HandleBgRallySetCommand(ChatHandler* handler, std::string_view key)
    {
        Player* player = handler->GetPlayer();
        MapDef* def = player ? FindMapForEdit(player->GetMapId()) : nullptr;
        if (!def || def->Kind != MapKind::Valley)
        {
            handler->SendSysMessage("This map has no playerbots_bg_node rows (stand in Alterac Valley).");
            return true;
        }
        NodeDef* row = nullptr;
        std::string keys;
        for (NodeDef& node : def->Nodes)
        {
            if (node.Key == key)
                row = &node;
            if (node.Kind == NodeKind::Rally || node.Kind == NodeKind::Gate)
                keys += (keys.empty() ? "" : ", ") + node.Key;
        }
        if (!row)
        {
            handler->PSendSysMessage("No row '%s' on this map. Rally points and gates: %s (any node key works too).", std::string(key).c_str(), keys.c_str());
            return true;
        }

        row->Pos.Relocate(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        std::string sql = Trinity::StringFormat("UPDATE `playerbots_bg_node` SET `x` = {:.3f}, `y` = {:.3f}, `z` = {:.3f} WHERE `map` = {} AND `node` = '{}';",
            row->Pos.GetPositionX(), row->Pos.GetPositionY(), row->Pos.GetPositionZ(), def->MapId, row->Key);
        WorldDatabase.Execute(sql.c_str());

        // how far it now is from the boss that gathers there
        std::string from;
        for (NodeDef const& node : def->Nodes)
            if (node.IsBoss() && node.Rally >= 0 && &def->Nodes[node.Rally] == row)
                from = Trinity::StringFormat(", {:.0f} yd from {} (leash {:.0f})", row->Pos.GetExactDist2d(node.Pos), node.Name, node.LeashRadius);
        handler->PSendSysMessage("%s (%s) moved to %.1f %.1f %.1f%s. Plans use it at once; for the SQL file: %s", row->Key.c_str(), NodeKindName(row->Kind),
            row->Pos.GetPositionX(), row->Pos.GetPositionY(), row->Pos.GetPositionZ(), from.c_str(), sql.c_str());
        return true;
    }

    // .bot bg show: a marker only the game master sees on every row of the map, gone after 5 minutes
    static bool HandleBgShowCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        MapDef const* def = player ? FindMap(player->GetMapId()) : nullptr;
        if (!def || def->Kind != MapKind::Valley)
        {
            handler->SendSysMessage("This map has no playerbots_bg_node rows (stand in Alterac Valley).");
            return true;
        }
        uint32 shown = 0;
        for (NodeDef const& row : def->Nodes)
        {
            TempSummon* marker = player->SummonCreature(MarkerEntry, row.Pos, TEMPSUMMON_TIMED_DESPAWN, MarkerLifetime, 0, 0, player->GetGUID());
            shown += marker ? 1 : 0;
            handler->PSendSysMessage("  %s %s (%s) %.1f %.1f %.1f, %.0f yd from you%s", row.Key.c_str(), row.Name.c_str(), NodeKindName(row.Kind),
                row.Pos.GetPositionX(), row.Pos.GetPositionY(), row.Pos.GetPositionZ(), player->GetExactDist(row.Pos), marker ? "" : " (no marker)");
        }
        handler->PSendSysMessage("%u markers for 5 minutes, only you see them. Move one with .bot bg rally set <key>.", shown);
        return true;
    }

    // .bot bg reload: playerbots_bg_node again; every plan starts over (they index the rows)
    static bool HandleBgReloadCommand(ChatHandler* handler)
    {
        uint32 rows = LoadNodes();
        sBgPlanner.Reset();
        handler->PSendSysMessage("Read %u playerbots_bg_node rows; the planned battlegrounds plan again from their next bot tick.", rows);
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
