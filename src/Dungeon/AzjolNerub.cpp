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

// Azjol-Nerub (map 601), after AzerothCore's Ai/Dungeon/AN with TrinityCore's entries and spell ids (checked against the 12.1.0 client data and
// this core's world data):
//  * Krik'thir the Gatewatcher: damage dealers kill a Web Wrap (it holds a player) first, then the watcher adds by priority (skirmisher,
//    shadowcaster, the named watchers, warrior) before the boss. Tank and healers keep their normal behaviour.
//  * Anub'arak: when he casts Pound, everybody in front of him steps behind him.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapAzjolNerub = 601;
constexpr uint32 NpcKrikthir = 28684;
constexpr uint32 NpcWatcherSilthik = 28731;
constexpr uint32 NpcWatcherGashra = 28730;
constexpr uint32 NpcWatcherNarjil = 28729;
constexpr uint32 NpcSkirmisher = 28734;
constexpr uint32 NpcShadowcaster = 28733;
constexpr uint32 NpcWarrior = 28732;
constexpr uint32 NpcWebWrap = 28619;
constexpr uint32 NpcAnubarak = 29120;
constexpr float DodgeRelevance = 82.0f;

bool IsDamage(BotRole role) { return role == BotRole::MeleeDps || role == BotRole::RangedDps; }

Unit* AzjolNerubTarget(PlayerbotAI& ai)
{
    if (!IsDamage(ai.GetRole()))
        return nullptr;

    Player* bot = ai.GetBot();
    if (Creature* web = bot->FindNearestCreature(NpcWebWrap, 60.0f, true); web && ai.IsValidEnemy(web))
        return web;

    if (FindBoss(bot, { NpcKrikthir }, 80.0f))
        return FirstAttacker(ai, { NpcSkirmisher, NpcShadowcaster, NpcWatcherSilthik, NpcWatcherGashra, NpcWatcherNarjil, NpcWarrior });
    return nullptr;
}

bool PoundTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcAnubarak }, 60.0f);
    return boss && IsCasting(boss, { 53472, 59433 }) && IsInFrontOf(boss, bot);
}
}

void RegisterAzjolNerub(AiRegistry& r)
{
    r.AddTrigger("an pound", 0, PoundTrigger);
    AddAction(r, "an dodge pound", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcAnubarak }, 60.0f);
        return boss && StepBehind(ai, boss, 3.0f);
    });

    DungeonDef def;
    def.MapId = MapAzjolNerub;
    def.Name = "azjol-nerub";
    def.Target = AzjolNerubTarget;
    def.Triggers = { { "an pound", { { "an dodge pound", DodgeRelevance } }, StrategyStates::Combat } };
    RegisterDungeon(r, std::move(def));
}
}
