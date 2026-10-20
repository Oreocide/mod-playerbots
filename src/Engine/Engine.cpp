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

#include "Engine.h"
#include "AiContext.h"
#include "PerfMonitor.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
// A chain of prerequisites and alternatives could loop (A needs B, B needs A), the tick gives up after this
constexpr uint32_t MaxStepsPerTick = 48;

// Prerequisites sort just above the action that asked for them, retries and alternatives just below / above
constexpr float PrerequisiteBonus = 0.002f;
constexpr float RetryBonus = 0.001f;
constexpr float AlternativeBonus = 0.003f;

// How long a continuer stays on offer; the next tick normally takes it at once, this covers a long tick delay
constexpr uint32_t PendingExpireMs = 3000;

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}
}

void Engine::AddStrategy(std::unique_ptr<Strategy> strategy)
{
    if (!strategy)
        return;

    RemoveStrategy(strategy->GetName());

    std::string_view group = strategy->GetGroup();
    if (!group.empty())
        std::erase_if(_strategies, [group](std::unique_ptr<Strategy> const& other) { return other->GetGroup() == group; });

    strategy->Init();
    _strategies.push_back(std::move(strategy));
}

bool Engine::RemoveStrategy(std::string_view name)
{
    return std::erase_if(_strategies, [name](std::unique_ptr<Strategy> const& strategy) { return strategy->GetName() == name; }) > 0;
}

bool Engine::HasStrategy(std::string_view name) const
{
    return std::ranges::any_of(_strategies, [name](std::unique_ptr<Strategy> const& strategy) { return strategy->GetName() == name; });
}

std::vector<std::string> Engine::GetStrategyNames() const
{
    std::vector<std::string> names;
    for (std::unique_ptr<Strategy> const& strategy : _strategies)
        names.push_back(strategy->GetName());
    return names;
}

std::string Engine::GetStrategyList(uint8_t states) const
{
    std::string list;
    for (std::unique_ptr<Strategy> const& strategy : _strategies)
    {
        if (!(strategy->GetStates() & states))
            continue;

        if (!list.empty())
            list += ", ";
        list += strategy->GetName();
    }
    return list;
}

std::string Engine::ApplyStrategyString(std::string_view changes, StrategyFactory factory, uint8_t states)
{
    std::string report;
    auto note = [&report](char const* verb, std::string_view name)
    {
        if (!report.empty())
            report += ", ";
        report += verb;
        report += ' ';
        report += name;
    };

    while (!changes.empty())
    {
        size_t comma = changes.find(',');
        std::string_view token = Trim(changes.substr(0, comma));
        changes = comma == std::string_view::npos ? std::string_view() : changes.substr(comma + 1);
        if (token.empty())
            continue;

        char op = token.front();
        if (op == '?')
        {
            if (!report.empty())
                report += "; ";
            report += "strategies: " + GetStrategyList(states);
            continue;
        }

        if (op != '+' && op != '-' && op != '~')
        {
            note("unknown operator in", token);
            continue;
        }

        std::string_view name = Trim(token.substr(1));
        if (name.empty())
            continue;

        bool add = op == '+' || (op == '~' && !HasStrategy(name));
        if (!add)
        {
            note(RemoveStrategy(name) ? "removed" : "not active:", name);
            continue;
        }

        std::unique_ptr<Strategy> strategy = factory ? factory(name) : nullptr;
        if (!strategy)
        {
            note("unknown strategy", name);
            continue;
        }

        if (!(strategy->GetStates() & states))
        {
            note("not for this situation:", name);
            continue;
        }

        AddStrategy(std::move(strategy));
        note("added", name);
    }

    return report;
}

void Engine::Push(Action* action, float relevance, bool skipPrerequisites)
{
    if (!action || relevance <= 0.0f)
        return;

    // an action offered twice (two triggers, a trigger and an alternative) keeps the higher offer
    for (Basket& basket : _baskets)
    {
        if (basket.Act == action)
        {
            if (relevance > basket.Relevance)
            {
                basket.Relevance = relevance;
                basket.SkipPrerequisites = skipPrerequisites;
            }
            return;
        }
    }

    _baskets.push_back({ action, relevance, skipPrerequisites });
}

void Engine::PushAll(std::vector<NextAction> const& next, float forcedRelevance)
{
    for (NextAction const& entry : next)
        Push(_context.GetAction(entry.Name), forcedRelevance > 0.0f ? forcedRelevance : entry.Relevance, false);
}

void Engine::NotifyStateChanged(PlayerbotAI& ai, uint8_t from, uint8_t to)
{
    _pending.clear();
    for (std::unique_ptr<Strategy> const& strategy : _strategies)
        strategy->OnStateChanged(ai, from, to);
}

char const* Engine::DoNextAction(PlayerbotAI& ai, uint8_t state)
{
    _baskets.clear();
    _activeMultipliers.clear();

    // what the last action asked to be offered again: once, if it is still wanted
    uint32_t now = _context.GetNowMs();
    for (Pending const& pending : _pending)
        if (int32_t(pending.ExpiresAtMs - now) > 0 && pending.Act->IsUseful(ai))
            Push(pending.Act, pending.Relevance, false);
    _pending.clear();

    {
        PerfScope scan("(relevance scan)");
        for (std::unique_ptr<Strategy> const& strategy : _strategies)
        {
            if (!(strategy->GetStates() & state))
                continue;

            for (std::unique_ptr<Action> const& action : strategy->GetActions())
                Push(action.get(), action->GetRelevance(ai), false);

            for (TriggerNode const& node : strategy->GetTriggers())
            {
                if (!(node.States & state))
                    continue;

                Trigger* trigger = _context.GetTrigger(node.Trigger);
                if (!trigger || !trigger->IsActive(ai))
                    continue;

                PushAll(node.Handlers, 0.0f);
            }

            for (std::unique_ptr<Multiplier> const& multiplier : strategy->GetMultipliers())
                _activeMultipliers.push_back(multiplier.get());
        }
    }

    for (uint32_t step = 0; step < MaxStepsPerTick && !_baskets.empty(); ++step)
    {
        auto top = std::ranges::max_element(_baskets, {}, &Basket::Relevance);
        Basket basket = *top;
        *top = _baskets.back();
        _baskets.pop_back();

        Action* action = basket.Act;
        if (!action->IsUseful(ai))
            continue;

        float relevance = basket.Relevance;
        for (Multiplier* multiplier : _activeMultipliers)
        {
            relevance *= multiplier->GetValue(ai, action);
            if (relevance <= 0.0f)
                break;
        }

        if (relevance <= 0.0f)
            continue;

        if (!action->IsPossible(ai))
        {
            PushAll(action->GetAlternatives(), relevance + AlternativeBonus);
            continue;
        }

        if (!basket.SkipPrerequisites)
        {
            size_t before = _baskets.size();
            PushAll(action->GetPrerequisites(), relevance + PrerequisiteBonus);
            if (_baskets.size() != before)
            {
                // the prerequisites run first, then the action is tried again without asking for them
                Push(action, relevance + RetryBonus, true);
                continue;
            }
        }

        PerfScope run(action->GetName());
        if (action->Execute(ai))
        {
            for (NextAction const& next : action->GetContinuers())
                if (Action* continuer = _context.GetAction(next.Name))
                    _pending.push_back({ continuer, relevance, now + PendingExpireMs });
            return action->GetName();
        }

        PushAll(action->GetAlternatives(), relevance + AlternativeBonus);
    }

    return nullptr;
}

ActionResult Engine::ExecuteAction(PlayerbotAI& ai, std::string const& name, std::string param)
{
    Action* action = _context.GetAction(name);
    if (!action)
        return ActionResult::Unknown;

    if (!action->IsUseful(ai))
        return ActionResult::Useless;

    if (!action->IsPossible(ai))
        return ActionResult::Impossible;

    action->SetParam(std::move(param));
    bool done = action->Execute(ai);
    action->SetParam({});
    return done ? ActionResult::Ok : ActionResult::Failed;
}
}
