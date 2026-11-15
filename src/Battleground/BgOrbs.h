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


#ifndef Playerbots_BgOrbs_h__
#define Playerbots_BgOrbs_h__

// Temple of Kotmogu (map 998), BG_PLAN.md phase 5.4: four Orbs of Power, no flags, no captures. A player takes an orb with a plain
// use of its pedestal object and scores while carrying it, every 5 s: 6 points in the central pit, 4 in the courtyard, 2 elsewhere;
// 1500 wins. A carrier takes more damage every 15 s, and the orb is back on its pedestal at once when the carrier dies; mounting,
// stealth and invisibility drop it too (worktrees/bg-kotmogu/KOTMOGU_DATA.md). The capture planner plays the orbs as nodes: an orb
// belongs to the team that carries it, and while it is carried its objective stands where the carrier is, so the planner's defenders
// escort our carriers and its attackers hunt theirs. A carrier goes to the pit and stays.

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

class GameObject;
class Map;
class Player;

namespace Playerbots
{
class PlayerbotAI;

namespace Bg
{
struct Assignment;
struct BgPlan;

namespace Orbs
{
constexpr uint32 MapId = 998;

/// Every plan, after the object pass: each orb's owner (the carrying team, from world states 6963-6970) and position (the carrier
/// while carried, else the pedestal); the pedestals are known before their objects are found
void ReadState(BgPlan& plan, Map* map);

/// What a bot does next on map 998; Bg::Play carries it out
struct Step
{
    bool Active = false;        // false: nothing map-specific, play as usual
    Position WalkTo;            // walk here (arrive yards); Arrive 0: stay
    float Arrive = 0.0f;
    float FightRange = 0.0f;    // first fight the nearest enemy within this of FightAt (0: no)
    Position FightAt;
    ObjectGuid Target;          // attack this player (an enemy carrier)
    GameObject* Use = nullptr;  // use this orb now
    bool ToPedestal = false;    // the walk goes to the job's free orb (a path failure there is the planner's to know)
};

Step Decide(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job);

/// The bot carries an orb (aura 116524)
bool Carries(Player const* bot);
/// A carrier outside the pit or far from its own spot there (`spot`): the "bg carrier" leash walks it back, in combat too
bool CarrierAway(Player const* bot, Position& spot);
/// A bot whose job is a free orb, held up by a fight away from its pedestal (`pedestal`): the leash breaks the fight off and walks it
/// there (a fight at the pedestal is fought; the orb is the point)
/// (`use` set: the bot stands at the pedestal and may take the orb now)
/// commit false: only asks (the trigger); true takes the orb's claim (the action)
bool TakerAway(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job, Position& pedestal, GameObject*& use, bool commit);
}
}
}

#endif
