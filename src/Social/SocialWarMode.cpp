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

// War Mode (no AzerothCore counterpart: 3.3.5 has none). About one bot in three wants War Mode (user decision 2026-11-13,
// stable by guid, level 20 or more); a bot in a player's party mirrors that player's choice. The core does the zone part:
// Player::UpdateWarModeAuras makes the flag active only where War Mode applies. Near a player, a free bot in active War
// Mode picks a fight with a hostile player in active War Mode close by (bots and players alike) within a level window.
//
// The core's own toggle (Player::SetWarModeDesired) only works while resting in a city that allows it; a bot is taken to
// have chosen it the last time it was in town, so the same three steps are done here without the place check.

#include "Engine/AiRegistry.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Social/SocialConfig.h"
#include "Social/SocialShared.h"

namespace Playerbots::Social
{
namespace
{
bool WantsWarMode(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    // a master on another map is updated by another map thread (MapUpdate.Threads > 1): keep the bot's setting until they share one
    if (Player* master = ai.GetMaster())
        return bot->IsInMap(master) ? master->IsWarModeDesired() : bot->IsWarModeDesired();
    return bot->GetLevel() >= sSocialConfig.WarModeMinLevel && bot->GetGUID().GetCounter() * 2654435761u % sSocialConfig.WarModeOneIn == 0;
}

bool WarModeDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.WarModeEnable)
        return false;
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsInCombat() || bot->InBattleground() || bot->InArena() || !ai.IsSimulated())
        return false;
    return WantsWarMode(ai) != bot->IsWarModeDesired();
}

class SetWarModeAction final : public Action
{
public:
    char const* GetName() const override { return "social war mode set"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        bool wanted = WantsWarMode(ai);
        if (wanted == bot->IsWarModeDesired())
            return false;
        // Player::SetWarModeDesired without its "resting in a War Mode city" check
        if (wanted)
            bot->SetPlayerFlag(PLAYER_FLAGS_WAR_MODE_DESIRED);
        else
            bot->RemovePlayerFlag(PLAYER_FLAGS_WAR_MODE_DESIRED);
        bot->SetPvP(wanted);
        bot->UpdateWarModeAuras();
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} turns War Mode {}", bot->GetName(), wanted ? "on" : "off");
        return true;
    }
};

Player* WarModeEnemy(Player* bot)
{
    uint32 gap = sSocialConfig.WarModeLevelGap;
    auto check = [bot, gap](Player* other)
    {
        return other && other != bot && other->IsAlive() && other->IsWarModeActive() && !bot->IsInSameGroupWith(other)
            && std::abs(int32(other->GetLevel()) - int32(bot->GetLevel())) <= int32(gap) && bot->IsValidAttackTarget(other)
            && !other->IsBeingTeleported();
    };
    std::vector<Player*> found;
    Trinity::PlayerListSearcher searcher(bot, found, check);
    Cell::VisitWorldObjects(bot, searcher, sSocialConfig.WarModeRange);

    Player* best = nullptr;
    for (Player* other : found)
        if (!best || bot->GetExactDistSq(other) < bot->GetExactDistSq(best))
            best = other;
    return best;
}

bool WarModeEnemyNear(PlayerbotAI& ai)
{
    if (!sSocialConfig.WarModeEnable || !IsFreeRandomBot(ai))
        return false;
    Player* bot = ai.GetBot();
    if (!bot->IsWarModeActive() || !bot->IsAlive() || bot->IsInCombat() || bot->duel || (bot->IsMounted() && bot->IsFlying()) || !ai.IsSimulated())
        return false;
    // a target set earlier (by this, "world react" or a battleground) that is dead, gone or no longer attackable is let go
    if (ObjectGuid forced = ai.GetForcedTarget(); !forced.IsEmpty())
    {
        if (ai.IsValidEnemy(ObjectAccessor::GetUnit(*bot, forced)))
            return false;
        ai.ClearForcedTarget();
    }
    return WarModeEnemy(bot) != nullptr;
}

class WarModeFightAction final : public Action
{
public:
    char const* GetName() const override { return "social war mode fight"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Player* enemy = WarModeEnemy(bot);
        if (!enemy)
            return false;
        ai.SetForcedTarget(enemy->GetGUID());
        bot->SetSelection(enemy->GetGUID());
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} attacks {} (War Mode)", bot->GetName(), enemy->GetName());
        return true;
    }
};
}

void RegisterWarMode(AiRegistry& registry)
{
    registry.AddTrigger("social war mode due", 15000, WarModeDue, IntervalMode::Gate, 5000);
    registry.AddAction<SetWarModeAction>("social war mode set");
    registry.AddTrigger("social war mode enemy", 2000, WarModeEnemyNear, IntervalMode::Gate, 1000);
    registry.AddAction<WarModeFightAction>("social war mode fight");
}
}
