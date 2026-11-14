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

#ifndef Playerbots_ProfessionsRules_h__
#define Playerbots_ProfessionsRules_h__

// Pure rules of the professions group (outfit storage text, outfit names, order words). No core types, tested standalone by
// tests/professions_rules_test.cpp.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Playerbots::Prof::Rules
{
/// One outfit piece: equipment slot and the low part of the item's guid
struct OutfitPiece
{
    uint8_t Slot = 0;
    uint64_t ItemLow = 0;
    bool operator==(OutfitPiece const&) const = default;
};

/// "slot:low;slot:low" (playerbots_db_store value of key "outfit:<name>")
inline std::string EncodeOutfit(std::vector<OutfitPiece> const& pieces)
{
    std::string out;
    for (OutfitPiece const& piece : pieces)
    {
        if (!out.empty())
            out += ';';
        out += std::to_string(piece.Slot);
        out += ':';
        out += std::to_string(piece.ItemLow);
    }
    return out;
}

/// The pieces of a stored outfit; malformed parts are skipped, slots >= maxSlot too
inline std::vector<OutfitPiece> DecodeOutfit(std::string_view text, uint8_t maxSlot)
{
    std::vector<OutfitPiece> pieces;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(';', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view part = text.substr(start, end - start);
        start = end + 1;

        size_t colon = part.find(':');
        if (colon == std::string_view::npos)
            continue;
        unsigned slot = 0;
        uint64_t low = 0;
        std::string_view slotText = part.substr(0, colon);
        std::string_view lowText = part.substr(colon + 1);
        if (std::from_chars(slotText.data(), slotText.data() + slotText.size(), slot).ec != std::errc() || slot >= maxSlot)
            continue;
        if (std::from_chars(lowText.data(), lowText.data() + lowText.size(), low).ec != std::errc() || !low)
            continue;
        pieces.push_back({ uint8_t(slot), low });
    }
    return pieces;
}

/// An outfit name as stored: trimmed, lower case, at most 24 characters of letters, digits, spaces, '-' and '_'. Empty if nothing is left
inline std::string CleanOutfitName(std::string_view raw)
{
    std::string name;
    for (char c : raw)
    {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_')
            name += char(std::tolower(u));
        else if (c == ' ' && !name.empty() && name.back() != ' ')
            name += ' ';
        if (name.size() >= 24)
            break;
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name;
}

/// The list of outfit names stored under key "outfits": names joined by '|'
inline std::vector<std::string> SplitNames(std::string_view text)
{
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('|', start);
        if (end == std::string_view::npos)
            end = text.size();
        if (end > start)
            names.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return names;
}

inline std::string JoinNames(std::vector<std::string> const& names)
{
    std::string out;
    for (std::string const& name : names)
    {
        if (!out.empty())
            out += '|';
        out += name;
    }
    return out;
}

enum class OutfitVerb
{
    List,
    Save,
    Equip,
    Delete,
    Show
};

struct OutfitOrder
{
    OutfitVerb Verb = OutfitVerb::List;
    std::string Name;
};

/// "outfit", "outfit list", "outfit save <name>" / "+<name>", "outfit equip <name>" / "<name>", "outfit delete <name>" / "-<name>",
/// "outfit show <name>" / "?<name>"
inline OutfitOrder ParseOutfitOrder(std::string_view param)
{
    auto trim = [](std::string_view s)
    {
        while (!s.empty() && s.front() == ' ')
            s.remove_prefix(1);
        while (!s.empty() && s.back() == ' ')
            s.remove_suffix(1);
        return s;
    };

    OutfitOrder order;
    std::string_view text = trim(param);
    if (text.empty() || text == "list" || text == "?")
        return order;

    if (text.front() == '+' || text.front() == '-' || text.front() == '?')
    {
        order.Verb = text.front() == '+' ? OutfitVerb::Save : text.front() == '-' ? OutfitVerb::Delete : OutfitVerb::Show;
        order.Name = CleanOutfitName(trim(text.substr(1)));
        return order;
    }

    size_t space = text.find(' ');
    std::string_view word = text.substr(0, space);
    std::string_view rest = space == std::string_view::npos ? std::string_view() : trim(text.substr(space + 1));
    static constexpr std::pair<std::string_view, OutfitVerb> Verbs[] =
    {
        { "save", OutfitVerb::Save }, { "equip", OutfitVerb::Equip }, { "wear", OutfitVerb::Equip },
        { "delete", OutfitVerb::Delete }, { "remove", OutfitVerb::Delete }, { "show", OutfitVerb::Show }
    };
    for (auto const& [verbWord, verb] : Verbs)
    {
        if (word == verbWord)
        {
            order.Verb = verb;
            order.Name = CleanOutfitName(rest);
            return order;
        }
    }

    order.Verb = OutfitVerb::Equip;
    order.Name = CleanOutfitName(text);
    return order;
}

/// How many casts of a recipe the reagents in the bags pay for: the smallest have/need over the reagents, capped
inline uint32_t AffordableCasts(std::vector<std::pair<uint32_t, uint32_t>> const& haveNeed, uint32_t cap)
{
    uint32_t casts = cap;
    for (auto const& [have, need] : haveNeed)
    {
        if (!need)
            continue;
        casts = std::min(casts, have / need);
    }
    return casts;
}
}

#endif // Playerbots_ProfessionsRules_h__
