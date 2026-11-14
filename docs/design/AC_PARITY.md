# AC parity (mod-playerbots#38)

What AzerothCore mod-playerbots does that this module did not, built 2026-11-13. Every behaviour sits behind its own key,
off by default (`conf/playerbots.conf.dist` documents each one). Everything runs only for simulated bots (near a player);
far and frozen bots do nothing new.

Decisions (2026-11-13): skinning, fishing (pools too), chests (shared ones too), lockboxes and herb/ore gathering give real
loot, as in AC. Social actions aimed at players use AC's numbers (AC ships invites and duel
challenges switched off: the keys here are off too, and use AC's rates when switched on), with two limits the user chose:
an invite or duel challenge reaches the same player at most once per 30 minutes, and bots post at most one channel line per
zone every 20 seconds. Bots buy their first bank tab (`Social.Bank.BuyTab`). War Mode for about 1 in 3 bots.
Enchanting own gear is built, its key is off. Master loot is not built.

## Loot (`src/Loot/BotLoot.*`)

Replaces the generic "loot" action, "corpse to loot" trigger and "nearest corpse" value (registered after them).

- `Loot.Visible`: walk to the corpse, keep the window open (the kneel shows to others), take, close. Retail evidence: other
  players' UNIT_FLAG_LOOTING (0x400) flips on loot open/close, held a median 1.0 s (p10 0, p90 2.0 s; 651 loots in 8
  captures). `HoldMinMs`/`HoldMaxMs`/`InstantPct` shape it.
- Every open closes every loot the open put in view. Area loot opens all corpses within 30 yards; the old action closed
  only the one it walked to, leaving the rest open and the looting flag on. This part applies with `Visible` off too.
- `Loot.Skin`: skin a looted corpse (or herb/mine one that needs it) with a known SKINNING-effect spell.
- `Loot.Chests`: open-world chests, shared ones too; never in instances or boss chests. Unskilled locks by the lowest
  "Opening"-type spell (found at startup), rogues by Pick Lock, keys if carried.
- `Loot.MasterLeash`: a party bot leaves loot farther than this from its master.
- `ll` order: all | normal (no grey) | quest | skill, `ll +<items>` always, `ll -<items>` never. Kept per bot until logout.

## Quests (`src/Quest/`)

- `Party.QuestSync` (PartyQuests): a PlayerScript notes the quest NPC a human had open when a quest entered or left the
  log; party bots near that NPC take the same quest, or hand it in (best reward, or the one asked with `reward <item>`).
  While the master talks to a quest NPC, bots hand in what they finished for it. QuestTravel still stays out of party bots.
- `Party.QuestShare`: a bot shares each shareable quest once per group with members near it who can take it; `share`.
- `Quest.UseItems` (QuestItems): quest items with an on-use spell that needs a creature or gameobject target are used on
  what the spell's target conditions name, else hostile creatures and quest NPCs, nearest first, three tries per run; in a
  fight only on the bot's own enemy. Self, area and ground items are never guessed.

## Party orders (`src/Cmd/PartyOrders.*`)

`formation near|melee|queue|line|arrow|circle|chaos|far`, `range <5-40>`, `rti <icons>`, `focus heal <names>`,
`save mana <0-5>`, `position <name> set|<name>|clear` (AC "rtsc", spots from where the master stands), `taxi`, `los`,
`reputation`. Hooks: PlayerbotAI::Follow/Chase, GroupCombat's ranged slots, the heal pickers in GenericStrategies and
RotationRunner ("lowest_ally"), RaidIcons::GetView. Each returns the old value while no order was given.

## Transports (`src/Travel/BotTransports.*`)

`Transport.Board`: a party bot follows its master onto a boat or zeppelin (only while it waits at the dock), elevator or
tram: mesh walk to the dock edge, then the one straight step onto the deck (GAPS-bots G45 exception), then
`AddPassenger` with a movement update to the clients (a bot has no client to send one). Aboard it stands still; it steps
off when the master does. `board` boards at once. Random bots still cross by virtual travel only.

## Professions (`src/Economy/`: BotCrafting, BotFishing, BotLootWatch, BotOutfits, ProfessionsAi, ProfessionsConfig)

`Craft.AnyProfession` (every production profession at its station, through the rpg Craft activity), `Disenchant.Enable`,
`Enchant.Enable`, `Recipes.Enable`, `Lockbox.Enable` + `open`, `Fishing.Enable`/`WithMaster` + `fish`,
`Outfits.Enable` + `outfit` (sets kept in playerbots_db_store, worn through HandleUseEquipmentSet). A "professions"
strategy reaches every bot outside battlegrounds through a context rule. Loot from fishing and disenchanting is taken on
the world thread (BotLootWatch).

`Rpg.GatherReal` (BotGathering): the rpg gather step casts the node's gathering spell (the bot's own, else the lowest
reagent-free spell of that lock type) and the loot watcher takes the loot; a node that is gone is skipped.

## Social (`src/Social/`)

`Social.Channel.*` (General, Trade, LookingForGroup posts and replies; rephrase hook in SocialRephrase.h for mod-ollama),
`Social.Invite.*` + `lfg`, `Social.Duel.*`, `Social.WarMode.*`, `Social.Trade.Gift` + `trade`, `Social.Bank.*` + `bank`,
`Social.GuildBank.*` + `gb`, `mail`/`sendmail`, `Social.MageRefreshments`. AC sources of each rate are in the conf block.

## Not covered

Master loot, random bots riding boats outside a party, loot filter and
order settings kept across logouts.
