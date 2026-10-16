#include "BotPerfReporter.h"
#include "BotPerf.h"
#include "Channel.h"
#include "CollectionMgr.h"
#include "GameTime.h"
#include "Guild.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "StringFormat.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "Player.h"
#include "Random.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <unistd.h>
#include <unordered_map>

namespace Playerbots
{
namespace
{
#define PERF_LOG "server.playerbots.perf"

double Ms(uint64 ns) { return double(ns) / 1e6; }
double MsFromUs(uint64 us) { return double(us) / 1e3; }

struct ClassCounts
{
    uint32 Online = 0;
    std::array<uint32, BotPerf::CLS_MAX> ByClass = { };
};
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

void BotPerfReporter::ApplyConfig()
{
    BotPerf::SetSlowTickMs(sPlayerbotsConfig.PerfMonSlowTickMs);
    BotPerf::SetOpcodeStats(sPlayerbotsConfig.PerfMonOpcodes);
    BotPerf::SetLoginDetail(sPlayerbotsConfig.PerfMonLoginDetail);
    WorldSession::SetBotFrozenTickMs(sPlayerbotsConfig.FrozenTickMs);
    BotPerf::BotOpts::Set(sPlayerbotsConfig.SleepIdleActiveObjects, sPlayerbotsConfig.LazyMMap, sPlayerbotsConfig.SkipBotVisibilityGridLoad);
    BotPerf::SetEnabled(sPlayerbotsConfig.PerfMonEnabled);
}

uint32 BotPerfReporter::CountSimulated() const
{
    uint32 n = 0;
    sPlayerbotMgr->ForEachRandomBot([&n](Player* bot, PlayerbotAI*)
    {
        if (!bot->GetSession()->IsBotFrozen())
            ++n;
    });
    return n;
}

void BotPerfReporter::Update(uint32 diff)
{
    bool enabled = sPlayerbotsConfig.PerfMonEnabled;
    if (enabled != _wasEnabled)
    {
        _wasEnabled = enabled;
        _reportTimer = _gridTimer = _memTimer = _memCensusTimer = 0;
        if (enabled)
            TC_LOG_INFO(PERF_LOG, "perf: instrumentation on, report every {} s", sPlayerbotsConfig.PerfMonReportSeconds);
    }
    if (!enabled)
    {
        if (_chatActive)
            DriveChat(diff);
        return;
    }

    BotPerf::Scope self(BotPerf::M_REPORTER);

    TrackHumans(diff);
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
    if (_chatActive)
        DriveChat(diff);

    _memTimer += diff;
    if (sPlayerbotsConfig.PerfMonMemorySeconds && _memTimer >= sPlayerbotsConfig.PerfMonMemorySeconds * 1000)
    {
        _memTimer = 0;
        LogMemory();
    }

    _gridTimer += diff;
    if (sPlayerbotsConfig.PerfMonGridCensusSeconds && _gridTimer >= sPlayerbotsConfig.PerfMonGridCensusSeconds * 1000)
    {
        _gridTimer = 0;
        LogGridCensus();
    }

    _memCensusTimer += diff;
    if (sPlayerbotsConfig.PerfMonMemCensusSeconds && _memCensusTimer >= sPlayerbotsConfig.PerfMonMemCensusSeconds * 1000)
    {
        _memCensusTimer = 0;
        LogMemCensus();
    }

    _reportTimer += diff;
    if (_reportTimer >= std::max<uint32>(5, sPlayerbotsConfig.PerfMonReportSeconds) * 1000)
    {
        _reportTimer = 0;
        Report();
    }
}

void BotPerfReporter::ForceReport()
{
    Report();
}

void BotPerfReporter::Report()
{
    BotPerf::Window w;
    BotPerf::DrainWindow(w);
    if (!w.Ticks)
        return;

    ++_reportSeq;
    double wallS = double(w.WallNs) / 1e9;
    TC_LOG_INFO(PERF_LOG, "report seq={} window_s={:.1f} ticks={} tick_p50_ms={:.1f} tick_p95_ms={:.1f} tick_max_ms={:.1f} diff_p50_ms={:.1f} diff_p95_ms={:.1f} diff_max_ms={}",
        _reportSeq, wallS, w.Ticks,
        MsFromUs(BotPerf::HistPercentileUs(w.TickHist, 0.5)), MsFromUs(BotPerf::HistPercentileUs(w.TickHist, 0.95)), Ms(w.MaxTickNs),
        MsFromUs(BotPerf::HistPercentileUs(w.DiffHist, 0.5)), MsFromUs(BotPerf::HistPercentileUs(w.DiffHist, 0.95)), w.MaxDiffMs);

    // population by class and map
    std::unordered_map<uint32, ClassCounts> byMap;
    ClassCounts total;
    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI*)
    {
        uint8 cls = bot->GetSession()->GetBotPerfClass();
        ClassCounts& m = byMap[bot->GetMapId()];
        ++m.Online;
        ++m.ByClass[cls];
        ++total.Online;
        ++total.ByClass[cls];
    });
    uint32 humans = 0;
    if (auto list = sPlayerbotMgr->GetNearbyHumans())
        humans = uint32(list->size());
    TC_LOG_INFO(PERF_LOG, "pop random_online={} frozen={} sim_rotation={} sim_near={} grouped={} instance={} humans={} freezes_total={} thaws_total={} channel_joins_total={} rss_mb={} char_db_queue={} login_db_queue={} world_db_queue={}",
        total.Online, total.ByClass[BotPerf::CLS_BOT_FROZEN], total.ByClass[BotPerf::CLS_BOT_SIM_ROTATION], total.ByClass[BotPerf::CLS_BOT_SIM_NEAR],
        total.ByClass[BotPerf::CLS_BOT_GROUPED], total.ByClass[BotPerf::CLS_BOT_INSTANCE], humans,
        _freezes.load(std::memory_order_relaxed), _thaws.load(std::memory_order_relaxed), _channelJoins.load(std::memory_order_relaxed), ReadRssMB(),
        CharacterDatabase.QueueSize(), LoginDatabase.QueueSize(), WorldDatabase.QueueSize());

    // wall time from the login request to the first world-thread tick with the bot in the world
    if (!_loginWallMs.empty())
    {
        std::sort(_loginWallMs.begin(), _loginWallMs.end());
        TC_LOG_INFO(PERF_LOG, "login n={} p50_ms={} p95_ms={} max_ms={}", _loginWallMs.size(), _loginWallMs[_loginWallMs.size() / 2],
            _loginWallMs[std::min(_loginWallMs.size() - 1, _loginWallMs.size() * 95 / 100)], _loginWallMs.back());
        _loginWallMs.clear();
    }

    // per map: Map::Update time and who is on it
    std::sort(w.Maps.begin(), w.Maps.end(), [](BotPerf::MapStat const& a, BotPerf::MapStat const& b) { return a.Ns > b.Ns; });
    uint32 shown = 0;
    for (BotPerf::MapStat const& m : w.Maps)
    {
        if (++shown > 12)
            break;
        ClassCounts const& c = byMap[m.MapId];
        TC_LOG_INFO(PERF_LOG, "map id={} inst={} updates={} avg_ms={:.2f} total_ms={:.0f} avg_players={:.1f} avg_bots={:.1f} online_bots={} frozen={} sim={}",
            m.MapId, m.InstanceId, m.Calls, m.Calls ? Ms(m.Ns) / double(m.Calls) : 0.0, Ms(m.Ns),
            m.Calls ? double(m.PlayerTicks) / double(m.Calls) : 0.0, m.Calls ? double(m.BotTicks) / double(m.Calls) : 0.0,
            c.Online, c.ByClass[BotPerf::CLS_BOT_FROZEN], c.Online - c.ByClass[BotPerf::CLS_BOT_FROZEN]);
    }

    // cost table: every slot x class that was hit
    for (uint32 s = 0; s < BotPerf::SLOT_MAX; ++s)
        for (uint32 c = 0; c < BotPerf::CLS_MAX; ++c)
        {
            BotPerf::Cell const& cell = w.Cells[s][c];
            if (!cell.Calls)
                continue;
            TC_LOG_INFO(PERF_LOG, "cost slot={} cls={} calls={} total_ms={:.1f} avg_us={:.1f} call_max_us={:.0f} tick_p50_us={} tick_p95_us={} tick_max_us={} bytes={} pct_of_tick={:.1f}",
                BotPerf::SlotName(uint8(s)), BotPerf::ClassName(uint8(c)), cell.Calls, Ms(cell.Ns), double(cell.Ns) / 1e3 / double(cell.Calls),
                double(cell.MaxNs) / 1e3, BotPerf::HistPercentileUs(cell.Hist, 0.5), BotPerf::HistPercentileUs(cell.Hist, 0.95), BotPerf::HistMaxUs(cell.Hist),
                cell.Bytes, w.Cells[BotPerf::W_TOTAL][0].Ns ? 100.0 * double(cell.Ns) / double(w.Cells[BotPerf::W_TOTAL][0].Ns) : 0.0);
        }

    // login phases of bot logins (nested: lf.* run inside login.load_from_db); heap is net bytes kept by the phase
    std::sort(w.Phases.begin(), w.Phases.end(), [](BotPerf::PhaseStat const& a, BotPerf::PhaseStat const& b) { return a.Name < b.Name; });
    for (BotPerf::PhaseStat const& p : w.Phases)
        TC_LOG_INFO(PERF_LOG, "loginphase name={} calls={} heap_kb_per_call={:.0f} ms_per_call={:.2f} heap_total_mb={:.1f}", p.Name, p.Calls,
            double(p.Bytes) / 1024.0 / double(std::max<uint64>(p.Calls, 1)), Ms(p.Ns) / double(std::max<uint64>(p.Calls, 1)), double(p.Bytes) / 1048576.0);

    // who created grids (call stacks as offsets: addr2line -Cfe worldserver <offset>), most frequent first
    std::sort(w.GridStacks.begin(), w.GridStacks.end(), [](BotPerf::StackStat const& a, BotPerf::StackStat const& b) { return a.Count > b.Count; });
    for (uint32 i = 0; i < w.GridStacks.size() && i < 8; ++i)
    {
        BotPerf::StackStat const& s = w.GridStacks[i];
        std::string frames;
        for (uint64 f : s.Frames)
            frames += Trinity::StringFormat("{:x},", f);
        TC_LOG_INFO(PERF_LOG, "gridstack hash={:x} count={} cls={} frames={}", s.Hash, s.Count, BotPerf::ClassName(s.Cls), frames);
    }

    // active non-player objects that cost the most inside VisitNearbyCellsOf
    std::sort(w.Active.begin(), w.Active.end(), [](BotPerf::ActiveStat const& a, BotPerf::ActiveStat const& b) { return a.Ns > b.Ns; });
    for (uint32 i = 0; i < w.Active.size() && i < 14; ++i)
    {
        BotPerf::ActiveStat const& a = w.Active[i];
        std::string name = "?";
        if (a.TypeId == TYPEID_UNIT)
        {
            if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(a.Entry))
                name = t->Name;
        }
        else if (a.TypeId == TYPEID_GAMEOBJECT)
        {
            if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(a.Entry))
                name = t->name;
        }
        TC_LOG_INFO(PERF_LOG, "activeobj type={} entry={} name=\"{}\" visits={} total_ms={:.1f} avg_us={:.0f}", a.TypeId, a.Entry, name, a.Visits, Ms(a.Ns),
            a.Visits ? double(a.Ns) / 1e3 / double(a.Visits) : 0.0);
    }

    // one line per phase call of a bot login (Playerbots.PerfMon.LoginDetail): the grid counter lets a reader tell which logins created grids
    static std::unordered_map<std::string, bool> const keepPhases = { { "login.new_player", true }, { "lf.map_select", true }, { "lf.after_map_select", true },
        { "am.ensure_grid", true }, { "login.add_to_map", true }, { "login.load_from_db", true }, { "lf.init_stats", true }, { "lf.petstable", true } };
    for (BotPerf::PhaseSample const& s : w.PhaseSamples)
        if (s.Name && keepPhases.contains(s.Name))
            TC_LOG_INFO(PERF_LOG, "phs n={} kb={} ms={:.2f} gc={}", s.Name, s.Bytes / 1024, Ms(s.Ns), s.GridCreates);

    // packets handed to bot sessions, biggest first
    std::sort(w.Opcodes.begin(), w.Opcodes.end(), [](BotPerf::OpcodeStat const& a, BotPerf::OpcodeStat const& b) { return a.Bytes > b.Bytes; });
    shown = 0;
    for (BotPerf::OpcodeStat const& o : w.Opcodes)
    {
        if (++shown > 14)
            break;
        TC_LOG_INFO(PERF_LOG, "opcode name={} cls={} count={} bytes={} count_per_s={:.1f}",
            GetOpcodeNameForLogging(static_cast<OpcodeServer>(o.Opcode)), BotPerf::ClassName(o.Cls), o.Count, o.Bytes, double(o.Count) / std::max(wallS, 0.001));
    }

    if (w.Slowest.TotalNs)
    {
        TC_LOG_INFO(PERF_LOG, "slowtick total_ms={:.1f} diff_ms={}", Ms(w.Slowest.TotalNs), w.Slowest.DiffMs);
        struct Row { uint32 S, C; uint64 Ns; };
        std::vector<Row> rows;
        for (uint32 s = 0; s < BotPerf::SLOT_MAX; ++s)
            for (uint32 c = 0; c < BotPerf::CLS_MAX; ++c)
                if (w.Slowest.Ns[s][c])
                    rows.push_back({ s, c, w.Slowest.Ns[s][c] });
        std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.Ns > b.Ns; });
        for (uint32 i = 0; i < rows.size() && i < 10; ++i)
            TC_LOG_INFO(PERF_LOG, "slowtick_slot slot={} cls={} ms={:.1f}", BotPerf::SlotName(uint8(rows[i].S)), BotPerf::ClassName(uint8(rows[i].C)), Ms(rows[i].Ns));
        uint32 topN = std::max<uint32>(1, sPlayerbotsConfig.PerfMonTopBots);
        for (uint32 i = 0; i < w.Slowest.Top.size() && i < topN; ++i)
        {
            BotPerf::TopPlayer const& p = w.Slowest.Top[i];
            TC_LOG_INFO(PERF_LOG, "slowtick_player guid={} cls={} map={} x={:.0f} y={:.0f} ms={:.2f}", p.Guid, BotPerf::ClassName(p.Cls), p.MapId, p.X, p.Y, Ms(p.Ns));
        }
    }

    if (_chatActive || _chatSentThisWindow)
    {
        TC_LOG_INFO(PERF_LOG, "chat kind={} rate={:.1f} sent_window={} sent_total={} no_target={}", _chatKind, _chatRate, _chatSentThisWindow, _chatSent, _chatNoTarget);
        _chatSentThisWindow = 0;
    }
}

void BotPerfReporter::LogMemory()
{
    BotPerf::HeapStats h = BotPerf::GetHeapStats();
    uint32 loaded = 0;
    sMapMgr->DoForAllMaps([&loaded](Map* map)
    {
        for (uint32 id = 0; id < 64 * 64; ++id)
            if (map->IsGridLoaded(id))
                ++loaded;
    });
    TC_LOG_INFO(PERF_LOG, "heap rss_mb={} jemalloc_ok={} allocated_mb={:.0f} active_mb={:.0f} resident_mb={:.0f} mapped_mb={:.0f} retained_mb={:.0f} bots={} loaded_grids={}",
        ReadRssMB(), h.Ok, double(h.Allocated) / 1048576.0, double(h.Active) / 1048576.0, double(h.Resident) / 1048576.0,
        double(h.Mapped) / 1048576.0, double(h.Retained) / 1048576.0, sPlayerbotMgr->GetRandomBotCount(), loaded);
}

void BotPerfReporter::LogGridCensus()
{
    constexpr uint32 N = 64;
    sMapMgr->DoForAllMaps([](Map* map)
    {
        if (map->GetPlayers().empty())
            return;

        // 0 = nobody near, 1 = only frozen bots, 2 = a simulated bot, 3 = a human (3x3 grids around each player: the activation range is far below one grid)
        std::array<uint8, N * N> held = { };
        uint32 humans = 0, simBots = 0, frozenBots = 0;
        for (MapReference const& ref : map->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || !p->IsInWorld())
                continue;
            uint8 level;
            if (!p->GetSession()->IsBot())
                level = 3, ++humans;
            else if (p->GetSession()->IsBotFrozen())
                level = 1, ++frozenBots;
            else
                level = 2, ++simBots;
            GridCoord g = Trinity::ComputeGridCoord(p->GetPositionX(), p->GetPositionY());
            for (int32 dy = -1; dy <= 1; ++dy)
                for (int32 dx = -1; dx <= 1; ++dx)
                {
                    int32 gx = int32(g.x_coord) + dx, gy = int32(g.y_coord) + dy;
                    if (gx < 0 || gy < 0 || gx >= int32(N) || gy >= int32(N))
                        continue;
                    held[gy * N + gx] = std::max<uint8>(held[gy * N + gx], level);
                }
        }

        uint32 loaded = 0, removal = 0, heldHuman = 0, heldSim = 0, heldFrozen = 0, unheld = 0, inverseMismatch = 0;
        for (uint32 gy = 0; gy < N; ++gy)
            for (uint32 gx = 0; gx < N; ++gx)
            {
                if (!map->IsGridLoaded(gy * N + gx))
                    continue;
                ++loaded;
                // centre of the grid (ComputeGridCoord is floor((x - offset) / size + CENTER + 0.5)); a grid that does not map
                // back is counted in inverse_mismatch instead of being read as 'not in removal'
                float x = CENTER_GRID_OFFSET + (float(gx) - float(CENTER_GRID_ID)) * SIZE_OF_GRIDS;
                float y = CENTER_GRID_OFFSET + (float(gy) - float(CENTER_GRID_ID)) * SIZE_OF_GRIDS;
                GridCoord back = Trinity::ComputeGridCoord(x, y);
                if (back.x_coord != gx || back.y_coord != gy)
                    ++inverseMismatch;
                else if (map->IsRemovalGrid(x, y))
                    ++removal;
                switch (held[gy * N + gx])
                {
                    case 3: ++heldHuman; break;
                    case 2: ++heldSim; break;
                    case 1: ++heldFrozen; break;
                    default: ++unheld; break;
                }
            }
        TC_LOG_INFO(PERF_LOG, "grids map={} inst={} loaded={} in_removal={} held_by_human={} held_by_sim_bot={} held_by_frozen_bots_only={} unheld={} humans={} sim_bots={} frozen_bots={} inverse_mismatch={}",
            map->GetId(), map->GetInstanceId(), loaded, removal, heldHuman, heldSim, heldFrozen, unheld, humans, simBots, frozenBots, inverseMismatch);
    });
}

void BotPerfReporter::LogMemCensus()
{
    struct Acc { uint32 N = 0; uint64 Items = 0, Spells = 0, Auras = 0, Rewarded = 0, Mails = 0, Channels = 0, NoChannel = 0;
                 uint64 MaxItems = 0, MaxSpells = 0; uint64 Mounts = 0, Toys = 0, Heirlooms = 0; };
    std::array<Acc, BotPerf::CLS_MAX> acc;
    std::unordered_map<std::string, uint32> channelSizes;
    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI*)
    {
        Acc& a = acc[bot->GetSession()->GetBotPerfClass()];
        ++a.N;
        uint64 items = 0;
        for (uint8 slot = 0; slot < PLAYER_SLOT_END; ++slot)
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++items;
        a.Items += items;
        a.MaxItems = std::max(a.MaxItems, items);
        uint64 spells = bot->GetSpellMap().size();
        a.Spells += spells;
        a.MaxSpells = std::max(a.MaxSpells, spells);
        a.Auras += bot->GetOwnedAuras().size();
        a.Rewarded += bot->GetRewardedQuestCount();
        a.Mails += bot->GetMailSize();
        uint32 channels = uint32(bot->GetJoinedChannels().size());
        a.Channels += channels;
        if (!channels)
            ++a.NoChannel;
        for (Channel* ch : bot->GetJoinedChannels())
            channelSizes[ch->GetName()] = ch->GetNumPlayers();
        if (CollectionMgr* cm = bot->GetSession()->GetCollectionMgr())
        {
            a.Mounts += cm->GetAccountMounts().size();
            a.Toys += cm->GetAccountToys().size();
            a.Heirlooms += cm->GetAccountHeirlooms().size();
        }
    });

    for (uint32 c = 0; c < BotPerf::CLS_MAX; ++c)
    {
        Acc const& a = acc[c];
        if (!a.N)
            continue;
        double n = double(a.N);
        TC_LOG_INFO(PERF_LOG, "memcensus cls={} bots={} avg_items={:.1f} max_items={} avg_spells={:.0f} max_spells={} avg_auras={:.1f} avg_rewarded_quests={:.0f} avg_mails={:.1f} avg_account_mounts={:.0f} avg_account_toys={:.0f} avg_account_heirlooms={:.0f} avg_channels={:.1f} bots_without_channel={}",
            BotPerf::ClassName(uint8(c)), a.N, double(a.Items) / n, a.MaxItems, double(a.Spells) / n, a.MaxSpells, double(a.Auras) / n, double(a.Rewarded) / n,
            double(a.Mails) / n, double(a.Mounts) / n, double(a.Toys) / n, double(a.Heirlooms) / n, double(a.Channels) / n, a.NoChannel);
    }
    // 12 biggest channels (the channel's own member count: humans and bots)
    std::vector<std::pair<std::string, uint32>> sizes(channelSizes.begin(), channelSizes.end());
    std::sort(sizes.begin(), sizes.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    for (uint32 i = 0; i < sizes.size() && i < 12; ++i)
        TC_LOG_INFO(PERF_LOG, "channel name=\"{}\" members={}", sizes[i].first, sizes[i].second);
}

// A human arriving, leaving or jumping far wakes (or lets go to sleep) many bots; the cost of that is logged for 10 s afterwards
void BotPerfReporter::TrackHumans(uint32 diff)
{
    for (HumanEvent& e : _events)
    {
        e.ElapsedMs += diff;
        e.PeakDiff = std::max(e.PeakDiff, diff);
        e.SumDiff += diff;
        ++e.Ticks;
    }
    for (auto it = _events.begin(); it != _events.end();)
    {
        if (it->ElapsedMs < 10000)
        {
            ++it;
            continue;
        }
        TC_LOG_INFO(PERF_LOG, "human_event kind={} guid={} map={} thawed={} refrozen={} sim_before={} sim_after={} peak_diff_ms={} avg_diff_ms={:.1f} ticks={}",
            it->Kind, it->Guid, it->MapId, _thaws.load(std::memory_order_relaxed) - it->Thaws0, _freezes.load(std::memory_order_relaxed) - it->Freezes0,
            it->SimBefore, CountSimulated(), it->PeakDiff, it->Ticks ? double(it->SumDiff) / double(it->Ticks) : 0.0, it->Ticks);
        it = _events.erase(it);
    }

    auto snapshot = sPlayerbotMgr->GetNearbyHumans();
    if (!snapshot || snapshot.get() == _lastHumansHolder.get())
        return;

    std::vector<HumanPos> now;
    now.reserve(snapshot->size());
    for (PlayerbotMgr::NearbyHuman const& h : *snapshot)
        now.push_back({ h.Guid.GetCounter(), h.MapId, h.X, h.Y });

    auto start = [this](char const* kind, HumanPos const& h)
    {
        if (_events.size() >= 8)
            return;
        HumanEvent e;
        e.Kind = kind;
        e.Guid = h.Guid;
        e.MapId = h.MapId;
        e.Thaws0 = _thaws.load(std::memory_order_relaxed);
        e.Freezes0 = _freezes.load(std::memory_order_relaxed);
        e.SimBefore = CountSimulated();
        _events.push_back(e);
    };

    if (_haveHumans)
    {
        for (HumanPos const& h : now)
        {
            auto old = std::find_if(_lastHumans.begin(), _lastHumans.end(), [&h](HumanPos const& o) { return o.Guid == h.Guid; });
            if (old == _lastHumans.end())
                start("arrive", h);
            else if (old->MapId != h.MapId || (h.X - old->X) * (h.X - old->X) + (h.Y - old->Y) * (h.Y - old->Y) > 200.f * 200.f)
                start("jump", h);
        }
        for (HumanPos const& o : _lastHumans)
            if (std::none_of(now.begin(), now.end(), [&o](HumanPos const& h) { return h.Guid == o.Guid; }))
                start("leave", o);
    }
    _haveHumans = true;
    _lastHumans = std::move(now);
    _lastHumansHolder = snapshot;
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
    BotPerf::Scope scope(BotPerf::M_CHAT_DRIVER);
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
            ++_chatSentThisWindow;
        }
        else
            ++_chatNoTarget;
    }
}
}
