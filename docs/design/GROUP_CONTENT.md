# Playerbots v2: battlegrounds, guilds, rotations, raid awareness

User request 2026-10-15. These were v1 non-goals; this doc moves them into a v2 track (ROADMAP P7-P11).

Paths: `mod:` = `modules/mod-playerbots/src/`, core = `src/server/game/`, `AC:` = `azerothcore-wotlk/modules/mod-playerbots/`. Client data = `wowdata --profile tc-local` (build 69933 = 12.1.0).

## 1. Battleground queueing (only with humans queued, like AC)

### Today (H)

- `ActivityMgr` already fills **only queues that contain a human**, every 5 s (`mod:ActivityMgr.cpp:51,466-526`):
  - BG join via `HandleBattlemasterJoinOpcode`; invite/leave via `HandleBattleFieldPortOpcode` (`:119-133,309-322`);
  - arena = a bot group of the opposite faction ±5 levels (`:530-614`);
  - LFG fills missing roles ±4 levels with 2-6 s delays (`:52-61,201-221,330-430`);
  - bots leave a queue after 60 s without a human (`:58,317`).
- Gap A: each team is filled only to `GetMinPlayersPerTeam`, at most 6 bots per scan (`:491-497,60`). AC fills each side to team size × (queues + running instances) (`AC:src/Ai/Base/Actions/BattleGroundJoinAction.cpp:195-439`).
- Gap B: **inside a BG bots have no objectives.** The "pvp" action chases the nearest enemy player. There is no prep-gate handling, and the in-place `AutoResurrect` is not retail behavior (`mod:PlayerbotAI.cpp:137-139`; `mod:Strategies/GenericStrategies.cpp:322-354,570-587`; README "Not done yet").

### Design

- **Queue fill:**
  - fill each side to the BG's max team size, balanced with the human side; keep the "human must be queued" gate;
  - the bracket comes from the queue;
  - stagger joins 2-10 s so a match does not pop with 9 identical join times.

  Config: `Playerbots.BG.FillToMax` (1), `BG.JoinDelayMin/Max`.
- **Inside a BG, generic objective layer first.** Retail BG objectives are game objects:
  - `GAMEOBJECT_TYPE_CAPTURE_POINT` = 42, `NEW_FLAG` = 36, `NEW_FLAG_DROP` = 37, older `FLAGSTAND` 24 / `FLAGDROP` 26 / `CONTROL_ZONE` 29 (core `Miscellaneous/SharedDefines.h:3188-3206`);
  - TC master has per-BG scripts in `src/server/scripts/Battlegrounds/` (WSG, AB, AV, EotS, BfG, Twin Peaks, Silvershard, Temple of Kotmogu, Seething Shore, Deephaul Ravine, IoC, SotA, arenas).

  Behavior:
  - each bot gets a role on entry (attack / defend / flag): AC's role 0-9 split (`AC:BattleGroundTactics.cpp:1207-1275`) reimplemented;
  - target = nearest objective GO not owned by its team (capture point or flag); interact through the GO use handler; fight enemies within 30 yd of the objective; defenders hold owned objectives;
  - flag carrier → its own base capture point;
  - wait inside the start area until the gates open;
  - resurrect at the BG spirit healer like a player, not in place.
- **Interaction:**
  - `CMSG_GAME_OBJ_USE` is INPLACE (`Server/Protocol/Opcodes.cpp:467`), so it is callable from the bot's map thread.
  - Retail capture points are **channeled casts** that damage interrupts and that need standing still. The objective layer holds position, retries after an interrupt, and has defenders peel attackers off the capper.
  - Flag pickup is a single click.
  - Hazard avoidance (§4) must not move a bot that is channeling a capture unless the hazard is lethal.
- **Per-BG tactics later**, as data (objective priority lists per map), not code per BG.
- Movement goes through BotMover (navmesh), so the mmaps rule holds in BGs too.

## 2. Random guilds

### Today (H)

- Bots only accept invites (`mod:Social/BotSocialMgr.cpp:53-59,190-203`).
- `playerbots_guild_names` is seeded (README: 400 names) and unused.
- No `Guild::Create` (core `Guilds/Guild.h:748`, `.cpp:1138`), no `sGuildMgr->AddGuild` (core `Guilds/GuildMgr.h:57`), no bot-to-bot invites.
- AC: `PlayerbotGuildMgr` (`AC:src/Mgr/Guild/PlayerbotGuildMgr.cpp:26-247`):
  - a bot without a guild joins a random same-faction non-full bot guild, or founds one with the next free name and becomes leader;
  - up to `RandomBotGuildCount` 20, size `RandomBotGuildSizeMax` 15, random rank and tabard.
  - guild tasks (`GuildTaskMgr`) are off by default.

### Design (reimplement AC behavior)

- `BotGuildMgr` on the world thread, **AC defaults (user decision 2026-10-15)**:
  - at startup, load the bot guilds: leader account is a random-bot account;
  - up to `Guild.Count` = 20 bot guilds in total (split by faction in proportion to the bot population), max `Guild.SizeMax` = 15 members (`AC:conf` `RandomBotGuildCount`/`RandomBotGuildSizeMax`);
  - bots ≥ level 10 with no guild: join with chance `Guild.JoinChance`, preferring the guild with most members in the same hub/zone, so guild-mates are seen together; else found one if under the target.
- Ranks: leader, 1-2 officers, rest member/initiate.
- Guild MOTD/info from a small text table.
- Guild chat stays silent until the chat module hook (P6).
- **Humans** (AC behavior):
  - bots accept invites from human guilds (existing);
  - bot guilds do not recruit humans;
  - a human who still joins one (e.g. a GM invite) is never kicked or demoted.
- Guild bank/tasks: out of scope.

## 3. Rotations from Assisted Combat ("the button")

### What exists (H)

- Module rotations: hand-written `.rot` scripts for 39 specs (`modules/mod-playerbots/data/rotations/`), parsed by `mod:Rotation/RotationScript`, run by `RotationRunner` (first known, ready, in-range, in-LOS entry whose condition holds; `mod:Rotation/RotationRunner.cpp:129-273`). They are used in groups for heal/taunt targets.
- **Retail Assisted Combat is entirely client-side.** The client evaluates the steps; the server only knows the spell. Pieces:
  - spell 1229376 "Single-Button Assistant" (dummy effect, +25% GCD). TC teaches it at login only via a local patch (core `Entities/Player/Player.cpp:19163-19167`, commit `0cfae181`), with no script;
  - no opcode (`Server/Protocol/Opcodes.cpp`);
  - TC has only DB2 metadata, `AssistedCombatMeta`/`RuleMeta`/`StepMeta` (core `DataStores/DB2Metadata.h:1433,1453,1479`), and no store.
- **The data is on disk.** `docker/data/dbc/enUS/AssistedCombat.db2`, `AssistedCombatStep.db2`, `AssistedCombatRule.db2` are extracted:

  | Table | Rows | Content |
  |---|---|---|
  | AssistedCombat | 40 | one per spec |
  | AssistedCombatStep | 693 | ordered spell list per set |
  | AssistedCombatRule | 3116 | AND conditions per step: `ConditionType` + 3 values |

  Fury (spec 72, set 29) order: Battle Shout, Thunder Clap, Whirlwind, Recklessness, Avatar, Odyn's Fury, Rampage, …, Bloodthirst, Raging Blow, Slam.
- **Unknown:** the meaning of the ~70 `ConditionType` values. The enum lives in the client binary.
  - Types 0/14/2 sit on almost every step, so they are probably generic usable/known/in-range checks.
  - Types 9/15/16 take spell IDs, so they are probably aura/talent present/absent.
  - Confidence L until decoded.

### Decision (user, 2026-10-15): keep the `.rot` scripts

The bots run the existing hand-written `.rot` rotations (`RotationRunner`). Assisted Combat is **reference data only**, used to review and tune the scripts:

1. **Offline tool**, no runtime load and no core patch: dump each spec's `AssistedCombatStep` order + rules via `wowdata --profile tc-local` into `data/rotations/assisted_combat/<spec>.txt`, one file per spec.
2. **Review pass per spec:** compare the dump with the `.rot` script. Look for spells the button uses that the script lacks (or the reverse), ordering differences, and the conditions we can read (aura/talent spell IDs on types 9/15/16). Each change to a `.rot` file is a normal reviewed edit, with the reason as a comment.
3. **What `.rot` keeps covering:** healing priorities, defensives, interrupts, taunts, AoE vs single target. Assisted Combat does not cover these.
4. **Bots cast the real spells**, never 1229376 (the button adds +25% GCD).
5. **Metric:** damage/healing per minute on a target dummy per spec, before and after each review pass.
6. Re-dump when the local client moves past 12.1.0 (69933).

## 4. Raid awareness: "where not to stand"

### What exists (H)

- No avoidance anywhere in the module (0 hits for AreaTrigger/DynamicObject/aoe).
- Positioning is plain `MoveChase`: melee does not get behind, ranged does not spread (`mod:PlayerbotAI.cpp:826-836`).
- Core gives the inputs:
  - AreaTriggers: searchers (core `Grids/Notifiers/GridNotifiers.h:707-744`), `GetSpellId`/`GetCaster`/`GetMaxSearchRadius`/`GetDuration` (`Entities/AreaTrigger/AreaTrigger.h:132,196,202,182`), shapes (`DataStores/DBCEnums.h:195`, `AreaTriggerTemplate.h:92-201`), splines/orbits (`AreaTrigger.h:205-211`).
  - **Point-in-shape is protected** (`SearchUnitIn*`, `AreaTrigger.cpp:761-942`).
  - DynamicObjects: `GetSpellId`/`GetRadius`/`GetCaster` (`Entities/DynamicObject/DynamicObject.h:76-85`) via `WorldObjectListSearcher` with `GRID_MAP_TYPE_MASK_DYNAMICOBJECT` (`Grids/GridDefines.h:78`).
  - Telegraphed casts: `Unit::GetCurrentSpell` (`Entities/Unit/Unit.h:1495`), `Spell::GetTimer` (`Spells/Spell.h:718`), `SpellInfo::GetSelectionCategory` (cone/area/line, `Spells/SpellInfo.h:183`), `ConeAngle`/`Width` (`:410-411`), `SpellEffectInfo::CalcRadius` (`:276`), `IsPositive` (`:510`).
  - Encounter state: `InstanceScript::IsEncounterInProgress` (`Instances/InstanceScript.h:212`).
  - Roles: `ChrSpecializationEntry::GetRole` (`DataStores/DB2Structure.h:939`) and group MAINTANK flags (`Groups/Group.h:76-78`).
- Most raid damage areas come from scripted `AreaTriggerAI::OnUnitEnter` (130 uses), not data actions. So "caster hostile + harmful spell" has to be the default hazard test, not reading actions.
- AC reference:
  - generic `AvoidAoeAction` covers DynamicObjects, trap GOs and trigger NPCs with periodic damage, with a whitelist and a max radius 15 (`AC:src/Ai/Base/Actions/MovementActions.cpp:1846-2245`); flee spots go left/right/behind the target, farthest from the hazard;
  - per-boss logic is hand-coded per encounter (Heigan `AC:src/Ai/Raid/Naxx/NaxxBossHelper.cpp:138-200`, Onyxia `AC:src/Ai/Raid/Ony/OnyActions.cpp:14-90`, Festergut `AC:src/Ai/Raid/ICC/Action/ICCActions_FG.cpp:386-420`).

### Design: generic hazard field + data hints, no per-boss C++ by default

1. **HazardScan** (per ACTIVE bot in combat, every 250-500 ms, own map thread), within 40 yd:
   - (a) AreaTriggers whose caster is hostile to the bot, or whose spell is not `IsPositive`;
   - (b) DynamicObjects, same test;
   - (c) hostile casts in progress with an AREA/CONE/LINE selection category: zone from caster position/facing + radius/cone/width, live for the remaining cast time;
   - (d) creatures flagged in the hint table (void-zone NPCs).

   The result is a list of shapes with an expiry.
2. **Point-in-shape: module approximation (user decision 2026-10-15, no core patch).**
   - The shapes are reimplemented in the module from public `m_areaTriggerData->ShapeData` (`AreaTrigger.h:221`) plus the trigger's position/orientation and `GetShapeProgress`.
   - Sphere/cylinder/disk/box/polygon/bounded-plane are exact for static triggers.
   - Spline/orbit (moving) triggers are approximated by `GetMaxSearchRadius` (`AreaTrigger.h:202`) around their current position, plus a 1 yd margin. That is conservative: the bot moves a bit further than needed.
   - Unit test: module geometry vs core `SearchUnitIn*` on recorded trigger samples (pure test, no server).
3. **SafeSpot:** when the bot stands in (or within 1.5 yd of) a hazard, sample 16 candidate points on rings of 4/8/12 yd. Each is navmesh-snapped through BotMover. Score:
   - outside all hazards with a margin: required;
   - still in role range of the target (melee ≤5 yd behind or beside, ranged ≤35 yd);
   - spread ≥ `spread` yd from other ranged, or stack when a hint says stack;
   - short path;
   - not claimed by another bot this tick (AC-fork idea, `AC: MovementActions.cpp:2070-2245`).

   Move at once and keep casting instant spells while moving (rotation flag).
4. **Role positioning** (always, not only on hazards):
   - tank: holds the boss and faces it away from the raid;
   - melee: behind the boss via `MotionMaster::MoveChase(target, range, angle π)` (core `Movement/MotionMaster.h:164`);
   - ranged/healers: spread arc at 25-30 yd.
5. **Encounter hints table** `playerbots_encounter_hint(map, boss_entry, spell, kind, radius, data)`: kind = avoid / stack / spread / soak / face_away / move_out_with_debuff / interrupt.
   - Authored from the local DBM/BigWigs references and JournalEncounterSection (memory: MDT/DBM drafts, WCL tooling), one raid as a pilot.
   - Hints refine the generic layer; without hints the generic layer still dodges hostile ground effects.
6. **Out of scope at first:** soak/assignment puzzles, vehicles, phase transitions with platforms. They go into hints with explicit per-boss review.

### Pilot: Firelands (user decision 2026-10-15)

- Firelands has local script work in `src/server/scripts/Kalimdor/Firelands/`: `boss_alysrazor.cpp`, `boss_baleroc.cpp`, `boss_lord_rhyolith.cpp`, `boss_majordomo_staghelm.cpp`, `boss_ragnaros_firelands.cpp`, `instance_firelands.cpp`. Beth'tilac and Shannox have **no script** in our tree. DBM references are under `/home/brandon/references/dbm`.
- First boss: a scripted one with clear ground hazards. Candidates are Baleroc (simplest positioning) or Lord Rhyolith (many ground effects). Choose after reading which AreaTrigger/DynamicObject spells each script spawns; Ragnaros comes last. Hints are authored for it from DBM/BigWigs timers and the Journal sections; then the rest of the instance.
- Run the core support gate (CLAUDE.md) on every spell a hint names before relying on it.

### Acceptance (pilot raid boss)

- a bot group with 1 human leader;
- time standing in hostile ground effects < 5% of the ground-effect time a stand-still baseline would take;
- 0 bee-line moves (BotMover);
- the boss kill rate is reported but not gated at first.
