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

#include "FlyingMounts.h"
#include "CollectionMgr.h"
#include "ConditionMgr.h"
#include "DB2Stores.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveToward.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "World/WorldActionQueue.h"

namespace Playerbots
{
namespace FlyingMounts
{
bool IsFlyer(uint32 mountSpell)
{
    MountEntry const* mount = sDB2Manager.GetMount(mountSpell);
    if (!mount)
        return false;

    DB2Manager::MountTypeXCapabilitySet const* capabilities = sDB2Manager.GetMountCapabilities(mount->MountTypeID);
    if (!capabilities)
        return false;

    for (MountTypeXCapabilityEntry const* link : *capabilities)
        if (MountCapabilityEntry const* capability = sMountCapabilityStore.LookupEntry(link->MountCapabilityID))
            if (capability->Flags & MOUNT_CAPABILITY_FLAG_FLYING)
                return true;
    return false;
}

std::vector<uint32> Known(Player const* bot)
{
    std::vector<uint32> spells;
    if (!bot || !bot->GetSession() || !bot->GetSession()->GetCollectionMgr())
        return spells;

    for (auto const& [spell, flags] : bot->GetSession()->GetCollectionMgr()->GetAccountMounts())
        if (IsFlyer(spell))
            spells.push_back(spell);
    return spells;
}

uint32 AnyFlyer()
{
    for (MountEntry const* mount : sMountStore)
        if (mount->SourceSpellID && !mount->PlayerConditionID && IsFlyer(mount->SourceSpellID))
            return mount->SourceSpellID;
    return 0;
}

std::vector<std::string> Describe(Player* bot, bool steadyTest)
{
    std::vector<std::string> lines;
    if (!bot || !bot->IsInWorld())
        return { "not in the world" };

    uint32 riding = bot->GetSkillValue(SKILL_RIDING);
    lines.push_back(Trinity::StringFormat("{}: level {}, riding skill {} (steady flight needs {}), map {} area {} (map instanceable: {})",
        bot->GetName(), bot->GetLevel(), riding, MinRidingSkill, bot->GetMapId(), bot->GetAreaId(), bot->GetMap()->Instanceable()));

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    lines.push_back(Trinity::StringFormat("  area mount flags {} -> flying allowed: {}", area ? int32(area->MountFlags) : -1,
        area && area->GetMountFlags().HasFlag(AreaMountFlags::AllowFlyingMounts)));

    std::vector<uint32> flyers = Known(bot);
    lines.push_back(Trinity::StringFormat("  flying mounts in the account collection: {}", flyers.size()));
    lines.push_back(Trinity::StringFormat("  knows skyriding spell 376777: {}, has the steady flight style aura {}: {}",
        bot->HasSpell(376777), SteadyFlightStyleAura, bot->HasAura(SteadyFlightStyleAura)));
    uint32 spell = flyers.empty() ? AnyFlyer() : flyers[bot->GetGUID().GetCounter() % flyers.size()];
    if (!spell)
        return lines;
    if (flyers.empty())
        lines.push_back("  the account holds no flyer: probing a flying mount of the client data (ownership does not change the capability)");
    MountEntry const* mount = sDB2Manager.GetMount(spell);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell, DIFFICULTY_NONE);
    lines.push_back(Trinity::StringFormat("  probing mount spell {} ({}), mount type {}", spell, info && info->SpellName ? info->SpellName->Str[LOCALE_enUS] : "?", mount ? mount->MountTypeID : 0));
    if (!mount)
        return lines;

    auto report = [&](char const* label)
    {
        MountCapabilityEntry const* capability = bot->GetMountCapability(mount->MountTypeID);
        if (!capability)
            lines.push_back(Trinity::StringFormat("  {}: no capability (the bot could not use this mount here)", label));
        else
            lines.push_back(Trinity::StringFormat("  {}: capability {} flags {:#x} flight capability {} (0 = steady flight, else skyriding) riding skill {} condition {}",
                label, capability->ID, capability->Flags, capability->FlightCapabilityID, capability->ReqRidingSkill, capability->PlayerConditionID));
    };

    report("as is");

    // every capability of the mount type, in the order the core tries them, with the first test it fails (the same tests as
    // Unit::GetMountCapability): shows why a flying mount gives only a ground capability to one bot and flight to another
    if (DB2Manager::MountTypeXCapabilitySet const* capabilities = sDB2Manager.GetMountCapabilities(mount->MountTypeID))
    {
        LiquidData liquid;
        ZLiquidStatus liquidStatus = bot->GetMap()->GetLiquidStatus(bot->GetPhaseShift(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), {}, &liquid);
        bool submerged = (liquidStatus & LIQUID_MAP_UNDER_WATER) != 0 || bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        bool inWater = (liquidStatus & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER)) != 0;
        EnumFlag<AreaMountFlags> mountFlags = AreaMountFlags::None;
        bool restricted = bot->HasAuraType(SPELL_AURA_MOUNT_RESTRICTIONS);
        if (restricted)
            for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_MOUNT_RESTRICTIONS))
                mountFlags |= AreaMountFlags(effect->GetMiscValue());
        else if (AreaTableEntry const* areaTable = sAreaTableStore.LookupEntry(bot->GetAreaId()))
            mountFlags = areaTable->GetMountFlags();
        lines.push_back(Trinity::StringFormat("  where it stands: in water {}, submerged {}, mount restriction aura {} -> mount flags {:#x}", inWater, submerged, restricted, uint32(mountFlags.AsUnderlyingType())));

        for (MountTypeXCapabilityEntry const* link : *capabilities)
        {
            MountCapabilityEntry const* c = sMountCapabilityStore.LookupEntry(link->MountCapabilityID);
            if (!c)
                continue;
            std::string verdict = "passes";
            if (riding < c->ReqRidingSkill)
                verdict = Trinity::StringFormat("riding skill {} < {}", riding, c->ReqRidingSkill);
            else if (!(c->Flags & MOUNT_CAPABIILTY_FLAG_IGNORE_RESTRICTIONS) && ((c->Flags & MOUNT_CAPABILITY_FLAG_FLYING && !mountFlags.HasFlag(AreaMountFlags::AllowFlyingMounts))
                || (c->Flags & MOUNT_CAPABILITY_FLAG_GROUND && !mountFlags.HasFlag(AreaMountFlags::AllowGroundMounts))))
                verdict = "the area does not allow this kind of mount";
            else if (!submerged && !inWater && !(c->Flags & MOUNT_CAPABILITY_FLAG_GROUND))
                verdict = "needs the ground flag out of the water";
            else if (!submerged && inWater && !(c->Flags & MOUNT_CAPABILITY_FLAG_FLOAT))
                verdict = "needs the float flag on the water";
            else if (submerged && inWater && !(c->Flags & MOUNT_CAPABILITY_FLAG_UNDERWATER))
                verdict = "the bot is under water: needs the underwater flag (a flying capability has none)";
            else if (submerged && !inWater && !(c->Flags & MOUNT_CAPABILITY_FLAG_FLOAT))
                verdict = "needs the float flag";
            else if (c->ReqMapID != -1 && int32(bot->GetMapId()) != c->ReqMapID)
                verdict = Trinity::StringFormat("wrong map (needs {})", c->ReqMapID);
            else if (c->ReqAreaID && !DB2Manager::IsInArea(bot->GetAreaId(), c->ReqAreaID))
                verdict = Trinity::StringFormat("wrong area (needs {})", c->ReqAreaID);
            else if (c->ReqSpellAuraID && !bot->HasAura(c->ReqSpellAuraID))
                verdict = Trinity::StringFormat("lacks the aura {}", c->ReqSpellAuraID);
            else if (c->ReqSpellKnownID && !bot->HasSpell(c->ReqSpellKnownID))
                verdict = Trinity::StringFormat("lacks the spell {}", c->ReqSpellKnownID);
            else if (!ConditionMgr::IsPlayerMeetingCondition(bot, c->PlayerConditionID))
            {
                PlayerConditionEntry const* condition = sPlayerConditionStore.LookupEntry(c->PlayerConditionID);
                verdict = Trinity::StringFormat("player condition {} fails (its modifier tree {}: {}, aura {} on the bot: {}, content tuning {})", c->PlayerConditionID,
                    condition ? condition->ModifierTreeID : 0, condition && condition->ModifierTreeID ? bot->ModifierTreeSatisfied(condition->ModifierTreeID) : true,
                    condition ? condition->AuraSpellID[0] : 0, condition ? bot->HasAura(condition->AuraSpellID[0]) : false, condition ? condition->ContentTuningID : 0);
            }
            lines.push_back(Trinity::StringFormat("    capability {} (order {}, flags {:#x}, flight {}, skill {}): {}", c->ID, link->OrderIndex, c->Flags, c->FlightCapabilityID, c->ReqRidingSkill, verdict));
        }
    }
    if (steadyTest && !bot->HasAura(SteadyFlightStyleAura))
    {
        bot->AddAura(SteadyFlightStyleAura, bot);
        report("with the steady flight style aura");
        bot->RemoveAurasDueToSpell(SteadyFlightStyleAura);
    }
    return lines;
}

void EnsureSteadyStyle(Player* bot)
{
    if (bot && !bot->HasAura(SteadyFlightStyleAura))
        bot->AddAura(SteadyFlightStyleAura, bot);
}

bool GrantNow(Player* player)
{
    bool changed = false;
    if (player->GetSkillValue(SKILL_RIDING) < 375)
    {
        player->SetSkill(SKILL_RIDING, 0, 375, 375);
        changed = true;
    }

    std::vector<uint32> flyers = Known(player);
    uint32 spell = flyers.empty() ? AnyFlyer() : flyers.front();
    if (flyers.empty() && spell && player->GetSession() && player->GetSession()->GetCollectionMgr())
    {
        player->GetSession()->GetCollectionMgr()->AddMount(spell, MOUNT_STATUS_NONE);
        changed = true;
    }
    EnsureSteadyStyle(player);

    if (changed)
        TC_LOG_INFO("server.playerbots.air", "Playerbots: {} granted master riding ({}) and flying mount {} ({} flyers in the collection now)",
            player->GetName(), player->GetSkillValue(SKILL_RIDING), spell, Known(player).size());
    return changed;
}

MountState EnsureMounted(Player* bot, bool grantMissing)
{
    if (bot->IsMounted() && bot->CanFly())
        return MountState::Ready;
    if (bot->HasUnitState(UNIT_STATE_CASTING))
        return MountState::Casting;
    // outdoors, and out of the water: under water the core only offers capabilities with the underwater flag, never a flying one
    if (!bot->IsOutdoors() || bot->IsInWater() || bot->IsUnderWater())
        return MountState::Cannot;

    std::vector<uint32> flyers = Known(bot);
    if (bot->GetSkillValue(SKILL_RIDING) < MinRidingSkill || flyers.empty())
    {
        if (!grantMissing)
            return MountState::Cannot;
        GrantNow(bot);
        flyers = Known(bot);
        if (bot->GetSkillValue(SKILL_RIDING) < MinRidingSkill || flyers.empty())
            return MountState::Cannot;
    }

    // a spline that is still running cancels the cast (the follow movement, a walk that arrived 10 yards from its end): stop it, cast next tick
    if (IsMovingOnFoot(bot))
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        return MountState::Casting;
    }

    EnsureSteadyStyle(bot);
    if (bot->IsMounted())
        bot->CancelMountAura();
    bot->CastSpell(bot, flyers[bot->GetGUID().GetCounter() % flyers.size()]);
    return MountState::Casting;
}

void GrantAsync(Player* bot)
{
    if (!bot)
        return;

    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [](Player* player) { GrantNow(player); });
}

void ProbeAsync(Player* bot, bool steadyTest)
{
    if (!bot)
        return;

    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [steadyTest](Player* player)
    {
        for (std::string const& line : Describe(player, steadyTest))
            TC_LOG_INFO("server.playerbots", "Playerbots air probe: {}", line);
    });
}
}
}
