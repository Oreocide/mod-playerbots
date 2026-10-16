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

#ifndef Playerbots_PerfMonitor_h__
#define Playerbots_PerfMonitor_h__

#include "Define.h"
#include <chrono>
#include <mutex>
#include <string>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

// Counterpart of AzerothCore's PerfMonitor: wall time per named operation (engine relevance scan,
// every action), collected only while Playerbots.PerfMon.Enable is set
namespace Playerbots
{
class PerfMonitor
{
public:
    struct Entry
    {
        std::string Name;
        uint64 Calls = 0;
        uint64 TotalUs = 0;
        uint32 MaxUs = 0;
    };

    static PerfMonitor& Instance();

    void Record(char const* name, uint64 microseconds);
    /// Slowest total time first
    std::vector<Entry> Snapshot() const;
    void Reset();

private:
    struct StringHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    // one table per recording thread, each behind its own (uncontended) mutex: no global lock and, with the
    // transparent lookup, no string allocation per call once a name is known
    struct Local
    {
        std::mutex Mutex;
        std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> Entries;
    };
    Local& GetLocal();

    mutable std::mutex _registryMutex;
    std::vector<std::unique_ptr<Local>> _locals;
};

/// Times its scope, does nothing (no clock read) while the monitor is disabled
class PerfScope
{
public:
    explicit PerfScope(char const* name);
    ~PerfScope();

    PerfScope(PerfScope const&) = delete;
    PerfScope& operator=(PerfScope const&) = delete;

private:
    char const* _name;
    bool _active;
    std::chrono::steady_clock::time_point _start;
};
}

#define sPlayerbotsPerf Playerbots::PerfMonitor::Instance()

#endif // Playerbots_PerfMonitor_h__
