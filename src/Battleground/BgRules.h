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

#ifndef Playerbots_BgRules_h__
#define Playerbots_BgRules_h__

// The rules of the battleground team planner that need no game objects (docs/design/BG_PLAN.md §2): the seeded roll of the
// defence share, the seeded shuffle, the attack/defend split with its hold time, and the spread of defenders over the held
// objectives. Header only, so tests/bg_rules_test.cpp builds it without TrinityCore

#include "Define.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Playerbots::BgRules
{
/// splitmix64 finalizer: a fixed mixer, the same on every build (std::hash is not)
inline uint64 Mix(uint64 x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// The seed of one team in one match: instance id, team (0 Alliance, 1 Horde) and the match start time (seconds). Fixed once
/// when the plan is made, so the split does not roll again later in the match
inline uint64 MatchSeed(uint32 instanceId, uint32 teamIndex, uint64 startTime)
{
    return Mix(Mix(instanceId) ^ Mix(startTime * 0x2545F4914F6CDD1Dull) ^ (uint64(teamIndex) + 1) * 0xD1B54A32D192ED03ull);
}

/// The defence share in percent, minPct..maxPct inclusive (swapped when given the wrong way round)
inline uint32 RollDefendPct(uint64 seed, uint32 minPct, uint32 maxPct)
{
    if (minPct > maxPct)
        std::swap(minPct, maxPct);
    minPct = std::min<uint32>(minPct, 100);
    maxPct = std::min<uint32>(maxPct, 100);
    return minPct + uint32(Mix(seed) % (maxPct - minPct + 1));
}

/// Where a bot stands in the seeded shuffle of its team (low key first). Not guid order: the same bot lands elsewhere next match
inline uint64 ShuffleKey(uint64 seed, uint64 guidLow)
{
    return Mix(seed ^ Mix(guidLow));
}

/// pct percent of count, rounded to the nearest
inline uint32 ShareOf(uint32 count, uint32 pct)
{
    return (count * pct + 50) / 100;
}

enum class Job : uint8
{
    None,       // not given a job yet (new in the match, or its objective was lost)
    Attack,
    Defend,
    Carrier,    // carries a flag: not part of the split
    Escort      // an attacker that guards a carrier of its team for now
};

inline char const* JobName(Job job)
{
    switch (job)
    {
        case Job::Attack: return "attack";
        case Job::Defend: return "defend";
        case Job::Carrier: return "carrier";
        case Job::Escort: return "escort";
        default: return "none";
    }
}

/// One bot the split may move
struct Seat
{
    uint64 Id = 0;              // guid counter, only for the caller
    uint64 Key = 0;             // ShuffleKey
    Job JobNow = Job::None;     // Attack, Defend or None; others are left alone
    uint32 SinceMs = 0;         // when it got that job
    float DefendCost = 0.0f;    // how far it is from an objective to defend: the nearest move first, the farthest come back first
};

/// Splits the seats into attackers and defenders, wantDefenders of them defending.
/// - A seat without a job takes one at once, in shuffle order: defend while defenders are short, attack after that.
/// - Then, while the count is still off, only seats whose job is at least holdMs old change, the nearest to defend first
///   (an attacker becoming a defender) or the farthest first (a defender going back to attack). Ties go by shuffle order.
/// Only the difference moves. Returns how many seats changed their job; SinceMs is set to nowMs for those.
inline uint32 Split(std::vector<Seat>& seats, uint32 wantDefenders, uint32 nowMs, uint32 holdMs)
{
    uint32 defenders = 0;
    std::vector<Seat*> fresh;
    for (Seat& seat : seats)
    {
        if (seat.JobNow == Job::Defend)
            ++defenders;
        else if (seat.JobNow == Job::None)
            fresh.push_back(&seat);
    }

    uint32 changed = 0;
    std::sort(fresh.begin(), fresh.end(), [](Seat const* a, Seat const* b) { return a->Key < b->Key; });
    for (Seat* seat : fresh)
    {
        seat->JobNow = defenders < wantDefenders ? Job::Defend : Job::Attack;
        if (seat->JobNow == Job::Defend)
            ++defenders;
        seat->SinceMs = nowMs;
        ++changed;
    }

    auto held = [&](Seat const* seat) { return nowMs - seat->SinceMs < holdMs; };
    if (defenders != wantDefenders)
    {
        bool promote = defenders < wantDefenders;
        std::vector<Seat*> movers;
        for (Seat& seat : seats)
            if (seat.JobNow == (promote ? Job::Attack : Job::Defend) && !held(&seat) && std::find(fresh.begin(), fresh.end(), &seat) == fresh.end())
                movers.push_back(&seat);

        std::sort(movers.begin(), movers.end(), [promote](Seat const* a, Seat const* b)
        {
            if (a->DefendCost != b->DefendCost)
                return promote ? a->DefendCost < b->DefendCost : a->DefendCost > b->DefendCost;
            return a->Key < b->Key;
        });

        for (Seat* seat : movers)
        {
            if (defenders == wantDefenders)
                break;
            seat->JobNow = promote ? Job::Defend : Job::Attack;
            seat->SinceMs = nowMs;
            defenders += promote ? 1 : -1;
            ++changed;
        }
    }
    return changed;
}

/// total spread over objectives by weight (largest remainder); weights <= 0 get nothing unless every weight is <= 0
inline std::vector<uint32> Spread(std::vector<float> const& weights, uint32 total)
{
    std::vector<uint32> counts(weights.size(), 0);
    if (weights.empty() || !total)
        return counts;

    float sum = 0.0f;
    for (float w : weights)
        sum += std::max(0.0f, w);

    std::vector<std::pair<float, size_t>> remainders;
    uint32 given = 0;
    for (size_t i = 0; i < weights.size(); ++i)
    {
        float exact = sum > 0.0f ? total * std::max(0.0f, weights[i]) / sum : float(total) / weights.size();
        counts[i] = uint32(std::floor(exact));
        given += counts[i];
        remainders.emplace_back(exact - counts[i], i);
    }
    std::stable_sort(remainders.begin(), remainders.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    for (size_t i = 0; given < total; i = (i + 1) % remainders.size())
    {
        ++counts[remainders[i].second];
        ++given;
    }
    return counts;
}

// ---- objective graph maps (Alterac Valley, phase 3) ----

/// Teams as TrinityCore's TeamId numbers them: 0 Alliance, 1 Horde, 2 nobody
constexpr uint8 Alliance = 0;
constexpr uint8 Horde = 1;
constexpr uint8 Nobody = 2;

/// The team of a playerbots_bg_node side value (0 none, 1 Alliance, 2 Horde)
inline uint8 TeamOfSide(uint8 side)
{
    return side == 1 ? Alliance : side == 2 ? Horde : Nobody;
}

struct NodeRead
{
    uint8 Owner = Nobody;       // who holds it; during an assault the team that held it before
    uint8 Assaulting = Nobody;  // the team whose assault is running
    bool Destroyed = false;     // a tower the other side burnt: nothing left to click
};

/// A node from its four world states (each 1 or 0; at most one is set). lastOwner is what the planner read the tick before
/// (the starting owner at first): an assault keeps it, so a graveyard assaulted from neutral stays neutral. A tower belongs to
/// builder; once the other side holds it, it is destroyed
inline NodeRead ReadNode(bool tower, uint8 builder, uint8 lastOwner, bool allianceControl, bool allianceAssault, bool hordeControl, bool hordeAssault)
{
    NodeRead read;
    if (allianceControl)
        read.Owner = Alliance;
    else if (hordeControl)
        read.Owner = Horde;
    else if (allianceAssault || hordeAssault)
    {
        read.Assaulting = allianceAssault ? Alliance : Horde;
        uint8 other = read.Assaulting == Alliance ? Horde : Alliance;
        if (lastOwner != read.Assaulting)
            read.Owner = lastOwner;
        else if (tower)
            read.Owner = builder;
        else
            read.Owner = builder == Nobody ? Nobody : other;
    }
    read.Destroyed = tower && read.Owner != Nobody && builder != Nobody && read.Owner != builder;
    return read;
}

/// "requires" of a node: Need of Keys must be done first (Need 0: all of them). Text "a,b" = both, "2:a,b,c" = any two
struct Requirement
{
    uint32 Need = 0;
    std::vector<std::string> Keys;
};

inline std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

inline Requirement ParseRequires(std::string_view text)
{
    Requirement req;
    text = Trim(text);
    if (size_t colon = text.find(':'); colon != std::string_view::npos)
    {
        uint32 need = 0;
        for (char c : Trim(text.substr(0, colon)))
            if (c >= '0' && c <= '9')
                need = need * 10 + uint32(c - '0');
        req.Need = need;
        text = text.substr(colon + 1);
    }
    while (!text.empty())
    {
        size_t comma = text.find(',');
        std::string_view key = Trim(text.substr(0, comma));
        if (!key.empty())
            req.Keys.emplace_back(key);
        if (comma == std::string_view::npos)
            break;
        text = text.substr(comma + 1);
    }
    return req;
}

/// done: how many of req.Keys the team has done
inline bool RequirementMet(Requirement const& req, uint32 done)
{
    if (req.Keys.empty())
        return true;
    uint32 need = req.Need ? std::min<uint32>(req.Need, uint32(req.Keys.size())) : uint32(req.Keys.size());
    return done >= need;
}

/// How far from a gate (or a rally point) a bot stops: from minYards to maxYards, the same for one bot every time, so a team
/// spreads out in front of the door instead of piling on one spot
inline float StopDistance(uint64 guidLow, float minYards, float maxYards)
{
    if (maxYards <= minYards)
        return minYards;
    return minYards + float(Mix(guidLow ^ 0x5DEECE66Dull) % 1000) / 1000.0f * (maxYards - minYards);
}

// ---- the rally before a boss (docs/design/BG_PLAN.md §2 "Rally") ----

enum class RallyState : uint8
{
    Idle,       // no boss is the team's target
    Gathering,  // walk to the rally point and wait there
    Charging,   // one bot pulls; the rest follow ChargeDelayMs later
    Fighting,   // everybody on the push fights the boss inside its leash
    Regroup,    // a wipe or the boss reset: back to the rally point, nobody runs in alone
    Done,       // the boss is dead
    Abandoned   // too few came twice: the planner picks another objective for a while
};

inline char const* RallyStateName(RallyState state)
{
    switch (state)
    {
        case RallyState::Gathering: return "gathering";
        case RallyState::Charging: return "charging";
        case RallyState::Fighting: return "fighting";
        case RallyState::Regroup: return "regroup";
        case RallyState::Done: return "done";
        case RallyState::Abandoned: return "abandoned";
        default: return "idle";
    }
}

struct RallyParams
{
    uint32 Floor = 15;              // this many present is enough to charge (Bg.Rally.GeneralFloor or CaptainFloor)
    uint32 Pct = 80;                // or this share of the living players on the push, whichever is smaller
    uint32 TimeoutMs = 120000;
    uint32 TimeoutChargePct = 50;   // at the timeout, this share of the target present still charges
    uint32 WipePct = 30;            // fewer living on the push than this share of those who charged: a wipe
    uint32 MaxRestarts = 1;         // gathers started again at a timeout (with the defenders pulled in) before giving up
    bool PullDefenders = true;
    uint32 ChargeDelayMs = 3000;    // the puller goes first, the rest this much later
    uint32 ApproachMs = 60000;      // a charge that has not reached the boss this long after it began failed (rally spots are up to 130 yd out)
    uint32 ResetGraceMs = 15000;    // once the boss was in combat, out of combat this long means it reset
    uint32 RegroupMinMs = 10000;    // a regroup lasts at least this long, and until the boss is out of combat
};

/// What the planner sees of the push on one tick
struct RallyView
{
    uint32 OnPush = 0;              // living players on the push: its bots and the humans who count (decision 2)
    uint32 Present = 0;             // living players of the team within the rally radius of the rally point
    bool BossAlive = true;
    bool BossInCombat = false;
    bool BossEvading = false;
};

struct Rally
{
    RallyState State = RallyState::Idle;
    uint32 SinceMs = 0;             // when State began
    uint32 GatherStartMs = 0;       // the gather timer
    uint32 Need = 0;                // how many present the last gathering tick wanted
    uint32 Charged = 0;             // how many were present at the charge
    uint32 ChargedMs = 0;           // when the push charged
    uint32 Restarts = 0;
    bool DefendersPulled = false;   // a restarted gather: the team's defenders are on the push too
    bool Engaged = false;           // the boss has been in combat since the charge (the walk in is over)
    uint32 BossCombatMs = 0;        // the boss was last seen in combat (Charging, Fighting)
};

/// min(floor, ceil(pct% of the push)), at least one
inline uint32 RallyNeed(uint32 onPush, RallyParams const& params)
{
    uint32 share = (onPush * params.Pct + 99) / 100;
    return std::max<uint32>(1, std::min(params.Floor, share));
}

inline void RallyStart(Rally& rally, uint32 nowMs)
{
    rally = Rally();
    rally.State = RallyState::Gathering;
    rally.SinceMs = nowMs;
    rally.GatherStartMs = nowMs;
}

/// One planner tick of the rally. Idle, Done and Abandoned stay as they are (RallyStart begins a push)
inline RallyState RallyStep(Rally& rally, RallyView const& view, RallyParams const& params, uint32 nowMs)
{
    auto enter = [&](RallyState state)
    {
        rally.State = state;
        rally.SinceMs = nowMs;
    };

    if (rally.State == RallyState::Idle || rally.State == RallyState::Done || rally.State == RallyState::Abandoned)
        return rally.State;
    if (!view.BossAlive)
    {
        enter(RallyState::Done);
        return rally.State;
    }

    switch (rally.State)
    {
        case RallyState::Gathering:
        {
            rally.Need = RallyNeed(view.OnPush, params);
            auto charge = [&]
            {
                rally.Charged = view.Present;
                rally.ChargedMs = nowMs;
                rally.Engaged = false;
                rally.BossCombatMs = nowMs;
                enter(RallyState::Charging);
            };
            if (view.Present && view.Present >= rally.Need)
                charge();
            else if (nowMs - rally.GatherStartMs >= params.TimeoutMs)
            {
                uint32 enough = std::max<uint32>(1, (rally.Need * params.TimeoutChargePct + 99) / 100);
                if (view.Present >= enough)
                    charge();
                else if (rally.Restarts < params.MaxRestarts)
                {
                    ++rally.Restarts;
                    rally.GatherStartMs = nowMs;
                    rally.DefendersPulled = params.PullDefenders;
                }
                else
                    enter(RallyState::Abandoned);
            }
            break;
        }
        case RallyState::Charging:
            if (view.BossInCombat)
            {
                rally.Engaged = true;
                rally.BossCombatMs = nowMs;
            }
            if (nowMs - rally.SinceMs >= params.ChargeDelayMs)
                enter(RallyState::Fighting);
            break;
        case RallyState::Fighting:
        {
            if (view.BossInCombat)
            {
                rally.Engaged = true;
                rally.BossCombatMs = nowMs;
            }
            // the reset clock runs from the last contact; before the first one the walk in has its own, longer limit
            bool reset = rally.Engaged ? nowMs - rally.BossCombatMs >= params.ResetGraceMs : nowMs - rally.ChargedMs >= params.ApproachMs;
            if (view.BossEvading || view.OnPush * 100 < params.WipePct * rally.Charged || reset)
                enter(RallyState::Regroup);
            break;
        }
        case RallyState::Regroup:
            if (!view.BossInCombat && !view.BossEvading && nowMs - rally.SinceMs >= params.RegroupMinMs)
            {
                rally.GatherStartMs = nowMs;
                enter(RallyState::Gathering);
            }
            break;
        default:
            break;
    }
    return rally.State;
}
}

#endif
