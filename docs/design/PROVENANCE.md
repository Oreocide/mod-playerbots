# Playerbots: provenance

## License position

- **AzerothCore mod-playerbots is GPL-2.0-or-later** (H). Evidence:
  - `azerothcore-wotlk/modules/mod-playerbots/LICENSE` is GPL v2;
  - 1,394 of 1,407 source files carry one identical "GNU GPL v2 … or any later version" header;
  - 0 Affero/AGPL hits;
  - lineage per `AUTHORS.md` is ike3 → cmangos playerbots → mod-playerbots.
- TrinityCore is GPL-2.0-or-later (file headers, e.g. `src/server/game/Entities/Player/Player.cpp`). **The licenses are compatible. Adapting code is allowed.** The brief's AGPL "no copy" rule does not trigger for mod-playerbots.
- **Do not copy:**
  - AGPL-headed AC core files: `Entities/Creature/Trainer.{h,cpp}`, `Server/Packets/NPCPackets.{h,cpp}`, `zone_teldrassil.cpp`, `zone_ironforge.cpp`, the local `TC9Sidecar/*`;
  - mod-ollama-chat (AGPL-3; its README says GPL-3, a conflict), mod-dungeon-clear (AGPL-3), mod-ah-bot-plus (AGPL-3 headers);
  - mod-bot-chat-intents, mod-bot-trade-chatter, mod-bot-spawners and mod-duel-anchors, which carry no license (all rights reserved). Use them as behavior references only.
- Project rule (CLAUDE.md): treat all borrowed logic as GPL2, and flag it if the project is ever redistributed publicly.

## Classes

- **original**: written for TC retail with no AC counterpart.
- **design-from-AC**: same concept or structure as an AC component, code written fresh against TC APIs (the module README says "retail-native rewrite, not a port").
- **adapted-from-AC**: AC code or data carried over and modified.

Evidence = the file's own comment that names AC, or NOTES.md. Git history is shallow and starts after the module existed, so "original" is M confidence unless a comment says otherwise. Every module source file carries the standard TC GPL-2+ header (H).

## Module files (`modules/mod-playerbots/`)

| Path | Class | Evidence | Conf |
|---|---|---|---|
| `src/Strategies/Rpg/RpgInfo.h` | adapted-from-AC | `:4` "Ported from AzerothCore mod-playerbots NewRpgInfo" | H |
| `src/Strategies/Rpg/RpgStrategy.cpp`, `NpcDispatch.*` | design-from-AC (NewRpg state machine, NPC dispatch scoring) | weights/timeouts match AC `conf:1507-1557`; NOTES.md | M |
| `src/Engine/*` (Engine, AiContext, AiRegistry, Trigger, Value, Multiplier, BotSpellBook) | design-from-AC | `src/Engine/Engine.h:29,38` "Same model as AzerothCore mod-playerbots" | H |
| `src/Engine/PerfMonitor.*` | design-from-AC | `src/Engine/PerfMonitor.h:30` "Counterpart of AzerothCore's PerfMonitor" | H |
| `src/RandomBotMgr.*` | design-from-AC (RandomPlayerbotMgr shape); retail account/char creation original | `src/RandomBotMgr.h:35`; `src/RandomBotMgr.cpp:1018` | H |
| `src/PlayerbotAI.*` | design-from-AC (AllowActive, ChangeToIdle after teleport); frozen tier original | `src/PlayerbotAI.h:67`; `src/PlayerbotAI.cpp:480` | H |
| `src/PlayerbotMgr.*`, `src/ActivityMgr.*` | design-from-AC (master bots, BG/LFG fill concept) | NOTES.md | M |
| `src/Db/PlayerbotRepository.*` | design-from-AC (repository; name category layout) | `src/Db/PlayerbotRepository.cpp:33` | H |
| `src/Travel/HubIndex.*`, `Hotspots.*`, `HotspotMgr.*` | design-from-AC (TravelMgr hubs, population hotspots) | `src/Travel/HubIndex.cpp:18`; `src/Travel/Hotspots.h:31` | H |
| `src/Travel/TravelMgr.*`, `DangerMap.*`, `Graph/*`, `RouteFollower.*` | design-from-AC (ideas from AC TravelNode: team-closed nodes, danger) | NOTES.md stage B | M |
| `src/Travel/PhaseGuard.*`, `HubClusters.*`, `MoveToward.*`, `Taxi.*` | original | no AC reference; retail phasing/DB2 | M |
| `src/Cmd/*` | design-from-AC (@filters, `;` chains) | NOTES.md phase 3 | M |
| `src/Social/*` | design-from-AC (invite/whisper replies) | NOTES.md | M |
| `src/GearFactory.*`, `src/Economy/*`, `src/Talents/*`, `src/Rotation/*`, `data/rotations/*` | original (retail items, professions, traits, rotations from Blizzard companion data) | NOTES.md phases 5-6; `trinitycore-tracking/follower-ai/follower_rotations.md` | M |
| `src/Script/*`, `src/PlayerbotsConfig.*`, `src/RemoteControl.*`, `src/Perf/*`, `src/playerbots_loader.cpp` | original | — | M |
| `data/sql/db-characters/playerbots_00_schema.sql` | adapted-from-AC (same table set as AC `acore_playerbots`) | module README `:40,53` | H |
| `data/sql/db-characters/playerbots_01_names.sql` | **adapted-from-AC data** (AC `characters/base/playerbots_names.sql`, names >12 chars dropped) | file header line 1; first rows identical (`Abennon`, `Abeo`, `Abeor`) | H |
| `data/sql/db-characters/playerbots_02_seed.sql` | adapted-from-AC (texts chance) | NOTES.md | M |
| `data/sql/db-world/2026_10_07_0{0,1}_*.sql`, `data/sql/db-auth/*` | original | — | H |
| `data/sql/db-world/ai_playerbot_texts*.sql`, `playerbots_*.sql` (AC table files) | adapted-from-AC (empty tables, schema only) | module README `:53` | M |

## Core files

The bot session patch, `PlayerScript::OnUpdate`, the modules framework and the BotPerf probes are original local changes on TC master. AzerothCore's equivalents (`WorldSession` bot flag, module loader) are design references only (M).

## v1 additions (planned)

| Planned file | Class | Source of behavior |
|---|---|---|
| `src/Travel/BotMover.*` | original | user requirement (mmaps-only); TC creature random-movement end-point trick |
| `src/Behavior/*` (profile store, overlays, service sequences) | design-from-AC | AC NewRpg weights/timeouts, NPC dispatch scores, jump chance (INVENTORY §1) |
| `src/Scheduler/*` | design-from-AC (AllowActive force rules, SmartScale) + original (per-map tickets) | AC `PlayerbotAI.cpp:4674,4870` |
| `src/Status/BotStatusBoard.*` | original | — |
| `src/World/WorldActionQueue.*` | original | THREADUNSAFE mail and auction steps; no AC queue |
| `src/Spawn/SpawnPolicy.*` | design-from-AC (enemy-zone/team filters, `AC:src/Bot/RandomPlayerbotMgr.cpp:1595-1741`) + original (graveyard conditions, DangerMap level fit) | IDENTITY §1 |
| GearFactory filter/score + `autogear` commands | design-from-AC (`AC:src/Ai/Base/Actions/TrainerAction.cpp:586-700`, `PlayerbotFactory::InitEquipment`, `StatsWeightCalculator`); armor rule original (`ChrClasses.ArmorTypeMask`) | IDENTITY §3 |
| `playerbots_race_mounts` seed | original (client Mount.db2 vendor rows); AC per-race lists **not** copied (they contain swapped factions) | IDENTITY §2 |
| `src/Bg/*` objective layer | design-from-AC (roles, `AC:BattleGroundTactics.cpp:1207-1275`); generic GO objective approach original | GROUP_CONTENT §1 |
| `src/Social/BotGuildMgr.*` | design-from-AC (`AC:src/Mgr/Guild/PlayerbotGuildMgr.cpp:26-247`) | GROUP_CONTENT §2 |
| `data/rotations/assisted_combat/*` (offline dumps) | client data reference (Blizzard DB2), used only to review `.rot` scripts | GROUP_CONTENT §3 |
| `src/Combat/HazardScan.*`, `SafeSpot.*` | design-from-AC (`AvoidAoeAction`, `AC:src/Ai/Base/Actions/MovementActions.cpp:1846-2245`) | GROUP_CONTENT §4 |

Rule for new files: add a row here when the file is created. Name the AC file:line it follows, or "original".
