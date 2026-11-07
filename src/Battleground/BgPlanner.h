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

#ifndef Playerbots_BgPlanner_h__
#define Playerbots_BgPlanner_h__

// The battleground team planner (docs/design/BG_PLAN.md §2): one plan per battleground instance, one team plan per side. It
// rolls each team's defence share, splits the ownerless bots into attackers and defenders, spreads the defenders over the held
// objectives, picks the attack targets and the carrier escorts. Bots read their job from it in BgPlay.
//
// Threads: the planner runs in the world thread (playerbots_bg OnUpdate, after the map updates), and battleground bots are ticked
// in the world thread too (PlayerbotMgr::TickOtherMapBots), so nothing here is locked. If battleground bot ticks ever move to a
// map thread, this API needs a lock first.

#include "Battleground/BgMapData.h"
#include "Battleground/BgRules.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Battleground;
class Creature;
class GameObject;
class Map;
class Player;

namespace Playerbots::Bg
{
using BgRules::Job;

constexpr int8 NoObjective = -1;
constexpr int8 FlagObjective = -2;      // Eye of the Storm: the flag in the middle

/// What one bot does now. Objective indexes BgPlan::Objectives (or FlagObjective)
struct Assignment
{
    Job Base = Job::None;               // what the split gave it: Attack or Defend
    Job Now = Job::None;                // Base, or Carrier / Escort for now
    int8 Objective = NoObjective;
    ObjectGuid Follow;                  // Escort: the carrier
    uint32 SinceMs = 0;                 // when Base was given
    int8 NoPathObjective = NoObjective; // the bot found no mesh path to this one: the planner gives it another for a while
    uint32 NoPathUntilMs = 0;
};

struct Objective
{
    uint32 Entry = 0;
    std::string Name;
    float Weight = 1.0f;
    ObjectGuid Guid;
    Position Pos;
    bool Known = false;                 // the game object was found once (Pos is set); Valley rows always
    TeamId Owner = TEAM_NEUTRAL;        // capture points: who captured it last and still holds it; towers: who controls it
    TeamId Assaulting = TEAM_NEUTRAL;   // capture points: the team whose assault is running (TEAM_NEUTRAL: none)

    // Valley maps: one objective per playerbots_bg_node row, same index as MapDef::Nodes (rally and gate rows included)
    bool Destroyed = false;             // a tower the other side burnt
    bool Alive = true;                  // a boss, from its world state
    bool WasAlive = true;               // the plan before
    ObjectGuid Boss;                    // the boss creature, looked up when needed
    uint32 BlockedUntilMs[2] = { 0, 0 };    // a team gave up its push here: not its target again before this
};

struct FlagInfo
{
    uint32 Entry = 0;
    ObjectGuid Guid;
    Position Home;                      // where the flag stands in its base
    bool Known = false;
    uint8 State = 0;                    // FlagState
    ObjectGuid Carrier;
    ObjectGuid Drop;                    // the dropped flag object, while it is on the ground
    Position DropPos;
};

struct TeamPlan
{
    uint64 Seed = 0;
    uint32 DefendPct = 0;
    uint32 Assignable = 0;              // ownerless bots of the team (the split moves these)
    uint32 Others = 0;                  // humans and bots with an owner: counted, never moved
    uint32 WantDefenders = 0;
    std::vector<int8> DefendTargets;
    std::vector<int8> AttackTargets;
    std::string LastReason = "match start";
    uint32 LastReasonMs = 0;
    uint32 LastChanged = 0;             // jobs or objectives changed on that event
    uint32 Reassigns = 0;               // events with at least one change
    uint32 LastRetargetMs = 0;

    // Valley maps: the push on a boss (BgRules::Rally)
    BgRules::Rally Rally;
    int8 RallyObjective = NoObjective;  // the boss
    ObjectGuid Puller;                  // the bot that pulls when the push charges
    uint32 RallyOnPush = 0;             // what the last plan saw
    uint32 RallyPresent = 0;
};

struct BgPlan
{
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    MapDef const* Def = nullptr;
    uint64 StartTime = 0;               // seconds, fixed when the plan is made (the seed uses it)
    uint32 CreatedMs = 0;
    uint32 LastTickMs = 0;
    uint32 LastTouchMs = 0;
    uint32 LastDiscoverMs = 0;
    uint32 Ticks = 0;
    bool Seeded = false;                // the teams rolled (once the match is in progress)
    std::vector<Objective> Objectives;
    FlagInfo Flags[2];                  // Flag maps: [TEAM_ALLIANCE] the Alliance flag; Eye: [0] the flag
    TeamPlan Teams[2];
    std::unordered_map<ObjectGuid, Assignment> Jobs;
    std::vector<TeamId> LastOwners;
    uint32 LastRoster[2] = { 0, 0 };
    int8 LastLeader = -1;               // 0 Alliance ahead, 1 Horde ahead, 2 level, -1 not seen yet
    uint8 LastFlagStates[2] = { 0, 0 };
    std::string LastEvent = "-";        // the last thing that changed in the match (a capture, a flag, the lead, a roster)
    uint32 LastEventMs = 0;
    uint32 CaptureCasts = 0;
    uint32 CaptureFallbacks = 0;        // the open spell failed and the bot used the object directly
    uint32 LastCastFailure = 0;         // SpellCastResult of the last failure
    uint32 CaptureGiveUps = 0;          // a bot stopped trying after interruptions in a row (it moves or fights, then tries again)
    uint32 LastGiveUpResult = 0;        // SpellCastResult of the last give-up
    uint32 LastBossLookupMs = 0;        // Valley: bosses missing a creature are looked up at most this often
};

class Planner
{
public:
    static Planner& Instance();

    /// World thread, every world tick: plans each match every Bg.Planner.TickMs, drops matches nobody touches
    void Update();

    /// Bot side: registers the match of the bot and returns its job (Base None until the first plan)
    Assignment Touch(Player* bot);
    /// Bot side: no mesh path to that objective, give the bot another for a while
    void NoteNoPath(Player* bot, int8 objective);
    /// Bot side: a capture cast was started (or failed and fell back to Use)
    void NoteCapture(Player* bot, bool fallback, uint32 failure);
    /// Bot side: the capture cast kept failing for a reason that may pass (range, line of sight, combat): the bot moves on for now
    void NoteCaptureGiveUp(Player* bot, uint32 result);

    BgPlan const* Find(uint32 mapId, uint32 instanceId) const;
    std::vector<BgPlan const*> List() const;
    /// Drops every plan (the objective rows were read again: plans index them); the next touch makes new ones
    void Reset();

private:
    void Plan(BgPlan& plan, Battleground* bg, Map* map, uint32 now);
    void Discover(BgPlan& plan, Map* map, uint32 now);
    void ReadState(BgPlan& plan, Map* map);
    void ReadValley(BgPlan& plan, Map* map, uint32 now);
    void PlanTeam(BgPlan& plan, Map* map, uint32 team, std::vector<Player*> const& members, bool retarget, std::string const& reason, uint32 now);
    void PlanValleyTeam(BgPlan& plan, Battleground* bg, Map* map, uint32 team, std::vector<Player*> const& members, bool retarget,
        std::string const& reason, uint32 now);
    void StepRally(BgPlan& plan, Map* map, uint32 team, std::vector<Player*> const& members, std::vector<Player*> const& bots, uint32& changed,
        std::string& rallyEvent, uint32 now);

    std::map<std::pair<uint32, uint32>, BgPlan> _plans;
};

/// Objective position of a job: the node, the flag, or for flag maps our or their flag base
bool ObjectivePosition(BgPlan const& plan, int8 objective, Position& out);
std::string ObjectiveName(BgPlan const& plan, int8 objective);
TeamId TeamOf(Player const* player);
/// The team's score: Battleground::GetTeamScore, or the map's score world states (Alterac Valley reinforcements)
uint32 TeamScore(BgPlan const& plan, Battleground const* bg, Map const* map, uint32 team);
/// Valley maps: the row of an objective (nullptr for the code-table maps or a bad index)
NodeDef const* RowOf(BgPlan const& plan, int8 objective);
/// Valley maps: the boss creature of a boss objective, if it is in the world
Creature* BossOf(BgPlan const& plan, Map* map, int8 objective);
/// Valley maps: the rally radius of a boss push (the row's, else Playerbots.Bg.Rally.Radius)
float RallyRadiusOf(BgPlan const& plan, int8 boss);
}

#define sBgPlanner Playerbots::Bg::Planner::Instance()

#endif
