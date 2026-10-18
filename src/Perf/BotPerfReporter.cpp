#include "BotPerfReporter.h"
#include "Channel.h"
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
#include <algorithm>
#include <cstdio>
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
}

void BotPerfReporter::LogMemory()
{
    uint32 loaded = 0;
    sMapMgr->DoForAllMaps([&loaded](Map* map)
    {
        for (uint32 id = 0; id < 64 * 64; ++id)
            if (map->IsGridLoaded(id))
                ++loaded;
    });
    TC_LOG_INFO(PERF_LOG, "memory rss_mb={} bots={} loaded_grids={}", ReadRssMB(), sPlayerbotMgr->GetRandomBotCount(), loaded);
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
