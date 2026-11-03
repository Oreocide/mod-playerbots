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
}

#endif
