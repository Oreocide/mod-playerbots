# Playerbots: identity (spawn safety, mounts, gear)

User request 2026-10-15:
- Horde bots never spawn in Alliance cities/zones, and Alliance bots never in Horde ones.
- No level-24 goblin on a horse.
- Armor limited to the class/spec the bot is marked as.

Paths: `mod:` = `modules/mod-playerbots/src/`, core = `src/server/game/`, `AC:` = `azerothcore-wotlk/modules/mod-playerbots/`. Client data = `wowdata --profile tc-local`, which is **build 69933 (12.1.0), not 12.1.5** (L for freshness).

## 1. Spawn and home safety

### Today: no single check guarantees faction-safe placement (all H)

| # | Hole | Cite |
|---|---|---|
| S1 | Spot faction comes only from the zone's `AreaTable.FactionGroupMask` (core `DataStores/DB2Structure.h:141`), and only when it is purely Alliance or purely Horde. Contested or zero-mask zones pass for both teams | `mod:RandomBotMgr.cpp:835-838,1159` |
| S2 | Graveyard spots ignore graveyard faction. Faction is now a condition (`CONDITION_SOURCE_TYPE_GRAVEYARD` = 27, core `Conditions/ConditionMgr.h:184`), and `BuildSpots` never evaluates it | `mod:RandomBotMgr.cpp:797` |
| S3 | `PickSpot` never calls `IsHostileGround` | `mod:RandomBotMgr.cpp:1145-1182` |
| S4 | `OnInterrupted` → `UpdateHome` saves wherever the bot is (after a summon, GM move or leaving a party) with no faction/level check | `mod:PlayerbotAI.cpp:517-543`; `mod:RandomBotMgr.cpp:1472-1477` |
| S5 | A bad position survives relogs: re-placement happens only on a `PlacementVersion` bump or in an instance | `mod:RandomBotMgr.cpp:75,1198-1206` |
| S6 | `HubIndex::Nearest/NearestAny` have no team filter; the camp fallback (4000 yd) and `DoFlight` use them | `mod:Travel/HubIndex.cpp:274-302`; `mod:Strategies/Rpg/RpgStrategy.cpp:422,483` |
| S7 | Quest hunt grounds and enders (`BestSpawn`, `NearestHunt`) have no hostility or level check | `mod:Strategies/QuestTravel.cpp:233-256,720-760` |
| S8 | Hotspot override/marker `team` can be 0 (both) on an enemy city | `mod:Travel/HotspotMgr.cpp:173-174`; hotspots SQL `:7` |
| S9 | Rpg moves call `MoveToward` directly and skip `RouteFollower::Begin`'s hostile check (the only place it is enforced, `mod:Travel/RouteFollower.cpp:54`) | `mod:Strategies/Rpg/RpgStrategy.cpp:174,294` |
| S10 | Level check is weak: retail ContentTuning gives most zones a 1-to-cap range | `mod:RandomBotMgr.cpp:817-823` |

**Audit 2026-10-16 (read-only, saved zone of each bot vs `AreaTable.FactionGroupMask` from the client data):** 29 of 750 bots (3.9%) are saved in zones that only the other faction owns: 9 Horde bots in Stormwind City, 5 in Dun Morogh, 3 in Elwynn Forest, 1 in Bloodmyst Isle, and 11 Alliance bots in Northern Barrens. Re-run it after the deploy: it should return 0 once every bot has logged in or rotated once.

**Built 2026-10-16 (module only, not deployed):** `Spawn/SpawnPolicy.{h,cpp}` (`IsEnemyGround`, `TeamOfGraveyard`). Used by: `BuildSpots` (graveyard faction from its conditions, S2), `PickSpot` (skips spots inside towns that attack the team, S3, filled lazily once the hub index exists), `Prepare` at login (re-place if on enemy ground, S5), `EvictFromEnemyGround` once a minute from `RotateBots` (summons and old placements, S4), `OnInterrupted` (never saves a home on enemy ground, S4) and `HubIndex::Nearest` (no service NPCs of a town that attacks the bot, S6). Eviction and hotspot placement use one predicate (`IsEnemyGroundAt` before a hotspot teleport), and a bot evicted twice within 10 minutes is left alone with a WARN line (a sign the two disagree). The hub-hostility check depends on the hub index only, not on `Playerbots.Travel.Enable`. The sanctuary exemption (R5) is not implemented: the AreaTable flag name was not found, and neutral cities have no faction mask, so they are not enemy ground. Split sanctuaries (Dalaran Silver Enclave / Sunreaver's Sanctuary, and the same pattern on 12.1) are v3 phase 13: the area mask is 0, so AC hard-blocks those districts (`AC:Mgr/Travel/TravelMgr.cpp:106-214`). Not done yet: quest hunt grounds and enders (S7), the hotspot override team column (S8), direct rpg moves into enemy ground (S9), zone level fit (S10).

### Design: one `SpawnPolicy::IsAllowed(bot, map, pos, purpose)`

Every place that picks or saves a bot position calls it:
- `BuildSpots`/`PickSpot`, `PlaceAtHotspot`, `UpdateHome`, login re-validation;
- `HubIndex::Nearest*` (gets a team parameter), QuestTravel spot pickers, rpg wander/camp targets, taxi landing nodes.

| Rule | Data | Applies to |
|---|---|---|
| R1 zone not enemy-only: `AreaTable.FactionGroupMask` of the zone **and** the sub-area | core `DB2Structure.h:141` | all |
| R2 not inside an enemy hub cluster: `IsHostileGround(team)` | `mod:Travel/TravelMgr.cpp:383-397` | all |
| R3 graveyard usable by this bot: evaluate `CONDITION_SOURCE_TYPE_GRAVEYARD` with the bot as condition object (as `ObjectMgr::GetClosestGraveyard(loc, team, conditionObject)` does, core `Globals/ObjectMgr.h:1127`) | ConditionMgr | graveyard spots |
| R4 level fit from the existing `DangerMap` (highest creature level per 128 yd cell, `mod:Travel/DangerMap.*`): reject if cell max > bot level + 3; for leveling spots also reject if cell max < bot level − 10. Towns, inns and sanctuaries are exempt from the lower bound | DangerMap | leveling/quest/wander spots |
| R5 neutral sanctuaries (e.g. Valdrakken, Dornogal; sanctuary flag in AreaTable) are allowed for both teams | AreaTable flags | all |
| R6 the hotspot `team = 0` is only honored when the cluster vote says both teams are friendly; otherwise team is derived | HotspotMgr | hotspots |

Behavior on violation:
- placement → next candidate, else race start position;
- `UpdateHome` → keep the previous home;
- login → re-place;
- moves → refuse the target and cool it down.

Log: `server.playerbots.spawn` with the rule that failed.

Acceptance:
- an offline audit query lists every bot home/spot against R1-R3 and returns 0 violations for 750 bots;
- 1 h live with relogs, summons and party leaves gives 0 violations.

## 2. Mounts and riding

### Today (H unless noted)

- **Riding is missing server-wide, not only for bots.**
  - 36 of 585 random bots at level ≥20 have skill 762.
  - 17 of 462 non-bot characters at level ≥10 have it.
  - riding is taught at trainers (learn spells 33389/33392/34092/90266 in `world.trainer_spell`; corrected 2026-10-16), but nobody visits one, and nothing in the module or core grants it otherwise.

  (Characters DB queries, 2026-10-15, H.) Mount use goes through `Unit::GetMountCapability`, which reads the riding skill (core `Entities/Unit/Unit.cpp:8398`), so mount casts without it should fail with `SPELL_FAILED_NOT_HERE` (core `Spells/SpellInfo.cpp:2320-2327`) (M: not observed in game).
- The module grants 6648 Chestnut Mare (Alliance) or 6654 Brown Wolf (Horde), by **team, never race** (`mod:GearFactory.cpp:355-360`, from `Rpg.MountMinLevel` = 20).
- **Why a goblin rides a horse (H mechanism, M observed):**
  - All 750 bots sit on **2 Battle.net accounts, and both mix Alliance and Horde characters** (DB query).
  - Mounts are account-wide (`auth.battlenet_account_mounts` holds 6648 and 6654).
  - On login, `CollectionMgr::LoadMounts` re-adds every account mount, and `AddMount` calls `LearnSpell` when the mount's `PlayerConditionID` passes (core `Entities/Player/CollectionMgr.cpp:380-446`).
  - Chestnut Mare has `PlayerConditionID` 0 and no `world.mount_definitions` faction pair, so a goblin learns the Alliance horse every login (not saved to `character_spell`, so it doesn't show there).
  - `PickMountSpell` then takes the *last* mount-aura spell from an unordered spell map (`mod:Travel/MoveToward.cpp:81-97`), which is often the horse.

### Client data (12.1.0, H)

- Riding spells (SkillLineAbility, SkillLine 762) with SpellLevels.BaseLevel:

  | Spell | Name | Level |
  |---|---|---|
  | 33388 | Apprentice Riding | 10 |
  | 33391 | Journeyman Riding | 10 |
  | 34090 | Expert Riding | 20 |
  | 90265 | Master Riding | 30 |

  Also present: 441143 Skyriding Basics (level 10), 54197 Cold Weather Flying and the pathfinders.
- `world.trainer_spell` teaches riding through the learn-spell variants 33389/33392/34092/90266 (corrected 2026-10-16). The module still grants it directly; trainers never teach skill-granting spells to bots (`TrainerIndex`).
- Racial ground mounts carry **no race condition** in `Mount.db2`: `PlayerConditionID` 0, and `SourceText` = vendor + zone. Examples: Goblin Trike 87090 / Turbo-Trike 87091 (Kall Worthaton, Orgrimmar); Gray/Brown Ram 6777/6899 (Dun Morogh); Striped/Spotted Frostsaber 8394/10789 (Darnassus); Red Hawkstrider 34795 (Eversong); White Kodo 64657 (Mulgore); Blue Skeletal Horse 17463 (Tirisfal); Red/Unpainted Mechanostrider 10873/17454; Timber/Brown Wolf 580/6654 (Orgrimmar); Brown Horse 458 / Chestnut Mare 6648. Allied-race mounts use `PlayerConditionID` (e.g. Exarch's Elekk 73629 → 27946, Flags 4).
- AC reference: per-race hard-coded lists (`AC:src/Bot/Factory/PlayerbotFactory.cpp:3943-4062`). The fallback lists there have Horde/Alliance swapped (`:3995-4008`), so do not copy them.

### Built 2026-10-16 (module only, not deployed)

`Economy/RaceMounts.{h,cpp}`: a compiled table (checked against `Mount.db2` at startup: `Validate()` logs problems) with 1-4 vendor ground mounts per race (client-verified spell ids: human 458/6648/472, orc 580/6654/6653, dwarf 6777/6899/6898, night elf 8394/10789/10793, undead 17463/17464/17462, tauren 64657/18990/18989, gnome 10873/17454/10969, troll 8395/10796/10799, goblin 87090/87091, blood elf 34795/35022/35018/35020, draenei 34406/35710/35711, worgen 87840 Running Wild only if known). Allied races use their cousin's list (kul tiran → human, dark iron/earthen → dwarf, void elf → night elf, nightborne → blood elf, highmountain → tauren, lightforged → draenei, zandalari → troll, vulpera → goblin, mag'har → orc, mechagnome → gnome); pandaren and unknown races use the faction's basic mounts. This replaces the planned `playerbots_race_mounts` table (a table can still replace it later).
- `RaceMounts::Grant` (from `GearFactory::Supply`, every 60 s for a simulated bot): learns Apprentice Riding (33388) at level 10 and Journeyman (33391) at 20, and from `Rpg.MountMinLevel` puts one mount of the race in the collection.
- `PickMountSpell`: a random bot rides only a mount of its race's list (first known one, from a fixed per-guid start), never "any mount it knows"; bots that follow a player keep the old scan of their own spell book. `UpdateMountForTravel` skips the spell scan when the bot has no riding skill.
- **Mounting rules (advisor review 2026-10-16):** riding switches mount code on that never ran before, so: a bot mounts only for a standing start on a leg of 70+ yards (`MoveAlongMesh` never mounts while a walk is running), only outdoors, only when `SpellInfo::CheckLocation` allows the spell here, and at most one attempt per bot every 30 s. Worgen use Running Wild when they know it, else the human list. `Validate()` covers every race in `For()`.
- Per-faction Battle.net accounts for new bots: **not done** (the race-list rule already neutralizes the mixed accounts).

### Design (original plan)

- **Data table** `playerbots_race_mounts(race, spell, tier, comment)` in the world DB, seeded by a script:
  1. take the `Mount` rows with `SourceTypeEnum = 2` (vendor) and ground type 230;
  2. map the vendor zone to its home race(s);
  3. include allied-race mounts whose `PlayerConditionID` matches the race;
  4. **review the result by hand**: about 25 races × 2-4 mounts.

  Classes with travel forms (druid) use them instead (M).
- **Riding:** learn 33388 at level 10 and Journeyman at its level, from `GearFactory::Provision` with the existing level hooks. Flying riding (34090+) is granted for collection parity, but **bots never use a flying mount in v1** (no air navmesh; skyriding impulse gap G6).
- **Grant:** `CollectionMgr::AddMount(spell, flags, factionMount, learned=true)` (core `Entities/Player/CollectionMgr.h:156`), plus `learnSpell`. Each bot gets one racial ground mount at 10. Store the pick in `playerbots_db_store` key `mount`, so it is stable across sessions.
- **Use:** BotMover mounts with the stored spell after `GetMountCapability` + `SpellInfo::CheckLocation` pass. The fallback is limited to **mounts in the bot's own race list, then its faction's vendor list**, then walking. Never "any known mount": account-wide collections make every mount on the bnet account known.
- **New bots:** create random-bot bnet accounts **per faction** (`<prefix>A…` / `<prefix>H…`), so collections never mix factions. Existing mixed accounts stay; the race-list rule covers them.
- **Migration:** existing bots with 6648/6654 get a racial mount added and the stored pick switched to it. The old mount stays in the collection; collections are account-wide anyway.

Acceptance: all 750 bots at level ≥10 have riding ≥75 and a racial mount pick. Spot check: goblin → trike, dwarf → ram, blood elf → hawkstrider, tauren → kodo, undead → skeletal horse. 0 `SPELL_FAILED_NOT_HERE` mount failures outdoors in a 1 h soak.

## 3. Gear: armor type and spec

### Today (H unless noted)

- The candidate pool is any armor or weapon of quality Normal..`GearMaxQuality`, with **no armor-subclass and no primary-stat filter** (`mod:GearFactory.cpp:150-193`). The sort is required level → quality → item level, then a random pick among the top 3 that pass `CanUseItem` + `IsUsableByLootSpecialization` + `CanEquipNewItem` (`:197-206,255-281`).
- Items without an `ItemSpec` row count as usable by every spec, so white no-stat cloth passes for a warrior. Lower armor types pass `CanUseItem` (core `Globals/ObjectMgr.cpp:3325-3364`) (M).
- The spec is real: a random non-pet ChrSpecialization at level ≥10 via `ActivateTalentGroup`, plus Blizzard's starter build (`mod:RandomBotMgr.cpp:1287-1299`, `mod:Talents/BotTalents.cpp`). The role comes from `ChrSpecialization` (`mod:PlayerbotAI.cpp:666-684`).

### Client rule

`ChrClasses.ArmorTypeMask` (loaded by TC, core `DataStores/DB2Structure.h:714`) holds exactly one wearable armor type per class (12.1.0, H). Bits = item armor subclass, i.e. a warrior wears plate from level 1.

| Class | Mask | Armor bits |
|---|---|---|
| Warrior | 113 | misc, plate, cosmetic, shield |
| Paladin | 2289 | plate + shield + relic |
| Hunter | 41 | mail |
| Rogue | 37 | leather |
| Priest / Mage / Warlock | 35 | cloth |
| Death Knight | 3121 | plate |
| Shaman | 2665 | mail + shield |
| Monk / Demon Hunter | 37 | leather |
| Druid | 2341 | leather |
| Evoker | 41 | mail |

### Design (`GearFactory` candidate filter)

1. **Armor slots** (head, shoulder, chest, wrist, hands, waist, legs, feet): the item subclass must be in `ArmorTypeMask` and not misc/cosmetic. Cloaks (cloth, back slot), rings, neck and trinkets are exempt. Shields/off-hands only if the mask (shield bit) and the spec allow them.
2. **Primary stat:** if the item has Strength/Agility/Intellect stats, at least one must be the spec's primary stat (`Player::GetPrimaryStat`, core `Entities/Player/Player.h:2183`), and it must carry no other primary stat. Items with no primary stat stay allowed (low-level whites).
3. **ItemSpec:** when the item has `ItemSpec` rows (core `DataStores/DB2Structure.h:2664`), the bot's spec must be listed. This is the existing `IsUsableByLootSpecialization`, now applied strictly.
4. **Weapons:** the subclass is allowed by the spec (one-hand/two-hand/dual wield/ranged), and the primary stat matches. Simple per-spec allow-lists in data (`playerbots_spec_weapons`), seeded from retail norms and reviewed.
5. **Migration:** a one-time re-gear pass for bots that are wearing a disallowed item (logged count).

### Audit and built 2026-10-16 (module only, not deployed)

**Audit (read-only, 4,635 equipped armor pieces of the 750 bots against `ChrClasses.ArmorTypeMask`):** only **108 pieces (2.3%) are outside the class armor type, all of them cloth worn by Monks** (leather class). The existing candidate rule already keeps almost everyone in their type because `IsUsableByLootSpecialization` checks the primary stat (stat-less cloth is the gap). Primary-stat mismatches were not audited (needs item stat data).

**Built:** in `GearFactory::EquipSlot` a strict pass takes only items of the class's armor type for armor slots (head, shoulders, chest/robe, waist, legs, feet, wrists, hands, shield); wrong-type items are skipped without counting against `MaxScan`. If nothing of the right type exists for the level, a second pass takes what the old rule took, so no slot stays empty, and the bot is left alone for an hour (no re-gear loop). `NeedsGear` now also returns true for a bot wearing armor outside its class type, which re-gears it once. Weapons: proficiency is still `CanUseItem` only.

**Built (scoring, not play-tested):** `EquipSlot` scores the retail budget (`GetRandomPropertyPoints` × `StatPercentEditor` × 0.0001, the same split as `Item::GetItemStatValue`). Weights are shared, not a per-spec table: primary 1, stamina 0.5, crit/haste/mastery/versatility 0.4, other stats 0.15, weapon DPS × 10, plus half the item level as a tiebreak. A second primary stat is rejected. Weapons are limited by primary stat in code (strength: axes/maces/swords/polearms/fist; agility: those plus daggers, bows, guns, crossbows, warglaives; intellect: staves, wands, daggers, maces, swords; a held off-hand is intellect only). A worn piece stays when its score is already at least as high. Whisper `autogear` and GM `.bot autogear <name> [white|green|blue|purple|<ilvl>|match|reset]`. `match` uses the master's average item level, or the GM's when run from `.bot`.

**Not done:** `playerbots_spec_stat_weights`, `playerbots_spec_weapons`, and AC's `BisListMgr`-style tiers. The weapon lists above are a code allow-list, not reviewed per spec.

### Reuse from AC "autogear" (user suggestion 2026-10-15; AC is GPL-2+, adapting allowed)

| AC piece | What it does | Reuse on TC retail |
|---|---|---|
| `autogear` chat command, `AutoGearAction::Execute` (`AC:src/Ai/Base/Actions/TrainerAction.cpp:586-700`) | `autogear [white\|green\|blue\|purple]` caps quality; `autogear <ilvl>` targets an item level; `autogear match` copies the commanding player's average equipped ilvl; `autogear reset` strips and re-gears; config caps `AutoGearQualityLimit`/`AutoGearScoreLimit` (`AC:conf/playerbots.conf.dist:612-640`) | **adapt the command UX**: whisper `autogear …` through `mod:Cmd/ChatCommands.cpp` and GM `.bot gear <name> [args]`, both calling `GearFactory` with (quality cap, ilvl target). `match` is very useful for party/raid bots |
| `PlayerbotFactory::InitEquipment(incremental)` (`AC:src/Bot/Factory/PlayerbotFactory.cpp:2280-2512`) | per slot: scan items by required level, filter, score, keep the best; incremental mode only replaces when the score improves | **adapt the structure** in `GearFactory`: per-slot best-score with an incremental upgrade compare, replacing today's "random of top 3 by ilvl" |
| `CanEquipArmor` (`:1927`) | skill check only (has plate skill → may wear plate) | **do not reuse**: it is the reason AC needed the local `PreferClassArmorType` hack. Use the `ArmorTypeMask` rule above |
| `StatsWeightCalculator::CalculateItem` (`AC:src/Mgr/Item/StatsWeightCalculator.cpp:91-130`; weights `:263-524`; type penalty `:630`) | Σ(stat weight × item stat), item-type penalties (2H for 1H specs, daggers for rogues), set bonus, sockets, quality×ilvl blend | **rewrite the weights** for retail: primary stat + crit/haste/mastery/versatility per spec, in a data table `playerbots_spec_stat_weights(spec, stat, weight)`. Keep the type-penalty and socket ideas. Retail item stats come from ItemSparse stat allocations scaled by item level/bonus lists, so use TC's item stat calculation, not raw template stats (M; verify the API) |
| `BisListMgr` + table `playerbots_bis_gear` (`AC:src/Mgr/Item/BisListMgr.cpp:33-44`) | `autogear bis`: per class/spec/faction/slot item from a DB table keyed by ilvl tier | **optional later**: same table shape for retail tiers if you want raid bots in set gear; data has to be authored per season |
| hit/expertise overflow penalties (`:742+`) | WotLK rating caps | **skip** (no caps in retail) |

Resulting GearFactory pipeline:
1. candidate filter (armor type, primary stat, ItemSpec, weapon allow-list);
2. score with spec weights;
3. per-slot best within (quality cap, ilvl target);
4. incremental replace.

Commands: `autogear`, `autogear match`, `autogear <ilvl>`, `autogear reset`. Provenance: design-from-AC (record in PROVENANCE.md).

Acceptance: an audit query over all 750 bots finds 0 equipped armor pieces outside the class type, and 0 items whose primary stat mismatches the spec. Spot check of 10 bots of each armor class.
