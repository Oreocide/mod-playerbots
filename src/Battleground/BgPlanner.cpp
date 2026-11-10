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

#include "Battleground/BgPlanner.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgMetrics.h"
#include "Battleground/BgReact.h"
#include "ActivityMgr.h"
#include "Battleground/BgTalk.h"
#include "BotSession.h"
#include "Engine/PerfMonitor.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Battleground.h"
#include "BattlegroundPackets.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace Playerbots::Bg
{
namespace
{
using CaptureState = WorldPackets::Battleground::BattlegroundCapturePointState;

uint64 TestSeedValue = 0;                    // .bot bg test start: the seed of the test match's rolls (0: the match's own)
constexpr uint32 DiscoverEveryMs = 5000;    // a missing objective is searched for at most this often
constexpr uint32 RetargetEveryMs = 60000;   // attack targets are picked again at least this often
constexpr uint32 NoPathMs = 60000;          // an objective without a mesh path is avoided by that bot this long
constexpr uint32 EscortCount = 2;
constexpr uint32 CarrierHealers = 2;        // #14: healers that stay with our carrier
constexpr float CarrierHealRange = 100.0f;  // #14: only healers this close to it
constexpr uint32 FlagRunnerCount = 2;
constexpr uint32 AbandonMs = 120000;        // Valley: a push given up is not the team's target again for this long
constexpr uint32 TwoTargetsFrom = 24;       // Valley: this many attackers split over two node targets (never over a boss)
constexpr uint32 PassOverMs = 60000;        // MoveOn: a push that left a hopeless target does not pick it again for this long
constexpr float PassOverEnemyRange = 40.0f; // MoveOn: the enemies this close to the target hold it (BgReact's threat range)
constexpr float PassOverAllyRange = 150.0f; // MoveOn: ours this close to it count for the fight, as do the bots sent to it
constexpr uint32 PassOverRatio = 2;
constexpr uint32 PassOverMinEnemies = 4;

bool Contains(std::vector<int8> const& list, int8 value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

char const* TeamName(TeamId team)
{
    return team == TEAM_ALLIANCE ? "Alliance" : team == TEAM_HORDE ? "Horde" : "nobody";
}

bool Blocked(Assignment const& job, int8 objective, uint32 now)
{
    // no mesh path there for a while, or (#19) three deaths there
    return (job.NoPathObjective == objective && int32(job.NoPathUntilMs - now) > 0)
        || (job.AvoidObjective == objective && int32(job.AvoidUntilMs - now) > 0);
}

float DistTo(BgPlan const& plan, Player const* player, int8 objective)
{
    Position pos;
    return ObjectivePosition(plan, objective, pos) ? player->GetExactDist2d(pos) : 100000.0f;
}

/// The split of the team's ownerless bots into attackers and defenders (BgRules::Split). Returns the bots whose job changed
uint32 SplitTeam(BgPlan& plan, TeamPlan const& teamPlan, std::vector<Player*> const& bots, uint32 now, std::vector<uint32> const* caps = nullptr)
{
    // MoveOn: a running capture of ours is no reason to be picked to defend or to stay a defender; the nodes we hold are (when there are any)
    bool anyUncapped = caps && caps->size() == teamPlan.DefendTargets.size()
        && std::find(caps->begin(), caps->end(), BgRules::Uncapped) != caps->end();
    std::vector<BgRules::Seat> seats;
    seats.reserve(bots.size());
    for (Player* bot : bots)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        if (job.Base == Job::Defend && teamPlan.DefendTargets.empty())
            job.Base = Job::None;               // nothing left to defend: free at once
        BgRules::Seat seat;
        seat.Id = bot->GetGUID().GetCounter();
        seat.Key = BgRules::ShuffleKey(teamPlan.Seed, seat.Id);
        seat.JobNow = job.Base == Job::Defend || job.Base == Job::Attack ? job.Base : Job::None;
        seat.SinceMs = job.SinceMs;
        seat.DefendCost = 0.0f;
        if (!teamPlan.DefendTargets.empty())
        {
            seat.DefendCost = std::numeric_limits<float>::max();
            for (size_t t = 0; t < teamPlan.DefendTargets.size(); ++t)
                if (!anyUncapped || (*caps)[t] == BgRules::Uncapped)
                    seat.DefendCost = std::min(seat.DefendCost, DistTo(plan, bot, teamPlan.DefendTargets[t]));
        }
        // #12: a turtle is the first to defend and the last to go back; a brawler or road fighter the other way round
        if (job.Style == BgRules::Archetype::Turtle)
        {
            seat.Key >>= 8;
            seat.DefendCost = 0.0f;
        }
        else if (job.Style == BgRules::Archetype::Brawler || job.Style == BgRules::Archetype::RoadFighter)
        {
            seat.Key |= 0xFF00000000000000ull;
            if (!teamPlan.DefendTargets.empty())
                seat.DefendCost = std::numeric_limits<float>::max();
        }
        seats.push_back(seat);
    }
    uint32 changed = BgRules::Split(seats, teamPlan.WantDefenders, now, sBgConfig.RoleHoldSeconds * 1000);
    for (size_t i = 0; i < bots.size(); ++i)
    {
        Assignment& job = plan.Jobs[bots[i]->GetGUID()];
        job.Base = seats[i].JobNow;
        job.SinceMs = seats[i].SinceMs;
    }
    return changed;
}

/// Defenders over the defended objectives by weight: the ones already there stay while the node's share has room. Returns the
/// defenders that moved. With caps (MoveOn), a capped target takes no more than its cap, and a defender with no room anywhere, or one
/// too many at a capped target, becomes an attacker (added to released)
uint32 PlaceDefenders(BgPlan& plan, TeamPlan const& teamPlan, std::vector<Player*> const& defenders, std::vector<float> const& weights, uint32 now,
    std::vector<uint32> const* caps = nullptr, std::vector<Player*>* released = nullptr)
{
    uint32 changed = 0;
    if (caps && (caps->size() != weights.size() || !released))
        caps = nullptr;
    std::vector<uint32> quota = caps ? BgRules::SpreadCapped(weights, *caps, defenders.size()) : BgRules::Spread(weights, defenders.size());
    auto capped = [&](size_t i) { return caps && (*caps)[i] != BgRules::Uncapped; };
    std::vector<Player*> unplaced;
    for (Player* bot : defenders)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        auto at = std::find(teamPlan.DefendTargets.begin(), teamPlan.DefendTargets.end(), job.Objective);
        size_t index = at - teamPlan.DefendTargets.begin();
        if (at != teamPlan.DefendTargets.end() && quota[index] > 0 && !Blocked(job, job.Objective, now))
            --quota[index];
        else
            unplaced.push_back(bot);
    }
    for (Player* bot : unplaced)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        // a guard too many at a running capture of ours goes on with the attackers; it does not walk back to a node we hold
        auto at = std::find(teamPlan.DefendTargets.begin(), teamPlan.DefendTargets.end(), job.Objective);
        bool surplus = at != teamPlan.DefendTargets.end() && capped(size_t(at - teamPlan.DefendTargets.begin()));
        int best = -1;
        float bestDist = 0.0f;
        for (int pass = 0; pass < 2 && best < 0 && !surplus; ++pass)
            for (size_t i = 0; i < teamPlan.DefendTargets.size(); ++i)
            {
                int8 target = teamPlan.DefendTargets[i];
                if ((pass == 0 && !quota[i]) || (pass == 1 && capped(i)) || (Blocked(job, target, now) && teamPlan.DefendTargets.size() > 1))
                    continue;
                float dist = DistTo(plan, bot, target);
                if (best < 0 || dist < bestDist)
                {
                    best = int(i);
                    bestDist = dist;
                }
            }
        if (best < 0 && caps)
        {
            job.Base = Job::Attack;
            job.SinceMs = now;
            job.Objective = NoObjective;
            released->push_back(bot);
            ++changed;
            continue;
        }
        if (best < 0)
            best = 0;
        if (quota[best])
            --quota[best];
        if (job.Objective != teamPlan.DefendTargets[best])
        {
            job.Objective = teamPlan.DefendTargets[best];
            ++changed;
        }
    }
    return changed;
}

/// MoveOn: a target of the push where the enemy stays at least twice as strong as all of ours there or on the way (and at least
/// PassOverMinEnemies) is left for PassOverMs, when the team has another candidate. Ours under enemy assault is never left this way
/// (ThreatResponse decides that fight), nor what eligible() refuses (bosses: the rally has its own wipe and give-up)
template <typename Eligible>
void PassOver(BgPlan& plan, Map* map, uint32 team, std::vector<int8>& candidates, Eligible eligible, std::string& why, uint32 now)
{
    TeamPlan const& teamPlan = plan.Teams[team];
    TeamId us = TeamId(team);
    TeamId them = TeamId(1 - team);
    struct Lost { int8 Target; uint32 Enemies; uint32 Allies; };
    std::vector<Lost> lost;
    for (int8 target : teamPlan.AttackTargets)
    {
        if (target < 0 || size_t(target) >= plan.Objectives.size() || !Contains(candidates, target) || !eligible(target))
            continue;
        Objective const& objective = plan.Objectives[target];
        if (objective.Owner == us && objective.Assaulting == them)
            continue;
        uint32 enemies = 0, near = 0, sent = 0;
        for (MapReference const& ref : map->GetPlayers())
        {
            Player* player = ref.GetSource();
            if (!player || !player->IsInWorld() || player->IsGameMaster() || !player->IsAlive())
                continue;
            TeamId side = TeamOf(player);
            float dist = player->GetExactDist2d(objective.Pos);
            if (side == them && dist <= PassOverEnemyRange)
                ++enemies;
            else if (side == us)
            {
                if (dist <= PassOverAllyRange)
                    ++near;
                auto job = plan.Jobs.find(player->GetGUID());
                if (job != plan.Jobs.end() && job->second.Base == Job::Attack && job->second.Objective == target)
                    ++sent;
            }
        }
        uint32 allies = std::max(near, sent);
        if (BgRules::Hopeless(enemies, allies, 0, PassOverRatio, PassOverMinEnemies))
            lost.push_back({ target, enemies, allies });
    }
    if (lost.empty() || lost.size() >= candidates.size())
        return;
    for (Lost const& left : lost)
    {
        Objective& objective = plan.Objectives[left.Target];
        uint32 until = now + PassOverMs;
        if (int32(objective.BlockedUntilMs[team] - until) < 0)
            objective.BlockedUntilMs[team] = until;
        candidates.erase(std::remove(candidates.begin(), candidates.end(), left.Target), candidates.end());
        why = Trinity::StringFormat("left {}: {} enemies to {} of ours", objective.Name, left.Enemies, left.Allies);
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} instance {} {}: {}", plan.Def->Name, plan.InstanceId, TeamName(us), why);
    }
}

/// Valley: the team has done a node of a "requires" list: holds the graveyard, burnt the tower, killed the boss
bool DoneBy(BgPlan const& plan, std::string const& key, TeamId team)
{
    for (size_t i = 0; i < plan.Def->Nodes.size() && i < plan.Objectives.size(); ++i)
    {
        NodeDef const& row = plan.Def->Nodes[i];
        if (row.Key != key)
            continue;
        Objective const& objective = plan.Objectives[i];
        switch (row.Kind)
        {
            case NodeKind::Capture: return objective.Owner == team;
            case NodeKind::Burn: return objective.Destroyed && objective.Owner == team;
            case NodeKind::KillCaptain:
            case NodeKind::KillGeneral: return !objective.Alive;
            default: return false;
        }
    }
    return false;
}

bool Pushing(BgRules::RallyState state)
{
    return state == BgRules::RallyState::Gathering || state == BgRules::RallyState::Charging || state == BgRules::RallyState::Fighting
        || state == BgRules::RallyState::Regroup;
}

/// #9: a bot on the push says what the push does now ("galv next", "pulling galv", "galv down")
void RallyCall(BgPlan& plan, uint32 team, std::vector<Player*> const& bots, int8 boss, BgRules::RallyState state, uint32 now)
{
    if (!sBgConfig.CallOuts)
        return;
    TeamPlan const& teamPlan = plan.Teams[team];
    // a bot on the push (the puller when it is one), else any attacker: once the boss is dead the push has its next target
    Player* speaker = nullptr;
    Player* anyone = nullptr;
    for (Player* bot : bots)
    {
        Assignment const& job = plan.Jobs[bot->GetGUID()];
        if (!bot->IsAlive() || job.Base != Job::Attack)
            continue;
        if (!anyone)
            anyone = bot;
        if (job.Objective != boss)
            continue;
        speaker = bot;
        if (bot->GetGUID() == teamPlan.Puller)
            break;
    }
    if (!speaker)
        speaker = anyone;
    if (!speaker)
        return;
    std::string name = CallName(plan, boss);
    switch (state)
    {
        case BgRules::RallyState::Gathering: Say(plan, team, speaker, name + " next, gather at the rally point", "gather:" + name, now); break;
        case BgRules::RallyState::Charging: Say(plan, team, speaker, "pulling " + name, "pull:" + name, now); break;
        case BgRules::RallyState::Regroup: Say(plan, team, speaker, "wipe, back to the rally point", "wipe:" + name, now); break;
        case BgRules::RallyState::Done: Say(plan, team, speaker, name + " down", "down:" + name, now); break;
        case BgRules::RallyState::Abandoned: Say(plan, team, speaker, "not enough people for " + name, "abandon:" + name, now); break;
        default: break;
    }
}
}

void SetTestSeed(uint64 seed)
{
    TestSeedValue = seed;
}

Planner& Planner::Instance()
{
    static Planner planner;
    return planner;
}

TeamId TeamOf(Player const* player)
{
    return GetTeamIdForTeam(player->GetBGTeam());
}

bool ObjectivePosition(BgPlan const& plan, int8 objective, Position& out)
{
    if (objective == FlagObjective)
    {
        FlagInfo const& flag = plan.Flags[0];
        if (flag.State == uint8(FlagState::Dropped) && !flag.Drop.IsEmpty())
        {
            out = flag.DropPos;
            return true;
        }
        if (!flag.Known)
            return false;
        out = flag.Home;
        return true;
    }
    if (objective < 0 || size_t(objective) >= plan.Objectives.size() || !plan.Objectives[objective].Known)
        return false;
    out = plan.Objectives[objective].Pos;
    return true;
}

std::string ObjectiveName(BgPlan const& plan, int8 objective)
{
    if (objective == FlagObjective)
        return "the flag";
    if (objective < 0 || size_t(objective) >= plan.Objectives.size())
        return "-";
    return plan.Objectives[objective].Name;
}

uint32 TeamScore(BgPlan const& plan, Battleground const* bg, Map const* map, uint32 team)
{
    if (plan.Def && plan.Def->ScoreWs[team] && map)
        return uint32(std::max(0, map->GetWorldStateValue(int32(plan.Def->ScoreWs[team]))));
    return bg ? bg->GetTeamScore(TeamId(team)) : 0;
}

NodeDef const* RowOf(BgPlan const& plan, int8 objective)
{
    if (!plan.Def || plan.Def->Kind != MapKind::Valley || objective < 0 || size_t(objective) >= plan.Def->Nodes.size())
        return nullptr;
    return &plan.Def->Nodes[objective];
}

Creature* BossOf(BgPlan const& plan, Map* map, int8 objective)
{
    if (objective < 0 || size_t(objective) >= plan.Objectives.size() || plan.Objectives[objective].Boss.IsEmpty())
        return nullptr;
    return map->GetCreature(plan.Objectives[objective].Boss);
}

float RallyRadiusOf(BgPlan const& plan, int8 boss)
{
    NodeDef const* row = RowOf(plan, boss);
    NodeDef const* rally = row && row->Rally >= 0 ? RowOf(plan, row->Rally) : nullptr;
    return rally && rally->Radius > 0.0f ? rally->Radius : sBgConfig.RallyRadius;
}

Assignment Planner::Touch(Player* bot)
{
    uint32 now = GameTime::GetGameTimeMS();
    auto [it, created] = _plans.try_emplace({ bot->GetMapId(), bot->GetInstanceId() });
    BgPlan& plan = it->second;
    if (created)
    {
        plan.MapId = bot->GetMapId();
        plan.InstanceId = bot->GetInstanceId();
        plan.Def = FindMap(plan.MapId);
        plan.CreatedMs = now;
        // the first plan within a second, then every TickMs, the matches spread over the tick
        plan.LastTickMs = now - sBgConfig.TickMs + (plan.InstanceId % 8) * 125;
        TC_LOG_INFO("server.playerbots", "Playerbots: battleground planner opened map {} instance {}", plan.MapId, plan.InstanceId);
    }
    plan.LastTouchMs = now;

    auto job = plan.Jobs.find(bot->GetGUID());
    return job != plan.Jobs.end() ? job->second : Assignment();
}

void Planner::NoteNoPath(Player* bot, int8 objective)
{
    auto it = _plans.find({ bot->GetMapId(), bot->GetInstanceId() });
    if (it == _plans.end())
        return;
    auto job = it->second.Jobs.find(bot->GetGUID());
    if (job == it->second.Jobs.end())
        return;
    job->second.NoPathObjective = objective;
    job->second.NoPathUntilMs = GameTime::GetGameTimeMS() + NoPathMs;
}

void Planner::NoteCapture(Player* bot, bool fallback, uint32 failure)
{
    auto it = _plans.find({ bot->GetMapId(), bot->GetInstanceId() });
    if (it == _plans.end())
        return;
    if (fallback)
    {
        ++it->second.CaptureFallbacks;
        it->second.LastCastFailure = failure;
    }
    else
        ++it->second.CaptureCasts;
}

void Planner::NoteCaptureGiveUp(Player* bot, uint32 result)
{
    auto it = _plans.find({ bot->GetMapId(), bot->GetInstanceId() });
    if (it == _plans.end())
        return;
    ++it->second.CaptureGiveUps;
    it->second.LastGiveUpResult = result;
}

BgPlan const* Planner::Find(uint32 mapId, uint32 instanceId) const
{
    auto it = _plans.find({ mapId, instanceId });
    return it != _plans.end() ? &it->second : nullptr;
}

BgPlan* Planner::FindForEdit(uint32 mapId, uint32 instanceId)
{
    auto it = _plans.find({ mapId, instanceId });
    return it != _plans.end() ? &it->second : nullptr;
}

void Planner::Reset()
{
    _plans.clear();
}

std::vector<BgPlan const*> Planner::List() const
{
    std::vector<BgPlan const*> list;
    for (auto const& [key, plan] : _plans)
        list.push_back(&plan);
    return list;
}

void Planner::Update()
{
    if (_plans.empty())
        return;

    PerfScope perf("(bg planner)");
    uint32 now = GameTime::GetGameTimeMS();
    for (auto it = _plans.begin(); it != _plans.end();)
    {
        BgPlan& plan = it->second;
        Map* map = sMapMgr->FindMap(plan.MapId, plan.InstanceId);
        BattlegroundMap* bgMap = map ? map->ToBattlegroundMap() : nullptr;
        Battleground* bg = bgMap ? bgMap->GetBG() : nullptr;
        // a plan lives as long as its match (mod-playerbots#33): bots touch it only from Bg::Play, which does not run while they fight,
        // so a match where every bot fought or lay dead for half a minute lost its plan, rolled the split again and restarted every job
        char const* gone = !bg ? "match gone" : !plan.Def ? "no definition" : bg->GetStatus() == STATUS_WAIT_LEAVE ? "match over"
            : !sBgConfig.Plans(plan.MapId) ? "map no longer planned" : nullptr;
        if (gone)
        {
            // #20: the result of the side a real player was on moves the streak (players on both sides, or none: no change)
            if (bg && plan.Def && bg->GetStatus() == STATUS_WAIT_LEAVE && !plan.ResultSeen && plan.Teams[0].HadHuman != plan.Teams[1].HadHuman)
            {
                plan.ResultSeen = true;
                PvPTeamId winner = bg->GetWinner();
                if (winner == PVP_TEAM_ALLIANCE || winner == PVP_TEAM_HORDE)
                {
                    TeamId won = winner == PVP_TEAM_ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE;
                    TeamId human = plan.Teams[TEAM_ALLIANCE].HadHuman ? TEAM_ALLIANCE : TEAM_HORDE;
                    _streak = BgRules::NextStreak(_streak, won == human);
                    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} instance {} won by the {}; the side of the real player has a streak of {}",
                        plan.Def->Name, plan.InstanceId, TeamName(won), _streak);
                }
            }
            TC_LOG_INFO("server.playerbots", "Playerbots: battleground planner closed map {} instance {} after {} plans ({})", plan.MapId, plan.InstanceId,
                plan.Ticks, gone);
            Metrics::Close(plan, bg, gone);
            it = _plans.erase(it);
            continue;
        }

        if (bg->GetStatus() == STATUS_IN_PROGRESS && now - plan.LastTickMs >= sBgConfig.TickMs)
        {
            plan.LastTickMs = now;
            Plan(plan, bg, map, now);
        }
        ++it;
    }
}

void Planner::Discover(BgPlan& plan, Map* map, uint32 now)
{
    if (plan.Objectives.empty())
    {
        if (plan.Def->Kind == MapKind::Flag)
        {
            plan.Objectives.push_back({ plan.Def->FlagEntries[TEAM_ALLIANCE], "Alliance flag room", 1.0f });
            plan.Objectives.push_back({ plan.Def->FlagEntries[TEAM_HORDE], "Horde flag room", 1.0f });
            plan.Flags[TEAM_ALLIANCE].Entry = plan.Def->FlagEntries[TEAM_ALLIANCE];
            plan.Flags[TEAM_HORDE].Entry = plan.Def->FlagEntries[TEAM_HORDE];
        }
        else
        {
            for (NodeDef const& node : plan.Def->Nodes)
                plan.Objectives.push_back({ node.Entry, node.Name, node.Weight });
            if (plan.Def->Kind == MapKind::Eye)
                plan.Flags[0].Entry = plan.Def->FlagEntries[0];
        }
    }

    bool missing = false;
    for (Objective const& objective : plan.Objectives)
        missing |= objective.Guid.IsEmpty() || !map->GetGameObject(objective.Guid);
    for (FlagInfo const& flag : plan.Flags)
        missing |= flag.Entry && (flag.Guid.IsEmpty() || !map->GetGameObject(flag.Guid));
    if (!missing || now - plan.LastDiscoverMs < DiscoverEveryMs)
        return;
    plan.LastDiscoverMs = now;

    // one pass over the objects of the map; a flag or a node that respawned has a new guid
    for (auto const& [guid, go] : map->GetObjectsStore().Data.FindContainer<GameObject>())
    {
        if (!go || !go->IsInWorld())
            continue;
        uint32 entry = go->GetEntry();
        for (Objective& objective : plan.Objectives)
            if (objective.Entry == entry && (objective.Guid.IsEmpty() || !map->GetGameObject(objective.Guid)))
            {
                objective.Guid = guid;
                objective.Pos = go->GetPosition();
                objective.Known = true;
            }
        for (FlagInfo& flag : plan.Flags)
            if (flag.Entry && flag.Entry == entry && (flag.Guid.IsEmpty() || !map->GetGameObject(flag.Guid)))
            {
                flag.Guid = guid;
                flag.Home = go->GetPosition();
                flag.Known = true;
            }
    }
}

void Planner::ReadState(BgPlan& plan, Map* map)
{
    for (Objective& objective : plan.Objectives)
    {
        GameObject* go = objective.Guid.IsEmpty() ? nullptr : map->GetGameObject(objective.Guid);
        if (!go)
            continue;

        switch (plan.Def->Kind)
        {
            case MapKind::Capture:
            {
                GameObjectValue const* value = go->GetGOValue();
                objective.Assaulting = TEAM_NEUTRAL;
                switch (value->CapturePoint.State)
                {
                    case CaptureState::AllianceCaptured: objective.Owner = TEAM_ALLIANCE; break;
                    case CaptureState::HordeCaptured: objective.Owner = TEAM_HORDE; break;
                    case CaptureState::ContestedAlliance: objective.Owner = value->CapturePoint.LastTeamCapture; objective.Assaulting = TEAM_ALLIANCE; break;
                    case CaptureState::ContestedHorde: objective.Owner = value->CapturePoint.LastTeamCapture; objective.Assaulting = TEAM_HORDE; break;
                    default: objective.Owner = TEAM_NEUTRAL; break;
                }
                // the retake timers (#2): the object counts the assault down itself
                objective.AssaultTotalMs = go->GetGOInfo()->capturePoint.CaptureTime;
                objective.AssaultLeftMs = objective.Assaulting != TEAM_NEUTRAL ? value->CapturePoint.AssaultTimer : 0;
                break;
            }
            case MapKind::Eye:
                objective.Owner = go->GetControllingTeam();
                break;
            default:
                break;
        }
    }

    for (FlagInfo& flag : plan.Flags)
    {
        if (!flag.Entry)
            continue;
        GameObject* go = flag.Guid.IsEmpty() ? nullptr : map->GetGameObject(flag.Guid);
        if (!go)
        {
            flag.State = 0;
            flag.Carrier.Clear();
            flag.Drop.Clear();
            continue;
        }
        flag.State = uint8(go->GetFlagState());
        flag.Carrier = go->GetFlagCarrierGUID();
        flag.Drop.Clear();
        if (flag.State == uint8(FlagState::Dropped))
            for (auto const& [guid, drop] : map->GetObjectsStore().Data.FindContainer<GameObject>())
                if (drop && drop->IsInWorld() && drop->GetGoType() == GAMEOBJECT_TYPE_NEW_FLAG_DROP && drop->GetOwnerGUID() == flag.Guid)
                {
                    flag.Drop = guid;
                    flag.DropPos = drop->GetPosition();
                    break;
                }
    }
}

void Planner::Plan(BgPlan& plan, Battleground* bg, Map* map, uint32 now)
{
    ++plan.Ticks;
    if (!plan.Seeded)
    {
        // fixed once: the time the match was created, so a plan made again later in the match rolls the same
        plan.StartTime = uint64(GameTime::GetGameTime()) - bg->GetElapsedTime() / 1000;
        for (uint32 team = 0; team < 2; ++team)
        {
            TeamPlan& teamPlan = plan.Teams[team];
            // .bot bg test start <map> <level> <seed>: the test match rolls from the given seed, so a pair of test matches can share one
            teamPlan.Seed = TestSeedValue && sActivityMgr->IsBgTest(plan.InstanceId) ? BgRules::MatchSeed(uint32(TestSeedValue), team, TestSeedValue)
                : BgRules::MatchSeed(plan.InstanceId, team, plan.StartTime);
            teamPlan.DefendPct = BgRules::RollDefendPct(teamPlan.Seed, sBgConfig.DefendMinPct, sBgConfig.DefendMaxPct);
            teamPlan.LastReasonMs = now;
        }
        plan.Seeded = true;
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} instance {} rolled defence {}% (Alliance) and {}% (Horde)", plan.Def->Name, plan.InstanceId,
            plan.Teams[0].DefendPct, plan.Teams[1].DefendPct);
    }

    bool valley = plan.Def->Kind == MapKind::Valley;
    if (valley)
        ReadValley(plan, map, now);
    else
    {
        Discover(plan, map, now);
        ReadState(plan, map);
    }

    std::vector<Player*> members[2];
    for (MapReference const& ref : map->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (!player || !player->IsInWorld() || player->IsGameMaster())
            continue;
        TeamId team = TeamOf(player);
        if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
            members[team].push_back(player);
    }

    // the reactive behaviours read one picture of the match per plan (phase 4); with all of them off nothing is gathered
    if (sBgConfig.Reacts())
        BuildPicture(plan, map, now);
    else
        plan.Picture.clear();

    // what changed since the last plan: objectives, rosters, the lead; and the periodic retarget
    std::string reason;
    if (plan.LastOwners.size() != plan.Objectives.size() * 2)
        plan.LastOwners.assign(plan.Objectives.size() * 2, TEAM_NEUTRAL);
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (plan.LastOwners[i * 2] != objective.Owner)
        {
            reason = Trinity::StringFormat("{} {} by {}", objective.Name, objective.Destroyed ? "destroyed" : "taken", TeamName(objective.Owner));
            plan.LastNodeChangeMs = now;
        }
        else if (plan.LastOwners[i * 2 + 1] != objective.Assaulting && objective.Assaulting != TEAM_NEUTRAL)
            reason = Trinity::StringFormat("{} assaulted by {}", objective.Name, TeamName(objective.Assaulting));
        plan.LastOwners[i * 2] = objective.Owner;
        plan.LastOwners[i * 2 + 1] = objective.Assaulting;
    }
    for (Objective& objective : plan.Objectives)
    {
        if (objective.WasAlive && !objective.Alive)
            reason = Trinity::StringFormat("{} killed", objective.Name);
        objective.WasAlive = objective.Alive;
    }
    for (uint32 team = 0; team < 2; ++team)
    {
        if (plan.LastRoster[team] != members[team].size())
        {
            if (reason.empty())
                reason = Trinity::StringFormat("{} roster {} -> {}", TeamName(TeamId(team)), plan.LastRoster[team], members[team].size());
            plan.LastRoster[team] = members[team].size();
        }
    }
    for (uint32 i = 0; i < 2; ++i)
    {
        FlagInfo const& flag = plan.Flags[i];
        if (!flag.Entry || flag.State == plan.LastFlagStates[i])
            continue;
        if (reason.empty() && plan.LastFlagStates[i])
        {
            char const* name = plan.Def->Kind == MapKind::Eye ? "the flag" : i == TEAM_ALLIANCE ? "the Alliance flag" : "the Horde flag";
            char const* state = flag.State == uint8(FlagState::Taken) ? "taken" : flag.State == uint8(FlagState::Dropped) ? "dropped"
                : flag.State == uint8(FlagState::InBase) ? "back in base" : "respawning";
            reason = Trinity::StringFormat("{} {}", name, state);
        }
        plan.LastFlagStates[i] = flag.State;
    }
    uint32 scoreA = TeamScore(plan, bg, map, TEAM_ALLIANCE), scoreH = TeamScore(plan, bg, map, TEAM_HORDE);
    int8 leader = scoreA > scoreH ? 0 : scoreH > scoreA ? 1 : 2;
    if (leader != plan.LastLeader)
    {
        if (reason.empty() && plan.LastLeader >= 0)
            reason = leader == 2 ? "score level" : Trinity::StringFormat("{} took the lead", TeamName(TeamId(leader)));
        plan.LastLeader = leader;
    }

    if (plan.Ticks == 1)
        reason = "match start";
    if (!reason.empty())
    {
        plan.LastEvent = reason;
        plan.LastEventMs = now;
    }

    // #4/#15: the score outlook and each team's stance (Normal while both keys are off)
    UpdateStances(plan, bg, map, now);

    for (uint32 team = 0; team < 2; ++team)
    {
        bool retarget = !reason.empty() || now - plan.Teams[team].LastRetargetMs >= RetargetEveryMs;
        if (valley)
            PlanValleyTeam(plan, bg, map, team, members[team], retarget, reason.empty() && retarget ? "periodic" : reason, now);
        else
            PlanTeam(plan, map, team, members[team], retarget, reason.empty() && retarget ? "periodic" : reason, now);
    }

    // players who left the match
    for (auto it = plan.Jobs.begin(); it != plan.Jobs.end();)
    {
        Player* player = ObjectAccessor::GetPlayer(map, it->first);
        if (!player)
            it = plan.Jobs.erase(it);
        else
            ++it;
    }

    // Playerbots.Bg.Metrics: read only, after everything above is decided
    Metrics::Sample(plan, bg, map, now);
}

void Planner::PlanTeam(BgPlan& plan, Map* map, uint32 team, std::vector<Player*> const& members, bool retarget, std::string const& reason, uint32 now)
{
    TeamPlan& teamPlan = plan.Teams[team];
    TeamId us = TeamId(team);
    TeamId them = TeamId(1 - team);
    MapKind kind = plan.Def->Kind;

    // only bots without an owner play the planner (a bot with an owner follows its owner and never reaches BgPlay)
    std::vector<Player*> bots;
    teamPlan.Others = 0;
    for (Player* player : members)
    {
        PlayerbotAI* ai = IsBot(player) ? sPlayerbotMgr->GetAI(player) : nullptr;
        if (ai && !ai->GetMaster())
            bots.push_back(player);
        else
            ++teamPlan.Others;
    }
    teamPlan.Assignable = bots.size();
    PrepareTeam(plan, team, members, bots, _streak);

    // what we defend: our flag room; on node maps the nodes we hold, the ones our assault is taking, and ours under assault
    teamPlan.DefendTargets.clear();
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (!objective.Known)
            continue;
        bool defend = kind == MapKind::Flag ? i == team : objective.Owner == us || objective.Assaulting == us;
        if (defend)
            teamPlan.DefendTargets.push_back(int8(i));
    }
    // the stance moves the share (#4: more at home when ahead, nearly nobody when behind late); turtling on a majority of the
    // nodes, the attackers beyond the harass share defend too
    teamPlan.WantDefenders = teamPlan.DefendTargets.empty() ? 0 : std::min<uint32>(bots.size(), BgRules::ShareOf(bots.size(), StanceDefendPct(teamPlan)));
    if (!teamPlan.DefendTargets.empty())
        teamPlan.WantDefenders = std::max<uint32>(teamPlan.WantDefenders, uint32(bots.size()) - TurtleCap(plan, team, uint32(bots.size())));
    // #10: a player's "everyone to ..." takes the defenders along while it holds
    CallOrder const* order = AttackOrder(plan, team, now);
    // the defended objectives' weights: ours under assault needs the most hands. MoveOn: our own running assault keeps
    // MoveOn.Guards, the defenders the share would have put there past that attack instead
    std::vector<float> weights;
    std::vector<uint32> caps;
    for (int8 target : teamPlan.DefendTargets)
    {
        Objective const& objective = plan.Objectives[target];
        weights.push_back(objective.Weight * (objective.Owner == us && objective.Assaulting == them ? 2.0f : 1.0f));
        caps.push_back(kind != MapKind::Flag && objective.Owner != us && objective.Assaulting == us ? sBgConfig.MoveOnGuards : BgRules::Uncapped);
    }
    bool moveOn = sBgConfig.MoveOn && !teamPlan.DefendTargets.empty();
    if (moveOn)
        teamPlan.WantDefenders = BgRules::CappedDefenders(weights, caps, teamPlan.WantDefenders);
    if (order && order->Kind == BgRules::CallKind::Everyone)
        teamPlan.WantDefenders = 0;

    auto distTo = [&](Player const* player, int8 objective) { return DistTo(plan, player, objective); };

    // the split
    uint32 changed = SplitTeam(plan, teamPlan, bots, now, moveOn ? &caps : nullptr);

    // defenders over the defended objectives: the ones already there stay while the node's share has room
    std::vector<Player*> defenders, attackers;
    for (Player* bot : bots)
        (plan.Jobs[bot->GetGUID()].Base == Job::Defend ? defenders : attackers).push_back(bot);

    if (!teamPlan.DefendTargets.empty())
    {
        std::vector<Player*> released;
        changed += PlaceDefenders(plan, teamPlan, defenders, weights, now, moveOn ? &caps : nullptr, &released);
        attackers.insert(attackers.end(), released.begin(), released.end());
    }

    // attack targets: picked again on an event, every minute, or when one stops being a target
    std::vector<int8> candidates;
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (!objective.Known)
            continue;
        // MoveOn: a target left as hopeless is not picked again for a while (nothing else blocks an objective on these maps)
        if (int32(objective.BlockedUntilMs[team] - now) > 0)
            continue;
        bool attack = kind == MapKind::Flag ? i == size_t(them) : (objective.Owner != us && objective.Assaulting != us) || objective.Assaulting == them;
        if (attack)
            candidates.push_back(int8(i));
    }
    std::string why = reason;
    if (sBgConfig.MoveOn && kind != MapKind::Flag)
        PassOver(plan, map, team, candidates, [](int8) { return true; }, why, now);
    bool stale = teamPlan.AttackTargets.empty();
    for (int8 target : teamPlan.AttackTargets)
        stale |= !Contains(candidates, target);
    if (retarget || stale)
    {
        Position centre;
        float x = 0.0f, y = 0.0f;
        uint32 counted = 0;
        for (Player* bot : attackers.empty() ? bots : attackers)
            if (bot->IsAlive())
            {
                x += bot->GetPositionX();
                y += bot->GetPositionY();
                ++counted;
            }
        if (counted)
            centre.Relocate(x / counted, y / counted, 0.0f);

        // ours under assault first, then the nearest to where the attackers are; all in (#4), the one with the fewest enemies at it
        bool allIn = teamPlan.Stance == BgRules::Stance::AllIn && !plan.Picture.empty();
        std::sort(candidates.begin(), candidates.end(), [&](int8 a, int8 b)
        {
            Objective const& oa = plan.Objectives[a];
            Objective const& ob = plan.Objectives[b];
            bool retakeA = oa.Owner == us && oa.Assaulting == them, retakeB = ob.Owner == us && ob.Assaulting == them;
            if (retakeA != retakeB)
                return retakeA;
            if (allIn && oa.Near[them] != ob.Near[them])
                return oa.Near[them] < ob.Near[them];
            return centre.GetExactDist2d(oa.Pos) < centre.GetExactDist2d(ob.Pos);
        });
        uint32 wanted = kind == MapKind::Flag ? 1 : std::clamp<uint32>(uint32(attackers.size() + 5) / 6, 1, 2);
        std::vector<int8> picked(candidates.begin(), candidates.begin() + std::min<size_t>(wanted, candidates.size()));
        if (picked != teamPlan.AttackTargets)
            ++changed;
        teamPlan.AttackTargets = std::move(picked);
        teamPlan.LastRetargetMs = now;
    }
    // #10: a player's "go farm" is the team's target while it holds, when it is one to attack
    if (order && Contains(candidates, order->Objective) && (teamPlan.AttackTargets.size() != 1 || teamPlan.AttackTargets[0] != order->Objective))
    {
        teamPlan.AttackTargets = { order->Objective };
        ++changed;
    }

    // Eye of the Storm: the free flag is worth two attackers while we hold a tower to bring it to
    Position flagPos;
    bool flagFree = false;
    if (kind == MapKind::Eye && plan.Flags[0].Carrier.IsEmpty()
        && (plan.Flags[0].State == uint8(FlagState::InBase) || plan.Flags[0].State == uint8(FlagState::Dropped))
        && ObjectivePosition(plan, FlagObjective, flagPos))
        for (Objective const& objective : plan.Objectives)
            flagFree |= objective.Owner == us;

    for (Player* bot : attackers)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        if (flagFree && job.Objective == FlagObjective)
            continue;
        if (teamPlan.AttackTargets.empty())
        {
            job.Objective = NoObjective;
            continue;
        }
        if (Contains(teamPlan.AttackTargets, job.Objective) && !Blocked(job, job.Objective, now))
            continue;
        int8 best = NoObjective;
        float bestDist = 0.0f;
        for (int8 target : teamPlan.AttackTargets)
        {
            if (Blocked(job, target, now) && teamPlan.AttackTargets.size() > 1)
                continue;
            float dist = distTo(bot, target);
            if (best == NoObjective || dist < bestDist)
            {
                best = target;
                bestDist = dist;
            }
        }
        if (job.Objective != best)
        {
            job.Objective = best;
            ++changed;
        }
    }

    // carriers and escorts (flag maps, Eye of the Storm): Now is Base unless it carries a flag or guards a carrier
    for (Player* bot : bots)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        job.Now = job.Base;
        job.Follow.Clear();
    }
    if (kind == MapKind::Flag || kind == MapKind::Eye)
    {
        ObjectGuid carrierGuid;
        if (kind == MapKind::Flag)
            carrierGuid = plan.Flags[them].Carrier;     // we carry their flag
        else if (!plan.Flags[0].Carrier.IsEmpty())
            if (Player* carrier = ObjectAccessor::GetPlayer(map, plan.Flags[0].Carrier))
                if (TeamOf(carrier) == us)
                    carrierGuid = carrier->GetGUID();

        Player* carrier = carrierGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(map, carrierGuid);
        if (carrier)
        {
            auto own = plan.Jobs.find(carrierGuid);
            if (own != plan.Jobs.end() && std::find(bots.begin(), bots.end(), carrier) != bots.end())
                own->second.Now = Job::Carrier;

            std::vector<Player*> near;
            for (Player* bot : attackers)
                if (bot != carrier && bot->IsAlive())
                    near.push_back(bot);
            std::sort(near.begin(), near.end(), [carrier](Player* a, Player* b) { return a->GetExactDist2d(carrier) < b->GetExactDist2d(carrier); });
            for (size_t i = 0; i < near.size() && i < EscortCount; ++i)
            {
                Assignment& job = plan.Jobs[near[i]->GetGUID()];
                job.Now = Job::Escort;
                job.Follow = carrierGuid;
            }

            // #14: the healers nearest our carrier stay with it (Playerbots.Bg.CarrierPlay)
            if (sBgConfig.CarrierPlay)
            {
                std::vector<Player*> healers;
                for (Player* bot : bots)
                {
                    if (bot == carrier || !bot->IsAlive() || plan.Jobs[bot->GetGUID()].Now == Job::Escort || bot->GetExactDist2d(carrier) > CarrierHealRange)
                        continue;
                    if (PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot); ai && ai->GetRole() == BotRole::Healer)
                        healers.push_back(bot);
                }
                std::sort(healers.begin(), healers.end(), [carrier](Player* a, Player* b) { return a->GetExactDist2d(carrier) < b->GetExactDist2d(carrier); });
                for (size_t i = 0; i < healers.size() && i < CarrierHealers; ++i)
                {
                    Assignment& job = plan.Jobs[healers[i]->GetGUID()];
                    job.Now = Job::Escort;
                    job.Follow = carrierGuid;
                }
            }
        }

        // the runners that already go for the free flag keep going; the nearest attackers fill up to FlagRunnerCount
        if (flagFree)
        {
            uint32 runners = 0;
            std::vector<Player*> near;
            for (Player* bot : attackers)
            {
                Assignment const& job = plan.Jobs[bot->GetGUID()];
                if (job.Objective == FlagObjective)
                    ++runners;
                else if (bot->IsAlive() && job.Now == Job::Attack)
                    near.push_back(bot);
            }
            std::sort(near.begin(), near.end(), [&flagPos](Player* a, Player* b) { return a->GetExactDist2d(flagPos) < b->GetExactDist2d(flagPos); });
            for (size_t i = 0; i < near.size() && runners < FlagRunnerCount; ++i, ++runners)
            {
                plan.Jobs[near[i]->GetGUID()].Objective = FlagObjective;
                ++changed;
            }
        }
    }

    // the reactive behaviours over the jobs above (phase 4, BgReact.cpp)
    std::string reactReason;
    uint32 reacted = ReactTeam(plan, team, bots, reactReason, now);

    if (changed || reacted)
    {
        teamPlan.LastReason = !changed ? reactReason : why.empty() ? "hold time over" : why;
        teamPlan.LastReasonMs = now;
        teamPlan.LastChanged = changed + reacted;
        ++teamPlan.Reassigns;
    }
}

void Planner::ReadValley(BgPlan& plan, Map* map, uint32 now)
{
    std::vector<NodeDef> const& rows = plan.Def->Nodes;
    if (plan.Objectives.size() != rows.size())
    {
        plan.Objectives.clear();
        for (NodeDef const& row : rows)
        {
            Objective objective;
            objective.Name = row.Name;
            objective.Known = true;
            objective.Pos = row.Pos;
            objective.Owner = row.IsNode() ? TeamId(BgRules::TeamOfSide(row.Side)) : TEAM_NEUTRAL;
            plan.Objectives.push_back(std::move(objective));
        }
    }

    // nodes and bosses from the world states the script writes: readable at any distance, no object search
    auto set = [map](uint32 id) { return id && map->GetWorldStateValue(int32(id)) != 0; };
    bool lookup = false;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        NodeDef const& row = rows[i];
        Objective& objective = plan.Objectives[i];
        objective.Pos = row.Pos;                // .bot bg rally set moves a row in place
        if (row.IsNode())
        {
            BgRules::NodeRead read = BgRules::ReadNode(row.Kind == NodeKind::Burn, BgRules::TeamOfSide(row.Side), uint8(objective.Owner),
                set(row.WsAllianceControl), set(row.WsAllianceAssault), set(row.WsHordeControl), set(row.WsHordeAssault));
            // the retake timers (#2): an assault takes the node 4 minutes after it began, which no world state says, so it runs from
            // the first plan that saw it (an assault already running when the plan was made looks younger than it is)
            if (TeamId(read.Assaulting) != TEAM_NEUTRAL && TeamId(read.Assaulting) != objective.Assaulting)
                objective.AssaultSinceMs = now;
            objective.Owner = TeamId(read.Owner);
            objective.Assaulting = TeamId(read.Assaulting);
            objective.Destroyed = read.Destroyed;
        }
        else if (row.IsBoss())
        {
            objective.Alive = !row.WsAlive || set(row.WsAlive);
            lookup |= objective.Alive && (objective.Boss.IsEmpty() || !map->GetCreature(objective.Boss));
        }
        else if (row.IsMine())
        {
            // #13: the owner from its control world states; its boss is the owner's (the mine's own while neutral)
            objective.Owner = set(row.WsAllianceControl) ? TEAM_ALLIANCE : set(row.WsHordeControl) ? TEAM_HORDE : TEAM_NEUTRAL;
            Creature* boss = objective.Boss.IsEmpty() ? nullptr : map->GetCreature(objective.Boss);
            bool current = boss && boss->IsAlive() && boss->GetEntry() == MineBossEntry(row, uint8(objective.Owner));
            objective.Alive = current;
            lookup |= sBgConfig.Mines && !current;
        }
    }

    // a boss without its creature: one pass over the map's creatures, at most every DiscoverEveryMs
    if (!lookup || now - plan.LastBossLookupMs < DiscoverEveryMs)
        return;
    plan.LastBossLookupMs = now;
    for (auto const& [guid, creature] : map->GetObjectsStore().Data.FindContainer<Creature>())
    {
        if (!creature || !creature->IsInWorld())
            continue;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].IsBoss() && rows[i].CreatureEntry == creature->GetEntry()
                && (plan.Objectives[i].Boss.IsEmpty() || !map->GetCreature(plan.Objectives[i].Boss)))
                plan.Objectives[i].Boss = guid;
            else if (rows[i].IsMine() && sBgConfig.Mines && creature->IsAlive()
                && creature->GetEntry() == MineBossEntry(rows[i], uint8(plan.Objectives[i].Owner)))
            {
                plan.Objectives[i].Boss = guid;
                plan.Objectives[i].Alive = true;
            }
        }
    }
}

void Planner::PlanValleyTeam(BgPlan& plan, Battleground* bg, Map* map, uint32 team, std::vector<Player*> const& members, bool retarget,
    std::string const& reason, uint32 now)
{
    TeamPlan& teamPlan = plan.Teams[team];
    TeamId us = TeamId(team);
    TeamId them = TeamId(1 - team);
    std::vector<NodeDef> const& rows = plan.Def->Nodes;

    // only bots without an owner play the planner; humans count toward a push (decision 2), bots with an owner follow it
    std::vector<Player*> bots;
    uint32 humans = 0;
    teamPlan.Others = 0;
    for (Player* player : members)
    {
        PlayerbotAI* ai = IsBot(player) ? sPlayerbotMgr->GetAI(player) : nullptr;
        if (ai && !ai->GetMaster())
            bots.push_back(player);
        else
        {
            ++teamPlan.Others;
            if (!IsBot(player))
                ++humans;
        }
    }
    teamPlan.Assignable = bots.size();
    PrepareTeam(plan, team, members, bots, _streak);

    // what we defend: our graveyards and standing towers, and our own running assaults (they go back to the enemy after 4 minutes
    // unless held); ours under assault weigh triple: retaking it is the defenders' job, the attackers never turn back for it
    teamPlan.DefendTargets.clear();
    std::vector<float> weights;
    std::vector<uint32> caps;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        NodeDef const& row = rows[i];
        Objective const& objective = plan.Objectives[i];
        if (!row.IsNode() || objective.Destroyed || row.Weights[team] <= 0.0f)
            continue;
        if (objective.Owner == us || objective.Assaulting == us)
        {
            teamPlan.DefendTargets.push_back(int8(i));
            weights.push_back(row.Weights[team] * (objective.Owner == us && objective.Assaulting == them ? 3.0f : 1.0f));
            caps.push_back(objective.Owner != us && objective.Assaulting == us ? sBgConfig.MoveOnGuards : BgRules::Uncapped);
        }
    }

    // a gather started again at its timeout takes the defenders along (Bg.Rally.PullDefenders); a certain win (#15) and a player's
    // "everyone to ..." (#10) send everyone
    CallOrder const* order = AttackOrder(plan, team, now);
    bool pulled = (teamPlan.Rally.DefendersPulled && Pushing(teamPlan.Rally.State)) || teamPlan.Stance == BgRules::Stance::Finish
        || (order && order->Kind == BgRules::CallKind::Everyone);
    teamPlan.WantDefenders = teamPlan.DefendTargets.empty() || pulled ? 0
        : std::min<uint32>(bots.size(), BgRules::ShareOf(bots.size(), StanceDefendPct(teamPlan)));
    // MoveOn: our own running assault keeps MoveOn.Guards, the defenders the share would have put there past that attack instead
    bool moveOn = sBgConfig.MoveOn && !teamPlan.DefendTargets.empty();
    if (moveOn)
        teamPlan.WantDefenders = BgRules::CappedDefenders(weights, caps, teamPlan.WantDefenders);
    if (pulled)
        for (Player* bot : bots)
            if (Assignment& job = plan.Jobs[bot->GetGUID()]; job.Base == Job::Defend)
                job.Base = Job::None;

    uint32 changed = SplitTeam(plan, teamPlan, bots, now, moveOn ? &caps : nullptr);

    std::vector<Player*> defenders, attackers;
    for (Player* bot : bots)
        (plan.Jobs[bot->GetGUID()].Base == Job::Defend ? defenders : attackers).push_back(bot);
    if (!teamPlan.DefendTargets.empty())
    {
        std::vector<Player*> released;
        changed += PlaceDefenders(plan, teamPlan, defenders, weights, now, moveOn ? &caps : nullptr, &released);
        attackers.insert(attackers.end(), released.begin(), released.end());
    }

    // what we attack: the highest weight first, ties to the nearest. A boss needs its rally row, MinGroup players and its "requires"
    // done, unless the soft gate is open (match time, or the enemy score low)
    uint32 startScore = plan.Def->ScoreMaxWs ? uint32(std::max(0, map->GetWorldStateValue(int32(plan.Def->ScoreMaxWs)))) : 0;
    // #4/#15: a team all in or about to win goes for the general without waiting for the towers
    bool softGate = (sBgConfig.SoftGateMinutes && bg->GetElapsedTime() >= sBgConfig.SoftGateMinutes * 60000u)
        || (sBgConfig.SoftGateScorePct && startScore && TeamScore(plan, bg, map, them) * 100 <= startScore * sBgConfig.SoftGateScorePct)
        || teamPlan.Stance == BgRules::Stance::AllIn || teamPlan.Stance == BgRules::Stance::Finish;
    uint32 force = uint32(attackers.size()) + humans;
    std::vector<int8> candidates;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        NodeDef const& row = rows[i];
        Objective const& objective = plan.Objectives[i];
        if (row.Weights[team] <= 0.0f || int32(objective.BlockedUntilMs[team] - now) > 0)
            continue;
        bool attack = false;
        switch (row.Kind)
        {
            case NodeKind::Capture:
                attack = objective.Owner != us && objective.Assaulting != us;
                break;
            case NodeKind::Burn:
                attack = objective.Owner == them && !objective.Destroyed && objective.Assaulting != us;
                break;
            case NodeKind::KillCaptain:
            case NodeKind::KillGeneral:
            {
                if (BgRules::TeamOfSide(row.Side) != uint8(them) || !objective.Alive || row.Rally < 0 || force < row.MinGroup)
                    break;
                uint32 done = 0;
                for (std::string const& key : row.Requires.Keys)
                    done += DoneBy(plan, key, us) ? 1 : 0;
                // a player who calls the boss leads: the towers need not be down first (#10)
                attack = softGate || BgRules::RequirementMet(row.Requires, done) || (order && order->Objective == int8(i));
                break;
            }
            default:
                break;
        }
        if (attack)
            candidates.push_back(int8(i));
    }
    std::string why = reason;
    if (sBgConfig.MoveOn)
        PassOver(plan, map, team, candidates, [&rows](int8 target) { return rows[target].IsNode(); }, why, now);

    // a push that charged keeps its boss until it is dead or the push gives up
    BgRules::RallyState rally = teamPlan.Rally.State;
    bool locked = teamPlan.RallyObjective != NoObjective && Contains(candidates, teamPlan.RallyObjective)
        && (rally == BgRules::RallyState::Charging || rally == BgRules::RallyState::Fighting || rally == BgRules::RallyState::Regroup);
    bool stale = teamPlan.AttackTargets.empty();
    for (int8 target : teamPlan.AttackTargets)
        stale |= !Contains(candidates, target);
    if (locked)
    {
        if (teamPlan.AttackTargets.size() != 1 || teamPlan.AttackTargets[0] != teamPlan.RallyObjective)
        {
            teamPlan.AttackTargets = { teamPlan.RallyObjective };
            ++changed;
        }
    }
    else if (retarget || stale)
    {
        Position centre;
        float x = 0.0f, y = 0.0f;
        uint32 counted = 0;
        for (Player* bot : attackers.empty() ? bots : attackers)
            if (bot->IsAlive())
            {
                x += bot->GetPositionX();
                y += bot->GetPositionY();
                ++counted;
            }
        if (counted)
            centre.Relocate(x / counted, y / counted, 0.0f);

        std::sort(candidates.begin(), candidates.end(), [&](int8 a, int8 b)
        {
            float wa = rows[a].Weights[team], wb = rows[b].Weights[team];
            if (wa != wb)
                return wa > wb;
            return centre.GetExactDist2d(plan.Objectives[a].Pos) < centre.GetExactDist2d(plan.Objectives[b].Pos);
        });
        // one push; a big team takes two nodes at once, never a boss and something else
        uint32 wanted = 1;
        if (attackers.size() >= TwoTargetsFrom && candidates.size() >= 2 && !rows[candidates[0]].IsBoss() && !rows[candidates[1]].IsBoss())
            wanted = 2;
        std::vector<int8> picked(candidates.begin(), candidates.begin() + std::min<size_t>(wanted, candidates.size()));
        if (picked != teamPlan.AttackTargets)
            ++changed;
        teamPlan.AttackTargets = std::move(picked);
        teamPlan.LastRetargetMs = now;
    }
    // #10: a player's "go ..." or "everyone to ..." is the team's target while it holds (a push that charged keeps its boss)
    if (!locked && order && Contains(candidates, order->Objective)
        && (teamPlan.AttackTargets.size() != 1 || teamPlan.AttackTargets[0] != order->Objective))
    {
        teamPlan.AttackTargets = { order->Objective };
        ++changed;
    }

    for (Player* bot : attackers)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        if (teamPlan.AttackTargets.empty())
        {
            job.Objective = NoObjective;
            continue;
        }
        if (Contains(teamPlan.AttackTargets, job.Objective) && !Blocked(job, job.Objective, now))
            continue;
        int8 best = NoObjective;
        float bestDist = 0.0f;
        for (int8 target : teamPlan.AttackTargets)
        {
            if (Blocked(job, target, now) && teamPlan.AttackTargets.size() > 1)
                continue;
            float dist = DistTo(plan, bot, target);
            if (best == NoObjective || dist < bestDist)
            {
                best = target;
                bestDist = dist;
            }
        }
        if (job.Objective != best)
        {
            job.Objective = best;
            ++changed;
        }
    }

    for (Player* bot : bots)
    {
        Assignment& job = plan.Jobs[bot->GetGUID()];
        job.Now = job.Base;
        job.Follow.Clear();
    }

    std::string rallyEvent;
    StepRally(plan, map, team, members, bots, changed, rallyEvent, now);

    // the reactive behaviours over the jobs above and the push (phase 4, BgReact.cpp)
    std::string reactReason;
    uint32 reacted = ReactTeam(plan, team, bots, reactReason, now);

    if (changed || reacted)
    {
        teamPlan.LastReason = !rallyEvent.empty() ? rallyEvent : !changed ? reactReason : why.empty() ? "hold time over" : why;
        teamPlan.LastReasonMs = now;
        teamPlan.LastChanged = changed + reacted;
        ++teamPlan.Reassigns;
    }
}

void Planner::StepRally(BgPlan& plan, Map* map, uint32 team, std::vector<Player*> const& members, std::vector<Player*> const& bots, uint32& changed,
    std::string& rallyEvent, uint32 now)
{
    TeamPlan& teamPlan = plan.Teams[team];
    int8 boss = NoObjective;
    for (int8 target : teamPlan.AttackTargets)
        if (NodeDef const* row = RowOf(plan, target); row && row->IsBoss())
            boss = target;

    if (boss != teamPlan.RallyObjective)
    {
        // the boss of the last push died (its target moved on before the rally saw Done)
        if (teamPlan.RallyObjective != NoObjective && size_t(teamPlan.RallyObjective) < plan.Objectives.size()
            && !plan.Objectives[teamPlan.RallyObjective].Alive && teamPlan.Rally.State != BgRules::RallyState::Done)
            RallyCall(plan, team, bots, teamPlan.RallyObjective, BgRules::RallyState::Done, now);
        teamPlan.RallyObjective = boss;
        teamPlan.Puller.Clear();
        teamPlan.RallyOnPush = 0;
        teamPlan.RallyPresent = 0;
        if (boss == NoObjective)
        {
            teamPlan.Rally = BgRules::Rally();
            return;
        }
        BgRules::RallyStart(teamPlan.Rally, now);
        rallyEvent = Trinity::StringFormat("{}: gathering", plan.Objectives[boss].Name);
        RallyCall(plan, team, bots, boss, BgRules::RallyState::Gathering, now);
    }
    if (boss == NoObjective)
        return;

    NodeDef const& row = plan.Def->Nodes[boss];
    Objective& objective = plan.Objectives[boss];
    Position const& rallyPos = plan.Objectives[row.Rally].Pos;
    float radius = RallyRadiusOf(plan, boss);
    Creature* creature = BossOf(plan, map, boss);

    // on the push: the bots sent to this boss, and the humans of the team who are not standing at one of our defended objectives
    std::unordered_set<ObjectGuid> planned;
    for (Player* bot : bots)
        planned.insert(bot->GetGUID());
    BgRules::RallyView view;
    for (Player* player : members)
    {
        if (!player->IsAlive())
            continue;
        if (player->GetExactDist2d(rallyPos) <= radius)
            ++view.Present;
        if (planned.contains(player->GetGUID()))
        {
            Assignment const& job = plan.Jobs[player->GetGUID()];
            if (job.Base == Job::Attack && job.Objective == boss)
                ++view.OnPush;
            continue;
        }
        if (IsBot(player))
            continue;
        bool defending = false;
        for (int8 target : teamPlan.DefendTargets)
            if (NodeDef const* node = RowOf(plan, target); node && player->GetExactDist2d(plan.Objectives[target].Pos) <= std::max(node->Radius, 15.0f))
                defending = true;
        if (!defending)
            ++view.OnPush;
    }
    view.BossAlive = objective.Alive;
    if (creature)
    {
        view.BossInCombat = creature->IsInCombat();
        view.BossEvading = creature->IsInEvadeMode();
    }

    BgRules::RallyParams params;
    params.Floor = row.Kind == NodeKind::KillGeneral ? sBgConfig.RallyGeneralFloor : sBgConfig.RallyCaptainFloor;
    params.Pct = sBgConfig.RallyPct;
    params.TimeoutMs = sBgConfig.RallyTimeoutSeconds * 1000;
    params.TimeoutChargePct = sBgConfig.RallyTimeoutChargePct;
    params.WipePct = sBgConfig.RallyWipePct;
    params.MaxRestarts = sBgConfig.RallyMaxRestarts;
    params.PullDefenders = sBgConfig.RallyPullDefenders;

    BgRules::RallyState before = teamPlan.Rally.State;
    uint32 restartsBefore = teamPlan.Rally.Restarts;
    BgRules::RallyState state = BgRules::RallyStep(teamPlan.Rally, view, params, now);
    teamPlan.RallyOnPush = view.OnPush;
    teamPlan.RallyPresent = view.Present;

    // the puller: a tank on the push nearest the boss, else the nearest melee, else the nearest ranged; never a healer
    if (state == BgRules::RallyState::Charging && before != BgRules::RallyState::Charging)
    {
        Position bossPos = creature ? creature->GetPosition() : objective.Pos;
        Player* best = nullptr;
        int bestRank = 0;
        float bestDist = 0.0f;
        for (Player* bot : bots)
        {
            Assignment const& job = plan.Jobs[bot->GetGUID()];
            PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
            if (!ai || !bot->IsAlive() || job.Base != Job::Attack || job.Objective != boss)
                continue;
            BotRole role = ai->GetRole();
            if (role == BotRole::Healer)
                continue;
            int rank = role == BotRole::Tank ? 0 : role == BotRole::MeleeDps ? 1 : 2;
            float dist = bot->GetExactDist2d(bossPos);
            if (!best || rank < bestRank || (rank == bestRank && dist < bestDist))
            {
                best = bot;
                bestRank = rank;
                bestDist = dist;
            }
        }
        teamPlan.Puller = best ? best->GetGUID() : ObjectGuid::Empty;
    }

    if (state != before || teamPlan.Rally.Restarts != restartsBefore)
    {
        ++changed;
        bool restarted = teamPlan.Rally.Restarts != restartsBefore;
        rallyEvent = Trinity::StringFormat("{}: {}{}", objective.Name, BgRules::RallyStateName(state),
            restarted ? (teamPlan.Rally.DefendersPulled ? " again, defenders called in" : " again") : "");
        if (state == BgRules::RallyState::Abandoned)
            objective.BlockedUntilMs[team] = now + AbandonMs;
        if (state != before)
            RallyCall(plan, team, bots, boss, state, now);
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} instance {} {}: {} (present {}, on the push {}, need {})", plan.Def->Name, plan.InstanceId,
            TeamName(TeamId(team)), rallyEvent, view.Present, view.OnPush, teamPlan.Rally.Need);
    }
}
}
