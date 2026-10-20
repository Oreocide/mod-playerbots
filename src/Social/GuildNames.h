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

#ifndef Playerbots_GuildNames_h__
#define Playerbots_GuildNames_h__

#include <cstdint>
#include <string>

// Names for the guilds random bots found (no core types, tested standalone by tests/run_engine_test.sh). Letters and spaces only,
// at most 24 characters: what the core accepts for a guild name
namespace Playerbots
{
namespace GuildNameData
{
inline constexpr char const* Adjectives[] =
{
    "Crimson", "Silent", "Golden", "Iron", "Shadow", "Silver", "Burning", "Frozen", "Wandering", "Broken", "Ancient", "Wild",
    "Fallen", "Rising", "Emerald", "Azure", "Black", "White", "Savage", "Noble", "Hidden", "Twilight", "Stormborn", "Ashen",
    "Gilded", "Hollow", "Grim", "Valiant", "Eternal", "Restless"
};

inline constexpr char const* Nouns[] =
{
    "Vanguard", "Legion", "Company", "Wardens", "Order", "Brotherhood", "Covenant", "Hunters", "Blades", "Guardians", "Raiders", "Pact",
    "Banner", "Watch", "Circle", "Reavers", "Pilgrims", "Sentinels", "Outriders", "Conclave", "Fellowship", "Tide", "Hammers", "Lanterns",
    "Ravens", "Wolves", "Lions", "Drakes", "Oathkeepers", "Vigil"
};

inline constexpr char const* Places[] =
{
    "North", "South", "East", "West", "Dawn", "Dusk", "Mountain", "Valley", "Coast", "Wastes", "Forest", "Highlands", "Marsh", "Plains",
    "Deep", "Storm", "Ember", "Frost", "Sun", "Moon"
};
}

/// A name from three indices (any values): the same indices give the same name, a different pair nearly always a different one
inline std::string MakeGuildName(uint32_t a, uint32_t b, uint32_t pattern)
{
    using namespace GuildNameData;
    char const* adjective = Adjectives[a % (sizeof(Adjectives) / sizeof(*Adjectives))];
    char const* noun = Nouns[b % (sizeof(Nouns) / sizeof(*Nouns))];
    char const* place = Places[(a * 7 + b * 3) % (sizeof(Places) / sizeof(*Places))];

    std::string name;
    switch (pattern % 3)
    {
        case 0: name = std::string(adjective) + " " + noun; break;
        case 1: name = std::string("The ") + adjective + " " + noun; break;
        default: name = std::string(noun) + " of the " + place; break;
    }

    // the longest pairs do not fit the limit: fall back to the short form
    if (name.size() > 24)
        name = std::string(adjective) + " " + noun;
    return name;
}
}

#endif
