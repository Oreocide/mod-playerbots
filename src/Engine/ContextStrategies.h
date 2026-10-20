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

#ifndef Playerbots_ContextStrategies_h__
#define Playerbots_ContextStrategies_h__

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Declarative "where the bot is -> which strategies it runs" (AzerothCore's ApplyInstanceStrategies and AiFactory
// without the hard-coded switch). Rules are registered once at startup (dungeon and raid strategies register theirs);
// PlayerbotAI applies only the difference when the situation changes, so a strategy a player toggled by chat inside
// a context stays as the player left it until the bot leaves that context. No core types, tested standalone
// (tests/context_strategies_test.cpp).
namespace Playerbots
{
enum class PlaceKind : uint8_t
{
    OpenWorld,
    Dungeon,
    Raid,
    Battleground,
    Arena
};

struct BotContext
{
    uint32_t MapId = 0;
    PlaceKind Place = PlaceKind::OpenWorld;
    uint32_t DifficultyId = 0;
    bool InGroup = false;
    bool HasMaster = false;

    bool operator==(BotContext const&) const = default;
};

struct ContextRule
{
    std::function<bool(BotContext const&)> When;
    std::vector<std::string> Strategies;    // added while the rule matches
};

class ContextStrategyTable
{
public:
    void Register(ContextRule rule) { _rules.push_back(std::move(rule)); }
    size_t Size() const { return _rules.size(); }

    /// Every strategy any matching rule wants, each once, in rule order
    std::vector<std::string> Resolve(BotContext const& context) const
    {
        std::vector<std::string> wanted;
        for (ContextRule const& rule : _rules)
        {
            if (!rule.When || !rule.When(context))
                continue;
            for (std::string const& name : rule.Strategies)
                if (std::find(wanted.begin(), wanted.end(), name) == wanted.end())
                    wanted.push_back(name);
        }
        return wanted;
    }

    struct Change
    {
        std::vector<std::string> Add;
        std::vector<std::string> Remove;
    };

    /// What to add and remove to go from the strategies applied for the old context to those of the new one
    static Change Diff(std::vector<std::string> const& applied, std::vector<std::string> const& wanted)
    {
        Change change;
        for (std::string const& name : wanted)
            if (std::find(applied.begin(), applied.end(), name) == applied.end())
                change.Add.push_back(name);
        for (std::string const& name : applied)
            if (std::find(wanted.begin(), wanted.end(), name) == wanted.end())
                change.Remove.push_back(name);
        return change;
    }

    static ContextStrategyTable& Instance()
    {
        static ContextStrategyTable table;
        return table;
    }

private:
    std::vector<ContextRule> _rules;
};
}

#define sContextStrategies Playerbots::ContextStrategyTable::Instance()

#endif
