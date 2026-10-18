# Playerbots v3: roadmap

Depends on PLAN_V3.md. Effort: S ≤ 1 day, M 2-4 days, L > 1 week. No phase starts without approval. No core edits. Movement stays `MoveAlongMesh`.

The P0/P1 short soak passed the crash bar on 2026-10-16 (see PLAN_V3.md). The 1-hour soak and the in-game innkeeper walk are still open. Phases 1-2 do not need them. Phase 5 and later do.

| Phase | Deliverable | Acceptance | Effort | Risk |
|---|---|---|---|---|
| **1 Visible reactions** | `React.ReturnEmotes` and `React.Chairs`. Reply through `HandleTextEmoteOpcode`. Sit via `HandleGameObjectUseOpcode` on a chair GO | One human in Stormwind: wave at a bot within greet range and get one wave back; the bot does not reply twice inside the cooldown. A resting bot within 15 yd of a chair uses it and stays seated until the goal ends. 0 calls to the stand-state handler from a map thread (debug assert on opcode place) | S | L |
| ↳ **Phase 1 status 2026-10-16: code built, not play-tested.** Nearest bot within `Rpg.GreetDistance` answers wave/hello/greet/bow/cheer/laugh/salute/thanks/welcome through `HandleTextEmoteOpcode`, once per sender per `Rpg.GreetCooldown`. A resting bot uses `HandleGameObjectUseOpcode` on the nearest chair within 15 yd and stands when the rest task ends via `SetStandState`, not the stand-state opcode. Keys default on. | — | — | — | — |
| **2 Goal table** | `playerbots_goal` + `playerbots_persona`, five seed goals, `.bot goal reload`, persona assigned once per bot. Far bots store the goal and do not path | With `Goal.Enable=1` and 10 bots near a human, `.bot inspect` shows a goal from the table. A weight edit plus reload changes the pick mix (χ² over 200 picks). Frozen bots log the goal and emit 0 `MoveAlongMesh` calls for it. `Goal.Enable=0` matches today's status mix | M | L |
| **3 Memory + inspect** | Rings for place, goal, person. `playerbots_memory` flush. `.bot inspect` shows the last 8 outcomes. Board copy for a later chat reader | Relog keeps the last goal. Reset clears that guid. Two map threads writing two bots do not corrupt the file (the P0 repository lock). Inspect from the world thread never takes a map lock | M | M |
| **4 World queue** | One queue for THREADUNSAFE steps: loot, gossip select, mail, AH, repop, reclaim, battlefield port. GUID re-resolved when the step runs | A forced repair and a forced loot each complete. A step queued for a bot who logged out is dropped. Queue depth stays < 256 with 50 near bots. 20 min, 4 map threads, `ForceActive=200`: 0 crash/assert | M | M |
| **5 Collect quests** | `Quest.Collect`: item objectives whose item is in `creature_loot_template`, loot through the queue. Still gated by the `IsSimpleKillQuest` exclusions (no daily, world, group, amount > 30) except the objective type | 20 kill-and-loot attempts in Elwynn or Durotar with a human within 150 yd: ≥16 complete, each failure has a log reason, 0 straight-line moves, 0 new `fall rescue` lines above the pre-phase count in that spot | M | M |
| **6 Quest breadth** | Interact (GO use), explore (area trigger), `RewardNextQuest` chains, a faction zone ladder, `Quest.AbstractXp` on the frozen tick | A chain of 3 quests advances in order. A frozen bot gains XP with 0 grid loads attributed to it. Explore completes by walking into the trigger. Counts of quests each switch unlocks match QUEST_PLAN.md within 5% after a data reload | L | M |
| **7 World death** | AzerothCore's revive, not a ghost run. A dead bot outside a battleground is marked, resurrected 60–300 s later, and teleported to a level spot. No `MoveAlongMesh` as a ghost | A bot that dies with nobody near is alive in a fitting zone within 5 min. A bot standing next to a human is not teleported out from under them by this timer. 0 ghost-path launches | S | L |
| **8 BG pilot** | Human-gated fill to max team size on Warsong (map 2106) and Arathi (map 2107). Roles and target priority in BG_PLAN.md. `/bg` lines from a phrase table | One human queues each map: both teams reach the template max, the human's side wins or loses on objectives (flag capture or node count), not on a wipe in the graveyard. 0 straight-line moves. Bots do not queue when no human is queued | L | H |
| **9 World reactions** | Hostile player (fight if the bot's level is within 5, else run to a guard), rare nearby, duel accept in a city, goal rows for the active `game_event` | Documented in a 30 min play note: one of each happens, or the log says why the row did not match | M | M |
| **10 Later maps** | Twin Peaks, Gilneas, Eye of the Storm once their objective GOs are listed the same way as Arathi. Wintergrasp and vehicle maps only after a separate vehicle note. Arenas last | Same acceptance as phase 8 for each map added | L | H |
| **11 Population roll** | Far bots only. Teleport interval 1–5 h, staying in the current band, with a small chance of one level. The stock of levels is the band table in PLAN_V3: weights 10 for 1–19, 20–39, 40–59, 60–69, 70–79, 80–84, 85–89, and 12 for level 90. A bot inside a band is a random level in it. Destination goes through `SpawnPolicy` and the district check | A census of far bots is near those weights (90 a step above each band, not most of the bots). No bot inside 150 yd of a human teleports. 0 `MoveAlongMesh` from this roll | S | L |
| **12 Craft at a forge** | A blacksmith in a city walks to a forge or anvil, works for a few seconds, and makes one random recipe it knows and can pay for. The existing auction post sells it later. No recipe: still stand and work. Far bots do not craft | In Stormwind or Orgrimmar, a crafter is at the forge and an item it made shows up on the auction house afterward. 0 crafts from a frozen bot | S | L |
| **13 Faction districts** | The split-sanctuary list in PLAN_V3: Northrend Dalaran (Silver Enclave / Sunreaver's Sanctuary), the Icecrown pavilions, Broken Isles Dalaran (Greyfang Enclave / Windrunner's Sanctuary), the Dalaran copy under area 6611, and Silvermoon City (15969). Silvermoon's five Horde quarters already have faction mask 4: Walk of Elders, the Royal Exchange, Court of Blood, Court of the Sun, Hall of Blood. A street box only where the parent city is what the area query still returns. Hub picks, wander targets, and the population-roll destination refuse the other side. A bot already standing there is evicted to its own side | An Alliance bot never paths to Sunreaver's Sanctuary, the Sunreaver pavilion, Windrunner's Sanctuary, or those five Silvermoon quarters, and a Horde bot never paths to the Silver Enclave, the Silver Covenant pavilion, or Greyfang Enclave. The rest of Silvermoon stays open to both, as do Shattrath, Valdrakken, Dornogal, and Oribos | M | M |

## Dependency graph

```
1 reactions
2 goals --> 3 memory --> 9 world reactions
2 goals --> 6 chains
4 world queue --> 5 collect --> 6 interact
13 faction districts --> 11 population roll
12 craft at a forge (auction post already exists)
8 BG pilot uses 2's goal kind "bg" but can ship with a hardcoded role if phase 2 slipped
```

Phases 1 and 2 can run in either order. Phase 4 is the gate for loot. A battleground corpse still has to release inside the BG pilot (phase 8); that is not the world-death teleport. Phases 11–13 are the visible population work and do not wait on quests.

## Explicitly not in v3

LLM chat, flying, Exile's Reach, AH valuation, taxi discovery, guild bank, rated PvP, per-boss puzzles, boats and zeppelins (L, needs transport passengers), and a ghost run back to a corpse. v1 phases still open (in-game innkeeper, gear audits, mailbox) stay in ROADMAP.md and are not replaced by this file.
