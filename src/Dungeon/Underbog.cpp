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
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "SpellAuras.h"
#include <list>
#include <vector>

// The Underbog (map 546), after AzerothCore's Ai/Dungeon/UB with TrinityCore's entries and spell ids (checked):
//  * Hungarfen: while he has Foul Spores (31673) everybody is at least 22 yards from him; an Underbog Mushroom that has turned into a Spore
//    Cloud (aura 34168, or 8 stacks of Grow 31698) is left by its radius (8 yards + reach + 1.5).
//  * Underbats: a bot that is not the tank does not stand within 6.5 yards of an Underbat that is not facing it (it lashes at its back).
//  * Not ported: AC's retreat path check against mushrooms, its melee rally point, and the exclusion of mushrooms from the target list (the
//    core's valid-target rule has no hook for that).
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapUnderbog = 546;
constexpr uint32 NpcHungarfen = 17770;
constexpr uint32 NpcMushroom = 17990;
constexpr uint32 NpcUnderbat = 17724;
constexpr uint32 SpellFoulSpores = 31673;
constexpr uint32 SpellGrow = 31698;
constexpr uint32 SpellSporeCloud = 34168;
constexpr float FoulSporesDistance = 22.0f;     // radius 20 + margin 2
constexpr float CloudRadius = 8.0f;
constexpr float CloudMargin = 1.5f;
constexpr uint8 GrowDanger = 8;
constexpr float LashRange = 6.5f;               // 5 + 1.5
constexpr float SafeArc = 2.6f;
constexpr float DodgeRelevance = 82.0f;

bool Dangerous(Creature* mushroom)
{
    if (!mushroom || !mushroom->IsAlive())
        return false;
    if (mushroom->HasAura(SpellSporeCloud))
        return true;
    Aura* grow = mushroom->GetAura(SpellGrow);
    return grow && grow->GetStackAmount() >= GrowDanger;
}

Creature* NearestDangerousMushroom(Player* bot)
{
    float range = CloudRadius + bot->GetCombatReach() + CloudMargin;
    Creature* best = nullptr;
    std::list<Creature*> mushrooms;
    bot->GetCreatureListWithEntryInGrid(mushrooms, NpcMushroom, 40.0f);
    for (Creature* mushroom : mushrooms)
        if (Dangerous(mushroom) && bot->GetExactDist2d(mushroom) <= range && (!best || bot->GetExactDistSq(mushroom) < bot->GetExactDistSq(best)))
            best = mushroom;
    return best;
}

/// The nearest Underbat in lash range that is not facing the bot
Creature* LashingBat(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* best = nullptr;
    for (ObjectGuid const& guid : ai.GetContext().GetValue<std::vector<ObjectGuid>>("attackers", ai))
    {
        if (guid.GetEntry() != NpcUnderbat)
            continue;
        Creature* bat = ObjectAccessor::GetCreature(*bot, guid);
        if (!bat || !bat->IsAlive() || bot->GetExactDist2d(bat) > LashRange || IsInFrontOf(bat, bot, SafeArc / 2.0f))
            continue;
        if (!best || bot->GetExactDistSq(bat) < bot->GetExactDistSq(best))
            best = bat;
    }
    return best;
}

bool SporesTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcHungarfen }, 80.0f);
    return boss && boss->HasAura(SpellFoulSpores) && bot->GetExactDist2d(boss) < FoulSporesDistance;
}

bool CloudTrigger(PlayerbotAI& ai) { return NearestDangerousMushroom(ai.GetBot()) != nullptr; }

bool BatTrigger(PlayerbotAI& ai) { return ai.GetRole() != BotRole::Tank && LashingBat(ai) != nullptr; }
}

void RegisterUnderbog(AiRegistry& r)
{
    r.AddTrigger("ub foul spores", 0, SporesTrigger);
    r.AddTrigger("ub spore cloud", 0, CloudTrigger);
    r.AddTrigger("ub underbat", 0, BatTrigger);

    AddAction(r, "ub leave spores", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcHungarfen }, 80.0f);
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), FoulSporesDistance);
    });
    AddAction(r, "ub leave cloud", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        Creature* mushroom = NearestDangerousMushroom(bot);
        return mushroom && StepAway(ai, mushroom->GetPositionX(), mushroom->GetPositionY(), CloudRadius + bot->GetCombatReach() + CloudMargin + 1.0f);
    });
    AddAction(r, "ub clear underbat", [](PlayerbotAI& ai)
    {
        Creature* bat = LashingBat(ai);
        return bat && StepAway(ai, bat->GetPositionX(), bat->GetPositionY(), LashRange + 1.0f);
    });

    DungeonDef def;
    def.MapId = MapUnderbog;
    def.Name = "underbog";
    def.Triggers = {
        { "ub foul spores", { { "ub leave spores", DodgeRelevance + 4.0f } }, StrategyStates::Combat },
        { "ub spore cloud", { { "ub leave cloud", DodgeRelevance + 3.0f } }, StrategyStates::Combat },
        { "ub underbat", { { "ub clear underbat", DodgeRelevance + 1.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
