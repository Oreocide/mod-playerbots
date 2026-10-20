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

// Standalone test of the guild name table (Social/GuildNames.h), see tests/run_engine_test.sh

#include "Social/GuildNames.h"
#include <cctype>
#include <cstdio>
#include <set>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    std::set<std::string> seen;
    for (uint32_t a = 0; a < 30; ++a)
        for (uint32_t b = 0; b < 30; ++b)
            for (uint32_t pattern = 0; pattern < 3; ++pattern)
            {
                std::string name = MakeGuildName(a, b, pattern);
                CHECK(!name.empty() && name.size() <= 24);
                for (unsigned char c : name)
                    CHECK(std::isalpha(c) || c == ' ');
                CHECK(name.front() != ' ' && name.back() != ' ');
                seen.insert(name);
            }

    // plenty of distinct names, and any index is accepted
    CHECK(seen.size() >= 1500);
    CHECK(MakeGuildName(4000000000u, 4000000001u, 4000000002u).size() <= 24);
    CHECK(MakeGuildName(3, 5, 1) == MakeGuildName(3, 5, 1));

    if (Failures == 0)
        std::printf("guild_names_test: ok (%zu names)\n", seen.size());
    return Failures == 0 ? 0 : 1;
}
