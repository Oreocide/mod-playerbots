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

// Standalone test of the rotation script parser and condition evaluator (Rotation/RotationScript)

#include "Rotation/RotationScript.h"
#include <cstdio>
#include <map>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

struct Facts : FactSource
{
    std::map<std::string, double> Numbers;
    std::map<std::string, bool> Flags;

    bool Number(std::string const& name, double& value) const override
    {
        auto itr = Numbers.find(name);
        if (itr == Numbers.end())
            return false;
        value = itr->second;
        return true;
    }

    bool Flag(std::string const& name, bool& value) const override
    {
        auto itr = Flags.find(name);
        if (itr == Flags.end())
            return false;
        value = itr->second;
        return true;
    }
};

bool Eval(char const* text, Facts const& facts)
{
    std::string error;
    std::unique_ptr<Expr> expr = ParseCondition(text, error);
    if (!expr)
    {
        std::printf("   parse error for '%s': %s\n", text, error.c_str());
        return false;
    }
    return Evaluate(*expr, facts);
}
}

int main()
{
    Facts f;
    f.Numbers = { { "target.hp", 15 }, { "self.power", 60 }, { "enemies", 3 }, { "debuff.rend.remains", 2.5 }, { "buff.1234.remains", 8 } };
    f.Flags = { { "in_combat", true }, { "moving", false }, { "buff.battle_shout", true }, { "buff.power_word:_fortitude", true }, { "debuff.multi-shot", false } };

    // numbers and comparisons
    CHECK(Eval("target.hp < 20", f));
    CHECK(!Eval("target.hp > 20", f));
    CHECK(Eval("target.hp <= 15 && self.power >= 60", f));
    CHECK(Eval("target.hp == 15", f));
    CHECK(Eval("target.hp != 14", f));
    CHECK(Eval("20 > target.hp", f));                                  // number on the left
    CHECK(Eval("debuff.Rend.remains < 3", f));                          // names are case insensitive
    CHECK(Eval("buff.1234.remains > 5", f));                            // spell ids in names
    CHECK(Eval("self.power > target.hp", f));                           // fact against fact
    CHECK(Eval("enemies >= 2.5", f));

    // flags and logic
    CHECK(Eval("in_combat", f));
    CHECK(!Eval("moving", f));
    CHECK(Eval("!moving", f));
    CHECK(Eval("!moving && in_combat", f));
    CHECK(Eval("moving || in_combat", f));
    CHECK(!Eval("moving || !in_combat", f));
    CHECK(Eval("buff.Battle_Shout", f));
    CHECK(Eval("!buff.Power_Word:_Fortitude == 0 && buff.Power_Word:_Fortitude", f));   // colons and hyphens belong to names
    CHECK(!Eval("debuff.Multi-Shot", f));
    CHECK(Eval("!(moving || self.power < 10)", f));
    CHECK(Eval("(moving || in_combat) && target.hp < 20", f));
    CHECK(Eval("in_combat && (moving || (enemies > 2 && target.hp < 30))", f));
    CHECK(Eval("!!in_combat", f));

    // precedence: && binds tighter than ||
    CHECK(Eval("in_combat || moving && moving", f));
    CHECK(!Eval("(in_combat || moving) && moving", f));

    // unknown facts are false, in comparisons too
    CHECK(!Eval("no.such.fact", f));
    CHECK(!Eval("no.such.fact < 5", f));
    CHECK(!Eval("no.such.fact > 5 || moving", f));
    CHECK(Eval("!no.such.fact", f));

    // malformed
    std::string error;
    CHECK(!ParseCondition("", error));
    CHECK(!ParseCondition("target.hp <", error));
    CHECK(!ParseCondition("(in_combat", error));
    CHECK(!ParseCondition("in_combat )", error));
    CHECK(!ParseCondition("in_combat & moving", error));
    CHECK(!ParseCondition("target.hp < < 5", error));

    // facts of a condition
    {
        std::unique_ptr<Expr> expr = ParseCondition("debuff.Rend.remains < 3 && !moving", error);
        std::vector<std::string> names;
        CollectFacts(*expr, names);
        CHECK(names.size() == 2 && names[0] == "debuff.rend.remains" && names[1] == "moving");
    }

    // scripts
    const char* script =
        "# Arms\n"
        "spec 71 Arms Warrior\n"
        "combat:\n"
        "  Execute        | if target.hp < 20          # finish them\n"
        "  Mortal Strike  | if debuff.Deep_Wounds.remains < 3\n"
        "  Slam\n"
        "  Heroic Throw   | target=enemy offgcd if !moving\n"
        "buffs:\n"
        "  Battle Shout   | target=self ooc if !buff.Battle_Shout\n"
        "\n"
        "spec 72 Fury Warrior\n"
        "combat:\n"
        "  Rampage\n";
    ParseResult parsed = ParseRotations(script, "arms.rot");
    CHECK(parsed.Errors.empty());
    CHECK(parsed.Sets.size() == 2);
    if (parsed.Sets.size() == 2)
    {
        RotationSet const& arms = *parsed.Sets[0];
        CHECK(arms.Spec == 71 && arms.Name == "Arms Warrior");
        CHECK(arms.Lists.size() == 2 && arms.Lists.contains("combat") && arms.Lists.contains("buffs"));
        RotationList const& combat = arms.Lists.at("combat");
        CHECK(combat.Entries.size() == 4);
        CHECK(combat.Entries[0].Spell == "Execute" && combat.Entries[0].Condition && combat.Entries[0].Target == "enemy");
        CHECK(combat.Entries[1].Spell == "Mortal Strike" && combat.Entries[1].ConditionText == "debuff.Deep_Wounds.remains < 3");
        CHECK(combat.Entries[2].Spell == "Slam" && !combat.Entries[2].Condition);
        CHECK(combat.Entries[3].OffGcd && combat.Entries[3].Target == "enemy" && combat.Entries[3].Condition);
        RotationEntry const& shout = arms.Lists.at("buffs").Entries[0];
        CHECK(shout.Target == "self" && shout.OutOfCombat && shout.Condition && shout.Line == 9);
        CHECK(parsed.Sets[1]->Spec == 72 && parsed.Sets[1]->Lists.at("combat").Entries.size() == 1);
    }

    // errors name file and line, and the rest still loads
    ParseResult bad = ParseRotations("combat:\nspec x\nspec 5\ncombat:\nSlam | bogus\nSmite | if (\nOk\n", "bad.rot");
    CHECK(bad.Errors.size() == 4);                       // list outside a spec, bad spec id, unknown option, bad condition
    CHECK(bad.Errors.size() >= 1 && bad.Errors[0].starts_with("bad.rot:1:"));
    CHECK(bad.Sets.size() == 1 && bad.Sets[0]->Lists.at("combat").Entries.size() == 1);   // only "Ok" is valid
    CHECK(ParseRotations("Slam\n", "x").Errors.size() == 1);   // a spell outside a list

    if (Failures)
        std::printf("%d check(s) failed\n", Failures);
    else
        std::printf("rotation script tests passed\n");
    return Failures ? 1 : 0;
}
