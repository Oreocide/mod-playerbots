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

#include "PlayerbotsConfig.h"
#include "Config.h"
#include "StringFormat.h"
#include "StringConvert.h"
#include "Util.h"
#include "World.h"
#include <algorithm>

namespace Playerbots
{
void Config::Load()
{
    Enabled = sConfigMgr->GetBoolDefault("Playerbots.Enable", true);
    MaxBotsPerPlayer = sConfigMgr->GetIntDefault("Playerbots.MaxBotsPerPlayer", 4);
    UpdateInterval = std::max(50, sConfigMgr->GetIntDefault("Playerbots.UpdateInterval", 250));
    ScanInterval = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.ScanInterval", 1500), 0, 30000));
    PerfMonEnabled = sConfigMgr->GetBoolDefault("Playerbots.PerfMon.Enable", false);
    PerfMonReportSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.PerfMon.ReportSeconds", 60), 5, 3600));
    PerfMonSlowTickMs = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.PerfMon.SlowTickMs", 150), 1, 60000));
    PerfMonTopBots = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.PerfMon.TopBots", 10), 1, 16));
    PerfMonOpcodes = sConfigMgr->GetBoolDefault("Playerbots.PerfMon.Opcodes", true);
    PerfMonGridCensusSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.PerfMon.GridCensusSeconds", 60), 0, 3600));
    PerfMonMemorySeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.PerfMon.MemorySeconds", 30), 0, 3600));
    PerfMonMemCensusSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.PerfMon.MemCensusSeconds", 300), 0, 86400));
    RandomJoinChannels = sConfigMgr->GetBoolDefault("Playerbots.Random.JoinChannels", false);
    PerfMonLoginDetail = sConfigMgr->GetBoolDefault("Playerbots.PerfMon.LoginDetail", false);
    PerfMonChatDriver = sConfigMgr->GetStringDefault("Playerbots.PerfMon.ChatDriver", "");
    WarmMaps = sConfigMgr->GetStringDefault("Playerbots.Warm.Maps", "");
    ManagerSliceMs = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.ManagerSliceMs", 0), 0, 2000));
    SimVerdictCacheMs = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.SimVerdictCacheMs", 0), 0, 2000));
    SleepIdleActiveObjects = sConfigMgr->GetBoolDefault("Playerbots.SleepIdleActiveObjects", false);
    SkipBotVisibilityGridLoad = sConfigMgr->GetBoolDefault("Playerbots.SkipBotVisibilityGridLoad", false);
    WarmHubs = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Warm.Hubs", 0), 0, 100));
    WarmPlayerPositions = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Warm.PlayerPositions", 0), 0, 50));
    WarmPin = sConfigMgr->GetBoolDefault("Playerbots.Warm.Pin", true);
    WarmRadius = std::clamp(sConfigMgr->GetFloatDefault("Playerbots.Warm.Radius", 120.0f), 30.0f, 400.0f);
    RandomCreateUpTo = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.CreateUpTo", 0), 0, 10000));
    FarBotHeightNoGridLoad = sConfigMgr->GetBoolDefault("Playerbots.FarBotHeightNoGridLoad", false);
    LazyMMap = sConfigMgr->GetBoolDefault("Playerbots.LazyMMap", false);
    FrozenTickMs = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.FrozenTickMs", 0), 0, 5000));
    RandomMaxRssMB = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Random.MaxRssMB", 0)));
    LoginTimeout = std::max(5000, sConfigMgr->GetIntDefault("Playerbots.LoginTimeout", 60000));
    FollowDistance = sConfigMgr->GetFloatDefault("Playerbots.FollowDistance", 2.5f);
    TeleportDistance = sConfigMgr->GetFloatDefault("Playerbots.TeleportDistance", 100.0f);
    RangedDistance = sConfigMgr->GetFloatDefault("Playerbots.RangedDistance", 25.0f);
    HealThreshold = sConfigMgr->GetFloatDefault("Playerbots.HealThreshold", 85.0f);
    AutoResurrect = sConfigMgr->GetBoolDefault("Playerbots.AutoResurrect", true);
    CommandPrefix = sConfigMgr->GetStringDefault("Playerbots.Command.Prefix", "");
    CommandSeparator = sConfigMgr->GetStringDefault("Playerbots.Command.Separator", ";");
    CommandNearbyDistance = std::max(1.0f, sConfigMgr->GetFloatDefault("Playerbots.Command.NearbyDistance", 30.0f));

    RandomEnabled = sConfigMgr->GetBoolDefault("Playerbots.Random.Enable", false);
    RandomCount = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.Count", 10), 0, 5000));
    int32 maxPlayerLevel = 90;
    if (int32 configured = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL); configured >= 1)
        maxPlayerLevel = configured;
    RandomMinLevel = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.MinLevel", 1), 1, maxPlayerLevel));
    RandomMaxLevel = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.MaxLevel", 10), int32(RandomMinLevel), maxPlayerLevel));
    RandomLoginDelay = uint32(std::max(100, sConfigMgr->GetIntDefault("Playerbots.Random.LoginDelay", 2000)));
    RandomCreateDelay = uint32(std::max(500, sConfigMgr->GetIntDefault("Playerbots.Random.CreateDelay", 3000)));
    RandomCharactersPerAccount = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.CharactersPerAccount", 40), 1, 50));
    RandomAllianceRatio = uint32(std::max(1, sConfigMgr->GetIntDefault("Playerbots.Random.AllianceRatio", 1)));
    RandomHordeRatio = uint32(std::max(1, sConfigMgr->GetIntDefault("Playerbots.Random.HordeRatio", 1)));
    RandomLoginBatch = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.LoginBatch", 5), 1, 50));
    RandomWanderRadius = sConfigMgr->GetFloatDefault("Playerbots.Random.WanderRadius", 30.0f);
    RandomEmailPrefix = sConfigMgr->GetStringDefault("Playerbots.Random.EmailPrefix", "RNDBOT");
    RandomEmailDomain = sConfigMgr->GetStringDefault("Playerbots.Random.EmailDomain", "PLAYERBOTS.LOCAL");
    RandomGameAccountsPerEmail = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.GameAccountsPerEmail", 10), 1, 200));
    RandomGear = sConfigMgr->GetBoolDefault("Playerbots.Random.Gear", true);
    RandomGearMaxQuality = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.GearMaxQuality", 3), 1, 5));

    RandomLfg = sConfigMgr->GetBoolDefault("Playerbots.Random.Lfg", true);
    RandomBattlegrounds = sConfigMgr->GetBoolDefault("Playerbots.Random.Battlegrounds", true);
    RandomArenas = sConfigMgr->GetBoolDefault("Playerbots.Random.Arenas", true);
    RandomQuests = sConfigMgr->GetBoolDefault("Playerbots.Random.Quests", true);
    RandomNamePool = sConfigMgr->GetBoolDefault("Playerbots.Random.NamePool", true);
    RandomQuestDistance = std::clamp(sConfigMgr->GetFloatDefault("Playerbots.Random.QuestDistance", 600.0f), 50.0f, 5000.0f);
    QuestMaxRouteMinutes = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Quest.MaxRouteMinutes", 20), 1, 600));
    RandomOnlyWithPlayers = sConfigMgr->GetBoolDefault("Playerbots.Random.OnlyWithPlayers", true);
    RandomPlayerLoginDelay = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Random.PlayerLoginDelay", 30)));
    RandomPlayerLogoutDelay = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Random.PlayerLogoutDelay", 300)));
    RandomGroupInvitations = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Random.GroupInvitations", 2), 0, 2));
    RandomGuildInvitations = sConfigMgr->GetBoolDefault("Playerbots.Random.GuildInvitations", true);
    RandomWhisperReply = sConfigMgr->GetBoolDefault("Playerbots.Random.WhisperReply", true);
    RandomMaps = sConfigMgr->GetStringDefault("Playerbots.Random.Maps", "");
    RandomMaxGrids = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Random.MaxGrids", 0)));

    BotActiveAlone = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone", 0), 0, 100));
    BotActiveAloneRadius = std::max(0.0f, sConfigMgr->GetFloatDefault("Playerbots.BotActiveAlone.Radius", 150.0f));
    BotActiveAloneZone = sConfigMgr->GetBoolDefault("Playerbots.BotActiveAlone.Zone", false);
    BotActiveAloneMap = sConfigMgr->GetBoolDefault("Playerbots.BotActiveAlone.Map", false);
    BotActiveAloneFriend = sConfigMgr->GetBoolDefault("Playerbots.BotActiveAlone.Friend", true);
    BotActiveAloneGuild = sConfigMgr->GetBoolDefault("Playerbots.BotActiveAlone.Guild", true);
    BotActiveAloneDurationSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.DurationSeconds", 30), 5, 3600));
    BotActiveAloneForceGraceSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.ForceGraceSeconds", 10), 0, 600));
    BotActiveAloneZoneMaxActive = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.ZoneMaxActive", 0)));
    BotActiveAloneRadiusMaxActive = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.RadiusMaxActive", 0)));
    BotActiveAloneSmartScale = sConfigMgr->GetBoolDefault("Playerbots.BotActiveAlone.SmartScale", true);
    BotActiveAloneSmartScaleFloor = uint32(std::max(1, sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.SmartScaleFloor", 50)));
    BotActiveAloneSmartScaleCeiling = uint32(std::max(int32(BotActiveAloneSmartScaleFloor + 1),
        sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.SmartScaleCeiling", 200)));
    BotActiveAloneSmartScaleMinLevel = uint32(std::max(1, sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.SmartScaleMinLevel", 1)));
    BotActiveAloneSmartScaleMaxLevel = uint32(std::max(int32(BotActiveAloneSmartScaleMinLevel),
        sConfigMgr->GetIntDefault("Playerbots.BotActiveAlone.SmartScaleMaxLevel", maxPlayerLevel)));

    HotspotAutoTiers = 0;
    std::string const autoTiers = sConfigMgr->GetStringDefault("Playerbots.Hotspot.AutoTiers", "1");
    for (std::string_view token : Trinity::Tokenize(autoTiers, ',', false))
        if (Optional<uint32> tier = Trinity::StringTo<uint32>(token); tier && *tier >= 1 && *tier <= 5)
            HotspotAutoTiers |= 1u << (*tier - 1);
    HotspotProb = std::clamp(sConfigMgr->GetFloatDefault("Playerbots.Hotspot.Prob", 0.35f), 0.0f, 1.0f);
    static constexpr uint32 DefaultWeights[6] = { 0, 5, 2, 1, 2, 1 };
    static constexpr uint32 DefaultOccupancy[6] = { 0, 75, 30, 10, 20, 15 };
    for (uint32 tier = 1; tier <= 5; ++tier)
    {
        HotspotTierWeight[tier] = uint32(std::max(0, sConfigMgr->GetIntDefault(Trinity::StringFormat("Playerbots.Hotspot.TierWeight.{}", tier), int32(DefaultWeights[tier]))));
        HotspotTierMaxOccupancy[tier] = uint32(std::max(0, sConfigMgr->GetIntDefault(Trinity::StringFormat("Playerbots.Hotspot.TierMaxOccupancy.{}", tier), int32(DefaultOccupancy[tier]))));
    }
    HotspotOccupancyRadius = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Hotspot.OccupancyRadius", 60.0f));
    HotspotSpreadRadius = std::max(0.0f, sConfigMgr->GetFloatDefault("Playerbots.Hotspot.SpreadRadius", 15.0f));
    HotspotWanderRadius = std::max(10.0f, sConfigMgr->GetFloatDefault("Playerbots.Hotspot.WanderRadius", 40.0f));
    HotspotRotateMinutes = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Hotspot.RotateMinutes", 30)));
    HotspotMarkers = sConfigMgr->GetBoolDefault("Playerbots.Hotspot.Markers", true);

    TravelEnable = sConfigMgr->GetBoolDefault("Playerbots.Travel.Enable", true);
    TravelMaxRouteMinutes = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Travel.MaxRouteMinutes", 45), 1, 600));
    TravelHopMinSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Travel.HopMinSeconds", 120), 10, 3600));
    TravelHopMaxSeconds = std::max(TravelHopMinSeconds, uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Travel.HopMaxSeconds", 300), 10, 3600)));
    TravelFreeFlights = sConfigMgr->GetBoolDefault("Playerbots.Travel.FreeFlights", true);
    TravelDangerSecondsPerLevel = std::max(0.0f, sConfigMgr->GetFloatDefault("Playerbots.Travel.DangerSecondsPerLevel", 30.0f));
    TravelDangerBlockLevels = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Travel.DangerBlockLevels", 12), 3, 255));

    RotationEnable = sConfigMgr->GetBoolDefault("Playerbots.Rotation.Enable", true);
    RotationPath = sConfigMgr->GetStringDefault("Playerbots.Rotation.Path", "");

    RpgEnable = sConfigMgr->GetBoolDefault("Playerbots.Rpg.Enable", true);
    RpgWeight[1] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.DoQuest", 60)));
    RpgWeight[2] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.WanderNpc", 20)));
    RpgWeight[3] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.TravelFlight", 15)));
    RpgWeight[4] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.GoGrind", 15)));
    RpgWeight[5] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.GoCamp", 10)));
    RpgWeight[6] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.Rest", 5)));
    RpgWeight[7] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.WanderRandom", 15)));
    RpgDurationMs[1] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.DoQuest", 1800000)));
    RpgDurationMs[2] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.WanderNpc", 300000)));
    RpgDurationMs[3] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.TravelFlight", 180000)));
    RpgDurationMs[4] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.GoGrind", 300000)));
    RpgDurationMs[5] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.GoCamp", 180000)));
    RpgDurationMs[6] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.Rest", 30000)));
    RpgDurationMs[7] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.WanderRandom", 300000)));
    RpgNpcStayTime = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.NpcStayTime", 8000)));
    RpgPoiStayTime = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.PoiStayTime", 300000)));
    RpgStuckTime = uint32(std::max(5000, sConfigMgr->GetIntDefault("Playerbots.Rpg.StuckTime", 90000)));
    RpgRepairDurabilityPct = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Rpg.RepairDurability", 80), 0, 100));
    RpgSellBagSpacePct = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Rpg.SellBagSpace", 80), 0, 100));
    RpgWeight[8] = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.Weight.Gather", 12)));
    RpgDurationMs[8] = uint32(std::max(1000, sConfigMgr->GetIntDefault("Playerbots.Rpg.Duration.Gather", 600000)));
    RpgGatherDistance = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Rpg.GatherDistance", 120), 20, 1000));
    RpgCapitalBoost = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.Rpg.CapitalBoost", 20)));
    RpgMountMinLevel = uint32(std::max(1, sConfigMgr->GetIntDefault("Playerbots.Rpg.MountMinLevel", 20)));

    RemotePort = uint16(std::clamp(sConfigMgr->GetIntDefault("Playerbots.Remote.Port", 0), 0, 65535));
    RemoteBindIp = sConfigMgr->GetStringDefault("Playerbots.Remote.BindIp", "127.0.0.1");
    RemoteToken = sConfigMgr->GetStringDefault("Playerbots.Remote.Token", "");
}

Config& Config::Instance()
{
    static Config instance;
    return instance;
}
}
