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

#include "PerfMonitor.h"
#include "PlayerbotsConfig.h"
#include <algorithm>

namespace Playerbots
{
PerfMonitor& PerfMonitor::Instance()
{
    static PerfMonitor instance;
    return instance;
}

PerfMonitor::Local& PerfMonitor::GetLocal()
{
    static thread_local Local* local = nullptr;
    if (!local)
    {
        std::lock_guard<std::mutex> lock(_registryMutex);
        _locals.push_back(std::make_unique<Local>());
        local = _locals.back().get();
    }
    return *local;
}

void PerfMonitor::Record(char const* name, uint64 microseconds)
{
    Local& local = GetLocal();
    std::lock_guard<std::mutex> lock(local.Mutex);
    auto it = local.Entries.find(std::string_view(name));
    if (it == local.Entries.end())
    {
        it = local.Entries.emplace(name, Entry{}).first;
        it->second.Name = name;
    }
    Entry& entry = it->second;
    ++entry.Calls;
    entry.TotalUs += microseconds;
    entry.MaxUs = std::max<uint32>(entry.MaxUs, uint32(std::min<uint64>(microseconds, UINT32_MAX)));
}

std::vector<PerfMonitor::Entry> PerfMonitor::Snapshot() const
{
    std::unordered_map<std::string, Entry> merged;
    {
        std::lock_guard<std::mutex> registry(_registryMutex);
        for (auto const& local : _locals)
        {
            std::lock_guard<std::mutex> lock(local->Mutex);
            for (auto const& [name, entry] : local->Entries)
            {
                Entry& m = merged[name];
                m.Name = entry.Name;
                m.Calls += entry.Calls;
                m.TotalUs += entry.TotalUs;
                m.MaxUs = std::max(m.MaxUs, entry.MaxUs);
            }
        }
    }
    std::vector<Entry> result;
    result.reserve(merged.size());
    for (auto& [name, entry] : merged)
        result.push_back(std::move(entry));

    std::ranges::sort(result, std::ranges::greater(), &Entry::TotalUs);
    return result;
}

void PerfMonitor::Reset()
{
    std::lock_guard<std::mutex> registry(_registryMutex);
    for (auto const& local : _locals)
    {
        std::lock_guard<std::mutex> lock(local->Mutex);
        local->Entries.clear();
    }
}

PerfScope::PerfScope(char const* name) : _name(name), _active(sPlayerbotsConfig.PerfMonEnabled)
{
    if (_active)
        _start = std::chrono::steady_clock::now();
}

PerfScope::~PerfScope()
{
    if (!_active)
        return;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _start);
    sPlayerbotsPerf.Record(_name, uint64(elapsed.count()));
}
}
