# Playerbots v1: inventory

Scope: town-life behaviors (idle, wander, hop, emote, mount, taxi, service NPCs) plus pause/throttle on TC master 12.1.5.70391.
The design candidate is the existing local module `modules/mod-playerbots` (86 source files, 18.2k LOC, mostly not play-tested: `NOTES.md:12-27`). AzerothCore mod-playerbots is a design reference only.

Path roots:
- `AC:` = `/home/brandon/azerothcore-wotlk/modules/mod-playerbots/` (commit 5d213ed8, 2026-09-19).
- `mod:` = `modules/mod-playerbots/`.
- Core paths = `src/server/game/`.

Confidence: H = read in code, M = inferred from code, L = not checked.

## 1. AC mod-playerbots (reference, GPL-2.0-or-later)

Portability: **direct** = copy with minor edits; **adapt** = keep the logic, swap API calls; **rewrite** = rebuild on retail systems; **skip**.

| AC dir | LOC | Responsibility | Portability | TC module counterpart | Conf |
|---|---|---|---|---|---|
| `src/Bot/Engine` | 3,777 | Engine, Strategy/Trigger/Action/Value, AiObjectContext, multipliers | adapt (done) | `mod:src/Engine/` (1,574 LOC) | H |
| `src/Bot` (PlayerbotAI 6,916; RandomPlayerbotMgr 3,474; PlayerbotMgr 1,964; RandomBotLevelMgr 1,171) | 14,884 | per-bot AI, population, master bots, AllowActive | adapt logic; rewrite session/login | `mod:src/PlayerbotAI.*`, `RandomBotMgr.*`, `PlayerbotMgr.*` | H |
| `src/Bot/Factory` | 7,747 | gear/talents/glyphs | rewrite (retail items, traits) | `mod:src/GearFactory.*`, `Talents/` | H |
| `src/Bot/Cmd` | 1,725 | ChatHelper, ChatFilter, command server | adapt (link formats changed) | `mod:src/Cmd/` | H |
| `src/Mgr/Travel` (TravelMgr 5,256; TravelNode 2,581) | 9,528 | destinations, hotspots, flight masters, travel graph | rewrite (3.3.5 DB/DBC data) | `mod:src/Travel/` (TravelMgr, HubIndex, Graph) | H |
| `src/Mgr/{Item,Guild,Talent,Text,Move,Security}` | 9,922 | random items, BiS, guild tasks, flee | rewrite / skip | — | H |
| `src/Db` | 343 | PlayerbotRepository (no `PlayerbotDbStore` in this tree) | adapt (done) | `mod:src/Db/PlayerbotRepository.*` | H |
| `src/Ai/World/Rpg` | 4,828 | NewRpg state machine, NPC dispatch, quest helper | adapt (core-agnostic logic) | `mod:src/Strategies/Rpg/` | H |
| `src/Ai/Base` | 67,885 | generic actions/triggers/values (movement, mount, vendor, quest, emote, taxi) | adapt; raw `WorldPacket` code → rewrite | partial in `mod:src/Strategies/` | H |
| `src/Ai/{Class,Dungeon,Raid}` | 123,807 | WotLK rotations, encounter scripts | skip | `mod:src/Rotation/` + `data/rotations` (retail) | H |
| `src/Script`, `src/Util` | 4,790 | hooks, loader, ServerFacade | rewrite | `mod:src/Script/` | H |

### Key reference files per v1 behavior

| Behavior | AC file:line | Described behavior (reimplement from this) | Conf |
|---|---|---|---|
| State picker | `AC:src/Ai/World/Rpg/.../NewRpgAction.cpp:251` → `NewRpgBaseAction.cpp:1211` | weighted random state from IDLE. Weights `conf:1550-1557`: WanderRandom 15, WanderNpc 20, GoGrind 15, GoCamp 10, DoQuest 60, TravelFlight 15, Rest 5. +15 WanderNpc/GoCamp in capitals. Timeouts back to IDLE: `conf:1507-1511` | H |
| Rest/idle | `NewRpgAction.cpp:455` | sits; 10%/tick applaud/cheer/laugh; 3 s wait | H |
| Wander | `NewRpgBaseAction.cpp:328` | random point at 0.4-1.0 × step; ≤8 tries; rejects unreachable, water, hostile areas | H |
| Long move / stuck | `NewRpgBaseAction.cpp:77`, `:141-178`; `Ai/Base/Trigger/StuckTriggers.cpp:13` | 70 yd path steps; <5 yd progress over 5 tries/90 s → teleport to dest; 5 min stuck trigger | H |
| Jump | `Ai/Base/Actions/MovementActions.cpp:2785-2808` | 1/s check while moving on foot outdoors, chance 0.02, 2-5 yd forward | H |
| Emote | `Ai/Base/Strategy/EmoteStrategy.cpp:10-34`, `Actions/EmoteAction.cpp:649` | "often"/"seldom" trigger rates (`TriggerContext.h:349-352`); off by default | H |
| Mount | `Ai/Base/Actions/CheckMountStateAction.cpp:64/150` | outdoors, level ≥20; mount when target >max(21, reach+10) yd; dismount at reach+2 | H |
| Taxi | `NewRpgAction.cpp:645`, `NewRpgBaseAction.cpp:1193` | random destination from nearest FM; walk there, dismount, learn node, `ActivateTaxiPathTo` | H |
| NPC dispatch | `NewRpgNpcDispatch.cpp:52` (score), `:151` (interact) | repair +100 if durability <80%; vendor +60 if bags >80%; innkeeper +40; AH +50 (capital); trainer +35; quest giver +20; 8 s stay | H |
| Auctioneer | `NewRpgNpcDispatch.cpp:105-120,:217` | visit only; no listing/buying logic | M |
| Mailbox | `Ai/Base/Actions/CheckMailAction.cpp:13/60` | take + return mail; autonomous trigger commented out (`WorldPacketHandlerStrategy.cpp:48`) | H |
| Innkeeper | `Ai/Base/Actions/SetHomeAction.cpp:12`; `Value/MaintenanceValues.cpp:29` | bind if bind point >1000 yd away | H |
| Trainer | `Ai/Base/Actions/TrainerAction.cpp:18` | train when spare money >0 | H |
| Activity | `src/Bot/PlayerbotAI.cpp:4674` (`AllowActive`), `:4870` (`AllowActivity` cache 4.5 s) | force-active near players/guild/group; else hash < `BotActiveAlone`; SmartScale on world diff | H |

## 2. Other bot-related AC modules (lower priority)

| Module | LOC | Purpose | Needs online bots | License | Use |
|---|---|---|---|---|---|
| mod-ah-bot-plus | 3.4k | AH seller/buyer bot | no | AGPL-3 in headers, no LICENSE file | do not copy |
| mod-ollama-chat | 34k | LLM chat replies | yes | AGPL-3 (README says GPL-3) | do not copy; behavior ref for the chat hook |
| mod-bot-chat-intents | 440 | chat intent → safe bot action | yes | none (all rights reserved) | behavior ref only |
| mod-bot-trade-chatter | 1.0k | "WTS" Trade channel posts | yes | none | behavior ref only |
| mod-bot-spawners | 255 | GM-placed hotspot NPCs | yes (TravelMgr) | none | idea already in `mod:src/Script/npc_bot_hotspot.cpp` |
| mod-duel-anchors | 62 | duel gathering NPCs | yes | none | behavior ref only |
| mod-dungeon-clear | 116k | tank-led dungeon clear | yes | AGPL-3 | out of v1 scope |

## 3. TC built-in to reuse

| Subsystem | Path | Use in v1 | Conf |
|---|---|---|---|
| AuctionHouseBot (seller/buyer) | `AuctionHouseBot/AuctionHouseBot.h`, `AuctionHouseBotSeller.cpp`, `AuctionHouseBotBuyer.cpp` (`IsBotChar` `AuctionHouseBot.h:243`) | supplies the market, so the bot auctioneer behavior stays visit + browse | H |

## 4. TC master subsystems the bots use

| Subsystem | API (cite) | Used by | Conf |
|---|---|---|---|
| WorldSession (bot) | ctor `isBot` flag `Server/WorldSession.h:986`; `SendPacket` bot branch `WorldSession.cpp:262-274`; `LoginBotPlayer` `Handlers/CharacterHandler.cpp:1313`; `InitializeSession(callback)` | login, packet sink | H |
| Accounts | `Battlenet::AccountMgr::CreateBattlenetAccount` `Accounts/BattlenetAccountMgr.cpp:34`; `AccountMgr::CreateAccount` `Accounts/AccountMgr.cpp:47` | `mod:src/RandomBotMgr.cpp:496,515` | H |
| Character creation | `HandleCharCreateOpcode` `Handlers/CharacterHandler.cpp:811`; `ValidateAppearance` `:760`; `Player::Create` `Entities/Player/Player.cpp:390` (NPE position `:425`); `CharacterCreateInfo::UseNPE` `Server/Packets/CharacterPackets.h:67` | `mod:src/RandomBotMgr.cpp:923-930` | H |
| Save/logout | `Player::SaveToDB` `Player.cpp:20448`; `WorldSession::LogoutPlayer`/`KickPlayer` (bot branches `WorldSession.cpp:729,754`) | logout, pause | H |
| Movement | `MotionMaster::MovePoint` `Movement/MotionMaster.h:169`, `MoveRandom` `:159`, `MoveJump` `:196`, `MoveTaxiFlight` `:213`; `PathGenerator` `Movement/PathGenerator.h:58-85` (`GetActualEndPosition` `:80`, limits `:32-35`); `MoveSplineInit::MoveTo` straight fallback `Movement/Spline/MoveSplineInit.cpp:293-303` | BotMover | H |
| Water/fly/mount | `Unit::IsInWater` `Entities/Unit/Unit.cpp:3350`; `Unit::GetMountCapability` `:8382`; `SpellInfo::CheckLocation` (`SPELL_FAILED_NOT_HERE`) `Spells/SpellInfo.cpp:2170` | mount choice | H |
| Taxi | `Player::ActivateTaxiPathTo(nodes, npc, …)` `Player.cpp:23168`; `PlayerTaxi::IsTaximaskNodeKnown/SetTaximaskNode` `Entities/Player/PlayerTaxi.h:50/56`; `ObjectMgr::GetNearestTaxiNode` `Globals/ObjectMgr.cpp:6646`; `HandleActivateTaxiOpcode` `Handlers/TaxiHandler.cpp:159` | taxi behavior | H |
| Auction | `HandleAuctionHelloOpcode` `Handlers/AuctionHouseHandler.cpp:193`; `HandleAuctionBrowseQuery` `:37`; interaction start `:1008` | AH browse | H |
| Mail | `CanOpenMailBox` `Handlers/MailHandler.cpp:38`; `HandleGetMailList` `:559` (interaction `:582`); `HandleMailTakeItem` `:428`; mailbox GO type 19 | mailbox | H |
| Gossip/vendor | `Player::GetNPCIfCanInteractWith` `Player.cpp:1842`; `GetGameObjectIfCanInteractWith` `:1899`; `PrepareGossipMenu` `:13913`; `HandleListInventoryOpcode` `Handlers/ItemHandler.cpp:564` (interaction `:586`); `HandleSellItemOpcode` `:381`; `BuyItemFromVendorSlot` `Player.cpp:23724` | vendor | H |
| Repair | `HandleRepairItemOpcode` `Handlers/NPCHandler.cpp:403`; `Player::DurabilityRepairAll` `Player.cpp:4601` | repair | H |
| Trainer | `HandleTrainerListOpcode` `Handlers/NPCHandler.cpp:97` (interaction `:125-126`); buy checks `:145-148` | trainer | H |
| Quest | `HandleQuestgiverAcceptQuestOpcode` `Handlers/QuestHandler.cpp:89`; `Player::CanTakeQuest` `Player.cpp:14509`; `AddQuestAndCheckCompletion` `:14663`; `RewardQuest` `:15052`; `GetQuestDialogStatus` `:16126` | quest giver | H |
| Bind/rest | `HandleBinderActivateOpcode` `Handlers/NPCHandler.cpp:330` → `SendBindPoint` `:349` (spell 3286) → `Spell::EffectBind` `Spells/SpellEffects.cpp:5263` → `Player::SetHomebind` `Player.cpp:17923` (DB write `:17929`); `RestMgr::SetRestFlag` `Entities/Player/RestMgr.cpp:95` | innkeeper | H |
| Emotes | `Unit::HandleEmoteCommand` `Unit.cpp:1663`; `HandleTextEmoteOpcode` `Handlers/ChatHandler.cpp:703` | emote | H |
| Phasing | `WorldObject::InSamePhase` `Entities/Object/Object.h:314`; `CanSeeOrDetect` `Object.cpp:855`; `PhasingHandler::InDbPhaseShift` `Phasing/PhasingHandler.cpp:577` | target filter | H |
| Threading | `World::Update`: sessions `World/World.cpp:2274` → maps `:2319` → `OnWorldUpdate` `:2437`; `PlayerScript::OnUpdate` (local hook) `Scripting/ScriptMgr.h:793` fired at `Player.cpp:902,1103` | scheduler | H |
| Teleport handshakes | `HandleMoveTeleportAck` `Handlers/MovementHandler.cpp:413`; bot replies in `mod:src/PlayerbotAI.cpp:625`, `mod:src/PlayerbotMgr.cpp:358-362` | teleports | H |
