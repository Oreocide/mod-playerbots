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
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "Travel/BotMover.h"
#include <algorithm>

// The Forge of Souls (map 632), after AzerothCore's Ai/Dungeon/FoS with TrinityCore's entries and spell ids (checked):
//  * Bronjahm: a bot carrying Corrupt Soul (68839) while he casts it runs 15 yards from him and otherwise holds still; damage dealers and
//    the tank's helpers kill the Corrupted Soul Fragment (36535) first, the tank keeps the boss; the tank takes him to AzerothCore's spot
//    when no fragment is around and moves away from a fragment that comes within 10 yards of him; during the Soulstorm the others gather at
//    him (6.5 yards for ranged, 2 for melee).
//  * Not ported: the Devourer of Souls' Mirrored Soul turn-away (what it protects against is not certain).
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapForgeOfSouls = 632;
constexpr uint32 NpcBronjahm = 36497;
constexpr uint32 NpcSoulFragment = 36535;
constexpr uint32 SpellCorruptSoul = 68839;
constexpr float RunDistance = 15.0f;
constexpr float DodgeRelevance = 82.0f;

Position const BronjahmTankSpot(5297.920f, 2506.698f, 686.068f);

Creature* Fragment(Player* bot) { return bot->FindNearestCreature(NpcSoulFragment, 50.0f, true); }
bool Soulstorm(Unit const* boss) { return HasAnyAura(boss, { 68872 }) || IsCasting(boss, { 68870, 68904, 68872 }); }

Unit* ForgeTarget(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    BotRole role = ai.GetRole();
    if (role == BotRole::Healer || role == BotRole::Tank || !FindBoss(bot, { NpcBronjahm }, 100.0f))
        return nullptr;
    Creature* fragment = Fragment(bot);
    return fragment && ai.IsValidEnemy(fragment) ? fragment : nullptr;
}

bool CorruptSoulTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcBronjahm }, 100.0f);
    return boss && bot->HasAura(SpellCorruptSoul) && IsCasting(boss, { SpellCorruptSoul });
}

bool PositionTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return FindBoss(bot, { NpcBronjahm }, 100.0f) && !bot->HasAura(SpellCorruptSoul);
}

/// Tank: away from a fragment next to the boss, else to the spot. Others: gather at the boss during the Soulstorm
bool PositionAction(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcBronjahm }, 100.0f);
    if (!boss)
        return false;

    Creature* fragment = Fragment(bot);
    if (ai.GetRole() == BotRole::Tank)
    {
        if (boss->GetVictim() != bot)
            return false;
        if (fragment)
            return fragment->GetExactDist2d(boss) < 10.0f && StepAway(ai, fragment->GetPositionX(), fragment->GetPositionY(), 13.0f);
        return bot->GetExactDist2d(BronjahmTankSpot) > 5.0f && MoveCombat(bot, BronjahmTankSpot, 3.0f, MovePriority::Position) != MoveResult::NoPath;
    }

    if (fragment || !Soulstorm(boss) || bot->GetExactDist2d(boss) <= 10.0f)
        return false;

    float arrive = ai.GetRole() == BotRole::RangedDps || ai.GetRole() == BotRole::Healer ? 6.5f : 2.0f;
    return MoveCombat(bot, boss->GetPosition(), arrive, MovePriority::Position) != MoveResult::NoPath;
}
}

void RegisterForgeOfSouls(AiRegistry& r)
{
    r.AddTrigger("fos corrupt soul", 0, CorruptSoulTrigger);
    r.AddTrigger("fos position", 0, PositionTrigger);

    AddAction(r, "fos run from bronjahm", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        Creature* boss = FindBoss(bot, { NpcBronjahm }, 100.0f);
        if (!boss)
            return false;
        // far enough: hold still (anything else the bot would do runs it back to him)
        if (bot->GetExactDist2d(boss) >= RunDistance)
            return true;
        return StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), RunDistance);
    });
    AddAction(r, "fos position", PositionAction);

    DungeonDef def;
    def.MapId = MapForgeOfSouls;
    def.Name = "forge of souls";
    def.Target = ForgeTarget;
    def.Triggers = {
        { "fos corrupt soul", { { "fos run from bronjahm", DodgeRelevance + 3.0f } }, StrategyStates::Combat },
        { "fos position", { { "fos position", 61.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
