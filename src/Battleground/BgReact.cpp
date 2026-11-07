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

#include "Battleground/BgReact.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgTalk.h"
#include "Battleground.h"
#include "BotSession.h"
#include "DB2Structure.h"
#include "GameObject.h"
#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <map>

namespace Playerbots::Bg
{
namespace
{
using BgRules::Overlay;
using BgRules::Urgency;

constexpr float ThreatRange = 40.0f;                // enemies and allies this close to a node count for it
constexpr uint32 ThreatCap = 6;                     // at most this many more are sent to one node
constexpr uint32 ThreatAttackers = 2;               // attackers one threat may take: twice that at half time, all in reach at the end
constexpr uint32 GiveUpRatio = 2;                   // twice as many enemies as we could ever have there: nobody is sent
constexpr uint32 GiveUpMinEnemies = 4;
constexpr uint32 RespondHoldMs = 15000;             // a helper keeps its errand this long after the threat was last seen
constexpr uint32 ValleyAssaultMs = 4 * 60 * 1000;   // Alterac Valley: an assault takes the node after 4 minutes
constexpr uint32 LastSecondsMs = 20000;
constexpr float FallBackRange = 40.0f;
constexpr uint32 FallBackRatio = 3;
constexpr uint32 FallBackMinEnemies = 3;
constexpr uint32 FallBackMs = 8000;
constexpr uint32 FallBackReArmMs = 15000;           // after a fall back, not another one for this long
constexpr float FallBackMinRun = 30.0f;             // a place to fall back to is at least this far (closer is the same fight)
constexpr float FallBackMaxRun = 150.0f;
constexpr float GroupRadius = 20.0f;                // a group to run to: an ally with two more allies this close
constexpr uint32 TurtlePct = 20;                    // #4: a team ahead defends with this much more of its bots (at most 70%)
constexpr uint32 AllInPct = 10;                     // #4: a team behind late keeps at most this share home
constexpr uint32 HarassPct = 30;                    // #4: turtling on a majority, at most this share attacks
constexpr uint32 StanceHoldMs = 20000;              // a new stance is taken once it held this long
constexpr uint32 FlagTimeLimitMs = 17 * 60 * 1000;  // Warsong Gulch and Twin Peaks end at this much elapsed time (their scripts)
constexpr float BrawlRadius = 25.0f;                // #15: enemies this close to each other are one group
constexpr uint32 MarkEveryMs = 5000;                // #7: the marks are looked at no more often than this
constexpr uint8 SkullIcon = 7;                      // TARGET_ICONS: Star 0 ... Cross 6, Skull 7
constexpr uint8 CrossIcon = 6;
constexpr float MarkCrowd = 40.0f;                  // #7: the healer marked is the one with the most of us this close
constexpr float CallRange = 60.0f;                  // #9: a bot calls a node by name when it stands this close to it
constexpr uint32 DefendCallBots = 3;                // #10: "def st" sends this many
constexpr uint32 StallMs = 3 * 60 * 1000;           // #13: no node changed hands this long: the front stalls
constexpr uint32 MineMinAttackers = 10;             // #13: a team sends a mine group only with this many free attackers
constexpr uint32 MineGroup = 4;
constexpr uint32 MineMs = 4 * 60 * 1000;            // #13: a mine errand gives up after this
constexpr uint32 MineBlockMs = 5 * 60 * 1000;       // #13: and that mine is not tried again for this long
constexpr uint32 StyleSkill = 80;                   // #12 without #20: a fifth of the bots play poorly
constexpr float TurtleCallRange = 80.0f;            // #12: a turtle leaves its node only for a threat this close
constexpr float BrawlerCallRange = 80.0f;           // #12: a brawler answers only a threat this close
constexpr uint32 LaneDeaths = 3;                    // #19: three deaths
constexpr uint32 LaneWindowMs = 10 * 60 * 1000;     // #19: within 10 minutes
constexpr float LaneRadius = 40.0f;                 // #19: within 40 yd of each other
constexpr float LaneSpotRange = 80.0f;              // #19: the objective avoided is the one nearest the spot, this close
constexpr uint32 LaneAvoidMs = 4 * 60 * 1000;       // #19: for 4 minutes
constexpr uint32 NinjaMs = 3 * 60 * 1000;           // #17: a ninja gives up after this
constexpr uint32 NinjaGuarded = 2;                  // #17: this many enemies at its node: the sneak failed
constexpr uint32 NinjaStealth = 1784;               // #17: rogue Stealth
constexpr uint32 NinjaProwl = 5215;                 // #17: druid Prowl (in Cat Form 768)
constexpr uint32 NinjaCatForm = 768;

bool Live(uint32 until, uint32 now)
{
    return int32(until - now) > 0;
}

/// The node race maps: points per tick by the bases a team holds, and the tick (battleground_arathi_basin.cpp,
/// battleground_battle_for_gilneas.cpp, battleground_eye_of_the_storm.cpp; the Eye flag pays extra, left out)
struct RaceRates
{
    uint32 TickMs;
    std::vector<uint32> Points;
    bool NeverLast;                 // holding all but one, nobody chases the last one (Arathi Basin, Battle for Gilneas)
};

RaceRates const* RatesOf(uint32 mapId)
{
    static RaceRates const arathi{ 2000, { 0, 2, 3, 4, 7, 60 }, true };
    static RaceRates const gilneas{ 1000, { 0, 1, 3, 30 }, true };
    static RaceRates const eye{ 2000, { 0, 1, 2, 5, 10 }, false };
    switch (mapId)
    {
        case 2107: return &arathi;
        case 761: return &gilneas;
        case 566: return &eye;
        default: return nullptr;
    }
}

/// Bases a team holds and scores with (a node under assault scores for nobody)
uint32 HeldBases(BgPlan const& plan, uint32 team)
{
    uint32 held = 0;
    for (Objective const& objective : plan.Objectives)
        if (objective.Known && objective.Owner == TeamId(team) && objective.Assaulting == TEAM_NEUTRAL)
            ++held;
    return held;
}

/// #15: the biggest group of living enemies: where the enemy with the most others within BrawlRadius stands (a real spot on the
/// ground, so the mesh path finds it)
bool BiggestGroup(BgPlan const& plan, TeamId them, Position& out)
{
    size_t best = plan.Picture.size();
    uint32 bestCount = 0;
    for (size_t i = 0; i < plan.Picture.size(); ++i)
    {
        Seen const& seen = plan.Picture[i];
        if (!seen.Alive || seen.Team != them)
            continue;
        uint32 count = 0;
        for (Seen const& other : plan.Picture)
            if (other.Alive && other.Team == them && other.Pos.GetExactDist2d(seen.Pos) <= BrawlRadius)
                ++count;
        if (count > bestCount)
        {
            best = i;
            bestCount = count;
        }
    }
    if (best == plan.Picture.size())
        return false;
    out = plan.Picture[best].Pos;
    return true;
}

/// #7: the bot that leads the team's battleground raid marks their carrier (Skull) and the enemy healer with the most of us
/// around (Cross), as a leader does; a raid a player leads keeps its own marks. Marks change only when the unit does
void MarkTargets(BgPlan& plan, uint32 team, std::vector<Player*> const& bots, uint32 now)
{
    TeamPlan& teamPlan = plan.Teams[team];
    if (now - teamPlan.LastMarkMs < MarkEveryMs)
        return;
    teamPlan.LastMarkMs = now;

    Group* group = nullptr;
    for (Player* bot : bots)
        if (Group* raid = bot->GetGroup(); raid && raid->isBGGroup())
        {
            group = raid;
            break;
        }
    if (!group)
        return;
    ObjectGuid leader = group->GetLeaderGUID();
    if (std::none_of(bots.begin(), bots.end(), [&leader](Player* bot) { return bot->GetGUID() == leader; }))
        return;

    TeamId us = TeamId(team), them = TeamId(1 - team);
    ObjectGuid skull;
    for (Seen const& seen : plan.Picture)
        if (seen.Alive && seen.Team == them && seen.Carrier)
            skull = seen.Guid;
    ObjectGuid cross;
    uint32 most = 1;                    // at least two of us near it
    for (Seen const& seen : plan.Picture)
    {
        if (!seen.Alive || seen.Team != them || !seen.Healer || seen.Guid == skull)
            continue;
        uint32 around = 0;
        for (Seen const& other : plan.Picture)
            if (other.Alive && other.Team == us && other.Pos.GetExactDist2d(seen.Pos) <= MarkCrowd)
                ++around;
        if (around > most)
        {
            most = around;
            cross = seen.Guid;
        }
    }
    if (skull != teamPlan.Marks[0])
    {
        group->SetTargetIcon(SkullIcon, skull, leader);
        teamPlan.Marks[0] = skull;
        ++teamPlan.MarksSet;
    }
    if (cross != teamPlan.Marks[1])
    {
        group->SetTargetIcon(CrossIcon, cross, leader);
        teamPlan.Marks[1] = cross;
        ++teamPlan.MarksSet;
    }
}

/// A node the team can hold (Valley: graveyards and standing towers; the code tables: every objective)
bool HoldableNode(BgPlan const& plan, size_t index)
{
    if (plan.Def->Kind != MapKind::Valley)
        return plan.Def->Kind != MapKind::Flag;
    NodeDef const* row = RowOf(plan, int8(index));
    return row && row->IsNode() && !plan.Objectives[index].Destroyed;
}

/// The objective nearest the bot within range, for a call ("help st"): nodes, flag rooms, bosses; NoObjective when none
int8 NearestNode(BgPlan const& plan, Player const* bot, float range)
{
    int8 best = NoObjective;
    float bestDist = range;
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (!objective.Known)
            continue;
        if (NodeDef const* row = RowOf(plan, int8(i)); row && !row->IsNode() && !row->IsBoss())
            continue;
        float dist = bot->GetExactDist2d(objective.Pos);
        if (dist < bestDist)
        {
            best = int8(i);
            bestDist = dist;
        }
    }
    return best;
}

/// Where an outnumbered bot runs: the nearest group of ours (an ally with two more allies close to it, not losing its own
/// fight) 30 to 150 yd away, else the nearest node we hold, else (flag maps) our flag room
bool FallBackPoint(BgPlan const& plan, Player const* bot, TeamId us, Position& out)
{
    bool found = false;
    float bestDist = 0.0f;
    for (Seen const& seen : plan.Picture)
    {
        if (!seen.Alive || seen.Team != us || seen.Guid == bot->GetGUID())
            continue;
        float dist = bot->GetExactDist2d(seen.Pos);
        if (dist < FallBackMinRun || dist > FallBackMaxRun || (found && dist >= bestDist))
            continue;
        uint32 friends = 0, foes = 0;
        for (Seen const& other : plan.Picture)
            if (other.Alive && other.Guid != seen.Guid && other.Pos.GetExactDist2d(seen.Pos) <= GroupRadius)
                ++(other.Team == us ? friends : foes);
        if (friends < 2 || foes > friends)
            continue;
        out = seen.Pos;
        bestDist = dist;
        found = true;
    }
    if (found)
        return true;

    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (!objective.Known || objective.Owner != us || !HoldableNode(plan, i))
            continue;
        float dist = bot->GetExactDist2d(objective.Pos);
        if (dist < FallBackMinRun || (found && dist >= bestDist))
            continue;
        out = objective.Pos;
        bestDist = dist;
        found = true;
    }
    if (found)
        return true;

    if (plan.Def->Kind == MapKind::Flag && size_t(us) < plan.Objectives.size() && plan.Objectives[us].Known
        && bot->GetExactDist2d(plan.Objectives[us].Pos) >= FallBackMinRun)
    {
        out = plan.Objectives[us].Pos;
        return true;
    }
    return false;
}
}

bool IsHealer(Player const* player)
{
    if (IsBot(player))
    {
        if (PlayerbotAI* ai = sPlayerbotMgr->GetAI(player))
            return ai->GetRole() == BotRole::Healer;
        return false;
    }
    ChrSpecializationEntry const* spec = player->GetPrimarySpecializationEntry();
    return spec && spec->Role == 1;
}

void UpdateStances(BgPlan& plan, Battleground const* bg, Map const* map, uint32 now)
{
    bool scoreClock = sBgConfig.ScoreClock, endgame = sBgConfig.Endgame;
    for (uint32 team = 0; team < 2; ++team)
    {
        TeamPlan& teamPlan = plan.Teams[team];
        if (!scoreClock && !endgame)
        {
            teamPlan.Stance = teamPlan.PendingStance = BgRules::Stance::Normal;
            teamPlan.Outlook = BgRules::Outlook();
            continue;
        }

        BgRules::OutlookParams params;
        BgRules::Outlook outlook;
        TeamId us = TeamId(team), them = TeamId(1 - team);
        switch (plan.Def->Kind)
        {
            case MapKind::Flag:
                outlook = BgRules::ProjectCaptures(bg->GetTeamScore(us), bg->GetTeamScore(them), bg->GetElapsedTime(), FlagTimeLimitMs, params);
                break;
            case MapKind::Valley:
            {
                uint32 start = plan.Def->ScoreMaxWs ? uint32(std::max(0, map->GetWorldStateValue(int32(plan.Def->ScoreMaxWs)))) : 0;
                outlook = BgRules::ProjectReinforcements(TeamScore(plan, bg, map, us), TeamScore(plan, bg, map, them), start, params);
                break;
            }
            default:
                if (RaceRates const* rates = RatesOf(plan.MapId))
                {
                    auto seconds = [&](uint32 side)
                    {
                        uint32 held = std::min<uint32>(HeldBases(plan, side), uint32(rates->Points.size() - 1));
                        return BgRules::SecondsToWin(bg->GetTeamScore(TeamId(side)), plan.Def->WinScore, rates->Points[held], rates->TickMs);
                    };
                    outlook = BgRules::ProjectRace(seconds(team), seconds(1 - team), params);
                }
                break;
        }
        teamPlan.Outlook = outlook;

        // a new stance is taken once plans StanceHoldMs apart agree on it, so a score swing of one tick does not flip the team
        BgRules::Stance want = BgRules::PickStance(outlook, scoreClock, endgame);
        if (want == teamPlan.Stance)
            teamPlan.PendingStance = want;
        else if (want != teamPlan.PendingStance)
        {
            teamPlan.PendingStance = want;
            teamPlan.PendingSinceMs = now;
        }
        else if (now - teamPlan.PendingSinceMs >= StanceHoldMs)
        {
            teamPlan.Stance = want;
            teamPlan.StanceSinceMs = now;
        }
    }
}

uint32 StanceDefendPct(TeamPlan const& teamPlan)
{
    return BgRules::StanceDefendPct(teamPlan.Stance, teamPlan.DefendPct, TurtlePct, AllInPct);
}

CallOrder const* AttackOrder(BgPlan const& plan, uint32 team, uint32 now)
{
    if (!sBgConfig.HumanCalls || team > 1)
        return nullptr;
    TeamId us = TeamId(team), them = TeamId(1 - team);
    // "everyone home" names our own flag room, a Horde player's "everyone to drek" their own general: no attack order
    auto targetable = [&](int8 objective)
    {
        if (objective < 0 || size_t(objective) >= plan.Objectives.size())
            return false;
        Objective const& target = plan.Objectives[objective];
        switch (plan.Def->Kind)
        {
            case MapKind::Flag:
                return objective == int8(them);
            case MapKind::Valley:
            {
                NodeDef const* row = RowOf(plan, objective);
                if (!row)
                    return false;
                if (row->IsBoss())
                    return BgRules::TeamOfSide(row->Side) == uint8(them) && target.Alive;
                return row->IsNode() && !target.Destroyed && target.Owner != us;
            }
            default:
                return target.Known && target.Owner != us;
        }
    };
    TeamPlan const& teamPlan = plan.Teams[team];
    for (auto it = teamPlan.Calls.rbegin(); it != teamPlan.Calls.rend(); ++it)
        if (Live(it->UntilMs, now) && (it->Kind == BgRules::CallKind::Attack || it->Kind == BgRules::CallKind::Everyone) && targetable(it->Objective))
            return &*it;
    return nullptr;
}

void PrepareTeam(BgPlan& plan, uint32 team, std::vector<Player*> const& members, std::vector<Player*> const& bots, int32 streak)
{
    TeamPlan& teamPlan = plan.Teams[team];
    bool human = std::any_of(members.begin(), members.end(), [](Player* player) { return !IsBot(player); });
    teamPlan.HadHuman |= human;

    // #20: decision quality only (who answers a threat, who takes a focus switch, how many play poorly), never stats
    if (!sBgConfig.TeamSkill)
    {
        teamPlan.Skill = 100;
        teamPlan.SkillRolled = false;
    }
    else if (!teamPlan.SkillRolled && plan.Seeded)
    {
        uint32 skill = BgRules::RollSkill(teamPlan.Seed, sBgConfig.TeamSkillMin, sBgConfig.TeamSkillMax);
        if (human)
            skill = BgRules::SkillAfterStreak(skill, streak, sBgConfig.TeamSkillStreakStep, sBgConfig.TeamSkillMin, sBgConfig.TeamSkillMax);
        teamPlan.Skill = skill;
        teamPlan.SkillRolled = true;
    }

    // #12: each bot's style for the match, from the team's seed (so the same bot plays the same way all match)
    bool valley = plan.Def->Kind == MapKind::Valley;
    for (Player* bot : bots)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        if (!sBgConfig.Archetypes)
        {
            job.Style = BgRules::Archetype::Objective;
            job.Styled = false;
            continue;
        }
        if (job.Styled || !plan.Seeded)
            continue;
        PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
        bool healer = ai && ai->GetRole() == BotRole::Healer;
        bool stealth = bot->GetClass() == CLASS_ROGUE || bot->GetClass() == CLASS_DRUID;
        job.Style = BgRules::PickArchetype(teamPlan.Seed, bot->GetGUID().GetCounter(), healer, stealth, valley,
            sBgConfig.TeamSkill ? teamPlan.Skill : StyleSkill);
        job.Styled = true;
    }
}

uint32 TurtleCap(BgPlan const& plan, uint32 team, uint32 bots)
{
    BgRules::Stance stance = plan.Teams[team].Stance;
    if (stance != BgRules::Stance::Turtle && stance != BgRules::Stance::Finish)
        return bots;
    RaceRates const* rates = RatesOf(plan.MapId);
    if (!rates)
        return bots;
    uint32 total = 0;
    for (Objective const& objective : plan.Objectives)
        total += objective.Known ? 1 : 0;
    return BgRules::TurtleAttackers(bots, HeldBases(plan, team), total, HarassPct, rates->NeverLast);
}

void BuildPicture(BgPlan& plan, Map* map, uint32 /*now*/)
{
    plan.Picture.clear();
    for (MapReference const& ref : map->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (!player || !player->IsInWorld() || player->IsGameMaster())
            continue;
        TeamId team = TeamOf(player);
        if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
            continue;
        Seen seen;
        seen.Guid = player->GetGUID();
        seen.Team = team;
        seen.Pos = player->GetPosition();
        seen.Alive = player->IsAlive();
        seen.Bot = IsBot(player);
        seen.Healer = IsHealer(player);
        seen.HealthPct = player->GetHealthPct();
        seen.Class = player->GetClass();
        plan.Picture.push_back(seen);
    }
    for (FlagInfo const& flag : plan.Flags)
        if (!flag.Carrier.IsEmpty())
            for (Seen& seen : plan.Picture)
                if (seen.Guid == flag.Carrier)
                    seen.Carrier = true;

    for (Objective& objective : plan.Objectives)
    {
        objective.Near[0] = objective.Near[1] = 0;
        if (!objective.Known)
            continue;
        for (Seen const& seen : plan.Picture)
            if (seen.Alive && seen.Pos.GetExactDist2d(objective.Pos) <= ThreatRange)
                ++objective.Near[seen.Team];
    }
}

uint32 ReactTeam(BgPlan& plan, uint32 team, std::vector<Player*> const& bots, std::string& reason, uint32 now)
{
    TeamPlan& teamPlan = plan.Teams[team];
    TeamId us = TeamId(team);
    TeamId them = TeamId(1 - team);
    bool valley = plan.Def->Kind == MapKind::Valley;
    teamPlan.Threats.clear();
    uint32 started = 0;

    auto onPush = [&](Assignment const& job)
    {
        return valley && job.Base == Job::Attack && job.Objective != NoObjective && job.Objective == teamPlan.RallyObjective
            && BgRules::PushActive(teamPlan.Rally.State);
    };
    auto charged = [&](Assignment const& job) { return onPush(job) && BgRules::PushCharged(teamPlan.Rally.State); };

    // #19: a bot whose last three deaths in 10 minutes were at one spot takes another objective for a while; with no other
    // target to attack it helps at the nearest node of ours instead
    for (Player* bot : bots)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        bool alive = bot->IsAlive();
        // deaths on a boss push are the push's business (a wipe regroups at the rally point), not a lane that kills the bot
        if (job.WasAlive && !alive && sBgConfig.LaneChange && !onPush(job))
        {
            std::vector<BgRules::Death>& deaths = plan.Deaths[bot->GetGUID()];
            deaths.push_back({ { bot->GetPositionX(), bot->GetPositionY() }, now });
            if (deaths.size() > LaneDeaths)
                deaths.erase(deaths.begin());
            if (BgRules::DiesAtOneSpot(deaths, now, LaneWindowMs, LaneRadius, LaneDeaths))
            {
                int8 spot = NearestNode(plan, bot, LaneSpotRange);
                if (spot != NoObjective)
                {
                    job.AvoidObjective = spot;
                    job.AvoidUntilMs = now + LaneAvoidMs;
                    ++teamPlan.LaneChanges;
                    ++started;
                    if (reason.empty())
                        reason = Trinity::StringFormat("{} died three times at {}: another lane", bot->GetName(), plan.Objectives[spot].Name);
                    bool otherTarget = std::any_of(teamPlan.AttackTargets.begin(), teamPlan.AttackTargets.end(), [spot](int8 t) { return t != spot; });
                    if (!otherTarget && job.Base == Job::Attack)
                    {
                        int8 home = NoObjective;
                        float bestDist = 0.0f;
                        for (int8 target : teamPlan.DefendTargets)
                            if (target != spot && size_t(target) < plan.Objectives.size())
                                if (float dist = bot->GetExactDist2d(plan.Objectives[target].Pos); home == NoObjective || dist < bestDist)
                                {
                                    home = target;
                                    bestDist = dist;
                                }
                        if (home != NoObjective)
                        {
                            job.Respond = home;
                            job.RespondUntilMs = job.AvoidUntilMs;
                        }
                    }
                }
                deaths.clear();
            }
        }
        job.WasAlive = alive;
        if (!sBgConfig.LaneChange)
            job.AvoidUntilMs = 0;
    }
    if (plan.Deaths.size() > 256)
        plan.Deaths.clear();

    // #8: outnumbered three to one with at least three enemies around: back to friends for a few seconds, then not again for a while
    if (sBgConfig.FallBack && !plan.Picture.empty())
        for (Player* bot : bots)
        {
            Assignment& job = plan.Jobs[bot->GetGUID()];
            if (!bot->IsAlive() || job.Now == Job::Carrier || job.Now == Job::Escort || charged(job) || Live(job.FallBackUntilMs, now)
                || Live(job.FallBackReadyMs, now))
                continue;
            uint32 enemies = 0, allies = 0;
            for (Seen const& seen : plan.Picture)
                if (seen.Alive && bot->GetExactDist2d(seen.Pos) <= FallBackRange)
                    ++(seen.Team == us ? allies : enemies);
            if (!BgRules::Outnumbered(enemies, allies, FallBackRatio, FallBackMinEnemies))
                continue;
            Position to;
            if (!FallBackPoint(plan, bot, us, to))
                continue;
            job.FallBackTo = to;
            job.FallBackUntilMs = now + FallBackMs;
            job.FallBackReadyMs = now + FallBackMs + FallBackReArmMs;
            ++teamPlan.FallBacks;
            ++started;
            if (reason.empty())
                reason = Trinity::StringFormat("{} falls back ({} against {})", bot->GetName(), allies, enemies);
            // #9: and calls for help where it stands
            int8 near = NearestNode(plan, bot, CallRange);
            std::string place = near != NoObjective ? CallName(plan, near) : std::string();
            Say(plan, team, bot, place.empty() ? "need help" : "help " + place, "help:" + (place.empty() ? std::string(bot->GetName()) : place), now);
        }

    // #10: the calls of the team's real players that still hold
    std::erase_if(teamPlan.Calls, [now](CallOrder const& order) { return !Live(order.UntilMs, now); });
    bool calls = sBgConfig.HumanCalls && !teamPlan.Calls.empty();

    // #1 and #2: a node of ours with more enemies than allies at it, or under their assault (someone has to click it back); a
    // player's "inc lm 3" or "help bs" counts as that many enemies at that node for a while (#10)
    if ((sBgConfig.ThreatResponse || sBgConfig.RetakeTimers || calls) && !plan.Picture.empty())
    {
        std::vector<Threat> threats;
        for (int8 target : teamPlan.DefendTargets)
        {
            if (target < 0 || size_t(target) >= plan.Objectives.size())
                continue;
            Objective const& objective = plan.Objectives[target];
            Threat threat;
            threat.Objective = target;
            threat.Enemies = objective.Near[them];
            threat.Allies = objective.Near[us];
            if (sBgConfig.RetakeTimers && objective.Owner == us && objective.Assaulting == them)
            {
                uint32 total = valley ? ValleyAssaultMs : objective.AssaultTotalMs;
                threat.LeftMs = valley ? BgRules::TimeLeft(objective.AssaultSinceMs, now, total) : objective.AssaultLeftMs;
                threat.Urgency = BgRules::RetakeUrgency(threat.LeftMs, total, LastSecondsMs);
            }
            bool called = false;
            if (calls)
                for (CallOrder const& order : teamPlan.Calls)
                    if (order.Objective == target && (order.Kind == BgRules::CallKind::Incoming || order.Kind == BgRules::CallKind::Help))
                    {
                        threat.Enemies = std::max<uint32>(threat.Enemies, std::max<uint32>(order.Count, 2));
                        called = true;
                    }
            threat.Need = sBgConfig.ThreatResponse || called ? BgRules::ThreatNeed(threat.Enemies, threat.Allies, ThreatCap) : 0;
            if (threat.Urgency != Urgency::None && threat.Allies == 0)
                threat.Need = std::max<uint32>(threat.Need, 1);
            if (threat.Need || threat.Urgency != Urgency::None)
                threats.push_back(threat);
        }

        // defenders per node (each node keeps one), and which nodes are threatened (their defenders stay)
        std::map<int8, uint32> defendersAt;
        for (Player* bot : bots)
            if (Assignment const& job = plan.Jobs[bot->GetGUID()]; bot->IsAlive() && job.Base == Job::Defend && job.Respond == NoObjective)
                ++defendersAt[job.Objective];
        auto threatened = [&threats](int8 objective)
        {
            return std::any_of(threats.begin(), threats.end(), [objective](Threat const& threat) { return threat.Objective == objective; });
        };
        // each threat is one event from its start: a bot's chance to answer it is rolled once for the whole threat (#20)
        for (int8 target : teamPlan.DefendTargets)
            if (target >= 0 && size_t(target) < plan.Objectives.size())
            {
                uint32& since = plan.Objectives[target].ThreatSinceMs[team];
                since = !threatened(target) ? 0 : since ? since : now;
            }

        for (Threat& threat : threats)
        {
            Objective const& objective = plan.Objectives[threat.Objective];

            // the bots already on this errand keep it while the threat lasts
            uint32 enRoute = 0;
            for (Player* bot : bots)
            {
                Assignment& job = plan.Jobs[bot->GetGUID()];
                if (job.Respond != threat.Objective || !Live(job.RespondUntilMs, now) || !bot->IsAlive())
                    continue;
                job.RespondUntilMs = now + RespondHoldMs;
                ++threat.Sent;
                if (bot->GetExactDist2d(objective.Pos) > ThreatRange)
                    ++enRoute;
            }

            // who could go: defenders of other nodes that are not threatened themselves and keep one, then a few attackers
            uint32 attackerLimit = threat.Urgency == Urgency::Last ? uint32(bots.size()) : threat.Urgency == Urgency::Half
                ? ThreatAttackers * 2 : ThreatAttackers;
            std::vector<std::pair<float, Player*>> candidates;
            for (Player* bot : bots)
            {
                Assignment const& job = plan.Jobs[bot->GetGUID()];
                if (!bot->IsAlive() || job.Now == Job::Carrier || job.Now == Job::Escort || onPush(job) || Live(job.FallBackUntilMs, now)
                    || (job.Respond != NoObjective && Live(job.RespondUntilMs, now)))
                    continue;
                if (job.Base == Job::Defend && (job.Objective == threat.Objective || threatened(job.Objective) || defendersAt[job.Objective] <= 1))
                    continue;
                if (job.Base != Job::Defend && job.Base != Job::Attack)
                    continue;
                float dist = bot->GetExactDist2d(objective.Pos);
                if (dist <= ThreatRange)
                    continue;               // already counted there
                if (threat.Urgency == Urgency::Last && !BgRules::InTime(dist, bot->GetSpeed(MOVE_RUN), threat.LeftMs))
                    continue;
                // #12: a turtle leaves its node, a brawler its fight, only for a threat close by
                if ((job.Style == BgRules::Archetype::Turtle && dist > TurtleCallRange)
                    || ((job.Style == BgRules::Archetype::Brawler || job.Style == BgRules::Archetype::RoadFighter) && dist > BrawlerCallRange))
                    continue;
                // #20: a team plays as well as its skill: this bot answers this threat or it does not
                if (sBgConfig.TeamSkill && !BgRules::EventRoll(teamPlan.Seed, bot->GetGUID().GetCounter(),
                    (uint64(uint8(threat.Objective)) << 32) | objective.ThreatSinceMs[team], teamPlan.Skill))
                    continue;
                candidates.emplace_back(dist, bot);
            }
            std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

            uint32 reachable = 0, attackers = 0;
            for (auto const& [dist, bot] : candidates)
            {
                bool attacker = plan.Jobs[bot->GetGUID()].Base == Job::Attack;
                if (attacker && attackers >= attackerLimit)
                    continue;
                attackers += attacker;
                ++reachable;
            }
            // #9: a defender at the node calls it out (once per call key, the team's chat limits hold)
            Player* local = nullptr;
            for (Player* bot : bots)
                if (bot->IsAlive() && bot->GetExactDist2d(objective.Pos) <= ThreatRange)
                {
                    local = bot;
                    break;
                }
            std::string name = CallName(plan, threat.Objective);

            if (threat.Enemies && BgRules::Hopeless(threat.Enemies, threat.Allies + enRoute, reachable, GiveUpRatio, GiveUpMinEnemies))
            {
                // never feed two into eight: the ones on the way go back to their jobs
                threat.GaveUp = true;
                for (Player* bot : bots)
                    if (Assignment& job = plan.Jobs[bot->GetGUID()]; job.Respond == threat.Objective)
                        job.RespondUntilMs = now;
                threat.Sent = 0;
                teamPlan.Threats.push_back(threat);
                if (local)
                    Say(plan, team, local, Trinity::StringFormat("{} lost, too many", name), "lost:" + name, now);
                continue;
            }
            if (local && threat.Enemies > threat.Allies)
                Say(plan, team, local, Trinity::StringFormat("inc {} {}", name, threat.Enemies), "inc:" + name, now);

            // the nearest go; at the last seconds everyone who gets there in time
            uint32 want = threat.Need > enRoute ? threat.Need - enRoute : 0;
            if (threat.Urgency == Urgency::Last)
                want = std::max<uint32>(want, reachable);
            uint32 sent = 0;
            attackers = 0;
            for (auto const& [dist, bot] : candidates)
            {
                if (sent >= want)
                    break;
                Assignment& job = plan.Jobs[bot->GetGUID()];
                bool attacker = job.Base == Job::Attack;
                if (attacker && attackers >= attackerLimit)
                    continue;
                if (!attacker)
                {
                    uint32& left = defendersAt[job.Objective];
                    if (left <= 1)
                        continue;
                    --left;
                }
                attackers += attacker;
                job.Respond = threat.Objective;
                job.RespondUntilMs = now + RespondHoldMs;
                ++sent;
                ++teamPlan.Responses;
                ++started;
            }
            threat.Sent += sent;
            if (sent && reason.empty())
                reason = Trinity::StringFormat("{}: {} against {}, {} sent{}", objective.Name, threat.Allies, threat.Enemies, sent,
                    threat.Urgency != Urgency::None ? Trinity::StringFormat(", their assault ends in {} s", threat.LeftMs / 1000) : std::string());
            teamPlan.Threats.push_back(threat);
        }
    }

    // #10: "def st": a few free attackers guard that node while the order holds
    if (calls)
        for (CallOrder const& order : teamPlan.Calls)
        {
            if (order.Kind != BgRules::CallKind::Defend || order.Objective < 0 || size_t(order.Objective) >= plan.Objectives.size())
                continue;
            Position const& pos = plan.Objectives[order.Objective].Pos;
            uint32 have = 0;
            std::vector<std::pair<float, Player*>> free;
            for (Player* bot : bots)
            {
                Assignment const& job = plan.Jobs[bot->GetGUID()];
                if (!bot->IsAlive())
                    continue;
                if (job.Respond == order.Objective && Live(job.RespondUntilMs, now))
                {
                    ++have;
                    continue;
                }
                if (job.Base != Job::Attack || job.Now == Job::Carrier || job.Now == Job::Escort || onPush(job) || Live(job.FallBackUntilMs, now)
                    || (job.Respond != NoObjective && Live(job.RespondUntilMs, now)))
                    continue;
                free.emplace_back(bot->GetExactDist2d(pos), bot);
            }
            std::sort(free.begin(), free.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            for (size_t i = 0; i < free.size() && have < DefendCallBots; ++i, ++have)
            {
                Assignment& job = plan.Jobs[free[i].second->GetGUID()];
                job.Respond = order.Objective;
                job.RespondUntilMs = order.UntilMs;
                ++teamPlan.Responses;
                ++started;
            }
        }

    // #13: while the front stalls, a few spare attackers kill the boss of a mine the team does not own
    if (sBgConfig.Mines && valley)
    {
        if (teamPlan.MineObjective != NoObjective && size_t(teamPlan.MineObjective) < plan.Objectives.size())
        {
            Objective& mine = plan.Objectives[teamPlan.MineObjective];
            bool ours = mine.Owner == us;
            if (ours || !Live(teamPlan.MineUntilMs, now))
            {
                std::string name = CallName(plan, teamPlan.MineObjective);
                if (ours)
                    ++teamPlan.MinesTaken;
                else
                    mine.BlockedUntilMs[team] = now + MineBlockMs;
                for (Player* bot : bots)
                    if (Assignment& job = plan.Jobs[bot->GetGUID()]; job.Errand == Overlay::Mine)
                    {
                        if (ours && bot->IsAlive())
                            Say(plan, team, bot, name + " is ours", "mine:" + name, now);
                        job.Errand = Overlay::None;
                        job.ErrandObjective = NoObjective;
                    }
                teamPlan.MineObjective = NoObjective;
            }
        }
        else
            teamPlan.MineObjective = NoObjective;

        if (teamPlan.MineObjective == NoObjective && now - plan.LastNodeChangeMs >= StallMs && now - plan.CreatedMs >= StallMs
            && !BgRules::PushCharged(teamPlan.Rally.State))
        {
            std::vector<Player*> free;
            for (Player* bot : bots)
            {
                Assignment const& job = plan.Jobs[bot->GetGUID()];
                if (bot->IsAlive() && job.Base == Job::Attack && job.Now == Job::Attack && !onPush(job) && job.Errand == Overlay::None
                    && !Live(job.FallBackUntilMs, now) && !(job.Respond != NoObjective && Live(job.RespondUntilMs, now)))
                    free.push_back(bot);
            }
            if (free.size() >= MineMinAttackers)
            {
                int8 best = NoObjective;
                for (size_t i = 0; i < plan.Def->Nodes.size() && i < plan.Objectives.size(); ++i)
                {
                    NodeDef const& row = plan.Def->Nodes[i];
                    Objective const& mine = plan.Objectives[i];
                    if (!row.IsMine() || mine.Owner == us || !mine.Alive || row.Weights[team] <= 0.0f || Live(mine.BlockedUntilMs[team], now))
                        continue;
                    if (best == NoObjective || row.Weights[team] > plan.Def->Nodes[best].Weights[team])
                        best = int8(i);
                }
                if (best != NoObjective)
                {
                    Position const& at = plan.Objectives[best].Pos;
                    std::sort(free.begin(), free.end(), [&at](Player* a, Player* b) { return a->GetExactDist2d(at) < b->GetExactDist2d(at); });
                    // the nearest, with one healer among them when the team has a free one
                    std::vector<Player*> group;
                    auto healer = [](Player* bot)
                    {
                        PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
                        return ai && ai->GetRole() == BotRole::Healer;
                    };
                    if (auto it = std::find_if(free.begin(), free.end(), healer); it != free.end())
                        group.push_back(*it);
                    for (Player* bot : free)
                        if (group.size() < MineGroup && std::find(group.begin(), group.end(), bot) == group.end() && !healer(bot))
                            group.push_back(bot);
                    teamPlan.MineObjective = best;
                    teamPlan.MineUntilMs = now + MineMs;
                    for (Player* bot : group)
                    {
                        Assignment& job = plan.Jobs[bot->GetGUID()];
                        job.Errand = Overlay::Mine;
                        job.ErrandObjective = best;
                        job.ErrandUntilMs = teamPlan.MineUntilMs;
                        ++started;
                    }
                    std::string name = CallName(plan, best);
                    if (reason.empty())
                        reason = Trinity::StringFormat("the front stalls: {} go for {}", group.size(), plan.Objectives[best].Name);
                    if (!group.empty())
                        Say(plan, team, group.back(), Trinity::StringFormat("{} of us going {}", group.size(), name), "going:" + name, now);
                }
            }
        }
    }

    // #17: one ninja per team: a stealth class (a ninja first, #12) sneaks to an enemy node nobody stands at, away from the fight
    if (sBgConfig.NinjaCaps && !plan.Picture.empty() && plan.Def->Kind != MapKind::Flag)
    {
        Player* ninja = nullptr;
        for (Player* bot : bots)
            if (Assignment const& job = plan.Jobs[bot->GetGUID()]; job.Errand == Overlay::Ninja && Live(job.ErrandUntilMs, now))
                ninja = bot;
        if (ninja)
        {
            Assignment& job = plan.Jobs[ninja->GetGUID()];
            bool valid = job.ErrandObjective >= 0 && size_t(job.ErrandObjective) < plan.Objectives.size();
            // it guards its assault until the node is ours: the call for help goes out as the assault starts
            if (valid && plan.Objectives[job.ErrandObjective].Assaulting == us)
            {
                std::string name = CallName(plan, job.ErrandObjective);
                Say(plan, team, ninja, Trinity::StringFormat("taking {}, send someone", name), "ninja:" + name, now);
            }
            bool taken = valid && plan.Objectives[job.ErrandObjective].Owner == us && plan.Objectives[job.ErrandObjective].Assaulting == TEAM_NEUTRAL;
            if (!valid || taken || plan.Objectives[job.ErrandObjective].Near[them] >= NinjaGuarded || !ninja->IsAlive())
            {
                if (taken)
                    ++teamPlan.NinjaCaps;
                job.Errand = Overlay::None;
                job.ErrandObjective = NoObjective;
            }
        }
        else
        {
            // the target: an enemy node with nobody at it, not one we attack, farthest from where our attackers are
            float x = 0.0f, y = 0.0f;
            uint32 n = 0;
            for (Player* bot : bots)
                if (bot->IsAlive() && plan.Jobs[bot->GetGUID()].Base == Job::Attack)
                {
                    x += bot->GetPositionX();
                    y += bot->GetPositionY();
                    ++n;
                }
            Position centre(n ? x / n : 0.0f, n ? y / n : 0.0f, 0.0f);
            int8 best = NoObjective;
            float bestDist = 0.0f;
            for (size_t i = 0; i < plan.Objectives.size(); ++i)
            {
                Objective const& objective = plan.Objectives[i];
                if (!objective.Known || !HoldableNode(plan, i) || objective.Owner != them || objective.Assaulting != TEAM_NEUTRAL
                    || objective.Near[them] || objective.Near[us] || std::find(teamPlan.AttackTargets.begin(), teamPlan.AttackTargets.end(), int8(i)) != teamPlan.AttackTargets.end())
                    continue;
                if (NodeDef const* row = RowOf(plan, int8(i)); row && row->Weights[team] <= 0.0f)
                    continue;
                float dist = centre.GetExactDist2d(objective.Pos);
                if (best == NoObjective || dist > bestDist)
                {
                    best = int8(i);
                    bestDist = dist;
                }
            }
            if (best != NoObjective)
            {
                Player* pick = nullptr;
                bool pickNinja = false;
                float pickDist = 0.0f;
                for (Player* bot : bots)
                {
                    Assignment const& job = plan.Jobs[bot->GetGUID()];
                    if (!bot->IsAlive() || job.Base != Job::Attack || job.Now != Job::Attack || onPush(job) || job.Errand != Overlay::None
                        || Live(job.FallBackUntilMs, now) || (job.Respond != NoObjective && Live(job.RespondUntilMs, now)))
                        continue;
                    bool rogue = bot->GetClass() == CLASS_ROGUE && bot->HasSpell(NinjaStealth);
                    bool druid = bot->GetClass() == CLASS_DRUID && bot->HasSpell(NinjaProwl) && bot->HasSpell(NinjaCatForm);
                    PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
                    if ((!rogue && !druid) || !ai || ai->GetRole() == BotRole::Healer)
                        continue;
                    bool isNinja = job.Style == BgRules::Archetype::Ninja;
                    float dist = bot->GetExactDist2d(plan.Objectives[best].Pos);
                    if (!pick || (isNinja && !pickNinja) || (isNinja == pickNinja && dist < pickDist))
                    {
                        pick = bot;
                        pickNinja = isNinja;
                        pickDist = dist;
                    }
                }
                if (pick)
                {
                    Assignment& job = plan.Jobs[pick->GetGUID()];
                    job.Errand = Overlay::Ninja;
                    job.ErrandObjective = best;
                    job.ErrandUntilMs = now + NinjaMs;
                    ++teamPlan.Ninjas;
                    ++started;
                    if (reason.empty())
                        reason = Trinity::StringFormat("{} sneaks to {}", pick->GetName(), plan.Objectives[best].Name);
                }
            }
        }
    }

    // #7: the bot leading the raid marks their carrier and a healer
    if (sBgConfig.FocusFire && !plan.Picture.empty())
        MarkTargets(plan, team, bots, now);

    // #9: flag maps: where the enemy carrier of our flag is
    if (sBgConfig.CallOuts && plan.Def->Kind == MapKind::Flag && plan.Flags[us].State == uint8(FlagState::Taken) && !plan.Flags[us].Carrier.IsEmpty()
        && plan.Objectives.size() >= 2 && plan.Objectives[0].Known && plan.Objectives[1].Known)
        for (Seen const& seen : plan.Picture)
            if (seen.Guid == plan.Flags[us].Carrier)
            {
                Position const& home = plan.Objectives[us].Pos;
                Position const& away = plan.Objectives[them].Pos;
                float toHome = seen.Pos.GetExactDist2d(home), toAway = seen.Pos.GetExactDist2d(away);
                float span = home.GetExactDist2d(away);
                char const* where = toHome < span * 0.3f ? "our base" : toAway < span * 0.3f ? "their base" : "mid";
                for (Player* bot : bots)
                    if (bot->IsAlive() && plan.Jobs[bot->GetGUID()].Base == Job::Defend)
                    {
                        Say(plan, team, bot, Trinity::StringFormat("efc {}", where), Trinity::StringFormat("efc:{}", where), now);
                        break;
                    }
                break;
            }

    // #15: a certain loss: the attackers fight where most enemies are instead of the objectives
    bool brawl = teamPlan.Stance == BgRules::Stance::Brawl && !plan.Picture.empty();
    teamPlan.BrawlKnown = brawl && BiggestGroup(plan, them, teamPlan.BrawlAt);

    // the overlay each bot follows, highest first (BgRules::Overlay)
    for (Player* bot : bots)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        if (job.Respond != NoObjective && (!Live(job.RespondUntilMs, now)
            || (!sBgConfig.ThreatResponse && !sBgConfig.RetakeTimers && !sBgConfig.HumanCalls && !sBgConfig.LaneChange)))
            job.Respond = NoObjective;
        if (!sBgConfig.FallBack)
            job.FallBackUntilMs = 0;
        if (job.Errand != Overlay::None && (!Live(job.ErrandUntilMs, now) || (job.Errand == Overlay::Mine && !sBgConfig.Mines)
            || (job.Errand == Overlay::Ninja && !sBgConfig.NinjaCaps)))
        {
            job.Errand = Overlay::None;
            job.ErrandObjective = NoObjective;
        }
        Overlay over = Overlay::None;
        if (job.Now == Job::Carrier)
            over = Overlay::Carrier;
        else if (charged(job))
            over = Overlay::Push;
        else if (Live(job.FallBackUntilMs, now))
            over = Overlay::FallBack;
        else if (job.Respond != NoObjective)
            over = Overlay::Respond;
        else if (job.Errand != Overlay::None && job.ErrandObjective != NoObjective)
            over = job.Errand;
        else if (teamPlan.BrawlKnown && job.Base == Job::Attack && job.Now == Job::Attack)
            over = Overlay::Brawl;
        job.Over = over;
    }
    return started;
}
}
