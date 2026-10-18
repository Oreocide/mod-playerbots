# Playerbots v1: data model

`mod:` = `modules/mod-playerbots/`. Custom SQL lives only in `mod:data/sql/db-{auth,characters,world}/` and is applied by the module SQL updater (local `database/Updater/DBUpdater.*` patch). TDB files and `sql/updates/` are never edited.

## Existing tables (kept)

| DB | Table | Use | Cite |
|---|---|---|---|
| characters | `playerbots_db_store` (guid, key, value) | per-bot key/value: `strategies`, `home`, `anchor`; v1 adds `profile`, `paused`, `npc_cd` | `mod:data/sql/db-characters/playerbots_00_schema.sql`; access only via `sPlayerbotRepository` (`mod:src/Db/PlayerbotRepository.cpp`) |
| characters | `playerbots_random_bots`, `playerbots_account_type`, `playerbots_names`, `playerbots_quest_blacklist` | population, names, quest blacklist | same |
| world | `playerbots_hotspot_marker`, `playerbots_hotspot_override` | hotspots | `mod:data/sql/db-world/2026_10_07_0{0,1}_world_mod_playerbots_*.sql` |
| auth | RBAC 1010 | `.bot` command permission | `mod:data/sql/db-auth/2026_09_29_00_auth_mod_playerbots_rbac.sql` |

## New tables

### world: `playerbots_behavior_profile`

One row per (profile, behavior). This is what replaces the `Playerbots.Rpg.Weight.*` / `Rpg.Duration.*` config keys (`conf/playerbots.conf.dist:670-705`).

| Column | Type | Meaning |
|---|---|---|
| `profile` | VARCHAR(32) | e.g. `default`, `townie`, `traveler`, `quester` |
| `behavior` | VARCHAR(32) | `idle`, `wander`, `wander_npc`, `innkeeper`, `vendor`, `repair`, `trainer`, `quest`, `mailbox`, `auction`, `flight`, `gather`, `grind`, `camp` |
| `weight` | SMALLINT UNSIGNED | 0 disables |
| `min_ms`, `max_ms` | INT UNSIGNED | duration window |
| `cooldown_ms` | INT UNSIGNED | per-bot cooldown after the behavior ends |
| `arrival_boost` | SMALLINT | weight bonus for the first pick after arriving in a town hub |
| `flags` | INT UNSIGNED | bit 0 town-only, bit 1 outdoor-only, bit 2 needs mount level |
| `comment` | VARCHAR(255) | |
| PK | (`profile`, `behavior`) | |

The seed rows copy today's config values into profile `default`, so behavior does not change at migration. Missing rows fall back to the config keys, which stay as defaults (H: design).

### world: `playerbots_behavior_overlay` (hop/emote)

| Column | Type | Meaning |
|---|---|---|
| `profile` | VARCHAR(32) | |
| `overlay` | VARCHAR(16) | `hop`, `hop_idle`, `emote`, `greet` |
| `chance_per_min` | FLOAT | |
| `data` | VARCHAR(255) | comma list of emote ids for `emote` |
| PK | (`profile`, `overlay`) | |

Profiles are assigned in `playerbots_db_store` key `profile` (default from config `Playerbots.Behavior.DefaultProfile`), so different bots can act differently.

## File naming

TC update naming is `YYYY_MM_DD_NN_<db>_<desc>.sql` in the module dirs, e.g.:
- `mod:data/sql/db-world/2026_10_20_00_world_playerbots_behavior_profile.sql` (CREATE + seed)
- `mod:data/sql/db-world/2026_10_20_01_world_playerbots_behavior_overlay.sql`

The existing `playerbots_0N_*.sql` character files keep their names, because renaming applied files breaks the updater's hash tracking. Every world-DB data change gets a backup and rollback script in `trinitycore-tracking/backups` (project rule).

## Config keys (new)

| Key | Default | Meaning |
|---|---|---|
| `Playerbots.Behavior.DefaultProfile` | `default` | profile for bots without a `profile` store key |
| `Playerbots.Behavior.NpcCooldownSeconds` | 600 | per-bot GUID cooldown after a failed target |
| `Playerbots.Behavior.LoopFailures` | 3 | failures within 10 min before the behavior is disabled for that bot for 30 min |
| `Playerbots.Mover.SnapMax2d` / `SnapMaxZ` | 3.0 / 2.5 | max distance between the wanted point and the navmesh end point |
| `Playerbots.Mover.SegmentMinProgress` | 5.0 | yards per segment |
| `Playerbots.Mover.FallRescueSeconds` | 3 | falling time before rescue |
| `Playerbots.AH.Post` | 1 | auctioneer visits may post grey/white items |
| `Playerbots.AH.MaxPostsPerVisit` | 3 | items per visit |
| `Playerbots.AH.PriceMultGrey` / `PriceMultWhite` | 2.0-4.0 / 3.0-6.0 | buyout = vendor SellPrice × random in range |
| `Playerbots.MaxActive` | 100 | ACTIVE count cap |
| `Playerbots.MaxOnline` | 1000 | logged-in cap |
| `Playerbots.Budget.MapTickMs` | 4 | per-map AI budget per world tick |
| `Playerbots.Budget.MaxDeferMs` | 2000 | starvation guard |
| `Playerbots.SaveIntervalMs` | 600000 | bot periodic save interval |
| `Playerbots.Debug.ForceActive` | 0 | N bots ACTIVE without humans (headless soak only) |

Every new key goes into `mod:conf/playerbots.conf.dist` **and** the hand-kept `docker/etc/worldserver.conf.d/playerbots.conf`. Otherwise the C++ default is used silently (NOTES.md). The docker conf is benchmark-protected: change it only with a backup, and restore it after runs (memory: restore conf after benchmarks).

## Config keys added by the built code (2026-10-16)

`Playerbots.Debug.ForceActive` (0), `Playerbots.Rpg.SellJunk` (1), `Rpg.Train` (1), `Rpg.TrainMaxPerVisit` (8), `Playerbots.Spawn.Evict` (1), `Playerbots.Gear.ClassArmorOnly` (1), `Playerbots.Mounts.RacialGrant` (1), `Playerbots.Rpg.HopChance` (0.03), `Rpg.HopMinInterval` (12000), `Rpg.IdleHopChance` (0.04), `Rpg.GreetEmotes` (1), `Rpg.GreetDistance` (12), `Rpg.GreetCooldown` (300000), `Rpg.GreetChance` (0.4). All documented in `conf/playerbots.conf.dist`; none added to the docker conf (the C++ defaults apply). GM commands added: `.bot rpg <bot> <status|service>`; `.bot travel info` also shows mesh mover counts.

## GM commands

See PERFORMANCE.md §GM commands (`pause`, `resume`, `spawn`, `inspect`, `stats`, `profile reload`) plus the existing set.

## Chat hook data

`BotStatus` lives in memory only (PLAN.md §Chat/intent hook). Nothing is persisted.
