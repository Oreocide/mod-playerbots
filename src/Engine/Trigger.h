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
#include <utility>

namespace Playerbots
{
/// A condition a strategy attaches actions to ("low health", "enemy nearby"). Evaluated at most once per
/// AI tick however many strategies use it; intervalMs > 0 keeps the last answer for that long (expensive checks)
class Trigger
{
public:
    explicit Trigger(uint32_t intervalMs = 0) : _interval(intervalMs) { }
    virtual ~Trigger() = default;

    void Bind(AiClock const* clock) { _clock = clock; }

    bool IsActive(PlayerbotAI& ai)
    {
        if (!_clock)
            return Check(ai);

        if (_checked && _tick == _clock->Tick)
            return _result;

        if (_checked && _interval && _clock->NowMs - _stamp < _interval)
        {
            _tick = _clock->Tick;
            return _result;
        }

        _result = Check(ai);
        _tick = _clock->Tick;
        _stamp = _clock->NowMs;
        _checked = true;
        return _result;
    }

protected:
    virtual bool Check(PlayerbotAI& ai) = 0;

private:
    AiClock const* _clock = nullptr;
    uint32_t _interval;
    uint32_t _tick = 0;
    uint32_t _stamp = 0;
    bool _result = false;
    bool _checked = false;
};

template <typename F>
class LambdaTrigger final : public Trigger
{
public:
    LambdaTrigger(uint32_t intervalMs, F fn) : Trigger(intervalMs), _fn(std::move(fn)) { }

protected:
    bool Check(PlayerbotAI& ai) override { return _fn(ai); }

private:
    F _fn;
};
}

#endif // Playerbots_Trigger_h__
