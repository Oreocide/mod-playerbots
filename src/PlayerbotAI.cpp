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

#include "PlayerbotAI.h"
#include "Travel/StuckDiagnosis.h"
#include "BotSession.h"
#include "Goal/GoalStore.h"
#include "Strategies/QuestTravel.h"
#include "GearFactory.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GenericStrategies.h"
#include "GridDefines.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementPackets.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "Db/PlayerbotRepository.h"
#include "GameTime.h"
#include "Log.h"
#include "RandomBotMgr.h"
#include "Spawn/SpawnPolicy.h"
#include "PlayerbotsConfig.h"
#include "SocialMgr.h"
#include "Talents/BotTalents.h"
#include "Social/BotSocialMgr.h"
#include "StringFormat.h"
#include "Engine/CastPolicy.h"
#include "Engine/TickPolicy.h"
#include "Travel/BotMover.h"
#include "Travel/MoveToward.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Formulas.h"
#include "Util.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

namespace Playerbots
{
namespace
{
constexpr uint32 ActiveAloneCacheMs = 500;
}

char const* GetRoleName(BotRole role)
{
    switch (role)
    {
        case BotRole::Tank: return "tank";
        case BotRole::Healer: return "healer";
        case BotRole::MeleeDps: return "melee dps";
        case BotRole::RangedDps: return "ranged dps";
    }
    return "unknown";
}

PlayerbotAI::PlayerbotAI(Player* bot, ObjectGuid master, uint8 formationIndex)
    : _bot(bot), _master(master), _formationIndex(formationIndex), _updateTimer(0)
{
    _botGuid = bot->GetGUID();
    // bots that logged in together must not all think in the same world tick
    _updateTimer = urand(0, sPlayerbotsConfig.UpdateInterval - 1);
    _gearTimer = urand(5000, 20000);
    EnsureAiRegistered();
    for (std::string_view name : DefaultStrategies)
        if (std::unique_ptr<Strategy> strategy = CreateStrategy(name))
            _engine.AddStrategy(std::move(strategy));
}

PlayerbotAI::~PlayerbotAI()
{
    GoalStore::Forget(_botGuid);
    SaveState();
}

void PlayerbotAI::SaveState() const
{
    // random bots get their strategies from the configuration, only bots of a player keep them
    if (_master.IsEmpty())
        return;

    sPlayerbotRepository->SetValue(_botGuid, "strategies", _engine.GetStrategyList());
}

void PlayerbotAI::RestoreState()
{
    if (_master.IsEmpty())
        return;

    Optional<std::string> saved = sPlayerbotRepository->GetValue(_botGuid, "strategies");
    if (!saved)
        return;

    std::vector<std::unique_ptr<Strategy>> strategies;
    for (std::string_view name : Trinity::Tokenize(*saved, ',', false))
    {
        while (!name.empty() && name.front() == ' ')
            name.remove_prefix(1);

        if (std::unique_ptr<Strategy> strategy = CreateStrategy(name))
            strategies.push_back(std::move(strategy));
    }

    // a saved list without the basic strategies would leave the bot inert, keep the defaults then
    if (std::ranges::none_of(strategies, [](std::unique_ptr<Strategy> const& strategy) { return strategy->GetName() == "death"; }))
        return;

    for (std::string const& name : _engine.GetStrategyNames())
        _engine.RemoveStrategy(name);

    for (std::unique_ptr<Strategy>& strategy : strategies)
        _engine.AddStrategy(std::move(strategy));
}

Player* PlayerbotAI::GetMaster() const
{
    if (!_master.IsEmpty())
    {
        Player* master = ObjectAccessor::FindConnectedPlayer(_master);
        return master && master->IsInWorld() ? master : nullptr;
    }

    // A random bot in a party led by a player follows that player. Battleground and battlefield
    // groups are not parties, and a bot led party is not a master either.
    Group* group = _bot->GetGroup();
    if (!group || group->isBGGroup() || group->isBFGroup())
        return nullptr;

    Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
    return leader && leader != _bot && leader->IsInWorld() && !IsBot(leader) ? leader : nullptr;
}

ObjectGuid PlayerbotAI::GetMasterGuid() const
{
    if (Player* master = GetMaster())
        return master->GetGUID();

    return _master;
}

bool PlayerbotAI::IsSimulated()
{
    uint32 const cacheMs = sPlayerbotsConfig.SimVerdictCacheMs;
    if (!cacheMs)
        return ComputeSimulated();

    // the group, LFG, battleground and instance checks are cheap one by one but run for every bot on every tick; their answer changes rarely
    uint32 now = GameTime::GetGameTimeMS();
    if (_verdictValid && now - _verdictStamp < cacheMs + _verdictJitter)
    {
        return _verdictSim;
    }

    _verdictSim = ComputeSimulated();
    _verdictStamp = now;
    _verdictJitter = uint32(_bot->GetGUID().GetCounter() % 64);
    _verdictValid = true;
    return _verdictSim;
}

bool PlayerbotAI::ComputeSimulated()
{
    if (!_master.IsEmpty() || GetMaster())
    {
        _far = false;
        return true;
    }

    if (_bot->InBattleground() || _bot->InBattlegroundQueue() || _bot->isUsingLfg())
    {
        _far = false;
        return true;
    }

    if (Map const* map = _bot->GetMap())
        if (map->IsDungeon() || map->IsRaid() || map->IsBattlegroundOrArena())
        {
            _far = false;
            return true;
        }

    // the proximity checks walk every online human, twice a second per bot is plenty
    uint32 now = GameTime::GetGameTimeMS();
    if (!_activeAloneValid || now - _activeAloneStamp >= ActiveAloneCacheMs + uint32(_bot->GetGUID().GetCounter() % 250))
    {
        _activeAlone = IsActiveAlone();
        _activeAloneStamp = now;
        _activeAloneValid = true;
    }

    _far = !_activeAlone || _simClass == SimRotation;
    return _activeAlone;
}

uint32 PlayerbotAI::RotatingSlot(uint32 max) const
{
    if (!max)
        return 0;

    // the finalizer of a 32 bit hash spreads the sequential guids evenly; mixing the time window in reshuffles all bots
    uint32 h = uint32(_bot->GetGUID().GetCounter());
    h ^= h >> 16;
    h *= 0x7feb352d;
    h ^= h >> 15;
    h *= 0x846ca68b;
    h ^= h >> 16;
    uint32 window = (GameTime::GetGameTimeMS() / 1000) / std::max<uint32>(1, sPlayerbotsConfig.BotActiveAloneDurationSeconds);
    return (h ^ (window * 0x9e3779b9u)) % max;
}

bool PlayerbotAI::IsActiveAlone()
{
    // test knob (Playerbots.Debug.ForceActive): about that many random bots run their full AI with no player near, so a
    // headless soak exercises the map-thread AI paths. The pick is a fixed hash of the guid, no rotation.
    if (uint32 forceActive = sPlayerbotsConfig.DebugForceActive)
    {
        uint32 h = uint32(_bot->GetGUID().GetCounter());
        h ^= h >> 16;
        h *= 0x7feb352d;
        h ^= h >> 15;
        h *= 0x846ca68b;
        h ^= h >> 16;
        if (h % std::max<uint32>(1, sPlayerbotsConfig.RandomCount) < forceActive)
        {
            _simClass = SimRotation;
            return true;
        }
    }

    uint32 pct = sPlayerbotsConfig.BotActiveAlone;
    if (pct >= 100)
    {
        _simClass = SimRotation;
        return true;
    }

    uint32 now = GameTime::GetGameTimeMS();
    bool forced = false;
    if (auto humans = sPlayerbotMgr->GetNearbyHumans())
    {
        uint32 botMap = _bot->GetMapId();
        uint32 botZone = _bot->GetZoneId();
        ObjectGuid::LowType guild = _bot->GetGuildId();
        PlayerSocial* social = _bot->GetSocial();
        float radius = sPlayerbotsConfig.BotActiveAloneRadius;
        float radiusSq = radius * radius;
        uint32 zoneCap = sPlayerbotsConfig.BotActiveAloneZoneMaxActive;
        uint32 radiusCap = sPlayerbotsConfig.BotActiveAloneRadiusMaxActive;
        float zoneRange = std::max(radius * 3.0f, 300.0f);

        // nearest player inside the zone / the radius, for the caps (-1: none)
        float nearestZoneSq = -1.0f;
        float nearestRadiusSq = -1.0f;
        for (PlayerbotMgr::NearbyHuman const& human : *humans)
        {
            if (sPlayerbotsConfig.BotActiveAloneMap && human.MapId == botMap)
            {
                forced = true;
                break;
            }
            if (sPlayerbotsConfig.BotActiveAloneGuild && guild && guild == human.GuildId)
            {
                forced = true;
                break;
            }
            if (sPlayerbotsConfig.BotActiveAloneFriend && social && social->HasFriend(human.Guid))
            {
                forced = true;
                break;
            }
            if (human.MapId != botMap)
                continue;

            float dx = _bot->GetPositionX() - human.X;
            float dy = _bot->GetPositionY() - human.Y;
            float distSq = dx * dx + dy * dy;
            if (sPlayerbotsConfig.BotActiveAloneZone && human.ZoneId && human.ZoneId == botZone
                && (nearestZoneSq < 0.0f || distSq < nearestZoneSq))
                nearestZoneSq = distSq;
            if (distSq <= radiusSq && (nearestRadiusSq < 0.0f || distSq < nearestRadiusSq))
                nearestRadiusSq = distSq;
        }

        // with a cap only the bots of the nearest slots wake: slot k of n may be as far as range * (k + 1) / n
        auto qualifies = [this](float distSq, float range, uint32 cap)
        {
            if (distSq < 0.0f)
                return false;
            if (!cap)
                return true;
            float reach = range * float(RotatingSlot(cap) + 1) / float(cap);
            return distSq <= reach * reach;
        };

        if (!forced)
        {
            // Close enough to be on screen. The slot cap thins the outer band only: after bots stopped
            // stacking on one graveyard, that cap was leaving the ones a player was looking at asleep.
            constexpr float SeenYards = 100.0f;
            bool seen = nearestRadiusSq >= 0.0f && nearestRadiusSq <= SeenYards * SeenYards;
            forced = seen || qualifies(nearestZoneSq, zoneRange, zoneCap) || qualifies(nearestRadiusSq, radius, radiusCap);
        }
    }

    // a bot a player just left does not fall asleep at once
    if (forced)
    {
        _forcedUntil = now + sPlayerbotsConfig.BotActiveAloneForceGraceSeconds * 1000;
        _simClass = SimNear;
        return true;
    }
    if (now < _forcedUntil)
    {
        _simClass = SimNear;
        return true;
    }

    if (!pct)
        return false;

    if (sPlayerbotsConfig.BotActiveAloneSmartScale)
    {
        uint32 level = _bot->GetLevel();
        if (level >= sPlayerbotsConfig.BotActiveAloneSmartScaleMinLevel
            && level <= sPlayerbotsConfig.BotActiveAloneSmartScaleMaxLevel)
        {
            uint32 worldDiff = sPlayerbotMgr->GetWorldDiffMax();
            uint32 floor = sPlayerbotsConfig.BotActiveAloneSmartScaleFloor;
            uint32 ceiling = sPlayerbotsConfig.BotActiveAloneSmartScaleCeiling;
            if (worldDiff >= ceiling)
                pct = 0;
            else if (worldDiff > floor)
                pct = uint32(float(pct) * float(ceiling - worldDiff) / float(ceiling - floor));
        }
    }

    _simClass = SimRotation;
    return RotatingSlot(100) < pct;
}

bool PlayerbotAI::SnapToGround(bool notify, float lift)
{
    if (!_bot->IsInWorld() || _bot->IsBeingTeleported())
        return false;


    // Party bots keep the master's Z (Ironforge, tunnels). Sky-height snap
    // puts them on the mountain above the instance interior.
    if (!_master.IsEmpty() || GetMaster())
    {
        _grounded = true;
        return true;
    }

    if (!_bot->GetMap())
        return false;

    float x = _bot->GetPositionX();
    float y = _bot->GetPositionY();
    float stand = GetStandZ(_bot, x, y, _bot->GetPositionZ(), lift);
    if (std::fabs(_bot->GetPositionZ() - stand) < 0.05f)
    {
        _grounded = true;
        return true;
    }

    if (std::fabs(_bot->GetPositionZ() - stand) > 1.0f)
        TC_LOG_INFO("server.playerbots.stuck", "Playerbots: ground snap for {}: map {} at ({:.1f}, {:.1f}), z {:.1f} -> {:.1f} (dz {:.1f}); {}",
            _bot->GetName(), _bot->GetMapId(), x, y, _bot->GetPositionZ(), stand, stand - _bot->GetPositionZ(),
            DescribeStuck(_bot, _hasLastStand && _lastStandMap == _bot->GetMapId() ? &_lastStand : nullptr));

    if (notify)
        _bot->NearTeleportTo(x, y, stand, _bot->GetOrientation());
    else
        _bot->UpdatePosition(x, y, stand, _bot->GetOrientation());

    _bot->SetFallInformation(0, stand);
    _bot->SetFall(false);
    _bot->StopMoving();
    _grounded = true;
    return true;
}

void PlayerbotAI::Update(uint32 diff)
{
    CompleteTeleportIfNeeded();

    if (!_bot->IsInWorld())
        return;

    if (_bot->IsBeingTeleported())
    {
        _grounded = false;
        _bot->GetSession()->SetDormant(false);
        return;
    }

    // A random bot that followed a player's party goes back to its own life when the party is gone
    if (_master.IsEmpty())
    {
        // group membership, not GetMaster(): that is empty for a moment while the leader is on a loading screen
        bool hasLeader = false;
        Group* group = _bot->GetGroup();
        if (group && !group->isBGGroup() && !group->isBFGroup())
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
            hasLeader = leader && leader != _bot && !IsBot(leader);
        }

        if (_hadLeader && !hasLeader)
            OnLeaderLost();
        _hadLeader = hasLeader;
    }

    if (!IsSimulated())
    {
        _bot->GetSession()->SetDormant(true);
        if (!_idle)
        {
            _idle = true;
            _bot->AttackStop();
            _bot->StopMoving();
            _bot->GetMotionMaster()->Clear();
            // Gravity is about to be switched off, so the client will keep whatever height it was given.
            // Tell it the floor when someone is close enough to see the hover; no lift, or they float.
            if (!_grounded)
                SnapToGround(sPlayerbotMgr->HasHumanWithin(_bot, 200.0f), 0.0f);
            _bot->SetDisableGravity(true);
        }
        else if (!_grounded)
            SnapToGround(false, 0.0f);
        if (_grounded && _master.IsEmpty())
            SetHome(_bot->GetPosition());

        // nobody can see the bot: a route is lived through in time, not walked
        if (_route.IsActive())
        {
            RouteStatus status = _route.StepVirtual(*this, diff);
            if (status == RouteStatus::Arrived || status == RouteStatus::Failed)
            {
                _idle = false;       // it was placed somewhere: the idle setup and the ground snap run again
                _grounded = false;
            }
        }
        if (sPlayerbotsConfig.GoalEnable && GameTime::GetGameTimeMS() >= _nextGoalMs)
        {
            if (GoalStore::ReadyForAbstractXp(_bot->GetGUID(), GameTime::GetGameTimeMS()))
                if (uint32 xp = QuestTravel::AbstractXp(_bot))
                {
                    _bot->GiveXP(xp, nullptr);
                    sPlayerbotRepository->Remember(_bot->GetGUID(), "quest", "abstract", std::to_string(xp));
                }
            _nextGoalMs = GoalStore::OnFrozen(*this, GameTime::GetGameTimeMS());
        }
        return;
    }

    if (_idle)
    {
        SnapToGround(true);
        _bot->SetDisableGravity(false);
        _idle = false;
    }
    _bot->GetSession()->SetDormant(false);

    // A bot has no client, so nothing sends the packets (time sync, "active mover complete") that set this flag, and without it
    // Player::CanNeverSee hides every object from the bot: CanSeeOrDetect is false for every creature, so it cannot pick a target,
    // attack or cast at anything. The core's own comment: it delays visible objects "until the client is ready". A bot is ready.
    if (!_bot->HasPlayerLocalFlag(PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME))
    {
        _bot->SetPlayerLocalFlag(PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME);
        _bot->UpdateObjectVisibility(false);
    }

    _updateTimer += diff;
    if (sPlayerbotsConfig.DynamicTickEnable)
    {
        // a fight that starts, or a bot that dies or gets up, is not left waiting for the end of an idle delay
        uint8 now = !_bot->IsAlive() ? StrategyStates::Dead : _bot->IsInCombat() ? StrategyStates::Combat : StrategyStates::NonCombat;
        if (_lastState && now != _lastState)
            _wake = true;
    }
    uint32 interval = sPlayerbotsConfig.DynamicTickEnable ? _nextDelayMs : sPlayerbotsConfig.UpdateInterval;
    if (!_wake && _updateTimer < interval)
        return;
    _wake = false;
    uint32 elapsed = _updateTimer;      // the time since the last tick, whatever the tick length
    _updateTimer = 0;
    _requestedDelay = 0;
    _context.BeginTick(GameTime::GetGameTimeMS());

    // a teleport is still noticed before the engine picks an action; the height query is the costly part
    RememberStand();
    DetectInterruption();

    BotSpellBook::StripStartingAuras(_bot);

    {
        MaintainGear(elapsed);
    }
    MaintainTalents();
    MaintainBagUpgrades(elapsed);

    _spellBook.Refresh(_bot);

    uint8 state = !_bot->IsAlive() ? StrategyStates::Dead : _bot->IsInCombat() ? StrategyStates::Combat : StrategyStates::NonCombat;
    if (_lastState && state != _lastState)
        _engine.NotifyStateChanged(*this, _lastState, state);
    _lastState = state;

    // a cast in progress is waited for, not interrupted by the next decision
    if (sPlayerbotsConfig.CastSupervisorEnable && SuperviseCast())
    {
        ScheduleNextTick(state);
        return;
    }

    _engine.DoNextAction(*this, state);
    ScheduleNextTick(state);
}

void PlayerbotAI::ScheduleNextTick(uint8 state)
{
    if (!sPlayerbotsConfig.DynamicTickEnable)
        return;

    TickPolicy policy;
    policy.CombatMs = sPlayerbotsConfig.DynamicTickCombatMs;
    policy.IdleMinMs = sPlayerbotsConfig.DynamicTickIdleMinMs;
    policy.IdleMaxMs = sPlayerbotsConfig.DynamicTickIdleMaxMs;
    policy.DeadMs = sPlayerbotsConfig.DynamicTickDeadMs;
    TickKind kind = state == StrategyStates::Combat ? TickKind::Combat : state == StrategyStates::Dead ? TickKind::Dead : TickKind::Idle;
    _nextDelayMs = NextTickDelay(policy, kind, _requestedDelay, rand_norm());
}

bool PlayerbotAI::SuperviseCast()
{
    bool channeled = false;
    Spell* spell = _bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell || spell->getState() != SPELL_STATE_PREPARING)
    {
        spell = _bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        channeled = spell != nullptr;
    }
    if (!spell)
        return false;

    SpellInfo const* info = spell->GetSpellInfo();
    Unit* target = spell->m_targets.GetUnitTarget();
    CastView view;
    view.Channeled = channeled;
    if (info && target && target != _bot)
    {
        view.HasUnitTarget = true;
        view.TargetAlive = target->IsAlive();
        view.SpellAllowsDeadTarget = info->HasAttribute(SPELL_ATTR2_ALLOW_DEAD_TARGET);
        view.TargetFriendly = _bot->IsFriendlyTo(target);
        view.SpellIsDirectHeal = info->HasEffect(SPELL_EFFECT_HEAL) && !info->HasAura(SPELL_AURA_PERIODIC_HEAL);
        view.TargetHealthPct = target->GetHealthPct();
        view.RemainingMs = channeled ? 0 : uint32(std::max<int32>(spell->GetTimer(), 0));
    }

    if (JudgeCast(view) == CastVerdict::Cancel)
    {
        _bot->InterruptSpell(channeled ? CURRENT_CHANNELED_SPELL : CURRENT_GENERIC_SPELL, false);
        return false;   // free to choose another action right away
    }

    // come back when the cast is about done
    RequestDelay(channeled ? 250 : std::clamp<uint32>(view.RemainingMs + 50, 100, 1000));
    return true;
}

void PlayerbotAI::UpdateContextStrategies()
{
    if (!sContextStrategies.Size())
        return;

    Map* map = _bot->GetMap();
    if (!map)
        return;

    BotContext place;
    place.MapId = map->GetId();
    place.Place = map->IsRaid() ? PlaceKind::Raid : map->IsBattleArena() ? PlaceKind::Arena : map->IsBattleground() ? PlaceKind::Battleground
        : map->IsDungeon() ? PlaceKind::Dungeon : PlaceKind::OpenWorld;
    place.DifficultyId = uint32(map->GetDifficultyID());
    place.InGroup = _bot->GetGroup() != nullptr;
    place.HasMaster = !_master.IsEmpty() || GetMaster() != nullptr;

    if (_hasPlace && place == _lastPlace)
        return;
    _lastPlace = place;
    _hasPlace = true;

    ContextStrategyTable::Change change = ContextStrategyTable::Diff(_contextStrategies, sContextStrategies.Resolve(place));
    for (std::string const& name : change.Remove)
    {
        _engine.RemoveStrategy(name);
        std::erase(_contextStrategies, name);
    }
    for (std::string const& name : change.Add)
    {
        if (std::unique_ptr<Strategy> strategy = CreateStrategy(name))
            _engine.AddStrategy(std::move(strategy));
        _contextStrategies.push_back(name);     // also when unknown: it is not retried on every tick
    }
}

namespace
{
// A bot walks, rides or falls a few yards per tick. Anything further was a teleport, summon or map change
constexpr float InterruptDistance = 60.0f;
}

// AzerothCore ChangeToIdle-after-teleport: a bot that was moved does not carry on with the job it had
// (walking back to an NPC on the other side of the continent), it looks around where it is now
void PlayerbotAI::DetectInterruption()
{
    if (_bot->IsInFlight())
    {
        _hasKnown = false; // no comparison across a taxi ride
        return;
    }

    Map* map = _bot->GetMap();
    if (!map)
        return;

    if (_hasKnown && _expectedMove)
    {
        // the bot moved itself (flight landing, portal, hearthstone, virtual arrival): it keeps its job
        _expectedMove = false;
        _hasKnown = false;
    }

    if (_hasKnown)
    {
        bool moved = _knownMap != map->GetId() || _knownInstance != map->GetInstanceId()
            || _knownPos.GetExactDist(_bot->GetPosition()) > InterruptDistance;
        if (moved)
            OnInterrupted();
    }

    _knownPos = _bot->GetPosition();
    _knownMap = map->GetId();
    _knownInstance = map->GetInstanceId();
    _hasKnown = true;
}

// The party was left or disbanded: the follow movement and anything a command changed (stay, passive) must not
// outlive it. Back to the routine a random bot logs in with, around where it stands now
void PlayerbotAI::OnLeaderLost()
{
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} lost its party leader, back to its own routine", _bot->GetName());
    ResetStrategies();
    _bot->AttackStop();
    OnInterrupted();
}

void PlayerbotAI::OnInterrupted()
{
    ++_interruptEpoch;

    _rpg.ChangeToIdle();
    _rpg.Started = GameTime::GetGameTimeMS();
    _rpg.WaitUntil = 0;
    _chaseTarget.Clear();
    _forcedTarget.Clear();
    NoteStopped(_moveState);
    _bot->GetMotionMaster()->Clear();

    // The area the bot works around is where it is now (not inside dungeons and battlegrounds, and not while
    // it follows a player)
    Map* map = _bot->GetMap();
    // never on ground that attacks the bot's team: the old home stays, and the eviction pass moves the bot back
    if (_master.IsEmpty() && !GetMaster() && map && !map->Instanceable() && !SpawnPolicy::IsEnemyGround(_bot))
    {
        SetHome(_bot->GetPosition());
        sRandomBotMgr->UpdateHome(_botGuid, map->GetId(), _bot->GetPosition());
    }

    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} was moved to map {} ({:.0f}, {:.0f}, {:.0f}), choosing a new job",
        _bot->GetName(), map ? map->GetId() : 0, _bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ());
}

void PlayerbotAI::RememberStand()
{
    if (!_bot->IsInWorld() || _bot->IsBeingTeleported() || _bot->IsInFlight())
        return;

    float stand = 0.0f;
    if (TryGetStandZ(_bot, _bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(), stand)
        && std::fabs(_bot->GetPositionZ() - stand) < 3.0f)
    {
        _lastStand = _bot->GetPosition();
        _lastStandMap = _bot->GetMapId();
        _hasLastStand = true;
        return;
    }

    if (!_hasLastStand)
        return;

    // A fall happens next to where the bot stood. A bot that was moved since (another map, a hop, a hotspot, the other side of the continent) must
    // not be pulled back to a stale stand: that put bots on the wrong continent, or in Stormwind, when they only stood lower than where they used to
    if (_lastStandMap != _bot->GetMapId() || _lastStand.GetExactDist2d(_bot->GetPosition()) > 60.0f)
    {
        _hasLastStand = false;
        return;
    }

    if (_bot->GetPositionZ() > _lastStand.GetPositionZ() - 8.0f && !_bot->IsFalling())
        return;

    TC_LOG_INFO("server.playerbots.stuck", "Playerbots: fall rescue for {}: map {} at ({:.1f}, {:.1f}, {:.1f}) falling={}, back to ({:.1f}, {:.1f}, {:.1f})",
        _bot->GetName(), _bot->GetMapId(), _bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(), _bot->IsFalling(),
        _lastStand.GetPositionX(), _lastStand.GetPositionY(), _lastStand.GetPositionZ());
    _bot->GetMotionMaster()->Clear();
    _bot->StopMoving();
    _bot->NearTeleportTo(_lastStand.GetPositionX(), _lastStand.GetPositionY(), _lastStand.GetPositionZ(), _lastStand.GetOrientation());
    _bot->SetFallInformation(0, _lastStand.GetPositionZ());
    _bot->SetFall(false);
}

// Random bots follow Blizzard's starter build as they level: it is applied again after a level or specialization
// change (the points of the new level need spending), never in combat
void PlayerbotAI::MaintainTalents()
{
    if (!_master.IsEmpty() || _bot->IsInCombat() || !_bot->IsAlive() || _bot->GetLevel() < MIN_SPECIALIZATION_LEVEL)
        return;

    uint32 spec = uint32(_bot->GetPrimarySpecialization());
    if (!spec || (_talentLevel == _bot->GetLevel() && _talentSpec == spec))
        return;

    _talentLevel = _bot->GetLevel();
    _talentSpec = spec;
    BotTalents::ApplyStarterBuild(_bot);
}

// A won roll or a traded gift sits in the bags until someone puts it on
void PlayerbotAI::MaintainBagUpgrades(uint32 diff)
{
    if (!sPlayerbotsConfig.GearEquipUpgrades || !sGearFactory->IsBuilt())
        return;

    if (_upgradeTimer > diff)
    {
        _upgradeTimer -= diff;
        return;
    }
    _upgradeTimer = 20000;

    if (!_bot->IsAlive() || _bot->IsInCombat() || _bot->HasUnitState(UNIT_STATE_CASTING) || _bot->IsMounted())
        return;

    sGearFactory->EquipBagUpgrade(_bot);
}

// Random (ownerless) bots dress themselves and re-gear while they level
void PlayerbotAI::MaintainGear(uint32 diff)
{
    if (!_master.IsEmpty() || !sPlayerbotsConfig.RandomGear || !sGearFactory->IsBuilt())
        return;

    if (_gearTimer > diff)
    {
        _gearTimer -= diff;
        return;
    }
    _gearTimer = 60000;

    if (!_bot->IsAlive() || _bot->IsInCombat())
        return;

    if (sGearFactory->NeedsGear(_bot))
        sGearFactory->Equip(_bot);

    sGearFactory->Supply(_bot);
    sGearFactory->Provision(_bot);
    BotSpellBook::StripJunk(_bot);
}

// A client acknowledges near teleports, bots do it themselves
// (far teleports are completed by PlayerbotMgr in the world thread)
void PlayerbotAI::CompleteTeleportIfNeeded()
{
    if (_bot->GetTeleportState() != TeleportState::WaitingForTeleportAck)
        return;

    WorldPackets::Movement::MoveTeleportAck ack{ WorldPacket(CMSG_MOVE_TELEPORT_ACK) };
    ack.MoverGUID = _bot->GetGUID();
    _bot->GetSession()->HandleMoveTeleportAck(ack);

    _chaseTarget.Clear();
    _grounded = false;
}

void PlayerbotAI::ResetStrategies()
{
    for (std::string const& name : _engine.GetStrategyNames())
        _engine.RemoveStrategy(name);
    // the context strategies went with them: apply them again on the next tick
    _contextStrategies.clear();
    _hasPlace = false;

    for (std::string_view name : DefaultStrategies)
        if (std::unique_ptr<Strategy> strategy = CreateStrategy(name))
            _engine.AddStrategy(std::move(strategy));

    // a bot without an owner also has its own routine: roaming and quests, or battleground hunting
    if (_master.IsEmpty())
        AddRandomBotStrategies();

    _chaseTarget.Clear();
    _forcedTarget.Clear();
}

void PlayerbotAI::AddRandomBotStrategies()
{
    if (sPlayerbotsConfig.RpgEnable)
    {
        if (std::unique_ptr<Strategy> rpg = CreateStrategy("rpg"))
            _engine.AddStrategy(std::move(rpg));
    }
    else if (std::unique_ptr<Strategy> wander = CreateStrategy("wander"))
        _engine.AddStrategy(std::move(wander));

    if (std::unique_ptr<Strategy> pvp = CreateStrategy("pvp"))
        _engine.AddStrategy(std::move(pvp));

    if (std::unique_ptr<Strategy> world = CreateStrategy("world"))
        _engine.AddStrategy(std::move(world));

    if (sPlayerbotsConfig.RandomQuests)
        if (std::unique_ptr<Strategy> quest = CreateStrategy("quest"))
            _engine.AddStrategy(std::move(quest));
}

BotRole PlayerbotAI::GetRole() const
{
    if (ChrSpecializationEntry const* spec = _bot->GetPrimarySpecializationEntry())
    {
        switch (spec->GetRole())
        {
            case ChrSpecializationRole::Tank:
                return BotRole::Tank;
            case ChrSpecializationRole::Healer:
                return BotRole::Healer;
            default:
                break;
        }

        if (spec->GetFlags().HasFlag(ChrSpecializationFlag::Ranged) || spec->GetFlags().HasFlag(ChrSpecializationFlag::Caster))
            return BotRole::RangedDps;

        return BotRole::MeleeDps;
    }

    switch (_bot->GetClass())
    {
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_PRIEST:
        case CLASS_HUNTER:
        case CLASS_EVOKER:
            return BotRole::RangedDps;
        default:
            return BotRole::MeleeDps;
    }
}

bool PlayerbotAI::IsValidEnemy(Unit* target) const
{
    return target && target->IsAlive() && target->IsInWorld() && target->IsInMap(_bot)
        && _bot->IsWithinDistInMap(target, 60.0f) && _bot->IsValidAttackTarget(target);
}

Unit* PlayerbotAI::SelectCombatTarget()
{
    ObjectGuid guid = _context.GetValue<ObjectGuid>("combat target", *this);
    return guid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*_bot, guid);
}

Unit* PlayerbotAI::FindCombatTarget() const
{
    if (!_forcedTarget.IsEmpty())
        if (Unit* forced = ObjectAccessor::GetUnit(*_bot, _forcedTarget); IsValidEnemy(forced))
            return forced;

    Player* master = GetMaster();
    if (master && master->IsInMap(_bot))
    {
        // assist: what the master fights, or what fights the master
        if (IsValidEnemy(master->GetVictim()))
            return master->GetVictim();

        if (Unit* selected = master->GetSelectedUnit(); IsValidEnemy(selected) && selected->IsInCombatWith(master))
            return selected;

        for (Unit* attacker : master->getAttackers())
            if (IsValidEnemy(attacker))
                return attacker;
    }

    if (IsValidEnemy(_bot->GetVictim()))
        return _bot->GetVictim();

    for (Unit* attacker : _bot->getAttackers())
        if (IsValidEnemy(attacker))
            return attacker;

    if (Group* group = _bot->GetGroup())
    {
        for (GroupReference const& ref : group->GetMembers())
        {
            Player* member = ref.GetSource();
            if (!member || member == _bot || !member->IsInMap(_bot))
                continue;

            for (Unit* attacker : member->getAttackers())
                if (IsValidEnemy(attacker))
                    return attacker;
        }
    }

    // ownerless bots pull a nearby creature of a similar level
    if (_master.IsEmpty() && !GetMaster() && !_bot->IsInCombat())
        if (Unit* nearby = FindNearbyEnemy())
            return nearby;

    return nullptr;
}

Unit* PlayerbotAI::FindNearbyEnemy() const
{
    struct NearbyEnemyCheck
    {
        Player* Bot = nullptr;

        bool operator()(Creature* creature) const
        {
            if (!creature->IsAlive() || creature->IsCritter() || creature->IsCivilian() || creature->IsTrigger()
                || creature->IsPet() || creature->IsTotem() || creature->IsElite() || creature->isWorldBoss()
                || creature->IsInEvadeMode() || !Bot->IsValidAttackTarget(creature))
                return false;

            uint8 level = creature->GetLevelForTarget(Bot);
            return level + 3 >= Bot->GetLevel() && level <= Bot->GetLevel() + 3;
        }
    };

    uint32 now = GameTime::GetGameTimeMS();
    if (_nextEnemyScan && now < _nextEnemyScan)
        return nullptr;

    std::vector<Creature*> creatures;
    NearbyEnemyCheck check{ _bot };
    Trinity::CreatureListSearcher searcher(_bot, creatures, check);
    Cell::VisitGridObjects(_bot, searcher, 30.0f);

    Creature* best = nullptr;
    for (Creature* creature : creatures)
        if (!best || _bot->GetExactDistSq(creature) < _bot->GetExactDistSq(best))
            best = creature;

    // an empty search is not repeated every tick
    _nextEnemyScan = best ? 0 : now + sPlayerbotsConfig.ScanInterval;
    return best;
}

void PlayerbotAI::TellMaster(std::string_view text) const
{
    if (Player* master = GetMaster())
        sBotSocialMgr->QueueWhisper(_botGuid, master->GetGUID(), std::string(text));
}

void PlayerbotAI::Follow(Player* master)
{
    // a positioning or avoid move (BotMover) is not undone by the follow generator
    if (MoveHeldAbove(_bot, MovePriority::Follow))
        return;

    if (_chaseTarget == master->GetGUID() && _bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        return;

    // spread the bots behind the master
    float angle = float(M_PI) + (_formationIndex % 2 ? 1.0f : -1.0f) * float(M_PI) / 6.0f * float(1 + _formationIndex / 2);
    _bot->GetMotionMaster()->Clear();
    _bot->GetMotionMaster()->MoveFollow(master, sPlayerbotsConfig.FollowDistance + _formationIndex / 2, ChaseAngle(angle));
    _chaseTarget = master->GetGUID();
}

void PlayerbotAI::Stay()
{
    if (_chaseTarget.IsEmpty() && _bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
        return;

    _bot->GetMotionMaster()->Clear();
    _bot->StopMoving();
    _chaseTarget.Clear();
}

void PlayerbotAI::Chase(Unit* target, bool ranged)
{
    // a positioning or avoid move (BotMover) is not undone by the chase generator
    if (MoveHeldAbove(_bot, MovePriority::Chase))
        return;

    if (_chaseTarget == target->GetGUID() && _bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        return;

    _bot->GetMotionMaster()->Clear();
    if (ranged)
        _bot->GetMotionMaster()->MoveChase(target, ChaseRange(0.0f, sPlayerbotsConfig.RangedDistance));
    else
        _bot->GetMotionMaster()->MoveChase(target);
    _chaseTarget = target->GetGUID();
}
}
