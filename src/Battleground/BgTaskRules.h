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

#ifndef Playerbots_BgTaskRules_h__
#define Playerbots_BgTaskRules_h__

// The battleground coordinator's rules that need no game objects (docs/design/BG_COORDINATOR.md §2-§4): task scores, the fill of a
// team's bots into tasks (quotas, the dead at a partial weight, commitment, the fallback, minimum squads), squad staging, the general
// push gate, the combat tether and the lane phases. Header only, tested by tests/bg_rules_test.cpp without TrinityCore

#include "Battleground/BgRules.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Playerbots::BgRules
{
// ---- tasks and their scores (§2.3) ----

/// What a mode offers a team: Take also covers a tower to burn; Squad is a boss with a quota (a captain), Push the general (all or nothing)
enum class TaskIntent : uint8
{
    Take,
    Hold,
    Retake,
    Escort,
    Hunt,
    Return,
    Squad,
    Push,
    Drive
};

enum class TaskPhase : uint8
{
    Early,
    Mid,
    Late
};

inline bool AttackIntent(TaskIntent intent)
{
    return intent == TaskIntent::Take || intent == TaskIntent::Retake || intent == TaskIntent::Squad || intent == TaskIntent::Push;
}

struct ScoreParams
{
    float TravelPer100Yards = 0.2f;     // Playerbots.Bg.Score.TravelPer100Yards
    float ThreatPerEnemy = 0.5f;        // Playerbots.Bg.Score.ThreatPerEnemy
    uint32 ThreatCap = 6;               // Playerbots.Bg.Score.ThreatCap: net enemies counted at most
    float QuotaFalloffPct = 10.0f;      // Playerbots.Bg.Score.QuotaFalloffPct: the value left at or past a task's max quota
};

/// The threat term: so much per enemy more than allies near the anchor (living players, the counting pass), capped
inline float Threat(uint32 enemies, uint32 allies, ScoreParams const& params)
{
    uint32 net = enemies > allies ? enemies - allies : 0;
    return params.ThreatPerEnemy * float(std::min(net, params.ThreatCap));
}

struct TaskView
{
    TaskIntent Intent = TaskIntent::Take;
    float Value = 0.0f;                 // V x W[phase]
    float X = 0.0f;                     // the anchor
    float Y = 0.0f;
    uint8 QuotaMin = 0;                 // Hold: filled first up to this (pass 1)
    uint8 QuotaMax = 0;                 // 0: no cap
    uint32 Enemies = 0;                 // living players near the anchor (the counting pass)
    uint32 Allies = 0;
    bool Closed = false;                // dissolved by pass 3 (a Take under the minimum squad)
};

/// A task worth nothing in this phase is not offered at all (a phase weight of 0 excludes it)
inline bool Offered(TaskView const& task)
{
    return task.Value > 0.0f && !task.Closed;
}

/// V x W x f - travel - threat. f applies to the value term only: on the whole score it would raise a negative score toward zero and
/// an over-quota task would draw bots instead of capping them
inline float TaskScore(TaskView const& task, float x, float y, float filled, ScoreParams const& params)
{
    float f = task.QuotaMax && filled >= float(task.QuotaMax) ? params.QuotaFalloffPct / 100.0f : 1.0f;
    float travel = params.TravelPer100Yards * std::hypot(task.X - x, task.Y - y) / 100.0f;
    return task.Value * f - travel - Threat(task.Enemies, task.Allies, params);
}

// ---- the fill (§2.3, §2.5, §2.6) ----

struct FillBot
{
    uint64 Id = 0;
    uint64 Key = 0;                     // ShuffleKey: free bots pick in this order
    float X = 0.0f;
    float Y = 0.0f;
    bool Alive = true;
    int32 Task = -1;                    // in: the current task (-1 none); out: the task after the fill
    uint32 SinceMs = 0;                 // when it got the current task (set to now when the fill changes it)
    uint32 RejoinUntilMs = 0;           // back from a graveyard: not rescored and not released before this
    bool Fallback = false;              // out: this fill gave it the fallback task
};

struct FillParams
{
    uint32 CommitMs = 30000;            // Playerbots.Bg.CommitSeconds
    float CommitMarginMin = 1.0f;       // Playerbots.Bg.CommitMarginMin
    float CommitMarginPct = 25.0f;      // Playerbots.Bg.CommitMarginPct
    float DeadWeight = 0.5f;            // Playerbots.Bg.Quota.DeadWeightPct / 100
    uint32 MinTake = 4;                 // Playerbots.Bg.Squad.MinTake
};

struct FillResult
{
    uint32 Changed = 0;                 // bots whose task changed
    uint32 Fallbacks = 0;               // bots that took the fallback task in this fill
    uint32 Closed = 0;                  // Take tasks dissolved under the minimum squad
};

/// After its commitment a bot moves only when the best task beats its current one by max(the floor, the share of |current|): scores
/// sit near zero or below it, so a margin of a share alone would let 0.1 beat 0.05
inline bool CommitMarginMet(float best, float current, FillParams const& params)
{
    return best - current >= std::max(params.CommitMarginMin, params.CommitMarginPct / 100.0f * std::fabs(current));
}

/// The fallback when no task scores above 0: the attack task with the most living bots (ties: the higher value), else the
/// highest-value attack task, else the highest-value Hold task; -1 with nothing offered
inline int32 MainPush(std::vector<TaskView> const& tasks, std::vector<FillBot> const& bots)
{
    std::vector<uint32> living(tasks.size(), 0);
    for (FillBot const& bot : bots)
        if (bot.Alive && bot.Task >= 0 && size_t(bot.Task) < tasks.size())
            ++living[bot.Task];
    int32 best = -1;
    for (size_t i = 0; i < tasks.size(); ++i)
    {
        if (!Offered(tasks[i]) || !AttackIntent(tasks[i].Intent))
            continue;
        if (best < 0 || living[i] > living[best] || (living[i] == living[best] && tasks[i].Value > tasks[best].Value))
            best = int32(i);
    }
    if (best >= 0)
        return best;
    for (size_t i = 0; i < tasks.size(); ++i)
        if (Offered(tasks[i]) && tasks[i].Intent == TaskIntent::Hold && (best < 0 || tasks[i].Value > tasks[best].Value))
            best = int32(i);
    return best;
}

/// The team's bots over the offered tasks, in three passes:
/// - kept: a dead bot, a bot inside its commitment or its rejoin time keeps its task; a task no longer offered frees its bots.
///   A living bot counts 1 toward its task's quota, a dead one DeadWeight. Past a task's max, the most recently assigned living bots
///   that are not rejoining are released first
/// - pass 1: Hold tasks below their minimum take the nearest free living bots
/// - pass 2: the other free bots, in key order, take their best task (counting live); one that already has a task moves only past the
///   commitment margin; one with nothing above 0 joins the main push (MainPush, counted as a fallback)
/// - pass 3: a Take task whose weighted count is above 0 and below MinTake is closed and its bots fill again; repeated until none
///   is left
inline FillResult FillTasks(std::vector<TaskView>& tasks, std::vector<FillBot>& bots, uint32 nowMs, ScoreParams const& sp, FillParams const& fp)
{
    FillResult result;
    std::vector<int32> before(bots.size());
    std::vector<float> count(tasks.size(), 0.0f);
    std::vector<bool> isFree(bots.size(), false);
    auto valid = [&](int32 task) { return task >= 0 && size_t(task) < tasks.size() && Offered(tasks[task]); };
    auto weight = [&](FillBot const& bot) { return bot.Alive ? 1.0f : fp.DeadWeight; };
    auto rejoining = [&](FillBot const& bot) { return int32(bot.RejoinUntilMs - nowMs) > 0; };

    for (size_t i = 0; i < bots.size(); ++i)
    {
        FillBot& bot = bots[i];
        before[i] = bot.Task;
        bot.Fallback = false;
        if (!valid(bot.Task))
        {
            bot.Task = -1;
            isFree[i] = bot.Alive;
            continue;
        }
        count[bot.Task] += weight(bot);
        bool committed = !bot.Alive || nowMs - bot.SinceMs < fp.CommitMs || rejoining(bot);
        isFree[i] = !committed;
    }

    // over quota (the dead came back): the latest assigned living bots that are not rejoining leave first
    for (size_t t = 0; t < tasks.size(); ++t)
    {
        if (!tasks[t].QuotaMax || count[t] <= float(tasks[t].QuotaMax))
            continue;
        std::vector<size_t> members;
        for (size_t i = 0; i < bots.size(); ++i)
            if (bots[i].Task == int32(t) && bots[i].Alive && !rejoining(bots[i]))
                members.push_back(i);
        std::sort(members.begin(), members.end(), [&](size_t a, size_t b) { return bots[a].SinceMs > bots[b].SinceMs; });
        for (size_t i : members)
        {
            if (count[t] <= float(tasks[t].QuotaMax))
                break;
            bots[i].Task = -1;
            count[t] -= 1.0f;
            isFree[i] = true;
        }
    }

    auto assign = [&](size_t i, int32 task)
    {
        FillBot& bot = bots[i];
        if (bot.Task >= 0)
            count[bot.Task] -= weight(bot);
        bot.Task = task;
        if (task >= 0)
            count[task] += weight(bot);
    };

    // pass 1: Hold minimums from the nearest free living bots
    for (size_t t = 0; t < tasks.size(); ++t)
    {
        TaskView const& task = tasks[t];
        while (Offered(task) && task.Intent == TaskIntent::Hold && count[t] < float(task.QuotaMin))
        {
            int32 nearest = -1;
            float nearestDist = 0.0f;
            for (size_t i = 0; i < bots.size(); ++i)
            {
                if (!isFree[i] || bots[i].Task == int32(t))
                    continue;
                float d = std::hypot(task.X - bots[i].X, task.Y - bots[i].Y);
                if (nearest < 0 || d < nearestDist)
                {
                    nearest = int32(i);
                    nearestDist = d;
                }
            }
            if (nearest < 0)
                break;
            assign(size_t(nearest), int32(t));
            isFree[nearest] = false;
        }
    }

    // pass 2 for a set of bots: best task with live counts, the margin for a bot that has one, the fallback for one with nothing
    auto pick = [&](std::vector<size_t> const& order)
    {
        for (size_t i : order)
        {
            FillBot& bot = bots[i];
            int32 best = -1;
            float bestScore = 0.0f;
            float currentScore = 0.0f;
            for (size_t t = 0; t < tasks.size(); ++t)
            {
                if (!Offered(tasks[t]))
                    continue;
                float filled = count[t] - (bot.Task == int32(t) ? weight(bot) : 0.0f);
                float score = TaskScore(tasks[t], bot.X, bot.Y, filled, sp);
                if (bot.Task == int32(t))
                    currentScore = score;
                if (best < 0 || score > bestScore)
                {
                    best = int32(t);
                    bestScore = score;
                }
            }
            if (bot.Task >= 0)
            {
                if (best >= 0 && best != bot.Task && CommitMarginMet(bestScore, currentScore, fp))
                    assign(i, best);
                continue;
            }
            if (best >= 0 && bestScore > 0.0f)
                assign(i, best);
            else if (int32 push = MainPush(tasks, bots); push >= 0)
            {
                assign(i, push);
                bot.Fallback = true;
                ++result.Fallbacks;
            }
        }
    };

    std::vector<size_t> order;
    for (size_t i = 0; i < bots.size(); ++i)
        if (isFree[i])
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return bots[a].Key < bots[b].Key; });
    pick(order);

    // pass 3: dissolve Take squads under the minimum, smallest first, and fill their bots again. The size is the weighted count: a
    // squad of 3 living and 6 dead waiting for the spirit healer is not small, its dead come back to it
    for (;;)
    {
        int32 smallest = -1;
        for (size_t t = 0; t < tasks.size(); ++t)
            if (Offered(tasks[t]) && tasks[t].Intent == TaskIntent::Take && count[t] > 0.0f && count[t] < float(fp.MinTake)
                && (smallest < 0 || count[t] < count[smallest]))
                smallest = int32(t);
        if (smallest < 0)
            break;
        tasks[smallest].Closed = true;
        ++result.Closed;
        std::vector<size_t> released;
        for (size_t i = 0; i < bots.size(); ++i)
        {
            if (bots[i].Task != smallest)
                continue;
            assign(i, -1);
            if (bots[i].Alive)
            {
                if (bots[i].Fallback)
                {
                    bots[i].Fallback = false;
                    --result.Fallbacks;
                }
                released.push_back(i);
            }
        }
        std::sort(released.begin(), released.end(), [&](size_t a, size_t b) { return bots[a].Key < bots[b].Key; });
        pick(released);
    }

    for (size_t i = 0; i < bots.size(); ++i)
        if (bots[i].Task != before[i])
        {
            bots[i].SinceMs = nowMs;
            ++result.Changed;
        }
    return result;
}

// ---- squads (§2.4) ----

enum class SquadStage : uint8
{
    Staging,                            // members walk to the staging point
    Moving                              // to the anchor; a member who joins now goes straight there
};

/// Staging ends once need living members stand at the point (within Playerbots.Bg.Squad.StageRadius) or the timeout runs out
inline SquadStage StepStage(SquadStage stage, uint32 present, uint32 need, uint32 sinceMs, uint32 timeoutMs, uint32 nowMs)
{
    if (stage == SquadStage::Moving || !need || present >= need || nowMs - sinceMs >= timeoutMs)
        return SquadStage::Moving;
    return stage;
}

// ---- the general push gate (§3.3) ----

struct PushGate
{
    bool On = false;
    uint32 CondSinceMs = 0;             // the start condition has held since (0: not holding)
    uint32 OffSinceMs = 0;              // both start conditions false since, while only gathering (0: not)
};

struct PushGateParams
{
    float MarginPct = 20.0f;            // Playerbots.Bg.Lane.PushMarginPct
    uint32 HoldMs = 10000;              // Playerbots.Bg.Lane.PushHoldSeconds
    uint32 DropMs = 30000;              // Playerbots.Bg.Lane.PushDropSeconds
};

/// The general's push starts when the soft gate is open or its value beats the best other Take by the margin
inline bool PushCondition(bool softGate, float generalValue, float bestOtherValue, PushGateParams const& params)
{
    return softGate || (generalValue > 0.0f && generalValue >= (1.0f + params.MarginPct / 100.0f) * bestOtherValue);
}

/// On after the condition held HoldMs; while only gathering (not locked) off again after both conditions stayed false DropMs; never
/// off from scores while the push charges, fights or regroups (locked). The rally's own Done/Abandoned resets the gate (the caller)
inline bool StepPushGate(PushGate& gate, bool condition, bool locked, uint32 nowMs, PushGateParams const& params)
{
    uint32 stamp = nowMs ? nowMs : 1;
    if (!gate.On)
    {
        gate.OffSinceMs = 0;
        if (!condition)
        {
            gate.CondSinceMs = 0;
            return false;
        }
        if (!gate.CondSinceMs)
            gate.CondSinceMs = stamp;
        if (stamp - gate.CondSinceMs >= params.HoldMs)
            gate.On = true;
        return gate.On;
    }
    if (locked || condition)
    {
        gate.OffSinceMs = 0;
        return true;
    }
    if (!gate.OffSinceMs)
        gate.OffSinceMs = stamp;
    if (stamp - gate.OffSinceMs >= params.DropMs)
    {
        gate.On = false;
        gate.CondSinceMs = 0;
        gate.OffSinceMs = 0;
    }
    return gate.On;
}

// ---- the combat tether (§2.7) ----

enum class TetherRole : uint8
{
    Fighter,
    Defender,
    Capper,
    Escort,
    Puller,                             // and a boss squad: the boss-room leash rules them
    Carrier,
    Travel,                             // back from a graveyard, rejoining
    Driver
};

enum class TetherVerdict : uint8
{
    Keep,                               // inside the zone
    Retaliate,                          // outside it, but it hit the bot and the budget lasts
    Drop                                // ignore it and go on
};

struct TetherZone
{
    float ObjectiveYards = 40.0f;       // Playerbots.Bg.Tether.ObjectiveYards
    float PathYards = 25.0f;            // Playerbots.Bg.Tether.PathYards
    float CapperYards = 20.0f;
    float EscortYards = 20.0f;
};

struct TetherCheck
{
    TetherRole Role = TetherRole::Fighter;
    float BotX = 0.0f, BotY = 0.0f;
    float TargetX = 0.0f, TargetY = 0.0f;   // the bot's victim
    float AnchorX = 0.0f, AnchorY = 0.0f;   // the task's anchor; Escort: the unit it escorts
    bool HasLeg = false;                    // the bot's current mesh leg; without one, the straight segment from the bot to the anchor
    float LegAX = 0.0f, LegAY = 0.0f, LegBX = 0.0f, LegBY = 0.0f;
    bool HitUsRecently = false;             // the victim damaged the bot within Playerbots.Bg.Tether.RetaliateSeconds
    uint32 BudgetLeftMs = 0;                // what is left of the retaliation budget
};

inline float DistToSegment(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

inline bool InTetherZone(TetherCheck const& check, TetherZone const& zone)
{
    float toAnchor = std::hypot(check.TargetX - check.AnchorX, check.TargetY - check.AnchorY);
    switch (check.Role)
    {
        case TetherRole::Fighter:
        case TetherRole::Defender:
        {
            if (toAnchor <= zone.ObjectiveYards)
                return true;
            float toPath = check.HasLeg ? DistToSegment(check.TargetX, check.TargetY, check.LegAX, check.LegAY, check.LegBX, check.LegBY)
                : DistToSegment(check.TargetX, check.TargetY, check.BotX, check.BotY, check.AnchorX, check.AnchorY);
            return toPath <= zone.PathYards;
        }
        case TetherRole::Capper:
            return toAnchor <= zone.CapperYards;
        case TetherRole::Escort:
            return toAnchor <= zone.EscortYards;
        case TetherRole::Puller:
            return true;
        default:
            return false;
    }
}

/// Inside the zone keep fighting; outside, fight back what hit the bot while the budget lasts (never a carrier or a driver); else drop
inline TetherVerdict JudgeTether(TetherCheck const& check, TetherZone const& zone)
{
    if (InTetherZone(check, zone))
        return TetherVerdict::Keep;
    if (check.Role != TetherRole::Carrier && check.Role != TetherRole::Driver && check.HitUsRecently && check.BudgetLeftMs > 0)
        return TetherVerdict::Retaliate;
    return TetherVerdict::Drop;
}

/// How long a dropped enemy is ignored: the base, doubled for each earlier drop of the same enemy, at most the cap (the counter
/// resets when the bot dies or its task changes)
inline uint32 TetherIgnoreMs(uint32 earlierDrops, uint32 baseMs = 8000, uint32 maxMs = 60000)
{
    uint64 ms = baseMs;
    for (uint32 i = 0; i < earlierDrops && ms < maxMs; ++i)
        ms *= 2;
    return uint32(std::min<uint64>(ms, maxMs));
}

struct RetaliateBudget
{
    uint32 LeftMs = 10000;
    uint32 CalmSinceMs = 0;             // in the zone or out of combat since (0: not)
};

/// Retaliating spends the budget; RefillAfterMs in the zone or out of combat fills it again
inline void StepRetaliateBudget(RetaliateBudget& budget, bool retaliating, bool calm, uint32 dtMs, uint32 nowMs, uint32 fullMs = 10000,
    uint32 refillAfterMs = 20000)
{
    if (retaliating)
    {
        budget.LeftMs = budget.LeftMs > dtMs ? budget.LeftMs - dtMs : 0;
        budget.CalmSinceMs = 0;
        return;
    }
    if (!calm)
    {
        budget.CalmSinceMs = 0;
        return;
    }
    uint32 stamp = nowMs ? nowMs : 1;
    if (!budget.CalmSinceMs)
        budget.CalmSinceMs = stamp;
    if (stamp - budget.CalmSinceMs >= refillAfterMs)
        budget.LeftMs = fullMs;
}

// ---- the lane phases (§4) ----

struct LanePhaseView
{
    uint32 PushTowersTaken = 0;         // towers on our push path under our assault or burnt
    uint32 PushTowers = 2;
    uint32 EnemyTowersBurnt = 0;
    uint32 LateTowers = 2;              // the general's requires: this many enemy towers burnt
    uint32 EnemyScore = 0;
    uint32 EnemyStart = 0;              // 0: no score gate
    uint32 PlayMs = 0;                  // since the gates opened (Battleground::GetInProgressDuration)
};

struct LanePhaseParams
{
    uint32 MidAfterMs = 6 * 60000;      // Playerbots.Bg.Lane.MidAfterMinutes
    uint32 LateScorePct = 40;           // Playerbots.Bg.SoftGateScorePct
    uint32 LateAfterMs = 20 * 60000;    // Playerbots.Bg.SoftGateMinutes
};

/// Early until both push-path towers are taken or MidAfter; late once LateTowers enemy towers burnt, the enemy score is down to the
/// share, or LateAfter. A phase never goes back
inline TaskPhase LanePhase(TaskPhase current, LanePhaseView const& view, LanePhaseParams const& params)
{
    bool late = view.EnemyTowersBurnt >= view.LateTowers || (view.EnemyStart && view.EnemyScore * 100 <= view.EnemyStart * params.LateScorePct)
        || view.PlayMs >= params.LateAfterMs;
    bool mid = view.PushTowersTaken >= view.PushTowers || view.PlayMs >= params.MidAfterMs;
    TaskPhase next = late ? TaskPhase::Late : mid ? TaskPhase::Mid : TaskPhase::Early;
    return std::max(current, next);
}
}

#endif
