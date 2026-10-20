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

// Standalone test of the group tactics rules (Combat/GroupTactics.h), see tests/run_engine_test.sh

#include "Combat/GroupTactics.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    // roles: main tank = first living tank, a leading tank wins; indexes count living members of a role
    std::vector<MemberView> party = {
        { 1, GroupRole::MeleeDps, true, false },
        { 2, GroupRole::Tank, true, false },
        { 3, GroupRole::Tank, true, true },
        { 4, GroupRole::Healer, true, false },
        { 5, GroupRole::MeleeDps, false, false },
        { 6, GroupRole::MeleeDps, true, false },
    };
    std::vector<RoleSlot> slots = AssignRoles(party);
    CHECK(slots.size() == 6);
    CHECK(!slots[1].MainTank && slots[2].MainTank);
    CHECK(slots[0].Index == 0 && slots[5].Index == 1 && slots[4].Index == 0xFF);   // the dead dps does not take an index
    CHECK(slots[1].Index == 0 && slots[2].Index == 1);

    party[2].Alive = false;
    slots = AssignRoles(party);
    CHECK(slots[1].MainTank && !slots[2].MainTank);                                // the leader tank died
    party[1].Alive = false;
    slots = AssignRoles(party);
    for (RoleSlot const& s : slots)
        CHECK(!s.MainTank);                                                         // no living tank
    CHECK(AssignRoles({}).empty());

    // tank target: the untanked enemy, a healer's attacker first, the nearest when all are tanked
    std::vector<EnemyView> fight = {
        { 10, true, false, false, true, 100.0f, 5.0f },      // on the tank
        { 11, false, false, false, false, 100.0f, 20.0f },   // loose, far
        { 12, false, true, false, false, 100.0f, 30.0f },    // on the healer
    };
    CHECK(PickTankTarget(fight) == 2);
    fight[2].TargetIsHealer = false;
    CHECK(PickTankTarget(fight) == 1);
    fight[1].TargetIsTank = true;
    fight[2].TargetIsTank = true;
    CHECK(PickTankTarget(fight) == 0);                                              // all tanked: the nearest
    CHECK(PickTankTarget({}) == -1);

    // dps target: the tank's victim beats everything, then a healer's attacker, then the wounded
    std::vector<EnemyView> dps = {
        { 20, false, false, false, false, 20.0f, 10.0f },    // nearly dead
        { 21, true, false, false, true, 100.0f, 12.0f },     // tank victim
        { 22, false, true, false, false, 100.0f, 10.0f },    // on the healer
    };
    CHECK(PickDpsTarget(dps) == 1);
    dps[1].TankVictim = false;
    CHECK(PickDpsTarget(dps) == 2);                                                 // healer's attacker beats a wounded one
    dps[2].TargetIsHealer = false;
    CHECK(PickDpsTarget(dps) == 0);

    // threat gate
    CHECK(ThreatAllows(50.0f, 100.0f, false));
    CHECK(!ThreatAllows(85.0f, 100.0f, false));
    CHECK(ThreatAllows(500.0f, 100.0f, true));                                      // the tank itself
    CHECK(ThreatAllows(10.0f, 0.0f, false));                                        // nobody has threat yet

    // wait for the pull: only with a human leading, wrap-safe
    CHECK(WaitAllowsAttack(1000, 0, 0, true));
    CHECK(WaitAllowsAttack(1000, 900, 2000, false));
    CHECK(!WaitAllowsAttack(1000, 900, 2000, true));
    CHECK(WaitAllowsAttack(3000, 900, 2000, true));
    CHECK(!WaitAllowsAttack(0x00000100u, 0xFFFFFF00u, 2000, true));
    CHECK(WaitAllowsAttack(0x00000900u, 0xFFFFFF00u, 2000, true));

    if (Failures)
    {
        std::printf("group_tactics_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("group_tactics_test: ok\n");
    return 0;
}
