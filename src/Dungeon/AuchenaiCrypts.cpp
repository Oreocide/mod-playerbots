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
#include <algorithm>
#include <cmath>

// Auchenai Crypts (map 558), after AzerothCore's Ai/Dungeon/AC with TrinityCore's entries (Shirrak 18371, Focus Fire 18374, checked against the
// world data). The two stand positions are AzerothCore's, the dungeon geometry is the same as in 3.3.5:
//  * Shirrak the Dead Watcher: the tank takes him to a spot at the end of the corridor; ranged bots and healers wait on an arc at the far
//    end, 25 yards or less from him; everybody leaves the Focus Fire (a creature that marks a spot) by 12 yards and stops the cast to do it.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapAuchenaiCrypts = 558;
constexpr uint32 NpcShirrak = 18371;
constexpr uint32 NpcFocusFire = 18374;
constexpr float FlareSearchRadius = 20.0f;
constexpr float FlareSafeDistance = 12.0f;
constexpr float EmergencyRelevance = 95.0f;
constexpr float PositionRelevance = 63.0f;
constexpr float RangedSpotRadius = 3.0f;

Position const RangedSpot(-21.777f, -162.700f, 26.062f);
Position const TankSpot(-65.171f, -162.920f, 26.504f);

Unit* AuchenaiTarget(PlayerbotAI& ai)
{
    if (ai.GetRole() != BotRole::Tank)
        return nullptr;
    Creature* shirrak = FindBoss(ai.GetBot(), { NpcShirrak }, 80.0f);
    return shirrak && ai.IsValidEnemy(shirrak) ? shirrak : nullptr;
}

bool FocusFireTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return FindBoss(bot, { NpcShirrak }, 80.0f) && FindBoss(bot, { NpcFocusFire }, FlareSearchRadius);
}

bool TankSpotTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (ai.GetRole() != BotRole::Tank)
        return false;
    Creature* shirrak = FindBoss(bot, { NpcShirrak }, 80.0f);
    return shirrak && shirrak->GetVictim() == bot && bot->GetHealthPct() >= 30.0f && bot->GetExactDist2d(TankSpot) > 3.0f;
}

bool RangedSpotTrigger(PlayerbotAI& ai)
{
    BotRole role = ai.GetRole();
    if (role != BotRole::RangedDps && role != BotRole::Healer)
        return false;
    Player* bot = ai.GetBot();
    Creature* shirrak = FindBoss(bot, { NpcShirrak }, 80.0f);
    return shirrak && bot->GetExactDist(shirrak) - shirrak->GetCombatReach() <= sPlayerbotsConfig.RangedDistance;
}

bool RangedSpotAction(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    GroupRolesInfo roles = ai.GetContext().GetValue<GroupRolesInfo>("group roles", ai);

    // my place on the arc among the ranged members and healers, in member order
    int slot = 0;
    int count = 0;
    for (RoleSlot const& s : roles.Slots)
    {
        if (s.Index == 0xFF || (s.Role != GroupRole::RangedDps && s.Role != GroupRole::Healer))
            continue;
        if (s.Id == bot->GetGUID().GetCounter())
            slot = count;
        ++count;
    }

    constexpr float arcSpan = Pi / 2.0f;
    float arcStart = Pi - arcSpan / 2.0f;
    float angle = count <= 1 ? Pi : arcStart + arcSpan * float(slot) / float(count - 1);
    Position target(RangedSpot.GetPositionX() + std::cos(angle) * RangedSpotRadius, RangedSpot.GetPositionY() + std::sin(angle) * RangedSpotRadius, RangedSpot.GetPositionZ());
    if (bot->GetExactDist2d(target) <= 4.0f)
        return false;
    return MoveCombat(bot, target, 2.0f, MovePriority::Position) != MoveResult::NoPath;
}
}

void RegisterAuchenaiCrypts(AiRegistry& r)
{
    r.AddTrigger("ac focus fire", 0, FocusFireTrigger);
    r.AddTrigger("ac tank spot", 0, TankSpotTrigger);
    r.AddTrigger("ac ranged spot", 0, RangedSpotTrigger);

    AddAction(r, "ac leave focus fire", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        Creature* flare = FindBoss(bot, { NpcFocusFire }, FlareSearchRadius);
        if (!flare || bot->GetExactDist2d(flare) >= FlareSafeDistance)
            return false;
        bot->InterruptNonMeleeSpells(false);
        return StepAway(ai, flare->GetPositionX(), flare->GetPositionY(), FlareSafeDistance);
    });
    AddAction(r, "ac tank to spot", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        return MoveCombat(bot, TankSpot, 3.0f, MovePriority::Position) != MoveResult::NoPath;
    });
    AddAction(r, "ac ranged to spot", RangedSpotAction);

    DungeonDef def;
    def.MapId = MapAuchenaiCrypts;
    def.Name = "auchenai crypts";
    def.Target = AuchenaiTarget;
    def.Triggers = {
        { "ac focus fire", { { "ac leave focus fire", EmergencyRelevance } }, StrategyStates::Combat },
        { "ac tank spot", { { "ac tank to spot", PositionRelevance } }, StrategyStates::Combat },
        { "ac ranged spot", { { "ac ranged to spot", PositionRelevance } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
