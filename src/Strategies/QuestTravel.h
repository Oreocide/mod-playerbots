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

#ifndef PlayerbotsQuestTravel_h__
#define PlayerbotsQuestTravel_h__

#include "Define.h"
#include "Position.h"
#include <memory>

class Player;

namespace Playerbots
{
class Strategy;

/// Quest travel for random bots. Only quests that consist of nothing but "kill N creatures" objectives are used:
/// the bot walks to the quest giver, accepts, hunts the creatures, walks to the turn-in NPC and takes the reward.
/// The index is built once in the world thread (before any bot logs in) and is read-only afterwards.
namespace QuestTravel
{
void BuildIndex();
bool IsBuilt();
uint32 GetPlanCount();
bool PickPoi(Player* bot, uint32 questId, bool turnIn, Position& out);
bool NearestHunt(Player* bot, Position& out);
/// XP of one quest the bot could be doing. Does not accept or complete it.
uint32 AbstractXp(Player* bot);
}

std::unique_ptr<Strategy> CreateQuestStrategy();
}

#endif // PlayerbotsQuestTravel_h__
