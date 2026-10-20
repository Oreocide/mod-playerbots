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

// Standalone test of the context strategy table (Engine/ContextStrategies.h), see tests/run_engine_test.sh

#include "Engine/ContextStrategies.h"
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    ContextStrategyTable table;
    CHECK(table.Resolve({}).empty());

    table.Register({ [](BotContext const& c) { return c.Place == PlaceKind::Dungeon; }, { "dungeon", "wait for attack" } });
    table.Register({ [](BotContext const& c) { return c.Place == PlaceKind::Dungeon && c.MapId == 574; }, { "utgarde keep", "dungeon" } });
    table.Register({ [](BotContext const& c) { return c.InGroup; }, { "group" } });

    BotContext world;
    CHECK(table.Resolve(world).empty());

    BotContext uk;
    uk.Place = PlaceKind::Dungeon;
    uk.MapId = 574;
    uk.InGroup = true;
    std::vector<std::string> wanted = table.Resolve(uk);
    CHECK(wanted.size() == 4);                       // "dungeon" once although two rules want it
    CHECK(wanted[0] == "dungeon" && wanted[1] == "wait for attack" && wanted[2] == "utgarde keep" && wanted[3] == "group");

    // entering the dungeon adds everything; leaving removes it; a bot that stays keeps what it has
    auto enter = ContextStrategyTable::Diff({}, wanted);
    CHECK(enter.Add.size() == 4 && enter.Remove.empty());

    BotContext nexus = uk;
    nexus.MapId = 576;
    auto move = ContextStrategyTable::Diff(wanted, table.Resolve(nexus));
    CHECK(move.Add.empty());
    CHECK(move.Remove.size() == 1 && move.Remove[0] == "utgarde keep");

    auto stay = ContextStrategyTable::Diff(wanted, table.Resolve(uk));
    CHECK(stay.Add.empty() && stay.Remove.empty());

    auto leave = ContextStrategyTable::Diff(wanted, table.Resolve(world));
    CHECK(leave.Add.empty() && leave.Remove.size() == 4);

    CHECK(uk == uk && !(uk == nexus));

    if (Failures)
    {
        std::printf("context_strategies_test: %d failure(s)\n", Failures);
        return 1;
    }
    std::printf("context_strategies_test: ok\n");
    return 0;
}
