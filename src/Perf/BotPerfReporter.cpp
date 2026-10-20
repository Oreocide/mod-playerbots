#include "BotPerfReporter.h"
#include "BotSession.h"
#include "Channel.h"
#include "GateCounters.h"
#include "GameTime.h"
#include "Guild.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "StringFormat.h"
#include <algorithm>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace Playerbots
{
namespace
{
#define PERF_LOG "server.playerbots.perf"
}

BotPerfReporter* BotPerfReporter::instance()
{
    static BotPerfReporter inst;
    return &inst;
}

uint32 BotPerfReporter::ReadRssMB()
{
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    unsigned long size = 0, resident = 0;
    int n = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);
    if (n != 2)
        return 0;
    return uint32(uint64(resident) * uint64(sysconf(_SC_PAGESIZE)) / (1024ull * 1024ull));
}

void BotPerfReporter::Update(uint32 diff)
{
    if (_chatActive)
        DriveChat(diff);

    if (!sPlayerbotsConfig.PerfMonEnabled)
        return;

    if (!_chatAutoStarted && !sPlayerbotsConfig.PerfMonChatDriver.empty()
        && sPlayerbotMgr->GetRandomBotCount() * 10 >= std::max<size_t>(1, sPlayerbotsConfig.RandomCount) * 9)
    {
        _chatAutoStarted = true;
        float rate = 0.f;
        char kind[32] = { };
        if (std::sscanf(sPlayerbotsConfig.PerfMonChatDriver.c_str(), "%f %31s", &rate, kind) == 2)
        {
            std::string error;
            if (StartChat(rate, kind, error))
                TC_LOG_INFO(PERF_LOG, "chat driver auto-started: {} msg/s {}", rate, kind);
            else
                TC_LOG_ERROR(PERF_LOG, "chat driver not started: {}", error);
        }
    }

    _memTimer += diff;
    if (sPlayerbotsConfig.PerfMonMemorySeconds && _memTimer >= sPlayerbotsConfig.PerfMonMemorySeconds * 1000)
    {
        _memTimer = 0;
        LogMemory();
    }

    NoteTick(diff);
    GridCensus(diff);
    _prevTickEndNs = GateCounters::SteadyNs();
}

uint32 BotPerfReporter::CountLoadedGrids()
{
    uint32 loaded = 0;
    sMapMgr->DoForAllMaps([&loaded](Map* map)
    {
        for (uint32 id = 0; id < 64 * 64; ++id)
            if (map->IsGridLoaded(id))
                ++loaded;
    });
    return loaded;
}

void BotPerfReporter::LogMemory()
{
    TC_LOG_INFO(PERF_LOG, "memory rss_mb={} bots={} loaded_grids={}", ReadRssMB(), sPlayerbotMgr->GetRandomBotCount(), CountLoadedGrids());
}

// ---- world tick report ----

namespace
{
uint64 ReadKbField(char const* text, char const* key)
{
    char const* at = std::strstr(text, key);
    if (!at)
        return 0;
    return std::strtoull(at + std::strlen(key), nullptr, 10);
}

std::string ReadWholeFile(char const* path)
{
    std::string content;
    if (FILE* f = std::fopen(path, "r"))
    {
        char buffer[4096];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
            content.append(buffer, n);
        std::fclose(f);
    }
    return content;
}

// "some avg10=1.23 ..." of a pressure file, -1 if the kernel has none
float ReadPsiSome10(char const* path)
{
    std::string text = ReadWholeFile(path);
    char const* at = std::strstr(text.c_str(), "some avg10=");
    return at ? std::strtof(at + 11, nullptr) : -1.0f;
}

uint64 Percentile(std::vector<uint32> const& sorted, double fraction)
{
    if (sorted.empty())
        return 0;
    size_t index = std::min(sorted.size() - 1, size_t(fraction * double(sorted.size())));
    return sorted[index];
}
}

// What the host was doing around a slow tick: free memory, swap and page faults decide whether the world thread was waiting for the
// disk. Counters are differences to the last time this was read.
BotPerfReporter::HostState BotPerfReporter::ReadHostState()
{
    HostState state;
    std::string meminfo = ReadWholeFile("/proc/meminfo");
    state.MemAvailableMB = ReadKbField(meminfo.c_str(), "MemAvailable:") / 1024;
    state.SwapFreeMB = ReadKbField(meminfo.c_str(), "SwapFree:") / 1024;
    state.ShmemMB = ReadKbField(meminfo.c_str(), "Shmem:") / 1024;
    std::string vmstat = ReadWholeFile("/proc/vmstat");
    state.SwapIn = ReadKbField(vmstat.c_str(), "\npswpin ");
    state.SwapOut = ReadKbField(vmstat.c_str(), "\npswpout ");
    state.MajorFaults = ReadKbField(vmstat.c_str(), "\npgmajfault ");
    state.IoPressure10 = ReadPsiSome10("/proc/pressure/io");
    state.MemPressure10 = ReadPsiSome10("/proc/pressure/memory");
    return state;
}

void BotPerfReporter::LogSlowTick(uint32 diff)
{
    HostState now = ReadHostState();
    HostState const& before = _hostPrev;
    uint64 swapIn = _hostPrevValid ? now.SwapIn - before.SwapIn : 0;
    uint64 swapOut = _hostPrevValid ? now.SwapOut - before.SwapOut : 0;
    uint64 majorFaults = _hostPrevValid ? now.MajorFaults - before.MajorFaults : 0;
    _hostPrev = now;
    _hostPrevValid = true;

    // the map whose hook finished last made the tick long; a lone late map names the slow map thread
    GateCounters& gates = sGateCounters;
    struct Finish { uint32 MapId; double AfterPrevEndMs; };
    std::vector<Finish> finished;
    for (uint32 id = 0; id < GateCounters::MaxStampedMaps; ++id)
        if (uint64 at = gates.MapFinishNs[id].load(std::memory_order_relaxed); at > _prevTickEndNs)
            finished.push_back({ id, double(at - _prevTickEndNs) / 1e6 });
    std::sort(finished.begin(), finished.end(), [](Finish const& a, Finish const& b) { return a.AfterPrevEndMs > b.AfterPrevEndMs; });
    std::string maps;
    for (size_t i = 0; i < finished.size() && i < 4; ++i)
        maps += Trinity::StringFormat("{}{}:{:.0f}ms", i ? " " : "", finished[i].MapId, finished[i].AfterPrevEndMs);

    TC_LOG_INFO(PERF_LOG, "slowtick diff_ms={} rss_mb={} mem_avail_mb={} swap_free_mb={} shmem_mb={} swapin_pages={} swapout_pages={} major_faults={} psi_io10={:.1f} psi_mem10={:.1f} bots={} loaded_grids={} last_maps=[{}]",
        diff, ReadRssMB(), now.MemAvailableMB, now.SwapFreeMB, now.ShmemMB, swapIn, swapOut, majorFaults, now.IoPressure10, now.MemPressure10,
        sPlayerbotMgr->GetRandomBotCount(), CountLoadedGrids(), maps);
}

void BotPerfReporter::NoteTick(uint32 diff)
{
    uint32 reportSeconds = sPlayerbotsConfig.PerfMonReportSeconds;
    if (!reportSeconds)
        return;

    if (_ticks.size() < 200000)
        _ticks.push_back(diff);
    _tickWindowMs += diff;

    uint32 slowMs = sPlayerbotsConfig.PerfMonSlowTickMs;

    // The host counters are sampled once a second, so that a slow tick can report the difference to the last sample before it: a stall
    // blocks this timer too, and the difference then covers the stall and little else
    if (diff < slowMs)
    {
        _hostTimer += diff;
        if (_hostTimer >= 1000)
        {
            _hostTimer = 0;
            _hostPrev = ReadHostState();
            _hostPrevValid = true;
        }
    }

    if (diff >= slowMs)
    {
        ++_slowTicks;
        _slowMs += diff;
        uint32 now = GameTime::GetGameTimeMS();
        // one line per second at most: a stall of many seconds is one tick, but a run of slow ticks must not flood the log
        if (!_slowLogged || now - _lastSlowLogMs >= 1000)
        {
            _slowLogged = true;
            _lastSlowLogMs = now;
            _hostTimer = 0;
            LogSlowTick(diff);
        }
    }

    if (_tickWindowMs < reportSeconds * 1000)
        return;

    std::vector<uint32> sorted = _ticks;
    std::sort(sorted.begin(), sorted.end());
    uint32 over1s = 0, over5s = 0;
    for (uint32 d : sorted)
    {
        over1s += d >= 1000;
        over5s += d >= 5000;
    }
    TC_LOG_INFO(PERF_LOG, "tick window_s={} ticks={} p50_ms={} p95_ms={} p99_ms={} max_ms={} over_slow={} slow_ms_total={} over_1s={} over_5s={} bots={} loaded_grids={}",
        _tickWindowMs / 1000, sorted.size(), Percentile(sorted, 0.50), Percentile(sorted, 0.95), Percentile(sorted, 0.99),
        sorted.empty() ? 0u : sorted.back(), _slowTicks, _slowMs, over1s, over5s, sPlayerbotMgr->GetRandomBotCount(), CountLoadedGrids());

    GateCounters& gates = sGateCounters;
    GateSnapshot now{ gates.HopDeferred.load(), gates.HopPlacedLoaded.load(), gates.HopPlacedHuman.load(), gates.HopFinal.load(),
        gates.HopPlacedAlways.load(), gates.HopMaterialized.load(), gates.NotifyCleared.load() };
    TC_LOG_INFO(PERF_LOG, "gates hop_deferred={} hop_placed_loaded={} hop_placed_human={} hop_final={} hop_placed_always={} hop_materialized={} notify_cleared={}",
        now.HopDeferred - _gatesPrev.HopDeferred, now.HopPlacedLoaded - _gatesPrev.HopPlacedLoaded, now.HopPlacedHuman - _gatesPrev.HopPlacedHuman,
        now.HopFinal - _gatesPrev.HopFinal, now.HopPlacedAlways - _gatesPrev.HopPlacedAlways, now.HopMaterialized - _gatesPrev.HopMaterialized,
        now.NotifyCleared - _gatesPrev.NotifyCleared);
    _gatesPrev = now;

    _ticks.clear();
    _tickWindowMs = 0;
    _slowTicks = 0;
    _slowMs = 0;
}

// ---- who creates grids ----

// Once a second: which grids exist now that did not a second ago, and who stood in or next to each (the cause of a grid load is
// not visible from here, but a bot that is the only player around it points at the bot's own placement, a route hop or the
// visibility range of its relocation). Window totals every PerfMon.GridCensusSeconds.
void BotPerfReporter::GridCensus(uint32 diff)
{
    uint32 windowSeconds = sPlayerbotsConfig.PerfMonGridCensusSeconds;
    if (!windowSeconds)
        return;

    _censusTimer += diff;
    if (_censusTimer < 1000)
        return;
    _censusTimer = 0;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    bool const first = !_censusStarted;
    _censusStarted = true;

    sMapMgr->DoForAllMaps([&](Map* map)
    {
        std::bitset<64 * 64>& seen = _gridsSeen[map->GetId() * 1000 + map->GetInstanceId() % 1000];
        for (uint32 id = 0; id < 64 * 64; ++id)
        {
            bool loaded = map->IsGridLoaded(id);
            if (loaded == seen.test(id))
                continue;

            seen.set(id, loaded);
            if (first)
                continue;
            if (!loaded)
            {
                ++_censusUnloaded;
                continue;
            }

            ++_censusNew;
            uint32 gx = id % 64, gy = id / 64;
            std::string cause;
            if (sGateCounters.WasHopLanding(map->GetId(), id, nowMs, 10000))
                cause = "hop_landing";
            else
            {
                // rank: a player inside the grid beats one in a neighbouring grid; human, teleporting bot, active bot, dormant bot
                int best = 99;
                for (MapReference const& ref : map->GetPlayers())
                {
                    Player* player = ref.GetSource();
                    if (!player || !player->IsInWorld())
                        continue;
                    GridCoord at = Trinity::ComputeGridCoord(player->GetPositionX(), player->GetPositionY());
                    uint32 dx = at.x_coord > gx ? at.x_coord - gx : gx - at.x_coord;
                    uint32 dy = at.y_coord > gy ? at.y_coord - gy : gy - at.y_coord;
                    if (dx > 1 || dy > 1)
                        continue;

                    int rank;
                    if (!IsBotSession(player->GetSession()))
                        rank = 0;
                    else if (player->IsBeingTeleported())
                        rank = 1;
                    else if (!player->GetSession()->IsDormant())
                        rank = 2;
                    else
                        rank = 3;
                    best = std::min(best, rank + ((dx || dy) ? 10 : 0));
                }
                static char const* const names[] = { "in_human", "in_bot_teleporting", "in_bot_active", "in_bot_dormant" };
                if (best == 99)
                    cause = "none";
                else
                    cause = std::string(best >= 10 ? "next_to_" : "in_") + (names[best % 10] + 3);
                if (best == 99 && _censusDetail < 12)
                {
                    ++_censusDetail;
                    TC_LOG_INFO(PERF_LOG, "gridnone map={} grid={},{}", map->GetId(), gx, gy);
                }
            }
            ++_censusCauses[cause];
        }
    });

    _censusWindowMs += 1000;
    if (_censusWindowMs < windowSeconds * 1000)
        return;

    std::string causes;
    for (auto const& [name, count] : _censusCauses)
        causes += Trinity::StringFormat(" {}={}", name, count);
    TC_LOG_INFO(PERF_LOG, "gridwin window_s={} new={} unloaded={} loaded={} bots={}{}", _censusWindowMs / 1000, _censusNew, _censusUnloaded, CountLoadedGrids(),
        sPlayerbotMgr->GetRandomBotCount(), causes);
    LogGridStock();
    _censusCauses.clear();
    _censusNew = 0;
    _censusUnloaded = 0;
    _censusWindowMs = 0;
    _censusDetail = 0;
}

// What the loaded grids are held by right now (the window totals above only classify grids as they appear). Each loaded grid is ranked by
// the players INSIDE it (human, teleporting bot, active bot, dormant bot) and, if it holds none, by the players in the 8 grids around it
// (a human there loads it for sight; a bot there only loads it through its own visibility range). "none" is a grid nobody stands in or
// next to: waiting out GridCleanUpDelay, or kept by active creatures. The extra counters size the levers: next_to_bot_* is what a bot
// that loaded only its own grid would not need, dormant_only_no_human is how many grids hold only dormant bots (informational: lazy
// navmesh for them was dropped, see CORE_EXIT.md), and dormant_bots_per_grid shows how well placement concentrates the frozen bots.
void BotPerfReporter::LogGridStock()
{
    struct Occupancy
    {
        uint8 Human = 0;
        uint8 Teleporting = 0;
        uint8 Active = 0;
        uint16 Dormant = 0;
    };

    uint32 loaded = 0, inHuman = 0, nearHuman = 0, inTele = 0, inActive = 0, inDormant = 0, nextActive = 0, nextDormant = 0, none = 0;
    uint32 dormantOnlyNoHuman = 0, dormantBotsInThem = 0, maxDormantInGrid = 0;
    std::map<uint32, uint32> perMap;

    sMapMgr->DoForAllMaps([&](Map* map)
    {
        std::unordered_map<uint32, Occupancy> occupancy;
        for (MapReference const& ref : map->GetPlayers())
        {
            Player* player = ref.GetSource();
            if (!player || !player->IsInWorld())
                continue;

            GridCoord at = Trinity::ComputeGridCoord(player->GetPositionX(), player->GetPositionY());
            Occupancy& slot = occupancy[at.y_coord * 64 + at.x_coord];
            if (!IsBotSession(player->GetSession()))
                ++slot.Human;
            else if (player->IsBeingTeleported())
                ++slot.Teleporting;
            else if (!player->GetSession()->IsDormant())
                ++slot.Active;
            else
                ++slot.Dormant;
        }

        for (uint32 id = 0; id < 64 * 64; ++id)
        {
            if (!map->IsGridLoaded(id))
                continue;

            ++loaded;
            ++perMap[map->GetId() * 1000 + map->GetInstanceId() % 1000];
            int32 const gx = id % 64, gy = id / 64;

            Occupancy own;
            if (auto itr = occupancy.find(id); itr != occupancy.end())
                own = itr->second;

            bool humanAround = own.Human > 0, activeAround = own.Active > 0 || own.Teleporting > 0, dormantAround = own.Dormant > 0;
            for (int32 dy = -1; dy <= 1; ++dy)
                for (int32 dx = -1; dx <= 1; ++dx)
                {
                    int32 nx = gx + dx, ny = gy + dy;
                    if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= 64 || ny >= 64)
                        continue;
                    auto itr = occupancy.find(ny * 64 + nx);
                    if (itr == occupancy.end())
                        continue;
                    humanAround |= itr->second.Human > 0;
                    activeAround |= itr->second.Active > 0 || itr->second.Teleporting > 0;
                    dormantAround |= itr->second.Dormant > 0;
                }

            if (own.Human)
                ++inHuman;
            else if (own.Teleporting)
                ++inTele;
            else if (own.Active)
                ++inActive;
            else if (own.Dormant)
                ++inDormant;
            else if (humanAround)
                ++nearHuman;
            else if (activeAround)
                ++nextActive;
            else if (dormantAround)
                ++nextDormant;
            else
                ++none;

            if (own.Dormant && !own.Human && !own.Active && !own.Teleporting && !humanAround)
            {
                ++dormantOnlyNoHuman;
                dormantBotsInThem += own.Dormant;
                maxDormantInGrid = std::max<uint32>(maxDormantInGrid, own.Dormant);
            }
        }
    });

    std::string maps;
    for (auto const& [key, count] : perMap)
        maps += Trinity::StringFormat("{}{}:{}", maps.empty() ? "" : ",", key, count);
    TC_LOG_INFO(PERF_LOG, "gridstock loaded={} in_human={} near_human={} in_bot_teleporting={} in_bot_active={} in_bot_dormant={} next_to_bot_active={} next_to_bot_dormant={} none={} "
        "dormant_only_no_human={} dormant_bots_in_them={} dormant_max_per_grid={} bots={} maps=[{}]",
        loaded, inHuman, nearHuman, inTele, inActive, inDormant, nextActive, nextDormant, none,
        dormantOnlyNoHuman, dormantBotsInThem, maxDormantInGrid, sPlayerbotMgr->GetRandomBotCount(), maps);
}

// ---- synthetic chat driver ----

bool BotPerfReporter::StartChat(float ratePerSecond, std::string const& kind, std::string& error)
{
    if (ratePerSecond <= 0.f || ratePerSecond > 2000.f)
    {
        error = "rate must be between 0 and 2000 messages per second";
        return false;
    }
    if (kind != "say" && kind != "yell" && kind != "emote" && kind != "channel" && kind != "guild" && kind != "mix")
    {
        error = "kind must be say, yell, emote, channel, guild or mix";
        return false;
    }
    _chatActive = true;
    _chatRate = ratePerSecond;
    _chatKind = kind;
    _chatBudget = 0.0;
    _chatBotRefresh = 0;
    _chatBots.clear();
    return true;
}

void BotPerfReporter::StopChat()
{
    _chatActive = false;
}

std::string BotPerfReporter::ChatStatus() const
{
    if (!_chatActive)
        return "chat driver off (sent " + std::to_string(_chatSent) + " in total)";
    return "chat driver on: " + _chatKind + " at " + std::to_string(_chatRate) + " msg/s, sent " + std::to_string(_chatSent) + ", no target " + std::to_string(_chatNoTarget);
}

void BotPerfReporter::RefreshChatBots()
{
    _chatBots.clear();
    sPlayerbotMgr->ForEachRandomBot([this](Player* bot, PlayerbotAI*) { _chatBots.push_back(bot->GetGUID()); });
}

void BotPerfReporter::DriveChat(uint32 diff)
{
    _chatBotRefresh += diff;
    if (_chatBots.empty() || _chatBotRefresh >= 5000)
    {
        _chatBotRefresh = 0;
        RefreshChatBots();
    }
    if (_chatBots.empty())
        return;

    // never more than one second worth of backlog, so a slow tick does not turn into a burst
    _chatBudget = std::min(_chatBudget + double(_chatRate) * double(diff) / 1000.0, double(_chatRate));
    while (_chatBudget >= 1.0)
    {
        _chatBudget -= 1.0;
        Player* bot = ObjectAccessor::FindPlayer(_chatBots[urand(0, uint32(_chatBots.size() - 1))]);
        if (!bot || !bot->IsInWorld())
        {
            ++_chatNoTarget;
            continue;
        }

        std::string kind = _chatKind;
        if (kind == "mix")
        {
            static char const* const kinds[] = { "say", "say", "say", "yell", "emote", "channel", "channel", "guild" };
            kind = kinds[urand(0, 7)];
        }

        std::string text = "perf message " + std::to_string(_chatSent);
        bool sent = false;
        if (kind == "say")
            bot->Say(text, LANG_UNIVERSAL), sent = true;
        else if (kind == "yell")
            bot->Yell(text, LANG_UNIVERSAL), sent = true;
        else if (kind == "emote")
            bot->TextEmote(text), sent = true;
        else if (kind == "channel")
        {
            auto const& channels = bot->GetJoinedChannels();
            if (!channels.empty())
            {
                auto it = channels.begin();
                std::advance(it, urand(0, uint32(channels.size() - 1)));
                (*it)->Say(bot->GetGUID(), text, LANG_UNIVERSAL);
                sent = true;
            }
        }
        else if (kind == "guild")
        {
            if (Guild* guild = bot->GetGuild())
            {
                guild->BroadcastToGuild(bot->GetSession(), false, text, LANG_UNIVERSAL);
                sent = true;
            }
        }

        if (sent)
        {
            ++_chatSent;
        }
        else
            ++_chatNoTarget;
    }
}
}
