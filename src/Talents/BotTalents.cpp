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

#include "BotTalents.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "TraitPackets.h"
#include "WorldSession.h"

namespace Playerbots
{
namespace BotTalents
{
bool ApplyStarterBuild(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->GetSession())
        return false;

    ChrSpecialization specialization = bot->GetPrimarySpecialization();
    if (specialization == ChrSpecialization::None)
        return false;

    // the combat trait config of the active specialization, the one the client's button works on
    UF::TraitConfig const* config = bot->m_activePlayerData->TraitConfigs.FindIf([specialization](UF::TraitConfig const& traitConfig)
    {
        return static_cast<TraitConfigType>(*traitConfig.Type) == TraitConfigType::Combat
            && traitConfig.ChrSpecializationID == int32(specialization)
            && (static_cast<TraitCombatConfigFlags>(*traitConfig.CombatConfigFlags) & TraitCombatConfigFlags::ActiveForSpec) != TraitCombatConfigFlags::None;
    }).second;
    if (!config)
    {
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} has no trait config for specialization {}", bot->GetName(), uint32(specialization));
        return false;
    }

    WorldPackets::Traits::ClassTalentsSetStarterBuildActive packet{ WorldPacket(CMSG_CLASS_TALENTS_SET_STARTER_BUILD_ACTIVE) };
    packet.ConfigID = config->ID;
    packet.Active = true;
    bot->GetSession()->HandleClassTalentsSetStarterBuildActive(packet);
    return true;
}
}
}
