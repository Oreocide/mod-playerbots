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
#include "Strategies/CustomStrategy.h"
#include "StringFormat.h"
#include "Social/BotHire.h"
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

/// The strategy is active: swap in the edited version
void Refresh(PlayerbotAI& ai, std::string const& name)
{
    std::string full = std::string(CustomStrategies::Prefix) + name;
    if (!ai.GetEngine().HasStrategy(full))
        return;

    ai.GetEngine().RemoveStrategy(full);
    if (std::unique_ptr<Strategy> strategy = ai.MakeStrategy(full))
        ai.GetEngine().AddStrategy(std::move(strategy));
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

bool HireCommand(PlayerbotAI& ai, std::string const&)
{
    Player* master = ai.GetMaster();
    if (!master)
        return false;

    uint64 cost = 0;
    std::string refusal = sBotHire->Check(master, ai.GetBot(), cost);
    if (!refusal.empty())
    {
        ai.TellMaster(refusal);
        return true;
    }

    if (!sBotHire->Start(master, ai.GetBot(), cost))
    {
        ai.TellMaster("I can't right now.");
        return true;
    }

    ai.TellMaster(Trinity::StringFormat("Deal, {}g {}s {}c. I will be one of your characters once I have logged out.", cost / 10000, (cost / 100) % 100, cost % 100));
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

bool CustomStrategyCommand(PlayerbotAI& ai, std::string const& param)
{
    ObjectGuid bot = ai.GetBot()->GetGUID();
    std::string_view text = Trim(param);

    if (text.empty())
    {
        std::vector<std::string> names = CustomStrategies::Names(bot);
        ai.TellMaster(names.empty() ? "No custom strategies. cs <name> <trigger> > <action> writes one."
            : Trinity::StringFormat("Custom strategies: {}. Switch one on with co +custom::<name>.", Join(names)));
        return true;
    }

    size_t cut = text.find(' ');
    std::string name = ToLower(text.substr(0, cut));
    std::string_view rest = cut == std::string_view::npos ? std::string_view() : Trim(text.substr(cut + 1));
    if (!IsValidCustomName(name))
    {
        ai.TellMaster("A name is letters, digits, - and _ (up to 24).");
        return true;
    }

    std::vector<CustomLine> lines = CustomStrategies::Load(bot, name);

    if (rest.empty())
    {
        if (lines.empty())
            ai.TellMaster(Trinity::StringFormat("There is no custom strategy {}.", name));
        else
            for (size_t i = 0; i < lines.size(); ++i)
                ai.TellMaster(Trinity::StringFormat("{}: {}", i + 1, FormatCustomLine(lines[i])));
        return true;
    }

    std::string lowered = ToLower(rest);
    if (lowered == "delete" || lowered == "clear")
    {
        std::string full = std::string(CustomStrategies::Prefix) + name;
        ai.GetEngine().RemoveStrategy(full);
        CustomStrategies::Delete(bot, name);
        ai.TellMaster(Trinity::StringFormat("Deleted {}.", name));
        return true;
    }

    if (lowered.starts_with("remove "))
    {
        size_t index = size_t(std::strtoul(lowered.c_str() + 7, nullptr, 10));
        if (index < 1 || index > lines.size())
        {
            ai.TellMaster("No such line.");
            return true;
        }

        lines.erase(lines.begin() + (index - 1));
        if (lines.empty())
            CustomStrategies::Delete(bot, name);
        else
            CustomStrategies::Save(bot, name, lines);
        Refresh(ai, name);
        ai.TellMaster(Trinity::StringFormat("Removed line {}.", index));
        return true;
    }

    std::optional<CustomLine> line = ParseCustomLine(rest);
    if (!line)
    {
        ai.TellMaster("cs <name> <trigger> > <action> [> <action>...]");
        return true;
    }

    // a name the engine does not know would only be reported once and then never run
    if (!sPlayerbotsAi.CreateTrigger(line->Trigger))
    {
        ai.TellMaster(Trinity::StringFormat("I don't know the trigger \"{}\".", line->Trigger));
        return true;
    }

    for (std::string const& action : line->Actions)
        if (!sPlayerbotsAi.CreateAction(action))
        {
            ai.TellMaster(Trinity::StringFormat("I don't know the action \"{}\".", action));
            return true;
        }

    lines.push_back(std::move(*line));
    if (!CustomStrategies::Save(bot, name, lines))
    {
        ai.TellMaster("That is too long to keep, remove a line first.");
        return true;
    }

    Refresh(ai, name);
    ai.TellMaster(Trinity::StringFormat("{} has {} {}. co +custom::{} switches it on.", name, lines.size(), lines.size() == 1 ? "line" : "lines", name));
    return true;
}
}
