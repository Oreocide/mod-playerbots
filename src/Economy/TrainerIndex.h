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

#ifndef PlayerbotsTrainerIndex_h__
#define PlayerbotsTrainerIndex_h__

#include "Define.h"
#include <vector>

class Player;

/// What each trainer teaches (world table trainer_spell), read once at startup. The core's Trainer class keeps its spell list
/// private, so a bot that wants to know "is there anything for me at this trainer" asks this index and then lets the
/// core's own CMSG_TRAINER_BUY_SPELL handler do the real checks and the teaching.
namespace Playerbots::TrainerIndex
{
void Build();

/// Spells of this trainer the bot looks able to learn now: not known, level and skill rank reached, required spells known,
/// affordable. The core still decides (Trainer::TeachSpell). At most maxCount, cheapest first.
std::vector<uint32> Learnable(Player const* bot, uint32 trainerId, uint32 maxCount);

/// Any spell of this trainer for the bot (a cheap yes/no for choosing where to go)
bool HasLearnable(Player const* bot, uint32 trainerId);
}

#endif
