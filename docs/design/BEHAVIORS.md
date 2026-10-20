# Playerbots v1: behavior catalog

`mod:` = `modules/mod-playerbots/`. Core paths are relative to `src/server/game/`. Opcode flags are from `Server/Protocol/Opcodes.cpp` (FEASIBILITY §c).

## Framework (applies to every behavior)

- **Pick.** When the current behavior ends, `RandomChangeStatus` (`mod:src/Strategies/Rpg/RpgStrategy.cpp:89`) rolls a weighted behavior from the bot's profile (DATA_MODEL.md). Statuses today: Idle, DoQuest, WanderNpc, TravelFlight, GoGrind, GoCamp, Rest, WanderRandom, Gather (`mod:src/Strategies/Rpg/RpgInfo.h:17-28`). v1 adds Hop/Emote as **overlays**, not statuses: they fire during other behaviors. It also splits WanderNpc into the service behaviors below.
- **Idle chance:** profile weight of `Idle` (today `Rpg.Weight.Rest = 5`, `conf/playerbots.conf.dist:675`). This is the user's "configurable chance to simply idle".
- **Every behavior has:** a max duration (profile), a result (ok / fail / fallback) written to `BotStatus`, a per-bot cooldown, and a bounded retry count.
- **Target acceptance** (NPC or GO):
  1. in the static PhaseGuard set;
  2. `InSamePhase` (`Entities/Object/Object.h:314`) + `CanSeeOrDetect` (`Object.cpp:855`);
  3. alive and not in combat;
  4. a navmesh path exists to a snapped point ≤3 yd from its interaction spot.

  On failure the GUID goes on the per-bot 10 min cooldown.
- **Fallback chain** for "no target": nearest hub with that service on the same map within `Travel.MaxRouteMinutes` → WanderRandom near home → Idle.
- **Loop guard:** the same behavior+target failing 3× within 10 min → that behavior gets weight 0 for this bot for 30 min, plus a log line.
- **Interaction distance:** stop at ≤4 yd (handlers use `GetNPCIfCanInteractWith`, `Entities/Player/Player.cpp:1842`). Face the target (`SetFacingToObject`).
- **Service sequences** run one step per AI tick. They replay the client's open → list → act → close order, because handlers check the interaction state the previous step set. THREADUNSAFE steps go into the world queue. At drain time a step re-resolves the bot by GUID and is dropped if the bot is gone, on another map, or >10 yd from the target.

## Movement (BotMover), used by every behavior below

`BotMover` (`mod:src/Travel/BotMover.*`, built 2026-10-18) is the facade over two modes: **travel** = `MoveAlongMesh` (`MoveToward.cpp`) and **combat** = `MoveCombat` (keeps attacking, 1 yd arrival, may cut into a walk, `MovePriority` arbitration: Travel < Follow < Chase < Position < Avoid). `SnapToMesh` is the forceDest=false end-point snap (G9). Exception (user, 2026-10-18, G45): boarding a transport while following may use a short non-mesh deck step.

| Aspect | Rule | Core API | Conf |
|---|---|---|---|
| Path | `PathGenerator` only (`Movement/PathGenerator.h:58-85`). Accept NORMAL; follow INCOMPLETE in segments (re-path at the segment end, ≥5 yd progress per segment). Never SHORTCUT/NOPATH, never `MoveSplineInit::MoveTo` with a raw point (it bee-lines on NOPATH, `Movement/Spline/MoveSplineInit.cpp:293-303`) | `MoveSplineInit::MovebyPath` | H |
| Destination | path with `forceDest=false`; target = `GetActualEndPosition()` (`PathGenerator.h:80`); reject if >3 yd 2D / >2.5 yd Z from the wanted point | — | H |
| Long distance | travel graph route (`mod:src/Travel/RouteFollower.cpp`); each walk leg is a BotMover path; a walk link failing a real path is blocked with backoff (`mod:src/Travel/TravelMgr.cpp:490+`) | — | H |
| Z | no `GetStandZ`-invented Z, no Z-snap teleport while simulated | — | H |
| Fall watchdog | falling >3 s, or >10 yd below the floor, or off-mesh after a spline → teleport to the last on-mesh segment end (else home), blacklist destination, log `server.playerbots.stuck` | `Unit::IsFalling` | M |
| Stuck | <5 yd progress in `Rpg.StuckTime` (90 s, `conf:707`) → stop, cooldown the destination, Idle (no teleport) | — | H |
| Swim | water segments allowed by the Player nav filter (`Movement/PathGenerator.cpp:668-671`); never mounted in water | `Unit::IsInWater` `Entities/Unit/Unit.cpp:3350` | H |
| Fly | not in v1 (no air navmesh; skyriding impulse gap) | — | H |

## Built 2026-10-16 (module only, not deployed, not play-tested)

- **Hop** (`Strategies/Rpg/Ambient.cpp`): `WalkingHop` rolls `Rpg.HopChance` (0.03/s) while a bot walks on foot, not in the last 8 yd of a walk, at most every `Rpg.HopMinInterval` (12 s); `IdleHop` rolls `Rpg.IdleHopChance` (0.04) per resting roll. `TryHop` only on level ground: outdoors, not mounted/swimming/falling/in combat/casting, landing point 2-3.5 yd ahead (0.8-1.6 for the idle hop) within 0.8 yd of the bot's height, in line of sight, and connected to the bot by the navmesh without a detour. `MotionMaster::MoveJump(EVENT_JUMP, ..)`; the mesh mover relaunches the walk after landing.
- **Greeting** (`GreetNearbyPlayer`, from `DoRest`): a resting bot with a human within `Rpg.GreetDistance` (12 yd) on its map waves, says hello, greets or bows through the client's own text-emote handler (`HandleTextEmoteOpcode` with `TEXT_EMOTE_WAVE/HELLO/GREET/BOW`), at most once per `Rpg.GreetCooldown` (5 min + up to 1 min) and then with chance `Rpg.GreetChance` (0.4). Otherwise the old oneshot emotes (30%) and the idle hop.
- **Innkeeper**: real bind through `HandleBinderActivateOpcode` (P1).
- **Service sequences (P3 part, `Strategies/Rpg/NpcDispatch.cpp`)**, all through the client's INPLACE handlers so they run on the bot's map thread:
  - **Repair**: `HandleRepairItemOpcode` with no item (repair all), when durability is under `Rpg.RepairDurability` (was a direct `DurabilityRepairAll`).
  - **Vendor**: `HandleSellAllJunkItems` (the retail "sell all junk") when the bot carries grey items (`Rpg.SellJunk`). Bots rarely carry greys, so mostly a no-op; buying food/drink stays with `GearFactory::Supply`.
  - **Trainer**: `Economy/TrainerIndex` reads `trainer_spell` at startup (the core's `Trainer` keeps its list private) and mirrors the core's `GetSpellState` rules (class/race fit, base skill rank, required spells, level, "learn X" effects already known, money). A visit opens the list (`HandleTrainerListOpcode`, which the buy handler requires), buys up to `Rpg.TrainMaxPerVisit` random qualifying spells with `HandleTrainerBuySpellOpcode`, and closes. A trainer with nothing for the bot scores 0, so bots do not walk to it. Spells that grant a skill line (the profession-learning spells such as Alchemy or Archaeology, and the riding ranks) are never taught: the 31 such spells at tradeskill trainers would otherwise give a bot a third primary profession. `.bot rpg <bot> trainer` sends a bot to the nearest trainer within 80 yd. Profession recipes are the main use: the world DB has 191 profession trainers with 5,255 skill-gated spells (class spells are learned on level-up). Recipes average about 16 gold and many are free; bots average 33 gold.
  - NPC walks stop at 3.5 yd (a creature answers within its combat reach + 4 yd); a target the mesh ends 1.5 yd short of still counts as reached.
- **World queue** (`World/WorldActionQueue`): map threads enqueue, `WorldScript::OnUpdate` drains at most 8 steps a tick. A step is dropped if the bot is gone, on another map, or more than 10 yd from the target. Depth cap 256.
- **Mailbox** (`Rpg.Mailbox`): at a mailbox GO, if the bot has delivered mail with money or items (COD left alone) or on a 5% roll, the queue runs `HandleGetMailList` then take-money and take-item.
- **Auction** (`AH.Post`): in a capital, when the bot holds a tradable item with a vendor price, any quality (grey through legendary and above), at most once per `AH.VisitCooldown` (30 min). Soulbound, conjured, expiring and refund-window items stay in the bag. The queue says hello, runs an empty browse, and posts up to `AH.MaxPostsPerVisit` (3). Price is `SellPrice` times a quality band (grey 2-4, white 3-6, green 8-16, blue 15-30, purple 25-50, higher 40-80), rounded to the silver. Stacks use the commodity packet. Duration is the core's 12 h slot.
- **Vendor buy** (`Rpg.BuyFood`, INPLACE, map thread): buys food, and drink for a mana user, up to 10, from the vendor's own list. Skips token costs.
- **Not built yet from this catalog:** real taxi use at the flight master, quest-giver turn-ins beyond kill quests, DB-driven profile weights (P2). Mailbox, auction posting and vendor buying are built (2026-10-16, after the short soak passed): see the service-sequence note below.

## Catalog

| Behavior | Preconditions | Core APIs | Data | Failure modes | Fallback |
|---|---|---|---|---|---|
| **Idle** | none | `SetStandState` (stand by default; sit only if hp <70% or mana <50%, already in `RpgStrategy.cpp:188-193`) | profile weight + duration (today `Rpg.Duration.Rest` 30 s) | none | — |
| **WanderRandom** | outdoors or in a town hub; not in combat | BotMover to a random snapped point 0.4-1.0 × radius from home/anchor, ≤8 tries (AC `NewRpgBaseAction.cpp:328`); reject water and hostile-town points (`TravelNode.Team`, `IsHostileGround`) | `Random.WanderRadius`, `Hotspot.WanderRadius` | no snapped point after 8 tries; unreachable | Idle |
| **Hop** (overlay) | moving on foot on a straight, flat, on-mesh stretch ≥8 yd ahead; outdoors; not in water/combat | `MotionMaster::MoveJump` (`Movement/MotionMaster.h:196`) to a path-snapped point 2-5 yd ahead, then re-launch the rest of the path | chance per second while moving (new `Behavior.Hop.Chance`, default 0.03; AC uses 0.02) | landing point off-mesh → skip; MoveJump leaves the bot off-path → re-path from landing | skip hop |
| **Hop in place** (overlay, idle) | standing idle, outdoors | `MoveJump` to the same spot | `Behavior.Hop.IdleChance` (default low) | — | skip |
| **Emote** (overlay) | idle or at an NPC; not moving | `Unit::HandleEmoteCommand` (`Entities/Unit/Unit.cpp:1663`) for oneshots (today 7 emotes, 30% per rest tick, `RpgStrategy.cpp:195-197`); text emotes via `HandleTextEmoteOpcode` (`Handlers/ChatHandler.cpp:703`, INPLACE) toward a nearby human (wave/greet when one comes within 10 yd, cooldown 5 min per human) | profile `emote` weight + list | target gone | skip |
| **Mount** | distance to destination >`MountDistance` (~21 yd today), outdoors, not in water, `Rpg.MountMinLevel` (20) | pick via `Unit::GetMountCapability` (`Unit.cpp:8382`) + `SpellInfo::CheckLocation` (`Spells/SpellInfo.cpp:2170`) over the bot's mounts (`CollectionMgr`, granted at `mod:src/GearFactory.cpp`); cast, **start the path after the cast ends** | mount list | cast fails/interrupted; indoor | walk |
| **Dismount** | within reach+2 yd of target; entering water/indoors; before the FM interaction | `CancelMountAura` | — | — | — |
| **Flight master** | distance to goal > walk threshold; route planner picks a taxi leg. **Cheapest option (user decision 2026-10-15): nodes on the route are marked known as today (`mod:src/Travel/Taxi.cpp:76-84`), no discovery state, no extra FM visits** | walk to the FM (visible bots), then `ActivateTaxiPathTo(nodes, npc)` (`Entities/Player/Player.cpp:23168`) directly: no handler, no available-nodes query. Frozen bots use virtual travel | `TaxiNodes`/`TaxiPath` DB2; faction flags (`Globals/ObjectMgr.cpp:6646-6673`) | no gold; FM missing in this phase | walk route without taxi, else pick a nearer goal |
| **Innkeeper** | bind point >1000 yd away (AC rule) or the bot is in a new home hub; town tier | `HandleBinderActivateOpcode` (`Handlers/NPCHandler.cpp:330`, INPLACE) → `SendBindPoint` (`:349`, casts 3286) → `Spell::EffectBind` (`Spells/SpellEffects.cpp:5263`) → `SetHomebind` (`Player.cpp:17923`, DB write `:17929`). Then Idle near the inn (resting flag from the inn areatrigger, `RestMgr::SetRestFlag` `Entities/Player/RestMgr.cpp:95`) | HubIndex innkeeper bit | innkeeper missing/other phase; instanceable map (bind refused, `:351`) | Idle at hub |
| **Vendor (sell)** | bag use >`Rpg.SellBagSpace` (80%) or any grey item | `HandleListInventoryOpcode` (`Handlers/ItemHandler.cpp:564`, INPLACE; starts the interaction `:586`) → `HandleSellItemOpcode` (`:381`, INPLACE) per grey/junk item | item quality | vendor gone; sell refused | skip |
| **Vendor (buy)** | food/drink stack below threshold (caster: drink) | `HandleBuyItemOpcode` → `Player::BuyItemFromVendorSlot` (`Player.cpp:23724`) | `npc_vendor` | no gold; item not sold | skip (GearFactory top-up still applies) |
| **Repair** | durability <80% | `HandleRepairItemOpcode` (`Handlers/NPCHandler.cpp:403`, INPLACE), or `DurabilityRepairAll` (`Player.cpp:4601`; used today `mod:src/Strategies/Rpg/NpcDispatch.cpp:152`) | npcflag REPAIR | no gold | skip |
| **Trainer** | a class spell learnable at this level is not known | `HandleTrainerListOpcode` (`Handlers/NPCHandler.cpp:97`; sets `TrainerId` `:125-126`) → `HandleTrainerBuySpellOpcode` (checks `:145-148`) per learnable spell | `ObjectMgr::GetTrainer`, creature trainer rows | wrong class trainer; no gold | skip |
| **Quest giver** | giver has an available non-combat quest (talk/deliver: no kill/collect objectives) or a completable one | `HandleQuestgiverHelloOpcode` → `HandleQuestgiverAcceptQuestOpcode` (`Handlers/QuestHandler.cpp:89`, INPLACE); turn-in via `HandleQuestgiverCompleteQuest` (`:501`) then `HandleQuestgiverChooseRewardOpcode` (`:239`); `CanTakeQuest` (`Player.cpp:14509`) | `quest_template` objectives; PhaseGuard quest blacklist | quest needs phase/condition the bot lacks (`PhaseGuard::BlockQuest`) | skip quest, blacklist |
| **Mailbox** | bot has mail (or a 5% chance when passing ≤30 yd of a mailbox) | world queue: `HandleGetMailList` (`Handlers/MailHandler.cpp:559`, THREADUNSAFE; sets SourceGuid `:582`) → `HandleMailTakeMoney`/`HandleMailTakeItem` (`:428`) → close | GO type 19 (HubIndex) | mailbox other phase; bags full | read only, leave mail |
| **Auctioneer** | in a capital/hotspot hub; cooldown 30 min; posts only when the bot holds a tradable item of any quality with a vendor sell price | `HandleAuctionHelloOpcode` (`Handlers/AuctionHouseHandler.cpp:193`) → world queue: `HandleAuctionBrowseQuery` (`:37`), then **post** up to `AH.MaxPostsPerVisit` (3) grey/white items. Single items go through `HandleAuctionSellItem` (`:819`); stackables (commodities, `AuctionPosting::IsCommodity` `AuctionHouse/AuctionHouseMgr.cpp:134-136`) through `HandleAuctionSellCommodity` (`:607`); both THREADUNSAFE. Eligibility = the handler's own checks (tradeable, not conjured, no expiration, `:894-896`); greys are not blocked by the handler. **Price (simple, user: not a big deal):** buyout = vendor `SellPrice` × `AH.PriceMult` random 2.0-4.0 for greys and 3.0-6.0 for whites; items with SellPrice 0 are skipped; duration 12 h. Market for buying stays TC's AuctionHouseBot | npcflag AUCTIONEER; item quality/SellPrice | auctioneer missing; deposit unaffordable; throttled (`CheckThrottle`) | browse only / Idle near the AH |

## Behavior-specific notes

- **Town habit order** (what a player does when coming to town): repair if needed → sell → mailbox → trainer if a level was gained → idle/emote near the bank or AH → leave. The profile can express this as a weight boost after arrival (`arrivalBoost`) rather than a fixed script.
- **Bunny hop realism:** retail players hop mostly on long straight runs and while waiting. Keep it to a few hops per minute at most; never hop indoors.
- **Near/far tiers:** all of this runs only for ACTIVE bots. THROTTLED (frozen) bots change state on abstract timers (virtual travel, no NPC use); PAUSED bots do nothing (PERFORMANCE.md).
