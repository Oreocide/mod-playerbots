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

#ifndef Playerbots_PartyOrders_h__
#define Playerbots_PartyOrders_h__

#include "Define.h"

class Unit;

namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;
}

/// Orders a master gives party bots in chat (AC parity, mod-playerbots#38): formation, range, rti, focus heal, save mana, position
/// (AC "rtsc": spots are taken from where the master stands, not from a click), taxi, los, reputation. Each order is per bot and
/// lasts until changed or the bot logs out. The hooks below are what the follow, chase and heal code ask; every one returns the
/// unchanged default while no order was given.
namespace Playerbots::PartyOrders
{
/// PlayerbotAI::Follow: the standoff and angle for this bot's place in the formation. false = the default spread
bool FormationOffset(PlayerbotAI& ai, uint8 index, float baseDistance, float& distance, float& angle);
/// PlayerbotAI::Chase for ranged bots, GroupCombat's ranged slots: the distance the master asked for, else fallback
float RangedDistance(PlayerbotAI& ai, float fallback);
/// Heal target pickers: false when the master gave this healer a focus list and the unit is not on it (the bot itself always is)
bool MayHeal(PlayerbotAI& ai, Unit const* unit);
/// The health percentage a healer heals at, lowered by "save mana"
float HealThreshold(PlayerbotAI& ai, float configured);
/// The mana percentage above which a healer also deals damage, raised by "save mana"
float DamageMinMana(PlayerbotAI& ai, float configured);
/// Raid icon mask from "rti <icons>" (RaidIconRules bit = icon index), 0 = the Playerbots.Combat.Icon.* default
uint32 RtiMask(PlayerbotAI& ai);

void Register(AiRegistry& registry);
}

#endif
