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

#ifndef Playerbots_ProfessionsAi_h__
#define Playerbots_ProfessionsAi_h__

namespace Playerbots
{
class AiRegistry;
}

namespace Playerbots::Prof
{
/// Everything of the professions group: the "professions" strategy (disenchant, enchant, recipes, lockboxes, fishing), which every
/// bot runs outside battlegrounds and arenas through a context rule, and the orders "outfit", "fish" and "open". Called from
/// RegisterGenericAi.
void RegisterProfessionsAi(AiRegistry& registry);
}

#endif // Playerbots_ProfessionsAi_h__
