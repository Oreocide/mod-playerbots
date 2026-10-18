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

#ifndef PlayerbotsBotProfessions_h__
#define PlayerbotsBotProfessions_h__

#include "ObjectGuid.h"

class Player;

namespace Playerbots::BotProfessions
{
/// A player of level 10+ has two primary professions that go together (herbalism + alchemy, mining + blacksmithing,
/// skinning + leatherworking...) plus cooking and fishing. The pair is chosen once from the character's guid. A character
/// that SeedOffline has seeded already has everything at 900; others follow the character's level (the gathering locks are
/// checked against the main skill line).
void Maintain(Player* bot);

/// Raises the profession skills of an offline character to 900 (its primaries or the pair it would draw, cooking, fishing, every
/// expansion line), so that no skill is raised in the world and the achievements those skills complete are not announced to the
/// guild and to nearby players.
void SeedOffline(ObjectGuid guid);

/// true when the bot has herbalism or mining
bool CanGather(Player const* bot);
}

#endif
