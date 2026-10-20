# Specialization rotations

One set per ChrSpecialization id, in `*.rot` files (loaded at startup, `.bot rotation reload` reads them again).
Format and facts: see `src/Rotation/RotationScript.h`. Spell names are matched against the spells the bot knows, a
name the bot does not know is skipped, so lists are written for the current expansion and work when talents change.
Lists: `combat` (needed for a rotation to be used), `buffs` (out of combat), `heal` (healers), `interrupt` and
`defensive` (before `combat`). Put an unconditional filler last so a list never runs dry.
Seeds: standard retail priorities and the casts of Blizzard's own Story raid companions
(`trinitycore-tracking/follower-ai/follower_rotations.md`).

## Utility list, targets and facts added for group play

`utility` runs in combat after `interrupt` and `defensive`, before `combat` (dispels, crowd control, battle resurrection, threat tools).
Out of combat, `buffs` also runs group buffs and resurrections.

Targets (besides enemy, self, pet, master, lowest_ally, tank, casting_enemy, loose_enemy):
- `dead_ally`: a dead group member in range (the spell decides whether it may be cast in combat).
- `lowest_mana_ally`: the ally with the least mana. `ally_without_buff.NAME`: a living ally without that buff (`ally_without_buff.arcane_intellect`).
- `dispellable_ally[.magic|curse|poison|disease]`: an ally with a harmful aura of that dispel kind (no kind = any of the four).
- `dispellable_enemy[.magic|enrage]`: an enemy with a beneficial aura of that kind (purge, spellsteal, tranquilizing shot, soothe). No kind = magic.
- `cc_target`: an enemy that is not the main target and not crowd controlled yet. `enemy_healer`: an enemy that is casting a heal. No rotation uses these two yet (Polymorph was left out until crowd control is play-tested: target selection would also have to skip crowd-controlled enemies).

All of these targets and the `utility` list are inert until `Playerbots.Rotation.Utility = 1`.

Facts: `self.has_aggro` (an enemy is attacking me), `self.casting`, `target.cc`, `target.power` (percent of its main power),
`target.has_magic_buff|has_enrage|has_magic_debuff|has_curse|has_poison|has_disease`, `talent.NAME` (the bot knows a spell or
talent of that name, passives included).
