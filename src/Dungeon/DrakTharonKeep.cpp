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
#include "Combat/GroupCombat.h"
#include "Creature.h"
#include "Engine/AiRegistry.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "Travel/BotMover.h"

// Drak'Tharon Keep (map 600), after AzerothCore's Ai/Dungeon/DTK with TrinityCore's entries and spell ids (checked):
//  * Trollgore: the corpses of Drakkari Invaders explode, nobody stands within 7 yards of one.
//  * Novos the Summoner: his Arcane Field (cast 47346) is left by 12 yards; while he is up the group waits at the AzerothCore party spot
//    when it has nothing to hit, the tank takes the Hulking Corpse then the Crystal Handler, one damage dealer (the first melee, else the
//    first ranged) takes the Risen Shadowcaster then the Fetid Troll Corpse (on the stairs), everybody else the Crystal Handler then the
//    Hulking Corpse. (AC runs the targets and the spot only while the Arcane Field is cast; here they run for the whole fight.)
//  * Not ported: Tharon'ja's Gift (the answers are class spells: Touch of Life, Bone Armor, Taunt, Slaying Strike).
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapDrakTharonKeep = 600;
constexpr uint32 NpcTrollgore = 26630;
constexpr uint32 NpcNovos = 26631;
constexpr uint32 NpcDrakkariInvader = 27709;
constexpr uint32 NpcCrystalHandler = 26627;
constexpr uint32 NpcHulkingCorpse = 27597;
constexpr uint32 NpcRisenShadowcaster = 27600;
constexpr uint32 NpcFetidTrollCorpse = 27598;
constexpr float CorpseDistance = 7.0f;
constexpr float FieldDistance = 12.0f;
constexpr float DodgeRelevance = 82.0f;

Position const NovosPartySpot(-378.852f, -760.349f, 28.587f);

bool IsDamage(BotRole role) { return role == BotRole::MeleeDps || role == BotRole::RangedDps; }

Unit* ValidOf(PlayerbotAI& ai, uint32 entry)
{
    Creature* creature = ai.GetBot()->FindNearestCreature(entry, 80.0f, true);
    return creature && ai.IsValidEnemy(creature) ? creature : nullptr;
}

/// The damage dealer that fights on the stairs: the first melee damage dealer of the group, else the first ranged one
bool IsStairsDps(PlayerbotAI& ai)
{
    GroupRolesInfo roles = ai.GetContext().GetValue<GroupRolesInfo>("group roles", ai);
    uint64 first = 0;
    for (GroupRole wanted : { GroupRole::MeleeDps, GroupRole::RangedDps })
    {
        for (RoleSlot const& slot : roles.Slots)
            if (slot.Index != 0xFF && slot.Role == wanted)
            {
                first = slot.Id;
                break;
            }
        if (first)
            break;
    }
    return first && first == ai.GetBot()->GetGUID().GetCounter();
}

Unit* DrakTharonTarget(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (ai.GetRole() == BotRole::Healer || !FindBoss(bot, { NpcNovos }, 100.0f))
        return nullptr;

    if (ai.GetRole() == BotRole::Tank)
    {
        if (Unit* unit = ValidOf(ai, NpcHulkingCorpse))
            return unit;
        return ValidOf(ai, NpcCrystalHandler);
    }

    if (IsStairsDps(ai))
    {
        if (Unit* unit = ValidOf(ai, NpcRisenShadowcaster))
            return unit;
        return ValidOf(ai, NpcFetidTrollCorpse);
    }

    if (!IsDamage(ai.GetRole()))
        return nullptr;
    if (Unit* unit = ValidOf(ai, NpcCrystalHandler))
        return unit;
    return ValidOf(ai, NpcHulkingCorpse);
}

bool CorpseTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return FindBoss(bot, { NpcTrollgore }, 80.0f) && bot->FindNearestCreature(NpcDrakkariInvader, CorpseDistance, false);
}

bool FieldTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcNovos }, 80.0f);
    return boss && IsCasting(boss, { 47346 }) && bot->GetExactDist2d(boss) < FieldDistance;
}

bool NovosSpotTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return FindBoss(bot, { NpcNovos }, 100.0f) && !ai.SelectCombatTarget() && bot->GetExactDist(NovosPartySpot) > 15.0f;
}
}

void RegisterDrakTharonKeep(AiRegistry& r)
{
    r.AddTrigger("dtk corpse", 0, CorpseTrigger);
    r.AddTrigger("dtk arcane field", 0, FieldTrigger);
    r.AddTrigger("dtk novos spot", 0, NovosSpotTrigger);

    AddAction(r, "dtk leave corpse", [](PlayerbotAI& ai)
    {
        Creature* corpse = ai.GetBot()->FindNearestCreature(NpcDrakkariInvader, CorpseDistance, false);
        return corpse && StepAway(ai, corpse->GetPositionX(), corpse->GetPositionY(), CorpseDistance);
    });
    AddAction(r, "dtk leave field", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcNovos }, 80.0f);
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), FieldDistance);
    });
    AddAction(r, "dtk novos spot", [](PlayerbotAI& ai)
    {
        return MoveCombat(ai.GetBot(), NovosPartySpot, 4.0f, MovePriority::Position) != MoveResult::NoPath;
    });

    DungeonDef def;
    def.MapId = MapDrakTharonKeep;
    def.Name = "drak'tharon keep";
    def.Target = DrakTharonTarget;
    def.Triggers = {
        { "dtk corpse", { { "dtk leave corpse", DodgeRelevance } }, StrategyStates::Combat },
        { "dtk arcane field", { { "dtk leave field", DodgeRelevance + 1.0f } }, StrategyStates::Combat },
        { "dtk novos spot", { { "dtk novos spot", 61.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
