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

#ifndef Playerbots_CommandText_h__
#define Playerbots_CommandText_h__

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Text side of the owner commands that take items, specializations and custom strategies (no core types, tested
// standalone by tests/run_engine_test.sh)
namespace Playerbots
{
inline std::string ToLower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

inline std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

/// The items a chat line names and the plain words around them: "e [Linen Cloth] [Cloth Shirt]" with real links, the
/// client sends |cnIQ1:|Hitem:2589::::::::80:...|h[Linen Cloth]|h|r (older clients |cffffffff|Hitem:2589:0:...|h[..]|h|r).
/// Only the id after "Hitem:" counts (enchants, gems and the bonus list of the link are not needed to find the stack)
struct ItemSelection
{
    std::vector<uint32_t> Items;     // in the order written, a repeated link is kept (sell two of them)
    std::vector<std::string> Words;  // lowercase, outside the links
};

inline ItemSelection ParseItemSelection(std::string_view text)
{
    ItemSelection result;
    constexpr std::string_view Tag = "|Hitem:";
    std::string plain;

    size_t pos = 0;
    while (pos < text.size())
    {
        size_t at = text.find(Tag, pos);
        if (at == std::string_view::npos)
        {
            plain.append(text.substr(pos));
            break;
        }

        // the color code in front of the tag belongs to the link
        size_t start = text.rfind("|c", at);
        plain.append(text.substr(pos, (start != std::string_view::npos && start >= pos ? start : at) - pos));

        size_t digits = at + Tag.size();
        size_t end = digits;
        uint64_t id = 0;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) && id < 100000000)
            id = id * 10 + uint64_t(text[end++] - '0');

        if (end > digits && id > 0 && id < 100000000)
            result.Items.push_back(uint32_t(id));

        // skip to the end of the link: |h[Name]|h, then the optional |r
        size_t nameStart = text.find("|h", end);
        size_t nameEnd = nameStart == std::string_view::npos ? nameStart : text.find("|h", nameStart + 2);
        if (nameEnd == std::string_view::npos)
            pos = text.size();
        else
        {
            pos = nameEnd + 2;
            if (text.substr(pos, 2) == "|r")
                pos += 2;
        }
        plain += ' ';
    }

    size_t i = 0;
    while (i < plain.size())
    {
        while (i < plain.size() && std::isspace(static_cast<unsigned char>(plain[i])))
            ++i;
        size_t j = i;
        while (j < plain.size() && !std::isspace(static_cast<unsigned char>(plain[j])))
            ++j;
        if (j > i)
            result.Words.push_back(ToLower(std::string_view(plain).substr(i, j - i)));
        i = j;
    }

    return result;
}

/// Picks one of the names for what was typed: the exact name, else the only name that starts with it, else the only name
/// that contains it (case does not matter). nullopt = nothing fits or several do. Returns the index.
inline std::optional<size_t> MatchName(std::string_view query, std::vector<std::string> const& names)
{
    std::string q = ToLower(Trim(query));
    if (q.empty())
        return std::nullopt;

    std::optional<size_t> prefix;
    std::optional<size_t> contains;
    size_t prefixes = 0;
    size_t containing = 0;
    for (size_t i = 0; i < names.size(); ++i)
    {
        std::string name = ToLower(names[i]);
        if (name == q)
            return i;
        if (name.starts_with(q))
        {
            prefix = i;
            ++prefixes;
        }
        else if (name.find(q) != std::string::npos)
        {
            contains = i;
            ++containing;
        }
    }

    if (prefixes == 1)
        return prefix;
    if (prefixes == 0 && containing == 1)
        return contains;
    return std::nullopt;
}

/// One line of a custom strategy: "<trigger> > <action> [> <action>...]". Trigger and action names may hold spaces and "::"
struct CustomLine
{
    std::string Trigger;
    std::vector<std::string> Actions;

    bool operator==(CustomLine const&) const = default;
};

inline std::optional<CustomLine> ParseCustomLine(std::string_view text)
{
    CustomLine line;
    bool first = true;
    while (true)
    {
        size_t cut = text.find('>');
        std::string part = ToLower(Trim(text.substr(0, cut)));
        if (part.empty() || part.find_first_of("|;\n") != std::string::npos)
            return std::nullopt;

        if (first)
            line.Trigger = std::move(part);
        else
            line.Actions.push_back(std::move(part));
        first = false;

        if (cut == std::string_view::npos)
            break;
        text.remove_prefix(cut + 1);
    }

    if (line.Actions.empty())
        return std::nullopt;
    return line;
}

inline std::string FormatCustomLine(CustomLine const& line)
{
    std::string out = line.Trigger;
    for (std::string const& action : line.Actions)
        out += ">" + action;
    return out;
}

/// What is stored for a bot: the lines joined by ';'. The store keeps 255 characters per value
constexpr size_t MaxCustomDefinition = 255;

inline std::string FormatCustomDefinition(std::vector<CustomLine> const& lines)
{
    std::string out;
    for (CustomLine const& line : lines)
    {
        if (!out.empty())
            out += ';';
        out += FormatCustomLine(line);
    }
    return out;
}

inline std::vector<CustomLine> ParseCustomDefinition(std::string_view stored)
{
    std::vector<CustomLine> lines;
    while (!stored.empty())
    {
        size_t cut = stored.find(';');
        if (std::optional<CustomLine> line = ParseCustomLine(stored.substr(0, cut)))
            lines.push_back(std::move(*line));
        if (cut == std::string_view::npos)
            break;
        stored.remove_prefix(cut + 1);
    }
    return lines;
}

/// A strategy name made of letters, digits, '-' and '_' (it ends up in a database key of 32 characters with "cs:" in front)
inline bool IsValidCustomName(std::string_view name)
{
    return !name.empty() && name.size() <= 24 && std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}
}

#endif // Playerbots_CommandText_h__
