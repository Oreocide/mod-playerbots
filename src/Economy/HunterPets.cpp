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


#include "Economy/HunterPets.h"
#include "CreatureData.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "Random.h"
#include <iterator>

namespace Playerbots::HunterPets
{
namespace
{
constexpr uint8 MinLevel = 10;
constexpr uint32 TameBeastSpell = 1515;

// One early-zone beast per family, the model and abilities come from the family; the level comes from the bot
constexpr uint32 Beasts[] =
{
    118,   // wolf
    681,   // cat
    120,   // spider
    822,   // bear
    454,   // boar
    1084,  // crocolisk
    154,   // carrion bird
    830,   // crab
    1557,  // gorilla
    854,   // raptor
    2321,  // tallstrider
    5431,  // turtle
    3249,  // bird of prey
};

bool HasHunterPet(PetStable const* stable)
{
    if (!stable)
        return false;

    for (auto const& pet : stable->ActivePets)
        if (pet && pet->Type == HUNTER_PET)
            return true;

    for (auto const& pet : stable->StabledPets)
        if (pet && pet->Type == HUNTER_PET)
            return true;

    return false;
}
}

bool Grant(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->GetClass() != CLASS_HUNTER || bot->GetLevel() < MinLevel)
        return false;

    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() || bot->GetTransport() || bot->GetVehicle() || !bot->GetPetGUID().IsEmpty())
        return false;

    if (HasHunterPet(bot->GetPetStable()))
        return false;

    uint32 entry = Beasts[urand(0, std::size(Beasts) - 1)];
    Pet* pet = bot->CreateTamedPetFrom(entry, TameBeastSpell);
    if (!pet)
        return false;

    float x, y, z;
    bot->GetClosePoint(x, y, z, pet->GetCombatReach(), PET_FOLLOW_DIST, pet->GetFollowAngle());
    pet->Relocate(x, y, z, bot->GetOrientation());

    // like the core's create-tamed-pet effect, the result of AddToMap is not checked: InitTamedPet has already put the pet in the stable
    pet->GetMap()->AddToMap(pet->ToCreature());

    pet->SendNewlyTamed();
    bot->SetMinion(pet, true);
    pet->SavePetToDB(PET_SAVE_AS_CURRENT);
    bot->PetSpellInitialize();

    TC_LOG_DEBUG("server.playerbots", "Playerbots: hunter {} tamed beast {}", bot->GetName(), entry);
    return true;
}
}
