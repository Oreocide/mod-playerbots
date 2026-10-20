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

#ifndef Playerbots_GroupTactics_h__
#define Playerbots_GroupTactics_h__

#include <algorithm>
#include <cstdint>
#include <vector>

// Who tanks, who a bot should hit, whether it may hit yet: the decisions of AzerothCore's group roles, tank/dps target and
// threat/wait-for-attack multipliers, on plain data. No core types, tested standalone (tests/group_tactics_test.cpp).
// The glue that builds these views from Group/Unit/ThreatManager is Combat/GroupCombat.cpp.
namespace Playerbots
{
enum class GroupRole : uint8_t
{
    Tank,
    Healer,
    MeleeDps,
    RangedDps
};

struct MemberView
{
    uint64_t Id = 0;
    GroupRole Role = GroupRole::MeleeDps;
    bool Alive = true;
    bool Leader = false;
};

struct RoleSlot
{
    uint64_t Id = 0;
    GroupRole Role = GroupRole::MeleeDps;
    uint8_t Index = 0xFF;       // position among the living members of the same role, in member order; 0xFF = dead
    bool MainTank = false;
};

/// The main tank is the first living tank, the leader first. Every living member gets its index within its role
/// (formation slots and assist order use it)
inline std::vector<RoleSlot> AssignRoles(std::vector<MemberView> const& members)
{
    std::vector<RoleSlot> slots;
    slots.reserve(members.size());
    uint8_t next[4] = { 0, 0, 0, 0 };
    int mainTank = -1;

    for (size_t i = 0; i < members.size(); ++i)
    {
        MemberView const& member = members[i];
        RoleSlot slot;
        slot.Id = member.Id;
        slot.Role = member.Role;
        if (member.Alive)
            slot.Index = next[uint8_t(member.Role)]++;
        slots.push_back(slot);

        if (member.Alive && member.Role == GroupRole::Tank && (mainTank < 0 || (member.Leader && !members[mainTank].Leader)))
            mainTank = int(i);
    }

    if (mainTank >= 0)
        slots[mainTank].MainTank = true;
    return slots;
}

/// One enemy in the fight, as the picking rules see it
struct EnemyView
{
    uint64_t Id = 0;
    bool TargetIsTank = false;      // it is hitting a tank (or the bot, when the bot tanks)
    bool TargetIsHealer = false;
    bool TargetsMe = false;
    bool TankVictim = false;        // the main tank is fighting it
    float HpPct = 100.0f;
    float Dist = 0.0f;
};

/// -1 if there is no enemy
inline int PickIndex(std::vector<EnemyView> const& enemies, float (*score)(EnemyView const&))
{
    int best = -1;
    float bestScore = 0.0f;
    for (size_t i = 0; i < enemies.size(); ++i)
    {
        float s = score(enemies[i]);
        if (best < 0 || s > bestScore)
        {
            best = int(i);
            bestScore = s;
        }
    }
    return best;
}

/// A tank takes what nobody tanks, most urgently what hits a healer; with everything tanked it keeps the nearest
inline int PickTankTarget(std::vector<EnemyView> const& enemies)
{
    return PickIndex(enemies, [](EnemyView const& e)
    {
        return (e.TargetIsTank ? 0.0f : 100.0f) + (e.TargetIsHealer ? 50.0f : 0.0f) - e.Dist * 0.5f;
    });
}

/// Damage dealers follow the main tank's target, then what hits a healer, the most wounded, the nearest
inline int PickDpsTarget(std::vector<EnemyView> const& enemies)
{
    return PickIndex(enemies, [](EnemyView const& e)
    {
        return (e.TankVictim ? 1000.0f : 0.0f) + (e.TargetIsHealer ? 40.0f : 0.0f) + (100.0f - e.HpPct) * 0.3f - e.Dist * 0.5f;
    });
}

/// May a bot that is not tanking keep attacking? false once its threat on the enemy nears the tank's
inline bool ThreatAllows(float myThreat, float tankThreat, bool iAmTank, float limitRatio = 0.8f)
{
    if (iAmTank || tankThreat <= 0.0f)
        return true;
    return myThreat / tankThreat < limitRatio;
}

/// Wait for the leader's pull: no attack until waitMs after the fight started (nowMs - startMs wraps safely)
inline bool WaitAllowsAttack(uint32_t nowMs, uint32_t combatStartMs, uint32_t waitMs, bool humanLeads)
{
    if (!waitMs || !humanLeads)
        return true;
    return uint32_t(nowMs - combatStartMs) >= waitMs;
}
}

#endif
