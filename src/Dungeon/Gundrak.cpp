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
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "Travel/BotMover.h"
#include <algorithm>
#include <list>
#include <vector>

// Gundrak (map 604), after AzerothCore's Ai/Dungeon/GD with TrinityCore's entries and spell ids (checked against the world and client data):
//  * Slad'ran: everybody leaves his Poison Nova (17 yards); a damage dealer that is not wrapped goes for the nearest Snake Wrap that no
//    closer bot of the group is already freeing; the rest of the group that is not busy with a wrap stays within 10 yards of the tank; the tank
//    keeps the boss unless one of the adds is within 12 yards of it.
//  * Gal'darah: Whirling Slash (aura 55250 / 59824): stay 7 yards from him, the tank 12.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapGundrak = 604;
constexpr uint32 NpcSladran = 29304;
constexpr uint32 NpcGaldarah = 29306;
constexpr uint32 NpcSnakeWrap = 29742;
constexpr uint32 NpcViper = 29680;
constexpr uint32 NpcConstrictor = 29713;
constexpr uint32 SpellSnakeWrap = 55126;
constexpr float NovaDistance = 17.0f;
constexpr float StackLeash = 10.0f;
constexpr float StackCloseTo = 5.0f;
constexpr float TankPickup = 12.0f;
constexpr float SlashRadius = 7.0f;
constexpr float SlashTankRadius = 12.0f;
constexpr float DodgeRelevance = 82.0f;

bool IsDamage(BotRole role) { return role == BotRole::MeleeDps || role == BotRole::RangedDps; }
bool IsAdd(Unit const* unit) { return unit && (unit->GetEntry() == NpcViper || unit->GetEntry() == NpcConstrictor); }

/// The Snake Wrap this damage dealer should free: the nearest one in line of sight that no closer bot of the group (also free and in sight of
/// it) is already going for
Unit* AssignedSnakeWrap(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || !bot->IsInCombat() || !IsDamage(ai.GetRole()) || bot->HasAura(SpellSnakeWrap) || !bot->GetGroup())
        return nullptr;

    std::list<Creature*> wraps;
    bot->GetCreatureListWithEntryInGrid(wraps, NpcSnakeWrap, 60.0f);
    std::vector<std::pair<float, Creature*>> candidates;
    for (Creature* wrap : wraps)
        if (wrap->IsAlive() && ai.IsValidEnemy(wrap))
            candidates.emplace_back(bot->GetExactDist2d(wrap), wrap);
    std::ranges::sort(candidates, {}, &std::pair<float, Creature*>::first);

    Group* group = bot->GetGroup();
    for (auto const& [distance, wrap] : candidates)
    {
        if (!bot->IsWithinLOSInMap(wrap))
            continue;

        bool someoneFreer = false;
        for (GroupReference const& ref : group->GetMembers())
        {
            Player* member = ref.GetSource();
            if (!member || member == bot || !member->IsAlive() || !member->IsInCombat() || !member->IsInMap(bot) || member->HasAura(SpellSnakeWrap)
                || !sPlayerbotMgr->GetAI(member) || !IsDamage(sPlayerbotMgr->GetAI(member)->GetRole()))
                continue;
            float memberDistance = member->GetExactDist2d(wrap);
            if (memberDistance > distance || (memberDistance == distance && bot->GetGUID() < member->GetGUID()))
                continue;
            if (member->IsWithinLOSInMap(wrap))
            {
                someoneFreer = true;
                break;
            }
        }
        if (!someoneFreer)
            return wrap;
    }
    return nullptr;
}

/// The main tank, for a bot that should be next to it (not the tank, not busy with a wrap, farther than the leash)
Player* StackTank(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || ai.GetRole() == BotRole::Tank || !FindBoss(bot, { NpcSladran }, 80.0f) || AssignedSnakeWrap(ai))
        return nullptr;

    Unit* victim = bot->GetVictim();
    if (victim && victim->IsAlive() && victim->GetEntry() == NpcSnakeWrap)
        return nullptr;

    GroupRolesInfo roles = ai.GetContext().GetValue<GroupRolesInfo>("group roles", ai);
    Player* tank = roles.MainTank.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, roles.MainTank);
    if (!tank || tank == bot || !tank->IsInMap(bot) || bot->GetExactDist2d(tank) <= StackLeash)
        return nullptr;
    return tank;
}

Unit* GundrakTarget(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcSladran }, 80.0f);
    if (!boss)
        return nullptr;

    BotRole role = ai.GetRole();
    if (role == BotRole::Tank)
    {
        // an add that is within reach of the tank stays the target; otherwise the boss is held
        Unit* victim = bot->GetVictim();
        if (victim && victim->IsAlive() && (victim == boss || (IsAdd(victim) && bot->GetExactDist2d(victim) <= TankPickup)))
            return nullptr;
        return ai.IsValidEnemy(boss) ? boss : nullptr;
    }
    return IsDamage(role) ? AssignedSnakeWrap(ai) : nullptr;
}

bool NovaTrigger(PlayerbotAI& ai)
{
    Creature* boss = FindBoss(ai.GetBot(), { NpcSladran }, 80.0f);
    return boss && IsCasting(boss, { 55081, 59842 }) && ai.GetBot()->GetExactDist2d(boss) < NovaDistance;
}

bool StackTrigger(PlayerbotAI& ai) { return StackTank(ai) != nullptr; }

bool SlashTrigger(PlayerbotAI& ai)
{
    Creature* boss = FindBoss(ai.GetBot(), { NpcGaldarah }, 60.0f);
    return boss && HasAnyAura(boss, { 55250, 59824 });
}
}

void RegisterGundrak(AiRegistry& r)
{
    r.AddTrigger("gd poison nova", 0, NovaTrigger);
    r.AddTrigger("gd stack on tank", 0, StackTrigger);
    r.AddTrigger("gd whirling slash", 0, SlashTrigger);

    AddAction(r, "gd leave nova", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcSladran }, 80.0f);
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), NovaDistance);
    });
    AddAction(r, "gd stack on tank", [](PlayerbotAI& ai)
    {
        Player* tank = StackTank(ai);
        return tank && MoveCombat(ai.GetBot(), tank->GetPosition(), StackCloseTo, MovePriority::Position) != MoveResult::NoPath;
    });
    AddAction(r, "gd leave slash", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcGaldarah }, 60.0f);
        float radius = ai.GetRole() == BotRole::Tank ? SlashTankRadius : SlashRadius;
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), radius);
    });

    DungeonDef def;
    def.MapId = MapGundrak;
    def.Name = "gundrak";
    def.Target = GundrakTarget;
    def.Triggers = {
        { "gd poison nova", { { "gd leave nova", DodgeRelevance + 2.0f } }, StrategyStates::Combat },
        { "gd whirling slash", { { "gd leave slash", DodgeRelevance + 2.0f } }, StrategyStates::Combat },
        { "gd stack on tank", { { "gd stack on tank", 61.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
