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
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "Travel/BotMover.h"
#include <cmath>
#include <list>

// Pit of Saron (map 658), after AzerothCore's Ai/Dungeon/PoS with TrinityCore's entries and spell ids (checked; the heroic Poison Nova id 70434 is
// not in the 12.1 client data, the normal one carries the heroic values):
//  * Ick and Krick: Pursuit (68987: the bot has it, or Ick casts it and the bot is not the tank): the bot runs to a point 20 yards from the tank
//    spot, sideways, away from Ick; Poison Nova (68989): out of 20 yards and nothing else until it ends; Exploding Orbs (units with aura 69017,
//    the Explosive Barrage): nobody stays within 7 yards of one; the tank takes Ick to AzerothCore's spot.
//  * Scourgelord Tyrannus: once he is hurt, ranged bots keep 10 yards from each other.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapPitOfSaron = 658;
constexpr uint32 NpcIck = 36476;
constexpr uint32 NpcKrick = 36477;
constexpr uint32 NpcTyrannus = 36658;
constexpr uint32 SpellPursuit = 68987;
constexpr uint32 SpellOrbVisual = 69017;
constexpr float NovaDistance = 20.0f;
constexpr float OrbDistance = 7.0f;
constexpr float SpreadDistance = 10.0f;
constexpr float DodgeRelevance = 82.0f;

Position const TankSpot(816.8508f, 102.331505f, 509.1586f);

Creature* Ick(Player* bot) { return FindBoss(bot, { NpcIck, NpcKrick }, 100.0f); }

bool Pursuing(PlayerbotAI& ai, Creature* boss)
{
    Player* bot = ai.GetBot();
    return ai.GetRole() != BotRole::Tank && (bot->HasAura(SpellPursuit) || IsCasting(boss, { SpellPursuit }));
}

Creature* NearestOrb(Player* bot)
{
    Creature* best = nullptr;
    // the orbs are units with the Exploding Orb aura: look at the creatures around that have it
    std::list<Creature*> nearby;
    bot->GetCreatureListWithEntryInGrid(nearby, 0, 30.0f);
    for (Creature* creature : nearby)
        if (creature->IsAlive() && creature->HasAura(SpellOrbVisual) && (!best || bot->GetExactDistSq(creature) < bot->GetExactDistSq(best)))
            best = creature;
    return best;
}

bool IckTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = Ick(bot);
    if (!boss)
        return false;
    if (Pursuing(ai, boss) && bot->GetExactDist2d(boss) < 20.0f)
        return true;
    if (IsCasting(boss, { 68989, 70434 }))
        return true;
    if (NearestOrb(bot) && bot->GetExactDist2d(NearestOrb(bot)) < OrbDistance)
        return true;
    return ai.GetRole() == BotRole::Tank && boss->GetVictim() == bot && bot->GetExactDist2d(TankSpot) > 7.0f;
}

bool IckAction(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = Ick(bot);
    if (!boss)
        return false;

    // Pursuit: sideways to 20 yards from the tank spot
    if (Pursuing(ai, boss) && bot->GetExactDist2d(boss) < 20.0f)
    {
        float dx = bot->GetPositionX() - TankSpot.GetPositionX();
        float dy = bot->GetPositionY() - TankSpot.GetPositionY();
        float length = std::hypot(dx, dy);
        if (length > 0.0f)
        {
            dx /= length;
            dy /= length;
        }
        Position target(TankSpot.GetPositionX() - dy * 20.0f, TankSpot.GetPositionY() + dx * 20.0f, bot->GetPositionZ());
        Position snapped;
        if (SnapToMesh(bot, target, snapped, 3.0f, 2.5f))
            return MoveCombat(bot, snapped, 1.5f, MovePriority::Avoid) != MoveResult::NoPath;
        return false;
    }

    // Poison Nova: get out, or hold still when far enough
    if (IsCasting(boss, { 68989, 70434 }))
        return bot->GetExactDist2d(boss) >= NovaDistance || StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), NovaDistance);

    if (Creature* orb = NearestOrb(bot); orb && bot->GetExactDist2d(orb) < OrbDistance)
        return StepAway(ai, orb->GetPositionX(), orb->GetPositionY(), OrbDistance + 1.0f);

    if (ai.GetRole() == BotRole::Tank && boss->GetVictim() == bot && bot->GetExactDist2d(TankSpot) > 7.0f)
        return MoveCombat(bot, TankSpot, 4.0f, MovePriority::Position) != MoveResult::NoPath;
    return false;
}

bool TyrannusTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    BotRole role = ai.GetRole();
    if (role != BotRole::RangedDps)
        return false;
    Creature* boss = FindBoss(bot, { NpcTyrannus }, 100.0f);
    if (!boss || boss->GetHealthPct() >= 99.0f)
        return false;
    if (Group* group = bot->GetGroup())
        for (GroupReference const& ref : group->GetMembers())
            if (Player* member = ref.GetSource(); member && member != bot && member->IsAlive() && member->IsInMap(bot) && bot->GetExactDist2d(member) < SpreadDistance)
                return true;
    return false;
}
}

void RegisterPitOfSaron(AiRegistry& r)
{
    r.AddTrigger("pos ick", 0, IckTrigger);
    r.AddTrigger("pos tyrannus", 0, TyrannusTrigger);
    AddAction(r, "pos ick", IckAction);
    AddAction(r, "pos spread", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        if (Group* group = bot->GetGroup())
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource(); member && member != bot && member->IsAlive() && member->IsInMap(bot) && bot->GetExactDist2d(member) < SpreadDistance)
                    return StepAway(ai, member->GetPositionX(), member->GetPositionY(), SpreadDistance + 1.0f);
        return false;
    });

    DungeonDef def;
    def.MapId = MapPitOfSaron;
    def.Name = "pit of saron";
    def.Triggers = {
        { "pos ick", { { "pos ick", DodgeRelevance + 3.0f } }, StrategyStates::Combat },
        { "pos tyrannus", { { "pos spread", DodgeRelevance - 2.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
