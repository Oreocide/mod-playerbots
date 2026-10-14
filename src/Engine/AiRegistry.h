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

#ifndef Playerbots_AiRegistry_h__
#define Playerbots_AiRegistry_h__

#include "Engine.h"
#include "Trigger.h"
#include "Value.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Playerbots
{
/// Everything a bot can be told by name: actions, triggers, values and strategies. Filled once at startup
/// (RegisterGenericAi and later the class contexts), read-only afterwards. A creator gets the part after "::"
/// of the requested name ("party member health::low" -> qualifier "low"), empty if there is none
class AiRegistry
{
public:
    using ActionCreator = std::function<std::unique_ptr<Action>(std::string const& qualifier)>;
    using TriggerCreator = std::function<std::unique_ptr<Trigger>(std::string const& qualifier)>;
    using ValueCreator = std::function<std::unique_ptr<ValueBase>(std::string const& qualifier)>;
    using StrategyCreator = std::function<std::unique_ptr<Strategy>(std::string const& qualifier)>;

    static AiRegistry& Instance();

    void AddAction(std::string name, ActionCreator creator) { _actions[std::move(name)] = std::move(creator); }
    void AddTrigger(std::string name, TriggerCreator creator) { _triggers[std::move(name)] = std::move(creator); }
    void AddValue(std::string name, ValueCreator creator) { _values[std::move(name)] = std::move(creator); }
    void AddStrategy(std::string name, StrategyCreator creator) { _strategies[std::move(name)] = std::move(creator); }

    /// Default constructible action
    template <typename A>
    void AddAction(std::string name)
    {
        AddAction(std::move(name), [](std::string const&) { return std::make_unique<A>(); });
    }

    /// Default constructible strategy (it must be constructible without arguments)
    template <typename S>
    void AddStrategy(std::string name)
    {
        AddStrategy(std::move(name), [](std::string const&) { return std::make_unique<S>(); });
    }

    /// Trigger from a callable bool(PlayerbotAI&)
    template <typename F>
    void AddTrigger(std::string name, uint32_t intervalMs, F fn)
    {
        AddTrigger(std::move(name), [intervalMs, fn](std::string const&) -> std::unique_ptr<Trigger>
        {
            return std::make_unique<LambdaTrigger<F>>(intervalMs, fn);
        });
    }

    /// Value from a callable T(PlayerbotAI&)
    template <typename T, typename F>
    void AddValue(std::string name, uint32_t intervalMs, F fn)
    {
        AddValue(std::move(name), [intervalMs, fn](std::string const&) -> std::unique_ptr<ValueBase>
        {
            return std::make_unique<LambdaValue<T, F>>(intervalMs, fn);
        });
    }

    /// "name" or "name::qualifier"; nullptr if the name is unknown
    std::unique_ptr<Action> CreateAction(std::string const& fullName) const;
    std::unique_ptr<Trigger> CreateTrigger(std::string const& fullName) const;
    std::unique_ptr<ValueBase> CreateValue(std::string const& fullName) const;
    std::unique_ptr<Strategy> CreateStrategy(std::string_view fullName) const;

    std::vector<std::string> GetStrategyNames() const;

    /// Receives complaints about unknown names (once per bot and name). Defaults to nothing
    static void SetErrorSink(void (*sink)(std::string const&));
    static void ReportError(std::string const& message);

    /// Splits "name::qualifier"
    static std::pair<std::string, std::string> SplitName(std::string_view fullName);

private:
    template <typename Creator>
    static Creator const* Find(std::unordered_map<std::string, Creator> const& map, std::string const& name)
    {
        auto itr = map.find(name);
        return itr == map.end() ? nullptr : &itr->second;
    }

    std::unordered_map<std::string, ActionCreator> _actions;
    std::unordered_map<std::string, TriggerCreator> _triggers;
    std::unordered_map<std::string, ValueCreator> _values;
    std::unordered_map<std::string, StrategyCreator> _strategies;
};
}

#define sPlayerbotsAi Playerbots::AiRegistry::Instance()

#endif // Playerbots_AiRegistry_h__
