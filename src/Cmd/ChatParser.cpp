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

#include "ChatParser.h"
#include <algorithm>
#include <cctype>
#include <charconv>

namespace Playerbots
{
namespace
{
std::string Lower(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return result;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool ParseNumber(std::string_view text, uint32_t& value)
{
    if (text.empty())
        return false;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
}

std::string NormalizeClass(std::string const& name)
{
    if (name == "dk" || name == "death knight")
        return "deathknight";
    if (name == "dh" || name == "demon hunter")
        return "demonhunter";
    if (name == "lock")
        return "warlock";
    if (name == "pala" || name == "pally")
        return "paladin";
    if (name == "war")
        return "warrior";
    return name;
}

bool IsClassName(std::string const& name)
{
    static constexpr std::string_view Classes[] = { "warrior", "paladin", "hunter", "rogue", "priest", "deathknight", "shaman",
        "mage", "warlock", "monk", "druid", "demonhunter", "evoker" };
    return std::ranges::find(Classes, name) != std::end(Classes);
}

// One alternative of a filter: "tank", "warrior", "group2", "level60-70", "nearby"
bool MatchPart(std::string part, BotTraits const& bot, float nearbyDistance)
{
    if (!part.empty() && part.front() == '@')
        part.erase(0, 1);
    part = Lower(part);
    if (part.empty())
        return false;

    if (part == "all")
        return true;
    if (part == "tank" || part == "healer" || part == "melee" || part == "ranged")
        return bot.Role == part;
    if (part == "heal")
        return bot.Role == "healer";
    if (part == "caster")
        return bot.Role == "ranged" || bot.Role == "healer";
    if (part == "dps")
        return bot.Role == "melee" || bot.Role == "ranged";
    if (part == "nearby")
        return bot.Distance <= nearbyDistance;
    if (part == "far")
        return bot.Distance > nearbyDistance;

    if (part.starts_with("group"))
    {
        uint32_t group = 0;
        return ParseNumber(std::string_view(part).substr(5), group) && group == bot.Group;
    }

    if (part.starts_with("level"))
    {
        std::string_view range = std::string_view(part).substr(5);
        uint32_t low = 0;
        uint32_t high = 0;
        size_t dash = range.find('-');
        if (dash == std::string_view::npos)
            return ParseNumber(range, low) && bot.Level == low;

        return ParseNumber(range.substr(0, dash), low) && ParseNumber(range.substr(dash + 1), high)
            && bot.Level >= low && bot.Level <= high;
    }

    std::string className = NormalizeClass(part);
    return IsClassName(className) && bot.Class == className;
}

bool MatchFilter(std::string_view token, BotTraits const& bot, float nearbyDistance)
{
    while (!token.empty())
    {
        size_t comma = token.find(',');
        if (MatchPart(std::string(token.substr(0, comma)), bot, nearbyDistance))
            return true;
        if (comma == std::string_view::npos)
            break;
        token.remove_prefix(comma + 1);
    }
    return false;
}
}

FilterResult ApplyChatFilters(std::string_view text, BotTraits const& bot, float nearbyDistance)
{
    FilterResult result;
    text = Trim(text);
    while (!text.empty() && text.front() == '@')
    {
        size_t space = text.find(' ');
        std::string_view token = text.substr(0, space);
        if (!MatchFilter(token, bot, nearbyDistance))
            result.Matches = false;

        text = space == std::string_view::npos ? std::string_view() : Trim(text.substr(space));
    }

    result.Rest = std::string(text);
    return result;
}

std::vector<std::string> SplitChain(std::string_view text, std::string_view separator)
{
    std::vector<std::string> parts;
    if (separator.empty())
    {
        if (!Trim(text).empty())
            parts.emplace_back(Trim(text));
        return parts;
    }

    while (true)
    {
        size_t split = text.find(separator);
        std::string_view part = Trim(text.substr(0, split));
        if (!part.empty())
            parts.emplace_back(part);
        if (split == std::string_view::npos)
            break;
        text.remove_prefix(split + separator.size());
    }
    return parts;
}

void ChatCommandTable::Add(std::string name, ChatCommandInfo info)
{
    _commands[std::move(name)] = std::move(info);
}

void ChatCommandTable::Alias(std::string alias, std::string const& name)
{
    _aliases[std::move(alias)] = name;
}

ChatCommandInfo const* ChatCommandTable::Find(std::string_view name) const
{
    std::string key = Lower(name);
    if (auto alias = _aliases.find(key); alias != _aliases.end())
        key = alias->second;

    auto itr = _commands.find(key);
    return itr == _commands.end() ? nullptr : &itr->second;
}

std::vector<std::string> ChatCommandTable::GetNames() const
{
    std::vector<std::string> names;
    names.reserve(_commands.size());
    for (auto const& [name, info] : _commands)
        names.push_back(name);
    return names;
}

std::optional<ParsedCommand> ParseCommand(ChatCommandTable const& table, std::string_view text)
{
    text = Trim(text);
    if (text.empty())
        return std::nullopt;

    // the whole text first, then drop words from the end until what is left is a command
    std::string_view name = text;
    while (true)
    {
        if (ChatCommandInfo const* info = table.Find(name))
        {
            ParsedCommand parsed;
            parsed.Name = Lower(name);
            parsed.Param = std::string(Trim(text.substr(name.size())));
            parsed.Info = info;
            return parsed;
        }

        size_t space = name.rfind(' ');
        if (space == std::string_view::npos)
            return std::nullopt;
        name = Trim(name.substr(0, space));
    }
}

bool StripPrefix(std::string& text, std::string_view prefix)
{
    if (prefix.empty())
        return true;

    std::string_view trimmed = Trim(text);
    if (!trimmed.starts_with(prefix))
        return false;

    trimmed.remove_prefix(prefix.size());
    text = std::string(Trim(trimmed));
    return true;
}
}
