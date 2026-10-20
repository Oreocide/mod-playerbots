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

#ifndef PlayerbotsAbstractLife_h__
#define PlayerbotsAbstractLife_h__

#include "Strategies/Rpg/RpgInfo.h"

namespace Playerbots
{
class PlayerbotAI;

/// What a frozen (far) bot is doing when Playerbots.Abstract.Enable is on: the same AC status weights as a near bot, lived as a
/// timer. Nothing is pathed or loaded. DoQuest and GoGrind pay a trickle of XP when they end, the rest is just time.
namespace AbstractLife
{
struct State
{
    RpgStatus Status = RpgStatus::Idle;   // Idle = nothing chosen yet
    uint32 UntilMs = 0;                   // when the status ends (game time ms)
};

/// Called by the frozen branch when its timer is due. A stored goal tied to a game event, hour or weekday comes first (GoalStore, timed
/// rows only), otherwise the running status continues or the next one is rolled. Returns when to be called again.
uint32 Tick(PlayerbotAI& ai, State& state, uint32 nowMs);

char const* StatusName(RpgStatus status);
}
}

#endif
