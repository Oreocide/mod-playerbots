#ifndef PlayerbotsGateCounters_h__
#define PlayerbotsGateCounters_h__

#include "Define.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace Playerbots
{
/// How often each switch that avoids a grid load fired, and where bots were placed by a route hop. Written from the map threads (bot AI
/// ticks) and the world thread, read by BotPerfReporter. The counters are only ever added to; the reporter logs the difference per window.
struct GateCounters
{
    std::atomic<uint64> HopDeferred{ 0 };        // Travel.DeferHops: the hop was not placed, the bot stayed where it was
    std::atomic<uint64> HopPlacedLoaded{ 0 };    // the hop target grid was already loaded, the bot was placed there as before
    std::atomic<uint64> HopPlacedHuman{ 0 };     // a player is near the hop target, the bot was placed there as before
    std::atomic<uint64> HopFinal{ 0 };           // last stretch of the route: always placed
    std::atomic<uint64> HopPlacedAlways{ 0 };    // Travel.DeferHops off: every hop is placed
    std::atomic<uint64> HopMaterialized{ 0 };    // a player came near a deferred bot: it was placed where the route had got to
    std::atomic<uint64> NotifyCleared{ 0 };      // FarBotNoVisibilityLoad: a pending visibility notify of a far bot was dropped

    /// When the module's hook of each world map finished its part of the last map update (steady clock, ns; 0 = never). The maps update on
    /// their own threads, so on a slow world tick the map that finished last is the slow one
    static constexpr uint32 MaxStampedMaps = 4096;
    std::array<std::atomic<uint64>, MaxStampedMaps> MapFinishNs{};

    static uint64 SteadyNs()
    {
        return uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void NoteMapFinish(uint32 mapId)
    {
        if (mapId < MaxStampedMaps)
            MapFinishNs[mapId].store(SteadyNs(), std::memory_order_relaxed);
    }

    static GateCounters& Get()
    {
        static GateCounters counters;
        return counters;
    }

    /// A route hop placed a bot in this grid (map thread). The grid census asks whether a new grid was created by one
    void RecordHopLanding(uint32 mapId, uint32 gridId, uint32 nowMs)
    {
        std::lock_guard lock(_landingLock);
        if (_landings.size() >= 512)
            _landings.erase(_landings.begin(), _landings.begin() + 256);
        _landings.push_back({ mapId, gridId, nowMs });
    }

    bool WasHopLanding(uint32 mapId, uint32 gridId, uint32 nowMs, uint32 withinMs)
    {
        std::lock_guard lock(_landingLock);
        for (Landing const& landing : _landings)
            if (landing.MapId == mapId && landing.GridId == gridId && nowMs - landing.AtMs <= withinMs)
                return true;
        return false;
    }

private:
    struct Landing
    {
        uint32 MapId;
        uint32 GridId;
        uint32 AtMs;
    };
    std::mutex _landingLock;
    std::vector<Landing> _landings;
};
}

#define sGateCounters Playerbots::GateCounters::Get()

#endif // PlayerbotsGateCounters_h__
