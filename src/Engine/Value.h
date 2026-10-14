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

#ifndef Playerbots_Value_h__
#define Playerbots_Value_h__

#include <cstdint>
#include <string>
#include <utility>

namespace Playerbots
{
class PlayerbotAI;

/// Time source of one bot's AI. The context advances it once per AI tick
struct AiClock
{
    uint32_t Tick = 0;
    uint32_t NowMs = 0;
};

class ValueBase
{
public:
    virtual ~ValueBase() = default;

    void Bind(AiClock const* clock) { _clock = clock; }

protected:
    AiClock const* _clock = nullptr;
};

/// Named piece of knowledge about the bot's situation ("health pct", "combat target"), read through
/// AiContext::GetValue<T>(name, ai)
template <typename T>
class Value : public ValueBase
{
public:
    virtual T Get(PlayerbotAI& ai) = 0;
};

/// Computed on demand and remembered: intervalMs 0 = at most once per AI tick, otherwise at most every intervalMs
template <typename T>
class CalculatedValue : public Value<T>
{
public:
    explicit CalculatedValue(uint32_t intervalMs = 0) : _interval(intervalMs) { }

    T Get(PlayerbotAI& ai) final
    {
        AiClock const* clock = this->_clock;
        if (!clock)
            return Calculate(ai);

        bool stale = !_valid || (_interval == 0 ? _tick != clock->Tick : clock->NowMs - _stamp >= _interval);
        if (stale)
        {
            _value = Calculate(ai);
            _tick = clock->Tick;
            _stamp = clock->NowMs;
            _valid = true;
        }
        return _value;
    }

    void Invalidate() { _valid = false; }

protected:
    virtual T Calculate(PlayerbotAI& ai) = 0;

private:
    T _value{};
    uint32_t _tick = 0;
    uint32_t _stamp = 0;
    uint32_t _interval;
    bool _valid = false;
};

/// Holds what something else set (target of a command, remembered position)
template <typename T>
class ManualValue : public Value<T>
{
public:
    explicit ManualValue(T initial = T{}) : _value(std::move(initial)) { }

    T Get(PlayerbotAI&) final { return _value; }
    void Set(T value) { _value = std::move(value); }

private:
    T _value;
};

/// CalculatedValue from a callable taking the AI
template <typename T, typename F>
class LambdaValue final : public CalculatedValue<T>
{
public:
    LambdaValue(uint32_t intervalMs, F fn) : CalculatedValue<T>(intervalMs), _fn(std::move(fn)) { }

protected:
    T Calculate(PlayerbotAI& ai) override { return _fn(ai); }

private:
    F _fn;
};
}

#endif // Playerbots_Value_h__
