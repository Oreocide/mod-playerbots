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

#include "CustomStrategy.h"
#include "Db/PlayerbotRepository.h"
#include "Engine.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
constexpr float BaseRelevance = 62.0f;   // above the attack (60), below consuming and healing (75, 80)
constexpr char const* ListKey = "cs:list";

/// Offers the actions of every line while the line's trigger is active; earlier lines and earlier actions win
class CustomStrategy final : public Strategy
{
public:
    CustomStrategy(std::string name, std::vector<CustomLine> lines) : Strategy(std::move(name)), _lines(std::move(lines)) { }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        float lineRelevance = BaseRelevance;
        for (CustomLine const& line : _lines)
        {
            TriggerNode node;
            node.Trigger = line.Trigger;
            float relevance = lineRelevance;
            for (std::string const& action : line.Actions)
            {
                node.Handlers.push_back({ action, relevance });
                relevance -= 0.1f;
            }
            triggers.push_back(std::move(node));
            lineRelevance -= 0.5f;
        }
    }

private:
    std::vector<CustomLine> _lines;
};

void SaveNames(ObjectGuid bot, std::vector<std::string> const& names)
{
    std::string joined;
    for (std::string const& name : names)
        joined += (joined.empty() ? "" : ",") + name;
    sPlayerbotRepository->SetValue(bot, ListKey, joined);
}
}

namespace CustomStrategies
{
std::vector<std::string> Names(ObjectGuid bot)
{
    std::vector<std::string> names;
    if (Optional<std::string> stored = sPlayerbotRepository->GetValue(bot, ListKey))
    {
        std::string_view rest = *stored;
        while (!rest.empty())
        {
            size_t cut = rest.find(',');
            if (cut != 0)
                names.emplace_back(rest.substr(0, cut));
            if (cut == std::string_view::npos)
                break;
            rest.remove_prefix(cut + 1);
        }
    }
    return names;
}

std::vector<CustomLine> Load(ObjectGuid bot, std::string const& name)
{
    Optional<std::string> stored = sPlayerbotRepository->GetValue(bot, "cs:" + name);
    return stored ? ParseCustomDefinition(*stored) : std::vector<CustomLine>{ };
}

bool Save(ObjectGuid bot, std::string const& name, std::vector<CustomLine> const& lines)
{
    std::string definition = FormatCustomDefinition(lines);
    if (definition.size() > MaxCustomDefinition)
        return false;

    std::vector<std::string> names = Names(bot);
    if (std::ranges::find(names, name) == names.end())
    {
        names.push_back(name);
        // the list is one value too
        std::string joined;
        for (std::string const& known : names)
            joined += (joined.empty() ? "" : ",") + known;
        if (joined.size() > MaxCustomDefinition)
            return false;
        SaveNames(bot, names);
    }

    sPlayerbotRepository->SetValue(bot, "cs:" + name, definition);
    return true;
}

void Delete(ObjectGuid bot, std::string const& name)
{
    std::vector<std::string> names = Names(bot);
    std::erase(names, name);
    SaveNames(bot, names);
    sPlayerbotRepository->EraseValue(bot, "cs:" + name);
}

std::unique_ptr<Strategy> Create(ObjectGuid bot, std::string_view fullName)
{
    if (!fullName.starts_with(Prefix))
        return nullptr;

    std::string name = ToLower(fullName.substr(Prefix.size()));
    if (!IsValidCustomName(name))
        return nullptr;

    std::vector<CustomLine> lines = Load(bot, name);
    if (lines.empty())
        return nullptr;

    return std::make_unique<CustomStrategy>(std::string(Prefix) + name, std::move(lines));
}
}
}
