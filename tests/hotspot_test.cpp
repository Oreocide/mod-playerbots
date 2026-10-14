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

// Standalone test of the hotspot registry (Travel/Hotspots), see tests/run_engine_test.sh

#include "Travel/Hotspots.h"
#include <cmath>
#include <cstdio>
#include <map>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

uint64_t State = 12345;
uint32_t Rng(uint32_t maxInclusive)
{
    State = State * 6364136223846793005ULL + 1442695040888963407ULL;
    return uint32_t((State >> 33) % (uint64_t(maxInclusive) + 1));
}

Hotspot Make(char const* name, uint8_t tier, uint8_t team = 0)
{
    Hotspot h;
    h.Name = name;
    h.Tier = tier;
    h.Team = team;
    h.Map = 1;
    return h;
}
}

int main()
{
    HotspotRules rules;
    Occupancy none = [](Hotspot const&) { return 0u; };

    {   // filters
        HotspotRegistry r;
        Hotspot capital = Make("ally capital", 1, 1);
        Hotspot horde = Make("horde capital", 1, 2);
        Hotspot town = Make("town", 2);
        town.MaxLevel = 40;
        Hotspot elfStart = Make("elf start", 3);
        elfStart.RaceMask = 1u << 3;           // race 4
        r.Add(capital); r.Add(horde); r.Add(town); r.Add(elfStart);

        std::map<std::string, int> seen;
        for (int i = 0; i < 400; ++i)
            if (auto h = r.Pick({ 60, 1, 1 }, rules, none, Rng))
                ++seen[h->Name];
        CHECK(seen.contains("ally capital") && !seen.contains("horde capital"));   // team
        CHECK(!seen.contains("town"));                                              // level 60 > 40
        CHECK(!seen.contains("elf start"));                                         // race 1 is not 4

        seen.clear();
        for (int i = 0; i < 400; ++i)
            if (auto h = r.Pick({ 10, 4, 1 }, rules, none, Rng))
                ++seen[h->Name];
        CHECK(seen.contains("elf start") && seen.contains("town") && seen.contains("ally capital"));
    }

    {   // weights decide the share
        HotspotRegistry r;
        r.Add(Make("capital", 1));     // weight 5
        r.Add(Make("town", 2));        // weight 2
        std::map<std::string, int> seen;
        for (int i = 0; i < 7000; ++i)
            ++seen[r.Pick({ 30, 1, 1 }, rules, none, Rng)->Name];
        CHECK(seen["capital"] > 4500 && seen["capital"] < 5500);       // 5/7 of 7000 = 5000
        CHECK(seen["town"] > 1500 && seen["town"] < 2500);
    }

    {   // weight 0 = never, no candidates = nothing
        HotspotRegistry r;
        r.Add(Make("hub", 4));
        HotspotRules off = rules;
        off.TierWeight[4] = 0;
        CHECK(!r.Pick({ 30, 1, 1 }, off, none, Rng));
        CHECK(!HotspotRegistry().Pick({ 30, 1, 1 }, rules, none, Rng));
    }

    {   // a full hotspot is skipped
        HotspotRegistry r;
        r.Add(Make("full", 1));
        r.Add(Make("free", 2));
        Occupancy crowded = [](Hotspot const& h) { return h.Name == "full" ? 1000u : 0u; };
        for (int i = 0; i < 100; ++i)
            CHECK(r.Pick({ 30, 1, 1 }, rules, crowded, Rng)->Name == "free");
        HotspotRules uncapped = rules;
        uncapped.TierMaxOccupancy[1] = 0;
        bool full = false;
        for (int i = 0; i < 100; ++i)
            full = full || r.Pick({ 30, 1, 1 }, uncapped, crowded, Rng)->Name == "full";
        CHECK(full);
    }

    {   // removal
        HotspotRegistry r;
        Hotspot marker = Make("marker", 2);
        marker.Owner = 77;
        uint32_t id = r.Add(Make("derived", 1));
        r.Add(marker);
        CHECK(r.Count() == 2);
        r.ClearDerived();
        CHECK(r.Count() == 1 && r.Snapshot()[0].Name == "marker");     // the marker stays
        r.RemoveOwner(77);
        CHECK(r.Count() == 0);
        CHECK(!r.Remove(id));
    }

    {   // points spread inside the radius around a known ground spot
        Hotspot h = Make("spread", 1);
        h.Points = { { 100, 200, 5 }, { 500, 600, 9 } };
        bool first = false, second = false;
        for (int i = 0; i < 300; ++i)
        {
            HotspotPoint p = HotspotRegistry::PickPoint(h, 15.0f, Rng);
            bool a = std::hypot(p.X - 100, p.Y - 200) <= 15.01f && p.Z == 5;
            bool b = std::hypot(p.X - 500, p.Y - 600) <= 15.01f && p.Z == 9;
            CHECK(a || b);
            first = first || a;
            second = second || b;
        }
        CHECK(first && second);

        Hotspot centre = Make("centre", 1);
        centre.Center = { 10, 20, 3 };
        HotspotPoint p = HotspotRegistry::PickPoint(centre, 0.0f, Rng);
        CHECK(std::fabs(p.X - 10) < 0.001f && std::fabs(p.Y - 20) < 0.001f);
    }

    if (Failures)
        std::printf("%d check(s) failed\n", Failures);
    else
        std::printf("hotspot tests passed\n");
    return Failures ? 1 : 0;
}
