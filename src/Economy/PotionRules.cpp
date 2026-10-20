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

#include "Economy/PotionRules.h"
#include "ItemTemplate.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace Playerbots
{
PotionKind ClassifyPotion(ItemTemplate const* proto, uint32* spellOut)
{
    if (!proto || proto->GetClass() != ITEM_CLASS_CONSUMABLE)
        return PotionKind::None;

    for (ItemEffectEntry const* effect : proto->Effects)
    {
        if (effect->TriggerType != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE);
        if (!spellInfo || spellInfo->HasAura(SPELL_AURA_MOD_REGEN) || spellInfo->HasAura(SPELL_AURA_MOD_POWER_REGEN))
            continue;

        if (spellInfo->HasEffect(SPELL_EFFECT_HEAL))
        {
            if (spellOut)
                *spellOut = spellInfo->Id;
            return PotionKind::Health;
        }

        for (SpellEffectInfo const& spellEffect : spellInfo->GetEffects())
            if (spellEffect.IsEffect(SPELL_EFFECT_ENERGIZE) && spellEffect.MiscValue == POWER_MANA)
            {
                if (spellOut)
                    *spellOut = spellInfo->Id;
                return PotionKind::Mana;
            }
    }
    return PotionKind::None;
}
}
