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

#include "AiContext.h"
#include "AiRegistry.h"

namespace Playerbots
{
namespace
{
/// "event:<name>": active during the tick that sees a raised event of that name
class EventTrigger final : public Trigger
{
public:
    EventTrigger(AiContext const& context, std::string name) : _context(context), _name(std::move(name)) { }

protected:
    bool Check(PlayerbotAI&) override { return _context.HasEvent(_name); }

private:
    AiContext const& _context;
    std::string _name;
};

constexpr std::string_view EventPrefix = "event:";
}

void AiContext::ReportMissing(std::string const& what)
{
    if (_reported.insert(what).second)
        AiRegistry::ReportError(what);
}

Action* AiContext::GetAction(std::string const& name)
{
    auto itr = _actions.find(name);
    if (itr != _actions.end())
        return itr->second.get();

    std::unique_ptr<Action> action = sPlayerbotsAi.CreateAction(name);
    if (!action)
    {
        ReportMissing("unknown action " + name);
        return nullptr;
    }

    return _actions.emplace(name, std::move(action)).first->second.get();
}

Trigger* AiContext::GetTrigger(std::string const& name)
{
    auto itr = _triggers.find(name);
    if (itr != _triggers.end())
        return itr->second.get();

    std::unique_ptr<Trigger> trigger;
    if (name.compare(0, EventPrefix.size(), EventPrefix) == 0)
        trigger = std::make_unique<EventTrigger>(*this, name.substr(EventPrefix.size()));
    else
        trigger = sPlayerbotsAi.CreateTrigger(name);
    if (!trigger)
    {
        ReportMissing("unknown trigger " + name);
        return nullptr;
    }

    trigger->Bind(&_clock);
    return _triggers.emplace(name, std::move(trigger)).first->second.get();
}

ValueBase* AiContext::GetValueBase(std::string const& name)
{
    auto itr = _values.find(name);
    if (itr != _values.end())
        return itr->second.get();

    std::unique_ptr<ValueBase> value = sPlayerbotsAi.CreateValue(name);
    if (!value)
    {
        ReportMissing("unknown value " + name);
        return nullptr;
    }

    value->Bind(&_clock);
    return _values.emplace(name, std::move(value)).first->second.get();
}
}
