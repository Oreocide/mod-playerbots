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

#include "Battleground/BgPlay.h"
#include "Battleground/BgConfig.h"
#include "Battleground/BgCoordinator.h"
#include "Battleground/BgPlanner.h"
#include "Battleground/BgReact.h"
#include "Battleground/BgReactRules.h"
#include "Db/PlayerbotRepository.h"
#include "Engine/AiRegistry.h"
#include "PlayerbotAI.h"
#include "Travel/MoveToward.h"
#include "Battleground.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "Player.h"
#include "Spell.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <unordered_map>

namespace Playerbots::Bg
{
namespace
{
constexpr float GuardRange = 40.0f;         // a defender or an attacker at its objective fights enemies this close to it
constexpr float CapRange = 15.0f;           // from here a bot heads for the object to capture it
constexpr float EnemyNearCap = 20.0f;       // enemies this close to a node are fought before anyone starts a capture
constexpr float HuntRange = 80.0f;          // attackers this close to the enemy flag carrier go after it
constexpr uint8 FailuresBeforeUse = 3;      // open-spell failures in a row (not interruptions) before the bot uses the object directly
constexpr uint8 TransientsBeforeGiveUp = 5; // interrupted or refused for a passing reason this often in a row: move or fight first
constexpr float BannerRange = 8.0f;         // Valley: a node's banners stand this close to its row position
constexpr float RallyGuard = 20.0f;         // Valley: a gathering push fights only enemies this close to its rally point
constexpr float WalkInFrom = 45.0f;         // Valley: farther from the boss than this, a bot on the push walks in first (IsValidEnemy: 60 yd)
constexpr float HealerStandOff = 25.0f;     // Valley: healers on a push stay this far from the boss's target
constexpr float LeashReach = 40.0f;         // Valley: the leash rule holds bots this far outside the leash circle; farther out they fight
constexpr float BrawlerRange = 50.0f;       // #12: a brawler fights enemies this close to it on the way
constexpr float RoadFighterRange = 55.0f;   // #12: a road fighter a little farther (PlayerbotAI::IsValidEnemy: 60 yd)
constexpr float StickyGroupRadius = 20.0f;  // #12: a sticky healer counts allies this close together as a group
constexpr float NinjaSneakFrom = 60.0f;     // #17: the ninja stealths this far from its node and walks the rest
constexpr uint32 NinjaStealth = 1784;       // #17: rogue Stealth
constexpr uint32 NinjaProwl = 5215;         // #17: druid Prowl, in Cat Form
constexpr uint32 NinjaCatForm = 768;
constexpr uint32 RebuffHoldMs = 6000;       // Bg.Rebuff: how long a bot back from a graveyard waits for its buffs
constexpr float LegArrive = 3.0f;           // Bg.LegYards: a leg ends this close to its corner, the next one starts there
bool IsOpening(Unit const* unit)
{
    Spell const* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell)
        return false;
    uint32 id = spell->GetSpellInfo()->Id;
    return id == OpenSpellForLock(2178) || id == OpenSpellForLock(1479);
}

bool Transient(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_FAILED_SPELL_IN_PROGRESS:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_INTERRUPTED:
        case SPELL_FAILED_INTERRUPTED_COMBAT:
        case SPELL_FAILED_NOT_READY:
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_LINE_OF_SIGHT:
        case SPELL_FAILED_AFFECTING_COMBAT:
        case SPELL_FAILED_CASTER_DEAD:
        case SPELL_FAILED_NOT_MOUNTED:
        case SPELL_FAILED_NOT_SHAPESHIFT:
        case SPELL_FAILED_STUNNED:
        case SPELL_FAILED_SILENCED:
        case SPELL_FAILED_PACIFIED:
        case SPELL_FAILED_CONFUSED:
        case SPELL_FAILED_FLEEING:
        case SPELL_FAILED_CHEST_IN_USE:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_FAILED_DONT_REPORT:
        case SPELL_FAILED_CASTER_AURASTATE:
            return true;
        default:
            return false;
    }
}

/// Along the navmesh only (never a straight line): mounts for long runs, tells the planner when there is no path
bool Go(Player* bot, Position const& where, float arrive, int8 objective)
{
    UpdateMountForTravel(bot, bot->GetExactDist(where));
    // Bg.LegYards: a far target in legs along the mesh route (a running leg is not planned again); a winding route that is short
    // in a straight line gets legs too once the one mesh path to it failed
    bool legs = sBgConfig.LegYards && !IsMovingOnFoot(bot);
    if (legs && bot->GetExactDist2d(where) > float(sBgConfig.LegYards))
        if (Optional<Position> leg = NextMeshLeg(bot, where, float(sBgConfig.LegYards)))
        {
            Coordinator::NoteLeg(bot, bot->GetPosition(), *leg);   // the coordinator's tether measures fights against it (key off: nothing)
            if (MoveAlongMesh(bot, *leg, LegArrive) != MoveResult::NoPath)
                return true;
        }
    if (MoveAlongMesh(bot, where, arrive) != MoveResult::NoPath)
        return true;
    if (legs && bot->GetExactDist2d(where) <= float(sBgConfig.LegYards))
        if (Optional<Position> leg = NextMeshLeg(bot, where, float(sBgConfig.LegYards)))
        {
            Coordinator::NoteLeg(bot, bot->GetPosition(), *leg);
            if (MoveAlongMesh(bot, *leg, LegArrive) != MoveResult::NoPath)
                return true;
        }

    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} has no path in battleground map {} and does not run straight", bot->GetName(), bot->GetMapId());
    sPlayerbotRepository->Remember(bot->GetGUID(), "badspot", std::to_string(bot->GetMapId()),
        Trinity::StringFormat("{:.0f} {:.0f}", where.GetPositionX(), where.GetPositionY()));
    if (objective != NoObjective)
        sBgPlanner.NoteNoPath(bot, objective);
    return false;
}

/// The nearest enemy player within range of where becomes the bot's target. False when there is none
bool Engage(Player* bot, PlayerbotAI& ai, Position const& where, float range)
{
    Player* best = nullptr;
    float bestDist = range * range;
    for (MapReference const& ref : bot->GetMap()->GetPlayers())
    {
        Player* other = ref.GetSource();
        if (!other || other == bot || !other->IsAlive() || !bot->IsValidAttackTarget(other) || !bot->CanSeeOrDetect(other))
            continue;
        float dist = other->GetExactDistSq(where);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = other;
        }
    }
    if (!best)
        return false;
    ai.SetForcedTarget(best->GetGUID());
    return true;
}

bool EnemyNear(Player* bot, Position const& where, float range)
{
    for (MapReference const& ref : bot->GetMap()->GetPlayers())
    {
        Player* other = ref.GetSource();
        if (other && other->IsAlive() && bot->IsValidAttackTarget(other) && other->GetExactDist2d(where) < range)
            return true;
    }
    return false;
}

/// A teammate is already casting the open spell on this object
bool TeammateOpening(Player* bot, GameObject const* go)
{
    TeamId team = TeamOf(bot);
    for (MapReference const& ref : bot->GetMap()->GetPlayers())
    {
        Player* other = ref.GetSource();
        if (!other || other == bot || !other->IsAlive() || TeamOf(other) != team || other->GetExactDist2d(go) > 15.0f)
            continue;
        if (Spell const* spell = other->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (IsOpening(other) && spell->m_targets.GetGOTargetGUID() == go->GetGUID())
                return true;
    }
    return false;
}

/// Uses a battleground object as a click does: the open spell of its lock when it has one (156098 "Capturing" on lock 2178),
/// else the object itself. False: not close enough (in the spell's range), or the cast was refused TransientsBeforeGiveUp times in
/// a row for a reason that may pass (range, line of sight, combat): the caller moves closer or fights, so the bot never stands
/// there for good. After FailuresBeforeUse failures in a row that are not passing ones, the object is used directly as the pilot
/// did. The planner counts both (.bot bg)
bool Interact(Player* bot, PlayerbotAI& ai, GameObject* go)
{
    uint32 spellId = OpenSpellForLock(go->GetGOInfo()->GetLockId());
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE) : nullptr;
    if (spellId && !spellInfo)
        spellId = 0;
    if (!go->IsAtInteractDistance(bot, spellInfo))
        return false;
    if (IsOpening(bot))
        return true;

    if (bot->IsMounted())
        bot->CancelMountAura();

    if (!spellId)
    {
        go->Use(bot);
        return true;
    }

    static std::unordered_map<ObjectGuid, uint8> failures;
    static std::unordered_map<ObjectGuid, uint8> transients;
    if (failures.size() > 4096)
        failures.clear();
    if (transients.size() > 4096)
        transients.clear();

    ai.ClearForcedTarget();
    bot->StopMoving();
    SpellCastResult result = bot->CastSpell(go, spellId, CastSpellExtraArgs(TRIGGERED_NONE));
    if (result == SPELL_CAST_OK)
    {
        failures.erase(bot->GetGUID());
        transients.erase(bot->GetGUID());
        sBgPlanner.NoteCapture(bot, false, 0);
        return true;
    }
    if (Transient(result))
    {
        uint8& count = transients[bot->GetGUID()];
        if (++count < TransientsBeforeGiveUp)
            return true;
        count = 0;
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} gives up casting {} on {} for now ({})", bot->GetName(), spellId, go->GetEntry(), uint32(result));
        sBgPlanner.NoteCaptureGiveUp(bot, uint32(result));
        return false;
    }
    transients.erase(bot->GetGUID());

    uint8& count = failures[bot->GetGUID()];
    if (++count < FailuresBeforeUse)
        return true;
    count = 0;

    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} could not cast {} on {} ({}): uses it directly", bot->GetName(), spellId, go->GetEntry(), uint32(result));
    sBgPlanner.NoteCapture(bot, true, uint32(result));
    if (go->GetGoType() == GAMEOBJECT_TYPE_CAPTURE_POINT)
        go->AssaultCapturePoint(bot);
    else
        go->Use(bot);
    return true;
}

Player* PlayerOf(Map* map, ObjectGuid const& guid)
{
    return guid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(map, guid);
}

// #14, further down: where our carrier goes, and a kite spot without a mesh path
bool CarrierGoal(Player* bot, BgPlan const& plan, Position& out, float& arrive, int& kite);
void MarkBadSpot(Player* bot, int index);

/// Healers walk with the team: the carrier of their team, else the nearest teammate on the same objective, else the objective
bool PlayHealer(Player* bot, BgPlan const& plan, Assignment const& job, Position const& objective)
{
    Map* map = bot->GetMap();
    Player* follow = PlayerOf(map, job.Follow);
    if (!follow)
    {
        float bestDist = 50.0f * 50.0f;
        TeamId team = TeamOf(bot);
        for (MapReference const& ref : map->GetPlayers())
        {
            Player* other = ref.GetSource();
            // never another healer: two healers that pick each other once the fighters ran ahead stand together for the whole match
            if (!other || other == bot || !other->IsAlive() || TeamOf(other) != team || IsHealer(other))
                continue;
            auto it = plan.Jobs.find(other->GetGUID());
            if (it != plan.Jobs.end() && it->second.Objective != job.Objective)
                continue;
            float dist = bot->GetExactDistSq(other);
            if (dist < bestDist)
            {
                bestDist = dist;
                follow = other;
            }
        }
    }
    // #12: a sticky healer stays with our carrier, else with the biggest group of the team
    if (job.Style == BgRules::Archetype::StickyHealer && job.Follow.IsEmpty() && !plan.Picture.empty())
    {
        TeamId team = TeamOf(bot);
        ObjectGuid best;
        uint32 most = 0;
        for (Seen const& seen : plan.Picture)
        {
            if (!seen.Alive || seen.Team != team || seen.Guid == bot->GetGUID())
                continue;
            uint32 around = seen.Carrier ? 1000 : 0;
            for (Seen const& other : plan.Picture)
                if (other.Alive && other.Team == team && other.Guid != seen.Guid && other.Pos.GetExactDist2d(seen.Pos) <= StickyGroupRadius)
                    ++around;
            if (around > most)
            {
                most = around;
                best = seen.Guid;
            }
        }
        if (Player* group = PlayerOf(map, best))
            follow = group;
    }
    if (follow)
        return Go(bot, follow->GetPosition(), 10.0f, NoObjective);
    return Go(bot, objective, 12.0f, job.Objective);
}

bool PlayFlagMap(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job)
{
    Map* map = bot->GetMap();
    TeamId us = TeamOf(bot);
    TeamId them = us == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    FlagInfo const& ourFlag = plan.Flags[us];
    FlagInfo const& theirFlag = plan.Flags[them];
    Position ourBase, theirBase;
    if (!ObjectivePosition(plan, int8(us), ourBase) || !ObjectivePosition(plan, int8(them), theirBase))
        return false;

    Player* enemyCarrier = PlayerOf(map, ourFlag.Carrier);
    if (enemyCarrier && !enemyCarrier->IsAlive())
        enemyCarrier = nullptr;
    bool healer = ai.GetRole() == BotRole::Healer;

    if (job.Now == Job::Carrier)
    {
        // home to our flag; the capture counts once our own flag is back in its base, so the carrier waits there (with
        // Playerbots.Bg.CarrierPlay it kites in the room while our flag is out)
        ai.ClearForcedTarget();
        Position goal = ourBase;
        float arrive = 3.0f;
        int kite = -1;
        CarrierGoal(bot, plan, goal, arrive, kite);
        if (!Go(bot, goal, arrive, kite < 0 ? int8(us) : NoObjective))
            MarkBadSpot(bot, kite);
        return true;
    }

    if (job.Now == Job::Escort)
    {
        if (Player* carrier = PlayerOf(map, job.Follow))
        {
            if (healer)
                return PlayHealer(bot, plan, job, carrier->GetPosition());
            Engage(bot, ai, carrier->GetPosition(), 30.0f);
            Go(bot, carrier->GetPosition(), 6.0f, NoObjective);
            return true;
        }
    }

    // our flag on the ground: return it (defenders always, attackers close to it)
    if (ourFlag.State == uint8(FlagState::Dropped) && !ourFlag.Drop.IsEmpty() && (job.Base == Job::Defend || bot->GetExactDist2d(ourFlag.DropPos) < 40.0f))
        if (GameObject* drop = map->GetGameObject(ourFlag.Drop))
        {
            if (Interact(bot, ai, drop))
                return true;
            Go(bot, drop->GetPosition(), 1.5f, NoObjective);
            return true;
        }

    if (job.Base == Job::Defend)
    {
        // our flag is out: the defenders hunt its carrier (their healers go with them), else they hold the flag room
        if (healer)
            return PlayHealer(bot, plan, job, ourBase);
        if (enemyCarrier)
        {
            ai.SetForcedTarget(enemyCarrier->GetGUID());
            Go(bot, enemyCarrier->GetPosition(), 5.0f, NoObjective);
            return true;
        }
        Engage(bot, ai, ourBase, GuardRange);
        Go(bot, ourBase, 12.0f, int8(us));
        return true;
    }

    // attackers
    if (theirFlag.State == uint8(FlagState::InBase))
    {
        if (!healer)
            if (GameObject* flag = map->GetGameObject(theirFlag.Guid))
                if (Interact(bot, ai, flag))
                    return true;
        if (healer)
            return PlayHealer(bot, plan, job, theirBase);
        Engage(bot, ai, theirBase, 30.0f);
        Go(bot, theirBase, 1.5f, int8(them));
        return true;
    }

    if (theirFlag.State == uint8(FlagState::Dropped) && !theirFlag.Drop.IsEmpty())
        if (GameObject* drop = map->GetGameObject(theirFlag.Drop))
        {
            if (!healer && Interact(bot, ai, drop))
                return true;
            Go(bot, drop->GetPosition(), 1.5f, NoObjective);
            return true;
        }

    // their flag is with our carrier: hunt theirs when ours is out, else stay near our carrier
    if (enemyCarrier && bot->GetExactDist2d(enemyCarrier) < HuntRange * 2)
    {
        ai.SetForcedTarget(enemyCarrier->GetGUID());
        Go(bot, enemyCarrier->GetPosition(), 5.0f, NoObjective);
        return true;
    }
    if (Player* carrier = PlayerOf(map, theirFlag.Carrier))
    {
        if (healer)
            return PlayHealer(bot, plan, job, carrier->GetPosition());
        Engage(bot, ai, carrier->GetPosition(), GuardRange);
        Go(bot, carrier->GetPosition(), 15.0f, NoObjective);
        return true;
    }

    // respawning: wait at their flag room
    Engage(bot, ai, theirBase, GuardRange);
    Go(bot, theirBase, 10.0f, int8(them));
    return true;
}

bool PlayNode(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job)
{
    Position pos;
    if (!ObjectivePosition(plan, job.Objective, pos))
        return false;

    bool healer = ai.GetRole() == BotRole::Healer;
    if (healer)
        return PlayHealer(bot, plan, job, pos);

    Objective const& objective = plan.Objectives[job.Objective];
    GameObject* node = objective.Guid.IsEmpty() ? nullptr : bot->GetMap()->GetGameObject(objective.Guid);
    if (node && node->GetGoType() == GAMEOBJECT_TYPE_CAPTURE_POINT && node->CanInteractWithCapturePoint(bot) && bot->GetExactDist2d(pos) < CapRange)
    {
        // fight first: a capture is a 6 s cast any hit breaks
        if (EnemyNear(bot, pos, EnemyNearCap) && Engage(bot, ai, pos, EnemyNearCap))
        {
            Go(bot, pos, 6.0f, job.Objective);
            return true;
        }
        // one capper per node, the others guard it
        if (!TeammateOpening(bot, node))
        {
            if (Interact(bot, ai, node))
                return true;
            Engage(bot, ai, pos, EnemyNearCap);
            Go(bot, pos, 1.0f, job.Objective);
            return true;
        }
    }

    Engage(bot, ai, pos, GuardRange);
    Go(bot, pos, job.Base == Job::Defend ? 8.0f : 6.0f, job.Objective);
    return true;
}

bool PlayEye(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job)
{
    Map* map = bot->GetMap();
    TeamId us = TeamOf(bot);
    FlagInfo const& flag = plan.Flags[0];
    bool healer = ai.GetRole() == BotRole::Healer;

    if (job.Now == Job::Carrier)
    {
        // to the nearest tower we hold; the score counts at its capture point
        int8 best = NoObjective;
        float bestDist = 0.0f;
        for (size_t i = 0; i < plan.Objectives.size(); ++i)
        {
            Objective const& tower = plan.Objectives[i];
            if (!tower.Known || tower.Owner != us)
                continue;
            float dist = bot->GetExactDist2d(tower.Pos);
            if (best == NoObjective || dist < bestDist)
            {
                best = int8(i);
                bestDist = dist;
            }
        }
        ai.ClearForcedTarget();
        if (best == NoObjective)
            return false;
        Go(bot, plan.Objectives[best].Pos, 3.0f, best);
        return true;
    }

    if (job.Now == Job::Escort)
        if (Player* carrier = PlayerOf(map, job.Follow))
        {
            if (healer)
                return PlayHealer(bot, plan, job, carrier->GetPosition());
            Engage(bot, ai, carrier->GetPosition(), 30.0f);
            Go(bot, carrier->GetPosition(), 6.0f, NoObjective);
            return true;
        }

    // the enemy carries the flag: attackers near him go after him
    Player* carrier = PlayerOf(map, flag.Carrier);
    if (job.Base == Job::Attack && carrier && carrier->IsAlive() && TeamOf(carrier) != us && bot->GetExactDist2d(carrier) < HuntRange)
    {
        ai.SetForcedTarget(carrier->GetGUID());
        Go(bot, carrier->GetPosition(), 5.0f, NoObjective);
        return true;
    }

    if (job.Objective == FlagObjective)
    {
        Position pos;
        if (!ObjectivePosition(plan, FlagObjective, pos))
            return false;
        if (!healer)
        {
            ObjectGuid guid = flag.State == uint8(FlagState::Dropped) ? flag.Drop : flag.Guid;
            if (GameObject* go = guid.IsEmpty() ? nullptr : map->GetGameObject(guid))
            {
                if (EnemyNear(bot, pos, EnemyNearCap) && Engage(bot, ai, pos, EnemyNearCap))
                {
                    Go(bot, pos, 5.0f, NoObjective);
                    return true;
                }
                if (Interact(bot, ai, go))
                    return true;
            }
        }
        Go(bot, pos, 1.5f, NoObjective);
        return true;
    }

    Position pos;
    if (!ObjectivePosition(plan, job.Objective, pos))
        return false;
    if (healer)
        return PlayHealer(bot, plan, job, pos);
    // towers are control zones: standing in them is the capture
    Engage(bot, ai, pos, GuardRange);
    Go(bot, pos, job.Base == Job::Defend ? 10.0f : 6.0f, job.Objective);
    return true;
}

/// The Alterac Valley banner scripts (go_av_capturable_object, go_av_contested_object), per entry
bool IsBanner(GameObject const* go)
{
    static std::unordered_map<uint32, bool> banners;
    auto it = banners.find(go->GetEntry());
    if (it == banners.end())
    {
        std::string const& script = sObjectMgr->GetScriptName(go->GetScriptId());
        it = banners.emplace(go->GetEntry(), script == "go_av_capturable_object" || script == "go_av_contested_object").first;
    }
    return it->second;
}

/// The banner of a node a player can click now: in world, spawned, ready (the script ignores any other state). Several banners
/// stand on the spot; spawn groups keep only the one of the node's state in the world
GameObject* FindBanner(Player* bot, Position const& node)
{
    struct BannerCheck
    {
        Position Node;
        bool operator()(GameObject* go) const
        {
            return go->IsInWorld() && go->isSpawned() && go->GetGoState() == GO_STATE_READY && go->GetExactDist2d(Node) <= BannerRange && IsBanner(go);
        }
    };
    std::vector<GameObject*> found;
    BannerCheck check{ node };
    Trinity::GameObjectListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, bot->GetExactDist2d(node) + BannerRange);
    GameObject* best = nullptr;
    for (GameObject* go : found)
        if (!best || go->GetExactDist2d(node) < best->GetExactDist2d(node))
            best = go;
    return best;
}

/// A bot on a boss push: waits at the rally point, the puller goes first, then the push fights the boss (BgRules::Rally)
bool PlayPush(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job, NodeDef const& row)
{
    using BgRules::RallyState;
    TeamPlan const& teamPlan = plan.Teams[TeamOf(bot)];
    Creature* boss = BossOf(plan, bot->GetMap(), job.Objective);
    Position const& rally = plan.Objectives[row.Rally].Pos;
    bool healer = ai.GetRole() == BotRole::Healer;
    RallyState state = teamPlan.RallyObjective == job.Objective ? teamPlan.Rally.State : RallyState::Gathering;
    ObjectGuid puller = teamPlan.Puller;
    // a coordinated captain squad runs its own push (BgCoordinator.h); else the team's, as before
    Coordinator::SquadPush(plan, TeamOf(bot), job.Objective, state, puller);

    auto atRally = [&]
    {
        // the boss is not pulled from here: only who comes to the rally point is fought
        ai.ClearForcedTarget();
        Engage(bot, ai, rally, RallyGuard);
        float stop = BgRules::StopDistance(bot->GetGUID().GetCounter(), 3.0f, std::max(4.0f, RallyRadiusOf(plan, job.Objective) * 0.4f));
        if (bot->GetExactDist2d(rally) > stop)
            Go(bot, rally, stop, job.Objective);
        return true;
    };

    switch (state)
    {
        case RallyState::Charging:
            if (bot->GetGUID() != puller)
                return atRally();
            [[fallthrough]];
        case RallyState::Fighting:
        {
            if (!boss || !boss->IsAlive())
                return atRally();
            if (healer)
            {
                Unit* tank = boss->GetVictim();
                Position where = tank ? tank->GetPosition() : boss->GetPosition();
                if (bot->GetExactDist2d(where) > HealerStandOff)
                    Go(bot, where, HealerStandOff, NoObjective);
                return true;
            }
            // the attack action takes the boss once it is in reach (PlayerbotAI::IsValidEnemy: 60 yd); farther, the bot walks in
            ai.SetForcedTarget(boss->GetGUID());
            if (bot->GetExactDist(boss) > WalkInFrom)
                Go(bot, boss->GetPosition(), WalkInFrom - 15.0f, NoObjective);
            return true;
        }
        default:
            return atRally();
    }
}

/// Alterac Valley: graveyards and towers are clicked (21651 "Opening" on the banner, one capper), held, retaken; bosses are pushed
bool PlayValley(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job)
{
    NodeDef const* row = RowOf(plan, job.Objective);
    if (!row)
        return false;
    if (row->IsBoss())
        return row->Rally >= 0 ? PlayPush(bot, ai, plan, job, *row) : false;

    Objective const& objective = plan.Objectives[job.Objective];
    TeamId us = TeamOf(bot);
    TeamId them = us == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    Position const& pos = objective.Pos;
    bool healer = ai.GetRole() == BotRole::Healer;

    // something to click: theirs or neutral that none of ours is taking (an assault), ours that they are taking (a defence)
    bool clickable = !objective.Destroyed
        && ((objective.Owner != us && objective.Assaulting != us) || (objective.Owner == us && objective.Assaulting == them));
    // a road fighter (#12) fights at the node and leaves the banner to the others
    if (!healer && clickable && job.Style != BgRules::Archetype::RoadFighter && bot->GetExactDist2d(pos) < CapRange)
    {
        // fight first: the 8 s "Opening" breaks on any hit
        if (EnemyNear(bot, pos, EnemyNearCap) && Engage(bot, ai, pos, EnemyNearCap))
        {
            Go(bot, pos, 6.0f, job.Objective);
            return true;
        }
        if (GameObject* banner = FindBanner(bot, pos); banner && !TeammateOpening(bot, banner))
        {
            if (Interact(bot, ai, banner))
                return true;
            Go(bot, banner->GetPosition(), 1.0f, job.Objective);
            return true;
        }
    }

    if (healer)
        return PlayHealer(bot, plan, job, pos);
    Engage(bot, ai, pos, GuardRange);
    Go(bot, pos, job.Base == Job::Defend ? 8.0f : 6.0f, job.Objective);
    return true;
}

/// While the gates are closed: to the team's gate row (a map without one: the bot waits where it stands)
void WaitAtGate(Player* bot)
{
    MapDef const* def = FindMap(bot->GetMapId());
    if (!def)
        return;
    uint8 team = uint8(TeamOf(bot));
    for (NodeDef const& row : def->Nodes)
    {
        if (row.Kind != NodeKind::Gate || BgRules::TeamOfSide(row.Side) != team)
            continue;
        float stop = BgRules::StopDistance(bot->GetGUID().GetCounter(), 6.0f, std::max(6.0f, row.Radius));
        if (bot->GetExactDist2d(row.Pos) > stop + 1.0f)
            Go(bot, row.Pos, stop, NoObjective);
        return;
    }
}

/// What the leash rule needs while a push fights its boss, or regroups after a wipe
struct LeashView
{
    Creature* Boss = nullptr;
    Position Home;
    float Radius = 0.0f;
    bool Regroup = false;
    Position Rally;
};

bool LeashOf(PlayerbotAI& ai, LeashView& out)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || !bot->IsInCombat() || ai.GetMaster() || !Plays(bot))
        return false;
    BgPlan const* plan = sBgPlanner.Find(bot->GetMapId(), bot->GetInstanceId());
    TeamId team = TeamOf(bot);
    if (!plan || !plan->Def || plan->Def->Kind != MapKind::Valley || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
        return false;
    auto it = plan->Jobs.find(bot->GetGUID());
    if (it == plan->Jobs.end())
        return false;
    Assignment const& job = it->second;
    TeamPlan const& teamPlan = plan->Teams[team];
    using BgRules::RallyState;
    RallyState state = teamPlan.Rally.State;
    ObjectGuid puller = teamPlan.Puller;
    // a coordinated captain squad runs its own push (BgCoordinator.h)
    bool squad = job.Objective != teamPlan.RallyObjective && Coordinator::SquadPush(*plan, team, job.Objective, state, puller);
    if (job.Base != Job::Attack || job.Objective == NoObjective || (job.Objective != teamPlan.RallyObjective && !squad))
        return false;
    // while the push charges only the puller is at the boss; the rest stay at the rally point and fight there
    if (state != RallyState::Fighting && state != RallyState::Regroup && !(state == RallyState::Charging && bot->GetGUID() == puller))
        return false;
    NodeDef const* row = RowOf(*plan, job.Objective);
    Creature* boss = BossOf(*plan, bot->GetMap(), job.Objective);
    if (!row || row->LeashRadius <= 0.0f || row->Rally < 0 || !boss || !boss->IsAlive())
        return false;
    // a bot far from the boss (on its way from a graveyard, attacked on the road) fights like any other: the rule is for the boss room
    if (bot->GetExactDist2d(boss->GetHomePosition()) > row->LeashRadius + LeashReach)
        return false;
    out.Boss = boss;
    out.Home = boss->GetHomePosition();
    out.Radius = row->LeashRadius;
    out.Regroup = state == RallyState::Regroup;
    out.Rally = plan->Objectives[row->Rally].Pos;
    return true;
}

/// In combat on a push: the bot or its target is outside the boss's leash circle (nobody chases out of it), or the push regroups
bool LeashTrigger(PlayerbotAI& ai)
{
    LeashView leash;
    if (!LeashOf(ai, leash))
        return false;
    Player* bot = ai.GetBot();
    if (leash.Regroup)
        return bot->GetExactDist2d(leash.Rally) > 10.0f;
    Unit* victim = bot->GetVictim();
    if (victim && victim != leash.Boss && victim->GetExactDist2d(leash.Home) > leash.Radius)
        return true;
    return bot->GetExactDist2d(leash.Home) > leash.Radius + 10.0f;
}

class LeashAction : public Action
{
public:
    char const* GetName() const override { return "bg leash"; }

    bool Execute(PlayerbotAI& ai) override
    {
        LeashView leash;
        if (!LeashOf(ai, leash))
            return false;
        Player* bot = ai.GetBot();

        // a wipe: everybody back to the rally point together, nobody stays in alone
        if (leash.Regroup)
        {
            ai.ClearForcedTarget();
            bot->AttackStop();
            return Go(bot, leash.Rally, 5.0f, NoObjective);
        }

        // the boss again; a target outside the leash is dropped, a bot outside it walks back in
        ai.SetForcedTarget(leash.Boss->GetGUID());
        if (Unit* victim = bot->GetVictim(); victim && victim != leash.Boss && victim->GetExactDist2d(leash.Home) > leash.Radius)
            bot->AttackStop();
        if (bot->GetExactDist2d(leash.Home) > leash.Radius + 10.0f)
            Go(bot, leash.Home, leash.Radius * 0.5f, NoObjective);
        return true;
    }
};

/// The coordinator's tether (§2.7, Playerbots.Bg.Tether): the bot's target is outside the zone of its task and did not hit it lately
bool TetherTrigger(PlayerbotAI& ai)
{
    return Coordinator::TetherWants(ai);
}

class TetherAction : public Action
{
public:
    char const* GetName() const override { return "bg tether"; }

    bool Execute(PlayerbotAI& ai) override
    {
        ObjectGuid enemy;
        uint32 ignoreMs = 0;
        if (!Coordinator::TakeTetherDrop(ai, enemy, ignoreMs))
            return false;
        // ignored for a while (doubled for each drop of the same enemy again), so the raid's attacker pickup does not take it back at
        // once; with no target left the battleground action runs again and the bot goes on with its task
        ai.IgnoreEnemy(enemy, ignoreMs);
        ai.ClearForcedTarget();
        ai.GetBot()->AttackStop();
        return true;
    }
};

/// The planner's job of a bot in combat on a planned map (none outside one, or for a bot with an owner)
Assignment const* CombatJob(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || !bot->IsInCombat() || ai.GetMaster() || !Plays(bot))
        return nullptr;
    BgPlan const* plan = sBgPlanner.Find(bot->GetMapId(), bot->GetInstanceId());
    if (!plan)
        return nullptr;
    auto it = plan->Jobs.find(bot->GetGUID());
    return it != plan->Jobs.end() ? &it->second : nullptr;
}

/// #8 in combat: an outnumbered bot runs to friends instead of fighting on (the planner picked the place and the time)
bool FallBackTrigger(PlayerbotAI& ai)
{
    if (!sBgConfig.FallBack)
        return false;
    Assignment const* job = CombatJob(ai);
    return job && job->Over == BgRules::Overlay::FallBack && int32(job->FallBackUntilMs - GameTime::GetGameTimeMS()) > 0
        && ai.GetBot()->GetExactDist2d(job->FallBackTo) > 6.0f;
}

class FallBackAction : public Action
{
public:
    char const* GetName() const override { return "bg fall back"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Assignment const* job = CombatJob(ai);
        if (!job || job->Over != BgRules::Overlay::FallBack)
            return false;
        Player* bot = ai.GetBot();
        Position to = job->FallBackTo;
        ai.ClearForcedTarget();
        bot->AttackStop();
        return Go(bot, to, 5.0f, NoObjective);
    }
};

// ---- #7 focus fire and #14 peel: in combat, a better target than the one the bot has ----

constexpr float FocusRange = 40.0f;
constexpr float CarrierNearRange = 20.0f;
constexpr float HealerReachMelee = 15.0f;
constexpr float HealerReachRanged = 30.0f;
constexpr float FinishRange = 30.0f;
constexpr float FinishHealthPct = 35.0f;
constexpr float AssistRange = 30.0f;
constexpr uint32 FocusSwitchMs = 4000;      // a bot changes its focus target no more often than this

/// What focus fire decided for one bot: the unit it set as the forced target, and the decision of the last check
struct FocusState
{
    ObjectGuid Target;          // the forced target focus set (and may clear again)
    uint32 SwitchMs = 0;
    ObjectGuid Pending;         // the check found a better unit
    bool ClearPending = false;  // the check found that Target is gone or out of reach
    uint32 CheckedMs = 0;
};

std::unordered_map<ObjectGuid, FocusState>& FocusStates()
{
    static std::unordered_map<ObjectGuid, FocusState> states;   // world thread only (battleground bots tick there)
    if (states.size() > 4096)
        states.clear();
    return states;
}

/// The units the focus rules see for one bot: the carriers, the marks, our real players' targets
struct FocusView
{
    TeamId Us = TEAM_NEUTRAL;
    Player* OurCarrier = nullptr;           // ours carries their flag (Eye: the flag)
    ObjectGuid TheirCarrier;                // theirs carries our flag (Eye: the flag)
    ObjectGuid Marks[2];
    std::vector<ObjectGuid> Assist;         // what the real players of the team near the bot fight
    float HealerReach = HealerReachRanged;
    bool Focus = false;                     // Playerbots.Bg.FocusFire
    bool Peel = false;                      // Playerbots.Bg.CarrierPlay
};

FocusView FocusViewOf(Player* bot, PlayerbotAI& ai, BgPlan const& plan)
{
    FocusView view;
    view.Us = TeamOf(bot);
    view.Focus = sBgConfig.FocusFire;
    view.Peel = sBgConfig.CarrierPlay;
    BotRole role = ai.GetRole();
    view.HealerReach = role == BotRole::Tank || role == BotRole::MeleeDps ? HealerReachMelee : HealerReachRanged;
    Map* map = bot->GetMap();
    TeamId them = view.Us == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    if (plan.Def->Kind == MapKind::Flag)
    {
        view.OurCarrier = PlayerOf(map, plan.Flags[them].Carrier);
        view.TheirCarrier = plan.Flags[view.Us].Carrier;
    }
    else if (plan.Def->Kind == MapKind::Eye && !plan.Flags[0].Carrier.IsEmpty())
    {
        Player* carrier = PlayerOf(map, plan.Flags[0].Carrier);
        if (carrier && TeamOf(carrier) == view.Us)
            view.OurCarrier = carrier;
        else if (carrier)
            view.TheirCarrier = carrier->GetGUID();
    }
    if (view.OurCarrier && !view.OurCarrier->IsAlive())
        view.OurCarrier = nullptr;
    if (view.Us == TEAM_ALLIANCE || view.Us == TEAM_HORDE)
    {
        view.Marks[0] = plan.Teams[view.Us].Marks[0];
        view.Marks[1] = plan.Teams[view.Us].Marks[1];
    }
    for (Seen const& seen : plan.Picture)
        if (!seen.Bot && seen.Alive && seen.Team == view.Us && bot->GetExactDist2d(seen.Pos) <= AssistRange)
            if (Player* human = PlayerOf(map, seen.Guid))
            {
                if (Unit* victim = human->GetVictim())
                    view.Assist.push_back(victim->GetGUID());
                if (human->IsInCombat() && !human->GetTarget().IsEmpty())
                    view.Assist.push_back(human->GetTarget());
            }
    return view;
}

/// Why this unit is worth a switch for this bot (BgRules::Focus, None when it is not)
BgRules::Focus FocusOf(Player* bot, FocusView const& view, Unit* unit, bool healer)
{
    using BgRules::Focus;
    if (!unit || !unit->IsAlive() || !bot->IsValidAttackTarget(unit) || !bot->CanSeeOrDetect(unit))
        return Focus::None;
    float dist = bot->GetExactDist(unit);
    if (dist > FocusRange)
        return Focus::None;
    if (view.Focus && unit->GetGUID() == view.TheirCarrier)
        return dist <= CarrierNearRange ? Focus::CarrierNear : Focus::CarrierFar;
    if (view.Peel && view.OurCarrier && (unit->GetVictim() == view.OurCarrier || unit->GetTarget() == view.OurCarrier->GetGUID()))
        return Focus::Peel;
    if (!view.Focus)
        return Focus::None;
    if (unit->GetGUID() == view.Marks[0] || unit->GetGUID() == view.Marks[1])
        return Focus::Marked;
    if (healer && dist <= view.HealerReach)
        return Focus::Healer;
    if (dist <= FinishRange && unit->GetHealthPct() < FinishHealthPct)
        for (Unit* attacker : unit->getAttackers())
            if (attacker != bot && attacker->GetTypeId() == TYPEID_PLAYER && TeamOf(attacker->ToPlayer()) == view.Us)
                return Focus::Finish;
    if (std::find(view.Assist.begin(), view.Assist.end(), unit->GetGUID()) != view.Assist.end())
        return Focus::Assist;
    return Focus::None;
}

/// One check for one bot in combat: a better target to switch to, or the forced target focus set has gone. Fills the state
bool CheckFocus(PlayerbotAI& ai, FocusState& state)
{
    Player* bot = ai.GetBot();
    uint32 now = GameTime::GetGameTimeMS();
    state.Pending.Clear();
    state.ClearPending = false;
    state.CheckedMs = now;
    if ((!sBgConfig.FocusFire && !sBgConfig.CarrierPlay) || ai.GetRole() == BotRole::Healer)
        return false;
    Assignment const* job = CombatJob(ai);
    // a push fights its boss, a carrier runs, a bot falling back runs: none of them is focus fire's to turn
    if (!job || job->Over == BgRules::Overlay::Push || job->Over == BgRules::Overlay::Carrier || job->Over == BgRules::Overlay::FallBack)
        return false;
    BgPlan const* plan = sBgPlanner.Find(bot->GetMapId(), bot->GetInstanceId());
    if (!plan || !plan->Def)
        return false;

    FocusView view = FocusViewOf(bot, ai, *plan);
    Map* map = bot->GetMap();
    auto healerOf = [plan](ObjectGuid const& guid)
    {
        for (Seen const& seen : plan->Picture)
            if (seen.Guid == guid)
                return seen.Healer;
        return false;
    };

    Unit* victim = bot->GetVictim();
    BgRules::Focus current = victim ? FocusOf(bot, view, victim, victim->GetTypeId() == TYPEID_PLAYER && healerOf(victim->GetGUID()))
        : BgRules::Focus::None;
    Unit* best = nullptr;
    BgRules::Focus bestFocus = BgRules::Focus::None;
    float bestDist = 0.0f;
    TeamId them = view.Us == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    for (Seen const& seen : plan->Picture)
    {
        if (!seen.Alive || seen.Team != them || bot->GetExactDist2d(seen.Pos) > FocusRange + 10.0f)
            continue;
        Player* enemy = PlayerOf(map, seen.Guid);
        BgRules::Focus focus = FocusOf(bot, view, enemy, seen.Healer);
        if (focus == BgRules::Focus::None)
            continue;
        float dist = bot->GetExactDist(enemy);
        if (focus > bestFocus || (focus == bestFocus && dist < bestDist))
        {
            best = enemy;
            bestFocus = focus;
            bestDist = dist;
        }
    }
    // #20: a team plays as well as its skill: this bot takes this switch or it does not (one roll per target)
    bool willing = true;
    if (best && sBgConfig.TeamSkill && (view.Us == TEAM_ALLIANCE || view.Us == TEAM_HORDE))
        willing = BgRules::EventRoll(plan->Teams[view.Us].Seed, bot->GetGUID().GetCounter(), best->GetGUID().GetCounter(), plan->Teams[view.Us].Skill);
    if (best && willing && best != victim && BgRules::ShouldSwitch(current, bestFocus, now - state.SwitchMs, FocusSwitchMs))
    {
        state.Pending = best->GetGUID();
        return true;
    }

    // the unit focus set is dead, gone or out of reach: drop it, so the bot does not run after it
    if (!state.Target.IsEmpty())
    {
        Unit* target = ObjectAccessor::GetUnit(*bot, state.Target);
        if (!target || !target->IsAlive() || bot->GetExactDist(target) > FocusRange + 5.0f)
        {
            state.ClearPending = true;
            return true;
        }
    }
    return false;
}

bool FocusTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if ((!sBgConfig.FocusFire && !sBgConfig.CarrierPlay) || !bot->IsInCombat() || !Plays(bot))
        return false;
    return CheckFocus(ai, FocusStates()[bot->GetGUID()]);
}

class FocusAction : public Action
{
public:
    char const* GetName() const override { return "bg focus"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        FocusState& state = FocusStates()[bot->GetGUID()];
        uint32 now = GameTime::GetGameTimeMS();
        if (now - state.CheckedMs > 1000 && !CheckFocus(ai, state))
            return false;
        if (!state.Pending.IsEmpty())
        {
            // the attack action takes the forced target first (Playerbots.Combat.RoleTargeting = 1)
            ai.SetForcedTarget(state.Pending);
            state.Target = state.Pending;
            state.SwitchMs = now;
            state.Pending.Clear();
            return true;
        }
        if (state.ClearPending)
        {
            ai.ClearForcedTarget();
            state.Target.Clear();
            state.ClearPending = false;
            return true;
        }
        return false;
    }
};

// ---- #14 carrier play: our carrier runs home in combat too, and kites in our flag room while it cannot capture ----

constexpr float KiteRadius = 10.0f;         // the kite spots: a ring this far around our flag
constexpr uint32 KiteSpots = 8;
constexpr uint32 KiteRepickMs = 5000;
constexpr float KiteDanger = 10.0f;         // an enemy this close picks a new spot at once
constexpr float KiteFrom = 40.0f;           // a carrier this close to our flag kites instead of standing on it
constexpr uint32 BadSpotMs = 60000;

struct KiteState
{
    int Spot = -1;
    uint32 PickedMs = 0;
    uint32 Bad = 0;                         // spots without a mesh path (bit per spot)
    uint32 BadUntilMs = 0;
};

std::unordered_map<ObjectGuid, KiteState>& KiteStates()
{
    static std::unordered_map<ObjectGuid, KiteState> states;    // world thread only
    if (states.size() > 1024)
        states.clear();
    return states;
}

/// A spot in our flag room away from the enemies there (BgRules::PickKiteSpot over a ring around the flag)
Position KiteSpot(Player* bot, BgPlan const& plan, Position const& base, int& index)
{
    KiteState& state = KiteStates()[bot->GetGUID()];
    uint32 now = GameTime::GetGameTimeMS();
    if (state.Bad && int32(state.BadUntilMs - now) <= 0)
        state.Bad = 0;
    std::vector<BgRules::Point2> ring = BgRules::RingPoints({ base.GetPositionX(), base.GetPositionY() }, KiteRadius, KiteSpots, 0.0f);
    TeamId us = TeamOf(bot);
    bool danger = false;
    std::vector<BgRules::Point2> enemies;
    for (Seen const& seen : plan.Picture)
        if (seen.Alive && seen.Team != us && seen.Pos.GetExactDist2d(base) <= KiteFrom)
        {
            enemies.push_back({ seen.Pos.GetPositionX(), seen.Pos.GetPositionY() });
            danger |= bot->GetExactDist2d(seen.Pos) <= KiteDanger;
        }
    if (state.Spot < 0 || now - state.PickedMs >= KiteRepickMs || danger || (state.Bad & (1u << state.Spot)))
    {
        std::vector<BgRules::Point2> open;
        std::vector<int> map;
        for (uint32 i = 0; i < ring.size(); ++i)
            if (!(state.Bad & (1u << i)))
            {
                open.push_back(ring[i]);
                map.push_back(int(i));
            }
        int pick = BgRules::PickKiteSpot(open, enemies);
        state.Spot = pick >= 0 ? map[pick] : -1;
        state.PickedMs = now;
    }
    index = state.Spot;
    if (state.Spot < 0)
        return base;
    return Position(ring[state.Spot].X, ring[state.Spot].Y, base.GetPositionZ());
}

void MarkBadSpot(Player* bot, int index)
{
    if (index < 0)
        return;
    KiteState& state = KiteStates()[bot->GetGUID()];
    state.Bad |= 1u << index;
    state.BadUntilMs = GameTime::GetGameTimeMS() + BadSpotMs;
}

/// Where our carrier goes now: our flag to capture (flag maps), a kite spot while our flag is out, the nearest tower we hold
/// (Eye). False when there is nowhere to go
bool CarrierGoal(Player* bot, BgPlan const& plan, Position& out, float& arrive, int& kite)
{
    kite = -1;
    TeamId us = TeamOf(bot);
    if (plan.Def->Kind == MapKind::Flag)
    {
        Position base;
        if (!ObjectivePosition(plan, int8(us), base))
            return false;
        FlagInfo const& ourFlag = plan.Flags[us];
        if (sBgConfig.CarrierPlay && ourFlag.State != uint8(FlagState::InBase) && bot->GetExactDist2d(base) < KiteFrom)
        {
            out = KiteSpot(bot, plan, base, kite);
            arrive = 1.5f;
            return true;
        }
        out = base;
        arrive = 3.0f;
        return true;
    }
    if (plan.Def->Kind == MapKind::Eye)
    {
        int8 best = NoObjective;
        float bestDist = 0.0f;
        for (size_t i = 0; i < plan.Objectives.size(); ++i)
        {
            Objective const& tower = plan.Objectives[i];
            if (!tower.Known || tower.Owner != us)
                continue;
            float dist = bot->GetExactDist2d(tower.Pos);
            if (best == NoObjective || dist < bestDist)
            {
                best = int8(i);
                bestDist = dist;
            }
        }
        if (best == NoObjective)
            return false;
        out = plan.Objectives[best].Pos;
        arrive = 3.0f;
        return true;
    }
    return false;
}

bool CarrierTrigger(PlayerbotAI& ai)
{
    if (!sBgConfig.CarrierPlay)
        return false;
    Assignment const* job = CombatJob(ai);
    if (!job || job->Over != BgRules::Overlay::Carrier)
        return false;
    Player* bot = ai.GetBot();
    BgPlan const* plan = sBgPlanner.Find(bot->GetMapId(), bot->GetInstanceId());
    Position goal;
    float arrive = 0.0f;
    int kite = -1;
    return plan && plan->Def && CarrierGoal(bot, *plan, goal, arrive, kite) && bot->GetExactDist2d(goal) > arrive + 1.0f;
}

class CarrierAction : public Action
{
public:
    char const* GetName() const override { return "bg carrier"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        BgPlan const* plan = sBgPlanner.Find(bot->GetMapId(), bot->GetInstanceId());
        Position goal;
        float arrive = 0.0f;
        int kite = -1;
        if (!plan || !plan->Def || !CarrierGoal(bot, *plan, goal, arrive, kite))
            return false;
        // a carrier does not stop to fight: it keeps going (what hits it is the escorts' job)
        ai.ClearForcedTarget();
        if (Go(bot, goal, arrive, NoObjective))
            return true;
        MarkBadSpot(bot, kite);
        return false;
    }
};
}

void Register(AiRegistry& registry)
{
    registry.AddTrigger("bg leash", 0, LeashTrigger);
    registry.AddAction<LeashAction>("bg leash");
    registry.AddTrigger("bg fall back", 0, FallBackTrigger);
    registry.AddAction<FallBackAction>("bg fall back");
    registry.AddTrigger("bg focus", 1000, FocusTrigger);
    registry.AddAction<FocusAction>("bg focus");
    registry.AddTrigger("bg carrier", 0, CarrierTrigger);
    registry.AddAction<CarrierAction>("bg carrier");
    registry.AddTrigger("bg tether", 0, TetherTrigger);
    registry.AddAction<TetherAction>("bg tether");
}

bool Plays(Player const* bot)
{
    return bot->InBattleground() && sBgConfig.Plans(bot->GetMapId()) && FindMap(bot->GetMapId());
}

bool Play(Player* bot, PlayerbotAI& ai)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;
    // a vehicle test drives this bot: nothing else moves it
    if (VehicleTestHolds(bot))
        return true;
    // the gates are still closed: wait at our gate on a map that has a gate row, else where the bot stands like the pilot does
    if (bg->GetStatus() != STATUS_IN_PROGRESS)
    {
        if (bg->GetStatus() == STATUS_WAIT_JOIN && bot->IsAlive())
            WaitAtGate(bot);
        return true;
    }

    Assignment job = sBgPlanner.Touch(bot);
    BgPlan const* plan = sBgPlanner.Find(bot->GetMapId(), bot->GetInstanceId());
    if (!plan || !plan->Def)
        return false;
    // not planned yet (the first plan comes within a second)
    if (job.Base == Job::None)
        return true;
    // a capture cast is running: stand still, set no target
    if (IsOpening(bot))
        return true;

    uint32 now = GameTime::GetGameTimeMS();
    // Bg.Rebuff: back from a graveyard (dead on the last plan, or seen alive again a moment ago), a bot with buffs to give stands for
    // a few seconds out of a fight, so the buff action above this one casts the raid buffs the death took, as players rebuff before
    // they run off. The buffs list only (standard buffs, forms, pets): no cooldown, no consumable
    if (sBgConfig.Rebuff && !bot->IsInCombat() && (!job.WasAlive || (job.RezMs && now - job.RezMs < RebuffHoldMs))
        && ai.GetRotation().HasList(ai, "buffs"))
    {
        if (bot->isMoving())
            bot->StopMoving();
        return true;
    }

    // the reactive behaviours (phase 4): an outnumbered bot runs to friends; a helper plays a defender of the node it was sent to
    if (job.Over == BgRules::Overlay::FallBack && int32(job.FallBackUntilMs - now) > 0)
    {
        ai.ClearForcedTarget();
        if (bot->GetExactDist2d(job.FallBackTo) > 6.0f)
            Go(bot, job.FallBackTo, 5.0f, NoObjective);
        return true;
    }
    if (job.Over == BgRules::Overlay::Respond && job.Respond != NoObjective && int32(job.RespondUntilMs - now) > 0)
    {
        job.Base = Job::Defend;
        job.Now = Job::Defend;
        job.Objective = job.Respond;
        job.Follow.Clear();
    }
    // #13: the mine group goes for the boss of its mine (the creatures on the way are fought as they come)
    if (job.Over == BgRules::Overlay::Mine && job.ErrandObjective != NoObjective && size_t(job.ErrandObjective) < plan->Objectives.size())
    {
        Creature* boss = BossOf(*plan, bot->GetMap(), job.ErrandObjective);
        Position at = boss && boss->IsAlive() ? boss->GetPosition() : plan->Objectives[job.ErrandObjective].Pos;
        if (ai.GetRole() == BotRole::Healer)
        {
            if (bot->GetExactDist2d(at) > HealerStandOff)
                Go(bot, at, HealerStandOff - 5.0f, NoObjective);
            return true;
        }
        if (boss && boss->IsAlive() && bot->GetExactDist(boss) <= WalkInFrom)
            ai.SetForcedTarget(boss->GetGUID());
        Go(bot, at, boss ? 4.0f : 6.0f, NoObjective);
        return true;
    }
    // #17: the ninja travels as anyone does, then sneaks the last stretch in stealth (no mount, no fight) and takes the node
    if (job.Over == BgRules::Overlay::Ninja && job.ErrandObjective != NoObjective && size_t(job.ErrandObjective) < plan->Objectives.size())
    {
        Position const& at = plan->Objectives[job.ErrandObjective].Pos;
        float dist = bot->GetExactDist2d(at);
        if (dist > NinjaSneakFrom)
        {
            Go(bot, at, NinjaSneakFrom - 10.0f, job.ErrandObjective);
            return true;
        }
        if (bot->IsMounted())
            bot->CancelMountAura();
        if (!bot->HasStealthAura() && !bot->IsInCombat())
        {
            if (bot->GetClass() == CLASS_DRUID && !bot->HasAura(NinjaCatForm) && bot->HasSpell(NinjaCatForm))
            {
                bot->CastSpell(bot, NinjaCatForm, CastSpellExtraArgs(TRIGGERED_NONE));
                return true;
            }
            uint32 stealth = bot->GetClass() == CLASS_ROGUE ? NinjaStealth : NinjaProwl;
            if (bot->HasSpell(stealth) && !bot->GetSpellHistory()->HasCooldown(stealth))
            {
                bot->CastSpell(bot, stealth, CastSpellExtraArgs(TRIGGERED_NONE));
                return true;
            }
        }
        if (dist > CapRange)
        {
            ai.ClearForcedTarget();
            if (MoveAlongMesh(bot, at, CapRange - 5.0f) == MoveResult::NoPath)
                sBgPlanner.NoteNoPath(bot, job.ErrandObjective);
            return true;
        }
        // there: take it as an attacker would (the cast breaks stealth)
        job.Base = Job::Attack;
        job.Now = Job::Attack;
        job.Objective = job.ErrandObjective;
        job.Follow.Clear();
    }
    // a certain loss (#15): the attackers fight where most of the enemies are
    if (TeamId team = TeamOf(bot); job.Over == BgRules::Overlay::Brawl && (team == TEAM_ALLIANCE || team == TEAM_HORDE) && plan->Teams[team].BrawlKnown)
    {
        Position const& at = plan->Teams[team].BrawlAt;
        if (ai.GetRole() == BotRole::Healer)
            return PlayHealer(bot, *plan, job, at);
        Engage(bot, ai, at, GuardRange);
        Go(bot, at, 10.0f, NoObjective);
        return true;
    }
    // #12: a brawler (in Alterac Valley a road fighter) fights whoever comes near on the way instead of walking past
    if (job.Over == BgRules::Overlay::None && job.Now == Job::Attack
        && (job.Style == BgRules::Archetype::Brawler || job.Style == BgRules::Archetype::RoadFighter)
        && Engage(bot, ai, bot->GetPosition(), job.Style == BgRules::Archetype::RoadFighter ? RoadFighterRange : BrawlerRange))
        return true;

    switch (plan->Def->Kind)
    {
        case MapKind::Flag: return PlayFlagMap(bot, ai, *plan, job);
        case MapKind::Capture: return PlayNode(bot, ai, *plan, job);
        case MapKind::Eye: return PlayEye(bot, ai, *plan, job);
        case MapKind::Valley: return PlayValley(bot, ai, *plan, job);
    }
    return false;
}
}
