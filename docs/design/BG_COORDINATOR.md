# Playerbots: battleground coordinator (core, modes, profiles)

Draft 2026-11-10, revised after the owner's Stop 2 review. This is a plan only: no code until the owner approves. It restructures the existing team planner (BG_PLAN.md, phases 0-5) into three layers; BG_PLAN.md stays the record of what was built. Decisions are listed in §7. Confidence per item: H high, M medium, L low. Paths are module paths unless they start with `src/server` (core).

## 0. Starting point (Step 0)

The three layers already exist in one piece of code:

| Layer | Today | Where |
|---|---|---|
| Core | `Planner`: one `BgPlan` per (map, instance), two `TeamPlan`s, an `Assignment` per bot keyed by guid, a world-thread tick every `Bg.Planner.TickMs` (3000), the seeded attack/defend split | `src/Battleground/BgPlanner.h:52-301`, `BgPlanner.cpp:472-757`, `BgRules.h:109` (H) |
| Modes | `MapKind` Flag / Capture / Eye / Valley, with one planner function and one bot-side function per kind | `BgMapData.h:32-38`; `BgPlanner.cpp:759` `PlanTeam`, `:1102` `PlanValleyTeam`; `BgPlay.cpp:317` `PlayFlagMap`, `:430` `PlayNode`, `:466` `PlayEye`, `:637` `PlayValley`, `:587` `PlayPush` (H) |
| Profiles | `MapDef` code tables for 2106, 726, 2107, 761 and 566; the world table `playerbots_bg_node` for 30 (25 rows + 2 mines) | `BgMapData.cpp:32-57`, `:128 LoadNodes`; `data/sql/db-world/2026_11_06_00_world_mod_playerbots_bg_node.sql` (H) |
| Adapters | GO values (`Discover` + `ReadState`: CapturePoint, control zone, NEW_FLAG) and world states (`ReadValley`) | `BgPlanner.cpp:515`, `:567`, `:1026` (H) |

What the core lacks for the target design (H, code):
- **Target choice.** Attack targets are ordered by weight alone; distance only breaks ties (`BgPlanner.cpp:1252-1258`). Attackers have no quota, so all of them go to one or two targets (`:1260-1262`).
- **Commitment.** Only the attack/defend flip has a hold time (`BgRules::Split`). An attacker's objective follows the target set at once (`:1277-1305`, `:905-925`).
- **Objectives.** They are fixed positions; flags are a separate struct (`FlagInfo`). Each map has exactly one kind.
- **Phases and time.** There are no phases. The soft gate reads `GetElapsedTime`, which includes the 2-minute preparation (`:1180`; `src/server/game/Battlegrounds/Battleground.h:284-285`).
- **Head counts.** The counts of enemies and allies near each objective (`Objective::Near`) are filled only by `BuildPicture` (`BgReact.cpp:459`, `:489-494`). That only runs while one of the phase-4 keys is on (`BgPlanner.cpp:668-671`), and all 14 are off live because of #31.
- **Combat pickup.** It has no bound:
  - an out-of-combat bot takes on the attackers of any raid member within 60 yd (`src/PlayerbotAI.cpp:1294-1305`, `:1169`);
  - the attack action (60) beats the battleground action (30) (`src/Strategies/GenericStrategies.cpp:2034`, `:2051`);
  - the leash exists only in the boss room (`BgPlay.cpp:726-733`).
- **Events.** The core scripts give a module no events to hook, so the module polls state (H).

## 1. Diagnosis (Step 1)

### Alterac Valley: Snowfall camping
Evidence: the code, plus two capped headless matches. On 11-07 MoveOn did not exist yet; on 11-10 it was on.

- **A. Everyone goes to Snowfall at the start (H).**
  - Snowfall's row weight is 9 for both teams, the top attack weight on either side.
  - The next candidate is a captain, and a boss is never paired with another target, so Snowfall becomes the one target.
  - On 11-10, 48 s into play, 26 of 26 Alliance and 19 of 19 Horde attackers were on it.
- **B. Snowfall stays a fight all match (H).**
  - While the Alliance holds it, it remains the Horde's top target. On 11-07, 19 Horde attackers were still on it at 837 s; on 11-10 the Horde plan did not change for 804 s.
  - It is also the Alliance's largest defend weight, 9 of 22 (`BgPlanner.cpp:1140-1144`). On 11-07, 5 of 9 Alliance defenders were there at 837 s.
- **C. The fight pulls in passing bots (code H; that the push bots were standing in it is M).**
  - Bots join it through the raid-attacker pickup, and nothing returns them to the gather.
  - The Galvangar rally point (-445, -195) sits on the Horde's road to Snowfall. On 11-10 that gather reached 0-1 of 8 bots for 2 x 120 s, and the push was abandoned.

Hypotheses:
1. Nearest or neutral scoring: confirmed in effect, but the cause is the static weight, not distance.
2. Bots wait until the assaulted node converts: rejected for attackers, confirmed for defenders.
3. Respawn makes bots pick Snowfall again: rejected as a re-pick, because the job survives death. The same effect still arrives through C (M).
4. No shared count of who targets what: confirmed for attackers.
5. Other:
   - C above.
   - A Snowfall back-and-forth: the code allows it (M), but neither run showed it.
   - The fight before the first click is short: the assault came about 120 s after the gates opened (L).

The assault-to-capture time is 4 minutes (`src/server/scripts/Battlegrounds/AlteracValley/alterac_valley.cpp:165`, H).

### Other battlegrounds (code; the 11-10 probe updates this list)
- **Warsong Gulch, Twin Peaks, Eye of the Storm:** the carrier stops to fight, because `CarrierPlay` is off (`BgPlay.cpp:1173`).
- **Warsong Gulch, Twin Peaks:**
  - the carrier parks on its base point while our flag is out (`:337-348`);
  - defenders chase the enemy carrier with no distance limit (`:374-379`).
- **Eye of the Storm:**
  - a carrier with no tower held falls back to the plain chase (`:491`);
  - defenders never hunt the enemy carrier (`:509`);
  - there are no flag runners before the team holds a tower (`BgPlanner.cpp:895-900`).
- **Arathi Basin, Gilneas:** attackers leave as soon as the 6 s capture cast lands (`:787`, `:841`).
- **Arathi Basin, Gilneas, Eye of the Storm:** targets are picked again by the attackers' centre on any event or every 60 s (`:848-889`). Suspected churn.
- **All planned maps:** dead bots count in the split (`:655-663`); cause C applies too.
- **The six unplanned maps (607, 628, 727, 998, 1803, 2656):** no objective play. Bots chase the nearest enemy, even before the gates open (`GenericStrategies.cpp:1549-1552`).

**Rule for the probe results:** if a cause assumed here is rejected, every design part that depends on it is flagged here and at that part. The probe covers Warsong Gulch, Arathi Basin and Eye of the Storm only. AV causes A-C are not re-tested by it.

### Probe results (2026-11-10 11:18-12:06 CST, bot-only, 150 bots, live keys = arm A)
- **Run:** WSG 13 v 16 (samples 162-848 s elapsed) and AB 13 v 15 (164-1,107 s). The Eye ran separately, 15:02-15:18 CST: 13 v 16, samples 164-863 s.
- **Eye of the Storm result (mod-playerbots#34):**
  - 13 minutes of play at 0-0; all four towers stayed neutral and the flag never left its base. No capture cast, no job change, no sampled bot in combat.
  - The sampled bots stood within 1 yd of their tower object all match (Alliance at Mage Tower, Horde at Blood Elf Tower).
  - Lead from code (M): the control zone counts only players with `Player::IsOutdoorPvPActive()` (`Player.cpp:7754`): `IsPvP()`, not flying. Headless bots may lack the PvP flag or carry a stale movement flag.
  - The planner's "no runners before a tower" rule (`BgPlanner.cpp:895-900`) then keeps the flag in base: confirmed in effect.
  - **Design parts affected:** §3.2's Eye fixes and backlog B3 assume a tower can be taken. B3 must first fix #34: the problem sits at the bot or core level, not in the plan.
- **Data:** runner, results and analyzer in the session scratchpad (`bgprobe.sh`, `bgprobe_results.txt`, `bgprobe_analyze.py`). Conf restored, md5 equal.

| Finding | Evidence | Verdict |
|---|---|---|
| **AB stalemate from cause C** | No node changed hands from 311 s to 1,107 s. Capture casts stayed at 23 after about 5 min; Stables and Gold Mine stayed neutral all match. The sampled Horde attackers assigned to Lumber Mill fought 40-87 yd from Blacksmith in every sample for 13 min and never reached it. The Alliance attacker assigned to Blacksmith fought 40-82 yd from it. In all, 15 of 25 sampled in-combat states were more than 60 yd from the bot's objective | **cause C confirmed outside AV**: H for the sampled bots (about 5 per map); M as a team-wide rate |
| **AB targets skip free nodes** | The Alliance attacked Farm and Blacksmith (Horde-held, defended) the whole match while Stables and Gold Mine stayed neutral. The Horde had Stables as a target for 13 min and never cast | observation H. Cause read in code (H): candidates are nodes neither ours nor under our assault, plus ours under their assault. They are sorted retakes first, then by distance from the attackers' centre, with no owner or defence term; 13 attackers get 2 targets (`BgPlanner.cpp:833-880`). The brawl's location sets the centre, so the nodes nearest the fight are picked again and again. A feedback loop with C |
| **WSG carrier fights at home instead of capturing** | At 450 s the Alliance carrier was in combat, attacking, 28 yd from its own flag with our flag in base. The capture came about 160 s later (score 1 at 616 s) | confirmed for one carrier (H for that sample) |
| **WSG defender held by a fight far from its post** | An Alliance defender was in combat 511-515 yd from its own flag (in the Horde base) in three samples over 232 s | cause C confirmed on WSG (H) |
| **Plan re-made mid-match (new)** | WSG sample at 616 s: "3 plans", last event "match start, 7 s ago", in a match already 10 min old. The plan object was dropped and made again, which reseeds the split and resets every job | re-made (H); cause most likely the 30 s no-touch drop (`BgPlanner.cpp:485`): bots touch the plan only from `Play`, which runs out of combat. Every bot fighting or dead for 30 s drops it (M). Issue mod-playerbots#33 |
| WSG captures | 2 by 848 s; the match ended at the time limit, Alliance 2-0 (inferred from the 17-min elapsed limit, M) | captures work |
| AB attackers leave once the cast lands (problem list) | No assault after about 5 min to observe | unclear |
| Churn | WSG 36 job or objective changes over 5 intervals for 20 bots (the restart included); AB 31 over 7 intervals for 28 bots | moderate. Commitment (2.5) bounds it |

**No assumed cause was rejected.** Design parts affected by the results:
- **§2.1:** the plan lives as long as the match (new rule, below), because of the mid-match re-make.
- **§2.3 threat term and §3.5:** the AB stalemate on defended nodes, with free ones left alone, is what the threat term and pass 3 address. The threat term is now backed by runtime evidence, not only code.
- **§2.7 tether:** cause C, now seen on all three maps, is its main justification.
- **§3.1 "guards stay until conversion" on AB:** still rests on code and the AV evidence. The AB part is unconfirmed at runtime.
- **§3.2 Eye fixes:** code-only until an Eye run.

## 2. Core (Step 2)

### 2.1 Coordinator and tick
- One `BgPlan` per instance, with one `TeamPlan` per team (exists). It ticks from the module world script (`BgScripts.cpp:257`) on the world thread. Bots read their assignment on the same thread, so no locks are needed (H).
- **The tick is 1000 ms while `Bg.Coordinator = 1`, set by `Bg.Coordinator.TickMs`.**
  - The old path keeps `Bg.Planner.TickMs`, which the live conf pins at 3000 (`playerbots.conf:557`).
  - Why 1 s:
    - Some assignments have to follow a team event within a second: escorts and hunters after a flag pickup, a returner after a drop, and the retake squad once an assault starts.
    - A bot already at a node acts on an enemy cast from its own side, every bot tick, not through the planner.
    - A human reacts in about a second.
    - The cost is small (2.11).
  - Churn is held down by commitment (2.5), not by a slow tick.
- The first-tick stagger by instance id stays.
- **Plan lifetime = match lifetime (new, from the probe):**
  - On the coordinator path, a plan is dropped only at `STATUS_WAIT_LEAVE`, when its map is gone (`sMapMgr->FindMap` returns null), or when the map leaves the config. The 30 s no-touch drop (`BgPlanner.cpp:485`) does not apply.
  - Bots touch the plan only from `Play`, which does not run while they fight. In the 11-10 WSG run the plan was re-made 10 minutes into the match, which reseeded the split and reset every job and commitment (mod-playerbots#33).
  - Creation stays as today: the first `Touch` makes the plan. The old path keeps its rule (baseline rule).
  - W0 logs every plan create and drop, with the reason, at INFO. This confirms #33's cause and marks which arm-A matches re-planned, so that noise can be kept out of the A/B comparison.

### 2.2 Objective model and counting pass
Each objective is a generic `Objective` (`BgPlanner.h`). A mode keeps any extra data of its own in its own tables, indexed by objective; the core never reads it.

| Field | Meaning | Today |
|---|---|---|
| Type | node, tower, boss, mine, flag, flag room, cart, orb, wall, vehicle, rally, gate wait | `NodeKind` + `MapKind` |
| Owner, Assaulting, Destroyed, Alive | state | exist |
| Timer | assault left, total and since; respawn | `AssaultLeftMs`, `AssaultTotalMs`, `AssaultSinceMs` |
| Anchor | where the objective is: a fixed position, a unit (carrier, vehicle) or a GO (dropped flag); `Pos` is refreshed from it every tick. **Interface only for now:** AV uses fixed positions, and the unit and GO kinds are implemented with Warsong Gulch (B2) | `Pos` only |
| Value[team] | base value | `Weights[2]` (rows), `Weight` (code tables) |
| Quota, Guards | the most bots a Take task gets; how many stay on our running assault. One per row: each team attacks its own rows, and the shared Snowfall row has equal numbers | new |
| WEarly, WMid, WLate | phase multipliers (columns, 2.9) | new |
| Near[team] | living players within 40 yd of the anchor | `BuildPicture` only (`BgReact.cpp:489-494`), off live |

- **Counting pass (new, core):** every tick, one pass over the match's players fills `Near[team]` for each objective.
  - It counts living players only. The dead are never "near" and never count as threat.
  - It does not depend on `BuildPicture` or on any phase-4 key, which keeps it apart from the #31 suspects, as MoveOn's `PassOver` already is.
  - `BuildPicture` keeps its own job for the overlays.
  - Its cost is in 2.11.

### 2.3 Tasks, scoring and fill
- **Task:** what a mode offers a team this tick, made of `(objective, intent)`. Intents:
  - Take, Hold, Retake;
  - Escort, Hunt, Return;
  - Squad (a boss with a squad and a quota);
  - Push (the general: all or nothing);
  - Drive.

  Each task also carries a role spec, a quota (min, max), an optional staging point (2.4), a commit time and a combat policy. Tasks replace today's Attack/Defend `Job` plus objective; Attack/Defend survive only as the share split.
- **Score of a bot for a task:** `V x W[phase] x f - travel - threat`.

  | Term | Definition | Scale |
  |---|---|---|
  | `V x W[phase]` | the row's value times its phase weight | towers 8, graveyards 6, Snowfall 3 (§4) |
  | `f` | 1 up to the task's max quota; `Score.QuotaFalloffPct` (10%) past it. Applied to the value term only: applied to the whole score it would raise a negative score toward zero, and an over-quota task would attract bots | - |
  | travel | `Score.TravelPer100Yards` (0.2) x d/100, where d is the 2D distance from the bot to the anchor. No path queries | crossing AV (about 2,200 yd) costs 4.4, about half a tower |
  | threat | `Score.ThreatPerEnemy` (0.5) x min(`Score.ThreatCap` (6), max(0, enemies - allies)), from the counting pass | 1 net enemy = 0.5, about 250 yd of travel; 4 net = 2.0, a quarter of a tower; capped at 3.0 |

- **Not offered:** a task whose `V x W` is 0 or less, so a phase weight of 0 really excludes it.
- **Fallback when no task scores above 0:** the bot joins the main push. That is the team's Take, Burn, Squad or Push task with the most living bots on it (ties go to the higher `V x W`). With no such task, it holds the team's highest-value objective of its own; on a flag map that is our flag room.
  - Every fallback assignment is counted in the metrics (2.10).
  - Fallback means the offers did not fit the situation, and many bots on the same fallback push is herding. If more than 15% of a team's assignments in a match are fallbacks, the test flags that profile for redesign (§5.3).
- **Quota counting.** A living bot on a task counts 1. A dead bot, or a ghost waiting for the spirit healer wave, counts `Bg.Quota.DeadWeightPct` (50%) until it is alive again. Why half:
  - **Counting it as 1** leaves the task short for the death, the rez wave and the run back. In AV that is about 1-2 min, a third to a half of a 4-minute assault (rez interval M, run H).
  - **Counting it as 0** sends a full replacement for every death. When the dead return, the squad is over quota and has to release bots again. After a 6-bot wipe, the coordinator would pull 6 bots off other tasks at once for a loss that lasts a minute.
  - **At half**, a 6-bot wipe brings 3 fresh bots. The 6 returning make the squad 9 for 6. The release then takes the most recently assigned replacements first, never a bot inside its rejoin time (2.6).
- **Fill:** two passes over the team's free bots.
  - Free bots are those whose commitment ran out, bots new to the match, and released bots. They go in seeded-shuffle order (`BgRules::ShuffleKey`), so matches still vary.
  - Pass 1 brings Hold tasks (defenders, and the guards of our running assaults) up to their minimum.
  - Pass 2 gives each remaining bot its best task, counting live as it goes.
  - Pass 3 dissolves any Take or Burn task whose weighted count (living 1, dead at half) is above 0 and below `Bg.Squad.MinTake` (4). Its bots are filled again by pass 2 with that task closed, until no task is below the minimum. Three bots deep in enemy ground take nothing and feed kills (the §4 mid example shows the effect).
  - The weighted count is used so that a squad of 3 living and 6 dead waiting for the spirit healer is not dissolved: its dead come back to it. A unit test caught this.
  - Cost: O(bots x tasks) per pass; pass 3 repeats at most once per task.
  - The quotas add up to more than the team on purpose: they are caps, not demands, and the scores decide which tasks fill.
- **Hold share:** the Hold tasks together get the team's seeded defend share (`Bg.DefendMinPct`-`MaxPct`, 20-40%), as today.
- **Pushes.** Only the general is all or nothing (3.3); captains are Squad tasks with a quota.

### 2.4 Squads, roles, staging
- A squad is the set of bots on one task. Its roles come from the mode's spec, for example:
  - Take: Capper 1, Fighter the rest.
  - Escort: Escort 2, a healer first.
  - Squad on a captain: Puller 1 (a tank), Fighter the rest, healers standing off.
  - Drive: Driver 1 per vehicle, Gunner per free seat.
- The generic roles are Capper/Carrier, Fighter/Escort, Defender, Puller and Driver/Gunner. The mode decides which exist and how each is picked: the capper is the nearest non-healer within 30 yd of the anchor; the carrier is whoever holds the flag; the puller is the tank nearest the boss.
- **Fallback is in the mode.** With no capper within 30 yd, the nearest fighter caps. With no tank, the nearest melee pulls, then ranged, never a healer (today's rule). With no healer for an escort, a fighter escorts.
- Healers are a modifier, not a role: they follow their squad. `PlayHealer` exists and never follows another healer (`BgPlay.cpp:264`, `:276`).
- There is no follow-the-leader movement (owner, 2026-11-10).
- **Optional staging point per squad:**
  - A task may carry `Stage = (position, StageMin N, StageTimeoutMs)`. The squad starts in Staging: members walk to the staging point.
  - Once N living members are within `Bg.Squad.StageRadius` (30 yd), or the timeout (`Bg.Squad.StageTimeoutSeconds`, 60) runs out, the squad moves on to the anchor.
  - A member who joins after that goes straight to the anchor.
  - Rows give the staging point through the existing `rally` column (a rally row) and N through the existing `min_group` column.
  - A boss Squad runs today's rally state machine (`BgRules::RallyStep`) for that squad alone: Staging = Gathering, then Charging, Fighting, Regroup. The general's Push keeps it per team.

### 2.5 Commitment
- A bot keeps its task for `Bg.CommitSeconds` (30), unless:
  - **done:** the node is ours or burnt, the boss is dead, the flag is captured or returned;
  - **lost:** the mode drops the task (the enemy holds it beyond the retake window, or the hopeless rule in 3.3 fires), `NoteNoPath` is raised, or the objective is blocked;
  - **over quota:** the squad is over its max (release order in 2.3).
- After the commitment the bot moves only when the best task beats its current one by `max(Bg.CommitMarginMin` (1.0)`, Bg.CommitMarginPct` (25%) `x |current score|)`. Scores can sit near zero or below it, so a purely relative margin would let 0.1 beat 0.05.
- `RoleHoldSeconds` (45) still holds the defend share (`BgRules::Split`).

### 2.6 Respawn
- The assignment survives death (exists).
- At the rez (`WasAlive` false to true, `BgReact.cpp:560`), the bot gets `RejoinUntil = now + Bg.RejoinSeconds` (20). It is not rescored before then, and the over-quota release skips it.
- Until it is within 40 yd of its anchor, its combat policy is Travel (2.7).
- Rebuff runs first (exists).

### 2.7 Leash and combat (the fix for cause C)
- New combat trigger and action `bg tether`, at `Priority::Attack + 3` on the same "pvp" strategy line (`GenericStrategies.cpp:2051`). `bg leash` keeps its boss-room rule.
- **Path:** the bot's current mesh leg, from leg start to leg end as `Go` / `NextMeshLeg` planned it (`BgPlay.cpp:111-127`), stored per bot when the leg is planned. On AV's bent roads the straight start-to-anchor line runs far from the road.
  - With no leg in the last 15 s (b006f28), the path is the straight way from where the bot stood when it got its task to the anchor, so a defender that got its task at its node has only the objective's circle.
  - It is never the line from where the bot stands now: that line always contains the bot, so a bot chasing an enemy away from its task would keep the enemy in its zone (found in review before the first run).
- **Zone per role:**

| Role | Zone |
|---|---|
| Fighter, Defender | within `Bg.Tether.ObjectiveYards` (40) of the anchor, or `Bg.Tether.PathYards` (25) of the path |
| Capper | 20 yd of the anchor |
| Escort | 20 yd of the unit it escorts |
| Puller and boss squad | the boss-room leash (exists) |
| Carrier | none: it keeps running (`CarrierTrigger`, `BgPlay.cpp:1173`); its escorts answer for it |
| Travel (rejoining) | none |
| Driver | the vehicle's own fire only |

- **Retaliation, always allowed** for every role except Carrier and Driver. A unit that damaged the bot within the last `Bg.Tether.RetaliateSeconds` (6) may be engaged wherever it stands, on a budget of `Bg.Tether.RetaliateBudgetSeconds` (10) of fighting outside the zone.
  - The budget refills after 20 s inside the zone or out of combat.
  - Once it is spent, the attacker is dropped like any other target outside the zone.
  - A ranged attacker beyond the zone therefore never gets free damage, and cannot pull the bot away for good.
- **Recording damage:** the generic `UnitScript::OnDamage(attacker, victim, damage)` hook (`src/server/game/Scripting/ScriptMgr.h:428`), no core edit. It is called from `Unit::DealDamage` (`src/server/game/Entities/Unit/Unit.cpp:848`) for every hit on any unit, so it starts with an early exit before any other work:
  1. `victim` is not a player: return.
  2. `!victim->GetMap()->IsBattleground()`: return (`src/server/game/Maps/Map.h:380`, a map flag read).
  3. `!Playerbots::IsBot(victim->ToPlayer())`: return. This is the session's headless flag (`BotSession.h:33-41`); no container lookup.
- **Which thread runs what (H, cited). The coordinator tick and the hook do not share a thread:**
  - **Coordinator tick and BG bot AI:** world thread. The planner runs from the module `WorldScript::OnUpdate` (`BgScripts.cpp:257-259`). BG bots tick from `PlayerbotsScripts.cpp:93`, `:104` (`TickOtherMapBots`): `ForEachMapScript` gives `WorldMapScript`s only world maps (`src/server/game/Scripting/ScriptMgr.cpp:1603-1610`), so BG maps never reach the per-map hook at `PlayerbotsScripts.cpp:240`.
  - **Hook:** on the BG map's own thread for hits processed in that map's update (melee swings, periodic damage, spells landing).
  - **Hook, second case:** also on the world thread, when a bot action on the world thread deals damage at once (an instant spell that hits inside the cast) (M; possible through the bot-AI path above).
  - **The two phases never overlap.** `World::Update` runs `sMapMgr->Update` (`src/server/game/World/World.cpp:2348`), which waits for every map thread (`src/server/game/Maps/MapManager.cpp:348`, `m_updater.wait()`). Only then does it call `OnWorldUpdate` (`World.cpp:2465`). One bot lives on one map, so only one map thread can ever write its record.
- **Minimal safe design, needing no lock:**
  - A module table `BgDamage` maps a bot's guid to a fixed array of 4 slots of `(attacker guid, game time ms)`.
  - Entries are inserted and erased only on the world thread: inserted when the coordinator first sees the bot in a match, erased when the plan drops it.
  - The hook does `find()` only and writes into an entry it found. During the map phase the world thread is blocked in `m_updater.wait()`, so no insert or rehash can run alongside the map threads' reads. Concurrent `find()`s on an unchanging hash table are safe, and different map threads write different entries.
  - The hook never calls `sPlayerbotMgr->GetAI()` (`PlayerbotMgr.cpp:530`, a lookup in `_bots`), which keeps it away from the bot registry.
  - What remains M: that no thread other than map threads and the world thread deals damage. No such path is known.
- **Drop:** when the bot's victim is outside the zone and is not a retaliation case, the bot drops it with `PlayerbotAI::IgnoreEnemy(guid, ms)` (`src/PlayerbotAI.h:105`) and clears the forced target.
  - The ignore time starts at `Bg.Tether.IgnoreSeconds` (8) and doubles each time the same enemy is dropped again, up to `Bg.Tether.IgnoreMaxSeconds` (60). Otherwise the raid-attacker pickup would pick it again after 8 s and the bot would loop: drop, run, re-engage.
  - The doubling resets when the bot dies or its task changes, since the old zone no longer applies.
  - A retaliation attacker is never ignored while its budget lasts. `IsValidEnemy` filters the attackers loop (`PlayerbotAI.cpp:1172-1179`, `:1262-1264`), so an ignored attacker could not be fought back.
- **Verified (H):** with no combat target, the battleground action runs in combat.
  - The "pvp zone" node has no state filter (`Engine/Engine.h:96`, default `StrategyStates::All`; `GenericStrategies.cpp:2051`).
  - `PvpTrigger` does not check for combat (`GenericStrategies.cpp:507-511`).
  - `Bg::Play` refuses only while a capture cast runs, or during the Rebuff hold, which applies out of combat only (`BgPlay.cpp:1229-1266`).
  - So once "has combat target" (attack, 60) stops firing, "pvp" (30) moves the bot on.

### 2.8 Configuration (`BgConfig`; code defaults equal the dist)
Core keys, all under `Playerbots.Bg.`:

| Key | Default | Meaning |
|---|---|---|
| `Coordinator` | 0 | the task engine; 0 keeps today's planner path |
| `Coordinator.TickMs` | 1000 | the plan interval with `Coordinator = 1`; `Planner.TickMs` (3000) stays for the old path |
| `CommitSeconds` | 30 | 2.5 |
| `CommitMarginMin` / `CommitMarginPct` | 1.0 / 25 | 2.5 |
| `RejoinSeconds` | 20 | 2.6 |
| `Quota.DeadWeightPct` | 50 | 2.3 |
| `Score.TravelPer100Yards` | 0.2 | value units per 100 yd |
| `Score.ThreatPerEnemy` / `Score.ThreatCap` | 0.5 / 6 | 2.3 |
| `Score.QuotaFalloffPct` | 10 | the value left past a task's max quota |
| `Squad.StageRadius` / `Squad.StageTimeoutSeconds` | 30 / 60 | 2.4 |
| `Tether` | 0 | 2.7 on/off |
| `Tether.ObjectiveYards` / `PathYards` / `IgnoreSeconds` / `IgnoreMaxSeconds` | 40 / 25 / 8 / 60 | 2.7 |
| `Squad.MinTake` | 4 | 2.3 pass 3 |
| `Tether.RetaliateSeconds` / `RetaliateBudgetSeconds` | 6 / 10 | 2.7 |
| `Metrics` | 0 | 2.10 |

- Kept as they are: `DefendMinPct`/`MaxPct`, `RoleHoldSeconds`, `Rally.*`, `LegYards`, `Rebuff`, `FillToMax`, `LeavePerScan`, and the 14 reactive keys.
- `Rally.PullDefenders` now never pulls the guards of a running assault.
- `SoftGateMinutes` and `SoftGateScorePct` read play time on the coordinator path, and act as the general push's gate.
- Per mode and per map: `Playerbots.Bg.<Mode>.<Key>` is the default, and `Playerbots.Bg.<Map>.<Key>` wins over it.
  - Modes: `Node`, `Flag`, `Lane`, `Siege`.
  - Maps: `AV`, `WSG`, `TP`, `AB`, `BFG`, `EOTS` (later `IOC`, `SOTA`, `SSM`, `TOK`, `SS`, `DR`).

| Key | Default | Map overrides |
|---|---|---|
| `Node.HoldTarget` | 0 = a majority | `AB.HoldTarget` 3, `BFG.HoldTarget` 2, `EOTS.HoldTarget` 2 (M, tuning) |
| `Node.TakeMax`, `Node.Guards` | 6, 2 | |
| `Flag.RoomDefenders`, `Flag.Escorts`, `Flag.Hunters`, `Flag.Runners` | 2, 2, 4, 5 | |
| `Lane.Guards` | 2 | |
| `Lane.MidAfterMinutes` | 6 | |
| `Lane.PushMarginPct` / `Lane.PushHoldSeconds` / `Lane.PushDropSeconds` | 20 / 10 / 30 | 3.3 |
| `AV.Captain` | 1 | 0 = no captain Squad tasks |

### 2.9 Data: `playerbots_bg_node`
- Config cannot hold per-objective rows (positions, world-state ids, values, requires), so the existing table stays. New columns:
  - `attack_quota TINYINT UNSIGNED NOT NULL DEFAULT 0` (0 = the mode default);
  - `guards TINYINT UNSIGNED NOT NULL DEFAULT 0` (0 = the mode default);
  - `w_early FLOAT NOT NULL DEFAULT 1`, `w_mid FLOAT NOT NULL DEFAULT 1`, `w_late FLOAT NOT NULL DEFAULT 1`: the phase multipliers, one column each (owner: no packed string).
  - `ally_value FLOAT NOT NULL DEFAULT 0`, `horde_value FLOAT NOT NULL DEFAULT 0`: the coordinator's value per team (0 = the row's weight).
    - The §4 values cannot go into `ally_weight` / `horde_weight`: the old path reads those, and changing them would change arm A (the baseline rule).
    - Each team's value is its attack value on the rows it attacks and its hold value on its own rows; the phase weights multiply only the attack value.
- As built (W7), the columns are added to the existing `2026_11_06_00_world_mod_playerbots_bg_node.sql`, which drops and recreates the table. A changed file is applied again at the next boot. The §4 values are `UPDATE`s at its end; the old weights are untouched.
- The staging point and its N reuse the existing `rally` and `min_group` columns.
- Which value wins: the row's non-default value, then `Playerbots.Bg.<Map>.<Key>`, then `Playerbots.Bg.<Mode>.<Key>`.
- **Pitfall:** a query on a missing column aborts worldserver 10 s later (TrinityCore `CLAUDE.md`). The `ALTER` ships as module SQL together with the binary that reads it. The loader checks first that the columns exist, and falls back to the defaults when they don't.
- The code-table maps keep their code tables; their quotas and weights come from mode and map keys. Moving them into rows is backlog (B7).

### 2.10 Debug and metrics
- `.bot bg squads [instance]`, per team and per task, shows:
  - objective, intent and squad state (Staging N/M, Moving, Charging, ...);
  - roles: capper, fighters, guards, healers, the dead counted at half;
  - score parts: `V x W`, travel, threat;
  - committed until.

  `.bot bg` gains a phase line per team.
- With `Bg.Metrics = 1`, the logger `server.playerbots.bg` writes to its own appender file. It needs its own `Appender`/`Logger` lines, or INFO is dropped (`modules/CLAUDE.md`).
  - **Every minute, per team:**
    - per objective, the bots assigned (dead at half) and the bots present (from the counting pass);
    - bot-seconds, deaths and score;
    - fallback assignments, and their share of all assignments;
    - tether drops, retaliations and re-engages.
  - **At the end of each match:**
    - play time and winner;
    - final score;
    - the time of the first assault on each objective;
    - when the main force reached the first tower;
    - captures, kills and deaths per team.
- `.bot bg metrics` gives the win rate per map since boot. The log keeps totals across boots.

### 2.11 Performance per tick (the largest map: AV, 80 players, 27 rows)
- **Counting pass:** 80 x 27 = 2.2k 2D distances. The old path's `Picture`, when on, is a second pass of the same size.
- **State:** at most 27 x 5 world-state lookups.
- **Tasks:** about 20 per team. Scoring is 40 x 20 x 2 = 1.6k, each a few flops plus one distance. The fill sorts 20 candidates per bot.
- **Total:** about 12k simple operations per tick, tens of microseconds (M, estimate).
- **Measured today (M):** the only planner measurement on record is from the 11-09 legs A/B log, and it is uncertain which match load it covers: `bg planner` averaged 2.0 us per world tick, max 1.9 ms per plan. At a 1 s tick, the worst case is about 2 ms/s per AV instance.
- **Bot side:** pvp was 0.26 ms per call (11-07). The tether is one point-to-segment distance per in-combat bot per check. `OnDamage` is one slot write per hit on a bot.
- Step 5 measures all of it.

## 3. Modes (Step 3)

Interface (sketch; final names at implementation):

```cpp
enum class Intent : uint8 { Take, Hold, Retake, Escort, Hunt, Return, Squad, Push, Drive };
enum class Role : uint8 { None, Capper, Carrier, Fighter, Escort, Defender, Puller, Driver, Gunner };
enum class Phase : uint8 { Early, Mid, Late };
enum class AnchorKind : uint8 { Position, Unit, GameObject };   // Unit and GameObject: interface only until B2 (WSG)
struct RoleSlot { Role Kind; uint8 Count; bool HealerFirst; };
struct Stage { int8 Row = -1; uint8 Min = 0; uint32 TimeoutMs = 0; };
struct Task
{
    int8 Objective; Intent Want; float Value; uint8 QuotaMin, QuotaMax;
    std::vector<RoleSlot> Roles; Stage Staging; uint32 CommitMs; Role CombatPolicy;
};

class Mode
{
public:
    virtual ~Mode() = default;
    virtual char const* Name() const = 0;
    /// the tasks this mode offers the team now (value before phase weight, travel and threat)
    virtual void Offer(BgPlan const& plan, TeamId team, Phase phase, std::vector<Task>& out) const = 0;
    /// done (completed) or lost: the squad is released at once
    virtual bool Ends(BgPlan const& plan, TeamId team, Task const& task) const = 0;
    /// who of the squad takes a role; the mode's fallback when nobody fits
    virtual Player* PickRole(BgPlan const& plan, Task const& task, Role role, std::vector<Player*> const& squad) const;
    /// bot side, out of combat: what this bot does for its task and role
    virtual bool Play(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job) const = 0;
};

class Adapter   // the profile's thin state reader
{
public:
    virtual ~Adapter() = default;
    virtual void Read(BgPlan& plan, Battleground* bg, Map* map, uint32 now) const = 0;
};

struct Profile
{
    uint32 MapId; char const* Short;        // "AV": the Playerbots.Bg.AV.* section
    MapDef const* Def;                      // objectives (code table or rows)
    std::vector<Mode const*> Modes;         // several modes: interface only until B2/B3; AV has one (Lane)
    Adapter const* State;
    std::function<Phase(BgPlan const&, Battleground const*, TeamId)> PhaseOf;
};
```

Today's code stays as it is, and the new modes are written beside it (the baseline rule, §6):
- The Lane mode is the coordinator's counterpart of `PlanValleyTeam` and `PlayValley`/`PlayPush`.
- The Node mode (B1) and the Flag mode (B2) are the counterparts of `PlanTeam`'s capture and flag parts.
- The adapters do what `Discover`/`ReadState` and `ReadValley` do today.
- The modes call the pure rules and the banner helpers (`Interact`, `FindBanner`, `TeammateOpening`) without changing them.
- The old functions are deleted only after the owner switches to the coordinator.

The phase-4 overlays stay as overlays over the task (`Assignment::Over`, highest first):
- ThreatResponse becomes a source of Retake tasks (B10);
- FallBack stays next to the tether;
- FocusFire is unchanged.

### 3.1 Node control (AB, BfG, Eye towers, IoC nodes; backlog B1)
- **Offers:**
  - **Take:** a node that is not ours and not under our assault; quota up to `Node.TakeMax`.
  - **Hold:** ours, shared by value.
  - **Our running assault:** exactly `Node.Guards` (1-2) hold it until it converts; everyone else is released in the same tick. This is the MoveOn cap's idea, implemented again in the mode.
  - **Retake:** ours under their assault, while the time left on the assault exceeds the travel time at mount speed.
- **Hold-count target:** while we hold at least `HoldTarget` nodes, Take values are x 0.3. If we are late and behind, the node with the fewest enemies near it keeps its full value (folds in ScoreClock #4).
- **Roles:** Take and Retake = Capper 1 + Fighters; Hold = Defenders. Fallback: the nearest fighter caps.
- **Timers:** IoC 1 min (agent survey, M); AB and BfG read the GO's own `AssaultTimer` (exists).

### 3.2 Capture the flag (WSG, TP, the Eye flag; later Kotmogu orbs, the Deephaul crystal; backlog B2)
- **Objectives:**
  - their flag, anchored at its base, its dropped GO or our carrier;
  - our flag, anchored at its base, their carrier, or dropped;
  - our flag room.

  These moving anchors are implemented here, first.
- **Offers:**
  - Take their flag: `Flag.Runners`.
  - Escort our carrier: `Flag.Escorts`, a healer first.
  - Hold our flag room: `Flag.RoomDefenders`.
  - Hunt their carrier, only while our flag is out: `Flag.Hunters`, anchored on their carrier.
  - Return our dropped flag: the nearest 2.
  - Pick up their dropped flag: the nearest 2.
- **Carrier:** the role of whoever holds a flag; no assignment is needed.
  - It runs home with combat policy None.
  - While our flag is out, it kites in our room (`CarrierPlay` logic, `BgPlay.cpp:1081-1172`).
- **Standoff (both flags out):** a Flag-mode state, not a phase. While it holds, the mode offers Escort 3 and Hunters 5.
- **Eye:**
  - The flag is captured at our nearest held tower.
  - With no tower held, the carrier waits by the middle and the Node mode doubles its Take values. This fixes the fall back to the chase at `BgPlay.cpp:491`.
  - Runners are offered from the start, which fixes `BgPlanner.cpp:895-900`.

### 3.3 Lane push (AV; the boss phase of IoC)
- **Objectives:**
  - enemy towers and bunkers (Burn);
  - enemy graveyards (Take, with the same banner click);
  - captains (Squad);
  - the general (Push);
  - rally rows (staging).

  Each objective's value is V x W[phase].
- **Burn and Take:**
  - Capper 1 + Fighters.
  - Our running assault keeps `Lane.Guards` (1-2) until it converts (AV: 4 min, `alterac_valley.cpp:165`, H); the rest are reassigned in the same tick.
  - Our own graveyards and towers are Hold tasks with the hold values in §4.
- **Captain = Squad task** (owner: a mid-phase squad with a quota, not a boss push):
  - Its quota comes from the row (`attack_quota`).
  - Its staging point is the rally row, with N = `min_group`.
  - It runs its own copy of the rally state machine, with the boss-room leash (exists).
  - Its value is set from what the kill does (§4).
- **General = Push**, all or nothing:
  - **Starts** when, for `Lane.PushHoldSeconds` (10) of consecutive ticks, either the soft gate is open (late phase) or the general's `V x W` is at least (1 + `Lane.PushMarginPct` 20%) x the best other Take or Burn value.
  - **Takes** every Lane attacker except the guards of our running assaults and Hold defenders. Guards stay; `Rally.PullDefenders` pulls plain defenders only.
  - **Ends** through the rally state machine (Done, Abandoned). While it is only Gathering, it also ends if both start conditions stay false for `Lane.PushDropSeconds` (30). It never ends from scores while Charging, Fighting or Regroup (today's lock, `BgPlanner.cpp:1224-1225`).
  - The margin and the two hold times are the hysteresis.
- **Hopeless rule** (MoveOn's `PassOver` idea, implemented in the mode from the counting pass): a Take or Burn target that at least 4 enemies hold at 2:1 against us is skipped for 60 s, if another task exists.

### 3.4 Siege and vehicles (IoC, SotA; later Wintergrasp; backlog B4, B5)
- **Objectives:**
  - vehicle spawns, gated by their node;
  - walls and gates (type 33 health);
  - siege rows;
  - bosses that exist only once a gate falls.
- **Offers:**
  - Drive: one per free vehicle while we hold its node.
  - Gun: one per free passenger seat.
  - Escort the vehicle: Fighters 3, anchored on the vehicle unit.
  - Bomb: Seaforium carriers to a gate.
- **Driver:**
  - It never leaves the vehicle, and nobody drives at a boss (the IoC script kills a vehicle that hits one).
  - It uses the calls proven in the 5.0 spike: `Unit::EnterVehicle`, a spline on the charmed base, `vehicleBase->CastSpell` (BG_PLAN.md §1 Vehicle note, H for those four steps).

### 3.5 Arathi Basin adapter (sketch)
- **Profile:** `AB` 2107, Modes {Node}.
  - Objectives: the five type-42 code-table entries (`BgMapData.cpp:36-43`).
  - Adapter: today's `Discover` + `ReadState` (CapturePoint state, assault timer).
- **Values:** 5 each; the node next to our own base gets +1 in the early phase.
- **Phases:**
  - **early:** the first 90 s of play. Take the 3 nearest nodes, quota 5 each.
  - **mid:** `HoldTarget` 3; Hold by value; Retake on assault.
  - **late:** behind, with the enemy at most 3 minutes from winning (the score-rate race, `BgRules::ProjectRace`, `BgReactRules.h:186`). Take the node with the fewest enemies.
- **Roles:** the Node mode's, with 1 guard per running assault (the 6 s cast plus the 60 s timer).
- **Core needs:** nothing beyond 2.2-2.7.

### 3.6 Warsong Gulch adapter (sketch; Twin Peaks the same with its own entries)
- **Profile:** `WSG` 2106, Modes {Flag}.
  - Objectives: the two flags (`BgMapData.cpp:34-35`) and the two flag rooms (at the flag homes, `FlagInfo::Home`).
  - Adapter: today's flag reader (`GetFlagState`, `GetFlagCarrierGUID`, the `NEW_FLAG_DROP` lookup).
- **Phases:**
  - **early:** Take with the most; room defenders 2.
  - **mid:** after the first capture by either team.
  - **late:** 12 of the 15 minutes of play gone (the match ends at 17 elapsed, preparation included). Ahead: Hold heavy. Behind: Hunt plus Take, all in.
  - Standoff is a mode state (3.2).
- **Core needs:** moving anchors and the Escort, Hunt and Return intents (3.7).

### 3.7 Core changes the two adapters forced
Interfaces are made now (W2); implementation goes where noted:
1. Objectives can move: their anchor can be a unit or a GO, refreshed every tick. The interface is made now and implemented with Warsong Gulch (B2), not with AV.
2. Tasks with intents replace Attack/Defend plus objective (2.3). Implemented now.
3. A profile can have several modes. The interface is made now and implemented with Warsong Gulch (B2), first used by the Eye (B3). AV has one mode.
4. Role specs and their fallbacks live in the mode; roles are counted per squad (2.4). Implemented now.
5. Play time (`Battleground::GetInProgressDuration`, `Battleground.h:285`) for phases and every time rule. Implemented now.
6. A mode may keep its own state that changes its offers (the Flag standoff). The core's phase is early, mid or late, per team.

## 4. Alterac Valley profile (Step 4)

- **Modes:** {Lane}. Towers and bunkers are Burn, graveyards are Take, captains are Squad, the general is Push. The mines stay the #13 errand (`Bg.Mines`).
- **Adapter:** `ReadValley` (world states, exists), plus one quirk. While Snowfall has never been captured, either team can re-assault it (`battleground_alterac_valley.cpp:1088-1096`, `:1149`, H), so its Take task is offered to both teams. Each re-assault probably restarts the 4 minutes (M).
- **Phase per team, on play time:**
  - **early:** from the start until both towers on our push path are under our assault or burnt, or `Lane.MidAfterMinutes` (6). The push-path towers are Iceblood Tower and Tower Point for the Alliance, Stonehearth Bunker and Icewing Bunker for the Horde.
  - **mid:** until 2 of the 4 enemy towers are burnt (the general's `requires`, unchanged), or enemy reinforcements fall to `SoftGateScorePct` (40%), or `SoftGateMinutes` (20) of play pass.
  - **late:** the general's Push. The remaining towers keep a small value.
- **Captain value**, verified in the script (H):
  - A kill costs the captain's team 100 reinforcements (`battleground_alterac_valley.cpp:681`, `:691`, `RESOURCE_LOSS_CAPTAIN` `:554`), against 75 for a tower (`:553`).
  - It ends the team buff: Stormpike's Salvation 23693 / Fury of the Frostwolf 22751, aura 133 with base points 20, cast every 2-6 min while the captain lives (`:616-637`, DB2 SpellEffect).
  - It pays honor and reputation, which does not affect the outcome.
  - It changes no spawns: no `conditions` row reads the captain's alive world state 1351/1352. A tower does change spawns: it despawns a marshal or warmaster at the general.
  - So the captain is worth 8, the same as a push-path tower: more score than a tower, plus the buff, but no help against the general and a harder fight (the captain plus guards, a 50 yd leash).

Values, Alliance side. All are M (tuning) until the Step 5 tests.

| Row (`node`) | Intent | Value | `w_early, w_mid, w_late` | `attack_quota` | `guards` | Staging (`rally`, `min_group`) | Today's weight |
|---|---|---|---|---|---|---|---|
| `snowfall_gy` | Take | 3 | 1, 0.3, 0 | 4 | 1 | - | 9 |
| `iceblood_tower` | Burn | 8 | 1, 1, 0.5 | 12 | 2 | - | 7 |
| `tower_point` | Burn | 8 | 1, 1, 0.5 | 12 | 2 | - | 7 |
| `galvangar` | Squad | 8 | 0, 1, 0.3 | 12 | - | `rally_galvangar`, 8 | 8 |
| `iceblood_gy` | Take | 6 | 0.3, 1, 0.5 | 8 | 2 | - | 6 |
| `frostwolf_gy` | Take | 6 | 0, 1, 0.8 | 8 | 2 | - | 5 |
| `fw_east`, `fw_west` | Burn | 7 | 0, 0.8, 1 | 10 | 2 | - | 4 |
| `relief_hut` | Take | 4 | 0, 0.3, 0.8 | 6 | 1 | - | 3 |
| `drekthar` | Push | 10 | 0, 0, 1 | all (3.3) | - | `rally_drekthar`, floor 15 | 2 |
| `irondeep_mine`, `coldtooth_mine` | errand (#13) | 2 | 0, 0.5, 0.3 | 4 | - | - | 2 / 1 |

Horde side, each row mirroring the Alliance row of the same rank:

| Row | Intent | Value | `w_early, w_mid, w_late` | `attack_quota` | `guards` | Staging | Today's weight |
|---|---|---|---|---|---|---|---|
| `snowfall_gy` | Take | 3 | 1, 0.3, 0 | 4 | 1 | - | 9 |
| `stonehearth_bunker` | Burn | 8 | 1, 1, 0.5 | 12 | 2 | - | 7 |
| `icewing` | Burn | 8 | 1, 1, 0.5 | 12 | 2 | - | 7 |
| `balinda` | Squad | 8 | 0, 1, 0.3 | 12 | - | `rally_balinda`, 8 | 8 |
| `stonehearth_gy` | Take | 6 | 0.3, 1, 0.5 | 8 | 2 | - | 6 |
| `stormpike_gy` | Take | 6 | 0, 1, 0.8 | 8 | 2 | - | 5 |
| `db_north`, `db_south` | Burn | 7 | 0, 0.8, 1 | 10 | 2 | - | 4 |
| `aid_station` | Take | 4 | 0, 0.3, 0.8 | 6 | 1 | - | 3 |
| `vanndar` | Push | 10 | 0, 0, 1 | all | - | `rally_vanndar`, floor 15 | 2 |

- **Hold values (our own objectives):** the graveyard nearest the front 3, the other graveyards 2, towers and bunkers 1, Snowfall 1 when held (today 9).
- **Rally:** the Galvangar staging point sits on the Horde's road to Snowfall. The Horde no longer sends everyone there (quota 4), and the tether covers the pull-in. If staging still fails, move the point with `.bot bg rally set` (exists).

**Worked opening (early phase, 40 bots per team, seeded defend share 30% = 12 defenders, 28 attackers, all at the gate).**
- Gates: Alliance (794.6, -493.5), Horde (-1382.1, -545.9), from the `gate_*` rows.
- Score = `V x W x f - travel - threat`. Threat is 0 at the opening, because nobody is near any objective yet.

| Team | Task | V x W | d (yd) | Travel | Threat | Score | Bots |
|---|---|---|---|---|---|---|---|
| Alliance | Iceblood Tower | 8 | 1,386 | 2.8 | 0 | 5.2 | 12 |
| Alliance | Tower Point | 8 | 1,568 | 3.1 | 0 | 4.9 | 12 |
| Alliance | Snowfall | 3 | 1,067 | 2.1 | 0 | 0.9 | 4 |
| Alliance | Iceblood GY | 1.8 | 1,411 | 2.8 | 0 | -1.0 | 0 |
| Horde | Stonehearth Bunker | 8 | 1,234 | 2.5 | 0 | 5.5 | 12 |
| Horde | Icewing | 8 | 1,596 | 3.2 | 0 | 4.8 | 12 |
| Horde | Snowfall | 3 | 1,257 | 2.5 | 0 | 0.5 | 4 |
| Horde | Stonehearth GY | 1.8 | 1,460 | 2.9 | 0 | -1.1 | 0 |

- Captains (early weight 0) and mines (early weight 0) are not offered. Today's weights put all 26 Alliance and 19 Horde attackers on Snowfall (§1).
- The same fill comes out at 0.5 and 1.0 per 100 yd. At 1.0, though, every score is negative and Snowfall ties Tower Point for the Alliance (-7.7), so travel decides the picks; 0.2 keeps the value in charge.

Defenders (Hold tasks, `BgRules::Spread` over the hold values) at the seeded share's low, middle and high ends:

| Team | Share | Front graveyard | 2nd graveyard | 3rd graveyard | Towers / bunkers (1 each) |
|---|---|---|---|---|---|
| Alliance | 20% (8) | Stonehearth GY 2 | Stormpike GY 1 | Aid Station 1 | DB North 1, DB South 1, Icewing 1, Stonehearth Bunker 1 |
| Alliance | 30% (12) | Stonehearth GY 4 | Stormpike GY 2 | Aid Station 2 | 1, 1, 1, 1 |
| Alliance | 40% (16) | Stonehearth GY 4 | Stormpike GY 3 | Aid Station 3 | 2, 2, 1, 1 |
| Horde | 20% (8) | Iceblood GY 2 | Frostwolf GY 1 | Relief Hut 1 | Iceblood Tower 1, Tower Point 1, FW East 1, FW West 1 |
| Horde | 30% (12) | Iceblood GY 4 | Frostwolf GY 2 | Relief Hut 2 | 1, 1, 1, 1 |
| Horde | 40% (16) | Iceblood GY 4 | Frostwolf GY 3 | Relief Hut 3 | 2, 2, 1, 1 |

**Threat in a mid-phase pick.** One Horde bot has just rezzed at Iceblood GY (-612.7, -396.7). The Alliance holds Snowfall with 5 near it; the enemy and ally counts come from the counting pass.

| Task | V x W | d (yd) | Travel | Enemies / allies near | Threat | Score |
|---|---|---|---|---|---|---|
| Balinda (Squad) | 8 | 566 | 1.13 | 1 / 0 | 0.5 | 6.37 |
| Stonehearth Bunker (Burn) | 8 | 462 | 0.92 | 6 / 2 | 2.0 | 5.08 |
| Stonehearth GY (Take) | 6 | 687 | 1.37 | 2 / 1 | 0.5 | 4.13 |
| Snowfall (Take) | 0.9 | 499 | 1.00 | 5 / 0 | 2.5 | -2.60 |

- The bot is inside its 20 s rejoin time, so it goes back to its own task instead.
- Rescored later, it would go to the captain squad if that squad has room. A Snowfall that the enemy holds in force scores below zero and draws nobody.

**Mid-phase split (Alliance, the whole team).**
- **State:**
  - 40 bots, defend share 30% (12).
  - Our assaults on Iceblood Tower and Tower Point are running, with 2 guards each.
  - Snowfall is ours.
  - The Horde holds Iceblood GY (4 near it), Frostwolf GY (2 near), both Frostwolf towers (1 near each) and Relief Hut.
- **Attackers' positions:** 10 at Iceblood Tower, 10 at Tower Point, 4 at Snowfall, 4 freed defenders at Stonehearth GY. These are the 28 attackers.
- **Offers:**

| Task | V x W | Quota | Threat | Score from Iceblood Tower / Tower Point |
|---|---|---|---|---|
| Galvangar (Squad, staging 8) | 8 | 12 | 0.5 | 7.30 / 6.90 |
| Frostwolf GY | 6 | 8 | 1.0 | 3.97 / 4.37 |
| Iceblood GY | 6 | 8 | 2.0 | 3.72 / 3.68 |
| Frostwolf East / West | 5.6 | 10 / 10 | 0.5 | 3.63 / 4.03 (each about the same) |
| Relief Hut | 1.2 | 6 | 0 | -0.46 / -0.07 |

- **Quotas:** they total 54 against 28 attackers; the scores decide who goes where.
- **Pass 2 alone** (simulated, seeded order) gives: Galvangar 12, Frostwolf GY 8, Iceblood GY 5, Frostwolf East 3, Frostwolf West 0, Relief Hut 0.
- **Pass 3** dissolves Frostwolf East (3, below the minimum of 4). Its bots try Frostwolf West, which also stays below 4 and is dissolved, and they end on Iceblood GY.
- **Final split:**

| Task | Bots |
|---|---|
| Galvangar squad | 12 |
| Iceblood GY | 8 |
| Frostwolf GY | 8 |
| Guards (Iceblood Tower, Tower Point) | 2 + 2 |
| Hold (`BgRules::Spread` of the remaining 8 over hold values 3, 2, 2, 1, 1, 1, 1, 1) | Stonehearth GY 2; Stormpike GY 1; Aid Station 1; DB North 1, DB South 1, Icewing 1, Stonehearth Bunker 1; Snowfall 0 |
| **Total** | **40** |

- Snowfall, held, gets no defender at this share: its hold value of 1 is the smallest remainder. That keeps it the small detail §4 wants.

**Roles:**
- **Take and Burn:** Capper 1 (the nearest non-healer, casting 21651, 8 s), Fighters the rest, healers with the squad.
- **Our running assault:** 1-2 guards until conversion (4 min); everyone else is reassigned the same tick.
- **Captain Squad:** stage at the rally row until 8 are present (or 60 s), then the puller (tank) pulls.
- **General Push:** today's rally (floor 15, 80%).

**Pass criteria for Step 5:**
- after the first rush, at most 4 attackers plus 1 guard per team at Snowfall;
- the main force on the two push-path towers.

## 4a. Provenance and license
- New code in this plan is original and written for TC 12.1. It reuses and moves existing module code (GPL-2.0-or-later headers, `docs/design/PROVENANCE.md`).
- Already recorded: the AV rally rows come from AzerothCore mod-playerbots (GPL-2) road paths in `BattleGroundTactics.cpp` (BG_PLAN.md §2 rally rows; PROVENANCE.md).
- Anything adapted later from AzerothCore's `BattleGroundTactics.cpp` gets an adapted-from-AC row in PROVENANCE.md when it lands. The candidate today is the IoC siege positions `IC_GATE_ATTACK_POS_*` (BG_PLAN.md §1).
- Nothing comes from AGPL or unlicensed sources (PROVENANCE.md "Do not copy" list).
- The values in §4 are this plan's own tuning.

## 5. Test and measure plan (Step 5)

### 5.1 Repeatable bot-only matches
- **Harness (exists):** `.bot bg test start <map> [level]` runs one normal match with random bots only, and `stop` ends it. Matches are capped at 45 min (`BgScripts.cpp:721`, `:88`), and it works from the console through `wscmd.py`.
- **Runner:** the 11-10 `bgprobe.sh`, made generic as `trinitycore-tracking/tools/bg/bgmatch.sh <map> <arm> <matches> [--opening <minutes of play>]`. With `--opening`, it stops each test at that play time.
  - It lives outside the public module repo, because it holds this machine's paths, the docker layout and the QA guids.
  - The session scratchpad in RAM-backed `/tmp` would not last.
- **Memory headroom:** the 11-10 probe went down to about 4.1 GB available against the 3 GB abort line. AV preloads every grid of its map, so an AV window may abort on memory. Check MemAvailable at the start of each window, and run AV only with 8 GB or more available.
- **Safety rules** (all already in the probe):
  - Count 150, OnlyWithPlayers 0, for the run only.
  - Abort when MemAvailable < 3 GB, when the user logs in (account 2, excluding QA guids 754/755), or when the abort file appears.
  - Two-stage restore, with an md5 check against the backup.
  - Logs copied before every reload; peers told the times; a window agreed with the boot coordinator.
  - Detached (`setsid nohup`), watched by pid with `kill -0` (`pgrep -f` matches the watcher's own command line).
  - An optional console gap for peers.
- **Seed:** the split seed comes from the instance id and the match start (`BgRules::MatchSeed`), so two runs differ. W0 adds an optional seed to `.bot bg test start`, so an A/B pair can share one.
- **Arms:** all four run with the 14 phase-4 keys off (#31 unexplained), so nobody switches them on to help an arm.
  - A: today's planner (`Bg.Coordinator = 0`, live keys).
  - C: the coordinator only (`Bg.Coordinator = 1`, `Bg.Tether = 0`).
  - B: the coordinator with the tether (`Bg.Coordinator = 1`, `Bg.Tether = 1`).
  - If B misses a criterion, C shows whether the tether or the task engine is the cause.
- **Two kinds of run:**
  - **(a) Opening-only matches** for the Snowfall and first-tower metrics.
    - The runner stops the test at 5:00 of play.
    - Each costs about 9 min: 2 min of preparation, 5 min of play, and the drain. A 60-minute window holds about 6, interleaved A C B A C B.
    - At least 10 per arm before the opening criteria are judged.
  - **(b) Full matches** for duration, the captain and general pushes, the tether over time, and the win rate.
    - An AV match runs 18-30 min, so one window holds about 2.
    - At least 3 per arm for the push criteria.
    - **The win rate is reported, never a pass criterion below 20 full matches per arm.** Fewer matches cannot tell a real change from the seeded roll and the skill variance.
- **Windows and boots:**
  - Both kinds span several coordinator windows and boots.
  - Each window starts with fresh bots on a fresh conf reload and swaps the arm order from the window before.
  - Compare across boots only at matched bot counts (within 2 per team), and note each window's binary: it has to carry the arm's code.
  - Report the median and range, never one match.

### 5.2 Metrics (W0 writes them; all arms)
- **Every minute, per team:**
  - per objective: bots assigned (dead at half) and bots present within 40 yd (counting pass);
  - bot-seconds per objective;
  - deaths and score;
  - tether drops, retaliations and re-engages per bot.
- **Per match:**
  - play time and winner;
  - final score;
  - the first assault on each objective;
  - when the main force reached the first tower;
  - captures, kills and deaths per team.
- **Cost:**
  - `bg planner` average and max per plan (`.bot perf`);
  - the `pvp` action average;
  - the `bg tether` trigger average;
  - world tick p50, p95 and p99 from `Playerbots-Perf.log` over the match window.

### 5.3 Pass criteria
**Alterac Valley, both teams.** Criteria 1 and 2 are judged on the opening-only matches (a), and 3-7 on the full matches (b).
1. **Snowfall after the rush.** From 3:00 of play, to 5:00 in (a) and to the end in (b):
   - bots assigned to Snowfall per team stay within quota plus guards (4 + 1 = 5) in every sample;
   - bots present within 40 yd stay at 5 or fewer in at least 90% of the per-minute samples (fights can pull a few in for a minute).
2. **Main force at the first tower.** At least half of the team's Take and Burn attackers are within 40 yd of a push-path tower by **4:00 of play**, in at least 8 of 10 opening matches per team; 3:00 is the stretch goal.
   - Alliance: Iceblood Tower (1,386 yd in a straight line from the gate) or Tower Point (1,568).
   - Horde: Stonehearth Bunker (1,234) or Icewing (1,596).
   - Why 4:00: roads run about 1.3 x the straight line, so 1,600-1,800 yd. On 11-10 bots assaulted Snowfall (1,067 yd) about 120 s into play, about 10 yd/s including mounting and legs. That makes about 3 minutes, plus a minute of margin.
3. **Captain squad:** in at least 2 of 3 matches, a captain squad of each team reaches Charging after staging (8 present, or the timeout).
4. **The general push** reaches Charging in at least 2 of 3 matches.
5. **Tether:**
   - re-engages of the same enemy stay at 2 or fewer per bot per minute in at least 95% of the samples;
   - retaliations are seen when ranged enemies hit from outside the zone (tuning, not settled).
6. **Fallback share:** at most 15% of a team's assignments in a match are fallbacks. More flags the profile for redesign (herding risk); this is not a tuning fix.
7. **No regression:**
   - every match ends with a winner within 35 min of play;
   - the win rate is reported per arm; it becomes a criterion only at 20 or more full matches per arm;
   - `bg planner` at most 2 ms per plan;
   - `pvp` at most 0.5 ms per call;
   - world tick p95 no more than 20% above arm A in the same window.

**Other maps** are measured now (the 11-10 probe and arm A) and judged when their mode lands (backlog):
- **Warsong Gulch, Twin Peaks:** captures per match, carrier deaths, time from a pickup to escorts present, defenders' distance from the flag room.
- **Arathi Basin, Gilneas:** nodes held over time, attackers present 10 s after our cast lands, retakes in time.
- **Eye of the Storm:** flag captures, and runners before the first tower.

### 5.3a Results so far (W8)

**Window 1, 2026-11-10 19:46-20:35 CST** (run dir `trinitycore-tracking/tools/bg/runs/2026-11-10_1946_av_AC`).
- **Setup:**
  - 4 opening-only Alterac Valley matches, arms A, C, A, C, on seeds 101, 101, 102, 102. A 5th was skipped for time.
  - Binary of boot 22: no tether, so no arm B yet.
  - Teams were uneven: Alliance 40 against Horde 24-28, because of the test bracket's online bots.

| Criterion | Arm A (today's planner) | Arm C (coordinator, no tether) |
|---|---|---|
| 1 Snowfall after the rush | 1 / 4 team-matches (the Horde kept 18-19 assigned there) | 4 / 4 (Alliance 1 guard, Horde none) |
| 2 Main force at a push-path tower by 4:00 | 0 / 4 (best: Alliance at Iceblood Tower, 385 s) | 0 / 4 (never) |
| 6 Fallback share | 0% (no coordinator) | inflated by a counting bug, fixed in 9da1462 (true share about 4 of 32 Alliance attackers) |

- **The split works:** Alliance Iceblood Tower 13-16 and Tower Point 12; Horde Stonehearth Bunker 8-12 and Icewing 6-9; the captain squad formed in the mid phase (10-12 bots, gathering).
- **The towers were not reached:**
  - The tower squads never got there: at most 2 present at a push-path tower in 7 minutes.
  - The armies met mid-map instead; half of each team's attackers first massed at Iceblood Graveyard at about 460 s.
  - It looked like cause C, which the tether (arm B) addresses. Window 2 tested that, and the tether did not change arrival (below): the cause is open.
- **Sample:** 2 matches per arm of the 10 needed. No pass or fail is claimed.

**Window 2, 2026-11-10 21:19-22:05 CST** (run dir `trinitycore-tracking/tools/bg/runs/2026-11-10_2119_av_ACB`).
- **Setup:**
  - 4 opening-only matches, arms A, C, B, A, on seeds 201, 201, 201, 202. Matches 5 and 6 were skipped for time.
  - Binary of the 21:02 boot (installed 20:49, rev 132023350214+), with the tether (b5f962d, b006f28) and 9da1462.
  - Teams were uneven: Alliance 40 against Horde 28.
- **The crash:**
  - The worldserver crashed at 22:04:28, about 10 s after `bot bg test stop` ended match 4 (arm A).
  - The fault: `Unit::GetAuraApplication` called on a null Unit, on a map thread (Oreocide/mod-playerbots#36).
  - That match's metrics lines were lost with Server.log. Its two samples in `results.txt` (242 s and 362 s) are used instead.
  - The BG windows are paused until a stack is caught.

| Criterion | Arm A (today's planner) | Arm C (coordinator, no tether) | Arm B (coordinator + tether) |
|---|---|---|---|
| 1 Snowfall after the rush | 1 / 4 team-matches (Horde 17 assigned in match 1; match 4 Horde 2 assigned but 20 present at 242 s, most likely the Balinda push passing through) | 2 / 2 (Alliance and Horde 4 at most) | 2 / 2 (Alliance 1 at most, Horde 0) |
| 2 Main force at a push-path tower by 4:00 | 0 / 4 (no tower targeted: the whole push went to the captains, 19-20 present) | 0 / 2 | 0 / 2 |
| 6 Fallback share | 0% (no coordinator) | Alliance 7%, Horde 2% | **Alliance 18%, over the 15% flag**; Horde 0% |
| Tether drops / retaliations / same enemy again | - | - | Alliance 201 / 24 / 20, Horde 638 / 42 / 106 |

- **Both windows together:**
  - Criterion 1: arm A 3 of 8 team-matches, arm C 6 of 6, arm B 2 of 2.
  - Criterion 6: window 1's arm C figures (290%, 19%) come from the counter before 9da1462 and are left out.
  - Arm A fails criterion 1 on the assigned half: its Snowfall presence was 5 or fewer in most samples (the 17-19 Horde assigned there were mostly elsewhere). The coordinator removes that assignment. "The coordinator fixes camping" overstates it.
- **The split works, but the squads still do not arrive:**
  - The assignments matched §2. Alliance: Iceblood Tower 11-13, Tower Point 11-12. Horde: Stonehearth Bunker 2-12, Icewing 4-12.
  - That held through 480 s, except B moved Iceblood Tower's squad to the graveyards from 240 s (0-3 left).
  - Few of the assigned were dead: 1-4 dead in 10 of the push-tower samples, none in the rest.
  - Tower Point was assigned 11-12 with nobody present in both matches until 420 s (B: 6 present at 480 s). Icewing had nobody present all match in both.
  - Iceblood Tower peaked at 3 present (C) and 6 (B). Stonehearth Bunker peaked at 2.
  - The mid-phase captain squad (Galvangar, 9-13 assigned) never had anyone present.
  - The same non-arrival shows in arm A: the Horde's 9-18 Snowfall-assigned bots had 0 present all through window 2's match 1. Only the old path's captain rally groups arrived (Galvangar 19, Balinda 20).
- **The tether did not change arrival:**
  - Arm B made 638 Horde drops, yet the Horde presence at Stonehearth and Icewing stayed where arm C had it.
  - 106 of the 638 dropped the same enemy again, so a dropped enemy is often picked again.
  - One match, Medium.
- **The navmesh is connected. The mover is not ruled out:**
  - `trinitycore-tracking/tools/bg/navcheck.cpp` is an offline Detour query on `docker/data/mmaps`. It mirrors the core's player filter and poly search (`PathGenerator.cpp:141-152`, `:650-673`).
  - Map 30 has 28 tiles. From each team's gate and graveyards, Iceblood Tower, Tower Point, both captains, the rally points and Snowfall route complete.
  - All four Alliance bunkers route partially from everywhere, but each path ends 13-18 yd from the anchor in 2D, inside the 40 yd presence circle (`BgMetrics.cpp:34`, `:147`).
  - The movement target and the presence point are the same: both come from `ObjectivePosition` (`BgPlay.cpp:440`, `BgMetrics.cpp:114`).
  - These routes are 700-2400 yd long. The core's smooth path gives up at 74 points, about 300 yd (`PathGenerator.cpp:951`). The bots get past that only through `Bg.LegYards` legs, which is 180 in the live conf. So navcheck shows the mesh is connected, not that the bots' mover gets there.
- **A lead, not yet the cause (Medium-Low):**
  - `PlayValley` calls `Engage(bot, ai, pos, GuardRange)` before `Go` at any distance (`BgPlay.cpp:684-685`).
  - `Engage` makes the enemy player nearest the *objective* a forced target, however far away the bot is (`BgPlay.cpp:144-164`).
  - With `Combat.RoleTargeting = 1` (live), a forced target comes first and skips the chase-distance check (`PlayerbotAI.cpp:1244-1246`; compare `withinChase` at `:1215`).
  - So a tower squad member anywhere on the map can take a defender at its tower as its target.
  - **What fits:**
    - The tasks `PlayValley` runs never fill: the arm A Horde Snowfall assignees, and the C/B tower squads.
    - The captain pushes do arrive: `PlayPush` engages only around its rally point, and the rallies stand away from hostile spawns (`BgPlay.cpp:610-613`).
  - **What does not fit yet:**
    - The C/B captain squads also go through `PlayPush`, yet nobody reached their rally. Maybe they were taken from parked tower squads still holding the old forced target; this is unchecked.
  - **The check:** `bot state` (position, combat target, last action) on tower-squad members during a window.
- **Criterion 2 as defined cannot pass with the approved split:**
  - `BgMetrics.cpp:183` counts a tower as massed only when the bots at one objective, doubled, reach the team's living attackers.
  - The opening sends 12 + 11 of 27 Alliance attackers to two towers: 24 < 27 even with full arrival. Horde Icewing: 14 < 18.
  - **Open decision (user):** either keep the definition and change the split, or measure arrival per push-path tower. Proposed per-tower measure: at least half of the tower's own squad within 40 yd, and at least 4 (`Squad.MinTake`). A pair-wide measure is the alternative: the team's two push-path towers together hold half the living attackers.
  - Window 2 under each, best value by 300 s:
    - Per squad: C Alliance 25%, Horde 2 of a shrunken squad of 2 (fails the 4 floor); B Alliance 46% (6 of 13 at 180 s), Horde 18%.
    - Pair-wide: C 11% / 12%, B 24% / 12%.
  - No arm passes either.
- **Next:**
  - A position diagnostic for assigned bots that are not present. It needs no build:
    - `bgmatch.sh --states`, with the sampled names taken from the tower squads (`.bot bg squads`, `.bot bg roles`), not the first three attackers;
    - `Logger.server.playerbots.stuck=2` for the window, the 11-07 method, which shows the no-path lines.
  - Then catch the #36 stack. Only then more windows.
- **Sample:** arm A 4 matches, arm C 4, arm B 1. No pass or fail is claimed.

### 5.4 Unit tests (`tests/bg_rules_test.cpp`, run by `tests/run_engine_test.sh`; both exist)
- **Score and fill:**
  - the §4 worked opening (4 / 12 / 12 per team) and the defender table;
  - the mid-phase threat table;
  - the falloff with negative scores;
  - a task with `V x W <= 0` is not offered;
  - the fallback when nothing scores above 0, and its count;
  - pass 3 dissolving squads below `Squad.MinTake`: the §4 mid-phase split, 12 / 8 / 8 plus the guards and holds, 40 in all.
- **Commitment:** the margin with a current score near 0 and below it.
- **Quota counting:** dead at half; the release order after a wipe.
- **Respawn:** no rescore during `RejoinSeconds`.
- **Staging:** N present, timeout, a late joiner.
- **General Push hysteresis:** start after 10 s over the margin, drop only while Gathering after 30 s, never while Charging.
- **Tether zones:** the leg corridor, the objective radius, the retaliation budget, and the doubling ignore (capped at 60 s, reset on death and on a task change).
- **The `OnDamage` early exit:** a non-player victim, a non-battleground map and a non-bot each return before the table lookup (a pure predicate, testable).
- **Phases:** the AV phase rules over towers burnt, score and play time.

## 6. Implementation plan (Step 6)

Order: core first, then the Lane mode, then the AV profile, as the brief asks. AV needs only the Lane mode, so moving anchors and several modes per map stay interfaces until Warsong Gulch (B2).

Every item is one commit in `Oreocide/mod-playerbots`, behind a key that defaults to off.
- **Baseline rule (owner):** with `Bg.Coordinator = 0`, behaviour stays identical to today through every item, so arm A is a true baseline.
  - The coordinator path is purely additive: new types, modes and adapters sit beside the old code, and the Lane mode gets its own copy of what it needs.
  - These stay unchanged until the owner switches to the coordinator, and are deleted only after that: `PlanTeam`, `PlanValleyTeam`, `StepRally`, `Discover`, `ReadState`, `ReadValley`, `PlaceDefenders`, `SplitTeam`, `PassOver`, the `Play*` functions, `FlagInfo`, the MoveOn cap.
  - **Identity check, at source level:** the diff of those functions is empty except for the dispatch lines, and the unit tests pass. A headless match cannot prove identity, because matches differ in timing and positions even with a shared seed.
  - **Dispatch lines as built:**
    - `Plan` and `Update` in `BgPlanner.cpp`;
    - `PlayPush` and `LeashOf` in `BgPlay.cpp` (W6b: a coordinated captain squad's own rally state and puller).
    - With the key off, `Coordinator::SquadPush` returns false and leaves the team's state as it was.
  - W0's metrics and plan logging only read state.
- **Commits:** no attribution lines. The owner waived showing the messages first for these work items (2026-11-10); nothing is pushed without approval.
- **New files:** a new `.cpp` needs `cmake .` run with no other build going (`modules/CLAUDE.md`).
- **Header layout:** a change to `BgPlanner.h` needs a `touch` and a full module rebuild (the header-layout pitfall).
- **Shared files:** `GenericStrategies.cpp`, `PlayerbotsScripts.cpp` and the conf.dist hold other sessions' hunks; splice only ours.

| # | Change | Files | Test | Risk |
|---|---|---|---|---|
| W-1 | Commit MoveOn first. It is uncommitted in the same files (`BgConfig.*` +6, `BgPlanner.cpp` +151/-28, `BgReactRules.h` +79), and live behind `Bg.MoveOn = 0` since 11-10 08:41. The message is shown to the owner first | the MoveOn hunks only (other sessions' conf.dist hunks stay out) | export syntax-compile with the build flags; `bg_rules_test` | low: no behaviour change, key 0. Without it every later commit has to splice around MoveOn, and arm A has no clean reference |
| W0 | Metrics, plan logging, test seed: per-minute and per-match lines (`Bg.Metrics`) with their own counting pass; plan create and drop with the reason at INFO; `.bot bg metrics`; optional seed on `.bot bg test start`. The runner goes to `trinitycore-tracking/tools/bg/` (not this repo) | new `BgMetrics.{h,cpp}`; `BgPlanner.cpp` (one call at the end of `Plan`, one log line at create and at drop); `BgScripts.cpp`; `BgConfig.*`; conf.dist (key + `Appender`/`Logger` lines) | arm A baseline: AV, WSG, AB and the Eye, one window each to start; the log lines appear | low (reads and logs only) |
| W1 | Play-time clock on the coordinator path only (owner). With `Coordinator = 0`, every decision reads the same clock as today, so arm A stays a true baseline. `.bot bg` adds a play-time field next to the elapsed one (display only) | `BgPlanner.cpp:1180`; `BgReact.cpp:323`; `BgScripts.cpp:474` | unit tests unchanged; a headless match with `Coordinator = 0` decides as arm A did | low |
| W2 | Core model and interfaces, additive: `Task`/`Intent`/`Role`/`Phase`/`Stage`, `Mode`/`Adapter`/`Profile`, `AnchorKind` (Position implemented, Unit/GameObject declared), and the coordinator's own state per plan. Existing structs and functions unchanged | new `BgMode.h`, `BgCoordinator.{h,cpp}`, `BgProfiles.cpp`; `BgPlanner.h` (one new member holding the coordinator state) | build; unit tests; the source-level identity check | medium: header layout (touch + full module rebuild) |
| W3 | Pure task rules: score, threat, two-pass fill, dead weight and release order, fallback, commitment margin, rejoin, staging, general Push hysteresis, tether zones and budget, AV phases | new `BgTaskRules.h`; `tests/bg_rules_test.cpp` | §5.4 | low (pure) |
| W4 | Coordinator path with `Bg.Coordinator = 1`: plan lifetime = match lifetime (2.1), counting pass, read, phase, offers, fill, squads and roles, staging, assignment. The bot side dispatches to `Mode::Play`. Adds `.bot bg squads` and the phase line | `BgPlanner.{h,cpp}`; `BgPlay.{h,cpp}`; `BgScripts.cpp`; `BgConfig.*`; conf.dist | arm C headless AV with mode defaults | medium-high: the biggest diff; the old path must stay as it is |
| W5 | Tether and retaliation: `bg tether` trigger and action; `Go` stores the current leg; `UnitScript::OnDamage` records hits on bots; rejoin uses the Travel policy | `BgPlay.{h,cpp}`; new `BgDamage.cpp` (the `UnitScript`); `GenericStrategies.cpp` (one node on the pvp line); `BgConfig.*`; conf.dist | unit zone and budget tests; arm B vs C: bots in combat more than 60 yd from their anchor; re-engage counts | medium: combat behaviour; the map-thread write rule (2.7, M); shared `GenericStrategies.cpp` |
| W6 | Lane mode: Burn, Take and Hold on banners; guards until conversion; captain Squad with staging (its own rally state per squad, using the pure `BgRules::RallyStep`); general Push with hysteresis; the hopeless rule; phase weights. Written new beside `PlanValleyTeam`, which stays as it is. The pure rules (`RallyStep`, `SpreadCapped`) are called, not moved | new `BgModeLane.cpp` | unit offers for a given state; arm C headless AV; the identity check | medium |
| W7 | AV profile: an `ALTER` adding `attack_quota`, `guards`, `w_early`, `w_mid`, `w_late`; an `UPDATE` of the AV rows (§4: values, quotas, guards, weights, `min_group` 8 on the captains); the loader checks the columns exist first; the Snowfall quirk; the AV phase rules; keys `Lane.*`, `AV.Captain` (1) | new `data/sql/db-world/2026_11_xx_00_world_mod_playerbots_bg_node_quota.sql` (name checked free first); `BgMapData.{h,cpp}`; `BgProfiles.cpp`; conf.dist | boot log `loaded N battleground objective rows`, no DB errors; §5.3 AV criteria, A/C/B over several windows | medium: the SQL and the binary must ship together (a missing column aborts worldserver); a changed SQL file re-applies at boot |
| W8 | AV acceptance and tuning: the arms until §5.3 passes; tune rows and keys; then ask the owner to set `Bg.Coordinator = 1` and `Bg.Tether = 1` live | rows, conf | §5.3 | low |

Order: W-1, W0, W1, W3 (pure, so it can come before W2), W2, W4, W5, W6, W7, W8. W0 comes early so arm A has numbers.

**Status 2026-11-10 (local main, not pushed):**

| Done | Commit |
|---|---|
| W-1 MoveOn | 96c5a58 |
| #33 fix | b35157a |
| W0 metrics and test seed | 82c7253 |
| W1 play-time display | 3a738c2 |
| W3 task rules | 9ecf1a5 |
| W2 interfaces | 01ccd57 |
| W7 row columns, AV values, loader | 95ac13b |
| W4 coordinator planning | 81ee3cc |
| W6 lane mode and AV profile | cd1b282 |

- All of them are live, keys off, since boot 20 (2026-11-10 16:09, binary d18a9f2ae528).
- Open:
  - **W6b (built: d289a97):** the captain Squad task with its staging point and its own rally per squad. Owner: captains on by default as a mid-phase squad. Staging N = max(`min_group`, `Squad.StageMin` 8), because `min_group` stays 5 for the old path. A squad under N is dissolved and offered again after 60 s.
  - **W5 (built: b5f962d, path fix b006f28):** tether and retaliation.
  - **W8:** the A/C/B runs. Windows 1 and 2 are done (§5.3a). Paused for #36, the crash after `bot bg test stop`. Next comes a position diagnostic for assigned bots that are not present, then the criterion 2 decision.
- W6b and W5 are live since the 21:02 boot of 2026-11-10 (binary installed 20:49, rev 132023350214+), keys off.

**Backlog (other maps, after AV):**
- B1 Node mode, with the AB and BFG profiles: `HoldTarget`, guards, retake in time (§3.1, §3.5).
- B2 Flag mode, WSG and TP: implements moving anchors and several modes per profile (§3.7 items 1 and 3).
  - carrier, escorts, hunters, return, standoff;
  - fixes the carrier fighting and parking, and caps how far defenders chase their carrier.
- B3 Eye profile, Node + Flag (the first map with two modes): runners from the start; a carrier with no tower waits by the middle.
- B4 Isle of Conquest: Node + Siege + the Lane boss phase (BG_PLAN.md phases 5.1/5.2: walls, vehicles, Seaforium); its siege rows go into PROVENANCE.md as adapted-from-AC.
- B5 Strand of the Ancients: Siege rounds. The script sets the status back to `WAIT_JOIN` between rounds, so the round state comes from its world states.
- B6 Silvershard Mines (carts, control zones), Kotmogu (orbs, world states only), Seething Shore (moving azerite nodes), Deephaul Ravine (crystal flag + carts).
- B7 The code-table maps moved into `playerbots_bg_node` rows (one data path).
- B8 Gate-wait rows for the other maps; bots stand at the spawn point during the preparation today.
- B9 The Random Battleground queue (template 32): which map bots land in, and whether the profile follows.
- B10 The phase-4 overlays folded into tasks: ThreatResponse becomes Retake offers; FallBack sits next to the tether.

## 7. Decisions
- 2026-11-10 (owner, Stop 1):
  - restructure the existing planner, do not rewrite;
  - this file's path;
  - a bot-only probe of WSG, AB and the Eye.
- 2026-11-10 (owner, Stop 2):
  - captains on by default, as a mid-phase Squad task with a quota, valued from what a kill does in the script;
  - squads without follow-the-leader movement, with an optional staging point;
  - the commitment margin has a floor;
  - a fallback task;
  - the dead count at a partial weight;
  - retaliation is always allowed, on a budget;
  - only the general is all or nothing, with hysteresis, and guards stay;
  - defenders and threat are shown in the worked example;
  - phase weights are columns;
  - moving anchors and several modes per map are interfaces only, implemented with Warsong Gulch.
- 2026-11-10 (owner, revision review):
  - log the fallback share, and flag a profile over 15% for redesign;
  - an early exit in `OnDamage` and the thread rule closed (2.7);
  - ignore doubling capped at 60 s and reset on death or a task change;
  - a mid-phase worked split;
  - opening-only and full matches tested apart, with no win-rate criterion below 20 full matches per arm;
  - W1 on the new path only, so arm A stays the baseline.
