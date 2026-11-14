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

#ifndef Playerbots_BotLoot_h__
#define Playerbots_BotLoot_h__

#include "Define.h"

namespace Playerbots
{
class AiRegistry;
}

/// Looting like a player (AC parity, mod-playerbots#38): walk to the corpse, keep the loot window open for a moment so the
/// kneel shows (retail holds it a median 1.0 s), take what the "ll" loot filter allows, close every loot the open put in view.
/// Then skin the corpse, and open chests. Replaces the generic "loot" action, "corpse to loot" trigger and "nearest corpse"
/// value, so Register runs after RegisterGenericAi's own registrations.
namespace Playerbots::BotLooting
{
struct Config
{
    bool Visible = false;       // Playerbots.Loot.Visible: walk over and hold the window (0: loot from where the bot stands, at once)
    uint32 HoldMinMs = 500;     // Playerbots.Loot.HoldMinMs / HoldMaxMs: how long the window stays open
    uint32 HoldMaxMs = 2000;
    uint32 InstantPct = 10;     // Playerbots.Loot.InstantPct: share of loots taken at once (auto loot)
    bool Skin = false;          // Playerbots.Loot.Skin: skin, or gather from, a looted corpse the bot has the skill for
    bool Chests = false;        // Playerbots.Loot.Chests: open chests in the open world, shared ones too
    float MasterLeash = 30.0f;  // Playerbots.Loot.MasterLeash: a party bot leaves loot farther than this from its master
};

Config const& GetConfig();
void LoadConfig();
/// World thread, after the spell store is loaded: finds the spells that open unskilled locks ("Opening")
void OnStartup();

void Register(AiRegistry& registry);
}

#endif
