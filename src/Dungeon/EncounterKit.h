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

#ifndef Playerbots_EncounterKit_h__
#define Playerbots_EncounterKit_h__

#include "Define.h"
#include "Engine/Engine.h"
#include "Travel/MovePriority.h"
#include <functional>
#include <initializer_list>
#include <memory>
#include <vector>

class Creature;
class Player;
class Unit;

// Encounter logic for dungeons (Playerbots.Dungeon.Enable): what a bot does in a boss fight beyond its rotation, written as small rules
// on the targeting, positioning and engine layers (ideas and mechanics from AzerothCore's Ai/Dungeon, rewritten for this engine, TC
// entries and difficulty ids). A dungeon registers (1) a strategy that the context table gives to bots in that map and (2) optionally a
// target rule that overrides the bot's combat target during the fight. Everything here runs on the bot's own map thread.
namespace Playerbots
{
class AiRegistry;
class PlayerbotAI;

namespace Dungeon
{
using TargetRule = Unit* (*)(PlayerbotAI& ai);

/// The rule is consulted for bots in `mapId` that run `strategy` (so `co -name` switches it off)
void RegisterTargetRule(uint32 mapId, char const* strategy, TargetRule rule);

/// The encounter's choice of target for this bot, nullptr = no rule applies, use the normal rules. Called by PlayerbotAI::FindCombatTarget
Unit* EncounterTarget(PlayerbotAI& ai);

/// The nearest living creature of any of the entries within range of the bot (a boss in either of its forms)
Creature* FindBoss(Player* bot, std::initializer_list<uint32> entries, float range = 100.0f);

/// The bot or a member of its group carries the aura
bool GroupMemberHasAura(Player* bot, uint32 spellId);

/// The unit is casting (or channeling) one of the spells right now
bool IsCasting(Unit const* unit, std::initializer_list<uint32> spellIds);

/// A unit's aura, any of the spell ids
bool HasAnyAura(Unit const* unit, std::initializer_list<uint32> spellIds);

/// The first of the entries (in the order given) that has a valid enemy attacking the bot's group: the priority list of a fight
Unit* FirstAttacker(PlayerbotAI& ai, std::initializer_list<uint32> entries);

/// `who` stands in the front arc of `boss` (halfArc radians each side of where it faces)
bool IsInFrontOf(Unit const* boss, Unit const* who, float halfArc = 1.5708f);

/// A navmesh step to a point at least minDist from (x, y), the nearest such point that does not turn the bot back the way it came.
/// false if the bot is already far enough or no step was found
bool StepAway(PlayerbotAI& ai, float x, float y, float minDist, MovePriority priority = MovePriority::Avoid);

/// A navmesh step to a point `behindYards` behind the unit (out of its front arc)
bool StepBehind(PlayerbotAI& ai, Unit const* boss, float behindYards = 3.0f, MovePriority priority = MovePriority::Avoid);

using ActionFn = std::function<bool(PlayerbotAI&)>;
using MultiplierFn = std::function<float(PlayerbotAI&, Action*)>;

/// An action made of one function (true = it did something)
void AddAction(AiRegistry& registry, std::string name, ActionFn fn);

/// A dungeon: a strategy named `Name` that the context table gives to bots in a group inside map `MapId` (Playerbots.Dungeon.Enable),
/// its trigger -> action nodes (triggers and actions are registered by the dungeon file), multipliers and an optional target rule
struct DungeonDef
{
    uint32 MapId = 0;
    char const* Name = "";
    TargetRule Target = nullptr;
    std::vector<TriggerNode> Triggers;
    std::vector<MultiplierFn> Multipliers;
};
void RegisterDungeon(AiRegistry& registry, DungeonDef def);

/// Registers every dungeon of the module (called once from RegisterGenericAi)
void RegisterAll(AiRegistry& registry);
}
}

#endif
