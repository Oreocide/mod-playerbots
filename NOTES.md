# mod-playerbots implementation notes

Current-state reference for the module's design and gotchas. Dated session logs, deploy incidents and rollback paths are
kept outside the repository. Gaps and decisions: `docs/design/`. Options: `conf/playerbots.conf.dist` (every key the code
reads is listed there, and the code defaults must equal the `.dist` values: the server never reads `.dist` files).

## Sessions and threading

- Bots are core **headless sessions** (`WorldSession::SetHeadless(packetSink)`, `InitializeSession(onInitialized)`,
  `LoginHeadlessPlayer(guid)`): no socket, not in World's session map, updated by `PlayerbotMgr`. The packet sink runs on the thread
  that sends the packet (usually a map thread): parse there, queue the answer, run it on the world thread (`World/WorldActionQueue`).
  Random bots read dungeon finder proposals (`ActivityMgr::OnBotPacket`) and loot rolls (`LootRolls::OnBotPacket`); party bots only loot rolls.
- AI ticks: one `WorldMapScript` per world map (map thread), `WorldScript::OnUpdate` for dungeons and battlegrounds.
- Frozen bots use `WorldSession::SetDormant`: the core skips `Player::Update` and the cell visits around them. Frozen bots `LoadGrid` and
  snap Z from the **current Z** (not `MAX_HEIGHT`: that is the mountain roof over Ironforge). Party bots skip the snap and keep the master's Z.
  `.appear` onto a sky-snapped bot drops you through the world.
- Bots complete teleport handshakes themselves (`HandleMoveTeleportAck`, `HandleSuspendTokenResponse`, `HandleMoveWorldportAck`).
- **Sight:** `Player::CanNeverSee` hides every object from a player without `PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME`, a flag only a
  real client sets. `PlayerbotAI::Update` (active branch) sets it and calls `UpdateObjectVisibility(false)` once. Without it a bot has no
  target, attack or cast. `.bot state` shows `sighted yes|NO (blind)`.
- NPC interaction: modules calling gossip, quest accept or reward directly skip what the real handlers do. `BeginInteraction`
  (`Strategies/Rpg/NpcDispatch.cpp`) targets and faces the NPC and pauses a walking NPC for the visit (the core's own pause is minutes long).
- Header layout edits (`PlayerbotAI.h`, `RpgInfo.h`, anything inside a `PlayerbotAI` member) can leave a mixed-layout binary when a
  compile straddles the edit: touch the headers, rebuild dependents, install.
- A header in a module subdirectory with the same name as a core header wins the include search: check names before adding files
  (`BotSocialMgr` is named so it does not shadow `SocialMgr.h`).

## Engine and chat

- `src/Engine`: `Engine` (baskets by relevance, multipliers, prerequisites/alternatives, `ApplyStrategyString`, `ExecuteAction`), `AiContext`
  (per bot, creates actions/triggers/values from `AiRegistry` by `name::qualifier`, `BeginTick` drives per-tick caches), `Trigger.h`,
  `Value.h`, `Multiplier.h` (free of core types). Generic strategies are trigger -> action offers registered in `RegisterGenericAi()`
  (`GenericStrategies.cpp`). Default relevance: Revive 100 > Teleport 90 > Heal 80 > Consume 75 > Attack 60 > Loot 50 > Pvp 30 > Quest 20 > Idle 10.
- Triggers: `TriggerNode.States` (combat/non-combat), `IntervalMode::Cache|Gate` with jitter, `CalculatedValue::LastChangedMs()`,
  `Action::GetContinuers()` (offered once on the next tick, 3 s expiry), `Strategy::OnStateChanged`, `AiContext::Raise` one-tick events
  (`event:<name>`, type `AiEvent`), `ContextStrategyTable` (strategies by map/place/group/master, applied by `UpdateContextStrategies`).
- Dynamic tick (`Playerbots.DynamicTick.*`, `TickPolicy.h`): combat, idle and dead delays; actions call `ai.RequestDelay(ms)`; a state
  change or chat line wakes the bot. Cast supervisor (`Playerbots.CastSupervisor.Enable`, `CastPolicy.h`) waits for a cast and cancels it
  when the target died or a heal would hit a full target.
- Chat: `Cmd/ChatParser` (@filters, `;` chains, longest-name parse), `Cmd/ChatCommands.cpp` (each command an action `cmd <name>`),
  `HandleBotChat` is the single entry from both `OnChat` hooks. Commands only come from the owner (`GetMasterGuid()`; for a random bot the
  human group leader). Item commands (`ItemCommands.cpp`: equip, unequip, use, sell, buy, destroy) and `spec`, `maintenance`, `cs` are
  `WhisperOnly` while `Command.Prefix` is empty. `Cmd/CommandText.h` is pure: item link parsing reads only the id after `Hitem:`.
  Whispers that are not commands go to `Playerbots::SetWhisperFallback` (mod-ollama) before "Unknown command" / the random bot hint.
- Custom strategies (`cs <name> <trigger> > <action>...`, `co +custom::<name>`): stored per bot in `playerbots_db_store` as `cs:<name>`
  (value column 255 chars, about 5 lines), relevance 62. Only bots of a player restore their strategy list.
- `.bot add a,b,c` and `.bot add *`. `.bot state <name>` explains why a bot does or does not act: flags, strategies, rpg status and target,
  last action, per-attacker validity and a `nearby:` rejection line per hostile creature.

## Data and placement

- `data/sql/db-characters/playerbots_00_schema.sql` creates AzerothCore's `playerbots_*` tables in the `characters` DB (a separate DB
  is impossible from a module); `_01_names` seeds `playerbots_names` (`gender` = race group*2+gender, not a gender). Access only through
  `Db/PlayerbotRepository`: read once in `WorldScript::OnStartup`, async write-behind. Used tables: `playerbots_db_store`,
  `playerbots_random_bots`, `playerbots_account_type`, `playerbots_names`, `playerbots_quest_blacklist`; the rest are unused.
- Module SQL files must have names unique across all update directories and be idempotent.
- Placement: `RandomBotMgr::Prepare` re-places every random bot once per `PlacementVersion` (bump it to force a reset at next login).
  `PlayerbotAI::DetectInterruption` (>60 yd or map change, not on a taxi) -> `OnInterrupted` (rpg idle, motion clear, QuestTravel drops its
  plan, home moves via `RandomBotMgr::UpdateHome`). Expected own teleports call `NoteExpectedMove()`.
- Hotspots: `Travel/HubClusters` + `HubIndex` (service clusters with tiers), `Travel/Hotspots` + `HotspotMgr` (occupancy every 5 s, markers),
  marker creatures 9100001-9100205, world tables `playerbots_hotspot_marker` and `playerbots_hotspot_override` (`enabled = 2` forces a
  town in, 0 removes, 1 only retunes), `Playerbots.Hotspot.AutoTiers` (default 1 = capitals). `Travel/PhaseGuard`: a usable spawn phase
  is 0, 169 or a shared phase (in `phase_area`, no type-26 `conditions` row).
- Random bots skip Demon Hunter and Evoker (they need a high-level character on the account). Dummy/test spells and auras with missing
  triggers are stripped after GiveLevel and gear (a log flood stalled the world thread and character creation timed out). GiveLevel is
  one bot per world tick.

## Travel and movement

- Mmaps only: no bee-line or VMAP-step fallback and no Z-snap teleports. `Travel/BotMover` (`CanMove`, `SnapToMesh`, `MoveCombat`,
  `MoveHeldAbove`, pure `MovePriority.h`: None < Travel < Follow < Chase < Position < Avoid) over `MoveAlongMesh` (`MoveToward.cpp`).
  Boarding a transport while following may use a short non-mesh deck step (the only exception).
- `Travel/Graph/TravelGraph` (pure A*), `TravelMgr` (graph from hub clusters, taxi network, portals, 6-nearest walking links, temporary
  hearthstone link, blocked links expire after 5 min and double per failure), `RouteFollower` (per-bot legs walk/taxi/portal/hearth, 3
  re-plans, virtual travel for frozen bots), `TakeTaxi` (`Travel/Taxi.cpp`), `DangerMap` (128 yd cells, creature level per team),
  enemy towns closed to a team (`TravelNode.Team`, `IsHostileGround`). Boats and zeppelins are Transport links; nobody boards, real mode
  teleports to the other dock. GM `.bot travel info|nodes|route`.
- Move fallbacks (`Playerbots.Move.Fallbacks`, `MoveGeometry.h`): other floors from `Map::GetHeight`, then cone-sampled stepping stones.
  NPC stand-off: `MoveAlongMesh(bot, dest, arrive, standoff)` trims the path (`TrimPathToStandoff`); `Playerbots.Rpg.NpcStandOff`.
- Path checks: `Playerbots.Move.LogBlockedPaths` (measure), `AvoidBlockedPaths` (`PathRepair`: detour points beside the first blocked
  segment, cliff paths refused), `HopLowObstacles` (a real player-style jump through `Travel/PlayerJump`: the move update must carry
  `MovementInfo::guid`, which only a client packet fills, so the module sets it). `Travel/StuckDiagnosis` (`DescribeStuck`) is appended to
  the `server.playerbots.stuck` log lines.
- Frozen branch switches gravity off (hover animation); the unfreeze keys on the module's own `_gravityOff` flag, not `IsGravityDisabled()`.

## Rpg, quests and activity

- Near bots run the AC-style status loop (Idle -> weighted pick -> chained statuses; weights from AC); the goal table
  (`GoalStore::TryBegin`) only gives near bots the kinds in `Playerbots.Goal.NearKinds` (default `town,event,bg`); far bots get every
  kind. `StatusPick.h` is the shared weighted roll. `DoQuest` with an empty log and `DoGrind` with no ground re-roll another status.
- Far bots (`Playerbots.Abstract.*`, `Strategies/Rpg/AbstractLife`): timed statuses without pathing (DoQuest, WanderNpc, GoGrind,
  GoCamp, Rest, WanderRandom), `QuestTravel::AbstractXp` pays XP. Far bots are strict and cheap, near bots player-like.
- `ChooseNpcTarget` takes any NPC scoring at least half of the best (spreads bots out), a walking NPC scores 20 less. Auctioneers score
  only when the bot has something postable. Rare watch: a bot looks at a rare at most `RareInterestMs`, then ignores it for `RareIgnoreMs`;
  evading or invisible rares are skipped.
- `QuestTravel`: selects the cheapest giver by `TravelMgr::ReachFor` route cost + random + 60 s per bot already on the quest; kill quests
  only; `PhaseGuard::BlockQuest` blacklists quests whose phase changes around accept or reward.
- `ActivityMgr`: bots only fill queues of online humans (LFG roles, BG teams, arena opposing group). Random bots only log in while a human
  is online. A human group leader counts as the bot's temporary master.
- Groups: `Combat/GroupTactics.h` + `Positioning.h` (pure) with `Combat/GroupCombat` (roles, attackers, `threat` and `wait for attack`
  multipliers, positioning and kiting), `Playerbots.Follow.*`, `Social/GroupEvents` (ready check, trade, quest share),
  `Social/LootRolls` (`Playerbots.Loot.Roll`; `LootRollRules.h` chooses need/greed/pass, the vote goes through `LootRoll::PlayerVote`).
- `unseen attacker` -> `escape` (`Playerbots.Combat.EscapeUnseen`), `combat stuck` -> `drop target` (`Combat.StuckSeconds`).
- Dungeons: `Dungeon/EncounterKit` (`DungeonDef`, `RegisterDungeon`, target rules, helpers `StepAway`, `StepBehind`, `FirstAttacker`),
  one file per dungeon with only the mechanics, enabled by `Playerbots.Dungeon.Enable`. 14 dungeons (spells, entries and objects
  checked against the client data and world DB; several stand spots are AzerothCore coordinates). Not done: Mechanar, Sethekk Halls,
  Violet Hold, Magisters' Terrace, Oculus, Trial of the Champion.
- Social: `BotSocialMgr` (invites and whispers to random bots, `PartyDeclineReason` for `Playerbots.Random.SayWhyNot`), `BotGuildMgr`
  (`Playerbots.Guild.*`, bot guilds are guilds led by a random-bot character, `.bot guild list|purge` to undo, never delete guild rows
  by SQL while the server runs; names from `GuildNames.h`), `BotHire` (`hire` whisper command, `Playerbots.Hire.*`, mirrors
  `.character changeaccount`, gold refunded on failure).

## Gear, economy and rotations

- `GearFactory` (`Supply` and `Provision` every 60 s for random bots): food, potions (`Gear.StockPotions`; `Economy/PotionRules`
  classifies from the on-use spell and is shared with `Combat.UsePotions`), faction mount, bags and pocket money, hunter pets
  (`Gear.HunterPets`), professions (`BotProfessions`: skill on the root skill line), `Gear.Variety` and `Gear.RerollVersion`,
  `EquipBagUpgrade`. `IsStocked` stops bots selling or posting what they were given. `maintenance` for owner bots does only repair and food.
- `Economy/MountVariety` (`Playerbots.Mounts.Variety`): ground mounts the bot meets the PlayerCondition for, chosen by guid hash, taught
  with `LearnSpell`. `Economy/FlyingMounts` + `Travel/AirCorridor`/`AirProfile` are the air legs work in progress.
- Rotations: `data/rotations/*.rot` (39 specs, format in its README), parser `Rotation/RotationScript` (pure), runner
  `Rotation/RotationRunner`, `.bot rotation reload|info`, `Playerbots.Rotation.Enable|Path|Utility`. The `utility` list and the group
  targets are inert until `Rotation.Utility = 1`. Spell names are checked against the client `SpellName`.
- Tests: `tests/run_engine_test.sh` builds and runs every standalone test with g++ only (no TrinityCore build).

## Performance

- `Engine/PerfMonitor` (`.bot perf`, `Playerbots.PerfMon.Enable`), `Perf/BotPerfReporter`, `Perf/GateCounters`. Opt-in gates, off by
  default: `FarBotNoVisibilityLoad` (uses `SetHeadlessLoadsVisibilityGrids`), `SleepElevators`, the `BotActiveAlone` share (0) with its
  zone/radius caps. `BotActiveAloneSmartScale` is on by default and only matters once the share is above 0 (slowest tick of the last 10 s). AI ticks are staggered per bot; `IsSimulated` caches the
  humans-proximity verdict 500 ms; combat target selection is cached per tick.
- Lazy navmesh loading was dropped for good (it saved memory but raised CPU).

## Operations gotchas

- `Playerbots.X = 1  # comment` is a "Bad value" in this config format: comments go on their own line.
- Server.log is replaced on container restart: a "World initialized in" line already present belongs to the previous run.
- New module `.cpp` files need `cmake .` before `make`. `make modules` builds only the module.
- A docker-style deployment keeps its own copy of the conf: a new `Playerbots.*` key must be added there too or the C++ default is used.
- Never delete bot guild or character rows by SQL while the server runs (caches).
- C2 map part, C3, C4 written 2026-10-19 (built, unit tests pass, NOT installed, NEVER flown in game): `Travel/AirCorridor.{h,cpp}` (`PlanLeg`: grid-load cap `Air.MaxGridLoads`, terrain probed from 900 yd above the higher end, every sample must be in an area with `AllowFlyingMounts`, profile from `AirProfile.h`, static answers cached per pair of ends, "grids not loaded" is `Transient` and never cached), `Travel/NavSnap.{h,cpp}` (`SnapPointToNavMesh`: nearest ground navmesh point via Detour, no path needed), `LinkType::Air` (TravelGraph, `RouteOptions::AllowAir`, `TravelMgr::AddAirLinks` between towns/flight masters of maps 0 and 1, 400-3000 yd, cost = 3D distance / 22 yd/s + 12 s; `Summary` counted types[5], now 6), `OptionsFor` sets AllowAir only for a bot on `Air.TestBots` with riding >= 225 and a flying mount in its collection, `PlanFor(.., noAir)`. `RouteFollower::StepAir`: walk to the takeoff spot, mount a flyer (3 tries; adds aura 404468), check `CanFly`, snap the landing, plan the corridor, one `MoveSplineInit` fly spline (`SetFly`, `SetUncompressed`, flight speed), poll, land (mount off). Any refusal sets `_noAir` and re-plans without air; the global link block is NOT used. `RecoverAir` (combat, dismount, lost flight, death, 6 min, route cancelled, frozen in the air) teleports to the nearer of takeoff/landing and logs under `server.playerbots.air`. `PlayerbotAI`: `RememberStand`, `DetectInterruption`, `IsHoveringOverVoid`, `SnapToGround` skip while `_route.InAir()`; `Update` recovers an orphaned flight first. Test commands: `.bot air probe|grant <bot>`, `.bot air fly <bot> <node>` (node ids: `.bot travel nodes <text>`). Keys (all default off): `Air.Enable`, `Air.TestBots`, `Air.MinLegYards`, `Air.MaxLegYards`, `Air.Neighbours`, `Air.MaxGridLoads`; links are built at startup, so enabling needs a restart.
- Known gaps: a leg whose takeoff/landing grid is not loaded may stop at "landing point is not on the navmesh"; party bots following a flying master are not covered; frozen bots finish air legs virtually (teleport to the node) like any leg; no automatic riding/mount grants for random bots; the core's per-mount hybrid choice (skyriding vs steady) is only assumed from the aura, probe v2 not yet run.
- `Gear.RerollVersion` destroys the worn items and cannot be undone per bot: take a characters backup before raising it.
- Hire: take a characters backup before first use. `Hire.Enable` stays 0 in a deployment until the owner turns it on. Undo a completed
  hire with `.character changeaccount <name> #<old account id>` (the id is in the `moved from account X to account Y` log line), then restart.
- Arena teams are not possible: 12.1 has no `CMSG_ARENA_TEAM_*` handlers.
- Air follow (user decisions 2026-10-19: every bot flies steady, party and raid bots follow a flying master, frozen bots keep their optimizations, auto-grant missing mounts, catch-up teleport): built, unit tests pass, NOT installed, never flown in game. `Travel/AirFollow.{h,cpp}` (`Playerbots.Air.Follow`, default 0): the follow action calls `AirFollow::Update` first. While the master is mounted and `IsFlying()` (not on a taxi or transport, same non-instance map) its positions are recorded as a trail (every 6 yd, 160 points); the bot gets onto a flying mount (`FlyingMounts::EnsureMounted`, grants riding 375 + a flyer when `Air.FollowGrant`, 4 tries then 5 min on the ground = ordinary follow), and every 0.9 s flies a fly spline to its place around the master (`FormationOffset`: 6-16 yd, behind, +-4 yd height; stable per bot guid) straight when within 60 yd and clear, else along the master's trail (`TrailAhead`, 25 yd spacing, 8 points, line-of-sight checked, 3 yd above terrain). More than `Air.FollowCatchUpYards` (150) behind for `FollowCatchUpSeconds` (20): `NearTeleportTo` behind the master, still mounted. When the master is down the bot descends onto a navmesh point 2-6 yd from it (`SnapPointToNavMesh`), checked within 3 yd, else once more, else placed; mount off. `AirFollow::EmergencyLand` (attack action): a bot in the air that is attacked descends to the navmesh point below it before `AttackAction` dismounts it (dismounting at height is a fall). `PlayerbotAI::InAir()` = route air leg or `AirFollow::Active`: the fall watchdog, ground snap, hover check and interruption check skip it. All mount casts of the module (`UpdateMountForTravel`, air legs, follow) first add the steady-flight style aura 404468 (`FlyingMounts::EnsureSteadyStyle`), so no bot resolves to skyriding. Pure geometry: `Travel/AirFollowGeometry.h`, `tests/air_follow_test.cpp`.
- Air follow review fixes (built, NOT installed): `AirFollow::Tick` runs first in `PlayerbotAI::Update` every tick: a bot in the air whose master is gone (no master, another map, an instance) or that froze comes down at once (`GroundNow`: descent onto the navmesh point below, a frozen bot is placed there, mount off); a `Boarding` stage keeps the mount try counter (4 tries, then 5 minutes of ordinary ground follow); a mount cast first stops a running spline (it would cancel the cast), in `FlyingMounts::EnsureMounted` and in `StepAir`; the bots land only after the master has been on the ground (alive, not falling, not on a taxi or transport) for 2.5 s, hover up to a minute if the master falls / is dead / rides a taxi, then land under themselves; a landing never places a bot at the master's raw position (navmesh point, else the ground probed from above).
- 2026-10-19 jump rework (built and installed, not running yet): (1) the threshold is the jump itself: every race jumps with the same speed (7.96 yd/s up, gravity 19.29: apex 1.64 yd), so an obstacle blocked at knee height but clear at 1.5 yd is jumped and only what blocks at 1.5 is gone round (`WaistHeight` 1.5 in `PathRepair.cpp`). (2) the hop is on the run: the path is launched whole, `MoveAlongMesh` schedules an event on the bot (`m_Events`) for the moment it reaches the obstacle (path distance / run speed, 80 ms early), and the jump lands one jump's reach (run speed x 0.824 s, 1.5 to 8 yd, mounted runs further) along the path, so the bot keeps its speed through the arc; the event does nothing if the bot is no longer running or is more than 3 yd from the obstacle. The first version cut the path at the obstacle and jumped after stopping, and a hop from a standstill landed toward the next path point, which is the wrong-direction jump the user saw.
- Walking NPCs (Thomas Miller): `DoWanderNpc` now keeps re-aiming at an NPC that walks (a new path when he has moved 4 yd from where the last was aimed, at most every 800 ms, by stopping the bot's spline so the mover plans again) and holds him still (`PauseMovement` 4 s) once the bot is within 8 yd, as a person stops him by talking to him. Before, the mover planned a path only when the last one had ended, so the bots trailed him. Sampled a live chase first: Bandamage targeting Thomas at 9.6 yd, NPC moving yes, spline running, no give-up logged.
