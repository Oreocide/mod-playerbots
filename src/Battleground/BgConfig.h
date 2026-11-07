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

#ifndef Playerbots_BgConfig_h__
#define Playerbots_BgConfig_h__

// The Playerbots.Bg.* keys of the team planner (docs/design/BG_PLAN.md). Kept out of PlayerbotsConfig so the planner does not
// change that header's layout; loaded by the playerbots_bg world script (Battleground/BgScripts.cpp) at start and on .reload config.
// The defaults here equal conf/playerbots.conf.dist

#include "Define.h"
#include <vector>

namespace Playerbots::Bg
{
struct Config
{
    bool Planner = false;                                   // Playerbots.Bg.Planner: the team planner plays the maps below
    std::vector<uint32> PlannerMaps = { 2106, 726, 2107, 761, 566, 30 };
    uint32 TickMs = 3000;                                   // how often each match is planned again
    uint32 DefendMinPct = 20;                               // each team rolls its defence share in this range per match
    uint32 DefendMaxPct = 40;
    uint32 RoleHoldSeconds = 45;                            // a bot keeps its job at least this long
    bool FillToMax = false;                                 // Playerbots.Bg.FillToMax: bots keep joining a running match a human is in
    uint32 FillMaxPerTeam = 0;                              // team size the fill stops at, 0 = the battleground's own maximum

    // the rally before a boss (Alterac Valley, phase 3): see conf/playerbots.conf.dist
    uint32 RallyPct = 80;                                   // charge with this share of the push present
    uint32 RallyGeneralFloor = 15;                          // or this many for a general, whichever is smaller
    uint32 RallyCaptainFloor = 8;                           // or this many for a captain
    float RallyRadius = 30.0f;                              // "present": this close to the rally point
    uint32 RallyTimeoutSeconds = 120;
    uint32 RallyTimeoutChargePct = 50;
    uint32 RallyWipePct = 30;
    bool RallyPullDefenders = true;
    uint32 RallyMaxRestarts = 1;
    uint32 SoftGateMinutes = 20;                            // a boss's "requires" stops counting this far into the match
    uint32 SoftGateScorePct = 40;                           // or once the enemy score is down to this share of its start

    void Load();
    /// Planner on, Playerbots.Bg.Objectives on, and the map in the list
    bool Plans(uint32 mapId) const;
};

Config& GetConfig();
}

#define sBgConfig Playerbots::Bg::GetConfig()

#endif
