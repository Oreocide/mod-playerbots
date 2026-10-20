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
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"

// Halls of Stone (map 599), after AzerothCore's Ai/Dungeon/HoS with TrinityCore's entries and spell ids:
//  * Krystallus: a player carrying Ground Slam (aura 50827 / 50833) is about to shatter: that bot walks away from the nearest group member.
//  * Sjonnir the Ironshaper: when he casts Lightning Ring (50840 / 59848) everybody steps out of 12 yards of him.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapHallsOfStone = 599;
constexpr uint32 NpcKrystallus = 27977;
constexpr uint32 NpcSjonnir = 27978;
constexpr float ShatterSpread = 12.0f;
constexpr float RingDistance = 12.0f;
constexpr float DodgeRelevance = 82.0f;

bool GroundSlamTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return HasAnyAura(bot, { 50827, 50833 }) && FindBoss(bot, { NpcKrystallus }, 80.0f);
}

bool LightningRingTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcSjonnir }, 80.0f);
    return boss && IsCasting(boss, { 50840, 59848 }) && bot->GetExactDist2d(boss) < RingDistance;
}
}

void RegisterHallsOfStone(AiRegistry& r)
{
    r.AddTrigger("hos ground slam", 0, GroundSlamTrigger);
    r.AddTrigger("hos lightning ring", 0, LightningRingTrigger);
    AddAction(r, "hos shatter spread", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        Player* nearest = nullptr;
        float nearestDist = 40.0f;
        if (Group* group = bot->GetGroup())
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource(); member && member != bot && member->IsAlive() && member->IsInMap(bot))
                    if (float dist = bot->GetExactDist2d(member); dist < nearestDist)
                    {
                        nearest = member;
                        nearestDist = dist;
                    }
        return nearest && StepAway(ai, nearest->GetPositionX(), nearest->GetPositionY(), ShatterSpread);
    });
    AddAction(r, "hos leave ring", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcSjonnir }, 80.0f);
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), RingDistance);
    });

    DungeonDef def;
    def.MapId = MapHallsOfStone;
    def.Name = "halls of stone";
    def.Triggers = {
        { "hos ground slam", { { "hos shatter spread", DodgeRelevance } }, StrategyStates::Combat },
        { "hos lightning ring", { { "hos leave ring", DodgeRelevance } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
