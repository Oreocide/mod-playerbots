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

#ifndef Playerbots_RotationScript_h__
#define Playerbots_RotationScript_h__

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Rotations as text, one set per specialization, in the spirit of a SimulationCraft action list: lists of spells by
// name, each with a target and a condition. No core types here, tested standalone.
//
//   spec 71 Arms Warrior
//   combat:
//     Execute        | if target.hp < 20
//     Mortal Strike  | if debuff.Deep_Wounds.remains < 3
//     Slam
//   buffs:
//     Battle Shout   | target=self if !buff.Battle_Shout
//
// A line is "Spell Name" then optionally "|" and options: target=NAME, offgcd, ooc, and "if CONDITION" (the rest of the line).
// Conditions: facts (target.hp, self.power, enemies, buff.NAME.remains, cooldown.NAME.ready...), numbers,
// < <= > >= == !=, ! && || and parentheses. A fact the engine does not know is false.
namespace Playerbots
{
/// Condition syntax tree
struct Expr
{
    enum class Kind : uint8_t { Number, Fact, Not, And, Or, Compare };
    enum class Op : uint8_t { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual };

    Kind Type = Kind::Number;
    double Value = 0.0;                          // Number
    std::string Name;                            // Fact: the dotted name
    Op Comparison = Op::Equal;                   // Compare
    std::unique_ptr<Expr> Left;                  // Not (Left), And/Or/Compare (Left, Right)
    std::unique_ptr<Expr> Right;
};

/// What a condition can ask about. The engine implements it for one bot and one target
class FactSource
{
public:
    virtual ~FactSource() = default;
    /// A number such as target.hp; false if the name is not a number fact
    virtual bool Number(std::string const& name, double& value) const = 0;
    /// A yes/no fact such as in_combat; false if the name is not a flag fact
    virtual bool Flag(std::string const& name, bool& value) const = 0;
};

/// true/false of a condition. A fact nobody knows makes a comparison false and a flag false
bool Evaluate(Expr const& expr, FactSource const& facts);
/// Names of all facts used in a condition
void CollectFacts(Expr const& expr, std::vector<std::string>& names);

struct RotationEntry
{
    std::string Spell;                           // name as written (underscores allowed), or a spell id
    std::string Target = "enemy";                // enemy, self, lowest_ally, tank, pet...
    bool OffGcd = false;                         // does not end the turn: the next entry is tried too
    bool OutOfCombat = false;                    // also used out of combat in this list
    std::unique_ptr<Expr> Condition;             // null = always
    std::string ConditionText;
    uint32_t Line = 0;
};

struct RotationList
{
    std::string Name;
    std::vector<RotationEntry> Entries;
};

struct RotationSet
{
    uint32_t Spec = 0;                           // ChrSpecialization id
    std::string Name;
    std::map<std::string, RotationList> Lists;
};

struct ParseResult
{
    std::vector<std::unique_ptr<RotationSet>> Sets;
    std::vector<std::string> Errors;             // "file:line: what"
};

ParseResult ParseRotations(std::string_view text, std::string const& fileName);

/// A single condition, for tests and tools. nullptr and a message in error if it is malformed
std::unique_ptr<Expr> ParseCondition(std::string_view text, std::string& error);
}

#endif // Playerbots_RotationScript_h__
