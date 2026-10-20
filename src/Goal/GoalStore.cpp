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

#include "GoalStore.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Db/PlayerbotRepository.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Strategies/Rpg/RpgInfo.h"
#include "StringFormat.h"
#include "Travel/HubIndex.h"
#include <algorithm>
#include <ctime>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace Playerbots::GoalStore
{
namespace
{
enum class Kind : uint8
{
    Town, QuestHub, Grind, Gather, Fish, Explore, Bg, Event, Afk, Logoff, Count
};

constexpr char const* PersonaName[] = { "quester", "crafter", "pvper", "socialite", "explorer", "idler" };
constexpr uint32 PersonaCount = 6;

constexpr char const* KindName[] =
{
    "town", "quest_hub", "grind", "gather", "fish", "explore", "bg", "event", "afk", "logoff"
};

struct Row
{
    uint32 Id = 0;
    std::string Name;
    Kind Kind = Kind::Town;
    uint32 Weight = 0;
    uint8 MinLevel = 1;
    uint8 MaxLevel = 255;
    uint8 Faction = 0;
    uint32 ClassMask = 0;
    uint64 RaceMask = 0;
    int32 Map = 0;
    uint32 Zone = 0;
    uint32 Profession = 0;
    int32 GameEvent = 0;
    uint8 HourFrom = 0;
    uint8 HourTo = 0;
    uint8 WeekdayMask = 0;
    uint32 DurationMin = 5;
    uint32 DurationMax = 15;
    uint32 CooldownSec = 0;
    uint32 MaxPerZone = 0;
    int8 ForceKind = -1;
    RpgStatus Status = RpgStatus::Rest;
};

struct Snapshot
{
    std::vector<Row> Rows;
    float Multiplier[PersonaCount][uint8(Kind::Count)] = {};
};

struct Active
{
    uint32 Id = 0;
    std::string Name;
    std::string Kind;
    std::string Persona;
    uint32 Zone = 0;
    uint32 UntilMs = 0;
    uint32 CooldownSec = 0;
    bool Held = false;
    bool Far = false;
    bool AbstractGranted = false;
};

std::shared_mutex SnapLock;
std::shared_ptr<Snapshot const> Snap = std::make_shared<Snapshot>();

std::mutex StateLock;
std::unordered_map<ObjectGuid, Active> ActiveByGuid;
std::unordered_map<uint64, uint32> Occupancy;
std::unordered_map<uint64, std::vector<std::pair<uint32, uint32>>> CooldownUntil;
std::unordered_map<uint32, uint8> PersonaByGuid;
bool PersonasLoaded = false;

uint64 OccKey(uint32 goal, uint32 zone)
{
    return (uint64(goal) << 32) | zone;
}

uint64 CoolKey(ObjectGuid guid)
{
    return guid.GetCounter();
}

bool ParseKind(std::string const& text, Kind& out)
{
    for (uint8 i = 0; i < uint8(Kind::Count); ++i)
        if (text == KindName[i])
        {
            out = Kind(i);
            return true;
        }
    return false;
}

int8 ForceFor(std::string const& status)
{
    if (status == "flight") return int8(HubKind::FlightMaster);
    if (status == "banker") return int8(HubKind::Banker);
    if (status == "auctioneer") return int8(HubKind::Auctioneer);
    if (status == "innkeeper") return int8(HubKind::Innkeeper);
    if (status == "vendor") return int8(HubKind::Vendor);
    if (status == "repair") return int8(HubKind::Repair);
    if (status == "mailbox") return int8(HubKind::Mailbox);
    return -1;
}

RpgStatus StatusFor(Kind kind, int8 force)
{
    switch (kind)
    {
        case Kind::Town: return force >= 0 ? RpgStatus::WanderNpc : RpgStatus::WanderRandom;
        case Kind::QuestHub: return RpgStatus::DoQuest;
        case Kind::Grind: return RpgStatus::GoGrind;
        case Kind::Gather: return RpgStatus::Gather;
        case Kind::Fish:
        case Kind::Explore: return RpgStatus::WanderRandom;
        default: return RpgStatus::Rest;
    }
}

bool HourMatches(uint8 from, uint8 to, int hour)
{
    if (from == to)
        return true;
    if (from < to)
        return hour >= int(from) && hour < int(to);
    return hour >= int(from) || hour < int(to);
}

bool RowMatches(Row const& row, Player const* bot, uint8 persona, uint32 nowSec, bool checkLevel = true)
{
    if (checkLevel && (bot->GetLevel() < row.MinLevel || bot->GetLevel() > row.MaxLevel))
        return false;

    if (row.Faction == 1 && Player::TeamForRace(bot->GetRace()) != ALLIANCE)
        return false;
    if (row.Faction == 2 && Player::TeamForRace(bot->GetRace()) != HORDE)
        return false;

    uint32 classId = bot->GetClass();
    if (row.ClassMask && (classId == 0 || classId > 32 || (row.ClassMask & (1u << (classId - 1))) == 0))
        return false;

    uint32 raceId = bot->GetRace();
    if (row.RaceMask && (raceId == 0 || raceId > 64 || (row.RaceMask & (uint64(1) << (raceId - 1))) == 0))
        return false;

    if (row.Map && int32(bot->GetMapId()) != row.Map)
        return false;
    // a quest hub names the zone the level belongs in, not the zone the bot is standing in
    if (row.Zone && bot->GetZoneId() != row.Zone && row.Kind != Kind::QuestHub)
        return false;
    if (row.Profession && !bot->HasSkill(row.Profession))
        return false;
    if (row.GameEvent > 0 && !sGameEventMgr->IsActiveEvent(uint16(row.GameEvent)))
        return false;

    std::time_t when = std::time_t(nowSec);
    std::tm local = {};
    localtime_r(&when, &local);
    if (!HourMatches(row.HourFrom, row.HourTo, local.tm_hour))
        return false;
    if (row.WeekdayMask && (row.WeekdayMask & (1u << local.tm_wday)) == 0)
        return false;

    auto cool = CooldownUntil.find(CoolKey(bot->GetGUID()));
    if (cool != CooldownUntil.end())
    {
        std::erase_if(cool->second, [nowSec](auto const& entry) { return entry.second <= nowSec; });
        for (auto const& [id, until] : cool->second)
            if (id == row.Id && until > nowSec)
                return false;
    }

    if (row.MaxPerZone)
    {
        auto occ = Occupancy.find(OccKey(row.Id, bot->GetZoneId()));
        if (occ != Occupancy.end() && occ->second >= row.MaxPerZone)
            return false;
    }

    return true;
}

void EndActive(ObjectGuid guid, Active& active, uint32 nowSec)
{
    if (active.Held)
    {
        uint64 key = OccKey(active.Id, active.Zone);
        auto occ = Occupancy.find(key);
        if (occ != Occupancy.end() && occ->second)
            --occ->second;
        active.Held = false;
    }
    if (active.Id && active.CooldownSec)
        CooldownUntil[CoolKey(guid)].emplace_back(active.Id, nowSec + active.CooldownSec);
}

uint8 PersonaOf(ObjectGuid guid)
{
    uint32 counter = uint32(guid.GetCounter());
    auto found = PersonaByGuid.find(counter);
    if (found != PersonaByGuid.end())
        return found->second;

    uint8 persona = uint8((guid.GetCounter() * 2654435761u) % PersonaCount);
    PersonaByGuid[counter] = persona;
    if (PersonasLoaded)
        CharacterDatabase.Execute(Trinity::StringFormat("INSERT IGNORE INTO playerbots_bot_persona (guid, persona) VALUES ({}, {})", counter, uint32(persona)).c_str());
    return persona;
}

void LoadPersonas()
{
    PersonaByGuid.clear();
    PersonasLoaded = false;
    if (QueryResult result = CharacterDatabase.Query("SELECT guid, persona FROM playerbots_bot_persona"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 persona = fields[1].GetUInt8();
            if (persona < PersonaCount)
                PersonaByGuid[fields[0].GetUInt32()] = persona;
        }
        while (result->NextRow());
        PersonasLoaded = true;
    }
}

std::shared_ptr<Snapshot> ReadTables()
{
    auto snap = std::make_shared<Snapshot>();
    for (uint32 p = 0; p < PersonaCount; ++p)
        for (uint8 k = 0; k < uint8(Kind::Count); ++k)
            snap->Multiplier[p][k] = 1.0f;

    QueryResult goals = WorldDatabase.Query(
        "SELECT id, name, kind, weight, min_level, max_level, faction, class_mask, race_mask, map, zone, profession, "
        "game_event, hour_from, hour_to, weekday_mask, duration_min, duration_max, cooldown_sec, max_per_zone, status "
        "FROM playerbots_goal");
    if (!goals)
        return snap;

    do
    {
        Field* fields = goals->Fetch();
        Kind kind;
        if (!ParseKind(fields[2].GetString(), kind))
            continue;

        Row row;
        row.Id = fields[0].GetUInt32();
        row.Name = fields[1].GetString();
        row.Kind = kind;
        row.Weight = fields[3].GetUInt32();
        row.MinLevel = fields[4].GetUInt8();
        row.MaxLevel = fields[5].GetUInt8();
        row.Faction = fields[6].GetUInt8();
        row.ClassMask = fields[7].GetUInt32();
        row.RaceMask = fields[8].GetUInt64();
        row.Map = fields[9].GetInt32();
        row.Zone = fields[10].GetUInt32();
        row.Profession = fields[11].GetUInt32();
        row.GameEvent = fields[12].GetInt32();
        row.HourFrom = fields[13].GetUInt8();
        row.HourTo = fields[14].GetUInt8();
        row.WeekdayMask = fields[15].GetUInt8();
        row.DurationMin = std::max<uint32>(1, fields[16].GetUInt32());
        row.DurationMax = std::max(row.DurationMin, fields[17].GetUInt32());
        row.CooldownSec = fields[18].GetUInt32();
        row.MaxPerZone = fields[19].GetUInt32();
        row.ForceKind = ForceFor(fields[20].GetString());
        row.Status = StatusFor(kind, row.ForceKind);
        if (row.Weight)
            snap->Rows.push_back(std::move(row));
    }
    while (goals->NextRow());

    if (QueryResult personas = WorldDatabase.Query("SELECT persona, kind, multiplier FROM playerbots_persona"))
    {
        do
        {
            Field* fields = personas->Fetch();
            std::string persona = fields[0].GetString();
            Kind kind;
            if (!ParseKind(fields[1].GetString(), kind))
                continue;
            for (uint8 i = 0; i < PersonaCount; ++i)
                if (persona == PersonaName[i])
                    snap->Multiplier[i][uint8(kind)] = fields[2].GetFloat();
        }
        while (personas->NextRow());
    }
    return snap;
}

struct Choice
{
    Row const* Goal = nullptr;
    uint32 Weight = 0;
};

/// "town,event,bg" -> bit mask over Kind. Unknown names are ignored
uint32 KindMask(std::string const& list)
{
    uint32 mask = 0;
    size_t start = 0;
    while (start <= list.size())
    {
        size_t comma = list.find(',', start);
        std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        for (uint8 i = 0; i < uint8(Kind::Count); ++i)
            if (name == KindName[i])
                mask |= 1u << i;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return mask;
}

// A row that only applies at certain times: during a game event, in an hour window, on certain weekdays
bool IsTimed(Row const& row)
{
    return row.GameEvent > 0 || row.HourFrom != row.HourTo || row.WeekdayMask;
}

bool Choose(std::shared_ptr<Snapshot const> const& snap, Player* bot, uint8 persona, uint32 nowSec, Row& out, uint32 kindMask = ~0u, bool timedOnly = false)
{
    if (!snap || snap->Rows.empty() || persona >= PersonaCount)
        return false;

    // Caller holds StateLock. The snapshot is a copy, so this does not take SnapLock.
    std::vector<Choice> choices;
    uint32 sum = 0;
    for (Row const& row : snap->Rows)
    {
        if (!(kindMask & (1u << uint8(row.Kind))) || (timedOnly && !IsTimed(row)) || !RowMatches(row, bot, persona, nowSec))
            continue;
        float mult = snap->Multiplier[persona][uint8(row.Kind)];
        if (mult <= 0.0f)
            mult = 1.0f;
        uint32 weight = uint32(float(row.Weight) * mult);
        if (row.Kind == Kind::QuestHub && row.Zone && row.Zone == bot->GetZoneId())
            weight *= 2;
        if (!weight)
            continue;
        choices.push_back({ &row, weight });
        sum += weight;
    }

    auto ladder = [](Choice const& choice) { return choice.Goal->Kind == Kind::QuestHub && choice.Goal->Zone; };
    bool onLadder = std::ranges::any_of(choices, ladder);
    if (!onLadder)
    {
        for (Row const& row : snap->Rows)
        {
            if (row.Kind != Kind::QuestHub || !row.Zone || !(kindMask & (1u << uint8(Kind::QuestHub))) || (timedOnly && !IsTimed(row)))
                continue;
            if (bot->GetLevel() <= row.MaxLevel || bot->GetLevel() > uint32(row.MaxLevel) + 10)
                continue;
            if (!RowMatches(row, bot, persona, nowSec, false))
                continue;
            float mult = snap->Multiplier[persona][uint8(row.Kind)];
            if (mult <= 0.0f)
                mult = 1.0f;
            uint32 weight = uint32(float(row.Weight) * mult);
            if (!weight)
                continue;
            choices.push_back({ &row, weight });
            sum += weight;
            onLadder = true;
        }
    }
    if (onLadder)
    {
        std::erase_if(choices, [](Choice const& choice) { return choice.Goal->Kind == Kind::QuestHub && !choice.Goal->Zone; });
        sum = 0;
        for (Choice const& choice : choices)
            sum += choice.Weight;
    }

    if (!sum)
        return false;

    uint32 roll = urand(1, sum);
    uint32 acc = 0;
    for (Choice const& choice : choices)
    {
        acc += choice.Weight;
        if (acc >= roll)
        {
            out = *choice.Goal;
            return true;
        }
    }
    return false;
}
}

uint32 Load()
{
    std::shared_ptr<Snapshot> snap = ReadTables();
    {
        std::unique_lock lock(SnapLock);
        Snap = snap;
    }
    {
        std::lock_guard lock(StateLock);
        LoadPersonas();
    }
    for (Row const& row : snap->Rows)
    {
        if (row.GameEvent <= 0)
            continue;
        if (sGameEventMgr->IsActiveEvent(uint16(row.GameEvent)))
            TC_LOG_INFO("server.playerbots", "Playerbots: goal {} matches active game event {}", row.Name, row.GameEvent);
        else
            TC_LOG_INFO("server.playerbots", "Playerbots: goal {} does not match, game event {} is not active", row.Name, row.GameEvent);
    }
    TC_LOG_INFO("server.playerbots", "Playerbots: goal table loaded, {} rows", snap->Rows.size());
    return uint32(snap->Rows.size());
}

uint32 Reload()
{
    std::shared_ptr<Snapshot> snap = ReadTables();
    uint32 count = uint32(snap->Rows.size());
    {
        std::unique_lock lock(SnapLock);
        Snap = snap;
    }
    for (Row const& row : snap->Rows)
    {
        if (row.GameEvent <= 0)
            continue;
        if (sGameEventMgr->IsActiveEvent(uint16(row.GameEvent)))
            TC_LOG_INFO("server.playerbots", "Playerbots: goal {} matches active game event {}", row.Name, row.GameEvent);
        else
            TC_LOG_INFO("server.playerbots", "Playerbots: goal {} does not match, game event {} is not active", row.Name, row.GameEvent);
    }
    TC_LOG_INFO("server.playerbots", "Playerbots: goal table reloaded, {} rows", count);
    return count;
}

bool TryBegin(PlayerbotAI& ai, uint32 nowMs)
{
    if (!sPlayerbotsConfig.GoalEnable)
        return false;

    Player* bot = ai.GetBot();
    if (!bot)
        return false;

    std::shared_ptr<Snapshot const> snap;
    {
        std::shared_lock lock(SnapLock);
        snap = Snap;
    }

    uint32 nowSec = uint32(GameTime::GetGameTime());
    std::lock_guard lock(StateLock);
    uint8 persona = PersonaOf(bot->GetGUID());

    Active& active = ActiveByGuid[bot->GetGUID()];
    if (active.Id)
        EndActive(bot->GetGUID(), active, nowSec);

    Row chosen;
    // a bot near a player gets only the kinds Playerbots.Goal.NearKinds names; the status loop does the rest
    if (!Choose(snap, bot, persona, nowSec, chosen, KindMask(sPlayerbotsConfig.GoalNearKinds)))
    {
        active = {};
        return false;
    }

    uint32 minutes = urand(chosen.DurationMin, chosen.DurationMax);
    active.Id = chosen.Id;
    active.Name = chosen.Name;
    active.Kind = KindName[uint8(chosen.Kind)];
    active.Persona = PersonaName[persona];
    active.Zone = bot->GetZoneId();
    active.UntilMs = nowMs + minutes * 60 * 1000;
    active.CooldownSec = chosen.CooldownSec;
    active.Held = chosen.MaxPerZone != 0;
    active.AbstractGranted = false;
    active.Far = false;
    if (active.Held)
        ++Occupancy[OccKey(chosen.Id, active.Zone)];

    RpgInfo& rpg = ai.GetRpg();
    rpg.Begin(chosen.Status, nowMs);
    rpg.ForceKind = chosen.ForceKind;
    rpg.DurationMs = minutes * 60 * 1000;
    sPlayerbotRepository->Remember(bot->GetGUID(), "goal", active.Name, active.Kind);
    return true;
}

bool ReadyForAbstractXp(ObjectGuid guid, uint32 nowMs)
{
    if (!sPlayerbotsConfig.QuestAbstractXp)
        return false;

    std::lock_guard lock(StateLock);
    auto itr = ActiveByGuid.find(guid);
    if (itr == ActiveByGuid.end())
        return false;

    Active& active = itr->second;
    if (active.AbstractGranted || active.Kind != "quest_hub" || !active.Id || active.UntilMs > nowMs)
        return false;

    active.AbstractGranted = true;
    return true;
}

uint32 OnFrozen(PlayerbotAI& ai, uint32 nowMs, bool timedOnly)
{
    if (!sPlayerbotsConfig.GoalEnable)
        return nowMs + 60000;

    Player* bot = ai.GetBot();
    if (!bot || !ai.GetMasterGuid().IsEmpty())
        return nowMs + 60000;

    std::shared_ptr<Snapshot const> snap;
    {
        std::shared_lock lock(SnapLock);
        snap = Snap;
    }

    uint32 nowSec = uint32(GameTime::GetGameTime());
    std::lock_guard lock(StateLock);
    Active& active = ActiveByGuid[bot->GetGUID()];
    if (active.Held)
    {
        uint64 key = OccKey(active.Id, active.Zone);
        auto occ = Occupancy.find(key);
        if (occ != Occupancy.end() && occ->second)
            --occ->second;
        active.Held = false;
    }

    if (active.Id && active.UntilMs > nowMs)
    {
        active.Far = true;
        return active.UntilMs;
    }

    uint8 persona = PersonaOf(bot->GetGUID());
    if (active.Id)
        EndActive(bot->GetGUID(), active, nowSec);

    Row chosen;
    if (!Choose(snap, bot, persona, nowSec, chosen, ~0u, timedOnly))
    {
        active = {};
        active.Persona = PersonaName[persona];
        return nowMs + 60000;
    }

    bool changed = active.Id != chosen.Id;
    uint32 minutes = urand(chosen.DurationMin, chosen.DurationMax);
    active.Id = chosen.Id;
    active.Name = chosen.Name;
    active.Kind = KindName[uint8(chosen.Kind)];
    active.Persona = PersonaName[persona];
    active.Zone = bot->GetZoneId();
    active.UntilMs = nowMs + minutes * 60 * 1000;
    active.CooldownSec = chosen.CooldownSec;
    active.Held = false;
    active.Far = true;
    active.AbstractGranted = false;
    if (changed)
    {
        TC_LOG_INFO("server.playerbots", "Playerbots: frozen {} takes goal {} ({})", bot->GetName(), active.Name, active.Kind);
        sPlayerbotRepository->Remember(bot->GetGUID(), "goal", active.Name, active.Kind);
    }
    return active.UntilMs;
}

bool HasGoal(ObjectGuid guid, uint32 nowMs)
{
    std::lock_guard lock(StateLock);
    auto itr = ActiveByGuid.find(guid);
    return itr != ActiveByGuid.end() && itr->second.Id && itr->second.UntilMs > nowMs;
}

void Forget(ObjectGuid guid)
{
    std::lock_guard lock(StateLock);
    auto itr = ActiveByGuid.find(guid);
    if (itr == ActiveByGuid.end())
        return;
    EndActive(guid, itr->second, uint32(GameTime::GetGameTime()));
    ActiveByGuid.erase(itr);
}

Inspect Describe(ObjectGuid guid)
{
    std::lock_guard lock(StateLock);
    Inspect out;
    auto itr = ActiveByGuid.find(guid);
    if (itr == ActiveByGuid.end())
    {
        auto persona = PersonaByGuid.find(uint32(guid.GetCounter()));
        if (persona != PersonaByGuid.end() && persona->second < PersonaCount)
        {
            out.Known = true;
            out.Persona = PersonaName[persona->second];
        }
        return out;
    }
    out.Known = true;
    out.Persona = itr->second.Persona;
    out.Goal = itr->second.Name;
    out.Kind = itr->second.Kind;
    out.Far = itr->second.Far;
    return out;
}
}
