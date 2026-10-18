# mod-playerbots

AI-controlled player characters for **TrinityCore master (retail 12.1.x)**, built as a drop-in module
(`modules/mod-playerbots`). It is the retail counterpart of AzerothCore's
[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots): the engine model (strategies, triggers, actions) and the
idea of ownerless "random bots" are carried over, but the code is a retail-native rewrite, not a translation. The 3.3.5
class AI does not apply to the current client, so rotations, talents, travel, quests, gear and professions are rebuilt on
retail data (DB2, `phase_area`, taxi paths, ChrClasses, traits).

> **Read the requirements first.** The module needs a TrinityCore with the module framework (`modules/`, not upstream)
> and one small, generic "dormant player" hook. Everything else uses stock TrinityCore. Patches for those two are not
> published yet, see [Requirements](#requirements).

## What it is

Two features share one engine:

1. **Party bots.** A player logs in other characters of their own account as bots that join their group, follow, fight,
   heal and obey chat commands.
2. **Random bots.** Ownerless characters on their own Battle.net accounts that populate the world: they stand in capitals,
   quest, grind, travel by flight path and boat, use vendors, mailboxes and the auction house, and fill the queues of
   real players (dungeon finder, battlegrounds, arenas) so a small server does not feel empty.

Bots are real `Player` objects in a socketless `WorldSession`. They call the same handlers a client would, so they obey
normal game rules (cooldowns, reagents, loot rules, phasing) and show up everywhere a player does.

## What the bots can do

Status legend: **live** = deployed and exercised on a running server, **built** = implemented and compiling, but not yet
play-tested. It reflects the project notes as of 2026-10-16, so treat anything marked built as unproven.

### Party bots

| Feature | Status |
|---|---|
| `.bot add <name>` / `remove` / `list`; up to `Playerbots.MaxBotsPerPlayer` (4) per player, strategies survive relogs | live |
| Follow in formation, stay, teleport to you when far away or on another map (out of combat) | live |
| Assist / attack / passive; melee chase, ranged and healers keep range | live |
| Healers heal the lowest group member; resurrect in place; loot corpses; eat and drink when hurt or low on mana | live |
| Chat commands with filters and chains: `@tank follow ; @dps attack` (see below) | live |
| Spec rotations: 39 specs (all 13 classes) as `data/rotations/*.rot` scripts, SimulationCraft-style, hot-reload with `.bot rotation reload` | built |
| Starter-build talents on retail traits at creation and on level/spec change | built |
| `goto <place>`: travel by foot, flight path, boat and portal | built |

Chat commands (whisper a bot, or speak in party chat to all of them): `follow`, `stay`, `passive`, `assist`, `attack`,
`status`, `gear`, `autogear`, `inventory`, `spells`, `quests`, `summon`, `reset`, `co` / `nc` / `de` (combat, non-combat,
dead strategies: `+name,-name,~name,?`), `strategies`, `cast <spell>`, `goto <place>`, `leave`, `help [command]`.
Filters: `@tank @healer @dps @melee @ranged @caster`, class names, `@group2`, `@nearby`, `@far`, `@level60-70`, `@all`.

### Random bots (`Playerbots.Random.Enable`, off by default)

| Feature | Status |
|---|---|
| Account/character pool created on demand, staggered login with a faction split, first-login roll of level, spec and spot | live |
| Gear by slot for class, race and spec at the bot's level; class-armor-only filter; food and drink | live / built |
| City life: bots gather at capitals and towns, weighted toward auction house, bank, mailbox, inn; hand-placed marker NPCs for custom spots | live |
| Answer party and guild invitations from players and greet them; whisper hint | live |
| Fill dungeon-finder roles, battleground teams and the opposing side of arena queues of real players | built |
| Rpg layer: quests (kill quests, QuestPOI walking), grind, vendors (sell junk, repair, buy food), trainers, mailbox, flight masters, resting, emotes and greetings | built |
| Travel graph and routes across the world (walk links, taxi network, area-trigger portals, boats, hearthstone) with team and danger awareness | built |
| Professions and gathering (scenery), racial mounts and riding, auction posting, crafting at a forge | built |
| Phase safety: bots stay in the default and shared phases and never take phase-changing quests | live |
| Scale controls: bots only fully simulate near a player; far bots use cheap virtual travel; tick staggering; scan throttles | live |
| Optional persona, goal and memory tables (`Playerbots.Goal.*`, `Playerbots.Memory.*`) | built, off |

Cost: bots nobody can see are made dormant and simulated abstractly, so memory is dominated by the map grids and navmesh
tiles the bots stand in, not by the bots themselves. Earlier measurements (750 bots, about a third of one core and 9-10 GiB,
nobody online) were taken with core scale switches that have since been removed; the numbers have to be measured again.

### Not done

Trading, dungeon and raid mechanics (bots in a dungeon follow and fight, nothing more), pet control, per-encounter
cooldown planning, battleground objectives (flags, nodes; `Playerbots.Bg.Objectives` is off), bank deposits and a
dedicated playerbots database (a module cannot open a fifth connection pool, so the `playerbots_*` tables live in the
`characters` database).

## Requirements

- A TrinityCore **fork based on master `34fd0ebd`** (12.1.5, client build 70391) with the two core changes below.
  Bot sessions, login, ticking, invites and the dungeon finder all run on stock public API; see
  [docs/design/CORE_EXIT.md](docs/design/CORE_EXIT.md).
- MySQL/MariaDB with the usual four TrinityCore databases (`auth`, `characters`, `world`, `hotfixes`).
- A client-extracted `dbc` / `maps` / `vmaps` / **`mmaps`** set. Bots move on mmaps only, there is no fallback.
- g++ with C++20 (same as the core) for the build; the engine unit tests need only g++.

**Core changes the module depends on** (not upstream, patches are not published yet). Both are generic, any module can use them:

Paths are relative to the TrinityCore root.

| Part | Files | What it does |
|---|---|---|
| Module framework | `modules/CMakeLists.txt`, `modules/ModulesLoader.cpp.in.cmake`, `modules/ModulesScriptLoader.h`, `src/server/CMakeLists.txt`, `src/server/worldserver/CMakeLists.txt`, `src/server/worldserver/Main.cpp`, `src/server/database/Updater/DBUpdater.*`, `src/server/database/Updater/UpdateFetcher.*` | builds `modules/*`, loads their scripts, applies `modules/*/data/sql/db-*` through the DB updater |
| Dormant players | `src/server/game/Server/WorldSession.h/.cpp` (`SetDormant`/`IsDormant`), `src/server/game/Entities/Player/Player.cpp`, `src/server/game/Maps/Map.cpp` (about 20 lines) | a script can mark a session dormant: its player stays in the world but skips `Unit::Update` and the map's cell visits. Nothing in the core sets it. The module uses it for bots nobody can see |

Without the dormant hook the module still runs, but every bot costs a full `Player::Update` and keeps its grids active.

## Installation

These steps assume the core with the two changes above is already checked out and building.

1. **Clone the module into the core's `modules/` directory.**

   ```sh
   cd <TrinityCore>/modules
   git clone https://github.com/Oreocide/mod-playerbots.git
   ```

2. **Re-run cmake and build.** The module list is globbed at configure time, so cmake has to run again after adding a
   module. The configure output lists `mod-playerbots` under "Module configuration".

   ```sh
   cd <build dir>
   cmake .
   make -j"$(nproc)" worldserver
   make install
   ```

   Disable it later without deleting it: `cmake -DMODULE_MOD_PLAYERBOTS=disabled .`

3. **Point the worldserver at the source tree.** Both the module's SQL files and the rotation scripts
   (`data/rotations/`) are read from `SourceDirectory`, not from the install prefix. In `worldserver.conf`:

   ```
   SourceDirectory = "<path to the TrinityCore checkout>"
   ```

   With Docker, mount the checkout into the container and set `SourceDirectory` to the mount point. If the directory is
   unreachable, the module's SQL is skipped without an error and the tables will be missing.

4. **Install the configuration.** The server loads `*.conf` files from `worldserver.conf.d` next to `worldserver.conf`
   and never reads `*.conf.dist`:

   ```sh
   cp <TrinityCore>/modules/mod-playerbots/conf/playerbots.conf.dist \
      <config dir>/worldserver.conf.d/playerbots.conf
   ```

   The file keeps its `[worldserver]` header. `make install` also copies the `.dist` to the install prefix, but a Docker
   container reads its own config directory, so copy it there. Copy the whole file and edit values in place: a key that is
   missing from your `.conf` silently falls back to the compiled default, which is not guaranteed to match the `.dist`.

5. **Start the worldserver.** The DB updater applies the module's SQL and records it in `updates`:

   | Database | What |
   |---|---|
   | `auth` | RBAC permission 1010 (`.bot`) linked to role 199, "Player Commands" |
   | `characters` | the `playerbots_*` and `ai_playerbot_texts*` tables, a 100,000-entry name pool, persona and memory tables |
   | `world` | hotspot marker NPCs (creature entries 9100001+), `playerbots_hotspot_marker`, `playerbots_hotspot_override`, `playerbots_goal`, `playerbots_persona` |

   Permission ids must stay below 1024. If another custom module already uses 1010, change it in
   `data/sql/db-auth/` and in `src/Script/PlayerbotCommandScript.cpp` together.

   Look for `Playerbots:` lines in `Server.log`, including the number of rotations loaded
   ("N rotations from M files (K problems)").

6. **Try it.** Log in with a character that has at least one more character on the same account and run `.bot add <name>`.
   The bot joins your group; whisper it `follow`, `attack` or `help`.

### Enabling random bots

Random bots are off by default. Back up your `characters` and `auth` databases first, because this creates accounts and
characters.

```
Playerbots.Random.Enable = 1
Playerbots.Random.Count = 200
Playerbots.Random.MinLevel = 1
Playerbots.Random.MaxLevel = 90
Playerbots.Random.OnlyWithPlayers = 1
```

What the first start creates:

- Battle.net accounts `RNDBOT0001@PLAYERBOTS.LOCAL`, `RNDBOT0002@...` (prefix and domain are configurable), each holding
  10 game accounts (`Playerbots.Random.GameAccountsPerEmail`). Passwords are random and never stored. 1000 bots need
  about 25 game accounts. Keep the account and per-realm character limits (`CharactersPerAccount`,
  `CharactersPerRealm`) high enough.
- Characters with names from the 100,000-name pool, created in the background and levelled one per world tick, then
  geared and placed on an outdoor spot or at a city hotspot. Demon hunters and evokers are skipped.
- With `OnlyWithPlayers = 1` (default) bots wait 30 seconds after a real player logs in and log out five minutes after the
  last one leaves.

Start small. `Playerbots.BotActiveAlone = 0` (default) keeps bots that nobody can see frozen, which is what makes hundreds of
bots affordable; raising it wakes grids across the world. See [docs/REFERENCE.md](docs/REFERENCE.md) for every scale option.

### Log noise

Bots have no client, so the core would log a line at ERROR level for every packet it sends them ("Prevented sending ... to
non existent socket"). `conf/playerbots.conf.dist` ships `Logger.network.opcode=0,Server`, which turns that category off (it
also hides the same message for real players). Every bot login still writes two harmless lines ("Account ... can't login with
that character", the stock login handler is used without a character list). Count the first kind with
`grep -c "Prevented sending" Server.log` if you want to check that the mute is active.

## Configuration

`conf/playerbots.conf.dist` documents all ~180 keys. The groups:

| Prefix | Controls |
|---|---|
| `Playerbots.Enable`, `MaxBotsPerPlayer`, `UpdateInterval`, `FollowDistance` | basics and party bots |
| `Playerbots.Random.*` | population size, levels, accounts, gear, quests, queues, invitations |
| `Playerbots.Rpg.*` | job weights and durations, vendors, trainers, mailbox, emotes |
| `Playerbots.Hotspot.*`, `Playerbots.Population.*` | where bots gather and how they spread over levels |
| `Playerbots.Travel.*`, `Playerbots.Quest.*` | routes and quest selection |
| `Playerbots.BotActiveAlone.*`, `Warm.*`, scale switches | simulation radius and cost |
| `Playerbots.Rotation.*` | rotation scripts and their path |
| `Playerbots.Remote.*` | optional authenticated TCP control port (off, localhost) |
| `Playerbots.PerfMon.*` | timing report (`.bot perf`) |

GM commands (`.bot`, RBAC debug permission for most): `perf`, `rpg`, `autogear`, `goal`, `inspect`, `hotspot list|go`,
`travel info|nodes|route`, `rotation reload|info`.

## Development

- Layout: `src/Engine` (strategies, triggers, actions, values), `src/Cmd` (chat), `src/Strategies` (generic, quest, rpg),
  `src/Travel` (hubs, graph, routes, phase guard), `src/Rotation`, `src/Economy`, `src/Social`, `src/Spawn`, `src/Db`,
  `src/Perf`, `data/rotations`, `data/sql`.
- Standalone tests (no TrinityCore build needed, only `Define.h` headers): `TRINITY_SRC=<TrinityCore> tests/run_engine_test.sh`.
- Design notes and per-subsystem details: [docs/REFERENCE.md](docs/REFERENCE.md) and [NOTES.md](NOTES.md). Roadmaps, plans
  and test plans are in [docs/design/](docs/design/).
- Keep module header names unique; all modules share one include path with the core, and a header called like a core header
  shadows it.

## License and credits

GNU GPL v2 or later, see [COPYING](COPYING). Source files carry the TrinityCore header. Parts follow AzerothCore's
mod-playerbots (also GPL v2 or later): the engine model, the random-bot population ideas, the NewRpg status model
(`src/Strategies/Rpg/RpgInfo.h`) and the name pool in `data/sql/db-characters/playerbots_01_names.sql`.
[docs/design/PROVENANCE.md](docs/design/PROVENANCE.md) lists where each part came from.
