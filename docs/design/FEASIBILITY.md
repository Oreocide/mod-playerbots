# Playerbots v1: feasibility

Each answer gives evidence (`path:line`) and confidence (H/M/L). Core paths are relative to `src/server/game/`. `mod:` = `modules/mod-playerbots/`.

## a. A Player whose WorldSession has no socket

**Yes. It already works through a local core patch (H).** Upstream master has no such mode. The patch:

| Upstream behavior without a socket | Patch | Cite |
|---|---|---|
| `SendPacket` drops packets when `m_Socket` is null | bot branch hands every packet to `_botPacketHandler` before the null check | `Server/WorldSession.cpp:262-274` |
| `Update` only drains `_recvQueue` while a socket exists | loop runs `while (m_Socket[...] \|\| _isBot)` | `WorldSession.cpp:395` |
| idle-connection kick | skipped for bots | `WorldSession.cpp:381` |
| time sync packets | skipped | `WorldSession.cpp:539` |
| `PlayerDisconnected()` true with no socket → logout | returns false for bots | `WorldSession.cpp:197` |
| dtor / `LogoutPlayer` set `account.online=0` | skipped (master bots share the owner's account) | `WorldSession.cpp:191,729` |
| `KickPlayer` closes the socket | sets `forceExit` for bots | `WorldSession.cpp:754` |
| no client to send `CMSG_PLAYER_LOGIN` | `LoginBotPlayer(guid)` builds the `LoginQueryHolder` and calls `HandlePlayerLogin` | `Handlers/CharacterHandler.cpp:1313` |
| Account data loads asynchronously | `InitializeSession(onInitialized)` callback, run after `InitializeSessionCallback` | `WorldSession.cpp:1494` |

Bot sessions are **not** in `World::m_sessions`. `PlayerbotMgr::Update` updates them on the world thread with a `WorldSessionFilter` (`mod:src/PlayerbotMgr.cpp:326-330`) (H).

Battle.net data: the session holds `unique_ptr<Battlenet::Account> _battlenetAccount` (`Server/WorldSession.h:2064`), and `GetBattlenetAccountId()` returns its GUID counter (`WorldSession.cpp:204`). Random bots get real bnet + game accounts (b), so all bnet-scoped data (collections, warband) loads normally (H code read; M runtime, warband groups untested for bots).

Save/logout: `Player::SaveToDB` (`Entities/Player/Player.cpp:20448`) runs unchanged. Saves at 1000 online cost real DB writes; see PERFORMANCE.md (M).

Rules that must hold:
1. **Never queue client packets for a frozen bot.** `WorldSessionFilter` rejects THREADSAFE opcodes for an in-world player, and the map-thread session update is skipped while frozen (`Maps/Map.cpp:749`). The module calls handshake handlers directly instead: `HandleMoveTeleportAck` (`mod:src/PlayerbotAI.cpp:625`), and `HandleSuspendTokenResponse`/`HandleMoveWorldportAck` (`mod:src/PlayerbotMgr.cpp:358-362`) (H).
2. Track `Server/BotPerf.{h,cpp}`. They are untracked, but tracked files include them (H; see GAPS G1).

## b. Accounts and characters for bots

| Step | How | Cite | Conf |
|---|---|---|---|
| Bnet account | `<prefix>NNNN@<domain>` via `CreateBattlenetAccount(email, pass, withGameAccount, &name)` | `mod:src/RandomBotMgr.cpp:496`; core `Accounts/BattlenetAccountMgr.cpp:34` | H |
| Game accounts | `sAccountMgr->CreateAccount(name, pass, email, bnetId, index)`, up to 10 per bnet email | `mod:src/RandomBotMgr.cpp:515`; core `Accounts/AccountMgr.cpp:47` | H |
| Race/class | ChrRaces + class unlock/expansion rules; Demon Hunter and Evoker skipped (they need an existing high-level character) | `mod:src/RandomBotMgr.cpp` race picker; NOTES.md | H |
| Customization | random valid choices that honor requirements (`FillCustomizations`), then the **real** `HandleCharCreateOpcode` with a timeout | `mod:src/RandomBotMgr.cpp:155,923-930`; core `Handlers/CharacterHandler.cpp:811`, `ValidateAppearance` `:760` | H |
| Empty choice list | 12.1.5 hangs on it. The core guard is removed in the working tree; the bot side is fixed in the module (`FillCustomizations` never sends an empty list), with no core edit (CORE_EXIT.md) | GAPS G2 | H |
| Starting zone / Exile's Reach | `UseNPE` is never set (default false, `Server/Packets/CharacterPackets.h:67`). The NPE is just `playercreateinfo.npe_*` (`Globals/ObjectMgr.cpp:3820`) chosen at `Entities/Player/Player.cpp:425`. Bots start at the classic race start, then `Place()` moves them to a hub or graveyard spot (`mod:src/RandomBotMgr.cpp:708` `BuildSpots`) | — | H |

**Decision: bots never enter Exile's Reach** (scripted scenario, intro scene; no value for town life).

## c. How a bot "clicks": handler call vs. direct API

| Criterion | Handler with a constructed client packet object | Direct core API |
|---|---|---|
| Validation | full: `GetNPCIfCanInteractWith` (`Player.cpp:1842`) checks distance, npcflag, faction, phase, alive, plus the handler's own checks | only what the API itself checks; e.g. `SetHomebind` checks nothing |
| Interaction state | **required**: buy checks `IsInteractingWith(Trainer)` + `TrainerId` (`Handlers/NPCHandler.cpp:145-148`), set by the list handler (`:125-126`); mail take needs `CanOpenMailBox(SourceGuid)` (`Handlers/MailHandler.cpp:357,379`), set by `MAIL_GET_LIST` (`:582`); vendor starts at LIST_INVENTORY (`Handlers/ItemHandler.cpp:586`) | bypasses it, so state can drift from what a client would see |
| Speed | packet object built on the stack plus handler body; negligible at ≤1 interaction per bot per ~10 s | marginally faster |
| Breakage on master merges | packet field renames are **compile errors** (visible) | silent behavior drift (new checks added to handlers are skipped) |
| Threading | must respect the opcode's processing flag (below) | same rules, but nothing tells you |

**Decision:** use handlers for every NPC service, replaying the client's order **open → list → act → close**. Use direct API only where no client path exists or the client path needs UI state the bot cannot produce: `ActivateTaxiPathTo` with no NPC for virtual travel (`mod:src/Travel/Taxi.cpp:65`), `DurabilityRepairAll` (`Player.cpp:4601`) (H). This is the existing pattern: `HandleCharCreateOpcode` (`mod:src/RandomBotMgr.cpp:930`) and BG/LFG joins in `ActivityMgr`.

Opcode processing flags (`Server/Protocol/Opcodes.cpp`) decide which thread may call each handler (H):

| Flag | Opcodes | Called from |
|---|---|---|
| INPLACE | BINDER_ACTIVATE :245, BUY_ITEM :257, LIST_INVENTORY :665, QUEST_GIVER_* :916-920, REPAIR_ITEM :954, SELL_ITEM :1017, SEND_TEXT_EMOTE :1024, TALK_TO_GOSSIP :1114, TRAINER_LIST/BUY_SPELL :1128-1129 | the bot's own map thread (in `PlayerbotAI::Update`) |
| THREADSAFE | ACTIVATE_TAXI :157, ENABLE_TAXI_NODE :457 | the bot's own map thread |
| THREADUNSAFE | AUCTION_BROWSE_QUERY :178, AUCTION_SELL_ITEM :192, GOSSIP_SELECT_OPTION :522, MAIL_GET_LIST/TAKE_ITEM/TAKE_MONEY :698-702 | **world thread only**, through a module queue drained in `WorldScript::OnUpdate`. At drain time the item re-resolves the bot by GUID and is dropped if the bot is gone, on another map, or out of range |

## d. Movement: path, jump, mount, swim, fly, stuck

**Requirement (user, 2026-10-15): bots move only along the mmap navmesh. No straight lines through buildings, no Z-snap teleports.**

Facts (H):
- mmaps are enabled (`docker/etc/worldserver.conf:424` `mmap.enablePathFinding = 1`). Data is present (`docker/data/mmaps`, 28,689 files). Whether the tiles match 12.1.5 maps is unverified (L).
- `PathGenerator` works for Players. The Player filter is ground+water+magma (`Movement/PathGenerator.cpp:668-671`).
- Limits are 74 polys / 74 points × 4 yd ≈ 300 yd (`Movement/PathGenerator.h:32-35`).
- With the `Playerbots.LazyMMap` gate on, `PathGenerator` loads a missing tile on demand (`Movement/PathGenerator.cpp:79`, `Maps/Map.h:284`). That costs about 400 ms the first time, so far bots must not path.

Where the module leaves the navmesh today (H):

| # | Code | Problem |
|---|---|---|
| M1 | `mod:src/Travel/MoveToward.cpp:176-196` | No walkable path → 18 yd VMAP-only step (`MovePositionToFirstCollision` `:181`), then `MoveSplineInit::MoveTo(..., true)` (`:196`). On NOPATH that call becomes a **2-point straight spline** (`Movement/Spline/MoveSplineInit.cpp:293-303`) → walks through walls |
| M2 | `TryGetStandZ` `MoveToward.cpp:25-64` | takes the first VMAP surface 50/80 yd below hint+2, accepting up to 40 yd of difference; can pick the floor under a bridge, city or tunnel. Applied by teleport in `PlayerbotAI.cpp:363-365` (ground snap), `Strategies/Rpg/RpgStrategy.cpp:167-168` (stuck snap); used for destination Z at `RpgStrategy.cpp:291`, `Strategies/GenericStrategies.cpp:564`, `Strategies/QuestTravel.cpp:222` → **fall-through source** |
| M3 | `RememberStand` `PlayerbotAI.cpp:548-574` | "last good stand" is judged with the same VMAP height, so the rescue target can itself be on the wrong floor |
| M4 | `PathIsWalkable` `MoveToward.cpp:105` | accepts INCOMPLETE paths with no segment plan; long walks stall or loop |
| M5 | travel graph walk links | straight-line estimates, never mmap-verified (NOTES.md stage B); virtual hops teleport to node coordinates (`mod:src/Travel/RouteFollower.cpp:467`) |
| M6 | `UpdateMountForTravel` `MoveToward.cpp:125-147` then the spline at `:172+` | mount cast and movement start in the same call → cast-time mounts get interrupted (M) |

Design: **`BotMover`** (replaces `MoveToward`; details in BEHAVIORS.md §Movement):
- **Path only.** Launch only `PathGenerator` results of type NORMAL, or INCOMPLETE followed in segments (re-path from the segment end; progress ≥5 yd per segment). Never `MoveTo` with a raw point, never the VMAP step, never SHORTCUT/NOPATH. With no path: mark the destination unreachable for this bot (10 min cooldown) and fall back.
- **Destinations snapped to the navmesh.** Path with `forceDest=false`, use `GetActualEndPosition()` (`Movement/PathGenerator.h:80`). Reject if >3 yd 2D or >2.5 yd Z from the desired NPC/spot. Same trick creature random movement relies on. `GetStandZ` is no longer used to invent a destination Z.
- **No Z-snap teleports for simulated bots.** Recovery teleports only to the last position where a path segment ended on the mesh, or to home/graveyard.
- **Fall watchdog** (simulated only, once per AI tick):
  - triggers: falling >3 s; Z more than 10 yd below the navmesh floor; off-mesh after a spline ends.
  - action: rescue to the last mesh position, blacklist the destination, log to `server.playerbots.stuck`.
- **Jump** = `MotionMaster::MoveJump` (`Movement/MotionMaster.h:196`), only on a straight, flat, on-mesh path stretch, landing point path-snapped; re-launch the remaining path after landing (M: Player + MoveJump spline must be play-tested).
- **Mount**: choose with `Unit::GetMountCapability` (`Entities/Unit/Unit.cpp:8382`; area `AllowFlyingMounts`, riding skill, liquid) and pre-check `SpellInfo::CheckLocation` (`Spells/SpellInfo.cpp:2170`). Never in water (`Unit::IsInWater` `Unit.cpp:3350`) or indoors. Start the path after the cast completes.
- **Swim**: PATHFIND paths through water are allowed by the filter; dismount before water; no mount in water.
- **Fly: not in v1.** There is no air navmesh. Skyriding lift is unimplemented (`SMSG_MOVE_ADD_IMPULSE`; GAPS G6). Long distance uses taxi.
- **Stuck**: keep the 90 s / 5 yd rule (`RpgStrategy.cpp:154`) but without the Z-snap. Add a loop detector: same behavior+target failing 3× in 10 min → blacklist + log.

## e. Taxi with no client

- Node state: `PlayerTaxi::IsTaximaskNodeKnown/SetTaximaskNode` (`Entities/Player/PlayerTaxi.h:50/56`) (H).
- Faction: `ObjectMgr::GetNearestTaxiNode` filters by `TaxiNodeFlags::ShowOnHordeMap/ShowOnAllianceMap` (`Globals/ObjectMgr.cpp:6646-6673`); `TaxiNodesEntry::MountCreatureID[team]` (H).
- Start: `Player::ActivateTaxiPathTo(nodes, npc, …)` (`Entities/Player/Player.cpp:23168`) works with or without an NPC. With an FM, `HandleActivateTaxiOpcode` (`Handlers/TaxiHandler.cpp:159`, THREADSAFE) gives full validation (H).
- Today: `TakeTaxi` marks from/dest **and every node on the route** known (`mod:src/Travel/Taxi.cpp:76-84`). `Travel.FreeFlights` grants 20 g instead of free flights (`RouteFollower.cpp:229-231`) (H).
- **Decision (user, 2026-10-15): least memory/CPU.** Keep the existing auto-unlock of route nodes. No per-bot discovery tracking, no node-mask-filtered planning (the planner keeps one shared graph), no extra FM trips. Visible bots still walk to the FM and call `ActivateTaxiPathTo(nodes, npc)` directly. Frozen bots use virtual travel.

## f. Phasing and visibility

- Static layer `PhaseGuard` (`mod:src/Travel/PhaseGuard.cpp:48-66`): spawn usable if phase 0, 169, or a *shared* phase (in `phase_area`, no type-26 condition). Midnight Silvermoon (shared phase 24825) depends on this (H).
- Runtime layer (new): before accepting any NPC/GO target, check `bot->InSamePhase(*target)` (`Entities/Object/Object.h:314`) and `bot->CanSeeOrDetect(target)` (`Entities/Object/Object.cpp:855`). The handler's `GetNPCIfCanInteractWith` repeats the check at use time (H).
- A target that fails either check goes on the per-bot GUID cooldown (10 min) (design).

## g. Destination discovery

- `HubIndex::Build` (`mod:src/Travel/HubIndex.cpp:119`) reads `creature` + `creature_template.npcflag` (flight master, banker, auctioneer, innkeeper, repair, vendor, trainer, quest giver bits) and GO type 19 mailboxes (`:180-183`), with the default phase/difficulty filter (`:96`), then clusters them into hubs with tiers (H).
- Hotspots: `HotspotMgr` + world tables `playerbots_hotspot_marker`, `playerbots_hotspot_override` (H).
- **Zones with no spawns**: lookups return null and the bot idles (`RpgStrategy.cpp:483-489`). `ChooseNpcTarget`'s hub fallback only looks 80 yd (`Strategies/Rpg/NpcDispatch.cpp:123`) (H).
- **Decision:** fallback chain: nearest hub with the service on the same map within `Travel.MaxRouteMinutes` → wander near home → idle. Never more than 3 attempts per service per 10 min; a missing NPC is never retried by GUID until its cooldown expires.

## h. Threading

| Work | Thread | Cite | Conf |
|---|---|---|---|
| RandomBotMgr (create/login/logout/rotate), PlayerbotMgr session updates (THREADUNSAFE opcodes), ActivityMgr, BotSocialMgr, perf reporter, **new world queue + scheduler** | world thread, `WorldScript::OnUpdate` after maps | `mod:src/Script/PlayerbotsScripts.cpp:61`; `World/World.cpp:2437` | H |
| `PlayerbotAI::Update`: decisions, BotMover, INPLACE/THREADSAFE handlers, grid searches on its own map | that bot's map thread | `PlayerbotsScripts.cpp:98`; `Entities/Player/Player.cpp:902,1103` | H |
| GM commands | world thread (chat command path) | — | M |
| Chat hooks (`OnChat` whisper/party), fired by a human's chat packet | world thread: chat opcodes are THREADUNSAFE (`Server/Protocol/Opcodes.cpp:333-337`), handled in `World::UpdateSessions` while no map thread runs | `PlayerbotsScripts.cpp:141-185` | H |

Shared state and locking:

| State | Today | Plan |
|---|---|---|
| HubIndex, PhaseGuard, TravelGraph, GatherNodes | built once at startup, then read-only | lock-free (immutable after build) |
| Hotspot occupancy | `shared_mutex` | keep |
| `RandomBotMgr::_homes/_homeMaps/_anchors` | **written from map threads, no lock** (`PlayerbotAI.cpp:542` → `UpdateHome`; `RpgStrategy.cpp:82` → `ClearAnchor`) | route writes through the world queue |
| `PlayerbotRepository::_store` | **no lock** (`Db/PlayerbotRepository.cpp:163,176`) | mutex (short critical sections) |
| Scheduler tickets | — | one `std::atomic<bool>` per bot AI, written by the world thread, read by the map thread |
| Metrics | per-thread BotPerf counters merged on the world thread | same pattern for new counters |

Docker runs `MapUpdate.Threads = 1` (`docker/etc/worldserver.conf:510`), so the races are latent today. The P0 soak runs with 4 threads to expose them.
