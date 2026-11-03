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
#include "Battleground/BgPlanner.h"
#include "Db/PlayerbotRepository.h"
#include "PlayerbotAI.h"
#include "Travel/MoveToward.h"
#include "Battleground.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
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
    if (MoveAlongMesh(bot, where, arrive) != MoveResult::NoPath)
        return true;

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
            if (!other || other == bot || !other->IsAlive() || TeamOf(other) != team)
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
        // home to our flag; the capture counts once our own flag is back in its base, so the carrier waits there
        ai.ClearForcedTarget();
        Go(bot, ourBase, 3.0f, int8(us));
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
    // the gates are still closed: wait like the pilot does
    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return true;

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

    switch (plan->Def->Kind)
    {
        case MapKind::Flag: return PlayFlagMap(bot, ai, *plan, job);
        case MapKind::Capture: return PlayNode(bot, ai, *plan, job);
        case MapKind::Eye: return PlayEye(bot, ai, *plan, job);
    }
    return false;
}
}
