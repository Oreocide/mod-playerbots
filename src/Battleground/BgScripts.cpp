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

#include "ActivityMgr.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgMapData.h"
#include "Battleground/BgMetrics.h"
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
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "ChatPackets.h"
#include "DB2Stores.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Vehicle.h"
#include "WorldSession.h"
#include <shared_mutex>

using namespace Trinity::ChatCommands;
using namespace Playerbots::Bg;

namespace
{
// ---- automated tests (BG_PLAN.md §5): .bot bg test (a match with no real player) and .bot bg vehicle test (phase 5.0) ----

// One vehicle test at a time: a bot boards a temporary vehicle, drives it on the mesh, fires its first spell and leaves. Every
// step is written to the log (and kept for .bot bg vehicle test). World thread: the command and the world script's update
struct VehicleTest
{
    enum class Step : uint8 { Idle, Board, Seated, Drive, Driving, Fire, Leave, Done };
    Step Now = Step::Idle;
    ObjectGuid Bot;
    ObjectGuid Vehicle;
    uint32 StartedMs = 0;
    uint32 StepMs = 0;
    Position BotStart;
    Position VehicleStart;
    Position Goal;
    std::vector<std::string> Lines;
};

VehicleTest& TheVehicleTest()
{
    static VehicleTest test;
    return test;
}

constexpr uint32 BgTestMinutes = 45;                // .bot bg test: the longest a test match may run
constexpr uint32 VehicleTestTimeoutMs = 90 * IN_MILLISECONDS;
constexpr uint32 VehicleBoardMs = 5 * IN_MILLISECONDS;
constexpr uint32 VehicleDriveMs = 25 * IN_MILLISECONDS;
constexpr uint32 VehicleFireWaitMs = 4 * IN_MILLISECONDS;
constexpr float VehicleDriveYards = 25.0f;
constexpr float VehicleAimYards = 20.0f;

void VehicleNote(std::string line)
{
    TC_LOG_INFO("server.playerbots", "Playerbots: vehicle test: {}", line);
    TheVehicleTest().Lines.push_back(std::move(line));
}

/// Ends the test whatever state it is in: the bot leaves, the vehicle goes, and both are checked
void VehicleFinish(Player* bot, Creature* vehicle, std::string const& why)
{
    VehicleTest& test = TheVehicleTest();
    if (bot && bot->GetVehicle())
        bot->ExitVehicle();
    if (bot)
        VehicleNote(Trinity::StringFormat("end ({}): bot moved {:.1f} yd from its start, rooted {}, in a vehicle {}", why,
            bot->GetExactDist2d(test.BotStart), bot->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no", bot->GetVehicle() ? "yes" : "no"));
    if (vehicle)
    {
        VehicleNote(Trinity::StringFormat("vehicle moved {:.1f} yd from its start, charmed by {}", vehicle->GetExactDist2d(test.VehicleStart),
            vehicle->GetCharmerGUID().IsEmpty() ? "nobody" : vehicle->GetCharmerGUID().ToString()));
        vehicle->DespawnOrUnsummon();
    }
    test.Now = VehicleTest::Step::Done;
}

void UpdateVehicleTest()
{
    VehicleTest& test = TheVehicleTest();
    if (test.Now == VehicleTest::Step::Idle || test.Now == VehicleTest::Step::Done)
        return;

    uint32 now = GameTime::GetGameTimeMS();
    Player* bot = ObjectAccessor::FindPlayer(test.Bot);
    Creature* vehicle = bot ? ObjectAccessor::GetCreature(*bot, test.Vehicle) : nullptr;
    if (!bot || !bot->IsInWorld())
        return VehicleFinish(nullptr, nullptr, "the bot is gone");
    if (!vehicle || !vehicle->IsAlive() || !vehicle->GetVehicleKit())
        return VehicleFinish(bot, vehicle, "the vehicle is gone");
    if (now - test.StartedMs > VehicleTestTimeoutMs)
        return VehicleFinish(bot, vehicle, "FAILED: the test ran out of time");

    switch (test.Now)
    {
        case VehicleTest::Step::Board:
        {
            int8 seat = -1;
            for (auto const& [id, vehicleSeat] : vehicle->GetVehicleKit()->Seats)
                if (vehicleSeat.SeatInfo && vehicleSeat.SeatInfo->HasFlag(VEHICLE_SEAT_FLAG_CAN_CONTROL) && vehicleSeat.IsEmpty())
                {
                    seat = id;
                    break;
                }
            if (seat < 0)
                return VehicleFinish(bot, vehicle, "FAILED: no free seat that drives");
            bot->EnterVehicle(vehicle, seat);
            VehicleNote(Trinity::StringFormat("board: EnterVehicle seat {}", int32(seat)));
            test.Now = VehicleTest::Step::Seated;
            test.StepMs = now;
            break;
        }
        case VehicleTest::Step::Seated:
            if (bot->GetVehicleBase() == vehicle)
            {
                VehicleNote(Trinity::StringFormat("seated after {} ms; the vehicle's charmer is {}, faction {} (the bot's {})", now - test.StepMs,
                    vehicle->GetCharmerGUID() == bot->GetGUID() ? "the bot" : vehicle->GetCharmerGUID().IsEmpty() ? "nobody" : "someone else",
                    vehicle->GetFaction(), bot->GetFaction()));
                test.Now = VehicleTest::Step::Drive;
                test.StepMs = now;
            }
            else if (now - test.StepMs > VehicleBoardMs)
                return VehicleFinish(bot, vehicle, "FAILED: not seated after 5 s");
            break;
        case VehicleTest::Step::Drive:
        {
            // the first direction with a full mesh path for the vehicle itself; no path, no move
            static float const Angles[] = { 0.0f, 0.785f, -0.785f, 1.571f, -1.571f, 3.142f };
            for (float angle : Angles)
            {
                Position goal = vehicle->GetNearPosition(VehicleDriveYards, angle);
                vehicle->UpdateAllowedPositionZ(goal.m_positionX, goal.m_positionY, goal.m_positionZ);
                PathGenerator path(vehicle);
                if (!path.CalculatePath(goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ()) || (path.GetPathType() & ~PATHFIND_NORMAL)
                    || path.GetPath().size() < 2)
                    continue;
                Movement::MoveSplineInit init(vehicle);
                init.MovebyPath(path.GetPath());
                init.SetWalk(false);
                init.Launch();
                test.Goal = goal;
                VehicleNote(Trinity::StringFormat("drive: a mesh path of {} points to {:.1f} {:.1f} {:.1f} ({:.0f} yd at angle {:.2f}), spline launched on the vehicle",
                    path.GetPath().size(), goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ(), VehicleDriveYards, angle));
                test.Now = VehicleTest::Step::Driving;
                test.StepMs = now;
                return;
            }
            return VehicleFinish(bot, vehicle, "FAILED: no mesh path for the vehicle in any direction");
        }
        case VehicleTest::Step::Driving:
            if ((now - test.StepMs > 500 && vehicle->movespline->Finalized()) || now - test.StepMs > VehicleDriveMs)
            {
                VehicleNote(Trinity::StringFormat("drove {} ms: the vehicle is {:.1f} yd from its start and {:.1f} yd from the goal; the bot {:.1f} yd from its start and {:.1f} yd from the vehicle",
                    now - test.StepMs, vehicle->GetExactDist2d(test.VehicleStart), vehicle->GetExactDist2d(test.Goal), bot->GetExactDist2d(test.BotStart),
                    bot->GetExactDist2d(vehicle)));
                test.Now = VehicleTest::Step::Fire;
                test.StepMs = now;
            }
            break;
        case VehicleTest::Step::Fire:
        {
            Position aim = vehicle->GetNearPosition(VehicleAimYards, 0.0f);
            vehicle->UpdateAllowedPositionZ(aim.m_positionX, aim.m_positionY, aim.m_positionZ);
            uint32 tried = 0;
            for (uint32 spellId : vehicle->m_spells)
            {
                SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE) : nullptr;
                if (!info || info->IsPassive())
                    continue;
                ++tried;
                CastSpellExtraArgs args(TRIGGERED_NONE);
                args.SetOriginalCaster(bot->GetGUID());
                SpellCastResult result = vehicle->CastSpell(aim, spellId, args);
                VehicleNote(Trinity::StringFormat("fire: {} ({}) at the ground {:.0f} yd ahead: result {}", spellId, info->SpellName->Str[DEFAULT_LOCALE],
                    VehicleAimYards, uint32(result)));
                if (result == SPELL_CAST_OK)
                    break;
            }
            if (!tried)
                VehicleNote("fire: the vehicle has no spell to cast");
            test.Now = VehicleTest::Step::Leave;
            test.StepMs = now;
            break;
        }
        case VehicleTest::Step::Leave:
            if (now - test.StepMs > VehicleFireWaitMs)
                return VehicleFinish(bot, vehicle, "done");
            break;
        default:
            break;
    }
}

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
        UpdateVehicleTest();
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

    // waiting in its queue; invited to this match and still on the accept dialog
    uint32 queued[2] = { 0, 0 };
    uint32 asked[2] = { 0, 0 };
    BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(bg->GetQueueID());
    for (uint32 type = 0; type < BG_QUEUE_GROUP_TYPES_COUNT; ++type)
        for (GroupQueueInfo const* info : queue.m_QueuedGroups[bg->GetBracketId()][type])
        {
            uint32 side = type == BG_QUEUE_PREMADE_ALLIANCE || type == BG_QUEUE_NORMAL_ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE;
            if (!info->IsInvitedToBGInstanceGUID)
                queued[side] += uint32(info->Players.size());
            else if (info->IsInvitedToBGInstanceGUID == bg->GetInstanceID())
                asked[side] += uint32(info->Players.size());
        }

    // accepted and travelling: players holding this match's id who are not inside yet. What the invited count holds beyond the
    // players inside, the open invites and the travellers is a seat nobody comes for (#15): the match cannot close while one is left
    uint32 travelling[2] = { 0, 0 };
    {
        std::shared_lock lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (player->GetBattlegroundId() == bg->GetInstanceID() && player->GetBattleground() == bg && !bg->GetPlayers().contains(guid))
                ++travelling[player->GetBGTeam() == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE];
    }

    std::string teams;
    for (uint32 team = 0; team < 2; ++team)
    {
        Team side = team == TEAM_ALLIANCE ? ALLIANCE : HORDE;
        uint32 inside = bg->GetPlayersCountByTeam(side);
        uint32 invited = bg->GetInvitedCount(side);
        uint32 accounted = inside + asked[team] + travelling[team];
        teams += Trinity::StringFormat("{}{} {} in ({} human), {} on the way, {} queued{}", team ? "; " : "", TeamLabel(team), inside, humans[team],
            invited > inside ? invited - inside : 0, queued[team],
            invited > accounted ? Trinity::StringFormat(", {} lost seats", invited - accounted) : std::string());
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

        static ChatCommandTable testCommandTable =
        {
            { "start", HandleBgTestStartCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "stop",  HandleBgTestStopCommand,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "say",   HandleBgTestSayCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "",      HandleBgTestCommand,      rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };

        static ChatCommandTable vehicleCommandTable =
        {
            { "test", HandleBgVehicleTestCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };

        static ChatCommandTable bgCommandTable =
        {
            { "list",    HandleBgListCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "roles",   HandleBgRolesCommand,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "rally",   rallyCommandTable },
            { "show",    HandleBgShowCommand,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "reload",  HandleBgReloadCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "metrics", HandleBgMetricsCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "test",    testCommandTable },
            { "vehicle", vehicleCommandTable },
            { "",        HandleBgCommand,       rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
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

        Playerbots::ActivityMgr::InviteCounters const& invites = sActivityMgr->GetInviteCounters();
        handler->PSendSysMessage("Bot invites since boot: %u accepts held while the bot was being teleported, %u accepted ports sent again, %u seats given up",
            invites.Held, invites.Resent, invites.Lost);
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
            // elapsed time counts the preparation; play time is the match since the gates opened (Battleground.h GetInProgressDuration)
            bg ? (bg->GetStatus() == STATUS_IN_PROGRESS ? Trinity::StringFormat("in progress {} s (play {} s)", bg->GetElapsedTime() / 1000,
                bg->GetInProgressDuration().count() / 1000).c_str() : "not started") : "gone",
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

            // the reactive behaviours (phase 4): who follows an overlay now, the threats at our nodes
            if (sBgConfig.Reacts())
            {
                uint32 overlays[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                for (auto const& [guid, job] : plan->Jobs)
                    if (Player* player = map ? ObjectAccessor::GetPlayer(map, guid) : nullptr; player && uint32(TeamOf(player)) == team)
                        ++overlays[std::min<uint32>(7, uint32(job.Over))];
                std::string over;
                for (uint32 i = 1; i < 8; ++i)
                    if (overlays[i])
                        over += Trinity::StringFormat("{}{} {}", over.empty() ? "" : ", ", Playerbots::BgRules::OverlayName(Playerbots::BgRules::Overlay(i)), overlays[i]);
                handler->PSendSysMessage("    overlays: %s; %u sent to threats, %u fall backs since the start", over.empty() ? "-" : over.c_str(),
                    teamPlan.Responses, teamPlan.FallBacks);
                std::string threats;
                for (Threat const& threat : teamPlan.Threats)
                {
                    threats += Trinity::StringFormat("{}{} {} against {}", threats.empty() ? "" : "; ", ObjectiveName(*plan, threat.Objective),
                        threat.Allies, threat.Enemies);
                    if (threat.GaveUp)
                        threats += ", given up";
                    else if (threat.Sent || threat.Need)
                        threats += Trinity::StringFormat(", {} on the way, {} more wanted", threat.Sent, threat.Need);
                    if (threat.Urgency != Playerbots::BgRules::Urgency::None)
                        threats += Trinity::StringFormat(", their assault ends in {} s ({})", threat.LeftMs / 1000,
                            Playerbots::BgRules::UrgencyName(threat.Urgency));
                }
                handler->PSendSysMessage("    threats: %s", threats.empty() ? "-" : threats.c_str());

                if (sBgConfig.Archetypes || sBgConfig.TeamSkill || sBgConfig.NinjaCaps || sBgConfig.LaneChange)
                {
                    uint32 styles[6] = { 0, 0, 0, 0, 0, 0 };
                    for (auto const& [guid, job] : plan->Jobs)
                        if (Player* player = map ? ObjectAccessor::GetPlayer(map, guid) : nullptr; player && uint32(TeamOf(player)) == team)
                            ++styles[std::min<uint32>(5, uint32(job.Style))];
                    std::string mix;
                    for (uint32 i = 0; i < 6; ++i)
                        if (styles[i])
                            mix += Trinity::StringFormat("{}{} {}", mix.empty() ? "" : ", ", Playerbots::BgRules::ArchetypeName(Playerbots::BgRules::Archetype(i)), styles[i]);
                    handler->PSendSysMessage("    skill %u%s; styles: %s; ninjas %u (%u nodes taken), lane changes %u", teamPlan.Skill,
                        teamPlan.HadHuman && sBgConfig.TeamSkill ? Trinity::StringFormat(" (your side's streak {:+})", sBgPlanner.Streak()).c_str() : "",
                        mix.empty() ? "-" : mix.c_str(), teamPlan.Ninjas, teamPlan.NinjaCaps, teamPlan.LaneChanges);
                }

                if (sBgConfig.Mines && plan->Def && plan->Def->Kind == MapKind::Valley)
                    handler->PSendSysMessage("    mines: %s%s; %u taken since the start",
                        teamPlan.MineObjective != NoObjective ? ObjectiveName(*plan, teamPlan.MineObjective).c_str() : "no mine group",
                        teamPlan.MineObjective != NoObjective ? Trinity::StringFormat(" ({} s left)", int32(teamPlan.MineUntilMs - now) > 0
                            ? (teamPlan.MineUntilMs - now) / 1000 : 0).c_str() : "", teamPlan.MinesTaken);

                if (sBgConfig.HumanCalls || sBgConfig.CallOuts)
                {
                    std::string holding;
                    for (CallOrder const& order : teamPlan.Calls)
                        if (int32(order.UntilMs - now) > 0)
                            holding += Trinity::StringFormat("{}{} {}{} ({} s left)", holding.empty() ? "" : ", ", Playerbots::BgRules::CallKindName(order.Kind),
                                ObjectiveName(*plan, order.Objective), order.Count ? Trinity::StringFormat(" {}", order.Count) : std::string(),
                                (order.UntilMs - now) / 1000);
                    handler->PSendSysMessage("    calls: %u heard, holding: %s; %u bot lines said, %u raid marks set", teamPlan.CallsHeard,
                        holding.empty() ? "-" : holding.c_str(), teamPlan.Lines, teamPlan.MarksSet);
                }

                if (sBgConfig.ScoreClock || sBgConfig.Endgame)
                {
                    using Playerbots::BgRules::Never;
                    Playerbots::BgRules::Outlook const& outlook = teamPlan.Outlook;
                    std::string race;
                    if (outlook.UsSeconds != Never || outlook.ThemSeconds != Never)
                        race = Trinity::StringFormat("; to win: us {}, them {}", outlook.UsSeconds == Never ? "never" : Trinity::StringFormat("{} s", outlook.UsSeconds),
                            outlook.ThemSeconds == Never ? "never" : Trinity::StringFormat("{} s", outlook.ThemSeconds));
                    handler->PSendSysMessage("    outlook: %s%s%s%s; stance %s for %u s%s", outlook.Ahead > 0 ? "ahead" : outlook.Ahead < 0 ? "behind" : "level",
                        outlook.Certain ? ", certain" : "", outlook.Late ? ", late" : "", race.c_str(), Playerbots::BgRules::StanceName(teamPlan.Stance),
                        (now - (teamPlan.StanceSinceMs ? teamPlan.StanceSinceMs : plan->CreatedMs)) / 1000,
                        teamPlan.PendingStance != teamPlan.Stance ? Trinity::StringFormat(" (going {})", Playerbots::BgRules::StanceName(teamPlan.PendingStance)).c_str() : "");
                }
            }

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
                if (!row.IsNode() && !row.IsBoss() && !row.IsMine())
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

    // .bot bg metrics: per map since boot, and the last minute line of each team in the running matches (Playerbots.Bg.Metrics)
    static bool HandleBgMetricsCommand(ChatHandler* handler)
    {
        for (std::string const& line : Metrics::Report())
            handler->SendSysMessage(line);
        return true;
    }

    // .bot bg test start <map> [level] [seed]: the random bots fill and play one normal battleground match with no real player in it;
    // a seed (not 0) makes the teams roll the same way in every test match started with it (level 0: the bracket with most bots)
    static bool HandleBgTestStartCommand(ChatHandler* handler, uint32 mapId, Optional<uint32> level, Optional<uint64> seed)
    {
        if (level && !*level)
            level.reset();
        // the normal (not rated, not arena) battleground whose only map is this one. Every BattlemasterList row, not the type ids
        // below MAX_BATTLEGROUND_TYPE_ID: today's Warsong Gulch (map 2106) and Arathi Basin (2107) are rows 1014 and 1018
        BattlegroundTemplate const* bgTemplate = nullptr;
        for (BattlemasterListEntry const* list : sBattlemasterListStore)
        {
            BattlegroundTemplate const* candidate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(BattlegroundTypeId(list->ID));
            if (candidate && candidate->MapIDs.size() == 1 && uint32(candidate->MapIDs.front()) == mapId && !candidate->IsArena())
            {
                bgTemplate = candidate;
                break;
            }
        }
        if (!bgTemplate || bgTemplate->MapIDs.empty())
        {
            handler->PSendSysMessage("No normal battleground on map %u.", mapId);
            return true;
        }

        // the bracket of the given level, else the one where the smaller team has the most random bots online
        std::map<uint8, std::array<uint32, 2>> online;
        sPlayerbotMgr->ForEachRandomBot([&](Player* bot, Playerbots::PlayerbotAI* /*ai*/)
        {
            if (bot->GetLevel() < bgTemplate->GetMinLevel() || bot->GetLevel() > bgTemplate->GetMaxLevel())
                return;
            if (PVPDifficultyEntry const* bracket = DB2Manager::GetBattlegroundBracketByLevel(mapId, bot->GetLevel()))
                ++online[uint8(bracket->GetBracketId())][bot->GetTeamId() == TEAM_ALLIANCE ? 0 : 1];
        });
        PVPDifficultyEntry const* bracket = level ? DB2Manager::GetBattlegroundBracketByLevel(mapId, *level) : nullptr;
        uint8 bracketId = bracket ? uint8(bracket->GetBracketId()) : 0;
        if (!bracket)
        {
            uint32 best = 0;
            for (auto const& [id, teams] : online)
                if (std::min(teams[0], teams[1]) > best)
                {
                    best = std::min(teams[0], teams[1]);
                    bracketId = id;
                }
            if (!best)
            {
                handler->SendSysMessage("No bracket of this battleground has random bots of both teams online (Playerbots.Random.OnlyWithPlayers = 1 keeps them offline while nobody plays).");
                return true;
            }
        }

        uint64 queue = BattlegroundMgr::BGQueueTypeId(uint16(bgTemplate->Id), BattlegroundQueueIdType::Battleground, false, 0).GetPacked();
        if (!sActivityMgr->StartBgTest(queue, bracketId, BgTestMinutes))
        {
            handler->SendSysMessage("That battleground queue cannot be tested.");
            return true;
        }
        handler->PSendSysMessage("Test match started: %s (map %u), bracket %u, %u Alliance and %u Horde random bots online in it, for at most %u minutes. "
            "The bots queue on the next scans (every 5 s); .bot bg test shows it, .bot bg test stop ends it.", bgTemplate->BattlemasterEntry->Name[DEFAULT_LOCALE],
            mapId, uint32(bracketId), online[bracketId][0], online[bracketId][1], BgTestMinutes);
        SetTestSeed(seed.value_or(0));
        if (seed && *seed)
            handler->SendSysMessage(Trinity::StringFormat("The teams of the test match roll from seed {}.", *seed));
        if (!sPlayerbotsConfig.RandomBattlegrounds)
            handler->SendSysMessage("Warning: Playerbots.Random.Battlegrounds is off, no bot will queue.");
        if (!sBgConfig.Plans(mapId))
            handler->SendSysMessage("Warning: the planner does not play this map (Playerbots.Bg.Planner, Playerbots.Bg.Planner.Maps).");
        return true;
    }

    static bool HandleBgTestStopCommand(ChatHandler* handler)
    {
        if (!sActivityMgr->GetBgTest().Active())
        {
            handler->SendSysMessage("No test match is running.");
            return true;
        }
        sActivityMgr->StopBgTest("stopped by command");
        handler->SendSysMessage("Test match stopped: the bots leave it on the next scan.");
        return true;
    }

    static bool HandleBgTestCommand(ChatHandler* handler)
    {
        Playerbots::ActivityMgr::BgTest const& test = sActivityMgr->GetBgTest();
        if (!test.Active())
        {
            handler->SendSysMessage("No test match is running (.bot bg test start <map> [level]).");
            return true;
        }
        uint32 now = GameTime::GetGameTimeMS();
        handler->PSendSysMessage("Test match: queue %llu bracket %u, instance %u, %u bots sent, running %u s, %u s left.", (unsigned long long)test.Queue,
            uint32(test.Bracket), test.InstanceId, test.Joined, (now - test.StartedMs) / IN_MILLISECONDS,
            int32(test.UntilMs - now) > 0 ? (test.UntilMs - now) / IN_MILLISECONDS : 0);
        for (BgPlan const* plan : sBgPlanner.List())
            if (test.InstanceId && plan->InstanceId == test.InstanceId)
                handler->PSendSysMessage("The planner plans it: .bot bg %u shows it.", plan->InstanceId);
        return true;
    }

    // .bot bg test say <text>: a bot of the test match says text in instance chat through its own session, the client's path
    // (the /bg check of reactive #10: Server.log shows `called "..."` when the hook fires and Playerbots.Bg.HumanCalls is on)
    static bool HandleBgTestSayCommand(ChatHandler* handler, Tail text)
    {
        Playerbots::ActivityMgr::BgTest const& test = sActivityMgr->GetBgTest();
        Battleground* bg = test.InstanceId ? sBattlegroundMgr->GetBattleground(test.InstanceId, BattlegroundQueueTypeId::FromPacked(test.Queue)) : nullptr;
        if (!bg || text.empty())
        {
            handler->SendSysMessage("No test match is running yet, or no text.");
            return true;
        }
        for (auto const& [guid, member] : bg->GetPlayers())
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
            Playerbots::PlayerbotAI* ai = bot && Playerbots::IsBot(bot) ? sPlayerbotMgr->GetAI(bot) : nullptr;
            if (!ai || ai->GetMaster() || !bot->IsInWorld() || !bot->GetGroup())
                continue;
            WorldPackets::Chat::ChatMessage packet{ WorldPacket(CMSG_CHAT_MESSAGE_INSTANCE_CHAT) };
            packet.Text = std::string(text);
            packet.Language = bot->GetTeam() == ALLIANCE ? LANG_COMMON : LANG_ORCISH;
            bot->GetSession()->HandleChatMessageOpcode(packet);
            handler->PSendSysMessage("%s (%s) said \"%s\" in instance chat.", bot->GetName().c_str(), TeamLabel(uint32(TeamOf(bot))),
                std::string(text).c_str());
            return true;
        }
        handler->SendSysMessage("No bot of the test match is in a battleground group yet.");
        return true;
    }

    // .bot bg vehicle test <bot> [entry]: phase 5.0, one bot boards a temporary vehicle, drives it on the mesh, fires and leaves
    static bool HandleBgVehicleTestCommand(ChatHandler* handler, Optional<std::string> botName, Optional<uint32> entry)
    {
        VehicleTest& test = TheVehicleTest();
        if (!botName)
        {
            if (test.Lines.empty())
                handler->SendSysMessage("No vehicle test has run (.bot bg vehicle test <bot> [entry, default 34775 Demolisher]).");
            for (std::string const& line : test.Lines)
                handler->PSendSysMessage("  %s", line.c_str());
            return true;
        }
        if (test.Now != VehicleTest::Step::Idle && test.Now != VehicleTest::Step::Done)
        {
            handler->SendSysMessage("A vehicle test is running.");
            return true;
        }
        Player* bot = ObjectAccessor::FindPlayerByName(*botName);
        if (!bot || !Playerbots::IsBot(bot) || !bot->IsInWorld() || !bot->IsAlive() || bot->GetVehicle() || bot->IsInCombat())
        {
            handler->SendSysMessage("No such bot online, alive, out of combat and out of a vehicle.");
            return true;
        }
        uint32 vehicleEntry = entry.value_or(34775);
        Position at = bot->GetNearPosition(6.0f, 0.0f);
        TempSummon* vehicle = bot->SummonCreature(vehicleEntry, at, TEMPSUMMON_TIMED_DESPAWN, 5min);
        if (!vehicle || !vehicle->GetVehicleKit())
        {
            handler->PSendSysMessage("Creature %u was not summoned as a vehicle.", vehicleEntry);
            if (vehicle)
                vehicle->DespawnOrUnsummon();
            return true;
        }
        vehicle->SetFaction(bot->GetFaction());
        test = VehicleTest();
        test.Now = VehicleTest::Step::Board;
        test.Bot = bot->GetGUID();
        test.Vehicle = vehicle->GetGUID();
        test.StartedMs = GameTime::GetGameTimeMS();
        test.StepMs = test.StartedMs;
        test.BotStart = bot->GetPosition();
        test.VehicleStart = vehicle->GetPosition();
        bool active = bot->InBattleground() || bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid();
        VehicleNote(Trinity::StringFormat("start: {} on map {} at {:.1f} {:.1f} {:.1f}, vehicle {} {} summoned 6 yd ahead{}", bot->GetName(), bot->GetMapId(),
            at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), vehicleEntry, vehicle->GetName(),
            active ? "" : " (open world: a bot far from players is dormant and may never finish boarding)"));
        handler->SendSysMessage("Vehicle test started; .bot bg vehicle test shows the steps (about 40 s).");
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
                std::string over;
                if (job.Over == Playerbots::BgRules::Overlay::Respond)
                    over = Trinity::StringFormat(" [helps at {}]", ObjectiveName(*plan, job.Respond));
                else if (job.Over != Playerbots::BgRules::Overlay::None)
                    over = Trinity::StringFormat(" [{}]", Playerbots::BgRules::OverlayName(job.Over));
                handler->PSendSysMessage("  %s: %s %s%s%s, job %u s%s%s", player->GetName().c_str(), Playerbots::BgRules::JobName(job.Now), ObjectiveName(*plan, job.Objective).c_str(),
                    follow ? ", with " : "", follow ? follow->GetName().c_str() : "", (now - job.SinceMs) / 1000,
                    player->IsAlive() ? "" : ", dead", over.c_str());
            }
        }
        return true;
    }
};
}

namespace Playerbots::Bg
{
bool VehicleTestHolds(Player const* bot)
{
    VehicleTest const& test = TheVehicleTest();
    return test.Now != VehicleTest::Step::Idle && test.Now != VehicleTest::Step::Done && test.Bot == bot->GetGUID();
}
}

void AddSC_playerbots_bg()
{
    new playerbots_bg_world();
    new playerbots_bg_commandscript();
}
