#ifndef BotPerfReporter_h__
#define BotPerfReporter_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <bitset>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace Playerbots
{
/// Load tools for benchmarking, written to the dedicated perf log ("server.playerbots.perf" -> Playerbots-Perf.log): the memory line
/// (rss, bots, loaded grids), the world tick window report and slow-tick host state (free memory, swap, page faults), what created new
/// grids, and the synthetic chat driver. Nothing here touches the core. World thread only.
class BotPerfReporter
{
public:
    static BotPerfReporter* instance();

    /// Every world tick (from OnWorldUpdate)
    void Update(uint32 diff);

    /// Synthetic chat: rate messages per second spread over random online bots; kind = say|yell|emote|channel|guild|mix
    bool StartChat(float ratePerSecond, std::string const& kind, std::string& error);
    void StopChat();
    std::string ChatStatus() const;

    /// Resident set size of this process in MiB (0 if unreadable)
    static uint32 ReadRssMB();

private:
    void LogMemory();
    static uint32 CountLoadedGrids();
    void NoteTick(uint32 diff);
    void LogSlowTick(uint32 diff);
    void GridCensus(uint32 diff);
    static void LogGridStock();

    struct HostState
    {
        uint64 MemAvailableMB = 0;
        uint64 SwapFreeMB = 0;
        uint64 ShmemMB = 0;
        uint64 SwapIn = 0;
        uint64 SwapOut = 0;
        uint64 MajorFaults = 0;
        float IoPressure10 = -1.0f;
        float MemPressure10 = -1.0f;
    };
    static HostState ReadHostState();

    struct GateSnapshot
    {
        uint64 HopDeferred = 0, HopPlacedLoaded = 0, HopPlacedHuman = 0, HopFinal = 0, HopPlacedAlways = 0, HopMaterialized = 0, NotifyCleared = 0;
    };

    // world tick window
    std::vector<uint32> _ticks;
    uint32 _tickWindowMs = 0;
    uint32 _slowTicks = 0;
    uint32 _slowMs = 0;
    bool _slowLogged = false;
    uint32 _lastSlowLogMs = 0;
    uint64 _prevTickEndNs = 0;
    HostState _hostPrev;
    bool _hostPrevValid = false;
    uint32 _hostTimer = 0;
    GateSnapshot _gatesPrev;

    // grid census
    uint32 _censusTimer = 0;
    uint32 _censusWindowMs = 0;
    bool _censusStarted = false;
    uint32 _censusNew = 0;
    uint32 _censusUnloaded = 0;
    uint32 _censusDetail = 0;
    std::map<std::string, uint32> _censusCauses;
    std::unordered_map<uint32, std::bitset<64 * 64>> _gridsSeen;
    void DriveChat(uint32 diff);
    void RefreshChatBots();

    uint32 _memTimer = 0;
    bool _chatActive = false;
    bool _chatAutoStarted = false;
    float _chatRate = 0.f;
    std::string _chatKind;
    double _chatBudget = 0.0;
    uint32 _chatBotRefresh = 0;
    std::vector<ObjectGuid> _chatBots;
    uint64 _chatSent = 0;
    uint64 _chatNoTarget = 0;
};
}

#define sBotPerfReporter Playerbots::BotPerfReporter::instance()

#endif // BotPerfReporter_h__
