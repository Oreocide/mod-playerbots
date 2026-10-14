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

#ifndef Playerbots_AiContext_h__
#define Playerbots_AiContext_h__

#include "Engine.h"
#include "Trigger.h"
#include "Value.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Playerbots
{
/// The actions, triggers and values of one bot, created from the AiRegistry the first time they are asked for.
/// Instances live as long as the bot, so values keep their cache and actions their own state
class AiContext
{
public:
    /// Starts the next AI tick; values and triggers recompute at most once per tick
    void BeginTick(uint32_t nowMs)
    {
        ++_clock.Tick;
        _clock.NowMs = nowMs;
    }

    uint32_t GetTick() const { return _clock.Tick; }
    uint32_t GetNowMs() const { return _clock.NowMs; }

    /// nullptr (reported once) if the name is not registered
    Action* GetAction(std::string const& name);
    Trigger* GetTrigger(std::string const& name);
    ValueBase* GetValueBase(std::string const& name);

    template <typename T>
    Value<T>* FindValue(std::string const& name)
    {
        ValueBase* base = GetValueBase(name);
        if (!base)
            return nullptr;

        auto* value = dynamic_cast<Value<T>*>(base);
        if (!value)
            ReportMissing("value " + name + " has a different type");
        return value;
    }

    /// Current value, T{} if it does not exist
    template <typename T>
    T GetValue(std::string const& name, PlayerbotAI& ai)
    {
        Value<T>* value = FindValue<T>(name);
        return value ? value->Get(ai) : T{};
    }

    /// "name::qualifier" shortcut
    template <typename T>
    T GetValue(std::string const& name, std::string const& qualifier, PlayerbotAI& ai)
    {
        return GetValue<T>(name + "::" + qualifier, ai);
    }

    /// Replaces what a manual value holds (target of a command)
    template <typename T>
    bool SetValue(std::string const& name, T value)
    {
        auto* manual = dynamic_cast<ManualValue<T>*>(GetValueBase(name));
        if (!manual)
            return false;
        manual->Set(std::move(value));
        return true;
    }

private:
    void ReportMissing(std::string const& what);

    AiClock _clock;
    std::unordered_map<std::string, std::unique_ptr<Action>> _actions;
    std::unordered_map<std::string, std::unique_ptr<Trigger>> _triggers;
    std::unordered_map<std::string, std::unique_ptr<ValueBase>> _values;
    std::unordered_set<std::string> _reported;
};
}

#endif // Playerbots_AiContext_h__
