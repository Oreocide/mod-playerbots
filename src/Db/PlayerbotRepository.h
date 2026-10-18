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

#ifndef PlayerbotRepository_h__
#define PlayerbotRepository_h__

#include "ObjectGuid.h"
#include "Optional.h"
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Playerbots
{
struct BotEvent
{
    int32 Value = 0;
    std::string Data;
    time_t Time = 0;
    uint32 ValidIn = 0;     // seconds after Time, 0 = never expires
};

/// Access to the playerbots_* tables of the characters database (modules/mod-playerbots/data/sql/db-characters).
/// Everything is read once at startup (synchronous, world thread) into memory, writes go out asynchronously.
/// Values and events (GetValue/SetValue/EraseValue/GetEvent/SetEvent) are locked: map threads write them
/// (homes and anchors of bots that moved). Names and account marks are world thread only. IsQuestBlacklisted()
/// only reads data that never changes after Load() and is safe from any thread.
class PlayerbotRepository
{
public:
    static PlayerbotRepository* instance();

    /// Reads the tables, removes rows of deleted characters and expired events
    void Load();

    // playerbots_db_store: per bot key/value
    Optional<std::string> GetValue(ObjectGuid bot, std::string const& key) const;
    void SetValue(ObjectGuid bot, std::string const& key, std::string const& value);
    void EraseValue(ObjectGuid bot, std::string const& key);

    // playerbots_random_bots: (owner, bot, event) -> value / data, optionally expiring
    Optional<BotEvent> GetEvent(uint32 owner, ObjectGuid bot, std::string const& event) const;
    void SetEvent(uint32 owner, ObjectGuid bot, std::string const& event, int32 value, uint32 validIn = 0, std::string const& data = {});

    // playerbots_account_type
    void MarkRandomAccount(uint32 accountId);

    // playerbots_names: name pool per category (race group * 2 + gender), each name is handed out once per run
    static uint8 NameCategory(uint8 race, uint8 gender);
    bool PopName(uint8 race, uint8 gender, std::string& name);
    size_t GetNameCount() const;

    // playerbots_quest_blacklist
    bool IsQuestBlacklisted(uint32 questId) const { return _questBlacklist.contains(questId); }

    struct MemoryLine
    {
        std::string Kind;
        std::string Key;
        std::string Payload;
        time_t At = 0;
    };

    /// Newest slot is 0. place and goal keep 16, person keeps 32. One transaction per push.
    void Remember(ObjectGuid bot, std::string const& kind, std::string const& key, std::string const& payload);
    void ForgetMemory(ObjectGuid bot);
    std::vector<MemoryLine> RecentMemory(ObjectGuid bot, uint32 limit) const;

private:
    using EventKey = std::pair<uint32, std::string>;    // (bot counter, event) per owner
    struct EventKeyHash
    {
        size_t operator()(std::pair<uint64, std::string> const& key) const;
    };

    static uint32 Low(ObjectGuid guid) { return uint32(guid.GetCounter()); }
    static std::string Escaped(std::string value);
    static bool Expired(BotEvent const& event, time_t now);

    mutable std::mutex _lock;   // _store and _events
    std::unordered_map<uint32, std::unordered_map<std::string, std::string>> _store;
    std::unordered_map<std::pair<uint64, std::string>, BotEvent, EventKeyHash> _events;   // ((owner << 32) | bot, event)
    std::vector<std::vector<std::string>> _names;
    std::unordered_set<uint32> _questBlacklist;
    struct MemorySlot
    {
        std::string Key;
        std::string Payload;
        time_t At = 0;
    };
    std::unordered_map<uint32, std::unordered_map<std::string, std::deque<MemorySlot>>> _memory;
    bool _loaded = false;
};
}

#define sPlayerbotRepository Playerbots::PlayerbotRepository::instance()

#endif // PlayerbotRepository_h__
