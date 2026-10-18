# Playerbots v3: quest plan

Counts are from the live `world` database on 2026-10-16. Objective types are `QuestObjectiveType` in `src/server/game/Quests/QuestDef.h:352-378` (H). Optional objectives (`QUEST_OBJECTIVE_FLAG_OPTIONAL` = 0x0004, `QuestDef.h:385`, H) are excluded from the "fully covered" counts.

## Objective inventory

| Type | Name | Objectives | Quests that have one | v3 |
|---|---|---|---|---|
| 0 | monster | 37,238 | 19,300 | already, via `IsSimpleKillQuest` |
| 1 | item | 15,282 | 11,990 | phase 5, and only when the item is a creature drop |
| 2 | gameobject | 2,948 | 1,691 | phase 6, `CMSG_GAME_OBJ_USE` |
| 14 | criteria tree | 2,083 | 1,691 | no (owning systems often unbuilt) |
| 15 | progress bar | 1,092 | 1,092 | no |
| 3 | talk to | 710 | 533 | phase 6, already close to the giver/ender flow |
| 19 | area-trigger enter | 500 | 328 | phase 6 |
| 4 | currency | 259 | 257 | no |
| 11 | win pet battle vs npc | 239 | 186 | no |
| 6 | min reputation | 230 | 218 | no |
| 21 | kill with label | 176 | 122 | no, until a label means a creature list we can query |
| 17 | obtain currency | 156 | 155 | no |
| 12 | defeat battle pet | 137 | 103 | no |
| 8 | money | 117 | 117 | no |
| 5 | learn spell | 108 | 103 | no |
| 10 | area trigger | 99 | 99 | phase 6, with type 19 |
| 9 | player kills | 72 | 72 | no (not a quest loop) |
| other | 16, 22, 20, 13, 7 | 64 | — | no |

## Quests a new capability fully unlocks

A quest counts only when every non-optional objective sits in the set. Each row adds the quests that need the new type and nothing outside the set.

| Capability | Rule | Quests unlocked beyond the previous row | Conf |
|---|---|---|---|
| Kill only (data ceiling) | types ⊆ {0} | 15,429 | H |
| Plus creature-loot items | types ⊆ {0,1} and at least one type 1 | 11,159 | H for the type set. **Not** H for "all of these are mob drops" |
| Plus GO use | types ⊆ {0,1,2} and at least one type 2 | 1,229 | H |
| Plus talk-to | types ⊆ {0,1,2,3} and at least one type 3 | 341 | H |
| Plus explore | types ⊆ {0,1,2,3,10,19} and at least one of 10 or 19 | 336 | H |
| Chain link | `RewardNextQuest` either direction | 13,203 quests sit on such a link | H that the column links them. M that a bot should follow every link (many are breadcrumbs) |

`IsSimpleKillQuest` (`modules/mod-playerbots/src/Strategies/QuestTravel.cpp:125-140`, H) is stricter than the kill-only row: no daily, weekly, monthly, seasonal, repeatable, world, dungeon-finder, auto-push, or tracking quest; no suggested-players above 1; no quest package; at most 4 objectives; each monster amount in 1..30. The 15,429 figure is the data ceiling. The picker stays on that function. Phase 5 relaxes only the `Type != MONSTER` check, and only for an item objective whose item is in `creature_loot_template`.

Type-1 split (world DB, 2026-10-16, optional flag 0x4 excluded, 14,800 objective rows): 4,960 have the item in `creature_loot_template`, 1,917 in `gameobject_loot_template` (484 of those rows are in both), 8,407 in neither. These are objective rows, not quests. Do not treat 11,159 as kill-and-loot quests. Phase 5's seed is the subset that joins `creature_loot_template`. GO-loot items wait for phase 6. Items from neither table are not quests we accept.

## How each kind runs

| Kind | Near a human | Far (frozen) | Thread |
|---|---|---|---|
| Kill | mesh to the nearest huntable spawn (`QuestTravel` already), rotation, loot the corpse | abstract: after the goal duration, credit the quest and grant its XP if the bot was eligible | map thread for the fight; loot step on the world queue |
| Collect | same, then `CMSG_LOOT_UNIT` / `CMSG_LOOT_ITEM` / `CMSG_LOOT_RELEASE` | same abstract credit, no loot | loot opcodes are THREADUNSAFE (`Opcodes.cpp:689-693`, H) |
| Interact | mesh to the GO, `HandleGameObjectUseOpcode` | abstract credit | GO use is INPLACE (`Opcodes.cpp:467`, H) |
| Explore | mesh into the trigger, `CMSG_AREA_TRIGGER` | abstract credit | INPLACE (`Opcodes.cpp:170`, H) |
| Talk / turn-in | existing giver and ender walk | abstract credit | gossip select is THREADUNSAFE (`Opcodes.cpp:522`, H) so the select step is queued |
| Chain | on turn-in, if `RewardNextQuest` matches level and faction, take it | the stored goal becomes the next quest id | world thread updates the goal row in memory |

Abstract credit does not complete the quest in the quest log of a bot who might walk into a human mid-goal with the objectives unfinished. It completes the goal and grants XP only. If the bot becomes near before the timer, it switches to the real behavior and keeps the real quest log. Confidence M that this is the right split: it avoids a frozen bot marking a quest rewarded while standing on empty objectives.

## Zone ladder

A static list in the module, not a new core table: for each faction, zones in level order (Elwynn, Westfall, Redridge, … and the Horde mirror). The goal picker prefers a `quest_hub` row in the zone that contains the bot's level. Missing hub → the next zone down, then idle. The list is reviewed by hand for the 1-30 classic continents first. Later expansions are more rows, not more code.

Group quests and elites (IDEAS item 7) stay out. `GetSuggestedPlayers() > 1` remains a reject.

## Failure

- No spawn of the objective creature on a mesh the bot can reach: `badspot`, abandon this pick for 30 min, do not teleport.
- Loot window empty: one retry, then the same cooldown.
- Phase-gated quest: existing `PhaseGuard` blacklist, unchanged.
- Amount above 30 or more than 4 objectives: still rejected, so a bot does not sit on a 50-kill quest.
