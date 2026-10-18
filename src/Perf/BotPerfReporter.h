#ifndef BotPerfReporter_h__
#define BotPerfReporter_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Player;

namespace Playerbots
{
/// Load tools for benchmarking: the memory log line (rss, bots, loaded grids) written to the dedicated perf log
/// ("server.playerbots.perf" -> Playerbots-Perf.log) and the synthetic chat driver. World thread only.
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
