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

#ifndef PlayerbotsConfig_h__
#define PlayerbotsConfig_h__

#include "Define.h"
#include <string>

namespace Playerbots
{
struct Config
{
    bool Enabled = true;
    uint32 MaxBotsPerPlayer = 4;
    uint32 UpdateInterval = 250;       // ms between two AI ticks of the same bot
    uint32 ScanInterval = 1500;        // ms a bot waits after a grid search (enemy, corpse) found nothing
    bool PerfMonEnabled = false;       // time engine ticks and actions, shown by ".bot perf"; also switches on the core BotPerf probes
    uint32 PerfMonReportSeconds = 60;  // seconds between two window reports in the perf log (Playerbots-Perf.log)
    uint32 PerfMonSlowTickMs = 150;    // a world tick longer than this gets a full attribution dump in the report
    uint32 PerfMonTopBots = 10;        // players listed in the slowest-tick dump
    bool PerfMonOpcodes = true;        // count packets sent to bot sessions per opcode
    uint32 PerfMonGridCensusSeconds = 60;   // loaded grids per map and why they are held (0 = off)
    uint32 PerfMonMemorySeconds = 30;       // RSS and jemalloc stats line (0 = off)
    uint32 PerfMonMemCensusSeconds = 300;   // per-bot content census: items, spells, channels... (0 = off)
    bool PerfMonLoginDetail = false;   // one perf-log line per login phase call (per bot), for per-bot vs per-grid regressions
    std::string PerfMonChatDriver;     // "<messages per second> <say|yell|emote|channel|guild|mix>": start the synthetic chat driver once bots are online
    std::string WarmMaps;              // continents to create at startup, "0:-8913:554,1:1629:-4373" (mapId or mapId:x:y; x,y also loads that grid); empty = off
    uint32 ManagerSliceMs = 0;         // frozen idle bots are visited by the manager loop once per this many ms (0 = every world tick)
    uint32 SimVerdictCacheMs = 0;      // how long a bot's whole 'simulated or frozen' verdict is reused (0 = recompute group/LFG/instance state every call)
    bool SleepIdleActiveObjects = false; // core: skip the cell visit of active game objects (elevators, doors) when no awake player is near
    bool SkipBotVisibilityGridLoad = false; // core: a bot relocating does not load the grids around it (only its own grid)
    uint32 WarmHubs = 0;               // Warm.Maps companion: pre-load the grids around this many hubs (capitals first), 0 = off
    uint32 WarmPlayerPositions = 0;    // ... and around the last positions of this many real (non-bot) characters, 0 = off
    bool WarmPin = true;               // keep the warmed grids loaded (no unloading after Grid cleanup delay)
    float WarmRadius = 120.0f;         // yards around each warm point whose grids are loaded (3x3 samples at this spacing)
    uint32 RandomCreateUpTo = 0;       // keep creating characters in the background (one per Random.CreateDelay, whatever is online) until this many exist; 0 = only to reach Random.Count
    bool FarBotHeightNoGridLoad = false; // a ground-height query of a frozen / rotation-only bot does not load the grid it asks about
    bool LazyMMap = false;             // core: load navmesh tiles when something first paths in them instead of with the grid
    uint32 FrozenTickMs = 0;           // frozen bots run their AI hook at most this often (0 = every map tick)
    bool RandomJoinChannels = false;   // join the zone channels (General, Trade, LFG...) of bots that are in none
    uint32 RandomMaxRssMB = 0;         // stop logging in bots while the process uses more than this (0 = off)
    uint32 LoginTimeout = 60000;       // ms until a bot that has not finished logging in is dropped
    float FollowDistance = 2.5f;
    float TeleportDistance = 100.0f;   // teleport to the master when further away (out of combat)
    float RangedDistance = 25.0f;
    float HealThreshold = 85.0f;       // heal group members below this health percentage
    bool AutoResurrect = true;         // resurrect in place once the master is out of combat
    std::string CommandPrefix;         // party chat is only a command for the bots when it starts with this (empty = always)
    std::string CommandSeparator = ";"; // splits one message into several commands (empty = no chaining)
    float CommandNearbyDistance = 30.0f; // yards for the @nearby / @far filters

    // random bots (own accounts, logged in without a master)
    bool RandomEnabled = false;
    uint32 RandomCount = 10;           // bots kept online
    uint32 RandomMinLevel = 1;         // level range of newly created bots
    uint32 RandomMaxLevel = 10;
    uint32 RandomLoginDelay = 2000;    // ms between two bot logins
    uint32 RandomCreateDelay = 3000;   // ms between two character creations
    uint32 RandomCharactersPerAccount = 40;
    uint32 RandomAllianceRatio = 1;    // share of the online population, against Horde
    uint32 RandomHordeRatio = 1;
    uint32 RandomLoginBatch = 5;       // bots logged in per RandomLoginDelay
    float RandomWanderRadius = 30.0f;
    std::string RandomEmailPrefix = "RNDBOT";      // emails are <prefix><0001..>@<domain>
    std::string RandomEmailDomain = "PLAYERBOTS.LOCAL";
    uint32 RandomGameAccountsPerEmail = 10;
    bool RandomGear = true;
    uint32 RandomGearMaxQuality = 3;
    bool RandomQuests = true;
    bool RandomNamePool = true;        // draw new bot names from playerbots_names before falling back to NameGen
    bool RandomLfg = true;             // fill dungeon finder queues of players
    bool RandomBattlegrounds = true;   // fill battleground queues of players
    bool RandomArenas = true;          // play arena opponents against player groups
    float RandomQuestDistance = 600.0f; // yards the hunting grounds of a quest may be from its giver
    uint32 QuestMaxRouteMinutes = 20;   // a bot takes a quest only if it can get to the giver within this
    bool RandomOnlyWithPlayers = true; // like AiPlayerbot.DisabledWithoutRealPlayer
    uint32 RandomPlayerLoginDelay = 30;  // seconds after a real player logs in
    uint32 RandomPlayerLogoutDelay = 300; // seconds after the last real player logs out
    uint32 RandomGroupInvitations = 2;  // party invitations of players: 0 = decline, 1 = accept within 10 levels, 2 = always accept
    bool RandomGuildInvitations = true; // join the guild of a player who invites the bot
    bool RandomWhisperReply = true;     // tell players who whisper a bot how to get it to listen
    uint32 RandomMaxGrids = 0;         // most distinct 533 yard grids the online random bots may stand in, 0 = no limit
    std::string RandomMaps;            // comma-separated continent map ids, empty = every outdoor continent

    // AiPlayerbot.BotActiveAlone: only a percent of lonely bots run full AI
    uint32 BotActiveAlone = 10;        // 0 = only near players, 100 = every bot
    float BotActiveAloneRadius = 150.0f;
    bool BotActiveAloneZone = false;
    bool BotActiveAloneMap = false;
    bool BotActiveAloneFriend = true;
    bool BotActiveAloneGuild = true;
    uint32 BotActiveAloneDurationSeconds = 30;   // the roster of lonely active bots changes this often
    uint32 BotActiveAloneForceGraceSeconds = 10; // a bot a player left stays awake this long (no flicker)
    uint32 BotActiveAloneZoneMaxActive = 0;      // 0 = every bot of the zone with a player wakes, else at most this many
    uint32 BotActiveAloneRadiusMaxActive = 0;    // same for the radius, the nearest ones first
    bool BotActiveAloneSmartScale = true;
    uint32 BotActiveAloneSmartScaleFloor = 50;    // world-tick ms below this: keep BotActiveAlone
    uint32 BotActiveAloneSmartScaleCeiling = 200; // at/above this: lonely bots freeze
    uint32 BotActiveAloneSmartScaleMinLevel = 1;
    uint32 BotActiveAloneSmartScaleMaxLevel = 90;

    // city anchors: hotspots are towns and cities (derived from service NPCs) and invisible marker NPCs
    float HotspotProb = 0.35f;                 // chance a placement or rotation puts a bot in a hotspot
    uint32 HotspotAutoTiers = 1;               // bit (tier - 1): tiers whose derived hubs become hotspots, default capitals only
    uint32 HotspotTierWeight[6] = { 0, 5, 2, 1, 2, 1 };         // by tier: 1 capital, 2 town, 3 outpost, 4 flight master, 5 transport
    uint32 HotspotTierMaxOccupancy[6] = { 0, 75, 30, 10, 20, 15 }; // players and bots near a hotspot before it counts as full
    float HotspotOccupancyRadius = 60.0f;
    float HotspotSpreadRadius = 15.0f;         // bots arrive within this of a service NPC of the hotspot
    float HotspotWanderRadius = 40.0f;         // an anchored bot wanders within this of its hotspot
    uint32 HotspotRotateMinutes = 30;          // idle bots get a new roll about this often, 0 = never
    bool HotspotMarkers = true;                // invisible marker NPCs register hotspots

    bool TravelEnable = true;                  // build the travel graph (routes between towns, flight paths, portals)
    uint32 TravelMaxRouteMinutes = 45;         // routes longer than this are not taken
    uint32 TravelHopMinSeconds = 120;          // a bot nobody can see jumps ahead along its route every hop,
    uint32 TravelHopMaxSeconds = 300;          // as long as the travel time of that stretch, within these limits
    bool TravelFreeFlights = true;             // a bot that boards a flight gets the gold for it
    float TravelDangerSecondsPerLevel = 30.0f; // a walk past creatures above the bot's level (beyond 3) counts this much longer per level
    uint32 TravelDangerBlockLevels = 12;       // past creatures this many levels above the bot a route is not walked at all

    bool RotationEnable = true;                // specialization rotations (data/rotations/*.rot) instead of the generic spell picker
    std::string RotationPath;                  // directory of the *.rot files, empty = modules/mod-playerbots/data/rotations of the source directory

    bool RpgEnable = true;
    uint32 RpgWeight[9] = { 0, 60, 20, 15, 15, 10, 5, 15, 12 };
    uint32 RpgDurationMs[9] = { 0, 1800000, 300000, 180000, 300000, 180000, 30000, 300000, 600000 };
    uint32 RpgGatherDistance = 120;            // yards a gatherer walks to a herb or ore node to work at it
    uint32 RpgNpcStayTime = 8000;
    uint32 RpgPoiStayTime = 300000;
    uint32 RpgStuckTime = 90000;
    uint32 RpgRepairDurabilityPct = 80;
    uint32 RpgSellBagSpacePct = 80;
    uint32 RpgCapitalBoost = 20;
    uint32 RpgMountMinLevel = 20;

    // remote control port (off unless a port is set)
    uint16 RemotePort = 0;
    std::string RemoteBindIp = "127.0.0.1";
    std::string RemoteToken;

    void Load();

    static Config& Instance();
};
}

#define sPlayerbotsConfig Playerbots::Config::Instance()

#endif // PlayerbotsConfig_h__
