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

#ifndef Playerbots_BgMode_h__
#define Playerbots_BgMode_h__

// The battleground coordinator's layers (docs/design/BG_COORDINATOR.md §3): a Mode offers a team tasks and plays a bot's role in
// one, an Adapter reads a map's state into the plan, a Profile puts one battleground together (objectives, modes, adapter, phases).
// The coordinator path (BgCoordinator.cpp) runs only with Playerbots.Bg.Coordinator = 1 on a map that has a profile; the planner's
// old path stays as it is (the baseline rule, §6)

#include "Battleground/BgMapData.h"
#include "Battleground/BgPlanner.h"
#include "Battleground/BgTaskRules.h"
#include <functional>
#include <vector>

class Battleground;
class Map;
class Player;

namespace Playerbots
{
class PlayerbotAI;
}

namespace Playerbots::Bg
{
using BgRules::TaskIntent;
using BgRules::TaskPhase;
using BgRules::TetherRole;

/// Where an objective is. Unit and GameObject anchors are declared for the flag maps (Warsong Gulch, backlog B2) and not yet read
enum class AnchorKind : uint8
{
    Position,
    Unit,
    GameObject
};

enum class Role : uint8
{
    None,
    Capper,
    Carrier,
    Fighter,
    Escort,
    Defender,
    Puller,
    Driver,
    Gunner
};

struct RoleSlot
{
    Role Kind = Role::None;
    uint8 Count = 0;
    bool HealerFirst = false;
};

/// A squad's staging point: a row of the map (a rally row), wait for Min living members or TimeoutMs
struct Stage
{
    int8 Row = NoObjective;
    uint8 Min = 0;
    uint32 TimeoutMs = 0;
};

/// What a mode offers a team this plan (§2.3)
struct Task
{
    int8 Objective = NoObjective;
    TaskIntent Intent = TaskIntent::Take;
    AnchorKind Anchor = AnchorKind::Position;
    float Value = 0.0f;             // V x W[phase], before travel and threat
    uint8 QuotaMin = 0;
    uint8 QuotaMax = 0;             // 0: no cap
    std::vector<RoleSlot> Roles;
    Stage Staging;
    bool FixedQuota = false;        // Hold: the mode set the quota (the guards of a running assault); else the team's defend share
    uint32 CommitMs = 0;            // 0: Playerbots.Bg.CommitSeconds
    TetherRole Combat = TetherRole::Fighter;
};

/// What a mode and a profile's phase rule may read besides the plan (world thread, valid for one plan)
struct Context
{
    Battleground* Bg = nullptr;
    Map* MatchMap = nullptr;
    uint32 Now = 0;                 // GameTime ms
    uint32 PlayMs = 0;              // Battleground::GetInProgressDuration: the match since the gates opened
};

class Mode
{
public:
    virtual ~Mode() = default;
    virtual char const* Name() const = 0;
    /// The tasks this mode offers the team now. A task it no longer offers ends: its squad is released at once
    virtual void Offer(BgPlan const& plan, Context const& context, uint32 team, TaskPhase phase, std::vector<Task>& out) const = 0;
};

/// The profile's thin state reader. The coordinator path reuses the planner's readers (ReadValley, Discover + ReadState); an
/// adapter adds what a map needs beyond them (the Alterac Valley Snowfall quirk)
class Adapter
{
public:
    virtual ~Adapter() = default;
    virtual void Read(BgPlan& plan, Battleground* bg, Map* map, uint32 now) const = 0;
};

/// One battleground for the coordinator
struct Profile
{
    uint32 MapId = 0;
    char const* Short = "";         // the Playerbots.Bg.<Short>.* section
    std::vector<Mode const*> Modes; // several modes per profile: declared, used first by the flag maps (backlog B2/B3)
    Adapter const* State = nullptr;
    std::function<TaskPhase(BgPlan const&, Context const&, uint32 team, TaskPhase current)> PhaseOf;
};

/// The profile of a map, nullptr when the coordinator does not handle it (the old path plays it, whatever the key says)
Profile const* FindProfile(uint32 mapId);

/// The lane push (BgModeLane.cpp, §3.3): Alterac Valley's banners (graveyards to take, towers to burn), holds and guards, retakes and
/// the general's push from the playerbots_bg_node rows
Mode const& LaneMode();
/// Alterac Valley's phase per team (§4): early until both towers of its push path are taken, late from two enemy towers burnt
TaskPhase AlteracValleyPhase(BgPlan const& plan, Context const& context, uint32 team, TaskPhase current);
/// The soft gate of a general (Playerbots.Bg.SoftGateMinutes of play, or the enemy score down to SoftGateScorePct of its start)
bool SoftGateOpen(BgPlan const& plan, Context const& context, uint32 team);
}

#endif
