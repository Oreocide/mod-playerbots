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

// Standalone test of the owner command text parsing (Cmd/CommandText.h), see tests/run_engine_test.sh

#include "Cmd/CommandText.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    // links of the modern client and of the old one, in any mix with plain words
    ItemSelection a = ParseItemSelection("|cnIQ1:|Hitem:2589::::::::80:577::::::|h[Linen Cloth]|h|r |cnIQ2:|Hitem:6948:0:0|h[Hearthstone]|h|r");
    CHECK((a.Items == std::vector<uint32_t>{ 2589, 6948 }));
    CHECK(a.Words.empty());

    ItemSelection b = ParseItemSelection("Greys |cff9d9d9d|Hitem:25:0:0:0:0:0:0:0:80|h[Worn Shortsword]|h|r vendor");
    CHECK((b.Items == std::vector<uint32_t>{ 25 }));
    CHECK((b.Words == std::vector<std::string>{ "greys", "vendor" }));

    // a repeated link stays (sell two), names with brackets and spaces are not words
    ItemSelection c = ParseItemSelection("|Hitem:100|h[A B]|h|Hitem:100|h[A B]|h");
    CHECK((c.Items == std::vector<uint32_t>{ 100, 100 }));
    CHECK(c.Words.empty());

    // broken input never crashes and gives nothing
    CHECK(ParseItemSelection("|Hitem:").Items.empty());
    CHECK(ParseItemSelection("|Hitem:abc|h[x]|h").Items.empty());
    CHECK(ParseItemSelection("|Hitem:0|h[x]|h").Items.empty());
    CHECK(ParseItemSelection("|Hitem:99999999999999|h[x]|h").Items.empty());
    CHECK(ParseItemSelection("").Items.empty());
    CHECK((ParseItemSelection("all").Words == std::vector<std::string>{ "all" }));

    // spec and other names
    std::vector<std::string> specs{ "Arms", "Fury", "Protection" };
    CHECK(MatchName("fury", specs) == 1u);
    CHECK(MatchName("PRO", specs) == 2u);
    CHECK(MatchName("rot", specs) == 2u);      // contained, and only one
    CHECK(!MatchName("r", specs));              // Arms, Fury, Protection all contain it
    CHECK(!MatchName("", specs));
    CHECK(!MatchName("holy", specs));
    std::vector<std::string> pal{ "Holy", "Protection", "Retribution" };
    CHECK(MatchName("r", pal) == 2u);           // the only prefix match wins over the containing ones
    CHECK(MatchName("Holy ", pal) == 0u);

    // custom strategy lines
    auto line = ParseCustomLine(" Needs Heal > heal > Eat/Drink ");
    CHECK(line && line->Trigger == "needs heal" && (line->Actions == std::vector<std::string>{ "heal", "eat/drink" }));
    CHECK(!ParseCustomLine("needs heal"));
    CHECK(!ParseCustomLine("> heal"));
    CHECK(!ParseCustomLine("needs heal >"));
    CHECK(!ParseCustomLine("a > b;c"));
    CHECK(ParseCustomLine("party member health::low > cast::healing touch") .has_value());

    std::vector<CustomLine> lines{ *ParseCustomLine("a > b > c"), *ParseCustomLine("d > e") };
    std::string stored = FormatCustomDefinition(lines);
    CHECK(stored == "a>b>c;d>e");
    CHECK(ParseCustomDefinition(stored) == lines);
    CHECK(ParseCustomDefinition("").empty());
    CHECK(ParseCustomDefinition(";;bad;a>b").size() == 1u);

    CHECK(IsValidCustomName("my-heal_2"));
    CHECK(!IsValidCustomName(""));
    CHECK(!IsValidCustomName("has space"));
    CHECK(!IsValidCustomName("a::b"));
    CHECK(!IsValidCustomName(std::string(25, 'x')));

    if (Failures == 0)
        std::printf("command_text_test: ok\n");
    return Failures == 0 ? 0 : 1;
}
