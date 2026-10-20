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

// The Culling of Stratholme (map 595), after AzerothCore's Ai/Dungeon/CoS with TrinityCore's entries (checked against the world data and the
// core's boss script: Explode Ghoul 52480 / 58825):
//  * Salramm the Fleshcrafter: everybody but the tank stays 12 yards from his Ghoul Minions (one is exploded now and then).
//  * Chrono-Lord Epoch: ranged bots and healers (not hunters) gather within 4 yards of him.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapCullingOfStratholme = 595;
constexpr uint32 NpcSalramm = 26530;
constexpr uint32 NpcEpoch = 26532;
constexpr uint32 NpcGhoulMinion = 27733;
constexpr uint32 NpcGhoulMinionHeroic = 36916;
constexpr float GhoulDistance = 12.0f;
constexpr float EpochDistance = 5.0f;
constexpr float DodgeRelevance = 82.0f;

bool GhoulTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (ai.GetRole() == BotRole::Tank || !FindBoss(bot, { NpcSalramm }, 80.0f))
        return false;
    Creature* ghoul = FindBoss(bot, { NpcGhoulMinion, NpcGhoulMinionHeroic }, GhoulDistance);
    return ghoul != nullptr;
}

bool EpochTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    BotRole role = ai.GetRole();
    if ((role != BotRole::RangedDps && role != BotRole::Healer) || bot->GetClass() == CLASS_HUNTER)
        return false;
    Creature* boss = FindBoss(bot, { NpcEpoch }, 60.0f);
    return boss && bot->GetExactDist2d(boss) > EpochDistance;
}
}

void RegisterCullingOfStratholme(AiRegistry& r)
{
    r.AddTrigger("cos ghoul", 0, GhoulTrigger);
    r.AddTrigger("cos epoch", 0, EpochTrigger);
    AddAction(r, "cos leave ghoul", [](PlayerbotAI& ai)
    {
        Creature* ghoul = FindBoss(ai.GetBot(), { NpcGhoulMinion, NpcGhoulMinionHeroic }, GhoulDistance);
        return ghoul && StepAway(ai, ghoul->GetPositionX(), ghoul->GetPositionY(), GhoulDistance);
    });
    AddAction(r, "cos stack epoch", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        Creature* boss = FindBoss(bot, { NpcEpoch }, 60.0f);
        return boss && MoveCombat(bot, boss->GetPosition(), 4.0f, MovePriority::Position) != MoveResult::NoPath;
    });

    DungeonDef def;
    def.MapId = MapCullingOfStratholme;
    def.Name = "culling of stratholme";
    def.Triggers = {
        { "cos ghoul", { { "cos leave ghoul", DodgeRelevance } }, StrategyStates::Combat },
        { "cos epoch", { { "cos stack epoch", DodgeRelevance - 2.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
