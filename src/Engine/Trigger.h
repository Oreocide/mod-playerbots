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

#ifndef Playerbots_Trigger_h__
#define Playerbots_Trigger_h__

#include "Value.h"
#include <cstdint>
#include <utility>

namespace Playerbots
{
/// What a trigger answers between two checks (intervalMs > 0)
enum class IntervalMode : uint8_t
{
    Cache,  // the last answer is kept (an expensive state check: "low mana")
    Gate    // false until the interval has passed (a periodic offer: "buff", "look around"), like AzerothCore's often/seldom
};

/// A condition a strategy attaches actions to ("low health", "enemy nearby"). Evaluated at most once per
/// AI tick however many strategies use it; intervalMs > 0 keeps the last answer for that long (expensive checks) or,
/// in Gate mode, makes it fire at most that often. jitterMs adds 0..jitterMs to every wait so bots do not fire in step
class Trigger
{
public:
    explicit Trigger(uint32_t intervalMs = 0, IntervalMode mode = IntervalMode::Cache, uint32_t jitterMs = 0)
        : _interval(intervalMs), _jitter(jitterMs), _mode(mode), _rng(uint32_t(reinterpret_cast<uintptr_t>(this) >> 4) | 1u) { }
    virtual ~Trigger() = default;

    void Bind(AiClock const* clock) { _clock = clock; }

    bool IsActive(PlayerbotAI& ai)
    {
        if (!_clock)
            return Check(ai);

        if (_checked && _tick == _clock->Tick)
            return _tickAnswer;

        bool answer;
        if (_checked && _interval && _clock->NowMs - _stamp < _wait)
            answer = _mode == IntervalMode::Cache ? _result : false;
        else
        {
            _result = Check(ai);
            _stamp = _clock->NowMs;
            _wait = _interval + (_jitter ? NextRandom() % (_jitter + 1) : 0);
            _checked = true;
            answer = _result;
        }

        _tick = _clock->Tick;
        _tickAnswer = answer;
        return answer;
    }

protected:
    virtual bool Check(PlayerbotAI& ai) = 0;

private:
    uint32_t NextRandom()
    {
        _rng ^= _rng << 13;
        _rng ^= _rng >> 17;
        _rng ^= _rng << 5;
        return _rng;
    }

    AiClock const* _clock = nullptr;
    uint32_t _interval;
    uint32_t _jitter;
    uint32_t _wait = 0;
    uint32_t _tick = 0;
    uint32_t _stamp = 0;
    IntervalMode _mode;
    uint32_t _rng;
    bool _result = false;
    bool _tickAnswer = false;
    bool _checked = false;
};

template <typename F>
class LambdaTrigger final : public Trigger
{
public:
    LambdaTrigger(uint32_t intervalMs, F fn, IntervalMode mode = IntervalMode::Cache, uint32_t jitterMs = 0)
        : Trigger(intervalMs, mode, jitterMs), _fn(std::move(fn)) { }

protected:
    bool Check(PlayerbotAI& ai) override { return _fn(ai); }

private:
    F _fn;
};
}

#endif // Playerbots_Trigger_h__
