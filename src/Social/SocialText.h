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

#ifndef Playerbots_SocialText_h__
#define Playerbots_SocialText_h__

#include <array>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Text side of the bots' public chat: the template lines (a selection of AzerothCore's ai_playerbot_texts English rows,
// GPL-2), placeholder filling, links and money. No core types, tested standalone (tests/social_text_test.cpp).
namespace Playerbots::SocialText
{
enum class Category : uint8_t
{
    SuggestQuest,       // %quest_link
    SuggestSomething,   // %zone_name %my_role
    SuggestInstance,    // %instance_name %my_role
    SuggestSell,        // %item_formatted_link %cost_gold
    ReplyLfgChannel,    // %quest_links %zone_name %my_role %other_name
    ReplyLfgWhisper,
    ReplyWtbChannel,    // %formatted_item_links %other_name
    ReplyWtbWhisper,
    JoinGroup           // %player
};

inline std::vector<std::string_view> const& Lines(Category category)
{
    static std::vector<std::string_view> const quest = {
        "Need help with %quest_link?", "Anyone wants to share %quest_link?", "Anyone doing %quest_link?",
        "Wanna do %quest_link.", "Looking to group for %quest_link.", "Who wants to help with %quest_link?",
        "Anyone interested in %quest_link?", "Need a team for %quest_link?", "Any takers for %quest_link?",
        "Need someone for %quest_link.", "Who is up for %quest_link?", "Anyone up for %quest_link?",
        "Anyone for %quest_link?", "Looking for some %quest_link help.", "Can anyone join me for %quest_link?",
        "Anyone want to team up for %quest_link?", "Looking to complete %quest_link." };
    static std::vector<std::string_view> const something = {
        "Wanna party in %zone_name.", "Anyone is looking for %my_role?", "%my_role is looking for guild.",
        "Looking for gold.", "Need a friend.", "Boring...", "Anybody doing something?", "%zone_name: is anybody here?",
        "%zone_name: where is everyone?", "Looks like I am alone in %zone_name.", "Meet me in %zone_name.",
        "Let's quest in %zone_name!", "Wanna go to %zone_name. Anybody with me?", "Are there good quests in %zone_name?",
        "Where to go after %zone_name?", "Who is in %zone_name?", "LFG in %zone_name.", "Catch me in %zone_name!",
        "Anyone has quests in %zone_name?", "Maybe a duel in %zone_name?", "hi everyone!", "%zone_name is comfy", "GL" };
    static std::vector<std::string_view> const instance = {
        "Anyone wants %instance_name?", "Any groups for %instance_name?", "Need help for %instance_name?",
        "LFD: %instance_name.", "Anyone needs %my_role for %instance_name?", "Missing %my_role for %instance_name?",
        "Can be a %my_role for %instance_name.", "Anyone needs gear from %instance_name?", "WTR %instance_name",
        "Wanna run %instance_name.", "%my_role looks for %instance_name.", "Who wants to farm %instance_name?",
        "Looking for %instance_name.", "Need help with %instance_name quests?", "%my_role: any place in group for %instance_name?",
        "Anybody needs more people for %instance_name?", "Wanna quick %instance_name run", "Another %instance_name run?" };
    static std::vector<std::string_view> const sell = {
        "WTS %item_formatted_link for %cost_gold.", "Who wants %item_formatted_link for %cost_gold?",
        "Anyone wants %item_formatted_link? Only %cost_gold.", "Just %cost_gold for %item_formatted_link!",
        "Selling %item_formatted_link for %cost_gold.", "Who needs %item_formatted_link? Only %cost_gold.",
        "%cost_gold for %item_formatted_link. Less than AH!", "Have %item_formatted_link. Who wants to buy for %cost_gold?",
        "Is %cost_gold a good price for %item_formatted_link?", "I have %item_formatted_link ready to sell you for %cost_gold." };
    static std::vector<std::string_view> const lfgChannel = {
        "I have %quest_links", "I also have %quest_links", "I also have %quest_links, I am in %zone_name right now",
        "%other_name, I also have %quest_links", "I am up for %quest_links, I am %my_role",
        "%other_name, I am up for %quest_links, I am in %zone_name right now", "Anyone need %quest_links? I'm in %zone_name." };
    static std::vector<std::string_view> const lfgWhisper = {
        "Hey, I'm up for %quest_links", "Hey, I could do %quest_links with you", "Hey, I also have %quest_links",
        "Hey %other_name, I'm up for %quest_links", "wanna group up for %quest_links ?", "I am up for %quest_links, I am %my_role",
        "I can help with %quest_links if you need", "I've got %quest_links to do too" };
    static std::vector<std::string_view> const wtbChannel = {
        "%other_name, I could sell you %formatted_item_links", "I could potentially sell %formatted_item_links",
        "Think I could sell %formatted_item_links" };
    static std::vector<std::string_view> const wtbWhisper = {
        "I could sell you %formatted_item_links", "Hey, I got %formatted_item_links for sale",
        "I have %formatted_item_links, are you interested?", "Looking to sell %formatted_item_links, what's your offer?" };
    static std::vector<std::string_view> const join = { "Hey %player, do you want join my group?" };

    switch (category)
    {
        case Category::SuggestQuest: return quest;
        case Category::SuggestSomething: return something;
        case Category::SuggestInstance: return instance;
        case Category::SuggestSell: return sell;
        case Category::ReplyLfgChannel: return lfgChannel;
        case Category::ReplyLfgWhisper: return lfgWhisper;
        case Category::ReplyWtbChannel: return wtbChannel;
        case Category::ReplyWtbWhisper: return wtbWhisper;
        case Category::JoinGroup: return join;
    }
    return join;
}

inline char const* CategoryName(Category category)
{
    switch (category)
    {
        case Category::SuggestQuest: return "suggest_quest";
        case Category::SuggestSomething: return "suggest_something";
        case Category::SuggestInstance: return "suggest_instance";
        case Category::SuggestSell: return "suggest_sell";
        case Category::ReplyLfgChannel: return "response_lfg_quests_channel";
        case Category::ReplyLfgWhisper: return "response_lfg_quests_whisper";
        case Category::ReplyWtbChannel: return "response_wtb_items_channel";
        case Category::ReplyWtbWhisper: return "response_wtb_items_whisper";
        case Category::JoinGroup: return "join_group";
    }
    return "";
}

using Placeholders = std::vector<std::pair<std::string_view, std::string>>;

/// Replaces every placeholder (longest keys first is the caller's job when one key is a prefix of another: %quest_link and
/// %quest_links; Fill handles that by trying the longer key at each '%')
inline std::string Fill(std::string_view line, Placeholders const& values)
{
    std::string out;
    out.reserve(line.size() + 32);
    size_t i = 0;
    while (i < line.size())
    {
        if (line[i] == '%')
        {
            std::pair<std::string_view, std::string> const* best = nullptr;
            for (auto const& value : values)
                if (line.substr(i, value.first.size()) == value.first && (!best || value.first.size() > best->first.size()))
                    best = &value;
            if (best)
            {
                out += best->second;
                i += best->first.size();
                continue;
            }
        }
        out += line[i++];
    }
    return out;
}

inline bool StartsWithWord(std::string_view text, std::string_view word)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    if (text.size() < word.size())
        return false;
    for (size_t i = 0; i < word.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(text[i])) != std::toupper(static_cast<unsigned char>(word[i])))
            return false;
    return text.size() == word.size() || !std::isalpha(static_cast<unsigned char>(text[word.size()]));
}

/// Ids of every |H<kind>:<id> link in a chat line ("quest", "item")
inline std::set<uint32_t> LinkIds(std::string_view text, std::string_view kind)
{
    std::set<uint32_t> ids;
    std::string tag = "|H";
    tag += kind;
    tag += ':';
    size_t pos = 0;
    while ((pos = text.find(tag, pos)) != std::string_view::npos)
    {
        pos += tag.size();
        uint64_t id = 0;
        size_t digits = 0;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) && id < 100000000)
        {
            id = id * 10 + uint64_t(text[pos++] - '0');
            ++digits;
        }
        if (digits && id && id < 100000000)
            ids.insert(uint32_t(id));
    }
    return ids;
}

/// "12g 5s 3c" style (the gold part is left out when it is 0)
inline std::string FormatMoney(uint64_t copper)
{
    uint64_t gold = copper / 10000;
    uint64_t silver = (copper / 100) % 100;
    uint64_t rest = copper % 100;
    std::string out;
    if (gold)
        out += std::to_string(gold) + "g";
    if (silver)
        out += (out.empty() ? "" : " ") + std::to_string(silver) + "s";
    if (rest || out.empty())
        out += (out.empty() ? "" : " ") + std::to_string(rest) + "c";
    return out;
}

/// "5g", "20s", "3c", "1g20s" -> copper; 0 when the word is not money
inline uint64_t ParseMoneyWord(std::string_view word)
{
    uint64_t total = 0;
    uint64_t number = 0;
    bool any = false;
    bool digits = false;
    for (char c : word)
    {
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            number = number * 10 + uint64_t(c - '0');
            digits = true;
            if (number > 10000000)
                return 0;
            continue;
        }
        char lower = char(std::tolower(static_cast<unsigned char>(c)));
        uint64_t unit = lower == 'g' ? 10000 : lower == 's' ? 100 : lower == 'c' ? 1 : 0;
        if (!unit || !digits)
            return 0;
        total += number * unit;
        number = 0;
        digits = false;
        any = true;
    }
    return digits || !any ? 0 : total;
}

/// Item link the 12.x client renders ("|cnIQ<quality>:|Hitem:<id>::::::::<level>::::::::::|h[<name>]|h|r"); x<count> after it
inline std::string ItemLink(uint32_t itemId, uint32_t quality, uint32_t level, std::string_view name, uint32_t count = 1)
{
    std::string link = "|cnIQ" + std::to_string(quality) + ":|Hitem:" + std::to_string(itemId) + "::::::::" + std::to_string(level)
        + "::::::::::|h[" + std::string(name) + "]|h|r";
    if (count > 1)
        link += "x" + std::to_string(count);
    return link;
}

/// Quest link ("|cffffff00|Hquest:<id>:<level>|h[<title>]|h|r")
inline std::string QuestLink(uint32_t questId, int32_t level, std::string_view title)
{
    return "|cffffff00|Hquest:" + std::to_string(questId) + ":" + std::to_string(level) + "|h[" + std::string(title) + "]|h|r";
}

/// Splits "item links and words": what follows "to " names a receiver ("sendmail [x] to Bob")
inline std::string ReceiverAfterTo(std::vector<std::string> const& words)
{
    for (size_t i = 0; i + 1 < words.size(); ++i)
        if (words[i] == "to")
            return words[i + 1];
    return {};
}
}

#endif // Playerbots_SocialText_h__
