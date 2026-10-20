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
    bool DynamicTickEnable = false;    // the AI tick delay follows the situation instead of the fixed UpdateInterval
    uint32 DynamicTickCombatMs = 300;  // delay between two decisions while fighting
    uint32 DynamicTickIdleMinMs = 1000;    // alive and not fighting: a random delay in this range
    uint32 DynamicTickIdleMaxMs = 2500;
    uint32 DynamicTickDeadMs = 2000;
    bool MoveFallbacks = false;        // a destination with no mesh path is tried at other heights, then through cone-sampled stepping stones
    bool CombatRoleTargeting = false;  // in a group: tanks take what nobody tanks, damage dealers follow the main tank's target
    uint32 CombatThreatLimitPct = 0;   // a bot that is not tanking stops attacking at this share of the tank's threat (0 = off)
    uint32 CombatWaitForAttackMs = 0;  // bots in a group a player leads do not attack for this long after a fight starts (0 = off)
    bool CombatPositioning = false;    // melee behind the target, ranged spread, tanks turn the enemy away from the group
    bool CombatKite = false;           // ranged bots step away from a melee enemy that is hitting them
    uint32 CombatStuckSeconds = 0;     // a fight where nothing changes (hit points of the enemy, the enemy itself) this long is given up (0 = off)
    bool CombatEscapeUnseen = false;   // a bot hit by attackers it cannot target (unseen, unattackable) runs from them, and goes home when that does not help
    bool CombatGrindQuality = false;   // random bots pick fights in line of sight, on their floor, not gray and not already taken
    bool FollowWalkFirst = false;      // a bot far from its master walks to it for a while before it teleports
    bool FollowMirrorMaster = false;   // a following bot mounts when its master mounts and takes the master's flight path
    uint32 FollowWalkSeconds = 45;
    uint32 FollowWalkMaxYards = 300;
    bool CombatUsePotions = false;     // in a fight: a health potion or healthstone below 35% health, a mana potion below 20% mana
    bool DeathSpiritHealer = false;    // an ownerless bot that dies outside a dungeon or battleground goes to the graveyard and the spirit healer
    bool GroupEvents = false;          // bots answer ready checks, open and accept trades from their group, and take quests the group shares
    bool DungeonEnable = false;        // encounter logic for the dungeons the module knows (Utgarde Keep so far)
    bool MoveHopLowObstacles = false;   // jump low obstacles on level ground instead of walking through them
    bool MoveAvoidBlockedPaths = false; // detour around geometry the mesh does not know, refuse cliff segments
    bool MoveLogBlockedPaths = false;  // log mesh paths that cross VMAP or game object geometry (measurement only, changes nothing)
    bool MountsVariety = false;        // random bots own several ground mounts by level instead of one vendor mount
    bool CommandMaintenance = false;   // the owner command "maintenance" (free repair and food)
    uint32 GearRerollVersion = 0;      // raise it to give every random bot a new gear roll once (0 = off)
    uint32 GearVariety = 0;            // percent of the best score an item may fall short of and still be picked (autogear and random gear)
    bool GearHunterPets = false;       // random hunters of level 10 and up without a beast get a tamed one
    bool GearStockPotions = false;     // random bots carry a few health potions (and mana potions for mana users) of their level, for Combat.UsePotions
    bool GearEquipUpgrades = false;    // an item in the bags that beats what the bot wears is put on (won rolls, traded gifts)
    bool RotationUtility = false;      // rotations may use the utility list and the dispel, resurrection, group buff and crowd control targets
    bool LootRoll = false;             // bots answer group loot rolls: need an upgrade, greed what sells, pass what they cannot use
    bool CastSupervisorEnable = false; // wait for a cast that is preparing; cancel it when the target died or a heal is wasted
    bool PerfMonEnabled = false;       // time engine ticks and actions, shown by ".bot perf"
    uint32 PerfMonMemorySeconds = 30;       // RSS and loaded grids line (0 = off)
    uint32 PerfMonReportSeconds = 60;       // world tick window report (p50/p95/max of the world diff, slow ticks, gate counters), 0 = off
    uint32 PerfMonSlowTickMs = 150;         // a world tick above this logs its host memory / swap / fault state
    uint32 PerfMonGridCensusSeconds = 60;   // window report of what created new grids (who stood in or next to them), 0 = off
    std::string PerfMonChatDriver;     // "<messages per second> <say|yell|emote|channel|guild|mix>": start the synthetic chat driver once bots are online
    std::string WarmMaps;              // continents to create at startup, "0:-8913:554,1:1629:-4373" (mapId or mapId:x:y; x,y also loads that grid); empty = off
    uint32 ManagerSliceMs = 0;         // frozen idle bots are visited by the manager loop once per this many ms (0 = every world tick)
    uint32 SimVerdictCacheMs = 0;      // how long a bot's whole 'simulated or frozen' verdict is reused (0 = recompute group/LFG/instance state every call)
    bool SleepElevators = false;       // put elevators (active game objects) to sleep so their surroundings only update near an awake player (world change, module-only)
    uint32 WarmHubs = 0;               // Warm.Maps companion: pre-load the grids around this many hubs (capitals first), 0 = off
    uint32 WarmPlayerPositions = 0;    // ... and around the last positions of this many real (non-bot) characters, 0 = off
    bool WarmPin = true;               // keep the warmed grids loaded (no unloading after Grid cleanup delay)
    float WarmRadius = 120.0f;         // yards around each warm point whose grids are loaded (3x3 samples at this spacing)
    uint32 RandomCreateUpTo = 0;       // keep creating characters in the background (one per Random.CreateDelay, whatever is online) until this many exist; 0 = only to reach Random.Count
    bool FarBotNoVisibilityLoad = false; // a far bot's pending visibility notify is dropped, so the core does not load the grids around it (LoadGridsInRange)
    bool NoVisibilityGridLoad = false; // bot sessions do not make the core load the grids around them for visibility (WorldSession::SetHeadlessLoadsVisibilityGrids)
    bool TravelDeferHops = false;     // a far bot's route hop is not placed (no TeleportTo, no new grid) unless the target grid is loaded or a player is near it
    bool FarBotHeightNoGridLoad = false; // a ground-height query of a frozen / rotation-only bot does not load the grid it asks about
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
    bool RandomSayWhyNot = false;      // a random bot whispers why it turns down a party invitation
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
    uint32 DebugForceActive = 0;                 // test knob: about this many random bots run as if a player were near (0 = off)
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
    bool AirEnable = false;                    // air legs: bots on the test list (or with a master) may fly between towns (Travel/AirCorridor)
    std::string AirTestBots;                   // names of the random bots allowed to fly, comma separated, any case
    uint32 AirMinLegYards = 400;               // an air link joins two towns at least this far apart ...
    uint32 AirMaxLegYards = 3000;              // ... and at most this far
    uint32 AirNeighbours = 3;                  // each town gets air links to this many towns in that range
    uint32 AirMaxGridLoads = 2;
    bool AirFollow = false;                    // party and raid bots follow a flying master into the air (Travel/AirFollow)
    bool AirFollowGrant = true;                // a bot that follows a flying master gets master riding and a flying mount when it has none
    uint32 AirFollowCatchUpYards = 150;        // a following bot further behind than this ...
    uint32 AirFollowCatchUpSeconds = 20;       // ... for this long is placed in the air behind the master                // a corridor that needs more unloaded grids than this is refused for now
    bool TravelFreeFlights = true;             // a bot that boards a flight gets the gold for it
    float TravelDangerSecondsPerLevel = 30.0f; // a walk past creatures above the bot's level (beyond 3) counts this much longer per level
    uint32 TravelDangerBlockLevels = 12;       // past creatures this many levels above the bot a route is not walked at all

    bool RotationEnable = true;                // specialization rotations (data/rotations/*.rot) instead of the generic spell picker
    std::string RotationPath;                  // directory of the *.rot files, empty = modules/mod-playerbots/data/rotations of the source directory

    bool RpgEnable = true;
    uint32 RpgWeight[10] = { 0, 60, 20, 15, 15, 10, 5, 15, 12, 8 };
    uint32 RpgDurationMs[10] = { 0, 1800000, 120000, 180000, 300000, 120000, 30000, 300000, 600000, 45000 };
    uint32 RpgGatherDistance = 120;            // yards a gatherer walks to a herb or ore node to work at it
    uint32 RpgNpcStayTime = 8000;
    uint32 RpgPoiStayTime = 300000;
    uint32 RpgStuckTime = 90000;
    bool RpgSellJunk = true;                  // a bot at a vendor sells the grey items it carries (the client's "sell all junk")
    bool RpgTrain = true;                     // a bot at a trainer learns what it can use and afford (profession recipes)
    uint32 RpgTrainMaxPerVisit = 8;
    bool RpgMailbox = true;                   // a bot at a mailbox takes money and items (world thread)
    bool RpgBuyFood = true;                   // a bot at a vendor buys food and drink when its stack is low
    bool AhPost = true;                       // a bot at an auctioneer posts grey and white items (world thread)
    uint32 AhMaxPostsPerVisit = 3;
    uint32 AhVisitCooldownMs = 1800000;       // 30 min between auction visits
    bool SpawnEvict = true;                   // login re-placement and the once-a-minute move of bots off enemy ground
    bool GearClassArmorOnly = true;           // armor slots take only the class's armor type; a wrong type triggers one re-gear
    bool MountsRacialGrant = true;            // riding skill by level and a racial mount in the collection
    float RpgHopChance = 0.03f;               // per second while walking on foot
    uint32 RpgHopMinIntervalMs = 12000;       // between two hops of one bot
    float RpgIdleHopChance = 0.04f;           // per resting roll
    bool RpgGreetEmotes = true;               // wave/hello/bow at a player standing close
    float RpgGreetDistance = 12.0f;
    uint32 RpgGreetCooldownMs = 300000;       // per bot
    float RpgGreetChance = 0.4f;              // when the cooldown is over and a player is close
    uint32 RpgRepairDurabilityPct = 80;
    uint32 RpgSellBagSpacePct = 80;
    uint32 RpgCapitalBoost = 15;
    uint32 RpgMountMinLevel = 20;
    bool GoalEnable = false;                  // playerbots_goal picks the activity; off keeps today's status weights
    bool MemoryEnable = false;                // rings of place, goal, and person, shown by .bot inspect
    bool QuestCollect = false;                // kill quests may also include an item a creature drops
    bool QuestInteract = false;               // gameobject use, talk-to, and chest loot
    bool QuestExplore = false;                // walk into an area trigger
    bool QuestAbstractXp = false;             // a far quest goal grants XP and does not complete the log quest
    bool BgObjectives = false;                // Warsong and Arathi bots play the flag and the nodes
    bool WorldReactions = false;              // a hostile player, a rare, or a duel in a city
    bool ReactReturnEmotes = true;            // a nearby bot answers a player's wave, hello, bow and the like
    bool ReactChairs = true;                  // a resting bot sits on a chair within 15 yards

    // where far bots land, by level band: 1-19, 20-39, 40-59, 60-69, 70-79, 80-84, 85-89, 90
    uint32 PopulationWeight[8] = { 10, 10, 10, 10, 10, 10, 10, 12 };
    uint32 PopulationTeleportMin = 3600;      // seconds between far-bot zone moves
    uint32 PopulationTeleportMax = 18000;
    uint32 PopulationLevelUpChance = 8;       // percent, on that move, of gaining one level
    uint32 PopulationReviveMin = 60;          // seconds a far corpse waits
    uint32 PopulationReviveMax = 300;

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
