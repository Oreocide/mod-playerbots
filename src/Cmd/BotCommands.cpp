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

#include "BotCommands.h"
#include "AiRegistry.h"
#include "CommandText.h"
#include "DB2Stores.h"
#include "GearFactory.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "PlayerbotAI.h"
#include "StringFormat.h"
#include "Talents/BotTalents.h"
#include "World.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
struct SpecChoice
{
    ChrSpecializationEntry const* Entry;
    std::string Name;
};

std::vector<SpecChoice> SpecsOf(Player const* bot)
{
    std::vector<SpecChoice> specs;
    for (uint32 index = 0; index < 4; ++index)
        if (ChrSpecializationEntry const* spec = sDB2Manager.GetChrSpecializationByIndex(bot->GetClass(), index))
            if (!spec->IsPetSpecialization())
                specs.push_back({ spec, spec->Name[sWorld->GetDefaultDbcLocale()] });
    return specs;
}

std::string Join(std::vector<std::string> const& items)
{
    std::string out;
    for (std::string const& item : items)
        out += (out.empty() ? "" : ", ") + item;
    return out;
}
}

bool SpecCommand(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    std::vector<SpecChoice> specs = SpecsOf(bot);
    if (specs.empty() || bot->GetLevel() < MIN_SPECIALIZATION_LEVEL)
    {
        ai.TellMaster(Trinity::StringFormat("I have no specialization before level {}.", uint32(MIN_SPECIALIZATION_LEVEL)));
        return true;
    }

    ChrSpecializationEntry const* current = bot->GetPrimarySpecializationEntry();
    std::vector<std::string> names;
    for (SpecChoice const& spec : specs)
        names.push_back(spec.Name);

    if (Trim(param).empty())
    {
        ai.TellMaster(Trinity::StringFormat("I am {}. Specializations: {}.", current ? current->Name[sWorld->GetDefaultDbcLocale()] : "unspecialized", Join(names)));
        return true;
    }

    std::optional<size_t> match = MatchName(param, names);
    if (!match)
    {
        ai.TellMaster(Trinity::StringFormat("Which one? {}.", Join(names)));
        return true;
    }

    SpecChoice const& choice = specs[*match];
    if (current == choice.Entry)
    {
        ai.TellMaster(Trinity::StringFormat("I am already {}.", choice.Name));
        return true;
    }

    if (bot->IsInCombat() || !bot->IsAlive())
    {
        ai.TellMaster("Not right now.");
        return false;
    }

    bot->ActivateTalentGroup(choice.Entry);
    bot->InitTalentForLevel();
    BotTalents::ApplyStarterBuild(bot);

    // the gear is the player's: autogear is the explicit way to fit it to the new role
    ai.TellMaster(Trinity::StringFormat("Now {}. \"autogear\" fits my gear to it.", choice.Name));
    return true;
}

bool MaintenanceCommand(PlayerbotAI& ai, std::string const&)
{
    if (!sPlayerbotsConfig.CommandMaintenance)
    {
        ai.TellMaster("Maintenance is switched off (Playerbots.Command.Maintenance).");
        return true;
    }

    Player* bot = ai.GetBot();
    if (bot->IsInCombat() || !bot->IsAlive())
    {
        ai.TellMaster("Not right now.");
        return false;
    }

    // a free repair and a few days of food and drink: the gear, money and spells of a player's character are not touched
    bot->DurabilityRepairAll(false, 0.0f, false);
    sGearFactory->StockFood(bot);
    ai.TellMaster("Repaired, food and drink stocked.");
    return true;
}
}
