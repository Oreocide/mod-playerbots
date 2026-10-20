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

#include "PlayerbotRepository.h"
#include "DatabaseEnv.h"
#include "Containers.h"
#include "Log.h"
#include "PlayerbotsConfig.h"
#include "RaceMask.h"
#include "Random.h"
#include "SharedDefines.h"
#include <algorithm>
#include <functional>

namespace Playerbots
{
namespace
{
// Same layout as AzerothCore's NameRaceAndGender: category = race group * 2 + gender
constexpr uint8 NameGroupCount = 9;

uint8 NameGroup(uint8 race)
{
    switch (race)
    {
        case RACE_GNOME:    return 1;
        case RACE_DWARF:    return 2;
        case RACE_NIGHTELF: return 3;
        case RACE_DRAENEI:  return 4;
        case RACE_ORC:      return 5;
        case RACE_TROLL:    return 6;
        case RACE_TAUREN:   return 7;
        case RACE_BLOODELF: return 8;
        default:            return 0;   // human, undead and every race without its own list
    }
}
}

size_t PlayerbotRepository::EventKeyHash::operator()(std::pair<uint64, std::string> const& key) const
{
    return std::hash<uint64>()(key.first) ^ (std::hash<std::string>()(key.second) << 1);
}

PlayerbotRepository* PlayerbotRepository::instance()
{
    static PlayerbotRepository instance;
    return &instance;
}

std::string PlayerbotRepository::Escaped(std::string value)
{
    CharacterDatabase.EscapeString(value);
    return value;
}

bool PlayerbotRepository::Expired(BotEvent const& event, time_t now)
{
    return event.ValidIn && event.Time + time_t(event.ValidIn) < now;
}

void PlayerbotRepository::Load()
{
    std::scoped_lock lock(_lock);
    _store.clear();
    _events.clear();
    _names.assign(NameGroupCount * 2, {});
    _questBlacklist.clear();

    // rows of characters that no longer exist
    CharacterDatabase.DirectExecute("DELETE FROM playerbots_db_store WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.DirectExecute("DELETE FROM playerbots_random_bots WHERE bot <> 0 AND bot NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.DirectExecute("DELETE FROM playerbots_random_bots WHERE validIn IS NOT NULL AND validIn > 0 AND `time` + validIn < UNIX_TIMESTAMP()");

    if (QueryResult result = CharacterDatabase.Query("SELECT guid, `key`, `value` FROM playerbots_db_store"))
    {
        do
        {
            Field* fields = result->Fetch();
            _store[fields[0].GetUInt32()][fields[1].GetString()] = fields[2].GetString();
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT owner, bot, `time`, validIn, event, `value`, `data` FROM playerbots_random_bots"))
    {
        do
        {
            Field* fields = result->Fetch();
            BotEvent event;
            event.Time = time_t(fields[2].GetUInt32());
            event.ValidIn = fields[3].IsNull() ? 0 : fields[3].GetUInt32();
            event.Value = fields[5].IsNull() ? 0 : fields[5].GetInt32();
            event.Data = fields[6].IsNull() ? std::string() : fields[6].GetString();
            uint64 key = (uint64(fields[0].GetUInt32()) << 32) | fields[1].GetUInt32();
            _events[{ key, fields[4].GetString() }] = std::move(event);
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT guid, kind, slot, `key`, payload, `at` FROM playerbots_memory ORDER BY guid, kind, slot"))
    {
        do
        {
            Field* fields = result->Fetch();
            MemorySlot slot;
            slot.Key = fields[3].GetString();
            slot.Payload = fields[4].GetString();
            slot.At = time_t(fields[5].GetUInt32());
            _memory[fields[0].GetUInt32()][fields[1].GetString()].push_back(std::move(slot));
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT quest_id FROM playerbots_quest_blacklist"))
    {
        do
        {
            _questBlacklist.insert((*result)[0].GetUInt32());
        } while (result->NextRow());
    }

    size_t nameCount = 0;
    if (sPlayerbotsConfig.RandomNamePool)
    {
        if (QueryResult result = CharacterDatabase.Query("SELECT name, gender FROM playerbots_names"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint8 category = fields[1].GetUInt8();
                if (category < _names.size())
                {
                    _names[category].push_back(fields[0].GetString());
                    ++nameCount;
                }
            } while (result->NextRow());
        }

        for (std::vector<std::string>& names : _names)
            Trinity::Containers::RandomShuffle(names);
    }

    _loaded = true;
    TC_LOG_INFO("server.playerbots", "Playerbots: loaded {} stored bot values, {} events, {} pooled names, {} blacklisted quests",
        _store.size(), _events.size(), nameCount, _questBlacklist.size());
}

Optional<std::string> PlayerbotRepository::GetValue(ObjectGuid bot, std::string const& key) const
{
    std::scoped_lock lock(_lock);
    auto bots = _store.find(Low(bot));
    if (bots == _store.end())
        return {};

    auto value = bots->second.find(key);
    if (value == bots->second.end())
        return {};

    return value->second;
}

void PlayerbotRepository::SetValue(ObjectGuid bot, std::string const& key, std::string const& value)
{
    if (!_loaded)
        return;

    std::scoped_lock lock(_lock);   // held while queueing the write, so two threads' writes reach the DB in memory order
    _store[Low(bot)][key] = value;

    auto trans = CharacterDatabase.BeginTransaction();
    trans->PAppend("DELETE FROM playerbots_db_store WHERE guid = {} AND `key` = '{}'", Low(bot), Escaped(key));
    trans->PAppend("INSERT INTO playerbots_db_store (guid, `key`, `value`) VALUES ({}, '{}', '{}')", Low(bot), Escaped(key), Escaped(value));
    CharacterDatabase.CommitTransaction(trans);
}

void PlayerbotRepository::EraseValue(ObjectGuid bot, std::string const& key)
{
    if (!_loaded)
        return;

    std::scoped_lock lock(_lock);
    auto bots = _store.find(Low(bot));
    if (bots == _store.end() || !bots->second.erase(key))
        return;

    CharacterDatabase.PExecute("DELETE FROM playerbots_db_store WHERE guid = {} AND `key` = '{}'", Low(bot), Escaped(key));
}

Optional<BotEvent> PlayerbotRepository::GetEvent(uint32 owner, ObjectGuid bot, std::string const& event) const
{
    std::scoped_lock lock(_lock);
    auto itr = _events.find({ (uint64(owner) << 32) | Low(bot), event });
    if (itr == _events.end() || Expired(itr->second, time(nullptr)))
        return {};

    return itr->second;
}

void PlayerbotRepository::SetEvent(uint32 owner, ObjectGuid bot, std::string const& event, int32 value, uint32 validIn, std::string const& data)
{
    if (!_loaded)
        return;

    BotEvent entry;
    entry.Value = value;
    entry.Data = data;
    entry.Time = time(nullptr);
    entry.ValidIn = validIn;

    std::scoped_lock lock(_lock);
    _events[{ (uint64(owner) << 32) | Low(bot), event }] = entry;

    auto trans = CharacterDatabase.BeginTransaction();
    trans->PAppend("DELETE FROM playerbots_random_bots WHERE owner = {} AND bot = {} AND event = '{}'", owner, Low(bot), Escaped(event));
    trans->PAppend("INSERT INTO playerbots_random_bots (owner, bot, `time`, validIn, event, `value`, `data`) VALUES ({}, {}, {}, {}, '{}', {}, '{}')",
        owner, Low(bot), uint32(entry.Time), validIn, Escaped(event), value, Escaped(data));
    CharacterDatabase.CommitTransaction(trans);
}

void PlayerbotRepository::Remember(ObjectGuid bot, std::string const& kind, std::string const& key, std::string const& payload)
{
    if (!sPlayerbotsConfig.MemoryEnable || kind.empty())
        return;

    uint32 cap = kind == "person" ? 32u : 16u;
    uint32 low = Low(bot);
    std::deque<MemorySlot> saved;
    {
        std::scoped_lock lock(_lock);
        std::deque<MemorySlot>& ring = _memory[low][kind];
        ring.push_front({ key.substr(0, 64), payload.substr(0, 255), time(nullptr) });
        while (ring.size() > cap)
            ring.pop_back();
        saved = ring;
    }

    auto trans = CharacterDatabase.BeginTransaction();
    trans->PAppend("DELETE FROM playerbots_memory WHERE guid = {} AND kind = '{}'", low, Escaped(kind));
    for (uint32 slot = 0; slot < saved.size(); ++slot)
        trans->PAppend("INSERT INTO playerbots_memory (guid, kind, slot, `key`, payload, `at`) VALUES ({}, '{}', {}, '{}', '{}', {})",
            low, Escaped(kind), slot, Escaped(saved[slot].Key), Escaped(saved[slot].Payload), uint32(saved[slot].At));
    CharacterDatabase.CommitTransaction(trans);
}

void PlayerbotRepository::ForgetMemory(ObjectGuid bot)
{
    uint32 low = Low(bot);
    {
        std::scoped_lock lock(_lock);
        _memory.erase(low);
    }
    CharacterDatabase.PExecute("DELETE FROM playerbots_memory WHERE guid = {}", low);
}

void PlayerbotRepository::ForgetBot(ObjectGuid bot)
{
    uint32 low = Low(bot);
    {
        std::scoped_lock lock(_lock);
        _store.erase(low);
        std::erase_if(_events, [low](auto const& entry) { return uint32(entry.first.first & 0xFFFFFFFFu) == low; });
    }
    CharacterDatabase.PExecute("DELETE FROM playerbots_db_store WHERE guid = {}", low);
    CharacterDatabase.PExecute("DELETE FROM playerbots_random_bots WHERE bot = {}", low);
    ForgetMemory(bot);
}

std::vector<PlayerbotRepository::MemoryLine> PlayerbotRepository::RecentMemory(ObjectGuid bot, uint32 limit) const
{
    std::vector<MemoryLine> lines;
    std::scoped_lock lock(_lock);
    auto botRows = _memory.find(Low(bot));
    if (botRows == _memory.end())
        return lines;

    for (auto const& [kind, ring] : botRows->second)
        for (MemorySlot const& slot : ring)
            lines.push_back({ kind, slot.Key, slot.Payload, slot.At });

    std::ranges::sort(lines, [](MemoryLine const& a, MemoryLine const& b) { return a.At > b.At; });
    if (lines.size() > limit)
        lines.resize(limit);
    return lines;
}

void PlayerbotRepository::MarkRandomAccount(uint32 accountId)
{
    CharacterDatabase.PExecute("INSERT IGNORE INTO playerbots_account_type (account_id, account_type) VALUES ({}, 1)", accountId);
}

uint8 PlayerbotRepository::NameCategory(uint8 race, uint8 gender)
{
    return uint8(NameGroup(race) * 2 + (gender == GENDER_FEMALE ? 1 : 0));
}

bool PlayerbotRepository::PopName(uint8 race, uint8 gender, std::string& name)
{
    uint8 category = NameCategory(race, gender);
    if (category >= _names.size())
        return false;

    std::vector<std::string>& pool = _names[category];
    if (pool.empty())
        return false;

    name = std::move(pool.back());
    pool.pop_back();
    return true;
}

size_t PlayerbotRepository::GetNameCount() const
{
    size_t count = 0;
    for (std::vector<std::string> const& names : _names)
        count += names.size();
    return count;
}
}
