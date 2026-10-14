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

// Standalone test of the engine framework (Engine, AiContext, AiRegistry, Trigger, Value, Multiplier).
// It stubs PlayerbotAI and the timing, so it builds without TrinityCore:
//   tests/run_engine_test.sh

#include "Engine/AiContext.h"
#include "Engine/AiRegistry.h"
#include "Engine/Engine.h"
#include "Engine/PerfMonitor.h"
#include <cstdio>
#include <cstdlib>

namespace Playerbots
{
class PlayerbotAI { };

PerfScope::PerfScope(char const* name) : _name(name), _active(false) { }
PerfScope::~PerfScope() { }
}

using namespace Playerbots;

namespace
{
int Failures = 0;
std::vector<std::string> Log;       // what ran, in order
std::vector<std::string> Errors;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

struct Flags
{
    bool Low = false;
    bool Enemy = false;
    bool HasPotion = true;
    bool Fails = false;
    bool Passive = false;
    uint32_t TriggerCalls = 0;
    uint32_t ValueCalls = 0;
    int Counter = 0;
} F;

class LogAction : public Action
{
public:
    LogAction(std::string name) : _name(std::move(name)) { }
    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI&) override { Log.push_back(_name + (GetParam().empty() ? "" : "(" + GetParam() + ")")); return !F.Fails; }
private:
    std::string _name;
};

class PotionAction : public LogAction
{
public:
    PotionAction() : LogAction("potion") { }
    bool IsPossible(PlayerbotAI&) override { return F.HasPotion; }
    std::vector<NextAction> GetAlternatives() const override { return { { "bandage", 0.0f } }; }
};

class ShootAction : public LogAction
{
public:
    ShootAction() : LogAction("shoot") { }
    std::vector<NextAction> GetPrerequisites() const override { return { { "equip bow", 0.0f } }; }
};

class LoopA : public LogAction
{
public:
    LoopA() : LogAction("loop a") { }
    bool IsPossible(PlayerbotAI&) override { return false; }
    std::vector<NextAction> GetAlternatives() const override { return { { "loop b", 0.0f } }; }
};

class LoopB : public LogAction
{
public:
    LoopB() : LogAction("loop b") { }
    bool IsPossible(PlayerbotAI&) override { return false; }
    std::vector<NextAction> GetAlternatives() const override { return { { "loop a", 0.0f } }; }
};

class ParamStrategy : public Strategy
{
public:
    ParamStrategy(std::string name, std::string group = {}, uint8_t states = StrategyStates::All)
        : Strategy(std::move(name)), _group(std::move(group)), _states(states) { }
    std::string_view GetGroup() const override { return _group; }
    uint8_t GetStates() const override { return _states; }
    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "low health", { { "potion", 50.0f } } });
        triggers.push_back({ "enemy", { { "attack", 20.0f }, { "shoot", 30.0f } } });
    }
private:
    std::string _group;
    uint8_t _states;
};

class PassiveMult : public Multiplier
{
public:
    float GetValue(PlayerbotAI&, Action* action) override
    {
        if (!F.Passive)
            return 1.0f;
        return std::string(action->GetName()) == "attack" ? 0.0f : 0.5f;
    }
};

class MultStrategy : public Strategy
{
public:
    MultStrategy() : Strategy("passive") { }
    void InitMultipliers(std::vector<std::unique_ptr<Multiplier>>& m) override { m.push_back(std::make_unique<PassiveMult>()); }
};

class LegacyAction : public LogAction
{
public:
    LegacyAction() : LogAction("legacy") { }
    float GetRelevance(PlayerbotAI&) override { return 5.0f; }
};

class LegacyStrategy : public Strategy
{
public:
    LegacyStrategy() : Strategy("legacy") { AddAction(std::make_unique<LegacyAction>()); }
};

class LoopStrategy : public Strategy
{
public:
    LoopStrategy() : Strategy("loop") { }
    void InitTriggers(std::vector<TriggerNode>& triggers) override { triggers.push_back({ "enemy", { { "loop a", 10.0f } } }); }
};

void Register()
{
    AiRegistry& r = sPlayerbotsAi;
    r.AddAction("potion", [](std::string const&) { return std::make_unique<PotionAction>(); });
    r.AddAction("bandage", [](std::string const&) { return std::make_unique<LogAction>("bandage"); });
    r.AddAction("attack", [](std::string const&) { return std::make_unique<LogAction>("attack"); });
    r.AddAction("equip bow", [](std::string const&) { return std::make_unique<LogAction>("equip bow"); });
    r.AddAction("co", [](std::string const&) { return std::make_unique<LogAction>("co"); });
    r.AddAction<ShootAction>("shoot");
    r.AddAction<LoopA>("loop a");
    r.AddAction<LoopB>("loop b");
    r.AddTrigger("low health", 0, [](PlayerbotAI&) { ++F.TriggerCalls; return F.Low; });
    r.AddTrigger("enemy", 0, [](PlayerbotAI&) { return F.Enemy; });
    r.AddTrigger("slow check", 1000, [](PlayerbotAI&) { ++F.TriggerCalls; return F.Counter++ == 0; });
    r.AddValue<int>("counter", 0, [](PlayerbotAI&) { ++F.ValueCalls; return F.Counter; });
    r.AddValue<int>("slow counter", 500, [](PlayerbotAI&) { ++F.ValueCalls; return F.Counter; });
    r.AddValue("manual", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<ManualValue<int>>(7); });
    r.AddValue("echo", [](std::string const& q) -> std::unique_ptr<ValueBase>
    {
        return std::make_unique<LambdaValue<std::string, std::function<std::string(PlayerbotAI&)>>>(0, [q](PlayerbotAI&) { return q; });
    });
    r.AddStrategy("a", [](std::string const&) { return std::make_unique<ParamStrategy>("a"); });
    r.AddStrategy("b", [](std::string const&) { return std::make_unique<ParamStrategy>("b"); });
    r.AddStrategy("dps", [](std::string const&) { return std::make_unique<ParamStrategy>("dps", "role"); });
    r.AddStrategy("tank", [](std::string const&) { return std::make_unique<ParamStrategy>("tank", "role"); });
    r.AddStrategy("combatonly", [](std::string const&) { return std::make_unique<ParamStrategy>("combatonly", "", StrategyStates::Combat); });
    r.AddStrategy<MultStrategy>("passive");
    r.AddStrategy<LegacyStrategy>("legacy");
    r.AddStrategy<LoopStrategy>("loop");
    AiRegistry::SetErrorSink([](std::string const& message) { Errors.push_back(message); });
}

std::unique_ptr<Strategy> Make(std::string_view name) { return sPlayerbotsAi.CreateStrategy(name); }

void Reset()
{
    F = Flags();
    Log.clear();
    Errors.clear();
}

struct Bot
{
    AiContext Context;
    Engine Eng{ Context };
    PlayerbotAI Ai;
    char const* Tick(uint32_t nowMs = 0, uint8_t state = StrategyStates::All)
    {
        Context.BeginTick(nowMs);
        return Eng.DoNextAction(Ai, state);
    }
};
}

int main()
{
    Register();

    {   // the most relevant offered action wins
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        F.Low = true;
        F.Enemy = true;
        CHECK(std::string(bot.Tick(1)) == "potion");           // 50 beats shoot 30 and attack 20
        F.Low = false;
        CHECK(Log.size() == 1);
    }
    {   // a prerequisite runs first, the action on the next try
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        F.Enemy = true;
        char const* done = bot.Tick(1);
        CHECK(std::string(done) == "equip bow");
        CHECK(Log.size() == 1 && Log[0] == "equip bow");
    }
    {   // nothing active, nothing happens
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        CHECK(bot.Tick(1) == nullptr);
    }
    {   // impossible action falls back to its alternative
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        F.Low = true;
        F.HasPotion = false;
        CHECK(std::string(bot.Tick(1)) == "bandage");
    }
    {   // failed action tries the alternative, then the next best action
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        F.Low = true;
        F.Fails = true;
        CHECK(bot.Tick(1) == nullptr);
        CHECK(Log.size() >= 2 && Log[0] == "potion" && Log[1] == "bandage");
    }
    {   // trigger evaluated once per tick however many strategies use it, again on the next tick
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        bot.Eng.AddStrategy(Make("b"));
        F.Low = true;
        bot.Tick(1);
        CHECK(F.TriggerCalls == 1);
        bot.Tick(2);
        CHECK(F.TriggerCalls == 2);
    }
    {   // the same action offered by two strategies runs once
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        bot.Eng.AddStrategy(Make("b"));
        F.Low = true;
        bot.Tick(1);
        CHECK(Log.size() == 1);
    }
    {   // multipliers veto and scale
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("a"));
        bot.Eng.AddStrategy(Make("passive"));
        F.Passive = true;
        F.Enemy = true;
        F.Low = true;
        CHECK(std::string(bot.Tick(1)) == "potion");           // 50 * 0.5 = 25 still beats 30 * 0.5? no: shoot 15, attack vetoed
        Log.clear();
        F.Low = false;
        char const* done = bot.Tick(2);
        CHECK(done && std::string(done) == "equip bow");        // attack vetoed, shoot (prerequisite chain) remains
    }
    {   // a vetoed action is gone, not retried
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("passive"));
        bot.Eng.AddStrategy(Make("loop"));
        F.Passive = true;
        F.Enemy = true;
        CHECK(bot.Tick(1) == nullptr);
    }
    {   // strategy states
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("combatonly"));
        F.Low = true;
        CHECK(bot.Tick(1, StrategyStates::NonCombat) == nullptr);
        CHECK(bot.Tick(2, StrategyStates::Combat) != nullptr);
    }
    {   // groups exclude each other
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("dps"));
        bot.Eng.AddStrategy(Make("a"));
        bot.Eng.AddStrategy(Make("tank"));
        CHECK(!bot.Eng.HasStrategy("dps") && bot.Eng.HasStrategy("tank") && bot.Eng.HasStrategy("a"));
    }
    {   // legacy actions with their own relevance
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("legacy"));
        CHECK(std::string(bot.Tick(1)) == "legacy");
    }
    {   // alternative cycles end
        Reset();
        Bot bot;
        bot.Eng.AddStrategy(Make("loop"));
        F.Enemy = true;
        CHECK(bot.Tick(1) == nullptr);
    }
    {   // strategy strings
        Reset();
        Bot bot;
        std::string r = bot.Eng.ApplyStrategyString("+a, +b ,-a,~b,~dps,+nope,*x,-zzz", Make);
        CHECK(!bot.Eng.HasStrategy("a"));
        CHECK(!bot.Eng.HasStrategy("b"));                      // toggled off
        CHECK(bot.Eng.HasStrategy("dps"));
        CHECK(r.find("unknown strategy nope") != std::string::npos);
        CHECK(r.find("unknown operator in *x") != std::string::npos);
        CHECK(r.find("not active: zzz") != std::string::npos);
        std::string list = bot.Eng.ApplyStrategyString("?", Make);
        CHECK(list == "strategies: dps");
    }
    {   // strategy strings limited to a situation
        Reset();
        Bot bot;
        std::string r = bot.Eng.ApplyStrategyString("+combatonly,+a", Make, StrategyStates::NonCombat);
        CHECK(!bot.Eng.HasStrategy("combatonly") && bot.Eng.HasStrategy("a"));
        CHECK(r.find("not for this situation: combatonly") != std::string::npos);
        bot.Eng.ApplyStrategyString("+combatonly", Make, StrategyStates::Combat);
        CHECK(bot.Eng.ApplyStrategyString("?", Make, StrategyStates::NonCombat) == "strategies: a");
        CHECK(bot.Eng.ApplyStrategyString("?", Make, StrategyStates::Combat) == "strategies: a, combatonly");
    }
    {   // direct execution
        Reset();
        Bot bot;
        CHECK(bot.Eng.ExecuteAction(bot.Ai, "nothing") == ActionResult::Unknown);
        CHECK(bot.Eng.ExecuteAction(bot.Ai, "co", "+dps") == ActionResult::Ok);
        CHECK(Log.size() == 1 && Log[0] == "co(+dps)");
        F.HasPotion = false;
        CHECK(bot.Eng.ExecuteAction(bot.Ai, "potion") == ActionResult::Impossible);
        F.HasPotion = true;
        F.Fails = true;
        CHECK(bot.Eng.ExecuteAction(bot.Ai, "potion") == ActionResult::Failed);
        CHECK(Errors.size() == 1 && Errors[0] == "unknown action nothing");
        bot.Eng.ExecuteAction(bot.Ai, "nothing");
        CHECK(Errors.size() == 1);                              // reported once
    }
    {   // values: per tick cache, interval cache, manual, qualifier, wrong type
        Reset();
        Bot bot;
        bot.Context.BeginTick(1000);
        CHECK(bot.Context.GetValue<int>("counter", bot.Ai) == 0);
        F.Counter = 5;
        CHECK(bot.Context.GetValue<int>("counter", bot.Ai) == 0);   // same tick, cached
        CHECK(F.ValueCalls == 1);
        bot.Context.BeginTick(1010);
        CHECK(bot.Context.GetValue<int>("counter", bot.Ai) == 5);
        CHECK(bot.Context.GetValue<int>("slow counter", bot.Ai) == 5);
        F.Counter = 9;
        bot.Context.BeginTick(1200);
        CHECK(bot.Context.GetValue<int>("slow counter", bot.Ai) == 5);  // 190 ms < 500
        bot.Context.BeginTick(1600);
        CHECK(bot.Context.GetValue<int>("slow counter", bot.Ai) == 9);
        CHECK(bot.Context.GetValue<int>("manual", bot.Ai) == 7);
        CHECK(bot.Context.SetValue<int>("manual", 3));
        CHECK(bot.Context.GetValue<int>("manual", bot.Ai) == 3);
        CHECK(bot.Context.GetValue<std::string>("echo", "hello", bot.Ai) == "hello");
        CHECK(bot.Context.GetValue<std::string>("echo", "again", bot.Ai) == "again");
        CHECK(bot.Context.GetValue<float>("counter", bot.Ai) == 0.0f);   // wrong type -> default, reported
        CHECK(bot.Context.GetValue<int>("missing", bot.Ai) == 0);
        CHECK(Errors.size() == 2);
    }
    {   // trigger interval keeps the answer
        Reset();
        Bot bot;
        Trigger* t = bot.Context.GetTrigger("slow check");
        bot.Context.BeginTick(0);
        CHECK(t->IsActive(bot.Ai));                  // first check: Counter 0 -> true
        bot.Context.BeginTick(300);
        CHECK(t->IsActive(bot.Ai));                  // held, not recomputed
        CHECK(F.TriggerCalls == 1);
        bot.Context.BeginTick(1500);
        CHECK(!t->IsActive(bot.Ai));                 // recomputed: Counter 1 -> false
        CHECK(F.TriggerCalls == 2);
    }

    if (Failures)
        std::printf("%d check(s) failed\n", Failures);
    else
        std::printf("engine tests passed\n");
    return Failures ? 1 : 0;
}
