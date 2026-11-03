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
#include "BotSession.h"
#include "Engine/PerfMonitor.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Battleground.h"
#include "BattlegroundPackets.h"
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

namespace Playerbots::Bg
{
namespace
{
using CaptureState = WorldPackets::Battleground::BattlegroundCapturePointState;

constexpr uint32 DropAfterMs = 30000;       // a plan nobody touched for this long is dropped
constexpr uint32 DiscoverEveryMs = 5000;    // a missing objective is searched for at most this often
constexpr uint32 RetargetEveryMs = 60000;   // attack targets are picked again at least this often
constexpr uint32 NoPathMs = 60000;          // an objective without a mesh path is avoided by that bot this long
constexpr uint32 EscortCount = 2;
constexpr uint32 FlagRunnerCount = 2;

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
    return job.NoPathObjective == objective && int32(job.NoPathUntilMs - now) > 0;
}
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
        TC_LOG_DEBUG("server.playerbots", "Playerbots: battleground planner opened map {} instance {}", plan.MapId, plan.InstanceId);
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
        if (!bg || !plan.Def || bg->GetStatus() == STATUS_WAIT_LEAVE || now - plan.LastTouchMs > DropAfterMs || !sBgConfig.Plans(plan.MapId))
        {
            TC_LOG_DEBUG("server.playerbots", "Playerbots: battleground planner closed map {} instance {} after {} plans", plan.MapId, plan.InstanceId, plan.Ticks);
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
            teamPlan.Seed = BgRules::MatchSeed(plan.InstanceId, team, plan.StartTime);
            teamPlan.DefendPct = BgRules::RollDefendPct(teamPlan.Seed, sBgConfig.DefendMinPct, sBgConfig.DefendMaxPct);
            teamPlan.LastReasonMs = now;
        }
        plan.Seeded = true;
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} instance {} rolled defence {}% (Alliance) and {}% (Horde)", plan.Def->Name, plan.InstanceId,
            plan.Teams[0].DefendPct, plan.Teams[1].DefendPct);
    }

    Discover(plan, map, now);
    ReadState(plan, map);

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

    // what changed since the last plan: objectives, rosters, the lead; and the periodic retarget
    std::string reason;
    if (plan.LastOwners.size() != plan.Objectives.size() * 2)
        plan.LastOwners.assign(plan.Objectives.size() * 2, TEAM_NEUTRAL);
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (plan.LastOwners[i * 2] != objective.Owner)
            reason = Trinity::StringFormat("{} taken by {}", objective.Name, TeamName(objective.Owner));
        else if (plan.LastOwners[i * 2 + 1] != objective.Assaulting && objective.Assaulting != TEAM_NEUTRAL)
            reason = Trinity::StringFormat("{} assaulted by {}", objective.Name, TeamName(objective.Assaulting));
        plan.LastOwners[i * 2] = objective.Owner;
        plan.LastOwners[i * 2 + 1] = objective.Assaulting;
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
    uint32 scoreA = bg->GetTeamScore(TEAM_ALLIANCE), scoreH = bg->GetTeamScore(TEAM_HORDE);
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

    for (uint32 team = 0; team < 2; ++team)
    {
        bool retarget = !reason.empty() || now - plan.Teams[team].LastRetargetMs >= RetargetEveryMs;
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
    teamPlan.WantDefenders = teamPlan.DefendTargets.empty() ? 0 : std::min<uint32>(bots.size(), BgRules::ShareOf(bots.size(), teamPlan.DefendPct));

    auto distTo = [&](Player const* player, int8 objective)
    {
        Position pos;
        return ObjectivePosition(plan, objective, pos) ? player->GetExactDist2d(pos) : 100000.0f;
    };

    // the split
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
            for (int8 target : teamPlan.DefendTargets)
                seat.DefendCost = std::min(seat.DefendCost, distTo(bot, target));
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

    // defenders over the defended objectives: the ones already there stay while the node's share has room
    std::vector<Player*> defenders, attackers;
    for (Player* bot : bots)
        (plan.Jobs[bot->GetGUID()].Base == Job::Defend ? defenders : attackers).push_back(bot);

    if (!teamPlan.DefendTargets.empty())
    {
        std::vector<float> weights;
        for (int8 target : teamPlan.DefendTargets)
        {
            Objective const& objective = plan.Objectives[target];
            // ours under assault needs the most hands, our own running assault the fewest
            float weight = objective.Weight * (objective.Owner == us && objective.Assaulting == them ? 2.0f : 1.0f);
            weights.push_back(weight);
        }
        std::vector<uint32> quota = BgRules::Spread(weights, defenders.size());
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
            int best = -1;
            float bestDist = 0.0f;
            for (int pass = 0; pass < 2 && best < 0; ++pass)
                for (size_t i = 0; i < teamPlan.DefendTargets.size(); ++i)
                {
                    int8 target = teamPlan.DefendTargets[i];
                    if ((pass == 0 && !quota[i]) || (Blocked(job, target, now) && teamPlan.DefendTargets.size() > 1))
                        continue;
                    float dist = distTo(bot, target);
                    if (best < 0 || dist < bestDist)
                    {
                        best = int(i);
                        bestDist = dist;
                    }
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
    }

    // attack targets: picked again on an event, every minute, or when one stops being a target
    std::vector<int8> candidates;
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        Objective const& objective = plan.Objectives[i];
        if (!objective.Known)
            continue;
        bool attack = kind == MapKind::Flag ? i == size_t(them) : (objective.Owner != us && objective.Assaulting != us) || objective.Assaulting == them;
        if (attack)
            candidates.push_back(int8(i));
    }
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

        // ours under assault first, then the nearest to where the attackers are
        std::sort(candidates.begin(), candidates.end(), [&](int8 a, int8 b)
        {
            Objective const& oa = plan.Objectives[a];
            Objective const& ob = plan.Objectives[b];
            bool retakeA = oa.Owner == us && oa.Assaulting == them, retakeB = ob.Owner == us && ob.Assaulting == them;
            if (retakeA != retakeB)
                return retakeA;
            return centre.GetExactDist2d(oa.Pos) < centre.GetExactDist2d(ob.Pos);
        });
        uint32 wanted = kind == MapKind::Flag ? 1 : std::clamp<uint32>(uint32(attackers.size() + 5) / 6, 1, 2);
        std::vector<int8> picked(candidates.begin(), candidates.begin() + std::min<size_t>(wanted, candidates.size()));
        if (picked != teamPlan.AttackTargets)
            ++changed;
        teamPlan.AttackTargets = std::move(picked);
        teamPlan.LastRetargetMs = now;
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

    if (changed)
    {
        teamPlan.LastReason = reason.empty() ? "hold time over" : reason;
        teamPlan.LastReasonMs = now;
        teamPlan.LastChanged = changed;
        ++teamPlan.Reassigns;
    }
}
}
