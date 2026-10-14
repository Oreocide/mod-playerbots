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

#include "Creature.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"
#include "Travel/HotspotMgr.h"

using namespace Playerbots;

// Invisible marker of a hotspot (creature_template 9100001 and up, data in world.playerbots_hotspot_marker).
// While it is in the world, random bots are sent to its area like to any town hotspot. Place one with
// ".npc add <entry>", it is a normal creature spawn.
struct npc_bot_hotspot : public ScriptedAI
{
    explicit npc_bot_hotspot(Creature* creature) : ScriptedAI(creature), _guid(creature->GetGUID()) { }

    void JustAppeared() override
    {
        me->SetVisible(false);
        sHotspotMgr->RegisterMarker(_guid, me->GetEntry(), me->GetMapId(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
    }

    ~npc_bot_hotspot() override
    {
        // the creature may be half destroyed by now, the guid was kept
        sHotspotMgr->UnregisterMarker(_guid);
    }

    void UpdateAI(uint32 /*diff*/) override { }
    void AttackStart(Unit* /*who*/) override { }
    void MoveInLineOfSight(Unit* /*who*/) override { }

private:
    ObjectGuid _guid;
};

void AddSC_playerbots_hotspot()
{
    RegisterCreatureAI(npc_bot_hotspot);
}
