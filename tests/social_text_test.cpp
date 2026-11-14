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

// Standalone test of Social/SocialText.h: g++ -std=c++20 -I../src tests/social_text_test.cpp && ./a.out
#include "Social/SocialText.h"
#include <cstdio>
#include <cstdlib>

using namespace Playerbots::SocialText;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    // the longer key wins at the same '%'
    CHECK(Fill("x %quest_links y %quest_link", { { "%quest_link", "A" }, { "%quest_links", "AB" } }) == "x AB y A");
    CHECK(Fill("100% sure, %zone_name", { { "%zone_name", "Elwynn" } }) == "100% sure, Elwynn");

    CHECK(StartsWithWord("LFG [x]", "lfg"));
    CHECK(StartsWithWord("  lfm 2 dps", "LFM"));
    CHECK(!StartsWithWord("LFGroup", "LFG"));
    CHECK(StartsWithWord("WTB", "WTB"));

    auto ids = LinkIds("LFG |cffffff00|Hquest:123:10|h[A]|h|r and |cffffff00|Hquest:45:1|h[B]|h|r", "quest");
    CHECK(ids.size() == 2 && ids.contains(123) && ids.contains(45));
    CHECK(LinkIds("WTB |cnIQ1:|Hitem:2589::::::::80:::|h[Linen Cloth]|h|r", "item").contains(2589));
    CHECK(LinkIds("no links", "item").empty());

    CHECK(FormatMoney(0) == "0c");
    CHECK(FormatMoney(123456) == "12g 34s 56c");
    CHECK(FormatMoney(500) == "5s");
    CHECK(FormatMoney(10000) == "1g");

    CHECK(ParseMoneyWord("5g") == 50000);
    CHECK(ParseMoneyWord("1g20s") == 12000);
    CHECK(ParseMoneyWord("3c") == 3);
    CHECK(ParseMoneyWord("g") == 0);
    CHECK(ParseMoneyWord("5") == 0);
    CHECK(ParseMoneyWord("to") == 0);

    CHECK(ReceiverAfterTo({ "x", "to", "bob" }) == "bob");
    CHECK(ReceiverAfterTo({ "to" }).empty());

    std::string link = ItemLink(2589, 1, 5, "Linen Cloth", 3);
    CHECK(LinkIds(link, "item").contains(2589));
    CHECK(link.ends_with("x3"));

    for (Category c : { Category::SuggestQuest, Category::SuggestSomething, Category::SuggestInstance, Category::SuggestSell,
        Category::ReplyLfgChannel, Category::ReplyLfgWhisper, Category::ReplyWtbChannel, Category::ReplyWtbWhisper, Category::JoinGroup })
        CHECK(!Lines(c).empty() && *CategoryName(c));

    if (failures)
        return EXIT_FAILURE;
    std::printf("social_text_test: all passed\n");
    return EXIT_SUCCESS;
}
