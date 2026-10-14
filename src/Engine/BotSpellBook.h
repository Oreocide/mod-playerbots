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

#ifndef Playerbots_BotSpellBook_h__
#define Playerbots_BotSpellBook_h__

#include "Define.h"
#include <vector>

class Player;
class SpellInfo;
class Unit;

namespace Playerbots
{
/// Class-agnostic view of the spells a bot knows, classified from their spell effects.
/// This replaces the hand-written per-class action lists of the 3.3.5 module until
/// retail spec strategies exist: every spec gets a usable (if naive) rotation.
class BotSpellBook
{
public:
    /// Dummy, test, and broken trigger spells. GiveLevel learns these; they stall the server if left on a bot
    static bool IsJunk(SpellInfo const* spellInfo);
    /// Removes dummy/test spells and auras (including ones applied by items, not just the spellbook)
    static void StripJunk(Player* bot);

    /// Rebuilds the lists when the spec, level or number of known spells changed
    void Refresh(Player const* bot);

    std::vector<SpellInfo const*> const& GetDamageSpells() const { return _damage; }
    std::vector<SpellInfo const*> const& GetHealSpells() const { return _heal; }

    /// Tries the spells in order, returns true when one was cast
    static bool CastFirstUsable(Player* bot, Unit* target, std::vector<SpellInfo const*> const& spells, uint32 maxAttempts);

private:
    std::vector<SpellInfo const*> _damage;
    std::vector<SpellInfo const*> _heal;

    uint32 _specId = 0;
    uint32 _level = 0;
    size_t _spellCount = 0;
};
}

#endif // Playerbots_BotSpellBook_h__
