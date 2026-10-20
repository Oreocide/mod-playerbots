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

#ifndef Playerbots_GroupCombat_h__
#define Playerbots_GroupCombat_h__

#include "Combat/GroupTactics.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include <vector>

class Group;
class Player;
class Unit;

// The part of the group combat rules that needs the core: reads Group, Unit and ThreatManager, builds the plain views of
// Combat/GroupTactics.h and Combat/Positioning.h, and turns the answers into targets, multipliers and BotMover steps.
// Everything is behind Playerbots.Combat.* keys, default off.
namespace Playerbots
{
class AiRegistry;

/// Who is who in the bot's group on its map, recomputed every 2 s (value "group roles")
struct GroupRolesInfo
{
    std::vector<RoleSlot> Slots;
    ObjectGuid MainTank;
    uint8 MyIndex = 0xFF;
    GroupRole MyRole = GroupRole::MeleeDps;
    bool IAmMainTank = false;
    uint32 Size = 0;            // living members on the bot's map, the bot included
};

namespace GroupCombat
{
/// The role a specialization (or, without one, the class) plays
BotRole SpecRole(Player const* player);
/// The role of a group member: a bot's own, a player's dungeon finder role, else the specialization
GroupRole RoleOf(Group const* group, Player* member);

GroupRolesInfo ComputeRoles(PlayerbotAI& ai);
/// The fight the bot should be in by role (Playerbots.Combat.RoleTargeting); nullptr = use the older rules (no group, no
/// enemies, a master and no tank to follow)
Unit* PickTarget(PlayerbotAI& ai);
/// Playerbots.Combat.ThreatLimit: may the bot keep attacking this enemy?
bool ThreatAllows(PlayerbotAI& ai, Unit* target);
/// Playerbots.Combat.WaitForAttackMs: may the bot start attacking yet?
bool WaitAllows(PlayerbotAI& ai);
/// One repositioning step for this fight (Playerbots.Combat.Positioning / Kite). true = a BotMover step was started
bool PositionStep(PlayerbotAI& ai, Unit* target);

void Register(AiRegistry& registry);
}
}

#endif
