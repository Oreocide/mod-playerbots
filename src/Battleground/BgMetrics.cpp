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

#include "Battleground/BgMetrics.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgCoordinator.h"
#include "Battleground/BgMapData.h"
#include "Battleground.h"
#include "BotSession.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

namespace Playerbots::Bg::Metrics
{
namespace
{
constexpr float PresentRange = 40.0f;       // a player this close to an objective is present there (the coordinator's counting pass)
constexpr uint32 MassedMinAttackers = 4;    // a team with fewer living attackers never counts as massed anywhere

struct Match
{
    std::string Name;
    uint32 LastSampleMs = 0;
    uint32 PlayMs = 0;                      // Battleground::GetInProgressDuration on the last sample (play time, no preparation)
    uint32 NextLineMs = 0;                  // play ms of the next line (Playerbots.Bg.Metrics.LineSeconds apart; 0: not set yet)
    uint32 Plans = 0;
    std::unordered_map<ObjectGuid, bool> Alive;     // every player as the last sample saw it
    uint32 Deaths[2] = { 0, 0 };
    uint32 MinuteDeaths[2] = { 0, 0 };
    uint32 Captures[2] = { 0, 0 };
    uint32 Score[2] = { 0, 0 };
    std::vector<uint64> BotMs[2];           // per objective: milliseconds of living bots whose job named it
    std::vector<int32> FirstAssault[2];     // per objective: play seconds of the team's first assault there (-1: none)
    std::vector<int32> Massed[2];           // per objective: play seconds when half the team's living attackers first stood there
    std::vector<uint8> LastOwner;           // TeamId per objective (TEAM_NEUTRAL included) on the last sample
    std::string LastLine[2];
};

struct MapTotals
{
    std::string Name;
    uint32 Matches = 0;
    uint32 Wins[2] = { 0, 0 };
    uint32 Undecided = 0;
    uint64 PlayMs = 0;
};

std::map<std::pair<uint32, uint32>, Match> Matches;
std::map<uint32, MapTotals> Totals;

char const* TeamWord(uint32 team)
{
    return team == TEAM_ALLIANCE ? "Alliance" : "Horde";
}

void Size(Match& match, size_t objectives)
{
    for (uint32 team = 0; team < 2; ++team)
    {
        match.BotMs[team].resize(objectives, 0);
        match.FirstAssault[team].resize(objectives, -1);
        match.Massed[team].resize(objectives, -1);
    }
    match.LastOwner.resize(objectives, uint8(TEAM_NEUTRAL));
}

std::string Firsts(BgPlan const& plan, std::vector<int32> const& seconds)
{
    std::string out;
    for (size_t i = 0; i < seconds.size(); ++i)
        if (seconds[i] >= 0)
            out += Trinity::StringFormat("{}{} {} s", out.empty() ? "" : ", ", ObjectiveName(plan, int8(i)), seconds[i]);
    return out.empty() ? "-" : out;
}

// Playerbots.Bg.Trace: world thread only (the planner's update and the battleground bots' own AI, PlayerbotMgr::TickOtherMapBots)
struct TraceBot
{
    bool Seen = false;
    uint32 SplineId = 0;
    bool SplineRunning = false;
    float DestX = 0.0f, DestY = 0.0f;
    uint32 Launches = 0;                    // splines started since the last line
    uint32 Restarts = 0;                    // of them, started while the one before was still running
    uint32 SameRestarts = 0;                // of those, to the same spot (5 yd): the move given again, the path started over
    uint32 GoCalls = 0;                     // Bg::Go calls since the last line
    uint32 GoNoPath = 0;
    Position GoTo;                          // the last Go target
    bool HasGo = false;
    uint32 LastLineMs = 0;
};

struct TraceMatch
{
    uint32 PickedAtPlan = std::numeric_limits<uint32>::max();
    std::vector<ObjectGuid> Bots;
};

std::map<std::pair<uint32, uint32>, TraceMatch> TraceMatches;
std::unordered_map<ObjectGuid, TraceBot> TraceBots;

int8 TraceObjective(BgPlan const& plan)
{
    if (sBgConfig.Trace.empty() || !plan.Def)
        return NoObjective;
    for (size_t i = 0; i < plan.Def->Nodes.size() && i < plan.Objectives.size(); ++i)
        if (plan.Def->Nodes[i].Key == sBgConfig.Trace)
            return int8(i);
    return NoObjective;
}

char const* MotionName(MovementGeneratorType type)
{
    switch (type)
    {
        case IDLE_MOTION_TYPE: return "idle";
        case CHASE_MOTION_TYPE: return "chase";
        case POINT_MOTION_TYPE: return "point";
        case FOLLOW_MOTION_TYPE: return "follow";
        case EFFECT_MOTION_TYPE: return "effect";
        case FLEEING_MOTION_TYPE: case TIMED_FLEEING_MOTION_TYPE: return "flee";
        case CONFUSED_MOTION_TYPE: return "confused";
        case FACE_MOTION_TYPE: return "face";
        default: return "other";
    }
}

std::string UnitText(Player const* bot, Unit const* unit)
{
    if (!unit)
        return "none";
    return Trinity::StringFormat("{} {:.0f} yd{}", unit->GetName(), bot->GetExactDist2d(unit), unit->IsAlive() ? "" : " dead");
}

/// The bots traced: those still attacking the objective stay, the rest are dropped, and the list is filled again in guid order
void PickTraced(BgPlan const& plan, Map* map, int8 objective, TraceMatch& match, uint32 playS)
{
    auto attacking = [&](ObjectGuid guid)
    {
        auto job = plan.Jobs.find(guid);
        return job != plan.Jobs.end() && job->second.Base == Job::Attack && job->second.Objective == objective;
    };
    std::erase_if(match.Bots, [&](ObjectGuid guid)
    {
        Player* bot = ObjectAccessor::GetPlayer(map, guid);
        if (bot && attacking(guid))
            return false;
        auto job = plan.Jobs.find(guid);
        TC_LOG_INFO("server.playerbots.bg", "Playerbots: bg trace map {} instance {} play {} s: {} dropped ({})", plan.MapId, plan.InstanceId, playS,
            bot ? bot->GetName() : guid.ToString(), !bot ? "left the match" : job == plan.Jobs.end() ? "no job"
            : Trinity::StringFormat("now {} {}", BgRules::JobName(job->second.Base), ObjectiveName(plan, job->second.Objective)));
        TraceBots.erase(guid);
        return true;
    });
    std::vector<ObjectGuid> candidates;
    for (auto const& [guid, job] : plan.Jobs)
        if (attacking(guid) && std::find(match.Bots.begin(), match.Bots.end(), guid) == match.Bots.end())
            if (Player* bot = ObjectAccessor::GetPlayer(map, guid); bot && IsBot(bot))
                candidates.push_back(guid);
    std::sort(candidates.begin(), candidates.end());
    NodeDef const* row = RowOf(plan, objective);
    for (ObjectGuid guid : candidates)
    {
        if (match.Bots.size() >= sBgConfig.TraceBots)
            break;
        match.Bots.push_back(guid);
        TraceBots[guid] = TraceBot();
        Player* bot = ObjectAccessor::GetPlayer(map, guid);
        TC_LOG_INFO("server.playerbots.bg", "Playerbots: bg trace map {} instance {} play {} s: {} ({}) traced, attacking {} (min_group {})", plan.MapId,
            plan.InstanceId, playS, bot->GetName(), TeamWord(TeamOf(bot)), ObjectiveName(plan, objective), row ? uint32(row->MinGroup) : 0);
    }
}

/// Every world tick: a new spline id is a launch; a launch while the one before still ran is a restart
void SampleMovement(Player const* bot, TraceBot& state)
{
    Movement::MoveSpline const& spline = *bot->movespline;
    uint32 id = spline.GetId();
    auto const& dest = spline.FinalDestination();
    if (state.Seen && id != state.SplineId)
    {
        ++state.Launches;
        if (state.SplineRunning)
        {
            ++state.Restarts;
            float dx = dest.x - state.DestX, dy = dest.y - state.DestY;
            if (dx * dx + dy * dy < 25.0f)
                ++state.SameRestarts;
        }
    }
    state.Seen = true;
    state.SplineId = id;
    state.SplineRunning = spline.Initialized() && !spline.Finalized();
    state.DestX = dest.x;
    state.DestY = dest.y;
}

void WriteTrace(BgPlan const& plan, Battleground* bg, int8 objective, Player* bot, TraceBot const& state, uint32 now)
{
    uint32 team = uint32(TeamOf(bot));
    Position at;
    bool placed = ObjectivePosition(plan, objective, at);
    float toObjective = placed ? bot->GetExactDist2d(at) : -1.0f;
    auto job = plan.Jobs.find(bot->GetGUID());
    std::string jobText = job == plan.Jobs.end() ? std::string("none") : Trinity::StringFormat("{} {}{}{}", BgRules::JobName(job->second.Now),
        ObjectiveName(plan, job->second.Objective), job->second.Over != BgRules::Overlay::None ? " (overlay)" : "",
        job->second.NoPathObjective != NoObjective && int32(job->second.NoPathUntilMs - now) > 0
            ? Trinity::StringFormat(" (no path to {})", ObjectiveName(plan, job->second.NoPathObjective)) : std::string());
    std::string task = team < 2 ? Coordinator::TraceTask(plan, team, bot->GetGUID(), now) : std::string();
    PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
    std::string action = ai ? Trinity::StringFormat("{} {} ms ago", ai->GetLastActionName(), now - ai->GetLastActionMs()) : std::string("no AI");
    std::string forced = "none";
    if (ai && !ai->GetForcedTarget().IsEmpty())
    {
        Unit const* unit = ObjectAccessor::GetUnit(*bot, ai->GetForcedTarget());
        forced = unit ? UnitText(bot, unit) : std::string("not on the map");
    }
    std::string pick = ai ? UnitText(bot, ai->FindCombatTarget()) : std::string("no AI");
    Movement::MoveSpline const& spline = *bot->movespline;
    auto const& dest = spline.FinalDestination();
    MovementGeneratorType motion = bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    std::string splineText = state.SplineRunning ? Trinity::StringFormat("to ({:.0f}, {:.0f}, {:.0f}) {:.0f} yd ahead, {:.0f} yd from the objective",
        dest.x, dest.y, dest.z, bot->GetExactDist2d(dest.x, dest.y), placed ? at.GetExactDist2d(dest.x, dest.y) : -1.0f) : std::string("none");
    std::string goText = state.HasGo ? Trinity::StringFormat("go {} no-path {}, last to {:.0f} yd from the objective", state.GoCalls, state.GoNoPath,
        placed ? at.GetExactDist2d(state.GoTo) : -1.0f) : Trinity::StringFormat("go {} no-path {}", state.GoCalls, state.GoNoPath);
    TeamPlan const* teamPlan = team < 2 ? &plan.Teams[team] : nullptr;
    std::string rally = teamPlan && teamPlan->RallyObjective != NoObjective ? Trinity::StringFormat("{} {} need {}", ObjectiveName(plan, teamPlan->RallyObjective),
        BgRules::RallyStateName(teamPlan->Rally.State), teamPlan->Rally.Need) : std::string("none");
    TC_LOG_INFO("server.playerbots.bg", "Playerbots: bg trace map {} instance {} play {} s {} ({}): {} | job {}{}{} | at ({:.0f}, {:.0f}, {:.0f}) {:.0f} yd from {} "
        "| action {} | move {} spline {}, launched {} restarted {} same spot {} | {} | forced {} | pick {} | victim {} | team rally {} | travel {}",
        plan.MapId, plan.InstanceId, uint32(bg->GetInProgressDuration().count() / 1000), bot->GetName(), team < 2 ? TeamWord(team) : "none",
        !bot->IsAlive() ? "dead" : bot->IsInCombat() ? "alive, in combat" : "alive", jobText, task.empty() ? "" : " | ", task,
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), toObjective, ObjectiveName(plan, objective), action, MotionName(motion),
        splineText, state.Launches, state.Restarts, state.SameRestarts, goText, forced, pick, UnitText(bot, bot->GetVictim()), rally,
        Coordinator::Travelling(bot) ? "yes" : "no");
}
}

void Sample(BgPlan const& plan, Battleground* bg, Map* map, uint32 now)
{
    if (!sBgConfig.Metrics || !bg || !map || !plan.Def || bg->GetStatus() != STATUS_IN_PROGRESS)
        return;

    Match& match = Matches[{ plan.MapId, plan.InstanceId }];
    size_t const objectives = plan.Objectives.size();
    if (match.Name.empty())
        match.Name = plan.Def->Name;
    Size(match, objectives);
    uint32 const dt = match.LastSampleMs ? now - match.LastSampleMs : 0;
    match.LastSampleMs = now;
    match.PlayMs = uint32(bg->GetInProgressDuration().count());
    match.Plans = plan.Ticks;
    int32 const playS = int32(match.PlayMs / 1000);

    std::vector<Position> where(objectives);
    std::vector<bool> placed(objectives, false);
    for (size_t i = 0; i < objectives; ++i)
        placed[i] = ObjectivePosition(plan, int8(i), where[i]);

    // the counting pass: every player once (deaths, presence), independent of the reactive behaviours' picture
    std::unordered_map<ObjectGuid, std::pair<uint8, bool>> seen;
    std::vector<uint32> present[2] = { std::vector<uint32>(objectives, 0), std::vector<uint32>(objectives, 0) };
    std::vector<uint32> massing[2] = { std::vector<uint32>(objectives, 0), std::vector<uint32>(objectives, 0) };
    uint32 attackers[2] = { 0, 0 };
    for (MapReference const& ref : map->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (!player || !player->IsInWorld() || player->IsGameMaster())
            continue;
        TeamId team = TeamOf(player);
        if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
            continue;
        bool alive = player->IsAlive();
        auto last = match.Alive.find(player->GetGUID());
        if (last != match.Alive.end() && last->second && !alive)
        {
            ++match.Deaths[team];
            ++match.MinuteDeaths[team];
        }
        match.Alive[player->GetGUID()] = alive;
        seen[player->GetGUID()] = { uint8(team), alive };
        if (!alive)
            continue;

        auto job = plan.Jobs.find(player->GetGUID());
        bool attacker = job != plan.Jobs.end() && job->second.Base == Job::Attack;
        if (attacker)
            ++attackers[team];
        for (size_t i = 0; i < objectives; ++i)
        {
            if (!placed[i] || player->GetExactDist2d(where[i]) > PresentRange)
                continue;
            ++present[team][i];
            if (attacker)
                ++massing[team][i];
        }
    }

    // what the jobs name: living bots and dead ones per objective
    std::vector<uint32> assigned[2] = { std::vector<uint32>(objectives, 0), std::vector<uint32>(objectives, 0) };
    std::vector<uint32> dead[2] = { std::vector<uint32>(objectives, 0), std::vector<uint32>(objectives, 0) };
    for (auto const& [guid, job] : plan.Jobs)
    {
        auto it = seen.find(guid);
        if (it == seen.end() || job.Objective < 0 || size_t(job.Objective) >= objectives)
            continue;
        uint8 team = it->second.first;
        if (it->second.second)
        {
            ++assigned[team][job.Objective];
            match.BotMs[team][job.Objective] += dt;
        }
        else
            ++dead[team][job.Objective];
    }

    for (size_t i = 0; i < objectives; ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (objective.Assaulting == TEAM_ALLIANCE || objective.Assaulting == TEAM_HORDE)
            if (match.FirstAssault[objective.Assaulting][i] < 0)
                match.FirstAssault[objective.Assaulting][i] = playS;
        if (objective.Owner != TeamId(match.LastOwner[i]) && (objective.Owner == TEAM_ALLIANCE || objective.Owner == TEAM_HORDE))
            ++match.Captures[objective.Owner];
        match.LastOwner[i] = uint8(objective.Owner);
        for (uint32 team = 0; team < 2; ++team)
            if (match.Massed[team][i] < 0 && attackers[team] >= MassedMinAttackers && massing[team][i] * 2 >= attackers[team])
                match.Massed[team][i] = playS;
    }
    for (uint32 team = 0; team < 2; ++team)
        match.Score[team] = TeamScore(plan, bg, map, team);

    if (!match.NextLineMs)
        match.NextLineMs = sBgConfig.MetricsLineMs;
    if (match.PlayMs < match.NextLineMs)
        return;
    while (match.NextLineMs <= match.PlayMs)
        match.NextLineMs += sBgConfig.MetricsLineMs;
    for (uint32 team = 0; team < 2; ++team)
    {
        std::string objectivesText;
        for (size_t i = 0; i < objectives; ++i)
        {
            if (!assigned[team][i] && !dead[team][i] && !present[team][i])
                continue;
            objectivesText += Trinity::StringFormat("{}{} a{}{} p{}", objectivesText.empty() ? "" : "; ", ObjectiveName(plan, int8(i)),
                assigned[team][i], dead[team][i] ? Trinity::StringFormat("+{}d", dead[team][i]) : std::string(), present[team][i]);
        }
        // the coordinator's fallback share (docs/design/BG_COORDINATOR.md §5.3: over 15% flags the profile for redesign)
        uint32 fallbacks = 0, assignments = 0;
        Coordinator::FallbackCounts(plan, team, fallbacks, assignments);
        uint32 drops = 0, retaliations = 0, reengages = 0;
        Coordinator::TetherCounts(plan, team, drops, retaliations, reengages);
        match.LastLine[team] = Trinity::StringFormat("{} instance {} play {} s {}: score {}, deaths {} (+{}), captures {}, attackers {}, fallbacks {}/{}, "
            "tether drops {} retaliations {} again {}; {}",
            match.Name, plan.InstanceId, playS, TeamWord(team), match.Score[team], match.Deaths[team], match.MinuteDeaths[team],
            match.Captures[team], attackers[team], fallbacks, assignments, drops, retaliations, reengages, objectivesText.empty() ? "-" : objectivesText);
        TC_LOG_INFO("server.playerbots.bg", "Playerbots: bg metrics map {} {}", plan.MapId, match.LastLine[team]);
        match.MinuteDeaths[team] = 0;
    }
}

void Close(BgPlan const& plan, Battleground* bg, char const* why)
{
    if (auto trace = TraceMatches.find({ plan.MapId, plan.InstanceId }); trace != TraceMatches.end())
    {
        for (ObjectGuid guid : trace->second.Bots)
            TraceBots.erase(guid);
        TraceMatches.erase(trace);
    }

    auto it = Matches.find({ plan.MapId, plan.InstanceId });
    if (it == Matches.end())
        return;
    Match& match = it->second;

    int8 winner = -1;
    if (bg && bg->GetStatus() == STATUS_WAIT_LEAVE)
    {
        PvPTeamId pvpWinner = bg->GetWinner();
        winner = pvpWinner == PVP_TEAM_ALLIANCE ? int8(TEAM_ALLIANCE) : pvpWinner == PVP_TEAM_HORDE ? int8(TEAM_HORDE) : -1;
    }
    TC_LOG_INFO("server.playerbots.bg", "Playerbots: bg match map {} {} instance {} play {} s, {} ({}), plans {}, score A {} H {}, deaths A {} H {}, "
        "captures A {} H {}; first assault A: {}; H: {}; massed A: {}; H: {}", plan.MapId, match.Name, plan.InstanceId, match.PlayMs / 1000,
        winner < 0 ? "no winner" : winner == TEAM_ALLIANCE ? "Alliance won" : "Horde won", why, match.Plans, match.Score[0], match.Score[1],
        match.Deaths[0], match.Deaths[1], match.Captures[0], match.Captures[1], Firsts(plan, match.FirstAssault[0]), Firsts(plan, match.FirstAssault[1]),
        Firsts(plan, match.Massed[0]), Firsts(plan, match.Massed[1]));

    MapTotals& totals = Totals[plan.MapId];
    totals.Name = match.Name;
    ++totals.Matches;
    if (winner < 0)
        ++totals.Undecided;
    else
        ++totals.Wins[winner];
    totals.PlayMs += match.PlayMs;
    Matches.erase(it);
}

std::vector<std::string> Report()
{
    std::vector<std::string> lines;
    if (!sBgConfig.Metrics)
        lines.push_back("Playerbots.Bg.Metrics is off: nothing is counted.");
    for (auto const& [mapId, totals] : Totals)
        lines.push_back(Trinity::StringFormat("{} (map {}): {} matches since boot, Alliance won {}, Horde won {}, no winner {}, play {} s on average",
            totals.Name, mapId, totals.Matches, totals.Wins[0], totals.Wins[1], totals.Undecided,
            totals.Matches ? uint32(totals.PlayMs / totals.Matches / 1000) : 0u));
    for (auto const& [key, match] : Matches)
        for (uint32 team = 0; team < 2; ++team)
            if (!match.LastLine[team].empty())
                lines.push_back("  " + match.LastLine[team]);
    if (lines.empty())
        lines.push_back("No battleground metrics yet.");
    return lines;
}

void Trace(BgPlan const& plan, Battleground* bg, Map* map, uint32 now)
{
    if (sBgConfig.Trace.empty() || !bg || !map || !plan.Def || bg->GetStatus() != STATUS_IN_PROGRESS)
        return;
    int8 objective = TraceObjective(plan);
    if (objective == NoObjective)
        return;
    TraceMatch& match = TraceMatches[{ plan.MapId, plan.InstanceId }];
    if (match.PickedAtPlan != plan.Ticks)
    {
        match.PickedAtPlan = plan.Ticks;
        PickTraced(plan, map, objective, match, uint32(bg->GetInProgressDuration().count() / 1000));
    }
    for (ObjectGuid guid : match.Bots)
    {
        Player* bot = ObjectAccessor::GetPlayer(map, guid);
        auto state = TraceBots.find(guid);
        if (!bot || state == TraceBots.end())
            continue;
        SampleMovement(bot, state->second);
        if (now - state->second.LastLineMs < sBgConfig.TraceMs)
            continue;
        WriteTrace(plan, bg, objective, bot, state->second, now);
        TraceBot& next = state->second;
        next.LastLineMs = now;
        next.Launches = next.Restarts = next.SameRestarts = next.GoCalls = next.GoNoPath = 0;
    }
}

void NoteGo(Player const* bot, Position const& where, bool noPath)
{
    if (sBgConfig.Trace.empty())
        return;
    auto it = TraceBots.find(bot->GetGUID());
    if (it == TraceBots.end())
        return;
    ++it->second.GoCalls;
    if (noPath)
        ++it->second.GoNoPath;
    it->second.GoTo = where;
    it->second.HasGo = true;
}
}
