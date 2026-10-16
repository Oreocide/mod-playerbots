#ifndef BotPerfReporter_h__
#define BotPerfReporter_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class Player;

namespace Playerbots
{
/// Formats the BotPerf counters into the dedicated perf log ("server.playerbots.perf" -> Playerbots-Perf.log) and runs the
/// censuses (population, grids, memory, channels), the human arrive/leave tracker and the synthetic chat driver.
/// World thread only, except the Note* counters which are single-writer atomics.
class BotPerfReporter
{
public:
    static BotPerfReporter* instance();

    /// Every world tick (from OnWorldUpdate)
    void Update(uint32 diff);
    /// Writes a window report now (".bot perf report")
    void ForceReport();
    /// Config changed: pushes the enable flag and tunables into the core probes
    void ApplyConfig();

    // bot state counters (single writer: the thread running the bot AI)
    void NoteFreeze() { _freezes.fetch_add(1, std::memory_order_relaxed); }
    void NoteThaw() { _thaws.fetch_add(1, std::memory_order_relaxed); }
    void NoteChannelJoin() { _channelJoins.fetch_add(1, std::memory_order_relaxed); }
    /// World thread: a random bot finished logging in, wall milliseconds since AddRandomBot
    void NoteLoginWall(uint32 ms) { _loginWallMs.push_back(ms); }

    /// Immediate extra lines for ".bot perf grids" / ".bot perf mem"
    void LogGridCensus();
    void LogMemCensus();

    /// Synthetic chat: rate messages per second spread over random online bots; kind = say|yell|emote|channel|guild|mix
    bool StartChat(float ratePerSecond, std::string const& kind, std::string& error);
    void StopChat();
    std::string ChatStatus() const;

    /// Resident set size of this process in MiB (0 if unreadable)
    static uint32 ReadRssMB();

private:
    struct HumanEvent
    {
        char const* Kind = "";
        uint64 Guid = 0;
        uint32 MapId = 0;
        uint32 ElapsedMs = 0;
        uint64 Thaws0 = 0;
        uint64 Freezes0 = 0;
        uint32 SimBefore = 0;
        uint32 PeakDiff = 0;
        uint64 SumDiff = 0;
        uint32 Ticks = 0;
    };

    void Report();
    void LogMemory();
    void TrackHumans(uint32 diff);
    void DriveChat(uint32 diff);
    void RefreshChatBots();
    uint32 CountSimulated() const;

    uint32 _reportTimer = 0;
    uint32 _gridTimer = 0;
    uint32 _memTimer = 0;
    uint32 _memCensusTimer = 0;
    bool _wasEnabled = false;
    uint64 _reportSeq = 0;
    std::vector<uint32> _loginWallMs;

    std::atomic<uint64> _freezes{ 0 };
    std::atomic<uint64> _thaws{ 0 };
    std::atomic<uint64> _channelJoins{ 0 };

    std::shared_ptr<void const> _lastHumansHolder;
    struct HumanPos { uint64 Guid; uint32 MapId; float X; float Y; };
    std::vector<HumanPos> _lastHumans;
    bool _haveHumans = false;
    std::vector<HumanEvent> _events;

    bool _chatActive = false;
    bool _chatAutoStarted = false;
    float _chatRate = 0.f;
    std::string _chatKind;
    double _chatBudget = 0.0;
    uint32 _chatBotRefresh = 0;
    std::vector<ObjectGuid> _chatBots;
    uint64 _chatSent = 0;
    uint64 _chatSentThisWindow = 0;
    uint64 _chatNoTarget = 0;
};
}

#define sBotPerfReporter Playerbots::BotPerfReporter::instance()

#endif // BotPerfReporter_h__
