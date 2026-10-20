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

// Standalone test of the travel graph and planner (Travel/Graph/TravelGraph), see tests/run_engine_test.sh

#include "Travel/Graph/TravelGraph.h"
#include <cmath>
#include <cstdio>

using namespace Playerbots;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

bool Near(float a, float b, float tolerance = 0.5f) { return std::fabs(a - b) <= tolerance; }

TravelNode Node(uint32_t map, float x, float y, char const* name, NodeKind kind = NodeKind::Hub)
{
    TravelNode n;
    n.Map = map;
    n.X = x;
    n.Y = y;
    n.Name = name;
    n.Kind = kind;
    return n;
}

struct World
{
    TravelGraph G;
    uint32_t A, B, C, D, E;
    uint32_t TaxiAC;

    World()
    {
        A = G.AddNode(Node(0, 0, 0, "A"));
        B = G.AddNode(Node(0, 1000, 0, "B"));
        C = G.AddNode(Node(0, 5000, 0, "C"));
        D = G.AddNode(Node(1, 0, 0, "D"));
        E = G.AddNode(Node(1, 500, 0, "E"));
        G.Finalize();
        G.AddWalkPair(A, B);
        G.AddWalkPair(B, C);
        G.AddWalkPair(D, E);

        TravelLink taxi;
        taxi.From = A;
        taxi.To = C;
        taxi.Type = LinkType::Taxi;
        taxi.Seconds = 60.0f;
        taxi.Team = 1;
        taxi.Gold = 100;
        taxi.MinLevel = 10;
        taxi.Ref = 77;
        TaxiAC = G.AddLink(taxi);

        TravelLink boat;
        boat.From = C;
        boat.To = D;
        boat.Type = LinkType::Transport;
        boat.Seconds = 200.0f;
        boat.Ref = 5;
        G.AddLink(boat);
    }
};

float LegSum(Route const& r)
{
    float s = 0;
    for (RouteLeg const& l : r.Legs)
        s += l.Seconds;
    return s;
}
}

int main()
{
    {   // walking between neighbours
        World w;
        Route r = w.G.Plan(w.A, w.B, {});
        CHECK(r.Found && r.Legs.size() == 1 && r.Legs[0].Type == LinkType::Walk);
        CHECK(Near(r.Seconds, 1000 * 1.3f / 7.0f));
        CHECK(Near(r.Seconds, LegSum(r)));
        CHECK(w.G.Plan(w.A, w.A, {}).Found && w.G.Plan(w.A, w.A, {}).Legs.empty());
    }
    {   // the flight path beats the walk for the long trip
        World w;
        RouteOptions o;
        o.Team = 1;
        o.Level = 20;
        o.Money = 1000;
        Route r = w.G.Plan(w.A, w.C, o);
        CHECK(r.Found && r.Legs.size() == 1 && r.Legs[0].Type == LinkType::Taxi && r.Legs[0].Ref == 77);
        CHECK(Near(r.Seconds, 60.0f));
    }
    {   // who may fly: team, level, money, switches, blocked links
        World w;
        RouteOptions o;
        o.Team = 2;
        o.Level = 20;
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Walk);        // Horde cannot use it
        o.Team = 1;
        o.Level = 5;
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Walk);        // too low
        o.Level = 20;
        o.Money = 50;
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Walk);        // too poor
        o.Money = 500;
        o.AllowTaxi = false;
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Walk);
        o.AllowTaxi = true;
        o.Blocked = [&](TravelLink const& l) { return l.Id == w.TaxiAC; };
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Walk);        // failed recently
        o.Blocked = nullptr;
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Taxi);
    }
    {   // gold can be worth less than time
        World w;
        RouteOptions o;
        o.Team = 1;
        o.Level = 20;
        o.Money = 1000;
        o.GoldSecondsPerCopper = 1.0f;                                         // 100 copper = 100 s, the walk is 928 s: still fly
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Taxi);
        o.GoldSecondsPerCopper = 20.0f;                                        // 2000 s of gold: walk
        CHECK(w.G.Plan(w.A, w.C, o).Legs[0].Type == LinkType::Walk);
    }
    {   // across maps: a boat, then a walk
        World w;
        RouteOptions o;
        o.Team = 1;
        o.Level = 20;
        Route r = w.G.Plan(w.A, w.E, o);
        CHECK(r.Found && r.Legs.size() == 3);
        CHECK(r.Legs[0].Type == LinkType::Taxi && r.Legs[1].Type == LinkType::Transport && r.Legs[2].Type == LinkType::Walk);
        o.AllowTransport = false;
        CHECK(!w.G.Plan(w.A, w.E, o).Found);                                   // no other way over the sea
        CHECK(!w.G.Plan(w.D, w.A, {}).Found);                                  // the boat only goes one way
    }
    {   // positions that are not nodes
        World w;
        RouteOptions o;
        o.Team = 1;
        o.Level = 20;
        Route r = w.G.PlanPositions({ 0, 20, 10, 0 }, { 0, 5030, 0, 0 }, o);
        CHECK(r.Found && r.Legs.size() == 3);
        CHECK(r.Legs[0].Type == LinkType::Walk && r.Legs[1].Type == LinkType::Taxi && r.Legs[2].Type == LinkType::Walk);
        CHECK(r.Legs[0].From == UINT32_MAX && r.Legs[2].To == UINT32_MAX);
        CHECK(Near(r.Seconds, LegSum(r), 0.1f));

        // a short trip is a plain walk
        Route near = w.G.PlanPositions({ 0, 0, 0, 0 }, { 0, 100, 0, 0 }, o);
        CHECK(near.Found && near.Legs.size() == 1 && near.Legs[0].From == UINT32_MAX && near.Legs[0].To == UINT32_MAX);

        // another map with no way there
        CHECK(!w.G.PlanPositions({ 0, 0, 0, 0 }, { 9, 0, 0, 0 }, o).Found);
        // the far side of the sea needs the boat
        Route sea = w.G.PlanPositions({ 0, 10, 0, 0 }, { 1, 520, 0, 0 }, o);
        CHECK(sea.Found && sea.Legs.size() == 5);                              // walk, taxi, boat, walk(D->E), walk to goal
    }
    {   // a hearthstone as a temporary link from the start
        World w;
        RouteOptions o;
        o.Team = 1;
        o.Level = 20;
        TravelLink hearth;
        hearth.To = w.C;
        hearth.Type = LinkType::Teleport;
        hearth.Seconds = 15.0f;
        Route r = w.G.PlanPositions({ 0, 0, 0, 0 }, { 0, 5100, 0, 0 }, o, { hearth });
        CHECK(r.Found && r.Legs[0].Type == LinkType::Teleport);                // 15 s beats a 60 s flight
        o.AllowTeleport = false;
        CHECK(w.G.PlanPositions({ 0, 0, 0, 0 }, { 0, 5100, 0, 0 }, o, { hearth }).Legs[0].Type != LinkType::Teleport);
    }
    {   // one pass over the graph ranks many goals, and agrees with the planner
        World w;
        RouteOptions o;
        o.Team = 1;
        o.Level = 20;
        o.Money = 1000;
        Position3 start{ 0, 20, 10, 0 };
        Reach reach = w.G.ComputeReach(start, o);
        float cost = 0, seconds = 0;
        Position3 farGoal{ 0, 5030, 0, 0 };
        CHECK(w.G.ReachPosition(reach, farGoal, o, cost, seconds));
        Route planned = w.G.PlanPositions(start, farGoal, o);
        CHECK(Near(seconds, planned.Seconds, 0.2f));                          // the flight, not the walk
        Position3 seaGoal{ 1, 520, 0, 0 };
        CHECK(w.G.ReachPosition(reach, seaGoal, o, cost, seconds));
        CHECK(Near(seconds, w.G.PlanPositions(start, seaGoal, o).Seconds, 0.2f));
        CHECK(!w.G.ReachPosition(reach, { 9, 0, 0, 0 }, o, cost, seconds));   // no way to another map
        CHECK(w.G.ReachPosition(reach, { 0, 100, 0, 0 }, o, cost, seconds) && seconds < 40.0f);   // a short walk is a walk

        o.Team = 2;                                                           // Horde: no flight, the long walk
        Reach horde = w.G.ComputeReach(start, o);
        CHECK(w.G.ReachPosition(horde, farGoal, o, cost, seconds) && seconds > 500.0f);
        // a hearthstone link makes a far place near
        TravelLink hearth;
        hearth.To = w.C;
        hearth.Type = LinkType::Teleport;
        hearth.Seconds = 15.0f;
        Reach hearthed = w.G.ComputeReach(start, o, { hearth });
        CHECK(w.G.ReachPosition(hearthed, farGoal, o, cost, seconds) && seconds < 60.0f);
    }
    {   // enemy ground is not walked through: a node can belong to a team or to nobody
        TravelGraph g;
        uint32_t home = g.AddNode(Node(0, 0, 0, "home"));
        uint32_t enemy = g.AddNode(Node(0, 500, 0, "enemy capital"));
        uint32_t far = g.AddNode(Node(0, 1000, 0, "far"));
        uint32_t detour = g.AddNode(Node(0, 500, 900, "detour"));
        g.Finalize();
        g.AddWalkPair(home, enemy);
        g.AddWalkPair(enemy, far);
        g.AddWalkPair(home, detour);
        g.AddWalkPair(detour, far);
        g.MutableNode(enemy).Team = 2;                       // Horde ground

        RouteOptions horde;
        horde.Team = 2;
        Route through = g.Plan(home, far, horde);
        CHECK(through.Found && through.Legs.size() == 2 && through.Legs[0].To == enemy);   // the short way

        RouteOptions alliance;
        alliance.Team = 1;
        Route around = g.Plan(home, far, alliance);
        CHECK(around.Found && around.Legs.size() == 2 && around.Legs[0].To == detour);      // around it

        g.MutableNode(detour).Team = 3;                      // closed to everyone
        CHECK(!g.Plan(home, far, alliance).Found);
        CHECK(g.Plan(home, far, horde).Found);
        // a start next to enemy ground picks another start node
        Route fromPos = g.PlanPositions({ 0, 495, 5, 0 }, { 0, 1000, 5, 0 }, alliance);
        CHECK(fromPos.Found);                                // at least the plain walk
    }
    {   // danger: a longer way round beats a short one past creatures far above the bot
        TravelGraph g;
        uint32_t a = g.AddNode(Node(0, 0, 0, "a"));
        uint32_t b = g.AddNode(Node(0, 1000, 0, "b"));
        uint32_t m = g.AddNode(Node(0, 500, 600, "m"));
        g.Finalize();
        g.AddWalkPair(a, b);
        g.AddWalkPair(a, m);
        g.AddWalkPair(m, b);
        uint32_t direct = UINT32_MAX;
        for (uint32_t id : g.LinksFrom(a))
            if (g.GetLink(id).To == b)
                direct = id;
        g.MutableLink(direct).DangerAlliance = 40;           // level 40 creatures along the direct link

        RouteOptions o;
        o.Team = 1;
        o.Level = 30;                                        // 10 above the free 3: 7 levels x 30 s = 210 s extra, short of the block
        Route r = g.Plan(a, b, o);
        CHECK(r.Found && r.Legs.size() == 2);                // the direct walk is 186 s + 210 s, the detour is about 290 s
        o.Level = 38;                                        // within the free levels: direct again
        CHECK(g.Plan(a, b, o).Legs.size() == 1);
        o.Level = 20;                                        // 20 above: blocked, the only way is the detour
        CHECK(g.Plan(a, b, o).Legs.size() == 2);
        o.Team = 2;                                          // Horde sees no danger on it
        CHECK(g.Plan(a, b, o).Legs.size() == 1);
        // route seconds are travel time, not the penalty
        o.Team = 1;
        o.Level = 30;
        Route again = g.Plan(a, b, o);
        float legs = 0;
        for (RouteLeg const& l : again.Legs)
            legs += l.Seconds;
        CHECK(Near(again.Seconds, legs));
    }
    {   // neighbours get linked, once per pair
        TravelGraph g;
        for (int i = 0; i < 5; ++i)
            g.AddNode(Node(0, float(i * 100), 0, "n"));
        g.AddNode(Node(1, 0, 0, "other map"));
        g.Finalize();
        g.ConnectNeighbours(2, 1000.0f);
        CHECK(g.Plan(0, 4, {}).Found);
        CHECK(!g.Plan(0, 5, {}).Found);
        size_t links = g.LinkCount();
        CHECK(links % 2 == 0);                                                 // walk pairs
        CHECK(g.NearestNodes(0, 10, 0, 2, 150.0f).size() == 2);
        CHECK(g.NearestNodes(0, 10, 0, 9, 150.0f).size() == 2);                // only two within 150 yd
        CHECK(g.NearestNodes(7, 0, 0, 3, 100.0f).empty());
    }

    {   // air links: used only by a bot that may fly, never otherwise; cost = its seconds; a blocked one is skipped
        World w;
        TravelLink air;
        air.From = w.A;
        air.To = w.C;
        air.Type = LinkType::Air;
        air.Seconds = 5000.0f / 22.0f + 12.0f;    // 239 s: slower than the 60 s flight path, faster than walking (~930 s)
        uint32_t airId = w.G.AddLink(air);
        std::swap(air.From, air.To);
        w.G.AddLink(air);

        RouteOptions noFlight;                    // no taxi gold: the flight path is out, only the walk and the air are left
        noFlight.Team = 1;
        noFlight.Level = 20;
        noFlight.Money = 0;
        Route walked = w.G.Plan(w.A, w.C, noFlight);
        CHECK(walked.Found && walked.Legs[0].Type == LinkType::Walk);                 // AllowAir is false by default

        RouteOptions flyer = noFlight;
        flyer.AllowAir = true;
        Route flown = w.G.Plan(w.A, w.C, flyer);
        CHECK(flown.Found && flown.Legs.size() == 1 && flown.Legs[0].Type == LinkType::Air && Near(flown.Seconds, 5000.0f / 22.0f + 12.0f));

        RouteOptions both = flyer;                // with the gold the 60 s flight path still wins
        both.Money = 1000;
        Route taxi = w.G.Plan(w.A, w.C, both);
        CHECK(taxi.Found && taxi.Legs[0].Type == LinkType::Taxi);

        flyer.Blocked = [airId](TravelLink const& link) { return link.Id == airId; };   // the A to C air link failed recently
        Route fallback = w.G.Plan(w.A, w.C, flyer);
        CHECK(fallback.Found && fallback.Legs[0].Type == LinkType::Walk);

        // the way back is its own link and is not blocked by the failure of the way there
        RouteOptions back = noFlight;
        back.AllowAir = true;
        Route home = w.G.Plan(w.C, w.A, back);
        CHECK(home.Found && home.Legs.size() == 1 && home.Legs[0].Type == LinkType::Air);

        // an air link to a node closed to the bot's team (enemy ground) is refused
        w.G.MutableNode(w.C).Team = 2;
        Route enemy = w.G.Plan(w.A, w.C, flyer);
        CHECK(!enemy.Found || enemy.Legs[0].Type != LinkType::Air);
    }

    if (Failures)
        std::printf("%d check(s) failed\n", Failures);
    else
        std::printf("travel graph tests passed\n");
    return Failures ? 1 : 0;
}
