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

#include "Battleground/BgMode.h"
#include "Battleground/BgConfig.h"
#include "Map.h"
#include <algorithm>

// The lane push (docs/design/BG_COORDINATOR.md §3.3) and Alterac Valley's phases (§4). Written beside the planner's PlanValleyTeam,
// which stays as it is for the old path; the shared pieces are the planner's state readers and pure rules
namespace Playerbots::Bg
{
namespace
{
constexpr uint32 ValleyAssaultMs = 4 * 60 * 1000;  // an Alterac Valley banner converts 4 minutes after its assault (alterac_valley.cpp:165)
constexpr uint32 RetakeWalkMs = 30000;             // an assault of theirs with less than this left is not worth walking to
constexpr uint8 RetakeQuota = 6;

TeamId SideTeam(uint8 side)
{
    return TeamId(BgRules::TeamOfSide(side));
}

/// The team has done a node of a "requires" list: holds the graveyard, burnt the tower, killed the boss
bool RowDone(BgPlan const& plan, std::string const& key, TeamId team)
{
    for (size_t i = 0; i < plan.Def->Nodes.size() && i < plan.Objectives.size(); ++i)
    {
        NodeDef const& row = plan.Def->Nodes[i];
        if (row.Key != key)
            continue;
        Objective const& objective = plan.Objectives[i];
        switch (row.Kind)
        {
            case NodeKind::Capture: return objective.Owner == team;
            case NodeKind::Burn: return objective.Destroyed && objective.Owner == team;
            case NodeKind::KillCaptain:
            case NodeKind::KillGeneral: return !objective.Alive;
            default: return false;
        }
    }
    return false;
}

/// The row's value for a team: the coordinator's column, else the old path's weight (which stays the old path's: the baseline rule)
float RowValue(NodeDef const& row, TeamId team)
{
    return row.Values[team] > 0.0f ? row.Values[team] : row.Weights[team];
}

/// What holding a node of ours is worth: its row's value for our side when it is one of ours from the start, else (Snowfall) 1
float HoldValue(NodeDef const& row, TeamId us)
{
    return SideTeam(row.Side) == us ? std::max(RowValue(row, us), 0.1f) : 1.0f;
}

class Lane : public Mode
{
public:
    char const* Name() const override { return "lane"; }

    void Offer(BgPlan const& plan, Context const& context, uint32 team, TaskPhase phase, std::vector<Task>& out) const override
    {
        if (!plan.Def || plan.Def->Kind != MapKind::Valley)
            return;
        TeamId us = TeamId(team);
        TeamId them = TeamId(1 - team);
        std::vector<NodeDef> const& rows = plan.Def->Nodes;
        bool softGate = SoftGateOpen(plan, context, team);

        for (size_t i = 0; i < rows.size() && i < plan.Objectives.size(); ++i)
        {
            NodeDef const& row = rows[i];
            Objective const& objective = plan.Objectives[i];
            if (int32(objective.BlockedUntilMs[team] - context.Now) > 0)
                continue;
            float weight = row.PhaseWeights[uint8(phase)];
            Task task;
            task.Objective = int8(i);
            switch (row.Kind)
            {
                case NodeKind::Capture:
                case NodeKind::Burn:
                {
                    if (objective.Destroyed)
                        break;
                    bool ours = objective.Owner == us;
                    if (ours && objective.Assaulting == them)
                    {
                        // theirs converts 4 minutes after it began (as first seen): retake it while there is time to get there
                        uint32 running = objective.AssaultSinceMs ? context.Now - objective.AssaultSinceMs : 0;
                        if (running + RetakeWalkMs >= ValleyAssaultMs)
                            break;
                        task.Intent = TaskIntent::Retake;
                        task.Value = HoldValue(row, us) * 3.0f;
                        task.QuotaMax = RetakeQuota;
                        task.Combat = TetherRole::Defender;
                    }
                    else if (ours)
                    {
                        task.Intent = TaskIntent::Hold;
                        task.Value = HoldValue(row, us);
                        task.Combat = TetherRole::Defender;
                    }
                    else if (objective.Assaulting == us)
                    {
                        // our own running assault: a guard or two until it converts, everyone else goes on
                        uint8 guards = row.Guards ? row.Guards : uint8(sBgConfig.LaneGuards);
                        if (!guards)
                            break;
                        task.Intent = TaskIntent::Hold;
                        task.Value = HoldValue(row, us);
                        task.QuotaMin = guards;
                        task.QuotaMax = guards;
                        task.FixedQuota = true;
                        task.Combat = TetherRole::Defender;
                    }
                    else
                    {
                        // a tower is always someone's: only theirs is burnt; a graveyard of theirs or a neutral one is taken
                        if (row.Kind == NodeKind::Burn && objective.Owner != them)
                            break;
                        task.Intent = TaskIntent::Take;
                        task.Value = RowValue(row, us) * weight;
                        task.QuotaMax = row.AttackQuota ? row.AttackQuota : uint8(std::min<uint32>(sBgConfig.LaneTakeMax, 255));
                        task.Roles = { { Role::Capper, 1, false } };
                    }
                    if (task.Value > 0.0f)
                        out.push_back(task);
                    break;
                }
                case NodeKind::KillGeneral:
                {
                    if (SideTeam(row.Side) != them || !objective.Alive || row.Rally < 0 || plan.LastRoster[team] < row.MinGroup)
                        break;
                    uint32 done = 0;
                    for (std::string const& key : row.Requires.Keys)
                        done += RowDone(plan, key, us) ? 1 : 0;
                    if (!softGate && !BgRules::RequirementMet(row.Requires, done))
                        break;
                    task.Intent = TaskIntent::Push;
                    // once the soft gate is open the general is worth his row's weight whatever the phase says
                    task.Value = softGate ? std::max(RowValue(row, us), 0.1f) : RowValue(row, us) * weight;
                    task.Combat = TetherRole::Puller;
                    if (task.Value > 0.0f)
                        out.push_back(task);
                    break;
                }
                case NodeKind::KillCaptain:
                {
                    // the enemy captain: a squad with a quota that stages at the rally row before its puller goes in (owner: on by
                    // default, Playerbots.Bg.AV.Captain); its phase weights make it a mid-phase task
                    if (!sBgConfig.AvCaptain || SideTeam(row.Side) != them || !objective.Alive || row.Rally < 0)
                        break;
                    task.Intent = TaskIntent::Squad;
                    task.Value = RowValue(row, us) * weight;
                    task.QuotaMax = row.AttackQuota ? row.AttackQuota : uint8(std::min<uint32>(sBgConfig.LaneTakeMax, 255));
                    task.Staging.Row = row.Rally;
                    task.Staging.Min = uint8(std::min<uint32>(std::max<uint32>(row.MinGroup, sBgConfig.SquadStageMin), 255));
                    task.Staging.TimeoutMs = sBgConfig.SquadStageTimeoutSeconds * 1000;
                    task.Combat = TetherRole::Puller;
                    if (task.Value > 0.0f)
                        out.push_back(task);
                    break;
                }
                default:
                    break;  // mines stay the #13 errand; rally and gate rows
            }
        }
    }
};
}

Mode const& LaneMode()
{
    static Lane const lane;
    return lane;
}

bool SoftGateOpen(BgPlan const& plan, Context const& context, uint32 team)
{
    if (sBgConfig.SoftGateMinutes && context.PlayMs >= sBgConfig.SoftGateMinutes * 60000u)
        return true;
    if (!sBgConfig.SoftGateScorePct || !plan.Def || !plan.Def->ScoreMaxWs || !context.MatchMap)
        return false;
    uint32 start = uint32(std::max(0, context.MatchMap->GetWorldStateValue(int32(plan.Def->ScoreMaxWs))));
    return start && TeamScore(plan, context.Bg, context.MatchMap, 1 - team) * 100 <= start * sBgConfig.SoftGateScorePct;
}

TaskPhase AlteracValleyPhase(BgPlan const& plan, Context const& context, uint32 team, TaskPhase current)
{
    // the towers on each team's push path (docs/design/BG_COORDINATOR.md §4)
    static char const* const PushPath[2][2] = { { "iceblood_tower", "tower_point" }, { "stonehearth_bunker", "icewing" } };
    if (!plan.Def)
        return current;
    TeamId us = TeamId(team);
    TeamId them = TeamId(1 - team);
    BgRules::LanePhaseView view;
    view.PushTowers = 2;
    view.LateTowers = 2;
    view.PlayMs = context.PlayMs;
    for (size_t i = 0; i < plan.Def->Nodes.size() && i < plan.Objectives.size(); ++i)
    {
        NodeDef const& row = plan.Def->Nodes[i];
        Objective const& objective = plan.Objectives[i];
        if (row.Kind != NodeKind::Burn)
            continue;
        if ((row.Key == PushPath[team][0] || row.Key == PushPath[team][1])
            && (objective.Assaulting == us || (objective.Destroyed && objective.Owner == us)))
            ++view.PushTowersTaken;
        if (SideTeam(row.Side) == them && objective.Destroyed)
            ++view.EnemyTowersBurnt;
    }
    if (plan.Def->ScoreMaxWs && context.MatchMap)
    {
        view.EnemyStart = uint32(std::max(0, context.MatchMap->GetWorldStateValue(int32(plan.Def->ScoreMaxWs))));
        view.EnemyScore = TeamScore(plan, context.Bg, context.MatchMap, them);
    }
    BgRules::LanePhaseParams params;
    params.MidAfterMs = sBgConfig.LaneMidAfterMinutes ? sBgConfig.LaneMidAfterMinutes * 60000u : ~0u;
    params.LateScorePct = sBgConfig.SoftGateScorePct;
    params.LateAfterMs = sBgConfig.SoftGateMinutes ? sBgConfig.SoftGateMinutes * 60000u : ~0u;
    return BgRules::LanePhase(current, view, params);
}
}
