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

#include "RaceMounts.h"
#include "MountVariety.h"
#include "CollectionMgr.h"
#include "DB2Stores.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "RaceMask.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include <array>

namespace Playerbots::RaceMounts
{
namespace
{
constexpr uint32 ApprenticeRiding = 33388;
constexpr uint32 JourneymanRiding = 33391;

// Spell ids of the vendor ground mounts (Mount.db2, 12.1.0 client data: SourceTypeEnum 2, mount type 230).
// The lists are short on purpose: a few of the race's own looks.
constexpr std::array<uint32, 3> Human = { 458, 6648, 472 };                      // Brown Horse, Chestnut Mare, Pinto
constexpr std::array<uint32, 3> Orc = { 580, 6654, 6653 };                       // Timber Wolf, Brown Wolf, Dire Wolf
constexpr std::array<uint32, 3> Dwarf = { 6777, 6899, 6898 };                    // Gray, Brown, White Ram
constexpr std::array<uint32, 3> NightElf = { 8394, 10789, 10793 };               // Striped, Spotted Frostsaber, Striped Nightsaber
constexpr std::array<uint32, 3> Undead = { 17463, 17464, 17462 };                // Blue, Brown, Red Skeletal Horse
constexpr std::array<uint32, 3> Tauren = { 64657, 18990, 18989 };                // White, Brown, Gray Kodo
constexpr std::array<uint32, 3> Gnome = { 10873, 17454, 10969 };                 // Red, Unpainted, Blue Mechanostrider
constexpr std::array<uint32, 3> Troll = { 8395, 10796, 10799 };                  // Emerald, Turquoise, Violet Raptor
constexpr std::array<uint32, 2> Goblin = { 87090, 87091 };                       // Goblin Trike, Turbo-Trike
constexpr std::array<uint32, 4> BloodElf = { 34795, 35022, 35018, 35020 };       // Red, Black, Purple, Blue Hawkstrider
constexpr std::array<uint32, 3> Draenei = { 34406, 35710, 35711 };               // Brown, Gray, Purple Elekk
constexpr uint32 RunningWild = 87840;                                            // the worgen racial (not a Mount.db2 mount): used only when the bot knows it

// Pandaren (faction mounts) and any race without a list of its own
std::span<uint32 const> FactionBasic(uint8 race)
{
    return Player::TeamForRace(race) == ALLIANCE ? std::span<uint32 const>(Human) : std::span<uint32 const>(Orc);
}

// a fixed starting point per character (the finalizer of a 32 bit hash spreads sequential guids evenly)
uint32 GuidHash(Player const* bot)
{
    uint32 h = uint32(bot->GetGUID().GetCounter());
    h ^= h >> 16;
    h *= 0x7feb352d;
    h ^= h >> 15;
    h *= 0x846ca68b;
    h ^= h >> 16;
    return h;
}
}

std::span<uint32 const> For(uint8 race)
{
    switch (race)
    {
        case RACE_HUMAN:
        case RACE_KUL_TIRAN:
            return Human;
        case RACE_ORC:
        case RACE_MAGHAR_ORC:
            return Orc;
        case RACE_DWARF:
        case RACE_DARK_IRON_DWARF:
        case RACE_EARTHEN_DWARF_HORDE:
        case RACE_EARTHEN_DWARF_ALLIANCE:
            return Dwarf;
        case RACE_NIGHTELF:
        case RACE_VOID_ELF:
            return NightElf;
        case RACE_UNDEAD_PLAYER:
            return Undead;
        case RACE_TAUREN:
        case RACE_HIGHMOUNTAIN_TAUREN:
            return Tauren;
        case RACE_GNOME:
        case RACE_MECHAGNOME:
            return Gnome;
        case RACE_TROLL:
        case RACE_ZANDALARI_TROLL:
            return Troll;
        case RACE_GOBLIN:
        case RACE_VULPERA:
            return Goblin;
        case RACE_BLOODELF:
        case RACE_NIGHTBORNE:
            return BloodElf;
        case RACE_DRAENEI:
        case RACE_LIGHTFORGED_DRAENEI:
            return Draenei;
        case RACE_WORGEN:   // Running Wild when known (PickKnown), else the human mounts
            return Human;
        default:
            return FactionBasic(race);
    }
}

uint32 PickKnown(Player const* bot)
{
    if (bot->GetRace() == RACE_WORGEN && bot->HasSpell(RunningWild))
        return RunningWild;

    std::span<uint32 const> list = For(bot->GetRace());
    if (list.empty())
        return 0;

    uint32 h = GuidHash(bot);
    for (size_t i = 0; i < list.size(); ++i)
    {
        uint32 spell = list[(h + i) % list.size()];
        if (bot->HasSpell(spell))
            return spell;
    }
    return 0;
}

void Grant(Player* bot)
{
    if (!sPlayerbotsConfig.MountsRacialGrant || !bot || !bot->IsInWorld() || !bot->GetSession())
        return;

    uint32 level = bot->GetLevel();
    if (level >= 10 && bot->GetSkillValue(SKILL_RIDING) < 75)
    {
        bot->LearnSpell(ApprenticeRiding, false);
        if (bot->GetSkillValue(SKILL_RIDING) < 75)
            bot->SetSkill(SKILL_RIDING, 1, 75, 75);
    }
    if (level >= 20 && bot->GetSkillValue(SKILL_RIDING) < 150)
    {
        bot->LearnSpell(JourneymanRiding, false);
        if (bot->GetSkillValue(SKILL_RIDING) < 150)
            bot->SetSkill(SKILL_RIDING, 2, 150, 150);
    }

    if (sPlayerbotsConfig.MountsVariety && level >= sPlayerbotsConfig.RpgMountMinLevel)
        MountVariety::Grant(bot);

    if (level < sPlayerbotsConfig.RpgMountMinLevel || PickKnown(bot))
        return;

    std::span<uint32 const> list = For(bot->GetRace());
    if (list.empty())
        return;

    bot->GetSession()->GetCollectionMgr()->AddMount(list[GuidHash(bot) % list.size()], MOUNT_STATUS_NONE);
}

void Validate()
{
    uint32 problems = 0;
    for (uint8 race : { uint8(RACE_HUMAN), uint8(RACE_ORC), uint8(RACE_DWARF), uint8(RACE_NIGHTELF), uint8(RACE_UNDEAD_PLAYER),
        uint8(RACE_TAUREN), uint8(RACE_GNOME), uint8(RACE_TROLL), uint8(RACE_GOBLIN), uint8(RACE_BLOODELF), uint8(RACE_DRAENEI),
        uint8(RACE_WORGEN), uint8(RACE_PANDAREN_ALLIANCE), uint8(RACE_PANDAREN_HORDE), uint8(RACE_NIGHTBORNE), uint8(RACE_HIGHMOUNTAIN_TAUREN),
        uint8(RACE_VOID_ELF), uint8(RACE_LIGHTFORGED_DRAENEI), uint8(RACE_ZANDALARI_TROLL), uint8(RACE_KUL_TIRAN), uint8(RACE_DARK_IRON_DWARF),
        uint8(RACE_VULPERA), uint8(RACE_MAGHAR_ORC), uint8(RACE_MECHAGNOME), uint8(RACE_EARTHEN_DWARF_HORDE), uint8(RACE_EARTHEN_DWARF_ALLIANCE) })
    {
        for (uint32 spell : For(race))
        {
            if (!sDB2Manager.GetMount(spell))
            {
                TC_LOG_ERROR("server.playerbots", "Playerbots: racial mount spell {} of race {} is not in Mount.db2", spell, uint32(race));
                ++problems;
            }
        }
    }
    TC_LOG_INFO("server.playerbots", "Playerbots: racial mount lists checked ({} problems)", problems);
}
}
