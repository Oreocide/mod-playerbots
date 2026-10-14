# Specialization rotations

One set per ChrSpecialization id, in `*.rot` files (loaded at startup, `.bot rotation reload` reads them again).
Format and facts: see `src/Rotation/RotationScript.h`. Spell names are matched against the spells the bot knows, a
name the bot does not know is skipped, so lists are written for the current expansion and work when talents change.
Lists: `combat` (needed for a rotation to be used), `buffs` (out of combat), `heal` (healers), `interrupt` and
`defensive` (before `combat`). Put an unconditional filler last so a list never runs dry.
Seeds: standard retail priorities and the casts of Blizzard's own Story raid companions
(`trinitycore-tracking/follower-ai/follower_rotations.md`).
