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
#include "Travel/BotMover.h"

// Halls of Lightning (map 602), after AzerothCore's Ai/Dungeon/HoL with TrinityCore's entries and spell ids (checked; heroic ids fold into the
// normal ones in the 12.1 data):
//  * General Bjarngrim: damage dealers kill a Stormforged Lieutenant first; his Whirlwind (52027) is left by 10 yards.
//  * Volkhan: damage dealers hit him, not the adds.
//  * Ionar: a bot carrying Static Overload (52658) is left by 10 yards by the others; Ball Lightning (52780): ranged bots keep 7 yards from
//    each other; the tank holds him at AzerothCore's spot; when he disperses (he is not visible, or casts Disperse 52770) everybody goes to the
//    disperse spot.
//  * Loken: ranged bots stand on him (2 yards, hunters 6.5) while he is not casting; Lightning Nova (52960) is left by 22 yards.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapHallsOfLightning = 602;
constexpr uint32 NpcBjarngrim = 28586;
constexpr uint32 NpcLieutenant = 29240;
constexpr uint32 NpcVolkhan = 28587;
constexpr uint32 NpcIonar = 28546;
constexpr uint32 NpcLoken = 28923;
constexpr uint32 SpellStaticOverload = 52658;
constexpr float WhirlwindDistance = 10.0f;
constexpr float OverloadDistance = 10.0f;
constexpr float BallSpread = 7.0f;
constexpr float NovaDistance = 22.0f;
constexpr float DodgeRelevance = 82.0f;

Position const IonarTankSpot(1078.860f, -261.928f, 61.226f);
Position const DisperseSpot(1161.152f, -261.584f, 53.223f);

bool IsDamage(BotRole role) { return role == BotRole::MeleeDps || role == BotRole::RangedDps; }

Unit* LightningTarget(PlayerbotAI& ai)
{
    if (!IsDamage(ai.GetRole()))
        return nullptr;

    Player* bot = ai.GetBot();
    if (FindBoss(bot, { NpcBjarngrim }, 100.0f))
        if (Unit* lieutenant = FirstAttacker(ai, { NpcLieutenant }))
            return lieutenant;

    if (Creature* volkhan = FindBoss(bot, { NpcVolkhan }, 100.0f); volkhan && ai.IsValidEnemy(volkhan))
        return volkhan;
    return nullptr;
}

Player* NearestOtherMember(Player* bot, float within, bool wantOverload)
{
    Player* best = nullptr;
    float bestDist = within;
    if (Group* group = bot->GetGroup())
        for (GroupReference const& ref : group->GetMembers())
            if (Player* member = ref.GetSource(); member && member != bot && member->IsAlive() && member->IsInMap(bot) && (!wantOverload || member->HasAura(SpellStaticOverload)))
                if (float dist = bot->GetExactDist2d(member); dist < bestDist)
                {
                    best = member;
                    bestDist = dist;
                }
    return best;
}

bool WhirlwindTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcBjarngrim }, 80.0f);
    return boss && IsCasting(boss, { 52027 }) && bot->GetExactDist2d(boss) < WhirlwindDistance;
}

bool OverloadTrigger(PlayerbotAI& ai) { return NearestOtherMember(ai.GetBot(), OverloadDistance, true) != nullptr; }

bool BallLightningTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (ai.GetRole() == BotRole::MeleeDps || ai.GetRole() == BotRole::Tank)
        return false;
    Creature* boss = FindBoss(bot, { NpcIonar }, 80.0f);
    return boss && IsCasting(boss, { 52780, 59800 }) && NearestOtherMember(bot, BallSpread, false);
}

bool IonarTankTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcIonar }, 80.0f);
    return boss && ai.GetRole() == BotRole::Tank && boss->GetVictim() == bot && bot->GetExactDist2d(IonarTankSpot) > 10.0f;
}

bool DisperseTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcIonar }, 100.0f);
    return boss && (!bot->CanSeeOrDetect(boss) || IsCasting(boss, { 52770 })) && bot->GetExactDist2d(DisperseSpot) > 8.0f;
}

bool LokenStackTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    BotRole role = ai.GetRole();
    if (role == BotRole::MeleeDps || role == BotRole::Tank)
        return false;
    Creature* boss = FindBoss(bot, { NpcLoken }, 80.0f);
    float stay = bot->GetClass() == CLASS_HUNTER ? 6.5f : 2.0f;
    return boss && !boss->HasUnitState(UNIT_STATE_CASTING) && bot->GetExactDist2d(boss) > stay;
}

bool LokenNovaTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcLoken }, 80.0f);
    return boss && IsCasting(boss, { 52960, 59835 }) && bot->GetExactDist2d(boss) < NovaDistance;
}
}

void RegisterHallsOfLightning(AiRegistry& r)
{
    r.AddTrigger("hol whirlwind", 0, WhirlwindTrigger);
    r.AddTrigger("hol overload", 0, OverloadTrigger);
    r.AddTrigger("hol ball lightning", 0, BallLightningTrigger);
    r.AddTrigger("hol ionar tank", 0, IonarTankTrigger);
    r.AddTrigger("hol disperse", 0, DisperseTrigger);
    r.AddTrigger("hol loken stack", 0, LokenStackTrigger);
    r.AddTrigger("hol loken nova", 0, LokenNovaTrigger);

    AddAction(r, "hol leave whirlwind", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcBjarngrim }, 80.0f);
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), WhirlwindDistance);
    });
    AddAction(r, "hol leave overload", [](PlayerbotAI& ai)
    {
        Player* carrier = NearestOtherMember(ai.GetBot(), OverloadDistance, true);
        return carrier && StepAway(ai, carrier->GetPositionX(), carrier->GetPositionY(), OverloadDistance + 1.0f);
    });
    AddAction(r, "hol spread ball lightning", [](PlayerbotAI& ai)
    {
        Player* near = NearestOtherMember(ai.GetBot(), BallSpread, false);
        return near && StepAway(ai, near->GetPositionX(), near->GetPositionY(), BallSpread + 1.0f);
    });
    AddAction(r, "hol ionar spot", [](PlayerbotAI& ai) { return MoveCombat(ai.GetBot(), IonarTankSpot, 4.0f, MovePriority::Position) != MoveResult::NoPath; });
    AddAction(r, "hol disperse spot", [](PlayerbotAI& ai) { return MoveCombat(ai.GetBot(), DisperseSpot, 4.0f, MovePriority::Avoid) != MoveResult::NoPath; });
    AddAction(r, "hol loken stack", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        Creature* boss = FindBoss(bot, { NpcLoken }, 80.0f);
        float stay = bot->GetClass() == CLASS_HUNTER ? 6.5f : 2.0f;
        return boss && MoveCombat(bot, boss->GetPosition(), stay, MovePriority::Position) != MoveResult::NoPath;
    });
    AddAction(r, "hol leave nova", [](PlayerbotAI& ai)
    {
        Creature* boss = FindBoss(ai.GetBot(), { NpcLoken }, 80.0f);
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), NovaDistance);
    });

    DungeonDef def;
    def.MapId = MapHallsOfLightning;
    def.Name = "halls of lightning";
    def.Target = LightningTarget;
    def.Triggers = {
        { "hol whirlwind", { { "hol leave whirlwind", DodgeRelevance + 1.0f } }, StrategyStates::Combat },
        { "hol loken nova", { { "hol leave nova", DodgeRelevance + 2.0f } }, StrategyStates::Combat },
        { "hol overload", { { "hol leave overload", DodgeRelevance - 1.0f } }, StrategyStates::Combat },
        { "hol ball lightning", { { "hol spread ball lightning", DodgeRelevance - 2.0f } }, StrategyStates::Combat },
        { "hol disperse", { { "hol disperse spot", DodgeRelevance + 3.0f } }, StrategyStates::Combat },
        { "hol ionar tank", { { "hol ionar spot", DodgeRelevance - 3.0f } }, StrategyStates::Combat },
        { "hol loken stack", { { "hol loken stack", DodgeRelevance - 3.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
