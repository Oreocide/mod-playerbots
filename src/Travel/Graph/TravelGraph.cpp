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

#include "TravelGraph.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace Playerbots
{
namespace
{
float Distance2d(TravelNode const& a, TravelNode const& b)
{
    return std::hypot(a.X - b.X, a.Y - b.Y);
}

constexpr float Infinity = std::numeric_limits<float>::infinity();
}

float TravelGraph::WalkSeconds(float distance, float runSpeed)
{
    return distance * DetourFactor / std::max(runSpeed, 1.0f);
}

uint32_t TravelGraph::AddNode(TravelNode node)
{
    node.Id = uint32_t(_nodes.size());
    _nodes.push_back(std::move(node));
    _out.emplace_back();
    _finalized = false;
    return _nodes.back().Id;
}

uint32_t TravelGraph::AddLink(TravelLink link)
{
    link.Id = uint32_t(_links.size());
    _out[link.From].push_back(link.Id);
    _links.push_back(link);
    return link.Id;
}

void TravelGraph::AddWalkPair(uint32_t a, uint32_t b, float runSpeed)
{
    if (_nodes[a].Map != _nodes[b].Map)
        return;

    TravelLink link;
    link.Type = LinkType::Walk;
    link.Seconds = WalkSeconds(Distance2d(_nodes[a], _nodes[b]), runSpeed);
    link.From = a;
    link.To = b;
    AddLink(link);
    link.From = b;
    link.To = a;
    AddLink(link);
}

void TravelGraph::Finalize()
{
    _byMap.clear();
    for (TravelNode const& node : _nodes)
        _byMap[node.Map].push_back(node.Id);
    _finalized = true;
}

std::vector<uint32_t> TravelGraph::NearestNodes(uint32_t map, float x, float y, uint32_t count, float maxDistance) const
{
    std::vector<std::pair<float, uint32_t>> found;
    auto itr = _byMap.find(map);
    if (itr == _byMap.end())
        return {};

    for (uint32_t id : itr->second)
    {
        float distance = std::hypot(_nodes[id].X - x, _nodes[id].Y - y);
        if (distance <= maxDistance)
            found.emplace_back(distance, id);
    }

    size_t keep = std::min<size_t>(count, found.size());
    std::partial_sort(found.begin(), found.begin() + keep, found.end());
    std::vector<uint32_t> ids;
    ids.reserve(keep);
    for (size_t i = 0; i < keep; ++i)
        ids.push_back(found[i].second);
    return ids;
}

void TravelGraph::ConnectNeighbours(uint32_t neighbours, float maxDistance, float runSpeed)
{
    if (!_finalized)
        Finalize();

    // a pair is linked once, whichever node found the other first
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    for (TravelNode const& node : _nodes)
    {
        // +1: the node is its own nearest
        for (uint32_t other : NearestNodes(node.Map, node.X, node.Y, neighbours + 1, maxDistance))
            if (other != node.Id)
                pairs.emplace_back(std::min(node.Id, other), std::max(node.Id, other));
    }

    std::ranges::sort(pairs);
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    for (auto const& [a, b] : pairs)
        AddWalkPair(a, b, runSpeed);
}

bool TravelGraph::NodeAllowed(uint32_t node, RouteOptions const& options) const
{
    uint8_t team = node < _nodes.size() ? _nodes[node].Team : 0;
    if (!team)
        return true;

    // 3 = enemy ground for both teams; otherwise the node belongs to one team
    return team != 3 && (!options.Team || team == options.Team);
}

float TravelGraph::Cost(TravelLink const& link, RouteOptions const& options) const
{
    float cost = link.Seconds + float(link.Gold) * options.GoldSecondsPerCopper;
    if (link.Type != LinkType::Walk)
        return cost;

    uint8_t danger = options.Team == 1 ? link.DangerAlliance : options.Team == 2 ? link.DangerHorde : std::max(link.DangerAlliance, link.DangerHorde);
    int over = int(danger) - int(options.Level) - int(options.DangerFreeLevels);
    if (over > 0)
        cost += float(over) * options.DangerSecondsPerLevel;
    return cost;
}

bool TravelGraph::Allowed(TravelLink const& link, RouteOptions const& options) const
{
    if (!NodeAllowed(link.From, options) || !NodeAllowed(link.To, options))
        return false;

    if (link.Type == LinkType::Walk)
    {
        uint8_t danger = options.Team == 1 ? link.DangerAlliance : options.Team == 2 ? link.DangerHorde : std::max(link.DangerAlliance, link.DangerHorde);
        if (int(danger) - int(options.Level) >= int(options.DangerBlockLevels))
            return false;
    }

    switch (link.Type)
    {
        case LinkType::Taxi: if (!options.AllowTaxi) return false; break;
        case LinkType::Transport: if (!options.AllowTransport) return false; break;
        case LinkType::Portal: if (!options.AllowPortal) return false; break;
        case LinkType::Teleport: if (!options.AllowTeleport) return false; break;
        case LinkType::Walk: break;
    }

    if (link.Team && options.Team && link.Team != options.Team)
        return false;
    if (options.Level < link.MinLevel)
        return false;
    if (link.Gold && uint64_t(link.Gold) > options.Money)
        return false;
    if (options.Blocked && options.Blocked(link))
        return false;
    return true;
}

Route TravelGraph::Plan(uint32_t from, uint32_t to, RouteOptions const& options) const
{
    Route route;
    if (from >= _nodes.size() || to >= _nodes.size())
        return route;

    if (from == to)
    {
        route.Found = true;
        return route;
    }

    TravelNode const& goal = _nodes[to];
    auto heuristic = [&](uint32_t id)
    {
        TravelNode const& node = _nodes[id];
        // nothing is known about the distance between maps
        return node.Map == goal.Map ? Distance2d(node, goal) / FastestSpeed : 0.0f;
    };

    struct Entry
    {
        float Priority;
        uint32_t Node;
        bool operator>(Entry const& other) const { return Priority > other.Priority; }
    };

    std::vector<float> best(_nodes.size(), Infinity);
    std::vector<uint32_t> cameBy(_nodes.size(), UINT32_MAX);
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    best[from] = 0.0f;
    open.push({ heuristic(from), from });

    while (!open.empty())
    {
        Entry top = open.top();
        open.pop();
        uint32_t node = top.Node;
        if (node == to)
            break;

        // an outdated queue entry
        if (top.Priority > best[node] + heuristic(node) + 0.001f)
            continue;

        for (uint32_t linkId : _out[node])
        {
            TravelLink const& link = _links[linkId];
            if (!Allowed(link, options))
                continue;

            float next = best[node] + Cost(link, options);
            if (next < best[link.To])
            {
                best[link.To] = next;
                cameBy[link.To] = linkId;
                open.push({ next + heuristic(link.To), link.To });
            }
        }
    }

    if (best[to] == Infinity)
        return route;

    for (uint32_t node = to; node != from;)
    {
        TravelLink const& link = _links[cameBy[node]];
        route.Legs.push_back({ link.From, link.To, link.Type, link.Seconds, link.Ref, link.Id });
        route.Seconds += link.Seconds;
        node = link.From;
    }

    std::ranges::reverse(route.Legs);
    route.Found = true;
    return route;
}

Reach TravelGraph::ComputeReach(Position3 const& from, RouteOptions const& options, std::vector<TravelLink> const& fromExtra) const
{
    constexpr uint32_t Candidates = 5;
    constexpr float ReachDistance = 2500.0f;

    Reach reach;
    reach.From = from;
    reach.Cost.assign(_nodes.size(), Infinity);
    reach.Seconds.assign(_nodes.size(), Infinity);

    struct Entry
    {
        float Cost;
        uint32_t Node;
        bool operator>(Entry const& other) const { return Cost > other.Cost; }
    };
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;

    auto seed = [&](uint32_t node, float cost, float seconds)
    {
        if (cost < reach.Cost[node])
        {
            reach.Cost[node] = cost;
            reach.Seconds[node] = seconds;
            open.push({ cost, node });
        }
    };

    for (uint32_t id : NearestNodes(from.Map, from.X, from.Y, Candidates, ReachDistance))
        if (NodeAllowed(id, options))
        {
            float walk = WalkSeconds(std::hypot(_nodes[id].X - from.X, _nodes[id].Y - from.Y), options.RunSpeed);
            seed(id, walk, walk);
        }
    for (TravelLink const& extra : fromExtra)
        if (extra.To < _nodes.size() && Allowed(extra, options))
            seed(extra.To, extra.Seconds, extra.Seconds);

    while (!open.empty())
    {
        Entry top = open.top();
        open.pop();
        if (top.Cost > reach.Cost[top.Node])
            continue;

        for (uint32_t linkId : _out[top.Node])
        {
            TravelLink const& link = _links[linkId];
            if (!Allowed(link, options))
                continue;

            float next = top.Cost + Cost(link, options);
            if (next < reach.Cost[link.To])
            {
                reach.Cost[link.To] = next;
                reach.Seconds[link.To] = reach.Seconds[top.Node] + link.Seconds;
                open.push({ next, link.To });
            }
        }
    }

    return reach;
}

bool TravelGraph::ReachPosition(Reach const& reach, Position3 const& goal, RouteOptions const& options, float& cost, float& seconds) const
{
    constexpr uint32_t Candidates = 5;
    constexpr float ReachDistance = 2500.0f;

    cost = Infinity;
    seconds = Infinity;

    // walking the whole way is always an option on one map
    if (reach.From.Map == goal.Map)
    {
        float walk = WalkSeconds(std::hypot(reach.From.X - goal.X, reach.From.Y - goal.Y), options.RunSpeed);
        cost = walk;
        seconds = walk;
    }

    for (uint32_t id : NearestNodes(goal.Map, goal.X, goal.Y, Candidates, ReachDistance))
    {
        if (reach.Cost[id] == Infinity || !NodeAllowed(id, options))
            continue;

        float walk = WalkSeconds(std::hypot(_nodes[id].X - goal.X, _nodes[id].Y - goal.Y), options.RunSpeed);
        if (reach.Cost[id] + walk < cost)
        {
            cost = reach.Cost[id] + walk;
            seconds = reach.Seconds[id] + walk;
        }
    }

    return cost != Infinity;
}

Route TravelGraph::PlanPositions(Position3 const& from, Position3 const& to, RouteOptions const& options, std::vector<TravelLink> const& fromExtra) const
{
    constexpr uint32_t Candidates = 5;
    constexpr float ReachDistance = 2500.0f;

    Route best;
    best.Seconds = Infinity;

    // the start and the goal are not nodes: walk to the nearest ones, or the whole way when it is on one map and short
    if (from.Map == to.Map)
    {
        float direct = std::hypot(from.X - to.X, from.Y - to.Y);
        best.Found = true;
        best.Seconds = WalkSeconds(direct, options.RunSpeed);
    }

    std::vector<uint32_t> starts = NearestNodes(from.Map, from.X, from.Y, Candidates, ReachDistance);
    std::vector<uint32_t> ends = NearestNodes(to.Map, to.X, to.Y, Candidates, ReachDistance);

    // hearthstone and similar: a link from the start to a node of the graph (From of the extra link is ignored)
    struct Start
    {
        uint32_t Node;
        float Seconds;
        bool Extra;
        TravelLink Link;
    };
    std::vector<Start> begin;
    for (uint32_t id : starts)
        if (NodeAllowed(id, options))
            begin.push_back({ id, WalkSeconds(std::hypot(_nodes[id].X - from.X, _nodes[id].Y - from.Y), options.RunSpeed), false, {} });
    for (TravelLink const& extra : fromExtra)
        if (extra.To < _nodes.size() && Allowed(extra, options))
            begin.push_back({ extra.To, extra.Seconds, true, extra });

    for (Start const& start : begin)
    {
        for (uint32_t end : ends)
        {
            if (!NodeAllowed(end, options))
                continue;

            Route route = Plan(start.Node, end, options);
            if (!route.Found)
                continue;

            float total = start.Seconds + route.Seconds + WalkSeconds(std::hypot(_nodes[end].X - to.X, _nodes[end].Y - to.Y), options.RunSpeed);
            if (total >= best.Seconds)
                continue;

            best = route;
            best.Found = true;
            best.Seconds = total;

            // the first leg is the walk to the start node, or the extra link
            RouteLeg first;
            first.From = UINT32_MAX;
            first.To = start.Node;
            first.Type = start.Extra ? start.Link.Type : LinkType::Walk;
            first.Seconds = start.Seconds;
            first.Ref = start.Extra ? start.Link.Ref : 0;
            first.LinkId = UINT32_MAX;
            best.Legs.insert(best.Legs.begin(), first);

            // and the last one the walk from the end node to the goal
            RouteLeg last;
            last.From = end;
            last.To = UINT32_MAX;
            last.Type = LinkType::Walk;
            last.Seconds = WalkSeconds(std::hypot(_nodes[end].X - to.X, _nodes[end].Y - to.Y), options.RunSpeed);
            last.LinkId = UINT32_MAX;
            best.Legs.push_back(last);
        }
    }

    if (best.Found && best.Legs.empty())
    {
        // walking the whole way
        RouteLeg leg;
        leg.From = UINT32_MAX;
        leg.To = UINT32_MAX;
        leg.Type = LinkType::Walk;
        leg.Seconds = best.Seconds;
        leg.LinkId = UINT32_MAX;
        best.Legs.push_back(leg);
    }

    if (best.Seconds == Infinity)
        best = Route();
    return best;
}
}
