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

#ifndef PlayerbotAI_h__
#define PlayerbotAI_h__

#include "BotSpellBook.h"
#include "Engine.h"
#include "AiContext.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Rotation/RotationRunner.h"
#include "Strategies/Rpg/RpgInfo.h"
#include "Travel/RouteFollower.h"
#include <string>
#include <string_view>
#include <unordered_set>

class Player;
class Unit;

namespace Playerbots
{
enum class BotRole
{
    Tank,
    Healer,
    MeleeDps,
    RangedDps
};

char const* GetRoleName(BotRole role);

/// AI of one bot character. Update() runs in the map thread of the bot,
/// HandleCommand() in the world thread (chat), they never overlap.
class PlayerbotAI
{
    enum SimClass : uint8 { SimRotation = 2, SimNear = 3 };

public:
    PlayerbotAI(Player* bot, ObjectGuid master, uint8 formationIndex);
    ~PlayerbotAI();

    /// Master controlled bots keep their strategies (follow/stay/combat) in playerbots_db_store across logins
    void SaveState() const;
    void RestoreState();

    Player* GetBot() const { return _bot; }
    /// The owner, or for random bots the leader of the player party they are in (LFG groups, invitations)
    ObjectGuid GetMasterGuid() const;
    /// nullptr if the master is offline or not in world, and for random bots that are not in a player party
    Player* GetMaster() const;

    void Update(uint32 diff);
    /// Nobody is near the bot: frozen, or simulated only by the activity rotation. Written and read by the bot's own AI tick
    bool IsFarBot() const { return _far; }
    /// Frozen bots only need a few ticks per second (Playerbots.FrozenTickMs, 0 = every map tick). Adds diff and says
    /// whether a tick is due; `elapsed` is the time since the last one
    bool FrozenTickDue(uint32 diff, uint32 frozenTickMs, uint32& elapsed)
    {
        _frozenAccum += diff;
        if (_frozenAccum < frozenTickMs)
            return false;
        elapsed = _frozenAccum;
        _frozenAccum = 0;
        return true;
    }
    /// Full AI (combat, wander, gear). False = freeze in place like AC BotActiveAlone
    bool IsSimulated();
    bool ComputeSimulated();

    BotRole GetRole() const;
    BotSpellBook& GetSpellBook() { return _spellBook; }
    Engine& GetEngine() { return _engine; }
    uint8 GetFormationIndex() const { return _formationIndex; }

    /// Current enemy the bot should fight, nullptr if none
    Unit* SelectCombatTarget();
    /// Searches for the enemy (called through the "combat target" value, once per tick)
    Unit* FindCombatTarget() const;
    /// Explicit target from the "attack" command
    void SetForcedTarget(ObjectGuid target) { _forcedTarget = target; }

    void TellMaster(std::string_view text) const;

    void ClearChaseTarget() { _chaseTarget.Clear(); }
    void ClearForcedTarget() { _forcedTarget.Clear(); }
    bool IsValidEnemy(Unit* target) const;
    /// Back to the strategies a new bot starts with. Random bots get their own routine on top (rpg or wander, pvp, quest)
    void ResetStrategies();
    /// The routine of a bot without an owner (rpg or wander, pvp, quest), added on top of the defaults
    void AddRandomBotStrategies();

    /// Movement helpers, they only issue a new movement generator if the current one differs
    void Follow(Player* master);
    void Stay();
    void Chase(Unit* target, bool ranged);

    /// Where an ownerless bot wanders. Set after it is placed in the world
    void SetHome(Position const& home) { _home = home; _hasHome = true; }
    bool HasHome() const { return _hasHome; }
    Position const& GetHome() const { return _home; }
    RpgInfo& GetRpg() { return _rpg; }
    RouteFollower& GetRoute() { return _route; }
    RotationRunner& GetRotation() { return _rotation; }
    /// The bot is about to be moved by its own doing (a flight, a portal, a hearthstone): not an interruption
    void NoteExpectedMove() { _expectedMove = true; }
    /// Changes every time the bot was moved by something other than itself (summon, GM teleport,
    /// map change). Strategies that keep a plan compare it with the value they saw last and start over
    uint32 GetInterruptEpoch() const { return _interruptEpoch; }
    void RememberStand();
    /// Number of AI ticks run so far; lets an action reuse a search it already did in the same tick
    uint32 GetTick() const { return _context.GetTick(); }
    AiContext& GetContext() { return _context; }
    /// Corpses the bot already looted or gave up on
    std::unordered_set<ObjectGuid>& GetLootedCorpses() { return _lootedCorpses; }

private:
    bool IsActiveAlone();
    /// Hash of the guid and the current rotation window, 0..max-1: the same bot gets another slot every window
    uint32 RotatingSlot(uint32 max) const;
    void CompleteTeleportIfNeeded();
    bool SnapToGround(bool notify, float lift = 0.25f);
    Unit* FindNearbyEnemy() const;
    void MaintainGear(uint32 diff);
    void MaintainTalents();
    void DetectInterruption();
    void OnInterrupted();
    void OnLeaderLost();

    Player* _bot;
    ObjectGuid _botGuid;          // _bot may be gone when the AI is destroyed
    ObjectGuid _master;
    uint8 _formationIndex;
    uint32 _frozenAccum = 0;
    uint32 _updateTimer;

    AiContext _context;               // before the engine, which refers to it
    Engine _engine{ _context };
    BotSpellBook _spellBook;
    std::unordered_set<ObjectGuid> _lootedCorpses;

    ObjectGuid _forcedTarget;
    ObjectGuid _chaseTarget;
    Position _home;
    bool _hasHome = false;
    bool _idle = false;
    bool _grounded = false;
    uint32 _gearTimer = 5000;
    uint32 _talentLevel = 0;         // level and specialization the starter build was applied for
    uint32 _talentSpec = 0;
    RpgInfo _rpg;
    RouteFollower _route;
    RotationRunner _rotation;
    bool _expectedMove = false;
    Position _lastStand;
    Position _knownPos;
    uint32 _knownMap = 0;
    uint32 _knownInstance = 0;
    bool _hasKnown = false;
    uint32 _interruptEpoch = 0;
    bool _hadLeader = false;   // random bot: followed a player party leader on the last tick
    bool _hasLastStand = false;

    mutable uint32 _nextEnemyScan = 0;   // GameTimeMS, no grid search before it
    // BotActiveAlone verdict, recomputed twice a second instead of on every world tick
    bool _activeAlone = false;
    uint32 _activeAloneStamp = 0;
    uint32 _forcedUntil = 0;
    bool _activeAloneValid = false;
    bool _verdictValid = false;   // Playerbots.SimVerdictCacheMs: the last IsSimulated() answer and when it was computed
    bool _verdictSim = false;
    uint32 _verdictStamp = 0;
    uint32 _verdictJitter = 0;
    uint8 _simClass = SimRotation; // why the bot is simulated alone: the activity rotation or a human nearby
    bool _far = false;         // nobody is near: frozen, or simulated only by the activity rotation
    uint32 _nextGoalMs = 0;    // far-bot goal check, so a frozen tick does not roll the table every time
};
}

#endif // PlayerbotAI_h__
