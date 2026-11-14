/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "BotGathering.h"
#include "BotLootWatch.h"
#include "Config.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "Loot.h"
#include "Map.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <array>

namespace Playerbots::BotGathering
{
namespace
{
constexpr float NodeReach = 5.0f;
constexpr uint32 LootWaitMs = 6000;     // after the cast: the loot opens when the spell lands

Config sConfig;

// The lowest reagent-free spell per lock type that opens it: a bot with the profession but not the spell (skills are seeded
// offline, spells are not always learned) still gathers. Built at startup on the world thread, read-only afterwards
std::array<uint32, 128> sLockSpell{};

void BuildSpells()
{
    sSpellMgr->ForEachSpellInfo([](SpellInfo const* info)
    {
        if (info->Difficulty != DIFFICULTY_NONE || info->IsPassive())
            return;
        for (int32 reagent : info->Reagent)
            if (reagent > 0)
                return;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_OPEN_LOCK)
                continue;
            uint32 type = uint32(effect.MiscValue);
            if (type < sLockSpell.size() && (!sLockSpell[type] || info->Id < sLockSpell[type]))
                sLockSpell[type] = info->Id;
        }
    });
}

bool OpensLockType(SpellInfo const* info, uint32 lockType)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.Effect == SPELL_EFFECT_OPEN_LOCK && uint32(effect.MiscValue) == lockType)
            return true;
    return false;
}

uint32 KnownSpellFor(Player* bot, uint32 lockType)
{
    for (auto const& [id, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled || !known.active)
            continue;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE); info && !info->IsPassive() && OpensLockType(info, lockType))
            return id;
    }
    return 0;
}

/// The gathering lock type of a node (herbalism or mining line of any expansion), 0 if it has none
uint32 GatherLockType(GameObject const* go)
{
    LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
    if (!lock)
        return 0;
    for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
        if (lock->Type[i] == LOCK_KEY_SKILL && lock->Index[i] > 0)
            return uint32(lock->Index[i]);
    return 0;
}
}

Config const& GetConfig()
{
    return sConfig;
}

void LoadConfig()
{
    Config cfg;
    cfg.Real = sConfigMgr->GetBoolDefault("Playerbots.Rpg.GatherReal", false);
    sConfig = cfg;
}

void OnStartup()
{
    sLockSpell.fill(0);
    BuildSpells();
}

GatherResult TryGather(Player* bot, uint64 spawnId, uint32& castMs)
{
    castMs = 0;
    GameObject* go = bot->GetMap()->GetGameObjectBySpawnId(spawnId);
    if (!go || !go->isSpawned() || go->getLootState() == GO_JUST_DEACTIVATED || go->HasDynamicFlag(GO_DYNFLAG_LO_NO_INTERACT)
        || !bot->IsWithinDist(go, NodeReach))
        return GatherResult::Gone;
    if (Loot const* loot = go->GetLootForPlayer(bot); loot && loot->isLooted())
        return GatherResult::Gone;

    uint32 lockType = GatherLockType(go);
    if (!lockType || lockType >= sLockSpell.size())
        return GatherResult::Cannot;

    uint32 spellId = KnownSpellFor(bot, lockType);
    if (!spellId)
        spellId = sLockSpell[lockType];
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE) : nullptr;
    if (!info || bot->CastSpell(go, spellId) != SPELL_CAST_OK)
        return GatherResult::Cannot;

    castMs = info->CalcCastTime();
    Prof::LootWatch::Expect(bot, go->GetGUID(), castMs + LootWaitMs);
    return GatherResult::Casting;
}
}
