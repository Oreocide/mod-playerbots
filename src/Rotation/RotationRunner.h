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

#ifndef PlayerbotsRotationRunner_h__
#define PlayerbotsRotationRunner_h__

#include "Define.h"
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class Unit;

namespace Playerbots
{
class PlayerbotAI;
struct RotationSet;

/// Casts for a bot from the rotation of its specialization: walks a list from the top, takes the first entry whose
/// spell is known, ready, in range and whose condition holds, and casts it. State of one bot, lives in its PlayerbotAI
class RotationRunner
{
public:
    /// Runs a list ("combat", "buffs", "heal", "interrupt", "defensive"...). enemy is the bot's combat target,
    /// null out of combat. true if a spell was cast (or one is being cast): the bot's turn is used
    bool Run(PlayerbotAI& ai, std::string const& list, Unit* enemy, bool inCombat);
    /// Whether the bot's specialization has a rotation with that list
    bool HasList(PlayerbotAI& ai, std::string const& list) const;

    /// The spells of a name (or id) the bot knows
    std::vector<uint32> const& Resolve(Player* bot, std::string const& name);
    /// Whether the bot knows a spell or talent of that name, passive ones included (fact talent.NAME)
    bool Knows(Player* bot, std::string const& name);

    /// Enemies in combat near the bot / near its target / in 30 yards, counted once per AI tick
    uint32 EnemiesAround(PlayerbotAI& ai, Unit const* target, uint8 which);

private:
    Unit* ResolveTarget(PlayerbotAI& ai, std::string const& what, Unit* enemy) const;
    void InvalidateIfNeeded(Player* bot);

    std::unordered_map<std::string, std::vector<uint32>> _spells;   // normalized name -> known spell ids
    std::unordered_map<std::string, bool> _known;                   // normalized name -> known, passives included
    size_t _spellCount = 0;
    uint32 _spec = 0;
    uint32 _level = 0;

    uint32 _countTick = 0;
    uint32 _counts[3] = { 0, 0, 0 };
    bool _counted[3] = { false, false, false };
};

/// "Mortal_Strike" and "mortal strike" are the same name
std::string NormalizeSpellName(std::string_view name);
}

#endif // PlayerbotsRotationRunner_h__
