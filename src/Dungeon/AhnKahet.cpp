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
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"

// Ahn'kahet: The Old Kingdom (map 619), after AzerothCore's Ai/Dungeon/AK with TrinityCore's entries and spell ids:
//  * Elder Nadox: while his Ahn'kahar Guardian lives, everybody but the healers kills it first.
//  * Jedoga Shadowseeker: Twilight Volunteers are killed before anything else (everybody but the healers).
//  * Forgotten Ones (the trash before Herald Volazj): the Shadow Crash is dodged. A bot it is aimed at steps away from the target of the
//    cast, and ranged bots and healers keep their distance from each other so the crash does not hit two of them.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapAhnKahet = 619;
constexpr uint32 NpcNadox = 29309;
constexpr uint32 NpcGuardian = 30176;
constexpr uint32 NpcJedoga = 29310;
constexpr uint32 NpcVolunteer = 30385;
constexpr uint32 NpcForgottenOne = 30414;
constexpr float CrashRadius = 10.0f;
constexpr float CrashStepTo = 12.0f;
constexpr float DodgeRelevance = 82.0f;

Unit* AhnKahetTarget(PlayerbotAI& ai)
{
    if (ai.GetRole() == BotRole::Healer)
        return nullptr;

    Player* bot = ai.GetBot();
    if (FindBoss(bot, { NpcNadox }, 80.0f))
        if (Creature* guardian = bot->FindNearestCreature(NpcGuardian, 80.0f, true); guardian && ai.IsValidEnemy(guardian))
            return guardian;

    if (FindBoss(bot, { NpcJedoga }, 80.0f))
        if (Creature* volunteer = bot->FindNearestCreature(NpcVolunteer, 80.0f, true); volunteer && ai.IsValidEnemy(volunteer))
            return volunteer;

    return nullptr;
}

bool ShadowCrashTrigger(PlayerbotAI& ai)
{
    BotRole role = ai.GetRole();
    if (role != BotRole::RangedDps && role != BotRole::Healer)
        return false;
    return FindBoss(ai.GetBot(), { NpcForgottenOne }, 60.0f) != nullptr;
}

Player* NearestGroupMember(Player* bot, float within)
{
    Player* best = nullptr;
    float bestDist = within;
    if (Group* group = bot->GetGroup())
        for (GroupReference const& ref : group->GetMembers())
            if (Player* member = ref.GetSource(); member && member != bot && member->IsAlive() && member->IsInMap(bot))
                if (float dist = bot->GetExactDist2d(member); dist < bestDist)
                {
                    best = member;
                    bestDist = dist;
                }
    return best;
}

bool AvoidShadowCrash(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* mob = FindBoss(bot, { NpcForgottenOne }, 60.0f);
    if (!mob)
        return false;

    // the cast is aimed at somebody: leave that person's side
    if (mob->HasUnitState(UNIT_STATE_CASTING))
        if (Unit* victim = ObjectAccessor::GetUnit(*bot, mob->GetTarget()); victim && victim != bot && bot->GetExactDist2d(victim) < CrashRadius)
            return StepAway(ai, victim->GetPositionX(), victim->GetPositionY(), CrashStepTo);

    // otherwise do not stand on a group member
    if (Player* near = NearestGroupMember(bot, CrashRadius))
        return StepAway(ai, near->GetPositionX(), near->GetPositionY(), CrashStepTo);
    return false;
}
}

void RegisterAhnKahet(AiRegistry& r)
{
    r.AddTrigger("ak shadow crash", 0, ShadowCrashTrigger);
    AddAction(r, "ak avoid shadow crash", AvoidShadowCrash);

    DungeonDef def;
    def.MapId = MapAhnKahet;
    def.Name = "ahn'kahet";
    def.Target = AhnKahetTarget;
    def.Triggers = { { "ak shadow crash", { { "ak avoid shadow crash", DodgeRelevance } }, StrategyStates::Combat } };
    RegisterDungeon(r, std::move(def));
}
}
