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

#ifndef Playerbots_Multiplier_h__
#define Playerbots_Multiplier_h__

namespace Playerbots
{
class PlayerbotAI;
class Action;

/// Scales the relevance of every candidate action: 1 = unchanged, a fraction = less urgent, 0 = vetoed
/// (a passive bot vetoes attacks, a bot at a boss vetoes movement)
class Multiplier
{
public:
    virtual ~Multiplier() = default;

    virtual float GetValue(PlayerbotAI& ai, Action* action) = 0;
};
}

#endif // Playerbots_Multiplier_h__
