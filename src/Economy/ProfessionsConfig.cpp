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

#include "ProfessionsConfig.h"
#include "Config.h"
#include <algorithm>

namespace Playerbots::Prof
{
Config& Config::Instance()
{
    static Config config;
    return config;
}

void Config::Load()
{
    CraftAny = sConfigMgr->GetBoolDefault("Playerbots.Craft.AnyProfession", false);
    CraftStationYards = std::clamp(sConfigMgr->GetFloatDefault("Playerbots.Craft.StationYards", 80.0f), 10.0f, 200.0f);
    CraftMaxCasts = std::clamp<uint32>(sConfigMgr->GetIntDefault("Playerbots.Craft.MaxCasts", 3), 1, 20);

    Disenchant = sConfigMgr->GetBoolDefault("Playerbots.Disenchant.Enable", false);
    DisenchantIntervalMs = std::max<uint32>(5000, sConfigMgr->GetIntDefault("Playerbots.Disenchant.IntervalMs", 60000));
    DisenchantMaxQuality = std::clamp<uint32>(sConfigMgr->GetIntDefault("Playerbots.Disenchant.MaxQuality", 4), 2, 4);

    Enchant = sConfigMgr->GetBoolDefault("Playerbots.Enchant.Enable", false);
    EnchantIntervalMs = std::max<uint32>(10000, sConfigMgr->GetIntDefault("Playerbots.Enchant.IntervalMs", 300000));

    Recipes = sConfigMgr->GetBoolDefault("Playerbots.Recipes.Enable", false);
    RecipesIntervalMs = std::max<uint32>(5000, sConfigMgr->GetIntDefault("Playerbots.Recipes.IntervalMs", 60000));

    Outfits = sConfigMgr->GetBoolDefault("Playerbots.Outfits.Enable", false);
    OutfitsMax = std::clamp<uint32>(sConfigMgr->GetIntDefault("Playerbots.Outfits.Max", 20), 1, 20);

    Lockbox = sConfigMgr->GetBoolDefault("Playerbots.Lockbox.Enable", false);
    LockboxIntervalMs = std::max<uint32>(5000, sConfigMgr->GetIntDefault("Playerbots.Lockbox.IntervalMs", 30000));

    Fishing = sConfigMgr->GetBoolDefault("Playerbots.Fishing.Enable", false);
    FishingWithMaster = sConfigMgr->GetBoolDefault("Playerbots.Fishing.WithMaster", false);
    FishingCheckMs = std::max<uint32>(5000, sConfigMgr->GetIntDefault("Playerbots.Fishing.CheckMs", 30000));
    FishingChancePct = std::min<uint32>(100, sConfigMgr->GetIntDefault("Playerbots.Fishing.ChancePct", 10));
    FishingSearchYards = std::clamp(sConfigMgr->GetFloatDefault("Playerbots.Fishing.SearchYards", 30.0f), 5.0f, 80.0f);
    FishingCastsMin = std::clamp<uint32>(sConfigMgr->GetIntDefault("Playerbots.Fishing.CastsMin", 3), 1, 50);
    FishingCastsMax = std::clamp<uint32>(sConfigMgr->GetIntDefault("Playerbots.Fishing.CastsMax", 8), FishingCastsMin, 50);
}
}
