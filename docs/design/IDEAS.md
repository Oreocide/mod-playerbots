# Playerbots: ideas backlog (brainstorm 2026-10-16)

User ask: bots that feel like real players and react to the world, kill mobs for quests, pick a "goal" from a database of suggestions, keep memories like AC's bots, and play battlegrounds properly.

Each idea notes **what exists**, **what to build**, effort (S/M/L) and how it fits the standing rules:
- act like a typical player;
- two tiers (real behavior only near humans, abstract far away);
- no core edits;
- mmaps-only movement.

Path roots: `mod:` = `modules/mod-playerbots/src/`, core = `src/server/game/`, `AC:` = `azerothcore-wotlk/modules/mod-playerbots/src/`.

## 1. Goals from a database table (the "what am I doing today" layer)

Today: `RandomChangeStatus` rolls one of 9 fixed rpg statuses from config weights (`mod:Strategies/Rpg/RpgStrategy.cpp:89`). AC does the same with NewRpg (`AC:Ai/World/Rpg/NewRpgBaseAction.cpp:1211`).

Build: a goal layer above the statuses.
- **World table `playerbots_goal`** — one row per suggestion: `id, name, kind, weight, min_level, max_level, faction, class_mask, race_mask, zone/area/map, profession, game_event, hour_from/hour_to, weekday_mask, duration_min/max, cooldown, max_per_zone, chat_line`.
- **Kinds** (each maps to an existing or new behavior):
  - questing (a hub), grind (a mob camp), gather route, fish at a spot;
  - town errands (repair, vendor, train, bank, mailbox, AH);
  - sit in a tavern, dance at a festival, duel near the city gate;
  - explore a zone, queue BG/LFG with humans, attend a world event (Darkmoon Faire, Brewfest via `game_event`), go AFK, log off.
- Pick = weighted roll over the rows that match the bot right now: level, faction, class, zone, time of day, active event, cooldowns, zone caps. A row with `max_per_zone` stops 40 bots fishing at one dock.
- **Personas**: a stable per-guid profile (quester, crafter, PvPer, socialite, explorer, idler) multiplies the weights, so the same bot behaves consistently day to day. A table `playerbots_persona(persona, kind, multiplier)`.
- **Far bots** (two-tier rule): the same pick, but the goal is lived abstractly (time passes, the bot "was" questing in Westfall); only near a player does it play out for real.
- Tunable without a rebuild (`.bot goal reload`), and visible in `.bot inspect`.
- Effort M. It replaces the planned `playerbots_behavior_profile` (P2) with something richer, so P2's table should be this one.

## 2. Memory (AC's idea, made persistent and useful)

AC keeps short-lived values on the AI context: `LastMovement`, `LastSpellCast`, `LastSaid`, `RememberTaxi`, rpg targets (`AC:Ai/Base/Value/Last*Value.h`, `AC:Ai/Base/Actions/RememberTaxiAction.h`). Ours: `RpgInfo.BadTargets`, `RecentNodes`, homes/anchors in `playerbots_db_store`.

Build: a bounded per-bot memory with a few typed lists (ring buffers, newest wins):

| Memory | Example use |
|---|---|
| places visited (hub, NPC, last time) | "already sold junk here today", spread visits over towns |
| goals done (goal id, when, outcome) | no repeats, "I just came from the Barrens" for chat |
| quests done/abandoned and why | no retrying a broken quest, the chain continues where it stopped |
| deaths (where, by what) | avoid that camp for a while, or come back with a friend |
| people (player guid, last greeted, whispered, grouped, duelled) | greet friends by name, remember who invited it |
| bad spots (no mesh path, stuck, evicted) | the existing blocklists, but persistent |
| favorites (inn, fishing spot, guild hall) | the habits that make a character feel like someone |

- Stored compactly per bot (one row per memory kind in a new characters table, or `playerbots_db_store` keys), loaded at login, written behind like the repository today.
- **Shared world memory** (all bots): death heat map per 128 yd cell (feeds `DangerMap`), "a rare was killed here", crowded NPCs.
- The chat hook (P6 `BotStatusBoard`) reads it, so a later LLM chat module has something true to say.
- Effort M; the AC part is S.

## 3. Killing mobs for quests (and levelling like a player)

Today: `QuestTravel` takes only kill quests (`IsSimpleKillQuest`, `mod:Strategies/QuestTravel.cpp:125`: every objective `QUEST_OBJECTIVE_MONSTER`, ≤30 each, not group/daily/world quests). It walks to givers and hunting grounds, accepts and turns in for real, and fights with the spec rotation.

Build, in order of payoff:
1. **Collect quests** (`QUEST_OBJECTIVE_ITEM` dropped by mobs): kill + loot. Loot through the client's loot handlers. Looting also produces the grey items that make vendor visits mean something.
2. **Interact quests** (`QUEST_OBJECTIVE_GAMEOBJECT` / item from a GO): walk to the GO and use it through `CMSG_GAME_OBJ_USE` (INPLACE).
3. **Talk-to and deliver**: already covered by the giver/ender flow.
4. **Explore** (`AREATRIGGER` objectives): walk into the trigger on the mesh.
5. **Chains**: follow `NextQuestInChain`/`PrevQuestID` and quest hubs, so a bot works a zone's story rather than random kill quests.
6. **Levelling path**: a zone ladder by level and faction (Elwynn → Westfall → Redridge…) from quest-hub density and ContentTuning. Bots level by questing near players and by an abstract XP trickle far away, instead of only `GiveLevel` at creation.
7. **Group quests / elites**: two or three nearby bots on the same quest group up (needs the group logic and threat-aware combat).

Out unless data allows: escorts, vehicles, scenario-driven and phase-heavy quests (the `PhaseGuard` blacklist keeps them out).
Effort: 1-4 M, 5-6 M-L, 7 L.

## 4. Reacting to the world and to players

Cheap and very visible:
- **Return emotes**: a player who waves or bows at a bot gets one back (hook `PlayerScript::OnTextEmote`, core `Scripting/ScriptMgr.h:754`, upstream; reply through `HandleTextEmoteOpcode`). S.
- **Sit on chairs and benches**: tavern stools, benches, chairs are `GAMEOBJECT_TYPE_CHAIR` (core `Miscellaneous/SharedDefines.h:3171`); a resting bot in a town uses one through `CMSG_GAME_OBJ_USE`. A full tavern looks alive. S.
- **Queue at busy NPCs**: a second bot waits a few yards back instead of stacking on the vendor. S.
- **Time of day**: more bots in inns at night, fishing at dawn, AFK at the bank; the population itself follows a daily curve (fewer bots at 4 a.m.). The goal table's `hour_from/hour_to` covers it. S-M.
- **World events**: `game_event` active → festival goals (Midsummer bonfires, Hallow's End candy buckets, Darkmoon Faire). M.
- **Weather**: rain → shelter under roofs or inside the inn in cities. S-M.

Real reactions:
- **Hostile players**: flagged enemy player attacks → fight back if winnable, else flee to guards (core guard AI does the rest). M.
- **Low health / out of mana**: eat and drink (exists); bandage, potion, retreat. S.
- **Death**: release, run back as a ghost on the mesh, resurrect at the corpse; spirit healer only after a few deaths. M.
- **Rares and world bosses**: a rare nearby → go look; a world boss with players → join in. M.
- **Social**: accept duels near cities (AC's duel anchors idea), trade window answers, invite replies (exist), guild hello/grats. S each.
- **Transports**: actually ride boats and zeppelins (today the route follower teleports between docks). L, because of transport movement.

## 5. Battlegrounds, fully

Today:
- `ActivityMgr` fills human-occupied queues to the minimum team size (`mod:ActivityMgr.cpp:466-526`).
- Inside, bots chase the nearest enemy player (`mod:Strategies/GenericStrategies.cpp:322-354`).

Navmesh tiles exist for the BG maps (verified 2026-10-16: Alterac 29, Warsong 13, Arathi 17, Eye of the Storm 25, Strand 69, Isle of Conquest 58, Twin Peaks 10, Gilneas 15, Silvershard and others present), so mmaps-only movement works there.

Build as layers:
1. **Queue and start** (P7): fill to full teams, staggered joins, wait at the gate, spirit healer.
2. **Objective model**: read the BG state from public APIs. Capture points `GAMEOBJECT_TYPE_CAPTURE_POINT` with `GameObject::GetControllingTeam()` (core `Entities/GameObject/GameObject.h:465`), flag stands/drops, scores. Plus one small per-BG data table: objective list, priorities, home/away nodes, flag-run routes (generated from mesh paths between the objectives, then hand-checked).
3. **Roles** (AC's split, `AC:Ai/Base/Actions/BattleGroundTactics.cpp:1207-1275`): attackers, defenders, flag carrier and escorts, healers who follow a group; re-balance as nodes change hands.
4. **Fighting where it matters**: target priority = enemy flag carrier > enemies on our node > nearest; healers heal the carrier; stop chasing past the objective (no "chase to the enemy graveyard").
5. **Talk like players**: short `/bg` call-outs from a phrase table ("inc mid 3", "need help at stables"), cosmetic but very human.
6. **Order of BGs**: capture-the-flag and node maps first (Warsong, Twin Peaks, Arathi, Gilneas, Eye of the Storm, Deepwind); vehicle and siege maps later (Strand, Isle of Conquest, Alterac Valley's NPC bosses; Wintergrasp with vehicles, as the user asked).
7. **Arenas**: separate work (line of sight, pillars, CC chains); after BGs.

Effort: 1 S-M, 2-4 L, 5 S, arenas L.

## 6. Infrastructure these need (build once)

- **World-thread action queue** (mail, AH, guild, queues): blocked on the soak validating threading.
- **`BotStatusBoard`** (P6): goal, target, memories, for chat modules and `.bot inspect`.
- **Loot through the client handlers**: needed by collect quests, grinding and vendor value.
- **Death and ghost run on the mesh**: needed by quests, BGs and world PvP alike.
- **A per-bot timeline in `.bot inspect`**: the last N goals/statuses/results, to debug "why is this bot standing here".

## Suggested order

1. Return emotes + chairs + NPC queueing (S, high visibility).
2. Goal table + personas + memory core (replaces P2's profile table).
3. Collect/interact quests + loot + chains (makes "kill mobs for quests" real).
4. BG layers 1-4 on Warsong and Arathi.
5. Death/ghost run, hostile-player reactions, events/time of day.

## Decisions to make

- Should bots level by playing (questing near players, abstract XP far away) instead of only being created at a level?
- Personas: which set, and does a bot keep its persona forever?
- Memory retention: how long, and should memories survive a bot's reset/rename?
- BGs: which two maps to pilot; do bots join only when a human is queued (as now) or keep a few BGs running for humans to join?
