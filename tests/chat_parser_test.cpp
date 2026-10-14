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

// Standalone test of the chat command text handling (Cmd/ChatParser), see tests/run_engine_test.sh

#include "Cmd/ChatParser.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

BotTraits Warrior() { return { "warrior", "tank", 1, 10.0f, 70 }; }
BotTraits Mage() { return { "mage", "ranged", 2, 50.0f, 65 }; }
BotTraits Dk() { return { "deathknight", "melee", 1, 25.0f, 80 }; }
BotTraits Priest() { return { "priest", "healer", 3, 5.0f, 70 }; }
}

int main()
{
    // filters
    CHECK(ApplyChatFilters("follow", Warrior(), 30).Matches);
    CHECK(ApplyChatFilters("follow", Warrior(), 30).Rest == "follow");
    CHECK(ApplyChatFilters("@tank follow", Warrior(), 30).Matches);
    CHECK(!ApplyChatFilters("@tank follow", Mage(), 30).Matches);
    CHECK(ApplyChatFilters("@tank follow", Mage(), 30).Rest == "follow");
    CHECK(ApplyChatFilters("@dps attack", Mage(), 30).Matches);
    CHECK(ApplyChatFilters("@dps attack", Dk(), 30).Matches);
    CHECK(!ApplyChatFilters("@dps attack", Warrior(), 30).Matches);
    CHECK(!ApplyChatFilters("@dps attack", Priest(), 30).Matches);
    CHECK(ApplyChatFilters("@heal stay", Priest(), 30).Matches);
    CHECK(ApplyChatFilters("@caster stay", Mage(), 30).Matches && ApplyChatFilters("@caster stay", Priest(), 30).Matches);
    CHECK(ApplyChatFilters("@ranged stay", Mage(), 30).Matches && !ApplyChatFilters("@ranged stay", Dk(), 30).Matches);
    CHECK(ApplyChatFilters("@dk follow", Dk(), 30).Matches && ApplyChatFilters("@deathknight follow", Dk(), 30).Matches);
    CHECK(ApplyChatFilters("@MAGE follow", Mage(), 30).Matches);                         // case
    CHECK(ApplyChatFilters("@mage,warrior follow", Warrior(), 30).Matches);               // any of
    CHECK(ApplyChatFilters("@mage,@warrior follow", Warrior(), 30).Matches);
    CHECK(!ApplyChatFilters("@mage,warrior follow", Priest(), 30).Matches);
    CHECK(ApplyChatFilters("@warrior @tank follow", Warrior(), 30).Matches);              // all of
    CHECK(!ApplyChatFilters("@warrior @healer follow", Warrior(), 30).Matches);
    CHECK(ApplyChatFilters("@warrior @tank co +dps", Warrior(), 30).Rest == "co +dps");
    CHECK(ApplyChatFilters("@group1 follow", Warrior(), 30).Matches && !ApplyChatFilters("@group1 follow", Mage(), 30).Matches);
    CHECK(!ApplyChatFilters("@group follow", Mage(), 30).Matches);                        // no number
    CHECK(ApplyChatFilters("@nearby follow", Warrior(), 30).Matches && !ApplyChatFilters("@nearby follow", Mage(), 30).Matches);
    CHECK(ApplyChatFilters("@far follow", Mage(), 30).Matches && !ApplyChatFilters("@far follow", Warrior(), 30).Matches);
    CHECK(ApplyChatFilters("@level70 follow", Warrior(), 30).Matches && !ApplyChatFilters("@level70 follow", Mage(), 30).Matches);
    CHECK(ApplyChatFilters("@level60-70 follow", Mage(), 30).Matches && !ApplyChatFilters("@level60-69 follow", Warrior(), 30).Matches);
    CHECK(ApplyChatFilters("@all follow", Mage(), 30).Matches);
    CHECK(!ApplyChatFilters("@tnak follow", Warrior(), 30).Matches);                      // unknown excludes
    CHECK(ApplyChatFilters("  @tank   follow  ", Warrior(), 30).Rest == "follow");
    CHECK(ApplyChatFilters("@tank", Warrior(), 30).Rest.empty());

    // chains
    auto chain = SplitChain("follow ; @mage stay;;  attack ", ";");
    CHECK(chain.size() == 3 && chain[0] == "follow" && chain[1] == "@mage stay" && chain[2] == "attack");
    CHECK(SplitChain("follow;stay", "").size() == 1);
    CHECK(SplitChain("   ", ";").empty());
    CHECK(SplitChain("a||b", "||").size() == 2);

    // commands
    ChatCommandTable table;
    table.Add("follow", { "follow", "Follow me" });
    table.Add("co", { "co", "Combat strategies" });
    table.Add("co list", { "co list", "list" });
    table.Add("status", { "status", "Status", false });
    table.Alias("stats", "status");
    CHECK(ParseCommand(table, "follow")->Name == "follow");
    CHECK(ParseCommand(table, "FOLLOW")->Info->Action == "follow");
    CHECK(ParseCommand(table, "  follow  ")->Param.empty());
    auto co = ParseCommand(table, "co +dps,-Follow");
    CHECK(co && co->Name == "co" && co->Param == "+dps,-Follow");
    auto longest = ParseCommand(table, "co list now");
    CHECK(longest && longest->Name == "co list" && longest->Param == "now");     // longest name wins
    auto alias = ParseCommand(table, "stats");
    CHECK(alias && alias->Info && !alias->Info->Persist);
    CHECK(!ParseCommand(table, "dance"));
    CHECK(!ParseCommand(table, "dance follow"));                                   // the command must come first
    CHECK(!ParseCommand(table, ""));
    CHECK(table.GetNames().size() == 4);

    // prefix
    std::string text = "  !follow me ";
    CHECK(StripPrefix(text, "!") && text == "follow me");
    text = "follow";
    CHECK(!StripPrefix(text, "!"));
    text = "follow";
    CHECK(StripPrefix(text, "") && text == "follow");

    if (Failures)
        std::printf("%d check(s) failed\n", Failures);
    else
        std::printf("chat parser tests passed\n");
    return Failures ? 1 : 0;
}
