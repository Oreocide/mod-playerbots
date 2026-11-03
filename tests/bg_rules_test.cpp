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

// Standalone test of the battleground planner rules (Battleground/BgRules.h), see tests/run_engine_test.sh

#include "Battleground/BgRules.h"
#include <cstdio>
#include <set>

using namespace Playerbots::BgRules;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

std::vector<Seat> MakeSeats(uint64 seed, uint32 count)
{
    std::vector<Seat> seats;
    for (uint32 i = 0; i < count; ++i)
    {
        Seat seat;
        seat.Id = 1000 + i;
        seat.Key = ShuffleKey(seed, seat.Id);
        seats.push_back(seat);
    }
    return seats;
}

uint32 CountJob(std::vector<Seat> const& seats, Job job)
{
    uint32 n = 0;
    for (Seat const& seat : seats)
        n += seat.JobNow == job;
    return n;
}

void TestRollRange()
{
    std::set<uint32> seen;
    for (uint32 i = 0; i < 2000; ++i)
    {
        uint32 pct = RollDefendPct(MatchSeed(i, i & 1, 1700000000 + i * 7), 20, 40);
        CHECK(pct >= 20 && pct <= 40);
        seen.insert(pct);
    }
    CHECK(seen.size() == 21);                                   // every value of the range comes up
    CHECK(RollDefendPct(12345, 30, 30) == 30);
    uint32 swapped = RollDefendPct(12345, 40, 20);              // min > max: swapped, no modulo by zero
    CHECK(swapped >= 20 && swapped <= 40);
    CHECK(RollDefendPct(12345, 150, 200) == 100);               // clamped to 100
}

void TestSeeds()
{
    // the same match gives the same seed, the two teams of one match different ones
    CHECK(MatchSeed(7, 0, 1700000000) == MatchSeed(7, 0, 1700000000));
    CHECK(MatchSeed(7, 0, 1700000000) != MatchSeed(7, 1, 1700000000));
    CHECK(MatchSeed(7, 0, 1700000000) != MatchSeed(8, 0, 1700000000));
    CHECK(MatchSeed(7, 0, 1700000000) != MatchSeed(7, 0, 1700000001));

    // the two teams get different shuffle orders (their shares may be equal: about 1 in 21 with 20-40)
    uint64 a = MatchSeed(7, 0, 1700000000), h = MatchSeed(7, 1, 1700000000);
    uint32 sameOrder = 0;
    for (uint64 g = 0; g < 50; ++g)
        sameOrder += (ShuffleKey(a, g) < ShuffleKey(a, g + 1)) == (ShuffleKey(h, g) < ShuffleKey(h, g + 1));
    CHECK(sameOrder < 45);
}

void TestShuffleNotGuidOrder()
{
    // guids in a row: the shuffle keys are not in that order
    uint64 seed = MatchSeed(3, 0, 1700000123);
    uint32 ascending = 0;
    for (uint64 g = 100; g < 140; ++g)
        ascending += ShuffleKey(seed, g) < ShuffleKey(seed, g + 1);
    CHECK(ascending > 5 && ascending < 35);

    // the first defenders of a 10-bot team are not the lowest guids every match
    std::set<uint64> firstPicks;
    for (uint32 match = 0; match < 20; ++match)
    {
        std::vector<Seat> seats = MakeSeats(MatchSeed(match, 0, 1700000000 + match), 10);
        Split(seats, 3, 0, 45000);
        for (Seat const& seat : seats)
            if (seat.JobNow == Job::Defend)
                firstPicks.insert(seat.Id);
    }
    CHECK(firstPicks.size() >= 8);
}

void TestInitialSplit()
{
    std::vector<Seat> seats = MakeSeats(99, 10);
    uint32 changed = Split(seats, 3, 1000, 45000);
    CHECK(changed == 10);
    CHECK(CountJob(seats, Job::Defend) == 3);
    CHECK(CountJob(seats, Job::Attack) == 7);
    for (Seat const& seat : seats)
        CHECK(seat.SinceMs == 1000);

    // the defenders are the lowest shuffle keys
    uint64 highestDefender = 0, lowestAttacker = ~0ull;
    for (Seat const& seat : seats)
    {
        if (seat.JobNow == Job::Defend)
            highestDefender = std::max(highestDefender, seat.Key);
        else
            lowestAttacker = std::min(lowestAttacker, seat.Key);
    }
    CHECK(highestDefender < lowestAttacker);
}

void TestHoldTime()
{
    std::vector<Seat> seats = MakeSeats(5, 10);
    Split(seats, 3, 1000, 45000);

    // 10 s later the team wants 5 defenders: nobody may move yet
    CHECK(Split(seats, 5, 11000, 45000) == 0);
    CHECK(CountJob(seats, Job::Defend) == 3);

    // after the hold time exactly the difference moves
    CHECK(Split(seats, 5, 46000, 45000) == 2);
    CHECK(CountJob(seats, Job::Defend) == 5);

    // the two new defenders are held: asking for 3 again moves two of the three old ones back to attack
    uint32 changed = Split(seats, 3, 50000, 45000);
    CHECK(changed == 2);
    CHECK(CountJob(seats, Job::Defend) == 3);
    for (Seat const& seat : seats)
        if (seat.JobNow == Job::Defend)
            CHECK(seat.SinceMs == 46000 || seat.SinceMs == 1000);
}

void TestDeltaOnly()
{
    std::vector<Seat> seats = MakeSeats(11, 20);
    Split(seats, 6, 0, 45000);
    std::vector<Seat> before = seats;

    CHECK(Split(seats, 7, 100000, 45000) == 1);
    uint32 moved = 0;
    for (size_t i = 0; i < seats.size(); ++i)
        moved += seats[i].JobNow != before[i].JobNow;
    CHECK(moved == 1);

    // the same want again: nothing moves
    CHECK(Split(seats, 7, 200000, 45000) == 0);
}

void TestNearestMoves()
{
    std::vector<Seat> seats = MakeSeats(21, 6);
    Split(seats, 0, 0, 1000);
    for (size_t i = 0; i < seats.size(); ++i)
        seats[i].DefendCost = float(100 - i * 10);              // the last seat is the nearest

    CHECK(Split(seats, 1, 5000, 1000) == 1);
    CHECK(seats.back().JobNow == Job::Defend);

    // giving one back: the farthest defender goes first
    seats[0].JobNow = Job::Defend;
    seats[0].SinceMs = 0;
    CHECK(Split(seats, 1, 10000, 1000) == 1);
    CHECK(seats[0].JobNow == Job::Attack);
    CHECK(seats.back().JobNow == Job::Defend);
}

void TestJoinerAndFreed()
{
    std::vector<Seat> seats = MakeSeats(31, 8);
    Split(seats, 2, 0, 45000);

    // a defender whose node fell is freed (None) and a new bot joins: both take a job at once, inside the hold time
    for (Seat& seat : seats)
        if (seat.JobNow == Job::Defend)
        {
            seat.JobNow = Job::None;
            break;
        }
    Seat joiner;
    joiner.Id = 5000;
    joiner.Key = ShuffleKey(31, joiner.Id);
    seats.push_back(joiner);

    CHECK(Split(seats, 2, 1000, 45000) == 2);
    CHECK(CountJob(seats, Job::Defend) == 2);
    CHECK(CountJob(seats, Job::None) == 0);

    // carriers and escorts are left alone
    seats[0].JobNow = Job::Carrier;
    seats[1].JobNow = Job::Escort;
    Split(seats, 9, 999999, 45000);
    CHECK(seats[0].JobNow == Job::Carrier);
    CHECK(seats[1].JobNow == Job::Escort);
}

void TestShare()
{
    CHECK(ShareOf(10, 20) == 2);
    CHECK(ShareOf(10, 25) == 3);                                // 2.5 rounds up
    CHECK(ShareOf(10, 24) == 2);
    CHECK(ShareOf(0, 40) == 0);
    CHECK(ShareOf(40, 40) == 16);
}

void TestSpread()
{
    std::vector<uint32> counts = Spread({ 1.0f, 1.0f, 1.0f }, 7);
    CHECK(counts[0] + counts[1] + counts[2] == 7);
    CHECK(*std::max_element(counts.begin(), counts.end()) - *std::min_element(counts.begin(), counts.end()) <= 1);

    counts = Spread({ 3.0f, 1.0f }, 4);
    CHECK(counts[0] == 3 && counts[1] == 1);

    counts = Spread({ 0.0f, 2.0f }, 3);
    CHECK(counts[0] == 0 && counts[1] == 3);

    counts = Spread({ 0.0f, 0.0f }, 3);                         // all zero: an even spread
    CHECK(counts[0] + counts[1] == 3);

    CHECK(Spread({}, 5).empty());
    counts = Spread({ 1.0f }, 0);
    CHECK(counts[0] == 0);
}
}

int main()
{
    TestRollRange();
    TestSeeds();
    TestShuffleNotGuidOrder();
    TestInitialSplit();
    TestHoldTime();
    TestDeltaOnly();
    TestNearestMoves();
    TestJoinerAndFreed();
    TestShare();
    TestSpread();

    if (Failures)
    {
        std::printf("bg rules test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("bg rules test: all passed\n");
    return 0;
}
