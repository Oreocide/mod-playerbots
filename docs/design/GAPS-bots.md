# Playerbots v1: gaps

Every API or data that is missing or unsupported for v1, with the minimal change. Core paths are relative to `src/server/game/`. `mod:` = `modules/mod-playerbots/`. Confidence H/M/L.

Tracking pointer: the local `trinitycore-tracking/` folder (outside this repository) (project convention, CLAUDE.md "Tracking files").

## Repo hygiene / defects

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G1 | `Server/BotPerf.{h,cpp}` are untracked. 15 working-tree files include `BotPerf.h` (core + module). HEAD builds, but committing the tracked changes without these two files breaks a clean checkout | `git ls-files` = 0; `git grep BotPerf.h HEAD` = none | **no core edit** (user directive): decide keep/drop with the core-exit questions (CORE_EXIT.md) | H |
| G2 | The working tree removes the committed empty-customization guard in `ValidateAppearance`. 12.1.5 hangs on an empty choice list, which matters for bot character creation | `git diff HEAD -- Handlers/CharacterHandler.cpp` (removed lines near `:760`) | module-side: `FillCustomizations` never sends an empty choice list; no core edit | H |
| G3 | **Fixed in code 2026-10-16, pending the 4-thread soak (not deployed):** shared lock + copies; map-thread callers checked. Unlocked writes from map threads to `RandomBotMgr::_homes/_homeMaps/_anchors` | `mod:src/PlayerbotAI.cpp:542` → `UpdateHome`; `mod:src/Strategies/Rpg/RpgStrategy.cpp:82` → `ClearAnchor`; no mutex in `mod:src/RandomBotMgr.cpp` | world-thread queue for writes (module) | H missing lock / M real race |
| G4 | **Fixed in code 2026-10-16, pending the 4-thread soak (not deployed):** mutex. `PlayerbotRepository::_store` was unlocked | `mod:src/Db/PlayerbotRepository.cpp:163,176` | mutex (module) | H |
| G5 | ~~cross-map chat hooks~~ **Withdrawn.** CMSG_CHAT_MESSAGE_SAY/PARTY/WHISPER are THREADUNSAFE (`Server/Protocol/Opcodes.cpp:333-337`), so the `OnChat` hooks (`mod:src/Script/PlayerbotsScripts.cpp:141-185`) run on the world thread while no map thread runs. Re-check only if bots ever speak from their own AI tick (map thread) | — | none | H |

## Movement

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G6 | Skyriding lift unimplemented (`SMSG_MOVE_ADD_IMPULSE`); no air navmesh | memory "Skyriding impulse gap"; mmaps are ground-only | v1: ground mounts + taxi only. Core: separate project | H |
| G7 | `MoveSplineInit::MoveTo(generatePath=true)` silently makes a straight 2-point spline on NOPATH | `Movement/Spline/MoveSplineInit.cpp:293-303` | module rule: BotMover never calls it with a raw point. Core: none | H |
| G8 | `PathGenerator` caps at 74 polys / 74 points (~300 yd) | `Movement/PathGenerator.h:32-35` | module segments long paths. Core: none | H |
| G9 | No public "nearest navmesh point" API | `PathGenerator` has only private poly lookups; `GetActualEndPosition` `PathGenerator.h:80` is the usable proxy | module: path with `forceDest=false` and use the end point. Core (optional, later): expose `findNearestPoly` | M |
| G10 | mmaps freshness vs 12.1.5 maps unknown | `docker/data/mmaps` has 28,689 files; build version not recorded | soak reports NOPATH per tile → regenerate with `mmaps_generator` if needed | L |
| G11 | Module height helpers can choose a lower floor; existing Z-snap teleports | `mod:src/Travel/MoveToward.cpp:25-64`; `mod:src/PlayerbotAI.cpp:363-365,548-574`; `RpgStrategy.cpp:167-168` | replaced by BotMover rules (module) | H |
| G12 | `Playerbots.LazyMMap` gate (other workstream) loads a navmesh tile on first path, ~400 ms on the map thread | `Movement/PathGenerator.cpp:79`; `Maps/Map.h:284` | far (THROTTLED/PAUSED) bots never path; only ACTIVE bots near humans (tiles already loaded) | M |

## Interaction / services

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G13 | Handlers depend on interaction state set by earlier handlers | trainer `Handlers/NPCHandler.cpp:145-148`; mail `Handlers/MailHandler.cpp:357,379`; vendor `Handlers/ItemHandler.cpp:586` | module replays open → list → act → close. Core: none | H |
| G14 | THREADUNSAFE handlers (mail, AH browse/sell, gossip select) cannot be called from map threads | `Server/Protocol/Opcodes.cpp:178,192,522,698-702` | module world queue with GUID re-resolve. Core: none | H |
| G15 | No `Player::SellItem` API, only the handler | `Handlers/ItemHandler.cpp:381` | use the handler with a constructed `WorldPackets::Item::SellItem`. Core: none | H |
| G16 | The current innkeeper action only sends the binder UI; homebind never changes | `mod:src/Strategies/Rpg/NpcDispatch.cpp:155` | call `HandleBinderActivateOpcode` (`Handlers/NPCHandler.cpp:330`) (module, P1) | H |
| G17 | Vendor sell, trainer, mailbox, auction posting and vendor buying are built (2026-10-16). Mail and auction run on the world queue. Not play-tested | `mod:src/Strategies/Rpg/NpcDispatch.cpp`; `mod:src/World/WorldActionQueue.cpp` | acceptance is the two-capital sample in ROADMAP P3 | H |
| G18 | AH posting not modeled | AC also has none (`AC: NewRpgNpcDispatch.cpp:217` visit only) | visit + browse + post grey/white at vendor-price multiples (user decision); market from TC `AuctionHouseBot/` | H |
| G19 | Taxi nodes are unlocked artificially | `mod:src/Travel/Taxi.cpp:76-84` | **keep** (user decision: cheapest); not a gap anymore | H |

## Framework / core hooks

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G20 | `PlayerScript::OnUpdate` is a local patch, not upstream | `Scripting/ScriptMgr.h:793`; absent at upstream `34fd0ebd` | replace with `WorldMapScript::OnUpdate` per world map + world-thread tick for instance/BG bots; then remove the hook (CORE_EXIT.md) | H |
| G21 | Socketless bot sessions + `LoginBotPlayer` are local patches; merge-conflict hotspot | `Server/WorldSession.cpp` bot branches; `Handlers/CharacterHandler.cpp:1313` | **kept as the minimal core kit** (user decision); no further core lines without approval | H |
| G22 | Frozen bots cannot process queued THREADSAFE packets | `Maps/Map.cpp:749` skip; `WorldSessionFilter` | module rule: never queue client packets for frozen bots; call handshake handlers directly (already done `mod:src/PlayerbotAI.cpp:625`, `mod:src/PlayerbotMgr.cpp:358-362`) | M |
| G23 | No per-map/per-thread budget facility in core | — | module scheduler with world-thread tickets. Core: none | H |
| G24 | Frozen bots never auto-save (save timer is after the frozen return) | `Entities/Player/Player.cpp:897` vs `:1026` | module saves on pause/logout; stretch active saves via `SetSaveTimer` (`Entities/Player/Player.h:2665`) | H |

## Data / content

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G25 | Empty zones / missing service NPCs / partial phasing | world DB state; `HubIndex` returns null → idle (`RpgStrategy.cpp:483-489`) | fallback chain + cooldowns (module); not a core gap | H |
| G26 | Exile's Reach (NPE) for bots unsupported | `UseNPE` never set (`Server/Packets/CharacterPackets.h:67`) | out of scope; bots start at race start then are placed | H |
| G27 | Demon Hunter / Evoker bots need an existing high-level character on the account | NOTES.md (RandomBotMgr) | skipped in v1 | H |

## Identity (IDENTITY.md)

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G29 | Spawn/home faction holes S1-S10: graveyard conditions ignored, no `IsHostileGround` in `PickSpot`, `UpdateHome` unchecked, no login re-validation, team-less `HubIndex::Nearest` | IDENTITY §1 table | `SpawnPolicy` (module) | H |
| G30 | **Corrected 2026-10-16:** riding IS taught at trainers, under the learn-spell variants (33389 Apprentice, 33392 Journeyman, 34092 Expert, 90266 Master; the earlier query looked for 33388/33391/34090/90265, the spells they teach). The gap is that nobody visits a riding trainer: 36/585 bots ≥20 and 17/462 non-bot characters ≥10 have skill 762 | characters + world DB queries 2026-10-15 | bots: learn riding at 10 (module). Humans: add riding trainer data or a level-up grant (world DB, separate task; check the retail acquisition method first) | H |
| G31 | Mounts are granted by team, not race; plus account-wide collections on 2 mixed-faction bnet accounts make Horde bots learn the Alliance horse on every login | `mod:src/GearFactory.cpp:355-360`; core `Entities/Player/CollectionMgr.cpp:380-446`; DB queries | racial pick + race/faction-limited fallback; per-faction bnet accounts for new bots (module) | H |
| G32 | Racial mounts carry no race condition in `Mount.db2`, so race → mount needs curated data | Mount rows `PlayerConditionID` 0 (12.1.0) | seed script + manual review | H |
| G33 | Gear has no armor-type/primary-stat filter; no-ItemSpec items pass for any spec | `mod:src/GearFactory.cpp:150-206`; core `Globals/ObjectMgr.cpp:3325-3364` | `ArmorTypeMask` + primary stat + strict ItemSpec (module) | H |
| G34 | Retail item stat values depend on ilvl/bonus lists; scoring needs TC's stat calculation, not raw template stats | — | find + use the core API in P2b (module) | M |

## Group content (GROUP_CONTENT.md)

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G35 | BG fill stops at `GetMinPlayersPerTeam`, ≤6 bots/scan | `mod:src/ActivityMgr.cpp:491-497,60` | fill to max team size (module) | H |
| G36 | No BG objective behavior; in-place resurrect | `mod:src/Strategies/GenericStrategies.cpp:322-354,570-587` | generic objective layer on CAPTURE_POINT/NEW_FLAG GOs (module) | H |
| G37 | No bot guild creation | `mod:src/Social/BotSocialMgr.cpp` only accepts invites | `BotGuildMgr` using `Guild::Create` (core `Guilds/Guild.h:748`) (module) | H |
| G38 | Assisted Combat DB2s not loaded by core | core `DataStores/DB2Metadata.h:1433-1479` metadata only | not needed: user chose `.rot` scripts; Assisted Combat used as offline reference (wowdata dumps) | H |
| G39 | `AssistedCombatRule.ConditionType` meaning undocumented (enum in client binary) | 3116 rows, ~70 types | only matters for the review pass; read the spell-ID conditions, ignore the rest | L |
| G40 | AreaTrigger point-in-shape is protected | core `Entities/AreaTrigger/AreaTrigger.h:223,249`; `.cpp:761-942` | **module approximation (user decision)**: exact static shapes from public `ShapeData`, moving triggers by max search radius; no core change | H |
| G41 | Scripted hazards (`AreaTriggerAI::OnUnitEnter`, 130 uses) have no data marking them harmful | core `AI/CoreAI/AreaTriggerAI.h:56` | default "hostile caster ⇒ hazard" + encounter hint table | M |
| G42 | Local client data is 12.1.0 (69933), the server is 12.1.5 | `wowdata` profile reports build 69933 | re-extract/re-check data for AssistedCombat, Mount, ChrClasses when the client updates | M |
| G43 | Melee does not get behind targets; no ranged spread | `mod:src/PlayerbotAI.cpp:826-836` | role positioning with `MoveChase(target, range, angle)` (module) | H |

## Licensing

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G28 | Chat modules for later (mod-ollama-chat) are AGPL-3. Linking them makes the binary AGPL-3, including the network-source duty. Unlicensed AC bot modules cannot be copied | INVENTORY §2 | DECIDED 2026-10-17: ported as the separate module mod-ollama under AGPL-3 (own private repo, never pushed); the network-source duty applies if it is ever distributed | H |

## AC port (2026-10-18, plan: AC `src/Ai` decision + movement logic, native port)

| ID | Gap | Evidence | Minimal change | Conf |
|---|---|---|---|---|
| G44 | Bots could not learn that a loot roll started (`Loot::_rolls` private, packets to bots dropped). **Resolved 2026-10-18 without a roll-specific core hook:** the core now has generic headless sessions with a packet sink (`WorldSession::SetHeadless`, see `mod:src/BotSession.h`), so a bot sees `SMSG_START_LOOT_ROLL` (loot object + slot). Still to do in the module (phase 7): party bots are created in `PlayerbotMgr::AddBot` with a nullptr sink (give them one, NOT through `ActivityMgr::OnBotPacket`, which would auto-accept LFG proposals); the sink runs on the sending (map) thread: parse there, queue `WorldSession::HandleLootRoll` through `WorldActionQueue`. The user's 2026-10-18 permission for a temporary loot-roll core hook is therefore not needed | `Loot/Loot.h:247-277,355`; `mod:src/BotSession.h` | module only | H |
| G45 | Following a master onto a transport (boat/zeppelin/elevator) needs a deck move that is not a mesh path (AC `FollowActions.cpp:~100-170`: `MovePoint(generatePath=false)` + `AddPassenger`) | `AC: Ai/Base/Actions/FollowActions.cpp` | **User allowed an exception to mmaps-only for boarding (2026-10-18)**: only the last few yards from the dock/deck entry point onto a `Transport`, never off a transport, never for far bots (they use virtual travel). Document the exception in BEHAVIORS.md Movement | M |
| G46 | `BotMover` was designed but never built; the mesh mover was `MoveAlongMesh` only (travel-tuned: `AttackStop`, 5 yd arrival, 5 yd progress, no relaunch while moving) | `mod:src/Travel/MoveToward.cpp:213` | **Built 2026-10-18** (module only, not deployed, not play-tested): `Travel/BotMover.{h,cpp}` (`CanMove`, `SnapToMesh` = G9 workaround, `MoveCombat`, `MoveHeldAbove`) + pure `Travel/MovePriority.h` (tested in `tests/bot_mover_test.cpp`); `PlayerbotAI::Chase/Follow` do not override a held higher-priority move | H |
