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

#ifndef Playerbots_CastPolicy_h__
#define Playerbots_CastPolicy_h__

#include <cstdint>

// Whether a cast that is still preparing should go on (Playerbots.CastSupervisor.Enable). No core types, tested
// standalone (tests/cast_policy_test.cpp). A bot that starts a heal on a target that is full again or a spell on a
// target that died wastes the cast and the time; a player would cancel it.
namespace Playerbots
{
struct CastView
{
    bool HasUnitTarget = false;         // false for self casts and ground targets: nothing to check
    bool TargetAlive = true;
    bool SpellAllowsDeadTarget = false; // resurrections
    bool TargetFriendly = false;
    bool SpellIsDirectHeal = false;     // a heal effect without a periodic heal (a HoT may be cast on a full target)
    bool Channeled = false;
    float TargetHealthPct = 100.0f;
    uint32_t RemainingMs = 0;           // cast time left
};

enum class CastVerdict : uint8_t
{
    Continue,
    Cancel
};

constexpr uint32_t CancelHealMinRemainingMs = 250;   // under this the cast is done anyway

inline CastVerdict JudgeCast(CastView const& view)
{
    if (!view.HasUnitTarget)
        return CastVerdict::Continue;

    if (!view.TargetAlive && !view.SpellAllowsDeadTarget)
        return CastVerdict::Cancel;

    if (view.SpellIsDirectHeal && view.TargetFriendly && !view.Channeled && view.TargetAlive
        && view.TargetHealthPct >= 100.0f && view.RemainingMs > CancelHealMinRemainingMs)
        return CastVerdict::Cancel;

    return CastVerdict::Continue;
}
}

#endif
