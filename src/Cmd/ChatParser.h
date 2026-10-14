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

#ifndef Playerbots_ChatParser_h__
#define Playerbots_ChatParser_h__

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Text side of the chat commands (no core types, tested standalone by tests/run_engine_test.sh):
//   "@tank,healer @group2 follow ; @warrior attack"
// A message is a chain of commands split at the separator. Every command may start with @filters that
// select which bots answer (all filters must match, a filter with commas matches if any part does),
// then the command name and its parameter ("co +dps,-follow"). The longest known name wins.
namespace Playerbots
{
/// What a filter needs to know about a bot
struct BotTraits
{
    std::string Class;       // lowercase class name: "warrior", "deathknight", "demonhunter"...
    std::string Role;        // "tank", "healer", "melee", "ranged"
    uint8_t Group = 0;       // party subgroup, 1 based, 0 = not grouped
    float Distance = 0.0f;   // yards to the one who spoke
    uint8_t Level = 1;
};

struct FilterResult
{
    bool Matches = true;
    std::string Rest;        // the text after the filters
};

/// Strips leading @filters. Matches = false if any of them excludes this bot (or is unknown)
FilterResult ApplyChatFilters(std::string_view text, BotTraits const& bot, float nearbyDistance);

/// Splits at the separator, empty parts are dropped. An empty separator means no chaining
std::vector<std::string> SplitChain(std::string_view text, std::string_view separator);

struct ChatCommandInfo
{
    std::string Action;      // registry action that runs it
    std::string Help;
    bool Persist = true;     // changes something worth saving (strategies), not a pure query
    bool Leave = false;      // asks the manager to log the bot out instead of running an action
};

class ChatCommandTable
{
public:
    void Add(std::string name, ChatCommandInfo info);
    void Alias(std::string alias, std::string const& name);
    ChatCommandInfo const* Find(std::string_view name) const;
    /// Names without aliases pointing at the same command, sorted
    std::vector<std::string> GetNames() const;

private:
    std::map<std::string, ChatCommandInfo> _commands;
    std::map<std::string, std::string> _aliases;
};

struct ParsedCommand
{
    std::string Name;        // lowercase, as typed (may be an alias)
    std::string Param;       // the rest, case kept
    ChatCommandInfo const* Info = nullptr;
};

/// "co +dps" -> name "co", param "+dps". std::nullopt if no prefix of the words is a command
std::optional<ParsedCommand> ParseCommand(ChatCommandTable const& table, std::string_view text);

/// Removes the command prefix of group chat ("!follow" -> "follow"). false = the message is not for the bots.
/// An empty prefix accepts everything
bool StripPrefix(std::string& text, std::string_view prefix);
}

#endif // Playerbots_ChatParser_h__
