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

#include "Dungeon/EncounterKit.h"
#include "Combat/Positioning.h"
#include "Creature.h"
#include "Engine/AiRegistry.h"
#include "Engine/ContextStrategies.h"
#include "Engine/Engine.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "Travel/BotMover.h"
#include <cstring>

// Utgarde Keep (map 574), after AzerothCore's Ai/Dungeon/UK, with TrinityCore's entries and spell ids (checked against the 12.1.0 client
// data and the boss scripts of this core):
//  * Prince Keleseth: a group member held in a Frost Tomb (aura 48400, cast on the player by the tomb creature 23965) is freed by killing
//    the tomb: everybody but the healers hits the tomb while one exists.
//  * Skarvald and Dalronn: Dalronn (the caster) dies first, by everybody but the tank and the healers, while he can be attacked.
//  * Ingvar the Plunderer: the tank steps behind him when he starts a Smash or Dark Smash (a frontal blow) and back in front once it is
//    over; ranged bots and healers do not start a cast-time spell while he casts a roar (it interrupts).
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapUtgardeKeep = 574;
constexpr uint32 NpcFrostTomb = 23965;
constexpr uint32 NpcDalronn = 24201;
constexpr uint32 NpcIngvar = 23954;
constexpr uint32 NpcIngvarUndead = 23980;
constexpr uint32 SpellFrostTombAura = 48400;
constexpr char const* StrategyName = "utgarde keep";

constexpr float DodgeBehindYards = 3.0f;
constexpr float DodgePriorityRelevance = 82.0f;     // above heal (80): the tank steps out of the blow first
constexpr float ReturnRelevance = 62.0f;            // just above attack (60)

Creature* Ingvar(Player* bot) { return FindBoss(bot, { NpcIngvar, NpcIngvarUndead }); }

bool IsSmashing(Unit const* boss) { return IsCasting(boss, { 42669, 59706, 42723, 59709 }); }
bool IsRoaring(Unit const* boss) { return IsCasting(boss, { 42708, 59708, 42729, 59734 }); }

Unit* UtgardeKeepTarget(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    BotRole role = ai.GetRole();
    if (role == BotRole::Healer)
        return nullptr;

    // Keleseth: free the player in the tomb
    if (GroupMemberHasAura(bot, SpellFrostTombAura))
        if (Creature* tomb = bot->FindNearestCreature(NpcFrostTomb, 60.0f, true); tomb && ai.IsValidEnemy(tomb))
            return tomb;

    // Skarvald and Dalronn: the caster first
    if (role != BotRole::Tank)
        if (Creature* dalronn = bot->FindNearestCreature(NpcDalronn, 80.0f, true); dalronn && dalronn->isTargetableForAttack() && ai.IsValidEnemy(dalronn))
            return dalronn;

    return nullptr;
}

bool SmashTrigger(PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.DungeonEnable || ai.GetRole() != BotRole::Tank)
        return false;
    Creature* boss = Ingvar(ai.GetBot());
    return boss && IsSmashing(boss);
}

/// The tank stands in front of Ingvar and he is not smashing: nothing to do. Behind him with no smash coming: step back in front
bool ReturnTrigger(PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.DungeonEnable || ai.GetRole() != BotRole::Tank)
        return false;
    Player* bot = ai.GetBot();
    Creature* boss = Ingvar(bot);
    if (!boss || IsSmashing(boss) || !bot->IsInCombat())
        return false;
    return !InFrontArc({ boss->GetPositionX(), boss->GetPositionY() }, boss->GetOrientation(), { bot->GetPositionX(), bot->GetPositionY() }, Pi / 2.0f);
}

bool StepTo(PlayerbotAI& ai, Pt wish, float z, MovePriority priority)
{
    Player* bot = ai.GetBot();
    Position snapped;
    if (!SnapToMesh(bot, Position(wish.X, wish.Y, z), snapped, 3.0f, 2.5f))
        return false;
    return MoveCombat(bot, snapped, 1.0f, priority) == MoveResult::Moving;
}

class DodgeSmashAction final : public Action
{
public:
    char const* GetName() const override { return "uk dodge smash"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Creature* boss = Ingvar(bot);
        if (!boss)
            return false;

        Pt where{ boss->GetPositionX(), boss->GetPositionY() };
        // already behind or at his side: the cone does not reach
        if (!InFrontArc(where, boss->GetOrientation(), { bot->GetPositionX(), bot->GetPositionY() }, Pi / 2.0f))
            return false;

        StepTo(ai, BehindPoint(where, boss->GetOrientation(), DodgeBehindYards), boss->GetPositionZ(), MovePriority::Avoid);
        return true;
    }
};

class ReturnAction final : public Action
{
public:
    char const* GetName() const override { return "uk smash return"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Creature* boss = Ingvar(bot);
        if (!boss)
            return false;

        Pt where{ boss->GetPositionX(), boss->GetPositionY() };
        float reach = std::max(1.5f, (bot->GetCombatReach() + boss->GetCombatReach()) * 0.6f);
        StepTo(ai, PointAt(where, boss->GetOrientation(), reach), boss->GetPositionZ(), MovePriority::Position);
        return true;
    }
};

/// While Ingvar casts a roar, ranged bots hold their attack (a cast started now is interrupted)
class RoarMultiplier final : public Multiplier
{
public:
    float GetValue(PlayerbotAI& ai, Action* action) override
    {
        if (!sPlayerbotsConfig.DungeonEnable || std::strcmp(action->GetName(), "attack") != 0)
            return 1.0f;

        BotRole role = ai.GetRole();
        if (role != BotRole::RangedDps && role != BotRole::Healer)
            return 1.0f;

        Creature* boss = Ingvar(ai.GetBot());
        return boss && IsRoaring(boss) ? 0.0f : 1.0f;
    }
};

class UtgardeKeepStrategy final : public Strategy
{
public:
    UtgardeKeepStrategy() : Strategy(StrategyName) { }
    uint8_t GetStates() const override { return StrategyStates::Combat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "uk smash", { { "uk dodge smash", DodgePriorityRelevance } }, StrategyStates::Combat });
        triggers.push_back({ "uk behind ingvar", { { "uk smash return", ReturnRelevance } }, StrategyStates::Combat });
    }

    void InitMultipliers(std::vector<std::unique_ptr<Multiplier>>& multipliers) override { multipliers.push_back(std::make_unique<RoarMultiplier>()); }
};
}

void RegisterUtgardeKeep(AiRegistry& r)
{
    r.AddTrigger("uk smash", 0, SmashTrigger);
    r.AddTrigger("uk behind ingvar", 0, ReturnTrigger);
    r.AddAction<DodgeSmashAction>("uk dodge smash");
    r.AddAction<ReturnAction>("uk smash return");
    r.AddStrategy<UtgardeKeepStrategy>(StrategyName);

    RegisterTargetRule(MapUtgardeKeep, StrategyName, UtgardeKeepTarget);
    sContextStrategies.Register({ [](BotContext const& context)
    {
        return sPlayerbotsConfig.DungeonEnable && context.Place == PlaceKind::Dungeon && context.MapId == MapUtgardeKeep && context.InGroup;
    }, { StrategyName } });
}
}
