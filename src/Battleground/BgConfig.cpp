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

#include "Battleground/BgConfig.h"
#include "PlayerbotsConfig.h"
#include "Config.h"
#include "Log.h"
#include "StringConvert.h"
#include "Util.h"
#include <algorithm>

namespace Playerbots::Bg
{
Config& GetConfig()
{
    static Config config;
    return config;
}

void Config::Load()
{
    Planner = sConfigMgr->GetBoolDefault("Playerbots.Bg.Planner", false);

    PlannerMaps.clear();
    std::string maps = sConfigMgr->GetStringDefault("Playerbots.Bg.Planner.Maps", "2106,726,2107,761,566,30");
    for (std::string_view token : Trinity::Tokenize(maps, ',', false))
        if (Optional<uint32> map = Trinity::StringTo<uint32>(token))
            PlannerMaps.push_back(*map);

    TickMs = std::max<uint32>(500, sConfigMgr->GetIntDefault("Playerbots.Bg.Planner.TickMs", 3000));
    DefendMinPct = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Bg.DefendMinPct", 20));
    DefendMaxPct = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Bg.DefendMaxPct", 40));
    if (DefendMinPct > DefendMaxPct)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: Playerbots.Bg.DefendMinPct ({}) is above Playerbots.Bg.DefendMaxPct ({}), swapped",
            DefendMinPct, DefendMaxPct);
        std::swap(DefendMinPct, DefendMaxPct);
    }
    RoleHoldSeconds = sConfigMgr->GetIntDefault("Playerbots.Bg.RoleHoldSeconds", 45);
    FillToMax = sConfigMgr->GetBoolDefault("Playerbots.Bg.FillToMax", false);
    FillMaxPerTeam = sConfigMgr->GetIntDefault("Playerbots.Bg.FillMaxPerTeam", 0);

    RallyPct = std::clamp<uint32>(sConfigMgr->GetIntDefault("Playerbots.Bg.Rally.Pct", 80), 1, 100);
    RallyGeneralFloor = std::max<uint32>(1, sConfigMgr->GetIntDefault("Playerbots.Bg.Rally.GeneralFloor", 15));
    RallyCaptainFloor = std::max<uint32>(1, sConfigMgr->GetIntDefault("Playerbots.Bg.Rally.CaptainFloor", 8));
    RallyRadius = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Bg.Rally.Radius", 30.0f));
    RallyTimeoutSeconds = std::max<uint32>(10, sConfigMgr->GetIntDefault("Playerbots.Bg.Rally.TimeoutSeconds", 120));
    RallyTimeoutChargePct = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Bg.Rally.TimeoutChargePct", 50));
    RallyWipePct = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Bg.Rally.WipePct", 30));
    RallyPullDefenders = sConfigMgr->GetBoolDefault("Playerbots.Bg.Rally.PullDefenders", true);
    RallyMaxRestarts = sConfigMgr->GetIntDefault("Playerbots.Bg.Rally.MaxRestarts", 1);
    SoftGateMinutes = sConfigMgr->GetIntDefault("Playerbots.Bg.SoftGateMinutes", 20);
    SoftGateScorePct = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Bg.SoftGateScorePct", 40));

    ThreatResponse = sConfigMgr->GetBoolDefault("Playerbots.Bg.ThreatResponse", false);
    RetakeTimers = sConfigMgr->GetBoolDefault("Playerbots.Bg.RetakeTimers", false);
    FallBack = sConfigMgr->GetBoolDefault("Playerbots.Bg.FallBack", false);
    ScoreClock = sConfigMgr->GetBoolDefault("Playerbots.Bg.ScoreClock", false);
    Endgame = sConfigMgr->GetBoolDefault("Playerbots.Bg.Endgame", false);
    FocusFire = sConfigMgr->GetBoolDefault("Playerbots.Bg.FocusFire", false);
    CarrierPlay = sConfigMgr->GetBoolDefault("Playerbots.Bg.CarrierPlay", false);
    CallOuts = sConfigMgr->GetBoolDefault("Playerbots.Bg.CallOuts", false);
    HumanCalls = sConfigMgr->GetBoolDefault("Playerbots.Bg.HumanCalls", false);
    Mines = sConfigMgr->GetBoolDefault("Playerbots.Bg.Mines", false);
    Archetypes = sConfigMgr->GetBoolDefault("Playerbots.Bg.Archetypes", false);
    TeamSkill = sConfigMgr->GetBoolDefault("Playerbots.Bg.TeamSkill", false);
    TeamSkillMin = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Bg.TeamSkillMin", 60));
    TeamSkillMax = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Bg.TeamSkillMax", 100));
    if (TeamSkillMin > TeamSkillMax)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: Playerbots.Bg.TeamSkillMin ({}) is above Playerbots.Bg.TeamSkillMax ({}), swapped",
            TeamSkillMin, TeamSkillMax);
        std::swap(TeamSkillMin, TeamSkillMax);
    }
    TeamSkillStreakStep = std::min<uint32>(50, sConfigMgr->GetIntDefault("Playerbots.Bg.TeamSkillStreakStep", 10));
    NinjaCaps = sConfigMgr->GetBoolDefault("Playerbots.Bg.NinjaCaps", false);
    LaneChange = sConfigMgr->GetBoolDefault("Playerbots.Bg.LaneChange", false);

    if (Planner && !sPlayerbotsConfig.BgObjectives)
        TC_LOG_WARN("server.playerbots", "Playerbots: Playerbots.Bg.Planner = 1 does nothing while Playerbots.Bg.Objectives = 0");
}

bool Config::Plans(uint32 mapId) const
{
    return Planner && sPlayerbotsConfig.BgObjectives && std::find(PlannerMaps.begin(), PlannerMaps.end(), mapId) != PlannerMaps.end();
}
}
