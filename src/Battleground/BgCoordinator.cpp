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

#include "Battleground/BgCoordinator.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgMode.h"
#include "Battleground/BgReact.h"
#include "Spell.h"
#include "BotSession.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Battleground.h"
#include "Creature.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "StringFormat.h"
#include <algorithm>
#include <array>
#include <map>
#include <unordered_map>

namespace Playerbots::Bg
{
namespace
{
using MatchKey = std::pair<uint32, uint32>;
constexpr float NearRange = 40.0f;          // the counting pass: a living player this close to an objective is near it

struct BotRecord
{
    int8 Objective = NoObjective;
    TaskIntent Intent = TaskIntent::Take;
    uint32 SinceMs = 0;
    bool Fallback = false;
};

/// A captain squad (§2.4, §3.3): its own push on the boss, the planner's rally rules run for this squad alone
struct SquadRecord
{
    BgRules::Rally Rally;
    ObjectGuid Puller;
    uint32 CooldownUntilMs = 0;             // dissolved for being too small: not offered again before this (no churn)
};

struct TeamRecord
{
    TaskPhase Phase = TaskPhase::Early;
    BgRules::PushGate Gate;
    uint32 Fallbacks = 0;                   // fallback assignments since the match began
    uint32 Assignments = 0;                 // task changes since the match began
    std::vector<Task> Tasks;                // the last plan's tasks, for .bot bg squads
    std::vector<uint32> Living;             // their living bots
    std::vector<uint32> Dead;
    std::map<int8, SquadRecord> Squads;     // by boss objective
};

struct Match
{
    bool Coordinated = false;
    TeamRecord Teams[2];
    std::unordered_map<ObjectGuid, BotRecord> Bots;
    uint32 TetherDrops[2] = { 0, 0 };
    uint32 TetherRetaliations[2] = { 0, 0 };
    uint32 TetherReengages[2] = { 0, 0 };   // drops of an enemy the bot had dropped before
};

std::map<MatchKey, Match> Matches;

// ---- the tether's state (§2.7) ----
struct Hit
{
    ObjectGuid Attacker;
    uint32 Ms = 0;
};

struct DamageEntry
{
    std::array<Hit, 4> Hits;
};

/// Who hit each bot of a coordinated match lately. Entries are made and erased only on the world thread (Coordinator::Plan, Close):
/// while the map threads run, the world thread waits for them (World::Update: sMapMgr->Update then OnWorldUpdate), so nothing is added
/// or rehashed while the OnDamage hook finds an entry; one bot lives on one map, so one map thread at most writes its entry
std::unordered_map<ObjectGuid, DamageEntry> DamageLog;

struct TetherState
{
    Position LegFrom;
    Position LegTo;
    uint32 LegMs = 0;
    BgRules::RetaliateBudget Budget;
    uint32 LastCheckMs = 0;
    bool Retaliating = false;
    std::unordered_map<ObjectGuid, uint32> Drops;   // enemies dropped since the bot's last death or task change
    int8 Objective = NoObjective;
    TaskIntent Intent = TaskIntent::Take;
    ObjectGuid Pending;                     // the trigger found this target to drop; the action takes it
};

/// World thread only (the planner and the bots' AI of battleground maps)
std::unordered_map<ObjectGuid, TetherState> Tethers;

bool HitRecently(ObjectGuid bot, ObjectGuid attacker, uint32 now)
{
    auto it = DamageLog.find(bot);
    if (it == DamageLog.end())
        return false;
    for (Hit const& hit : it->second.Hits)
        if (hit.Attacker == attacker && now - hit.Ms <= sBgConfig.TetherRetaliateSeconds * 1000)
            return true;
    return false;
}

void ForgetBot(ObjectGuid guid)
{
    DamageLog.erase(guid);
    Tethers.erase(guid);
}

char const* IntentName(TaskIntent intent)
{
    switch (intent)
    {
        case TaskIntent::Take: return "take";
        case TaskIntent::Hold: return "hold";
        case TaskIntent::Retake: return "retake";
        case TaskIntent::Escort: return "escort";
        case TaskIntent::Hunt: return "hunt";
        case TaskIntent::Return: return "return";
        case TaskIntent::Squad: return "squad";
        case TaskIntent::Push: return "push";
        case TaskIntent::Drive: return "drive";
    }
    return "?";
}

char const* PhaseName(TaskPhase phase)
{
    return phase == TaskPhase::Early ? "early" : phase == TaskPhase::Mid ? "mid" : "late";
}

/// The counting pass (§2.2): living players of each team near each objective, from one pass over the match's players. It does not
/// need the reactive behaviours' picture, whose keys are off
void CountNear(BgPlan& plan, Map* map)
{
    std::vector<Position> where(plan.Objectives.size());
    std::vector<bool> placed(plan.Objectives.size(), false);
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
    {
        plan.Objectives[i].Near[0] = plan.Objectives[i].Near[1] = 0;
        placed[i] = ObjectivePosition(plan, int8(i), where[i]);
    }
    for (MapReference const& ref : map->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (!player || !player->IsInWorld() || player->IsGameMaster() || !player->IsAlive())
            continue;
        TeamId team = TeamOf(player);
        if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
            continue;
        for (size_t i = 0; i < plan.Objectives.size(); ++i)
            if (placed[i] && player->GetExactDist2d(where[i]) <= NearRange)
                ++plan.Objectives[i].Near[team];
    }
}

bool Charged(BgRules::RallyState state)
{
    return state == BgRules::RallyState::Charging || state == BgRules::RallyState::Fighting || state == BgRules::RallyState::Regroup;
}

constexpr uint32 SquadCooldownMs = 60000;   // a squad dissolved for being too small is not offered again this long
constexpr uint32 SquadAbandonMs = 120000;   // a captain push given up is not the team's task again this long (as the planner's)

/// One plan of a captain squad's push: gather at the staging row until the floor is present or the timeout, then the puller goes
/// in (BgRules::RallyStep, the planner's rules, run for this squad); a push given up blocks the captain for a while
void StepSquad(BgPlan& plan, Map* map, uint32 team, Task const& task, uint32 onPush, std::vector<Player*> const& members,
    std::vector<Player*> const& bots, SquadRecord& squad, uint32 now)
{
    int8 boss = task.Objective;
    if (boss < 0 || size_t(boss) >= plan.Objectives.size() || task.Staging.Row < 0 || size_t(task.Staging.Row) >= plan.Objectives.size())
        return;
    if (squad.Rally.State == BgRules::RallyState::Idle || squad.Rally.State == BgRules::RallyState::Done
        || squad.Rally.State == BgRules::RallyState::Abandoned)
    {
        BgRules::RallyStart(squad.Rally, now);
        squad.Puller.Clear();
    }
    Position const& stage = plan.Objectives[task.Staging.Row].Pos;
    float radius = RallyRadiusOf(plan, boss);
    BgRules::RallyView view;
    view.OnPush = onPush;
    for (Player* player : members)
        if (player->IsAlive() && player->GetExactDist2d(stage) <= radius)
            ++view.Present;
    Objective& objective = plan.Objectives[boss];
    view.BossAlive = objective.Alive;
    Creature* creature = BossOf(plan, map, boss);
    if (creature)
    {
        view.BossInCombat = creature->IsInCombat();
        view.BossEvading = creature->IsInEvadeMode();
    }
    BgRules::RallyParams params;
    params.Floor = task.Staging.Min;
    params.Pct = sBgConfig.RallyPct;
    params.TimeoutMs = task.Staging.TimeoutMs;
    params.TimeoutChargePct = sBgConfig.RallyTimeoutChargePct;
    params.WipePct = sBgConfig.RallyWipePct;
    params.MaxRestarts = sBgConfig.RallyMaxRestarts;
    params.PullDefenders = false;           // the squad is its own: nobody else is called in
    BgRules::RallyState before = squad.Rally.State;
    BgRules::RallyState state = BgRules::RallyStep(squad.Rally, view, params, now);

    // the puller: a tank of the squad nearest the boss, else the nearest melee, else the nearest ranged; never a healer
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
            if (!ai || !bot->IsAlive() || job.Base != Job::Attack || job.Objective != boss || ai->GetRole() == BotRole::Healer)
                continue;
            int rank = ai->GetRole() == BotRole::Tank ? 0 : ai->GetRole() == BotRole::MeleeDps ? 1 : 2;
            float dist = bot->GetExactDist2d(bossPos);
            if (!best || rank < bestRank || (rank == bestRank && dist < bestDist))
            {
                best = bot;
                bestRank = rank;
                bestDist = dist;
            }
        }
        squad.Puller = best ? best->GetGUID() : ObjectGuid::Empty;
    }
    if (state != before)
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} instance {} team {} captain squad on {}: {} (present {}, on the push {})", plan.Def->Name,
            plan.InstanceId, team, objective.Name, BgRules::RallyStateName(state), view.Present, view.OnPush);
    if (state == BgRules::RallyState::Abandoned)
    {
        objective.BlockedUntilMs[team] = now + SquadAbandonMs;
        squad.Rally = BgRules::Rally();
        squad.Puller.Clear();
    }
    else if (state == BgRules::RallyState::Done)
    {
        squad.Rally = BgRules::Rally();
        squad.Puller.Clear();
    }
}
}

bool Coordinator::Coordinates(BgPlan const& plan)
{
    auto [it, created] = Matches.try_emplace({ plan.MapId, plan.InstanceId });
    if (created)
    {
        it->second.Coordinated = sBgConfig.Coordinator && FindProfile(plan.MapId) != nullptr;
        if (it->second.Coordinated)
            TC_LOG_INFO("server.playerbots", "Playerbots: battleground coordinator plans map {} instance {}", plan.MapId, plan.InstanceId);
    }
    return it->second.Coordinated;
}

void Coordinator::Plan(Planner& planner, BgPlan& plan, Battleground* bg, Map* map, std::vector<Player*> const (&members)[2], std::string const& reason,
    uint32 now)
{
    Profile const* profile = FindProfile(plan.MapId);
    if (!profile || !plan.Def || !bg || !map)
        return;
    Match& match = Matches[{ plan.MapId, plan.InstanceId }];
    Context context;
    context.Bg = bg;
    context.MatchMap = map;
    context.Now = now;
    context.PlayMs = uint32(bg->GetInProgressDuration().count());
    if (profile->State)
        profile->State->Read(plan, bg, map, now);
    CountNear(plan, map);

    BgRules::ScoreParams scoreParams;
    scoreParams.TravelPer100Yards = sBgConfig.TravelPer100Yards;
    scoreParams.ThreatPerEnemy = sBgConfig.ThreatPerEnemy;
    scoreParams.ThreatCap = sBgConfig.ThreatCap;
    scoreParams.QuotaFalloffPct = float(sBgConfig.QuotaFalloffPct);
    BgRules::FillParams fillParams;
    fillParams.CommitMs = sBgConfig.CommitSeconds * 1000;
    fillParams.CommitMarginMin = sBgConfig.CommitMarginMin;
    fillParams.CommitMarginPct = float(sBgConfig.CommitMarginPct);
    fillParams.DeadWeight = float(sBgConfig.DeadWeightPct) / 100.0f;
    fillParams.MinTake = sBgConfig.SquadMinTake;
    fillParams.MinSquad = sBgConfig.SquadStageMin;
    BgRules::PushGateParams gateParams;
    gateParams.MarginPct = float(sBgConfig.LanePushMarginPct);
    gateParams.HoldMs = sBgConfig.LanePushHoldSeconds * 1000;
    gateParams.DropMs = sBgConfig.LanePushDropSeconds * 1000;

    for (uint32 team = 0; team < 2; ++team)
    {
        TeamPlan& teamPlan = plan.Teams[team];
        TeamRecord& record = match.Teams[team];
        TeamId us = TeamId(team);
        TeamId them = TeamId(1 - team);

        // ownerless bots are planned; humans and owned bots count (as in the old path)
        std::vector<Player*> bots;
        teamPlan.Others = 0;
        for (Player* player : members[team])
        {
            PlayerbotAI* ai = IsBot(player) ? sPlayerbotMgr->GetAI(player) : nullptr;
            if (ai && !ai->GetMaster())
                bots.push_back(player);
            else
                ++teamPlan.Others;
        }
        teamPlan.Assignable = uint32(bots.size());
        PrepareTeam(plan, team, members[team], bots, planner._streak);

        if (profile->PhaseOf)
            record.Phase = profile->PhaseOf(plan, context, team, record.Phase);
        std::vector<Task> tasks;
        for (Mode const* mode : profile->Modes)
            mode->Offer(plan, context, team, record.Phase, tasks);
        // a captain squad dissolved for being too small waits out its cooldown
        std::erase_if(tasks, [&](Task const& task)
        {
            auto squad = record.Squads.find(task.Objective);
            return task.Intent == TaskIntent::Squad && squad != record.Squads.end() && int32(squad->second.CooldownUntilMs - now) > 0;
        });

        // the general's push: all or nothing behind its gate (§3.3); guards and the defenders stay
        int32 push = -1;
        float bestOther = 0.0f;
        for (size_t t = 0; t < tasks.size(); ++t)
        {
            if (tasks[t].Intent == TaskIntent::Push)
                push = int32(t);
            else if (tasks[t].Intent == TaskIntent::Take)
                bestOther = std::max(bestOther, tasks[t].Value);
        }
        bool pushOn = false;
        if (push >= 0)
        {
            bool locked = teamPlan.RallyObjective == tasks[push].Objective && Charged(teamPlan.Rally.State);
            bool condition = BgRules::PushCondition(SoftGateOpen(plan, context, team), tasks[push].Value, bestOther, gateParams);
            pushOn = BgRules::StepPushGate(record.Gate, condition, locked, now, gateParams);
        }
        else
            record.Gate = BgRules::PushGate();
        std::erase_if(tasks, [&](Task const& task)
        {
            if (task.Intent == TaskIntent::Push)
                return !pushOn;
            return pushOn && task.Intent != TaskIntent::Hold && task.Intent != TaskIntent::Retake;
        });

        // the hold share: the guards of our running assaults first, then the team's seeded share over the holds by value. A rally
        // that restarted with the defenders called in (Bg.Rally.PullDefenders) keeps only the guards
        uint32 want = BgRules::ShareOf(uint32(bots.size()), StanceDefendPct(teamPlan));
        if (pushOn && teamPlan.Rally.DefendersPulled)
            want = 0;
        uint32 fixed = 0;
        std::vector<float> holdValues;
        std::vector<size_t> holds;
        for (size_t t = 0; t < tasks.size(); ++t)
        {
            if (tasks[t].Intent != TaskIntent::Hold)
                continue;
            if (tasks[t].FixedQuota)
                fixed += tasks[t].QuotaMin;
            else
            {
                holds.push_back(t);
                holdValues.push_back(tasks[t].Value);
            }
        }
        std::vector<uint32> shares = BgRules::Spread(holdValues, want > fixed ? want - fixed : 0);
        for (size_t h = 0; h < holds.size(); ++h)
        {
            uint8 quota = uint8(std::min<uint32>(shares[h], 255));
            tasks[holds[h]].QuotaMin = quota;
            tasks[holds[h]].QuotaMax = quota;
        }
        // a hold that gets no share is not offered (a max quota of 0 would mean no cap)
        std::erase_if(tasks, [](Task const& task) { return task.Intent == TaskIntent::Hold && !task.QuotaMax; });
        teamPlan.WantDefenders = want;

        std::vector<BgRules::TaskView> views;
        views.reserve(tasks.size());
        for (Task const& task : tasks)
        {
            BgRules::TaskView view;
            view.Intent = task.Intent;
            view.Value = task.Value;
            Position at;
            if (ObjectivePosition(plan, task.Objective, at))
            {
                view.X = at.GetPositionX();
                view.Y = at.GetPositionY();
            }
            view.QuotaMin = task.QuotaMin;
            view.QuotaMax = task.QuotaMax;
            if (task.Objective >= 0 && size_t(task.Objective) < plan.Objectives.size())
            {
                view.Enemies = plan.Objectives[task.Objective].Near[them];
                view.Allies = plan.Objectives[task.Objective].Near[us];
            }
            views.push_back(view);
        }

        std::vector<BgRules::FillBot> fill;
        fill.reserve(bots.size());
        for (Player* bot : bots)
        {
            BotRecord& bot0 = match.Bots[bot->GetGUID()];
            Assignment const& job = plan.Jobs[bot->GetGUID()];
            BgRules::FillBot entry;
            entry.Id = bot->GetGUID().GetCounter();
            entry.Key = BgRules::ShuffleKey(teamPlan.Seed, entry.Id);
            entry.X = bot->GetPositionX();
            entry.Y = bot->GetPositionY();
            entry.Alive = bot->IsAlive();
            for (size_t t = 0; t < tasks.size(); ++t)
                if (tasks[t].Objective == bot0.Objective && tasks[t].Intent == bot0.Intent)
                    entry.Task = int32(t);
            entry.SinceMs = bot0.SinceMs;
            entry.RejoinUntilMs = job.RezMs ? job.RezMs + sBgConfig.RejoinSeconds * 1000 : 0;
            fill.push_back(entry);
        }
        BgRules::FillResult result = BgRules::FillTasks(views, fill, now, scoreParams, fillParams);
        for (size_t t = 0; t < tasks.size(); ++t)
            if (tasks[t].Intent == TaskIntent::Squad && views[t].Closed)
            {
                SquadRecord& squad = record.Squads[tasks[t].Objective];
                squad = SquadRecord();
                squad.CooldownUntilMs = now + SquadCooldownMs;
            }

        // the jobs the bot side reads: an attack intent attacks its objective, a hold or a retake defends it. PlayValley does the
        // banners (one capper, the others guard), PlayPush the general's push
        uint32 changed = 0;
        record.Living.assign(tasks.size(), 0);
        record.Dead.assign(tasks.size(), 0);
        for (size_t i = 0; i < bots.size(); ++i)
        {
            BgRules::FillBot const& entry = fill[i];
            BotRecord& bot0 = match.Bots[bots[i]->GetGUID()];
            Assignment& job = plan.Jobs[bots[i]->GetGUID()];
            int8 objective = NoObjective;
            Job base = Job::Attack;
            if (entry.Task >= 0)
            {
                Task const& task = tasks[entry.Task];
                bot0.Objective = task.Objective;
                bot0.Intent = task.Intent;
                bot0.SinceMs = entry.SinceMs;
                bot0.Fallback = entry.Fallback;
                objective = task.Objective;
                base = task.Intent == TaskIntent::Hold || task.Intent == TaskIntent::Retake ? Job::Defend : Job::Attack;
                ++(entry.Alive ? record.Living : record.Dead)[entry.Task];
            }
            else
                bot0 = BotRecord();
            if (job.Base != base || job.Objective != objective)
            {
                ++changed;
                job.SinceMs = now;
            }
            job.Base = base;
            job.Now = base;
            job.Objective = objective;
            job.Follow.Clear();

            // the tether's entries (made here, on the world thread); a death or a new task forgets the enemies it dropped
            if (sBgConfig.Tether)
            {
                DamageLog.try_emplace(bots[i]->GetGUID());
                auto [tether, created] = Tethers.try_emplace(bots[i]->GetGUID());
                if (created)
                    tether->second.Budget.LeftMs = sBgConfig.TetherRetaliateBudgetSeconds * 1000;
                if (!entry.Alive || tether->second.Objective != bot0.Objective || tether->second.Intent != bot0.Intent)
                {
                    tether->second.Drops.clear();
                    tether->second.Objective = bot0.Objective;
                    tether->second.Intent = bot0.Intent;
                }
            }
        }
        record.Fallbacks += result.Fallbacks;
        record.Assignments += result.Changed;
        record.Tasks = tasks;

        // the planner's view of the team: .bot bg prints it, StepRally reads the push from AttackTargets
        teamPlan.AttackTargets.clear();
        teamPlan.DefendTargets.clear();
        for (size_t t = 0; t < tasks.size(); ++t)
        {
            if (!record.Living[t] && !record.Dead[t])
                continue;
            // a captain squad runs its own push: in AttackTargets StepRally would take the captain for the team's push
            if (tasks[t].Intent == TaskIntent::Squad)
                continue;
            std::vector<int8>& list = BgRules::AttackIntent(tasks[t].Intent) && tasks[t].Intent != TaskIntent::Retake ? teamPlan.AttackTargets
                : teamPlan.DefendTargets;
            if (std::find(list.begin(), list.end(), tasks[t].Objective) == list.end())
                list.push_back(tasks[t].Objective);
        }
        if (pushOn)
            teamPlan.AttackTargets = { tasks[std::distance(tasks.begin(), std::find_if(tasks.begin(), tasks.end(),
                [](Task const& task) { return task.Intent == TaskIntent::Push; }))].Objective };

        std::string rallyEvent;
        uint32 rallyChanged = 0;
        planner.StepRally(plan, map, team, members[team], bots, rallyChanged, rallyEvent, now);
        // deaths and resurrections (Bg.Rebuff and the rejoin time read them) and the overlays, which stay off with their keys
        // the captain squads' own pushes, and the records of squads no longer offered
        for (size_t t = 0; t < tasks.size(); ++t)
        {
            if (tasks[t].Intent != TaskIntent::Squad)
                continue;
            SquadRecord& squad = record.Squads[tasks[t].Objective];
            if (!record.Living[t] && !record.Dead[t])
            {
                squad.Rally = BgRules::Rally();
                squad.Puller.Clear();
                continue;
            }
            StepSquad(plan, map, team, tasks[t], record.Living[t], members[team], bots, squad, now);
        }
        std::erase_if(record.Squads, [&](auto const& entry)
        {
            bool offered = std::any_of(tasks.begin(), tasks.end(), [&](Task const& task) { return task.Intent == TaskIntent::Squad && task.Objective == entry.first; });
            return !offered && int32(entry.second.CooldownUntilMs - now) <= 0;
        });

        std::string reactReason;
        uint32 reacted = ReactTeam(plan, team, bots, reactReason, now);

        if (changed || rallyChanged || reacted)
        {
            teamPlan.LastReason = !rallyEvent.empty() ? rallyEvent : !changed ? reactReason : reason.empty() ? "periodic" : reason;
            teamPlan.LastReasonMs = now;
            teamPlan.LastChanged = changed + rallyChanged + reacted;
            ++teamPlan.Reassigns;
        }
    }

    // bots that left the match
    std::erase_if(match.Bots, [&plan](auto const& entry)
    {
        if (plan.Jobs.contains(entry.first))
            return false;
        ForgetBot(entry.first);
        return true;
    });
}

void Coordinator::Close(BgPlan const& plan)
{
    auto it = Matches.find({ plan.MapId, plan.InstanceId });
    if (it == Matches.end())
        return;
    for (auto const& [guid, bot] : it->second.Bots)
        ForgetBot(guid);
    Matches.erase(it);
}

void Coordinator::NoteDamage(Unit* attacker, Unit* victim)
{
    // early exits before any other work: the hook runs for every hit on every unit while Playerbots.Bg.Tether is on
    if (!attacker || !victim || !victim->IsPlayer())
        return;
    Map const* map = victim->GetMap();
    if (!map || !map->IsBattleground())
        return;
    Player const* bot = victim->ToPlayer();
    if (!IsBot(bot))
        return;
    auto it = DamageLog.find(bot->GetGUID());   // find only: entries are made on the world thread
    if (it == DamageLog.end())
        return;
    uint32 now = GameTime::GetGameTimeMS();
    auto note = [&](ObjectGuid source)
    {
        Hit* slot = nullptr;
        for (Hit& hit : it->second.Hits)
        {
            if (hit.Attacker == source)
            {
                slot = &hit;
                break;
            }
            if (!slot || hit.Ms < slot->Ms)
                slot = &hit;
        }
        slot->Attacker = source;
        slot->Ms = now;
    };
    note(attacker->GetGUID());
    if (Unit const* owner = attacker->GetCharmerOrOwner())
        note(owner->GetGUID());
}

void Coordinator::NoteLeg(Player const* bot, Position const& from, Position const& to)
{
    if (!sBgConfig.Tether)
        return;
    auto it = Tethers.find(bot->GetGUID());
    if (it == Tethers.end())
        return;
    it->second.LegFrom = from;
    it->second.LegTo = to;
    it->second.LegMs = GameTime::GetGameTimeMS();
}

bool Coordinator::TetherWants(PlayerbotAI& ai)
{
    if (!sBgConfig.Tether)
        return false;
    Player* bot = ai.GetBot();
    auto tit = Tethers.find(bot->GetGUID());
    if (tit == Tethers.end())
        return false;
    TetherState& tether = tit->second;
    tether.Pending.Clear();
    if (!bot->IsAlive() || !bot->IsInCombat() || ai.GetMaster() || !bot->InBattleground())
        return false;
    BgPlan const* plan = sBgPlanner.Find(bot->GetMapId(), bot->GetInstanceId());
    auto mit = plan ? Matches.find({ plan->MapId, plan->InstanceId }) : Matches.end();
    if (mit == Matches.end() || !mit->second.Coordinated)
        return false;
    auto bit = mit->second.Bots.find(bot->GetGUID());
    if (bit == mit->second.Bots.end() || bit->second.Objective == NoObjective)
        return false;
    // a cast runs (a banner's 8 s open, a heal): the bot decides its fight again when it is done
    if (bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        return false;
    Unit* victim = bot->GetVictim();
    if (!victim)
        victim = ai.SelectCombatTarget();
    if (!victim || !victim->IsAlive())
        return false;
    Position anchor;
    if (!ObjectivePosition(*plan, bit->second.Objective, anchor))
        return false;

    uint32 now = GameTime::GetGameTimeMS();
    TeamId team = TeamOf(bot);
    TaskIntent intent = bit->second.Intent;
    BgRules::TetherCheck check;
    check.Role = intent == TaskIntent::Hold || intent == TaskIntent::Retake ? TetherRole::Defender
        : intent == TaskIntent::Squad || intent == TaskIntent::Push ? TetherRole::Puller : TetherRole::Fighter;
    // back from a graveyard and still on the way: only what hits it
    auto job = plan->Jobs.find(bot->GetGUID());
    if (job != plan->Jobs.end() && job->second.RezMs && now - job->second.RezMs < sBgConfig.RejoinSeconds * 1000
        && bot->GetExactDist2d(anchor) > sBgConfig.TetherObjectiveYards)
        check.Role = TetherRole::Travel;
    check.BotX = bot->GetPositionX();
    check.BotY = bot->GetPositionY();
    check.TargetX = victim->GetPositionX();
    check.TargetY = victim->GetPositionY();
    check.AnchorX = anchor.GetPositionX();
    check.AnchorY = anchor.GetPositionY();
    if (tether.LegMs && now - tether.LegMs < 15000)
    {
        check.HasLeg = true;
        check.LegAX = tether.LegFrom.GetPositionX();
        check.LegAY = tether.LegFrom.GetPositionY();
        check.LegBX = tether.LegTo.GetPositionX();
        check.LegBY = tether.LegTo.GetPositionY();
    }
    check.HitUsRecently = HitRecently(bot->GetGUID(), victim->GetGUID(), now);
    // a long calm (the trigger only runs in combat) fills the retaliation budget again
    uint32 budgetMs = sBgConfig.TetherRetaliateBudgetSeconds * 1000;
    if (tether.LastCheckMs && now - tether.LastCheckMs >= 20000)
        tether.Budget.LeftMs = budgetMs;
    check.BudgetLeftMs = tether.Budget.LeftMs;
    BgRules::TetherZone zone;
    zone.ObjectiveYards = sBgConfig.TetherObjectiveYards;
    zone.PathYards = sBgConfig.TetherPathYards;
    BgRules::TetherVerdict verdict = BgRules::JudgeTether(check, zone);

    uint32 dt = tether.LastCheckMs ? std::min<uint32>(now - tether.LastCheckMs, 2000) : 0;
    tether.LastCheckMs = now;
    bool retaliating = verdict == BgRules::TetherVerdict::Retaliate;
    if (retaliating && !tether.Retaliating && (team == TEAM_ALLIANCE || team == TEAM_HORDE))
        ++mit->second.TetherRetaliations[team];
    tether.Retaliating = retaliating;
    BgRules::StepRetaliateBudget(tether.Budget, retaliating, verdict == BgRules::TetherVerdict::Keep, dt, now, budgetMs);
    if (verdict != BgRules::TetherVerdict::Drop)
        return false;
    tether.Pending = victim->GetGUID();
    return true;
}

bool Coordinator::TakeTetherDrop(PlayerbotAI& ai, ObjectGuid& enemy, uint32& ignoreMs)
{
    Player* bot = ai.GetBot();
    auto tit = Tethers.find(bot->GetGUID());
    if (tit == Tethers.end() || tit->second.Pending.IsEmpty())
        return false;
    TetherState& tether = tit->second;
    enemy = tether.Pending;
    tether.Pending.Clear();
    uint32 earlier = tether.Drops[enemy]++;
    ignoreMs = BgRules::TetherIgnoreMs(earlier, sBgConfig.TetherIgnoreSeconds * 1000, sBgConfig.TetherIgnoreMaxSeconds * 1000);
    TeamId team = TeamOf(bot);
    auto mit = Matches.find({ bot->GetMapId(), bot->GetInstanceId() });
    if (mit != Matches.end() && (team == TEAM_ALLIANCE || team == TEAM_HORDE))
    {
        ++mit->second.TetherDrops[team];
        if (earlier)
            ++mit->second.TetherReengages[team];
    }
    return true;
}

void Coordinator::TetherCounts(BgPlan const& plan, uint32 team, uint32& drops, uint32& retaliations, uint32& reengages)
{
    drops = retaliations = reengages = 0;
    auto it = Matches.find({ plan.MapId, plan.InstanceId });
    if (it == Matches.end() || team > 1)
        return;
    drops = it->second.TetherDrops[team];
    retaliations = it->second.TetherRetaliations[team];
    reengages = it->second.TetherReengages[team];
}

std::vector<std::string> Coordinator::Report(BgPlan const& plan)
{
    std::vector<std::string> lines;
    auto it = Matches.find({ plan.MapId, plan.InstanceId });
    if (it == Matches.end() || !it->second.Coordinated)
    {
        lines.push_back("The coordinator does not plan this match (Playerbots.Bg.Coordinator off when it began, or no profile for the map).");
        return lines;
    }
    for (uint32 team = 0; team < 2; ++team)
    {
        TeamRecord const& record = it->second.Teams[team];
        TeamPlan const& teamPlan = plan.Teams[team];
        lines.push_back(Trinity::StringFormat("{}: phase {}, general's push {}, fallbacks {} of {} assignments ({}%), defend share {}",
            team == TEAM_ALLIANCE ? "Alliance" : "Horde", PhaseName(record.Phase), record.Gate.On ? "on" : "off", record.Fallbacks,
            record.Assignments, record.Assignments ? record.Fallbacks * 100 / record.Assignments : 0, teamPlan.WantDefenders));
        for (auto const& [boss, squad] : record.Squads)
            lines.push_back(Trinity::StringFormat("  captain squad on {}: {}{}", ObjectiveName(plan, boss), BgRules::RallyStateName(squad.Rally.State),
                int32(squad.CooldownUntilMs - GameTime::GetGameTimeMS()) > 0 ? " (cooling down)" : ""));
        for (size_t t = 0; t < record.Tasks.size(); ++t)
        {
            Task const& task = record.Tasks[t];
            Objective const* objective = task.Objective >= 0 && size_t(task.Objective) < plan.Objectives.size() ? &plan.Objectives[task.Objective] : nullptr;
            lines.push_back(Trinity::StringFormat("  {} {}: value {:.1f}, quota {}-{}, bots {}{}, near us {} them {}", IntentName(task.Intent),
                ObjectiveName(plan, task.Objective), task.Value, uint32(task.QuotaMin), task.QuotaMax ? std::to_string(task.QuotaMax) : "any",
                t < record.Living.size() ? record.Living[t] : 0u, t < record.Dead.size() && record.Dead[t] ? Trinity::StringFormat(" (+{} dead)", record.Dead[t]) : "",
                objective ? objective->Near[team] : 0u, objective ? objective->Near[1 - team] : 0u));
        }
    }
    return lines;
}

bool Coordinator::SquadPush(BgPlan const& plan, uint32 team, int8 boss, BgRules::RallyState& state, ObjectGuid& puller)
{
    if (team > 1)
        return false;
    auto it = Matches.find({ plan.MapId, plan.InstanceId });
    if (it == Matches.end() || !it->second.Coordinated)
        return false;
    auto squad = it->second.Teams[team].Squads.find(boss);
    if (squad == it->second.Teams[team].Squads.end() || squad->second.Rally.State == BgRules::RallyState::Idle)
        return false;
    state = squad->second.Rally.State;
    puller = squad->second.Puller;
    return true;
}

void Coordinator::FallbackCounts(BgPlan const& plan, uint32 team, uint32& fallbacks, uint32& assignments)
{
    fallbacks = assignments = 0;
    auto it = Matches.find({ plan.MapId, plan.InstanceId });
    if (it == Matches.end() || !it->second.Coordinated || team > 1)
        return;
    fallbacks = it->second.Teams[team].Fallbacks;
    assignments = it->second.Teams[team].Assignments;
}
}
