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

#include "Battleground/BgCoordinator.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgMode.h"
#include "Log.h"
#include <map>

namespace Playerbots::Bg
{
namespace
{
using MatchKey = std::pair<uint32, uint32>;

/// The path of each match, fixed at its first plan
std::map<MatchKey, bool> Paths;
}

bool Coordinator::Coordinates(BgPlan const& plan)
{
    auto [it, created] = Paths.try_emplace({ plan.MapId, plan.InstanceId }, false);
    if (created)
    {
        it->second = sBgConfig.Coordinator && FindProfile(plan.MapId) != nullptr;
        if (it->second)
            TC_LOG_INFO("server.playerbots", "Playerbots: battleground coordinator plans map {} instance {}", plan.MapId, plan.InstanceId);
    }
    return it->second;
}

void Coordinator::Plan(Planner& /*planner*/, BgPlan& /*plan*/, Battleground* /*bg*/, Map* /*map*/, std::vector<Player*> const (&/*members*/)[2],
    std::string const& /*reason*/, uint32 /*now*/)
{
    // W4 (docs/design/BG_COORDINATOR.md §6): no map has a profile yet, so Coordinates() is false and this is never reached
}

void Coordinator::Close(BgPlan const& plan)
{
    Paths.erase({ plan.MapId, plan.InstanceId });
}
}
