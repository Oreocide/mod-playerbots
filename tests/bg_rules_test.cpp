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
#include "Battleground/BgReactRules.h"
#include "Battleground/BgTaskRules.h"
#include <cmath>
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

void TestReadNode()
{
    // a Horde graveyard, held, then assaulted by the Alliance, then taken
    NodeRead read = ReadNode(false, Horde, Horde, false, false, true, false);
    CHECK(read.Owner == Horde && read.Assaulting == Nobody && !read.Destroyed);
    read = ReadNode(false, Horde, Horde, false, true, false, false);
    CHECK(read.Owner == Horde && read.Assaulting == Alliance);
    read = ReadNode(false, Horde, Horde, true, false, false, false);
    CHECK(read.Owner == Alliance && read.Assaulting == Nobody && !read.Destroyed);

    // Snowfall: neutral, assaulted from neutral it stays neutral until it falls
    read = ReadNode(false, Nobody, Nobody, false, false, false, false);
    CHECK(read.Owner == Nobody && read.Assaulting == Nobody);
    read = ReadNode(false, Nobody, Nobody, false, false, false, true);
    CHECK(read.Owner == Nobody && read.Assaulting == Horde);

    // first read in the middle of an assault by the team that built it can only mean the other side took it before
    read = ReadNode(false, Alliance, Alliance, true, false, false, false);
    CHECK(read.Owner == Alliance);
    read = ReadNode(false, Alliance, Horde, false, true, false, false);
    CHECK(read.Owner == Horde && read.Assaulting == Alliance);

    // an Alliance bunker: standing, assaulted by the Horde, burnt (the Horde's control state is the destroyed one)
    read = ReadNode(true, Alliance, Alliance, true, false, false, false);
    CHECK(read.Owner == Alliance && !read.Destroyed);
    read = ReadNode(true, Alliance, Alliance, false, false, false, true);
    CHECK(read.Owner == Alliance && read.Assaulting == Horde && !read.Destroyed);
    read = ReadNode(true, Alliance, Alliance, false, false, true, false);
    CHECK(read.Owner == Horde && read.Destroyed);
    // a tower read under assault on the first plan still belongs to its builder
    read = ReadNode(true, Horde, Horde, false, true, false, false);
    CHECK(read.Owner == Horde && read.Assaulting == Alliance && !read.Destroyed);
}

void TestRequires()
{
    Requirement all = ParseRequires("a, b");
    CHECK(all.Need == 0 && all.Keys.size() == 2 && all.Keys[0] == "a" && all.Keys[1] == "b");
    CHECK(!RequirementMet(all, 1));
    CHECK(RequirementMet(all, 2));

    Requirement two = ParseRequires("2:iceblood_tower,tower_point,fw_east,fw_west");
    CHECK(two.Need == 2 && two.Keys.size() == 4 && two.Keys[3] == "fw_west");
    CHECK(!RequirementMet(two, 1));
    CHECK(RequirementMet(two, 2));

    CHECK(RequirementMet(ParseRequires(""), 0));
    CHECK(ParseRequires("  ").Keys.empty());
    // more needed than listed: all of them
    Requirement over = ParseRequires("5:a,b");
    CHECK(!RequirementMet(over, 1) && RequirementMet(over, 2));
}

void TestStopDistance()
{
    std::set<int> seen;
    for (uint64 guid = 1; guid < 200; ++guid)
    {
        float stop = StopDistance(guid, 6.0f, 13.0f);
        CHECK(stop >= 6.0f && stop <= 13.0f);
        CHECK(stop == StopDistance(guid, 6.0f, 13.0f));
        seen.insert(int(stop));
    }
    CHECK(seen.size() >= 6);                                    // a spread, not one spot
    CHECK(StopDistance(7, 5.0f, 5.0f) == 5.0f);
}

void TestRallyNeed()
{
    RallyParams params;                                         // floor 15, 80 %
    CHECK(RallyNeed(30, params) == 15);                         // enough people: the floor
    CHECK(RallyNeed(10, params) == 8);                          // ceil(8.0)
    CHECK(RallyNeed(11, params) == 9);                          // ceil(8.8)
    CHECK(RallyNeed(0, params) == 1);
    params.Floor = 8;
    CHECK(RallyNeed(30, params) == 8);
}

void TestRallyCharge()
{
    RallyParams params;
    params.Floor = 8;
    Rally rally;
    RallyView view;
    view.OnPush = 12;

    CHECK(RallyStep(rally, view, params, 0) == RallyState::Idle);  // nothing starts on its own
    RallyStart(rally, 1000);
    view.Present = 7;
    CHECK(RallyStep(rally, view, params, 4000) == RallyState::Gathering && rally.Need == 8);
    view.Present = 8;
    CHECK(RallyStep(rally, view, params, 7000) == RallyState::Charging && rally.Charged == 8);
    view.BossInCombat = true;
    CHECK(RallyStep(rally, view, params, 9000) == RallyState::Charging);    // the others wait ChargeDelayMs
    CHECK(RallyStep(rally, view, params, 10000) == RallyState::Fighting);
    CHECK(RallyStep(rally, view, params, 40000) == RallyState::Fighting);
    view.BossAlive = false;
    CHECK(RallyStep(rally, view, params, 43000) == RallyState::Done);
    CHECK(RallyStep(rally, view, params, 46000) == RallyState::Done);
}

void TestRallyTimeout()
{
    RallyParams params;                                         // 120 s, 50 %, one restart
    params.Floor = 10;
    Rally rally;
    RallyView view;
    view.OnPush = 20;
    RallyStart(rally, 0);

    view.Present = 5;                                           // half of 10: charges at the timeout
    CHECK(RallyStep(rally, view, params, 119000) == RallyState::Gathering);
    CHECK(RallyStep(rally, view, params, 120000) == RallyState::Charging);

    RallyStart(rally, 0);
    view.Present = 4;                                           // below half: the gather starts again with the defenders
    CHECK(RallyStep(rally, view, params, 120000) == RallyState::Gathering);
    CHECK(rally.Restarts == 1 && rally.DefendersPulled && rally.GatherStartMs == 120000);
    CHECK(RallyStep(rally, view, params, 200000) == RallyState::Gathering);
    CHECK(RallyStep(rally, view, params, 240000) == RallyState::Abandoned);
    CHECK(RallyStep(rally, view, params, 250000) == RallyState::Abandoned);

    params.PullDefenders = false;
    RallyStart(rally, 0);
    CHECK(RallyStep(rally, view, params, 120000) == RallyState::Gathering && !rally.DefendersPulled);
}

void TestRallyWipe()
{
    RallyParams params;
    params.Floor = 10;
    Rally rally;
    RallyView view;
    view.OnPush = 12;
    view.Present = 10;
    RallyStart(rally, 0);
    CHECK(RallyStep(rally, view, params, 3000) == RallyState::Charging);
    view.BossInCombat = true;
    CHECK(RallyStep(rally, view, params, 6000) == RallyState::Fighting);

    view.OnPush = 3;                                            // 3 of 10 charged: 30 % is not below 30 %
    CHECK(RallyStep(rally, view, params, 9000) == RallyState::Fighting);
    view.OnPush = 2;                                            // below: a wipe
    CHECK(RallyStep(rally, view, params, 12000) == RallyState::Regroup);
    // the regroup waits for the boss to leave combat and at least RegroupMinMs
    CHECK(RallyStep(rally, view, params, 30000) == RallyState::Regroup);
    view.BossInCombat = false;
    CHECK(RallyStep(rally, view, params, 31000) == RallyState::Gathering && rally.GatherStartMs == 31000);

    // a far rally point: the walk in takes longer than the reset grace; the reset clock starts at the first contact
    RallyStart(rally, 0);
    view.OnPush = 12;
    view.Present = 10;
    CHECK(RallyStep(rally, view, params, 1000) == RallyState::Charging);
    CHECK(RallyStep(rally, view, params, 4000) == RallyState::Fighting);
    CHECK(RallyStep(rally, view, params, 31000) == RallyState::Fighting);      // 30 s walking in, the boss never in combat
    CHECK(!rally.Engaged);
    view.BossInCombat = true;
    CHECK(RallyStep(rally, view, params, 34000) == RallyState::Fighting && rally.Engaged);
    view.BossInCombat = false;                                                // the boss leaves combat after contact
    CHECK(RallyStep(rally, view, params, 46000) == RallyState::Fighting);
    CHECK(RallyStep(rally, view, params, 49000) == RallyState::Regroup);       // 15 s after the last contact

    // a charge that never reaches the boss fails after ApproachMs
    RallyStart(rally, 0);
    CHECK(RallyStep(rally, view, params, 1000) == RallyState::Charging);
    CHECK(RallyStep(rally, view, params, 4000) == RallyState::Fighting);
    CHECK(RallyStep(rally, view, params, 60000) == RallyState::Fighting);
    CHECK(RallyStep(rally, view, params, 61000) == RallyState::Regroup);

    RallyStart(rally, 0);
    CHECK(RallyStep(rally, view, params, 1000) == RallyState::Charging);
    CHECK(RallyStep(rally, view, params, 4000) == RallyState::Fighting);
    view.BossEvading = true;
    CHECK(RallyStep(rally, view, params, 5000) == RallyState::Regroup);
    CHECK(RallyStep(rally, view, params, 20000) == RallyState::Regroup);       // still evading
    view.BossEvading = false;
    CHECK(RallyStep(rally, view, params, 21000) == RallyState::Gathering);
}

// ---- phase 4 (BgReactRules.h) ----

void TestEventRoll()
{
    CHECK(EventRoll(1, 2, 3, 50) == EventRoll(1, 2, 3, 50));   // one event, one answer
    CHECK(EventRoll(1, 2, 3, 100));
    CHECK(!EventRoll(1, 2, 3, 0));
    uint32 yes = 0, flips = 0;
    for (uint32 e = 0; e < 2000; ++e)
    {
        bool a = EventRoll(77, 1234, e, 70);
        yes += a;
        flips += a != EventRoll(77, 1234, e + 1, 70);
    }
    CHECK(yes > 1240 && yes < 1560);                            // about 70%
    CHECK(flips > 300);                                         // a new event can answer differently
}

void TestThreat()
{
    CHECK(ThreatNeed(0, 2, 6) == 0);
    CHECK(ThreatNeed(3, 0, 6) == 4);
    CHECK(ThreatNeed(3, 2, 6) == 2);
    CHECK(ThreatNeed(3, 3, 6) == 1);                            // level: one more
    CHECK(ThreatNeed(3, 4, 6) == 0);
    CHECK(ThreatNeed(10, 0, 6) == 6);                           // capped
    CHECK(Hopeless(8, 0, 2, 2, 4));                             // 2 into 8 is not sent
    CHECK(!Hopeless(5, 1, 2, 2, 4));
    CHECK(!Hopeless(3, 0, 0, 2, 4));                            // too few enemies to give a node up
}

void TestUrgency()
{
    CHECK(TimeLeft(1000, 31000, 60000) == 30000);
    CHECK(TimeLeft(1000, 61000, 60000) == 0);
    CHECK(TimeLeft(1000, 90000, 60000) == 0);
    CHECK(RetakeUrgency(240000, 240000, 20000) == Urgency::Normal);
    CHECK(RetakeUrgency(120000, 240000, 20000) == Urgency::Half);
    CHECK(RetakeUrgency(15000, 240000, 20000) == Urgency::Last);
    CHECK(RetakeUrgency(5000, 0, 20000) == Urgency::None);
    CHECK(InTime(70.0f, 7.0f, 15000));                          // 10 s of running for 15 s left
    CHECK(!InTime(140.0f, 7.0f, 15000));
    CHECK(!InTime(10.0f, 0.0f, 15000));
}

void TestOutnumbered()
{
    CHECK(Outnumbered(9, 3, 3, 3));
    CHECK(!Outnumbered(8, 3, 3, 3));
    CHECK(Outnumbered(3, 1, 3, 3));
    CHECK(Outnumbered(3, 0, 3, 3));                             // allies count the bot itself
    CHECK(!Outnumbered(2, 0, 3, 3));                            // too few to run from
}

void TestOutlook()
{
    // Arathi Basin with three bases: 4 points every 2 s
    CHECK(SecondsToWin(1000, 1500, 4, 2000) == 250);
    CHECK(SecondsToWin(1500, 1500, 4, 2000) == 0);
    CHECK(SecondsToWin(100, 1500, 0, 2000) == Never);

    OutlookParams params;
    Outlook o = ProjectRace(100, 400, params);
    CHECK(o.Ahead == 1 && !o.Certain && o.Late);
    o = ProjectRace(80, 200, params);
    CHECK(o.Ahead == 1 && o.Certain);
    o = ProjectRace(80, 150, params);
    CHECK(o.Ahead == 1 && !o.Certain);                          // not twice as long
    o = ProjectRace(300, 310, params);
    CHECK(o.Ahead == 0 && !o.Certain && !o.Late);              // too close to call
    o = ProjectRace(Never, 500, params);
    CHECK(o.Ahead == -1 && !o.Certain);
    o = ProjectRace(Never, 60, params);
    CHECK(o.Ahead == -1 && o.Certain && o.Late);
    o = ProjectRace(Never, Never, params);
    CHECK(o.Ahead == 0);

    // Warsong Gulch: 17 minutes of elapsed time
    uint32 limit = 17 * 60000;
    o = ProjectCaptures(2, 0, limit - 170000, limit, params);
    CHECK(o.Ahead == 1 && o.Certain);
    o = ProjectCaptures(1, 0, limit - 200000, limit, params);
    CHECK(o.Ahead == 1 && !o.Certain && !o.Late);
    o = ProjectCaptures(0, 1, limit - 170000, limit, params);
    CHECK(o.Ahead == -1 && o.Late && !o.Certain);
    o = ProjectCaptures(1, 0, limit - 50000, limit, params);
    CHECK(o.Certain);
    o = ProjectCaptures(1, 1, limit - 50000, limit, params);
    CHECK(o.Ahead == 0 && !o.Certain);

    // Alterac Valley, 700 reinforcements
    o = ProjectReinforcements(500, 200, 700, params);
    CHECK(o.Ahead == 1 && !o.Certain && o.Late);
    o = ProjectReinforcements(400, 60, 700, params);
    CHECK(o.Ahead == 1 && o.Certain);
    o = ProjectReinforcements(400, 380, 700, params);
    CHECK(o.Ahead == 0);
    o = ProjectReinforcements(100, 600, 700, params);
    CHECK(o.Ahead == -1 && o.Late);
}

void TestStance()
{
    Outlook o;
    o.Ahead = 1;
    o.Certain = true;
    CHECK(PickStance(o, true, true) == Stance::Finish);
    CHECK(PickStance(o, true, false) == Stance::Turtle);
    CHECK(PickStance(o, false, false) == Stance::Normal);
    o.Ahead = -1;
    CHECK(PickStance(o, false, true) == Stance::Brawl);
    CHECK(PickStance(o, true, false) == Stance::AllIn);
    o.Certain = false;
    o.Late = false;
    CHECK(PickStance(o, true, true) == Stance::Normal);         // behind but with time
    o.Late = true;
    CHECK(PickStance(o, true, true) == Stance::AllIn);

    CHECK(StanceDefendPct(Stance::Turtle, 30, 20, 10) == 50);
    CHECK(StanceDefendPct(Stance::Turtle, 60, 20, 10) == 70);
    CHECK(StanceDefendPct(Stance::AllIn, 30, 20, 10) == 10);
    CHECK(StanceDefendPct(Stance::Brawl, 5, 20, 10) == 5);
    CHECK(StanceDefendPct(Stance::Normal, 30, 20, 10) == 30);

    CHECK(TurtleAttackers(20, 3, 5, 30, true) == 6);            // Arathi Basin: hold three, harass
    CHECK(TurtleAttackers(20, 4, 5, 30, true) == 0);            // never chase the 5-cap
    CHECK(TurtleAttackers(20, 2, 5, 30, true) == 20);           // no majority: no cap
    CHECK(TurtleAttackers(20, 2, 3, 30, true) == 0);            // Battle for Gilneas: two of three
    CHECK(TurtleAttackers(20, 3, 4, 30, false) == 6);           // Eye of the Storm: its flag still pays
}

void TestFocus()
{
    CHECK(ShouldSwitch(Focus::None, Focus::Healer, 0, 4000));
    CHECK(!ShouldSwitch(Focus::Healer, Focus::Healer, 9000, 4000));
    CHECK(!ShouldSwitch(Focus::Healer, Focus::Peel, 1000, 4000));
    CHECK(ShouldSwitch(Focus::Healer, Focus::Peel, 5000, 4000));
    CHECK(!ShouldSwitch(Focus::Peel, Focus::Healer, 9000, 4000));
    CHECK(!ShouldSwitch(Focus::None, Focus::None, 9000, 4000));

    std::vector<Point2> ring = RingPoints({ 0.0f, 0.0f }, 15.0f, 8, 0.0f);
    CHECK(ring.size() == 8);
    CHECK(std::abs(ring[0].X - 15.0f) < 0.01f && std::abs(ring[0].Y) < 0.01f);
    int pick = PickKiteSpot(ring, { { 20.0f, 0.0f } });
    CHECK(pick == 4);                                           // the far side of the room
    CHECK(PickKiteSpot(ring, {}) == 0);
    CHECK(PickKiteSpot({}, { { 1.0f, 1.0f } }) == -1);
}

void TestParseCall()
{
    std::vector<Alias> ab = { { "st", 0 }, { "stables", 0 }, { "bs", 1 }, { "blacksmith", 1 }, { "farm", 2 }, { "gm", 3 },
        { "gold mine", 3 }, { "mine", 5 }, { "lm", 4 }, { "lumber mill", 4 }, { "drek", 9 } };
    Call c = ParseCall("inc LM 3", ab);
    CHECK(c.Kind == CallKind::Incoming && c.Node == 4 && c.Count == 3);
    c = ParseCall("3 inc lumber mill!!", ab);
    CHECK(c.Kind == CallKind::Incoming && c.Node == 4 && c.Count == 3);
    c = ParseCall("help BS", ab);
    CHECK(c.Kind == CallKind::Help && c.Node == 1);
    c = ParseCall("def stables pls", ab);
    CHECK(c.Kind == CallKind::Defend && c.Node == 0);
    c = ParseCall("everyone to Drek", ab);
    CHECK(c.Kind == CallKind::Everyone && c.Node == 9);
    c = ParseCall("go farm", ab);
    CHECK(c.Kind == CallKind::Attack && c.Node == 2);
    c = ParseCall("lm 2", ab);
    CHECK(c.Kind == CallKind::Incoming && c.Node == 4 && c.Count == 2);
    c = ParseCall("inc gold mine", ab);
    CHECK(c.Kind == CallKind::Incoming && c.Node == 3);         // two words beat "mine"
    c = ParseCall("farm clear", ab);
    CHECK(c.Kind == CallKind::None && c.Node == -1);
    c = ParseCall("hello everyone", ab);
    CHECK(c.Kind == CallKind::None);
    c = ParseCall("inc", ab);
    CHECK(c.Kind == CallKind::None);
    c = ParseCall("lm needs help 5", ab);
    CHECK(c.Kind == CallKind::Help && c.Node == 4 && c.Count == 5);
    c = ParseCall("inc 100 st", ab);
    CHECK(c.Kind == CallKind::Incoming && c.Node == 0 && c.Count == 0);  // not a head count
    c = ParseCall("", ab);
    CHECK(c.Kind == CallKind::None);
}

void TestArchetypes()
{
    std::set<Archetype> healers, top, bottom, valley, plain;
    for (uint64 guid = 1; guid <= 400; ++guid)
    {
        healers.insert(PickArchetype(9, guid, true, false, false, 80));
        top.insert(PickArchetype(9, guid, false, true, false, 100));
        bottom.insert(PickArchetype(9, guid, false, false, false, 0));
        valley.insert(PickArchetype(9, guid, false, false, true, 0));
        plain.insert(PickArchetype(9, guid, false, false, false, 100));
    }
    CHECK(healers.size() == 2 && healers.contains(Archetype::StickyHealer) && healers.contains(Archetype::Objective));
    CHECK(!top.contains(Archetype::Brawler) && !top.contains(Archetype::RoadFighter) && top.contains(Archetype::Ninja));
    CHECK(bottom.size() == 1 && bottom.contains(Archetype::Brawler));
    CHECK(valley.contains(Archetype::RoadFighter) && valley.contains(Archetype::Brawler) && valley.size() == 2);
    CHECK(!plain.contains(Archetype::Ninja));                   // only stealth classes sneak
    CHECK(PickArchetype(9, 17, false, true, true, 70) == PickArchetype(9, 17, false, true, true, 70));

    for (uint64 seed = 1; seed < 200; ++seed)
    {
        uint32 skill = RollSkill(seed, 60, 100);
        CHECK(skill >= 60 && skill <= 100);
    }
    CHECK(SkillAfterStreak(80, 2, 10, 60, 100) == 60);
    CHECK(SkillAfterStreak(80, 3, 10, 60, 100) == 60);          // clamped
    CHECK(SkillAfterStreak(80, -3, 10, 60, 100) == 100);
    CHECK(SkillAfterStreak(80, 0, 10, 60, 100) == 80);
    CHECK(NextStreak(2, true) == 3);
    CHECK(NextStreak(2, false) == -1);
    CHECK(NextStreak(-1, false) == -2);
    CHECK(NextStreak(-2, true) == 1);
    CHECK(NextStreak(0, true) == 1);
}

void TestDeaths()
{
    std::vector<Death> deaths = { { { 500.0f, 500.0f }, 1000 }, { { 0.0f, 0.0f }, 100000 }, { { 20.0f, 10.0f }, 200000 },
        { { 5.0f, 25.0f }, 300000 } };
    CHECK(DiesAtOneSpot(deaths, 400000, 600000, 40.0f, 3));     // the old far death does not count
    CHECK(!DiesAtOneSpot(deaths, 750000, 600000, 40.0f, 3));    // the first of the three is too old by then
    deaths.back().Where = { 60.0f, 0.0f };
    CHECK(!DiesAtOneSpot(deaths, 400000, 600000, 40.0f, 3));    // not one spot
    std::vector<Death> two = { { { 0.0f, 0.0f }, 1000 }, { { 1.0f, 1.0f }, 2000 } };
    CHECK(!DiesAtOneSpot(two, 3000, 600000, 40.0f, 3));
}

void TestSpreadCapped()
{
    // Snowfall under our own timer (weight 9, two guards) and three of our nodes: the rest of the 13 by 3:2:1
    std::vector<uint32> counts = SpreadCapped({ 9.0f, 3.0f, 2.0f, 1.0f }, { 2, Uncapped, Uncapped, Uncapped }, 13);
    CHECK(counts[0] == 2 && counts[0] + counts[1] + counts[2] + counts[3] == 13);
    CHECK(counts[1] >= counts[2] && counts[2] >= counts[3] && counts[1] == 5);
    // no cap reached: the same as Spread
    CHECK(SpreadCapped({ 1.0f, 1.0f }, { Uncapped, Uncapped }, 6) == Spread({ 1.0f, 1.0f }, 6));
    // every target full: the rest stays unplaced
    counts = SpreadCapped({ 9.0f, 5.0f }, { 2, 1 }, 10);
    CHECK(counts[0] == 2 && counts[1] == 1);
    // open targets with no weight share what is left
    counts = SpreadCapped({ 9.0f, 0.0f, 0.0f }, { 1, Uncapped, Uncapped }, 5);
    CHECK(counts[0] == 1 && counts[1] + counts[2] == 4 && counts[1] == 2);
    CHECK(CapTotal({ 2, 1 }) == 3 && CapTotal({ 2, Uncapped }) == Uncapped);
    CHECK(SpreadCapped({}, {}, 5).empty());

    // MoveOn: Snowfall (9) and our nodes (13 in all), 12 defenders: the plain spread puts 5 on Snowfall; with 2 guards the other 3
    // attack and our nodes keep their 7
    CHECK(Spread({ 9.0f, 13.0f }, 12)[0] == 5);
    CHECK(CappedDefenders({ 9.0f, 13.0f }, { 2, Uncapped }, 12) == 9);
    counts = SpreadCapped({ 9.0f, 13.0f }, { 2, Uncapped }, 9);
    CHECK(counts[0] == 2 && counts[1] == 7);
    // only running captures: their guards; nothing capped: all of them; none wanted: none
    CHECK(CappedDefenders({ 5.0f, 5.0f }, { 2, 2 }, 10) == 4);
    CHECK(CappedDefenders({ 1.0f, 1.0f }, { Uncapped, Uncapped }, 7) == 7);
    CHECK(CappedDefenders({ 9.0f }, { 2 }, 0) == 0);
    CHECK(CappedDefenders({}, {}, 3) == 3);
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

void TestLegs()
{
    // corners 100 yd apart along x: a 180 yd leg ends at the farthest corner within it (100), which keeps its height
    std::vector<Point3> route = { { 0, 0, 10 }, { 100, 0, 20 }, { 200, 0, 30 }, { 300, 0, 40 } };
    LegGoal leg = PickLeg(route, 180.0f, 10.0f);
    CHECK(leg.Use && leg.Grounded && Near(leg.At.X, 100.0f) && Near(leg.At.Z, 20.0f));
    // a 250 yd leg reaches the corner at 200
    leg = PickLeg(route, 250.0f, 10.0f);
    CHECK(leg.Use && leg.Grounded && Near(leg.At.X, 200.0f));
    // the route is measured along its corners, not in a straight line: a dog-leg of 2 x 100 yd ends the 150 yd leg at the bend
    std::vector<Point3> bend = { { 0, 0, 0 }, { 100, 0, 0 }, { 100, 100, 0 } };
    leg = PickLeg(bend, 150.0f, 10.0f);
    CHECK(leg.Use && leg.Grounded && Near(leg.At.X, 100.0f) && Near(leg.At.Y, 0.0f));
    // one long open segment: 180 yd along it, its height left to the caller
    std::vector<Point3> open = { { 0, 0, 0 }, { 900, 0, 90 } };
    leg = PickLeg(open, 180.0f, 10.0f);
    CHECK(leg.Use && !leg.Grounded && Near(leg.At.X, 180.0f) && Near(leg.At.Z, 18.0f));
    // a corner next to the walker and the next one far beyond: no leg to the near corner (that would re-plan in place), the leg goes
    // on inside the next segment
    std::vector<Point3> near = { { 0, 0, 0 }, { 2, 0, 0 }, { 802, 0, 0 } };
    leg = PickLeg(near, 180.0f, 10.0f);
    CHECK(leg.Use && !leg.Grounded && Near(leg.At.X, 180.0f));
    // the whole route is shorter than a leg: its end
    std::vector<Point3> shortRoute = { { 0, 0, 0 }, { 40, 0, 0 }, { 80, 30, 0 } };
    leg = PickLeg(shortRoute, 180.0f, 10.0f);
    CHECK(leg.Use && leg.Grounded && Near(leg.At.X, 80.0f) && Near(leg.At.Y, 30.0f));
    // nothing worth a leg: the route ends within minGain, a single point, the key off
    std::vector<Point3> tiny = { { 0, 0, 0 }, { 3, 0, 0 }, { 6, 0, 0 } };
    CHECK(!PickLeg(tiny, 180.0f, 10.0f).Use);
    CHECK(!PickLeg({ { 0, 0, 0 } }, 180.0f, 10.0f).Use);
    CHECK(!PickLeg(route, 0.0f, 10.0f).Use);
    // every leg makes progress: walking leg after leg along a winding route reaches its end
    std::vector<Point3> wind = { { 0, 0, 0 } };
    for (int i = 1; i <= 30; ++i)
        wind.push_back({ float(i * 40), float((i % 2) * 35), 0.0f });
    Point3 at = wind[0];
    int legs = 0;
    while (legs < 40)
    {
        std::vector<Point3> rest = { at };
        for (Point3 const& corner : wind)
            if (corner.X > at.X + 0.5f)
                rest.push_back(corner);
        LegGoal next = PickLeg(rest, 180.0f, 10.0f);
        if (!next.Use)
            break;
        CHECK(next.At.X > at.X);
        at = next.At;
        ++legs;
    }
    CHECK(Near(at.X, wind.back().X) && legs >= 6 && legs < 40);
}

// ---- the coordinator's task rules (Battleground/BgTaskRules.h, docs/design/BG_COORDINATOR.md) ----

TaskView TaskAt(TaskIntent intent, float value, float x, float y, uint8 quotaMax, uint32 enemies = 0, uint32 allies = 0)
{
    TaskView task;
    task.Intent = intent;
    task.Value = value;
    task.X = x;
    task.Y = y;
    task.QuotaMax = quotaMax;
    task.Enemies = enemies;
    task.Allies = allies;
    return task;
}

std::vector<uint32> Counts(std::vector<TaskView> const& tasks, std::vector<FillBot> const& bots)
{
    std::vector<uint32> n(tasks.size(), 0);
    for (FillBot const& bot : bots)
        if (bot.Task >= 0 && bot.Alive)
            ++n[bot.Task];
    return n;
}

std::vector<FillBot> BotsAt(uint64 seed, uint32 first, uint32 count, float x, float y)
{
    std::vector<FillBot> bots;
    for (uint32 i = 0; i < count; ++i)
    {
        FillBot bot;
        bot.Id = first + i;
        bot.Key = ShuffleKey(seed, bot.Id);
        bot.X = x;
        bot.Y = y;
        bots.push_back(bot);
    }
    return bots;
}

void TestTaskOpening()
{
    // §4 worked opening: 28 attackers per team at their gate, early weights, 0.2 per 100 yd
    ScoreParams sp;
    FillParams fp;
    std::vector<TaskView> alliance = { TaskAt(TaskIntent::Take, 8.0f, -571.9f, -262.8f, 12), TaskAt(TaskIntent::Take, 8.0f, -768.1f, -362.7f, 12),
        TaskAt(TaskIntent::Take, 3.0f, -202.6f, -112.8f, 4), TaskAt(TaskIntent::Take, 1.8f, -612.7f, -396.7f, 8) };
    CHECK(std::fabs(TaskScore(alliance[0], 794.6f, -493.5f, 0.0f, sp) - 5.2f) < 0.05f);
    CHECK(std::fabs(TaskScore(alliance[1], 794.6f, -493.5f, 0.0f, sp) - 4.9f) < 0.05f);
    CHECK(std::fabs(TaskScore(alliance[2], 794.6f, -493.5f, 0.0f, sp) - 0.9f) < 0.05f);
    CHECK(std::fabs(TaskScore(alliance[3], 794.6f, -493.5f, 0.0f, sp) + 1.0f) < 0.05f);
    for (uint64 seed = 1; seed <= 20; ++seed)
    {
        std::vector<TaskView> tasks = alliance;
        std::vector<FillBot> bots = BotsAt(seed, 100, 28, 794.6f, -493.5f);
        FillResult result = FillTasks(tasks, bots, 100000, sp, fp);
        std::vector<uint32> n = Counts(tasks, bots);
        CHECK(n[0] == 12 && n[1] == 12 && n[2] == 4 && n[3] == 0);
        CHECK(result.Fallbacks == 0 && result.Closed == 0 && result.Changed == 28);
    }
    std::vector<TaskView> horde = { TaskAt(TaskIntent::Take, 8.0f, -152.4f, -441.6f, 12), TaskAt(TaskIntent::Take, 8.0f, 203.2f, -360.3f, 12),
        TaskAt(TaskIntent::Take, 3.0f, -202.6f, -112.8f, 4), TaskAt(TaskIntent::Take, 1.8f, 73.3f, -425.8f, 8) };
    std::vector<FillBot> bots = BotsAt(9, 200, 28, -1382.1f, -545.9f);
    FillTasks(horde, bots, 100000, sp, fp);
    std::vector<uint32> n = Counts(horde, bots);
    CHECK(n[0] == 12 && n[1] == 12 && n[2] == 4 && n[3] == 0);
}

void TestTaskMidSplit()
{
    // §4 mid-phase split: the quotas add up to 54 for 28 attackers; no Take squad below 4 is left, the captain squad fills first
    ScoreParams sp;
    FillParams fp;
    for (uint64 seed = 1; seed <= 30; ++seed)
    {
        std::vector<TaskView> tasks = { TaskAt(TaskIntent::Squad, 8.0f, -545.2f, -165.4f, 12, 1), TaskAt(TaskIntent::Take, 6.0f, -612.7f, -396.7f, 8, 4),
            TaskAt(TaskIntent::Take, 6.0f, -1082.5f, -346.6f, 8, 2), TaskAt(TaskIntent::Take, 5.6f, -1302.9f, -317.0f, 10, 1),
            TaskAt(TaskIntent::Take, 5.6f, -1297.7f, -266.7f, 10, 1), TaskAt(TaskIntent::Take, 1.2f, -1402.2f, -307.5f, 6) };
        std::vector<FillBot> bots = BotsAt(seed, 300, 10, -571.9f, -262.8f);
        for (auto const& group : { BotsAt(seed, 310, 10, -768.1f, -362.7f), BotsAt(seed, 320, 4, -202.6f, -112.8f), BotsAt(seed, 324, 4, 73.3f, -425.8f) })
            bots.insert(bots.end(), group.begin(), group.end());
        FillResult result = FillTasks(tasks, bots, 100000, sp, fp);
        std::vector<uint32> n = Counts(tasks, bots);
        uint32 sum = 0;
        for (size_t t = 0; t < tasks.size(); ++t)
        {
            sum += n[t];
            CHECK(!tasks[t].QuotaMax || n[t] <= tasks[t].QuotaMax);
            if (tasks[t].Intent == TaskIntent::Take)
                CHECK(n[t] == 0 || n[t] >= fp.MinTake);
        }
        CHECK(sum == 28 && n[0] == 12 && n[5] == 0 && result.Fallbacks == 0);
    }
}

void TestTaskFalloffNegative()
{
    // past the quota the value term falls, so a full task far away is worth less than an open one: never more (negative scores)
    ScoreParams sp;
    TaskView full = TaskAt(TaskIntent::Take, 2.0f, 2000.0f, 0.0f, 1);
    TaskView open = TaskAt(TaskIntent::Take, 1.0f, 2000.0f, 0.0f, 10);
    CHECK(std::fabs(TaskScore(full, 0.0f, 0.0f, 0.0f, sp) + 2.0f) < 0.01f);
    CHECK(std::fabs(TaskScore(full, 0.0f, 0.0f, 1.0f, sp) + 3.8f) < 0.01f);
    CHECK(TaskScore(open, 0.0f, 0.0f, 0.0f, sp) > TaskScore(full, 0.0f, 0.0f, 1.0f, sp));
    CHECK(Threat(7, 1, sp) == 3.0f && Threat(2, 4, sp) == 0.0f && Threat(3, 2, sp) == 0.5f);
}

void TestTaskNotOffered()
{
    ScoreParams sp;
    FillParams fp;
    std::vector<TaskView> tasks = { TaskAt(TaskIntent::Take, 0.0f, 0.0f, 0.0f, 0), TaskAt(TaskIntent::Take, 1.0f, 100.0f, 0.0f, 0) };
    CHECK(!Offered(tasks[0]) && Offered(tasks[1]));
    std::vector<FillBot> bots = BotsAt(3, 400, 4, 0.0f, 0.0f);
    FillTasks(tasks, bots, 100000, sp, fp);
    for (FillBot const& bot : bots)
        CHECK(bot.Task == 1);
}

void TestTaskFallback()
{
    // nothing scores above 0: free bots join the attack task with the most living bots and are counted
    ScoreParams sp;
    FillParams fp;
    std::vector<TaskView> tasks = { TaskAt(TaskIntent::Take, 0.5f, 1000.0f, 0.0f, 0), TaskAt(TaskIntent::Take, 0.5f, -1000.0f, 0.0f, 0) };
    std::vector<FillBot> bots = BotsAt(4, 500, 5, 0.0f, 0.0f);
    for (uint32 i = 0; i < 3; ++i)
    {
        bots[i].Task = 1;
        bots[i].SinceMs = 99000;    // committed
    }
    FillResult result = FillTasks(tasks, bots, 100000, sp, fp);
    CHECK(result.Fallbacks == 2 && bots[3].Task == 1 && bots[4].Task == 1 && bots[3].Fallback && !bots[0].Fallback);
    CHECK(MainPush(tasks, bots) == 1);
}

void TestTaskFallbackStable()
{
    // 32 attackers for 28 slots (the 2026-11-10 Alliance): the 4 extra fall back once and stay; the next plan moves nobody and counts
    // no new fallback (they were released and counted again every plan before)
    ScoreParams sp;
    FillParams fp;
    std::vector<TaskView> tasks = { TaskAt(TaskIntent::Take, 8.0f, -571.9f, -262.8f, 12), TaskAt(TaskIntent::Take, 8.0f, -768.1f, -362.7f, 12),
        TaskAt(TaskIntent::Take, 3.0f, -202.6f, -112.8f, 4), TaskAt(TaskIntent::Take, 1.8f, -612.7f, -396.7f, 8) };
    std::vector<FillBot> bots = BotsAt(11, 1200, 32, 794.6f, -493.5f);
    FillResult first = FillTasks(tasks, bots, 100000, sp, fp);
    uint32 fallen = 0;
    for (FillBot const& bot : bots)
        fallen += bot.Fallback ? 1 : 0;
    CHECK(first.Fallbacks == 4 && fallen == 4 && first.Changed == 32);
    for (uint32 tick = 1; tick <= 40; ++tick)
    {
        FillResult next = FillTasks(tasks, bots, 100000 + tick * 1000, sp, fp);
        CHECK(next.Fallbacks == 0 && next.Changed == 0);
    }
    std::vector<uint32> n = Counts(tasks, bots);
    CHECK(n[2] == 4 && n[0] + n[1] == 28);
}

void TestCommitMargin()
{
    FillParams fp;
    CHECK(!CommitMarginMet(0.5f, -0.1f, fp));   // 0.6 < the floor 1.0
    CHECK(CommitMarginMet(5.5f, 4.0f, fp));     // 1.5 >= max(1.0, 1.0)
    CHECK(!CommitMarginMet(12.0f, 10.0f, fp));  // 2.0 < 25% of 10
    CHECK(CommitMarginMet(13.0f, 10.0f, fp));
    // past its commitment a bot on a 4.0 task stays when another scores 4.8
    ScoreParams sp;
    std::vector<TaskView> tasks = { TaskAt(TaskIntent::Take, 4.0f, 0.0f, 0.0f, 0), TaskAt(TaskIntent::Take, 4.8f, 0.0f, 0.0f, 0) };
    std::vector<FillBot> bots = BotsAt(5, 600, 4, 0.0f, 0.0f);
    for (FillBot& bot : bots)
        bot.Task = 0;
    FillTasks(tasks, bots, 100000, sp, fp);
    for (FillBot const& bot : bots)
        CHECK(bot.Task == 0);
}

void TestTaskDeadWeight()
{
    // six dead on a task of six count three: three replacements; when the six are back, the replacements leave, not the rejoining
    ScoreParams sp;
    FillParams fp;
    std::vector<TaskView> tasks = { TaskAt(TaskIntent::Take, 5.0f, 0.0f, 0.0f, 6), TaskAt(TaskIntent::Take, 3.0f, 10.0f, 0.0f, 10) };
    std::vector<FillBot> bots = BotsAt(6, 700, 6, 0.0f, 0.0f);
    for (FillBot& bot : bots)
    {
        bot.Task = 0;
        bot.Alive = false;
    }
    for (FillBot const& bot : BotsAt(6, 800, 7, 0.0f, 0.0f))
        bots.push_back(bot);
    FillTasks(tasks, bots, 100000, sp, fp);
    std::vector<uint32> n = Counts(tasks, bots);
    CHECK(n[0] == 3 && n[1] == 4);
    for (uint32 i = 0; i < 6; ++i)
    {
        bots[i].Alive = true;
        bots[i].RejoinUntilMs = 120000;
    }
    FillTasks(tasks, bots, 105000, sp, fp);
    n = Counts(tasks, bots);
    CHECK(n[0] == 6 && n[1] == 7);
    for (uint32 i = 0; i < 6; ++i)
        CHECK(bots[i].Task == 0);
}

void TestTaskRejoin()
{
    ScoreParams sp;
    FillParams fp;
    std::vector<TaskView> tasks = { TaskAt(TaskIntent::Take, 1.0f, 0.0f, 0.0f, 0), TaskAt(TaskIntent::Take, 8.0f, 0.0f, 0.0f, 0) };
    std::vector<FillBot> bots = BotsAt(7, 900, 4, 0.0f, 0.0f);
    for (FillBot& bot : bots)
    {
        bot.Task = 0;
        bot.RejoinUntilMs = 110000;
    }
    FillTasks(tasks, bots, 100000, sp, fp);
    for (FillBot const& bot : bots)
        CHECK(bot.Task == 0);
    FillTasks(tasks, bots, 110000, sp, fp);    // the rejoin time is over and so is the commitment: the margin moves them
    for (FillBot const& bot : bots)
        CHECK(bot.Task == 1);
}

void TestTaskMinSquad()
{
    // a captain squad of 5 is dissolved under the minimum of 8; one of 9 stays
    ScoreParams sp;
    FillParams fp;
    std::vector<TaskView> tasks = { TaskAt(TaskIntent::Squad, 8.0f, 0.0f, 0.0f, 12), TaskAt(TaskIntent::Take, 5.0f, 200.0f, 0.0f, 12) };
    std::vector<FillBot> bots = BotsAt(8, 1000, 5, 0.0f, 0.0f);
    FillResult result = FillTasks(tasks, bots, 100000, sp, fp);
    std::vector<uint32> n = Counts(tasks, bots);
    CHECK(n[0] == 0 && n[1] == 5 && result.Closed == 1 && tasks[0].Closed);
    tasks = { TaskAt(TaskIntent::Squad, 8.0f, 0.0f, 0.0f, 12), TaskAt(TaskIntent::Take, 5.0f, 200.0f, 0.0f, 12) };
    bots = BotsAt(8, 1100, 9, 0.0f, 0.0f);
    FillTasks(tasks, bots, 100000, sp, fp);
    n = Counts(tasks, bots);
    CHECK(n[0] == 9 && n[1] == 0);
}

void TestStage()
{
    CHECK(StepStage(SquadStage::Staging, 7, 8, 0, 60000, 59000) == SquadStage::Staging);
    CHECK(StepStage(SquadStage::Staging, 8, 8, 0, 60000, 1000) == SquadStage::Moving);
    CHECK(StepStage(SquadStage::Staging, 2, 8, 0, 60000, 60000) == SquadStage::Moving);
    CHECK(StepStage(SquadStage::Staging, 0, 0, 0, 60000, 1) == SquadStage::Moving);
    CHECK(StepStage(SquadStage::Moving, 0, 8, 0, 60000, 1) == SquadStage::Moving);
}

void TestPushGate()
{
    PushGateParams params;
    CHECK(PushCondition(false, 10.0f, 8.0f, params));     // 10 >= 1.2 x 8
    CHECK(!PushCondition(false, 9.0f, 8.0f, params));
    CHECK(PushCondition(true, 0.0f, 8.0f, params));
    PushGate gate;
    CHECK(!StepPushGate(gate, true, false, 1000, params));
    CHECK(!StepPushGate(gate, true, false, 10999, params));
    CHECK(StepPushGate(gate, true, false, 11000, params));
    CHECK(StepPushGate(gate, false, false, 20000, params));
    CHECK(StepPushGate(gate, false, false, 49999, params));
    CHECK(!StepPushGate(gate, false, false, 50000, params));
    CHECK(!StepPushGate(gate, false, false, 60000, params));
    // the condition flickering resets the hold
    CHECK(!StepPushGate(gate, true, false, 61000, params));
    CHECK(!StepPushGate(gate, false, false, 65000, params));
    CHECK(!StepPushGate(gate, true, false, 66000, params));
    CHECK(!StepPushGate(gate, true, false, 75999, params));
    CHECK(StepPushGate(gate, true, false, 76000, params));
    // charging, fighting or regrouping: never off from scores
    CHECK(StepPushGate(gate, false, true, 200000, params) && StepPushGate(gate, false, true, 400000, params));
}

void TestTetherRules()
{
    TetherZone zone;
    TetherCheck check;
    check.Role = TetherRole::Fighter;
    check.BotX = 500.0f;
    check.TargetX = 30.0f;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Keep);         // within 40 yd of the anchor
    check.TargetX = 250.0f;
    check.TargetY = 20.0f;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Keep);         // 20 yd off the straight way to the anchor
    check.TargetY = 60.0f;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Drop);
    check.HitUsRecently = true;
    check.BudgetLeftMs = 5000;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Retaliate);
    check.BudgetLeftMs = 0;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Drop);
    check.BudgetLeftMs = 5000;
    check.Role = TetherRole::Carrier;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Drop);
    check.Role = TetherRole::Capper;
    check.HitUsRecently = false;
    check.TargetX = 25.0f;
    check.TargetY = 0.0f;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Drop);         // a capper fights only within 20 yd of the banner
    check.TargetX = 15.0f;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Keep);
    // the leg: a bent road
    check.Role = TetherRole::Fighter;
    check.HasLeg = true;
    check.LegAX = 500.0f;
    check.LegAY = 0.0f;
    check.LegBX = 400.0f;
    check.LegBY = 100.0f;
    check.TargetX = 440.0f;
    check.TargetY = 75.0f;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Keep);
    check.TargetX = 250.0f;
    check.TargetY = 10.0f;
    CHECK(JudgeTether(check, zone) == TetherVerdict::Drop);         // near the straight line, far from the road
    CHECK(TetherIgnoreMs(0) == 8000 && TetherIgnoreMs(1) == 16000 && TetherIgnoreMs(2) == 32000 && TetherIgnoreMs(3) == 60000
        && TetherIgnoreMs(12) == 60000);
    RetaliateBudget budget;
    StepRetaliateBudget(budget, true, false, 4000, 1000);
    CHECK(budget.LeftMs == 6000);
    StepRetaliateBudget(budget, true, false, 7000, 8000);
    CHECK(budget.LeftMs == 0);
    StepRetaliateBudget(budget, false, true, 0, 10000);
    StepRetaliateBudget(budget, false, true, 0, 29999);
    CHECK(budget.LeftMs == 0);
    StepRetaliateBudget(budget, false, true, 0, 30000);
    CHECK(budget.LeftMs == 10000);
}

void TestTravelIgnoresAssist()
{
    // attacking tasks on the way: no group fights; at the objective, and for defenders or other tasks, the old rules
    CHECK(TravelIgnoresAssist(TaskIntent::Take, 590.0f, 60.0f));
    CHECK(TravelIgnoresAssist(TaskIntent::Squad, 120.0f, 60.0f));
    CHECK(TravelIgnoresAssist(TaskIntent::Push, 61.0f, 60.0f));
    CHECK(!TravelIgnoresAssist(TaskIntent::Take, 60.0f, 60.0f));
    CHECK(!TravelIgnoresAssist(TaskIntent::Take, 12.0f, 60.0f));
    CHECK(!TravelIgnoresAssist(TaskIntent::Hold, 590.0f, 60.0f));
    CHECK(!TravelIgnoresAssist(TaskIntent::Retake, 590.0f, 60.0f));
    CHECK(!TravelIgnoresAssist(TaskIntent::Escort, 590.0f, 60.0f));
}

void TestLanePhase()
{
    LanePhaseParams params;
    LanePhaseView view;
    CHECK(LanePhase(TaskPhase::Early, view, params) == TaskPhase::Early);
    view.PushTowersTaken = 1;
    CHECK(LanePhase(TaskPhase::Early, view, params) == TaskPhase::Early);
    view.PushTowersTaken = 2;
    CHECK(LanePhase(TaskPhase::Early, view, params) == TaskPhase::Mid);
    view = LanePhaseView();
    view.PlayMs = 6 * 60000;
    CHECK(LanePhase(TaskPhase::Early, view, params) == TaskPhase::Mid);
    view.EnemyTowersBurnt = 2;
    CHECK(LanePhase(TaskPhase::Mid, view, params) == TaskPhase::Late);
    view = LanePhaseView();
    view.EnemyStart = 700;
    view.EnemyScore = 281;
    CHECK(LanePhase(TaskPhase::Early, view, params) == TaskPhase::Early);
    view.EnemyScore = 280;
    CHECK(LanePhase(TaskPhase::Early, view, params) == TaskPhase::Late);
    CHECK(LanePhase(TaskPhase::Late, LanePhaseView(), params) == TaskPhase::Late);
    CHECK(LanePhase(TaskPhase::Mid, LanePhaseView(), params) == TaskPhase::Mid);
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
    TestReadNode();
    TestRequires();
    TestStopDistance();
    TestRallyNeed();
    TestRallyCharge();
    TestRallyTimeout();
    TestRallyWipe();
    TestEventRoll();
    TestThreat();
    TestUrgency();
    TestOutnumbered();
    TestOutlook();
    TestStance();
    TestFocus();
    TestParseCall();
    TestArchetypes();
    TestDeaths();
    TestLegs();
    TestSpreadCapped();
    TestTaskOpening();
    TestTaskMidSplit();
    TestTaskFalloffNegative();
    TestTaskNotOffered();
    TestTaskFallback();
    TestTaskFallbackStable();
    TestCommitMargin();
    TestTaskDeadWeight();
    TestTaskRejoin();
    TestTaskMinSquad();
    TestStage();
    TestPushGate();
    TestTetherRules();
    TestLanePhase();
    TestTravelIgnoresAssist();

    if (Failures)
    {
        std::printf("bg rules test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("bg rules test: all passed\n");
    return 0;
}
