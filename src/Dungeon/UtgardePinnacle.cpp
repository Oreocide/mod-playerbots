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
#include "Creature.h"
#include "Engine/AiRegistry.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include <cstring>
#include <vector>

// Utgarde Pinnacle (map 575), after AzerothCore's Ai/Dungeon/UP with TrinityCore's entries and spell ids:
//  * Skadi the Ruthless: while his mount Grauf flies over the room nobody attacks the boss or the mount (they cannot be reached), the adds
//    on the ground are the targets. The Freezing Cloud (Grauf's breath and the clouds it leaves, on the Flame Breath Trigger creatures) is
//    stepped out of. Once he is on the ground, his Whirlwind is stepped away from (the tank farther: the boss chases it).
//  * King Ymiron: while Bane is up (his cast or his aura) nobody attacks, melee stops and casters stop casting.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapUtgardePinnacle = 575;
constexpr uint32 NpcSkadi = 26693;
constexpr uint32 NpcGrauf = 26893;
constexpr uint32 NpcBreathTrigger = 28351;
constexpr uint32 NpcYmiron = 26861;
constexpr uint32 SpellBreathPeriodic = 47592;
constexpr float CloudStepDistance = 5.0f;
constexpr float WhirlwindRadius = 7.0f;
constexpr float WhirlwindTankRadius = 12.0f;
constexpr float DodgeRelevance = 82.0f;

bool BaneActive(Unit const* ymiron) { return ymiron && (IsCasting(ymiron, { 48294, 59301 }) || HasAnyAura(ymiron, { 48294, 59301 })); }

bool CloudActive(Player* bot)
{
    Creature* grauf = FindBoss(bot, { NpcGrauf }, 120.0f);
    if (!grauf)
        return false;
    if (grauf->HasAura(SpellBreathPeriodic))
        return true;

    Creature* cloud = FindBoss(bot, { NpcBreathTrigger }, 40.0f);
    return cloud && HasAnyAura(cloud, { 47579, 60020 });
}

/// While Grauf flies: the nearest add that is not Skadi or his mount
Unit* PinnacleTarget(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (ai.GetRole() == BotRole::Healer || !FindBoss(bot, { NpcGrauf }, 120.0f))
        return nullptr;

    Unit* best = nullptr;
    for (ObjectGuid const& guid : ai.GetContext().GetValue<std::vector<ObjectGuid>>("attackers", ai))
    {
        if (guid.GetEntry() == NpcSkadi || guid.GetEntry() == NpcGrauf)
            continue;
        if (Unit* unit = ObjectAccessor::GetUnit(*bot, guid); unit && ai.IsValidEnemy(unit) && (!best || bot->GetExactDistSq(unit) < bot->GetExactDistSq(best)))
            best = unit;
    }
    return best;
}

bool FreezingCloudTrigger(PlayerbotAI& ai) { return CloudActive(ai.GetBot()); }

bool SkadiWhirlwindTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (FindBoss(bot, { NpcGrauf }, 120.0f))
        return false;
    Creature* skadi = FindBoss(bot, { NpcSkadi }, 60.0f);
    return skadi && HasAnyAura(skadi, { 50228, 59322 });
}

bool BaneTrigger(PlayerbotAI& ai) { return BaneActive(FindBoss(ai.GetBot(), { NpcYmiron }, 80.0f)); }
}

void RegisterUtgardePinnacle(AiRegistry& r)
{
    r.AddTrigger("up freezing cloud", 0, FreezingCloudTrigger);
    r.AddTrigger("up skadi whirlwind", 0, SkadiWhirlwindTrigger);
    r.AddTrigger("up ymiron bane", 0, BaneTrigger);

    AddAction(r, "up avoid freezing cloud", [](PlayerbotAI& ai)
    {
        Creature* cloud = FindBoss(ai.GetBot(), { NpcBreathTrigger }, 40.0f);
        return cloud && StepAway(ai, cloud->GetPositionX(), cloud->GetPositionY(), CloudStepDistance);
    });
    AddAction(r, "up avoid whirlwind", [](PlayerbotAI& ai)
    {
        Creature* skadi = FindBoss(ai.GetBot(), { NpcSkadi }, 60.0f);
        float radius = ai.GetRole() == BotRole::Tank ? WhirlwindTankRadius : WhirlwindRadius;
        return skadi && StepAway(ai, skadi->GetPositionX(), skadi->GetPositionY(), radius);
    });
    AddAction(r, "up stop attack", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        bot->AttackStop();
        bot->InterruptNonMeleeSpells(false);
        return true;
    });

    DungeonDef def;
    def.MapId = MapUtgardePinnacle;
    def.Name = "utgarde pinnacle";
    def.Target = PinnacleTarget;
    def.Triggers = {
        { "up freezing cloud", { { "up avoid freezing cloud", DodgeRelevance + 3.0f } }, StrategyStates::Combat },
        { "up skadi whirlwind", { { "up avoid whirlwind", DodgeRelevance + 2.0f } }, StrategyStates::Combat },
        { "up ymiron bane", { { "up stop attack", DodgeRelevance + 3.0f } }, StrategyStates::Combat },
    };
    def.Multipliers = {
        // Bane: nobody attacks
        [](PlayerbotAI& ai, Action* action)
        {
            return std::strcmp(action->GetName(), "attack") == 0 && BaneActive(FindBoss(ai.GetBot(), { NpcYmiron }, 80.0f)) ? 0.0f : 1.0f;
        },
        // Skadi in the air: do not chase the boss or the mount
        [](PlayerbotAI& ai, Action* action)
        {
            if (std::strcmp(action->GetName(), "attack") != 0)
                return 1.0f;
            Unit* target = ai.SelectCombatTarget();
            if (!target || (target->GetEntry() != NpcSkadi && target->GetEntry() != NpcGrauf))
                return 1.0f;
            return FindBoss(ai.GetBot(), { NpcGrauf }, 120.0f) ? 0.0f : 1.0f;
        },
    };
    RegisterDungeon(r, std::move(def));
}
}
