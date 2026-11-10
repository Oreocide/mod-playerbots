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
#include "Battleground/BgMapData.h"
#include "Battleground.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "StringFormat.h"
#include <map>
#include <unordered_map>

namespace Playerbots::Bg::Metrics
{
namespace
{
constexpr float PresentRange = 40.0f;       // a player this close to an objective is present there (the coordinator's counting pass)
constexpr uint32 LineEveryMs = 60000;       // one line per team per minute of play
constexpr uint32 MassedMinAttackers = 4;    // a team with fewer living attackers never counts as massed anywhere

struct Match
{
    std::string Name;
    uint32 LastSampleMs = 0;
    uint32 PlayMs = 0;                      // Battleground::GetInProgressDuration on the last sample (play time, no preparation)
    uint32 NextLineMs = LineEveryMs;
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

    if (match.PlayMs < match.NextLineMs)
        return;
    while (match.NextLineMs <= match.PlayMs)
        match.NextLineMs += LineEveryMs;
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
        match.LastLine[team] = Trinity::StringFormat("{} instance {} play {} s {}: score {}, deaths {} (+{}), captures {}, attackers {}; {}",
            match.Name, plan.InstanceId, playS, TeamWord(team), match.Score[team], match.Deaths[team], match.MinuteDeaths[team],
            match.Captures[team], attackers[team], objectivesText.empty() ? "-" : objectivesText);
        TC_LOG_INFO("server.playerbots.bg", "Playerbots: bg metrics map {} {}", plan.MapId, match.LastLine[team]);
        match.MinuteDeaths[team] = 0;
    }
}

void Close(BgPlan const& plan, Battleground* bg, char const* why)
{
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
}
