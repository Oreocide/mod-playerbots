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

#ifndef Playerbots_ProfessionsConfig_h__
#define Playerbots_ProfessionsConfig_h__

#include "Define.h"

// The keys of the professions group of the AC parity work (docs/design/AC_PARITY.md, group D): crafting for every profession,
// disenchanting, enchanting, recipes, outfits, lockboxes and fishing. Kept out of PlayerbotsConfig so that header keeps its
// layout; loaded by the playerbots_professions world script (ProfessionsAi.cpp) at start and on .reload config. Every switch is
// off by default; the defaults here equal conf/playerbots.conf.dist.
namespace Playerbots::Prof
{
struct Config
{
    // Playerbots.Craft.AnyProfession: the rpg "craft" activity uses every production profession the bot has (at the station its
    // recipe needs, or anywhere when it needs none) instead of blacksmithing at a forge in a capital only
    bool CraftAny = false;
    float CraftStationYards = 80.0f;        // how far a station (forge, anvil, cooking fire...) may be
    uint32 CraftMaxCasts = 3;               // items made per visit at most

    // Playerbots.Disenchant.Enable: enchanters disenchant soulbound uncommon+ gear they cannot use or that is no upgrade
    bool Disenchant = false;
    uint32 DisenchantIntervalMs = 60000;
    uint32 DisenchantMaxQuality = 4;        // 2 uncommon, 3 rare, 4 epic

    // Playerbots.Enchant.Enable: enchanters enchant their own unenchanted gear with an enchant they know and carry reagents for
    bool Enchant = false;
    uint32 EnchantIntervalMs = 300000;

    // Playerbots.Recipes.Enable: recipe items in the bags are learned when the bot can
    bool Recipes = false;
    uint32 RecipesIntervalMs = 60000;

    // Playerbots.Outfits.Enable: the "outfit" order (save, equip, list, delete named gear sets)
    bool Outfits = false;
    uint32 OutfitsMax = 20;                 // the client's equipment set limit

    // Playerbots.Lockbox.Enable: lockboxes, clams and other bag items with loot are opened out of combat; a rogue picks a locked one
    bool Lockbox = false;
    uint32 LockboxIntervalMs = 30000;

    // Playerbots.Fishing.Enable: an idle bot with fishing near water fishes a few casts (real loot, pools too)
    bool Fishing = false;
    bool FishingWithMaster = false;         // Playerbots.Fishing.WithMaster: a party bot fishes next to its master while the master fishes
    uint32 FishingCheckMs = 30000;          // how often an idle bot considers fishing
    uint32 FishingChancePct = 10;           // and the chance that it starts then
    float FishingSearchYards = 30.0f;       // how far it walks to a shore
    uint32 FishingCastsMin = 3;
    uint32 FishingCastsMax = 8;

    void Load();
    static Config& Instance();
};
}

#define sProfConfig Playerbots::Prof::Config::Instance()

#endif // Playerbots_ProfessionsConfig_h__
