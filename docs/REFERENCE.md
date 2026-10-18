# mod-playerbots reference (TrinityCore retail)

Deep-dive notes on how each subsystem works. For an overview and installation see the top-level [README](../README.md).

Lets a player log in other characters of their own account as AI-controlled party members,
in the spirit of AzerothCore's mod-playerbots. This is a retail-native rewrite, not a port: the
3.3.5 class AI does not apply to the current client, only the engine model (strategies -> actions,
highest relevance wins) and the bot session approach are carried over.

## Usage

| Command | |
|---|---|
| `.bot add <name>` | log in a character of your account as bot, it joins your group |
| `.bot remove [name]` | log out one bot, or all of them |
| `.bot list` | your bots |

Whisper a bot (or write in party chat for all of them): `follow`, `stay`, `attack` (your target),
`assist` / `passive`, `status`, `leave`, `help`.

## What bots do

- follow you in formation, teleport to you when far away or on another map (out of combat)
- assist: fight what you fight / what attacks the group; melee specs chase, ranged specs and healers keep range
- healers heal the group member with the lowest health, other bots heal themselves when low
- resurrect in place when you are out of combat (or accept resurrection spells)
- loot corpses within 25 yards after combat (gold and everything they can store; group loot rules still apply)
- eat and drink food from their bags when hurt (< 70% health) or low on mana (< 50%), while you stand still

Spells come from the per-specialization rotation scripts in `data/rotations/` (all 13 classes, 39 specs, see "Spec AI"
below). A specialization without a script, or `Playerbots.Rotation.Enable = 0`, falls back to a generic picker that
uses the bot's damage / heal spells (cooldown <= 2 min, no reagents).

## Not done yet

Trading, dungeon/raid mechanics (bots in a dungeon follow and fight, nothing more), pet control, and a separate
playerbots database (a module cannot open a fifth database connection pool, so the tables live in the `characters`
database, see below). Battleground objective play (`Playerbots.Bg.Objectives`), auction posting beyond the basics and
bank deposits are early or off by default. See the README for the current status of each feature.

## Database

`data/sql/db-characters/` creates the same tables as AzerothCore's `acore_playerbots` database
(`playerbots_*`, `ai_playerbot_texts*`) inside the `characters` database, applied by the normal DB updater
at startup. Everything is read once at startup and written asynchronously (`src/Db/PlayerbotRepository`).

| Table | State |
|---|---|
| `playerbots_db_store` | **used**: per bot `strategies` (bots of a player: follow/stay/combat survive relogs), `home` (random bots) |
| `playerbots_random_bots` | **used**: event table, `randomize` = level/spec/spot roll done (before, inferred from the level) |
| `playerbots_account_type` | **used**: random bot game accounts are marked type 1 (identification is still the email prefix) |
| `playerbots_names` | **used**: name pool per race group and gender (100,000 names), `Playerbots.Random.NamePool`; falls back to NameGen |
| `playerbots_quest_blacklist` | **used**: quest ids bots never pick (empty by default) |
| `playerbots_guild_names`, `playerbots_arena_team_names` | seeded (400 / 300), no consumer yet |
| `ai_playerbot_texts_chance` | seeded (3 rows), no consumer yet |
| `ai_playerbot_texts`, `playerbots_account_links/keys`, `playerbots_custom_strategy`, `playerbots_guild_tasks`, `playerbots_preferred_mounts`, `playerbots_enchants`, `playerbots_weightscales(+_data)`, `playerbots_bis_gear`, `playerbots_dungeon_suggestion_*`, `playerbots_travelnode*` | created empty, not used yet. AzerothCore's seed data for these is 3.3.5 specific (item, quest, map ids, the 1.4M row travel graph) and is not copied |

`playerbots_names.gender` is a name category (race group * 2 + gender: 0/1 human, undead and every race without
its own list, then gnome, dwarf, night elf, draenei, orc, troll, tauren, blood elf).
Random bots keep their strategy set from the configuration, only their `home` is stored.

## Random bots

Random bots (`Playerbots.Random.Enable`) are ownerless characters on their own
Battle.net accounts. The population manager follows AzerothCore mod-playerbots:
reuse characters, log them in staggered with an alliance/horde split, and on
first login roll a level, a specialization and an outdoor continent spot.
Retail leveling scales with the zone, so a low-level bot in Icecrown or Silithus is
normal. They are not placed in dungeons, raids or garrisons.

- **Placement**: at login every random bot is put on a valid outdoor spot for its level and faction once per
  `PlacementVersion` (`RandomBotMgr.cpp`, stored as the `placed` event). Bump the constant to reset all bots
  again, e.g. if some ended up under the map. Bots inside dungeons/battlegrounds are placed on every login.
- **Interruptions**: when a bot is moved farther than 60 yards or to another map by anything but itself (player
  summon, GM teleport, dungeon exit) it drops its current RPG job, quest plan and movement, and looks for a new
  chain of NPCs, quests and grind spots around where it stands now; its home moves there too
  (`PlayerbotAI::DetectInterruption/OnInterrupted`, same idea as AzerothCore's `ChangeToIdle` after teleports).
  Taxi flights are not interruptions.

- **Only with players** (`Playerbots.Random.OnlyWithPlayers`, on by default): like
  AzerothCore `DisabledWithoutRealPlayer`. Bots wait 30 seconds after a real player
  logs in, and log out 5 minutes after the last player leaves.

- **Only fully simulate near a player** (`Playerbots.BotActiveAlone`, 0 by default):
  like AzerothCore `BotActiveAlone`, but lonely bots also skip `Player::Update` and
  `VisitNearbyCellsOf` so they do not keep creature grids hot on other continents.
  They wake within Radius yards (150). `Zone`/`Map` force-active is off by default
  (a retail zone is huge). AzerothCore's 10% leftover-active still wakes grids;
  leave the percent at 0 unless you want background world noise. `SmartScale` still
  applies if you raise the percent. Set `BotActiveAlone = 100` for always-on.

- **Scaling**: the server limits characters per game account (`CharactersPerAccount`,
  `CharactersPerRealm`, 60 by default). Bots are spread over many game accounts, and
  game accounts over Battle.net logins `RNDBOT0001@PLAYERBOTS.LOCAL`, `RNDBOT0002@...`
  (`Playerbots.Random.EmailPrefix/EmailDomain/GameAccountsPerEmail`). Accounts are created
  on demand with random passwords, nothing is stored. 1000 bots need 25 game accounts.
- **Gear** (`Playerbots.Random.Gear`): the best item per slot that the class, race and
  active specialization can use at the bot's level, plus food and drink. Re-checked
  every minute so bots re-gear as they level.
- **Remote port** (`Playerbots.Remote.*`): off by default, token authenticated,
  localhost only unless changed. `printf 'AUTH <token>\nstatus\n' | nc 127.0.0.1 <port>`.
  Commands: `status`, `count <n>`, `stop`, `quit`.
- **Purpose (NewRpg)** (`Playerbots.Rpg.Enable`): ownerless bots pick a weighted
  job instead of circling: follow quest POIs, visit innkeepers/vendors/bankers/AH/mail
  and flight masters, taxi between nodes, grind nearby spawns, rest. They mount for
  walks longer than 21 yards (cast a learned mount, never `Unit::Mount`). Movement
  uses mmaps and wall collision so they do not bee-line through Ironforge.

- **Quests** (`Playerbots.Random.Quests`): idle bots walk to a nearby quest giver, take a
  kill quest they can do, hunt, and also walk QuestPOI waypoints from the RPG layer.
- **Dungeon finder, battlegrounds, arenas** (`Playerbots.Random.Lfg/Battlegrounds/Arenas`):
  bots never queue by themselves, they fill the queues of real players. Idle bots of the
  right faction and level join the missing roles of a dungeon finder group, the missing
  players of each battleground team, or form the opposing group of an arena queue.
  They accept proposals/invitations after a few seconds, follow the group leader in a
  dungeon, fight in battlegrounds, and leave queues that no player is waiting in.

## Core requirements

Uses core hooks added for modules: bot `WorldSession`s without a socket (`WorldSession::IsBot`,
`SetBotPacketHandler`, `LoginBotPlayer`, `InitializeSession(callback)`) and `PlayerScript::OnUpdate`.

## AI engine

Same model as AzerothCore: a bot runs named **strategies**; a strategy ties **triggers** ("low health") to **actions**
("healing potion") with a relevance, may add **multipliers** that scale or veto actions, and each tick the most
relevant action that is useful, possible and succeeds runs. Actions can name **prerequisites** (run first) and
**alternatives** (tried when impossible or failed). **Values** ("combat target", "heal target") are named, cached
knowledge (once per tick, or an interval). Everything is created by name, per bot, from the `AiRegistry`
(`src/Engine/`: `Engine`, `AiContext`, `AiRegistry`, `Trigger.h`, `Value.h`, `Multiplier.h`); names may carry a
qualifier (`"party member health::low"`). Strategies can be limited to combat / non-combat / dead and grouped so
that `dps`/`tank`/`heal` exclude each other. `Engine::ApplyStrategyString("+dps,-follow,~loot,?")` and
`Engine::ExecuteAction(ai, name, param)` are the entry points for chat commands.
The generic behaviour (`death`, `teleport`, `combat`, `loot`, `consume`, `follow`, `stay`, `wander`, `pvp`) is
registered in `Strategies/GenericStrategies.cpp`. The rpg and quest strategies still place actions directly
(`AddAction` + `GetRelevance`), which the engine keeps supporting.
`tests/run_engine_test.sh` builds and runs a standalone test of the engine (no TrinityCore build needed).

## Chat commands

A master whispers a bot, or speaks in party chat to all of its bots (also random bots that follow the speaker as
party leader). Code in `src/Cmd/`: `ChatParser` (text side, tested standalone) and `ChatCommands` (the commands,
each one an engine action `cmd <name>`, registered with `EnsureAiRegistered()`).
- Message = chain of commands split at `Playerbots.Command.Separator` (`;`): `@tank follow ; @dps attack`.
- A command may start with `@filters` that pick the bots: `@tank @healer @dps @melee @ranged @caster`, class
  names (`@warrior`, `@dk`...), `@group2`, `@nearby`/`@far`, `@level60-70`, `@all`. Several filters must all match,
  commas inside one filter mean "any of" (`@mage,warlock`). An unknown filter matches nobody.
- Party chat only counts when it starts with `Playerbots.Command.Prefix` (empty = always). Whispers never need it.
- The longest known command name wins, the rest is the parameter (`co +dps,-follow`).
- Commands: follow, stay, passive, assist (aggressive), attack, status (stats), gear, inventory (inv, i), spells,
  quests (q), summon, reset, co / nc / de (combat / non combat / dead strategies, `+x,-x,~x,?`), strategies (ss),
  cast `<spell name or id>`, help `[command]` (?), leave (logout).
- Add one with `AddCommand(name, help, fn, persist)` in `Cmd/ChatCommands.cpp` (or register an action and a
  `ChatCommandInfo` for it): no other wiring is needed.

## Random bots and real players (`Social/BotSocialMgr`)

Random bots answer a player's party invitation after 1.5-4.5 s (`Playerbots.Random.GroupInvitations`: 0 decline,
1 within 10 levels, 2 always; they decline in a queue, in a battleground or when already grouped) and greet the
inviter with a whisper. A guild invitation is accepted when the bot has no guild
(`Playerbots.Random.GuildInvitations`). Once in the player's party the bot follows the leader and obeys the chat
commands above. A whisper to a random bot that is not in your party gets a one line hint
(`Playerbots.Random.WhisperReply`, once per 20 s per player). Only bots you added with `.bot add` listen to
whispers without a party.

## City life: hotspots and phases

- **Default and shared phases only.** Bots live in the default phase (`Travel/PhaseGuard`: phase 0 or 169) and in
  phases every player of an area gets without doing anything (a `phase_area` row that no condition restricts;
  128 of them, e.g. the one Midnight Silvermoon's 650 service NPCs are in). The hub index, the
  quest index and all placement ignore spawns of any other phase, and bots never take the quests that open, close
  or change a phase (world `conditions` of source type 25/26 that test a quest, about 335 quests; plus
  `playerbots_quest_blacklist`).
- **Towns** (`Travel/HubIndex`): service NPCs (innkeeper, banker, auctioneer, flight master, vendor, repair,
  mailbox) are grouped into clusters (a seed claims everything within 150 yd). A cluster is a capital (auction
  house + bank + inn), a town (inn), an outpost (shops only) or a flight master hub. Clusters whose NPCs attack a
  team are not used by that team.
- **Hotspots** (`Travel/Hotspots` pure core, `HotspotMgr`): every usable town is a hotspot, plus invisible marker
  NPCs (creature entries 9100001-9100205, rows in `world.playerbots_hotspot_marker`: tier, team, level range,
  radius). Place a marker with `.npc add <entry>`; it registers while loaded. Bots pick a hotspot by tier weight
  (`Playerbots.Hotspot.TierWeight.N`), skipping full ones (`TierMaxOccupancy.N`), and arrive spread around a
  service NPC.
- **Anchoring**: a bot placed at a hotspot is anchored (saved in `playerbots_db_store` key `anchor`): it only does
  town things (wander NPCs, camp at the town's NPCs, rest, wander around the hotspot), until a rotation
  (`Playerbots.Hotspot.RotateMinutes`, about once per interval for idle bots no player can see) or a move far
  away releases it. `Playerbots.Hotspot.Prob` is the share of placements and rotations that go to hotspots.
- GM commands: `.bot hotspot list [tier]`, `.bot hotspot go <id>`.
- **Where bots gather.** Arrivals in capitals and towns are placed around the places players stand: auction house
  spots count four times, bank three, mailbox/inn/flight master twice, shops once. An anchored bot stays within
  `Playerbots.Hotspot.WanderRadius` (40) of where it arrived and camps at the services in reach (auction house,
  bank, mailbox, inn...). Hand-placed markers (`.npc add 9100001`) work the same for any spot, e.g. the middle of a
  plaza.
- **Overrides**: world table `playerbots_hotspot_override` retunes a derived hotspot by place (level range, tier,
  team, or remove it). Seeded: Silvermoon City (Midnight) is for level 80+.

## Travel graph and routes (stage B)

- `Travel/Graph/TravelGraph` (pure, tested): nodes and typed links (walk, flight path, boat, portal, hearthstone) with
  cost in seconds, team/level/gold limits, an A* planner and `PlanPositions` (walk to a nearby node, ride the graph,
  walk from a node near the goal; the straight walk is the baseline).
- `Travel/TravelMgr` builds it at startup (`Playerbots.Travel.Enable`): a node per town (HubIndex cluster), a node
  per flight master of the taxi network with a link per `TaxiPath` (length at 32 yd/s + stops, gold cost, team from
  the node flags), open world area trigger portals, and walking links between each node and its 6 nearest
  neighbours within 1,500 yd (cost = distance x 1.3 / run speed, mounted bots 11.2 yd/s). The log prints sample
  routes (Stormwind to Orgrimmar and others) as a sanity check. Boats and zeppelins are links between the ground nodes
  next to their docks (stops of the transport's taxi path, trip = cycle time / stops + wait; gunships and raid ships
  excluded). Nobody boards: a bot waits at the dock for the trip time and is placed at the other dock.
- `Travel/RouteFollower` (per bot): walk legs with `MoveToward` and stuck detection, flights through
  `TakeTaxi` (no NPC needed, gold granted when `Travel.FreeFlights`), portals by walking into the mouth,
  hearthstone by casting it. A failed leg is re-planned (3 times, a refused flight is blocked for 5 minutes), then
  the bot gives up. Bots nobody can see (frozen) do not walk: they jump ahead along the route one stretch at a time,
  after that stretch's travel time clamped to `Playerbots.Travel.HopMinSeconds` .. `HopMaxSeconds` (2 to 5 minutes).
- Engine: strategy `travel` (default for every bot) runs the route at relevance 55 (below fighting, above looting
  and rpg), `MasterAwayTrigger` is off while a route is active. Chat: `goto <place or node id>`; `stay`/`follow` cancel.
- Enemy ground and danger (ideas from AC's travel code): a town whose creatures attack a team is closed to it, so
  no route of that team walks through it and no route ends in it; every walking link carries the highest level of
  a creature near it that attacks each team (`Travel/DangerMap`, from the creature spawns, level = the lowest the
  content tuning gives it), which makes it longer in the plan (`Playerbots.Travel.DangerSecondsPerLevel`) or
  removes it (`DangerBlockLevels`) for bots well below that level. Links that fail are blocked for 5 minutes, twice
  as long after each failure in a row (up to a day), and forgiven once a bot gets through.
- GM: `.bot travel info`, `.bot travel nodes <text>`, `.bot travel route <node id>` (from where you stand).
- Not yet: mmap-verified walking links, using routes from quests and rpg (stage C).

## Quests across the world (stage C)

- Selection (`Strategies/QuestTravel.cpp`): every quest giver of the world is a candidate. The bot's costs to the whole
  travel graph are computed once (`TravelMgr::ReachFor`, one Dijkstra) and each giver is rated from that
  (`Estimate`); givers further than `Playerbots.Quest.MaxRouteMinutes` (20) or inside a town that attacks the bot's
  team are skipped, quests in the phase blacklist too. Ranking = route cost + up to 90 s of luck + 60 s per bot that
  already works on the quest. Without the travel graph the bot stays on its own map within `Random.QuestDistance`.
- Movement: anything further than 250 yd or on another map goes through the route follower (walk, flight, boat,
  portal, hearthstone; virtual hops for bots nobody can see). The travel strategy runs the route while the quest
  action waits; a quest's clocks restart when a route ends, a route that fails twice gives the quest up. Hunting
  grounds and turn-in NPCs on other maps are found by route cost too (a bot restored with a quest in its log on the
  wrong continent travels to the hunting grounds).
- Phase safety: before and after accepting and handing in a quest the bot's phases are compared; a bot that gains a
  phase that is not the default, a shared phase or cosmetic (`PhaseGuard::ForeignPhases`) abandons the quest and the
  quest goes to `playerbots_quest_blacklist` (`PhaseGuard::BlockQuest`, also kept in memory for this run). Static gating
  (conditions of phase sources, about 335 quests) is applied when the index is built.
- A bot anchored at a hotspot does not quest (it does town things); a rotation releases it.


## Spec AI on retail data (phase 5)

- Talents (`Talents/BotTalents.cpp`): a bot takes the retail starter build of its specialization
  (`HandleClassTalentsSetStarterBuildActive`) when it is created and again when level or spec changes
  (`PlayerbotAI::MaintainTalents`).
- Rotations (`Rotation/`, data in `data/rotations/<class>.rot`): one text set per specialization id, SimulationCraft
  style: `spec <id> <name>`, then lists (`buffs`, `interrupt`, `defensive`, `heal`, `combat`) of
  `Spell Name | target=X offgcd ooc if CONDITION`; the first castable line of a list wins, unconditional fillers go last.
  Spells are resolved by name against what the bot knows, so a talent that is not taken simply skips its line.
  Facts: self./target./enemies/group./buff./debuff./cooldown.X.ready|remains/spell.X.known/flags (see
  `data/rotations/README.md`). All 13 classes (39 specializations) are written.
- A specialization without rotation, or `Playerbots.Rotation.Enable = 0`, uses the generic spell picker.
- `.bot rotation reload|info` re-reads the files without a restart; unknown facts are reported at load
  ("Playerbots: N rotations from M files ... (K problems)").
- Not yet: pet control, cooldown planning per encounter, play-tested numbers (all thresholds are first guesses).

### Hotspot selection

Derived hotspots only come from the tiers in `Playerbots.Hotspot.AutoTiers` (default 1 = capitals, where players
crowd). Add a town by an override row with `enabled = 2`, or place a marker NPC. `.bot hotspot list` shows what is left.
