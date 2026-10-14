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

#include "AiRegistry.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
void (*ErrorSink)(std::string const&) = nullptr;
}

AiRegistry& AiRegistry::Instance()
{
    static AiRegistry instance;
    return instance;
}

std::pair<std::string, std::string> AiRegistry::SplitName(std::string_view fullName)
{
    size_t split = fullName.find("::");
    if (split == std::string_view::npos)
        return { std::string(fullName), std::string() };

    return { std::string(fullName.substr(0, split)), std::string(fullName.substr(split + 2)) };
}

std::unique_ptr<Action> AiRegistry::CreateAction(std::string const& fullName) const
{
    auto [name, qualifier] = SplitName(fullName);
    ActionCreator const* creator = Find(_actions, name);
    return creator ? (*creator)(qualifier) : nullptr;
}

std::unique_ptr<Trigger> AiRegistry::CreateTrigger(std::string const& fullName) const
{
    auto [name, qualifier] = SplitName(fullName);
    TriggerCreator const* creator = Find(_triggers, name);
    return creator ? (*creator)(qualifier) : nullptr;
}

std::unique_ptr<ValueBase> AiRegistry::CreateValue(std::string const& fullName) const
{
    auto [name, qualifier] = SplitName(fullName);
    ValueCreator const* creator = Find(_values, name);
    return creator ? (*creator)(qualifier) : nullptr;
}

std::unique_ptr<Strategy> AiRegistry::CreateStrategy(std::string_view fullName) const
{
    auto [name, qualifier] = SplitName(fullName);
    StrategyCreator const* creator = Find(_strategies, name);
    return creator ? (*creator)(qualifier) : nullptr;
}

std::vector<std::string> AiRegistry::GetStrategyNames() const
{
    std::vector<std::string> names;
    names.reserve(_strategies.size());
    for (auto const& [name, creator] : _strategies)
        names.push_back(name);
    std::ranges::sort(names);
    return names;
}

void AiRegistry::SetErrorSink(void (*sink)(std::string const&))
{
    ErrorSink = sink;
}

void AiRegistry::ReportError(std::string const& message)
{
    if (ErrorSink)
        ErrorSink(message);
}
}
