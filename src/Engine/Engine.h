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

#ifndef Playerbots_Engine_h__
#define Playerbots_Engine_h__

#include "Multiplier.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Same model as AzerothCore mod-playerbots: a bot runs a set of named strategies. A strategy ties triggers
// ("low health") to actions ("healing potion") with a relevance, may veto or scale actions with multipliers,
// and every tick the most relevant action that is useful, possible and succeeds is executed.
// Triggers, actions and values are created by name from the AiRegistry, one instance set per bot (AiContext).
namespace Playerbots
{
class AiContext;
class PlayerbotAI;

/// Bot situations a strategy is active in, like AzerothCore's combat / non-combat / dead engines
namespace StrategyStates
{
constexpr uint8_t Combat = 1;
constexpr uint8_t NonCombat = 2;
constexpr uint8_t Dead = 4;
constexpr uint8_t All = Combat | NonCombat | Dead;
}

/// An action by name with the relevance it is offered with
struct NextAction
{
    std::string Name;
    float Relevance = 0.0f;
};

class Action
{
public:
    virtual ~Action() = default;

    virtual char const* GetName() const = 0;

    /// Only for actions placed directly in a strategy (AddAction): 0 = not useful right now, higher = more urgent.
    /// Actions reached through a trigger get their relevance from the trigger node
    virtual float GetRelevance(PlayerbotAI& /*ai*/) { return 0.0f; }

    /// Cheap "does it make sense now" check, a useless action is dropped without trying alternatives
    virtual bool IsUseful(PlayerbotAI& /*ai*/) { return true; }
    /// Hard requirement (has the item, knows the spell), when false the alternatives are tried
    virtual bool IsPossible(PlayerbotAI& /*ai*/) { return true; }

    /// true if the action did something (stops the evaluation for this tick)
    virtual bool Execute(PlayerbotAI& ai) = 0;

    /// Actions that must run first (equip the bow before shooting), offered just above this action
    virtual std::vector<NextAction> GetPrerequisites() const { return {}; }
    /// Tried when this action is impossible or failed (healthstone -> healing potion), offered just below
    virtual std::vector<NextAction> GetAlternatives() const { return {}; }
    /// Offered again on the next tick at the relevance this action ran with, for a few seconds (a move that needs a
    /// second step, a heal that is followed by a bandage). Dropped as soon as one is not useful
    virtual std::vector<NextAction> GetContinuers() const { return {}; }

    /// Argument of a chat command ("co" with "+dps,-follow"), set before Execute by Engine::ExecuteAction
    std::string const& GetParam() const { return _param; }
    void SetParam(std::string param) { _param = std::move(param); }

private:
    std::string _param;
};

/// Offers actions while a trigger is active
struct TriggerNode
{
    std::string Trigger;
    std::vector<NextAction> Handlers;
    /// Situations this node is evaluated in (a loot or buff trigger is not looked at in combat)
    uint8_t States = StrategyStates::All;
};

class Strategy
{
public:
    explicit Strategy(std::string name) : _name(std::move(name)) { }
    virtual ~Strategy() = default;

    std::string const& GetName() const { return _name; }
    std::vector<std::unique_ptr<Action>> const& GetActions() const { return _actions; }

    /// Bot situations this strategy takes part in
    virtual uint8_t GetStates() const { return StrategyStates::All; }
    /// Strategies of one group exclude each other ("dps", "tank", "heal"); empty = no group
    virtual std::string_view GetGroup() const { return {}; }

    virtual void InitTriggers(std::vector<TriggerNode>& /*triggers*/) { }
    virtual void InitMultipliers(std::vector<std::unique_ptr<Multiplier>>& /*multipliers*/) { }
    /// The bot went from one situation to another (StrategyStates): reset what belongs to the old one
    virtual void OnStateChanged(PlayerbotAI& /*ai*/, uint8_t /*from*/, uint8_t /*to*/) { }

    /// Called once when the strategy joins an engine
    void Init()
    {
        _triggers.clear();
        _multipliers.clear();
        InitTriggers(_triggers);
        InitMultipliers(_multipliers);
    }

    std::vector<TriggerNode> const& GetTriggers() const { return _triggers; }
    std::vector<std::unique_ptr<Multiplier>> const& GetMultipliers() const { return _multipliers; }

protected:
    void AddAction(std::unique_ptr<Action> action) { _actions.push_back(std::move(action)); }
    void AddTrigger(std::string trigger, std::vector<NextAction> handlers, uint8_t states = StrategyStates::All)
    {
        _triggers.push_back({ std::move(trigger), std::move(handlers), states });
    }

private:
    std::string _name;
    std::vector<std::unique_ptr<Action>> _actions;
    std::vector<TriggerNode> _triggers;
    std::vector<std::unique_ptr<Multiplier>> _multipliers;
};

enum class ActionResult
{
    Unknown,    // no such action
    Ok,
    Impossible,
    Useless,
    Failed
};

using StrategyFactory = std::function<std::unique_ptr<Strategy>(std::string_view name)>;

class Engine
{
public:
    explicit Engine(AiContext& context) : _context(context) { }

    /// Replaces a strategy of the same name and any other strategy of its group
    void AddStrategy(std::unique_ptr<Strategy> strategy);
    bool RemoveStrategy(std::string_view name);
    bool HasStrategy(std::string_view name) const;
    std::string GetStrategyList(uint8_t states = StrategyStates::All) const;
    std::vector<std::string> GetStrategyNames() const;

    /// "+dps,-follow,~loot,?": add, remove, toggle, list. Returns what happened, for the reply to the master.
    /// states limits it to one situation ("co" = combat): other strategies are refused and not listed
    std::string ApplyStrategyString(std::string_view changes, StrategyFactory factory, uint8_t states = StrategyStates::All);

    /// Executes the most relevant action, returns its name or nullptr if nothing was done
    char const* DoNextAction(PlayerbotAI& ai, uint8_t state = StrategyStates::All);

    /// The bot's situation changed: tells every strategy and drops the continuers of the old one
    void NotifyStateChanged(PlayerbotAI& ai, uint8_t from, uint8_t to);

    /// Runs one action by name now (chat commands), ignoring strategies and multipliers
    ActionResult ExecuteAction(PlayerbotAI& ai, std::string const& name, std::string param = {});

private:
    struct Basket
    {
        Action* Act;
        float Relevance;
        bool SkipPrerequisites;
    };

    void Push(Action* action, float relevance, bool skipPrerequisites);
    void PushAll(std::vector<NextAction> const& next, float forcedRelevance);

    struct Pending
    {
        Action* Act;
        float Relevance;
        uint32_t ExpiresAtMs;
    };

    AiContext& _context;
    std::vector<std::unique_ptr<Strategy>> _strategies;
    /// Continuers of the action that ran last, merged into the next tick's baskets
    std::vector<Pending> _pending;
    /// Reused every tick, a fresh vector per bot per tick was the biggest allocation in the AI loop
    std::vector<Basket> _baskets;
    std::vector<Multiplier*> _activeMultipliers;
};
}

#endif // Playerbots_Engine_h__
