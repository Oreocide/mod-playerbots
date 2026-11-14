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

// Mage refreshments (AzerothCore's "give food" / "give water" for 3.3.5 conjured items; retail has one spell for both).
// Conjure Refreshment (190336, spell_mage_conjure_refreshment) makes Conjured Mana Buns for a mage on its own and puts
// down a Refreshment Table (167145, gameobject 233282) when the mage is in a group. A mage without food conjures some; in
// a group where somebody near has none and no table of the group stands close, it puts a table down; a member without food
// walks to the group's table and takes from it, like players do.

#include "Engine/AiRegistry.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Social/SocialConfig.h"
#include "Social/SocialShared.h"
#include "SpellHistory.h"
#include "Travel/MoveToward.h"
#include "World/WorldActionQueue.h"

namespace Playerbots::Social
{
namespace
{
constexpr uint32 ConjureRefreshment = 190336;
constexpr uint32 RefreshmentTableEntry = 233282;
constexpr float TableReach = 3.0f;

bool HasFood(Player const* player)
{
    bool found = false;
    player->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->GetClass() == ITEM_CLASS_CONSUMABLE && proto->GetSubClass() == ITEM_SUBCLASS_FOOD_DRINK)
        {
            found = true;
            return ItemSearchCallbackResult::Stop;
        }
        return ItemSearchCallbackResult::Continue;
    });
    return found;
}

/// A refreshment table of the bot's group (or its own) close by
GameObject* GroupTable(Player* bot)
{
    Group* group = bot->GetGroup();
    float range = sSocialConfig.MageTableRange;
    auto check = [bot, group, range](GameObject* go)
    {
        if (go->GetEntry() != RefreshmentTableEntry || !go->isSpawned() || !bot->IsWithinDistInMap(go, range))
            return false;
        ObjectGuid owner = go->GetOwnerGUID();
        return owner == bot->GetGUID() || (group && group->IsMember(owner));
    };
    std::vector<GameObject*> found;
    Trinity::GameObjectListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, range);
    GameObject* best = nullptr;
    for (GameObject* go : found)
        if (!best || bot->GetExactDistSq(go) < bot->GetExactDistSq(best))
            best = go;
    return best;
}

/// Someone of the group near the mage has no food (bots and players alike)
bool GroupNeedsFood(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;
    for (GroupReference const& ref : group->GetMembers())
        if (Player* member = ref.GetSource(); member && member != bot && member->IsInMap(bot) && member->IsAlive()
            && bot->IsWithinDistInMap(member, sSocialConfig.MageTableRange) && !HasFood(member))
            return true;
    return false;
}

enum class Plan : uint8
{
    None,
    Conjure,
    UseTable
};

Plan WhatToDo(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (bot->HasSpell(ConjureRefreshment) && !bot->GetSpellHistory()->HasCooldown(ConjureRefreshment))
    {
        if (!bot->GetGroup() ? !HasFood(bot) : (GroupNeedsFood(bot) && !GroupTable(bot)))
            return Plan::Conjure;
    }
    if (bot->GetGroup() && !HasFood(bot) && GroupTable(bot))
        return Plan::UseTable;
    return Plan::None;
}

bool RefreshmentsDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.MageRefreshments)
        return false;
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() || bot->HasUnitState(UNIT_STATE_CASTING)
        || bot->InBattleground() || !ai.IsSimulated())
        return false;
    return WhatToDo(ai) != Plan::None;
}

class RefreshmentsAction final : public Action
{
public:
    char const* GetName() const override { return "social refreshments"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        switch (WhatToDo(ai))
        {
            case Plan::Conjure:
            {
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                SpellCastResult result = bot->CastSpell(bot, ConjureRefreshment, false);
                if (result == SPELL_CAST_OK)
                {
                    ai.RequestDelay(3500);
                    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} conjures refreshments{}", bot->GetName(), bot->GetGroup() ? " (a table)" : "");
                }
                return result == SPELL_CAST_OK;
            }
            case Plan::UseTable:
            {
                GameObject* table = GroupTable(bot);
                if (!table)
                    return false;
                if (!bot->IsWithinDistInMap(table, TableReach))
                    return MoveAlongMesh(bot, table->GetPosition(), TableReach - 0.5f) != MoveResult::NoPath;
                bot->StopMoving();
                bot->SetFacingToObject(table);
                ObjectGuid tableGuid = table->GetGUID();
                sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), tableGuid, [tableGuid](Player* player)
                {
                    if (GameObject* go = player->GetMap()->GetGameObject(tableGuid))
                        go->Use(player);
                });
                ai.RequestDelay(1500);
                return true;
            }
            default:
                return false;
        }
    }
};
}

void RegisterMageRefreshments(AiRegistry& registry)
{
    registry.AddTrigger("social refreshments due", sSocialConfig.MageCheckMs, RefreshmentsDue, IntervalMode::Gate, 1000);
    registry.AddAction<RefreshmentsAction>("social refreshments");
}
}
