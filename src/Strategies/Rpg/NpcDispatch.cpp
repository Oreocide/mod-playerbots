#include "NpcDispatch.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DB2Stores.h"
#include "DBCEnums.h"
#include "GameObject.h"
#include "GridNotifiersImpl.h"
#include "GossipDef.h"
#include "Item.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "SharedDefines.h"
#include "Travel/HubIndex.h"
#include "UnitDefines.h"
#include <algorithm>
#include <vector>

namespace Playerbots
{
namespace
{
bool InCapital(Player const* bot)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    return zone && zone->GetFlags().HasFlag(AreaFlags::LinkedChat);
}

uint8 DurabilityPct(Player* bot)
{
    uint32 cur = 0;
    uint32 max = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        uint32 itemMax = *item->m_itemData->MaxDurability;
        if (!itemMax)
            continue;
        max += itemMax;
        cur += *item->m_itemData->Durability;
    }
    if (!max)
        return 100;
    return uint8(cur * 100 / max);
}

uint8 UsedBagPct(Player* bot)
{
    uint32 free = bot->GetFreeInventorySlotCount();
    uint32 usedGuess = free >= 80 ? 0 : (80 - std::min(free, 80u));
    return uint8(usedGuess * 100 / 80);
}

void OpenGossip(Player* bot, Creature* npc)
{
    uint32 menu = 0;
    if (CreatureTemplate const* temp = npc->GetCreatureTemplate(); temp && !temp->GossipMenuIds.empty())
        menu = temp->GossipMenuIds[0];
    bot->PrepareGossipMenu(npc, menu);
    bot->SendPreparedGossip(npc);
}
}

int32 ScoreNpcTarget(Player* bot, WorldObject* object)
{
    if (!object)
        return 0;

    int32 score = 1;
    if (Creature* creature = object->ToCreature())
    {
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && DurabilityPct(bot) < sPlayerbotsConfig.RpgRepairDurabilityPct)
            score += 100;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_MASK))
            score += UsedBagPct(bot) > sPlayerbotsConfig.RpgSellBagSpacePct ? 60 : 25;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            score += 40;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) && InCapital(bot))
            score += 50;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER) || creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER_CLASS))
            score += 35;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
            score += 20;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_BANKER) || creature->HasNpcFlag(UNIT_NPC_FLAG_MAILBOX))
            score += 15;
    }

    if (object->GetTypeId() == TYPEID_GAMEOBJECT)
        if (GameObject const* go = object->ToGameObject(); go && go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
            score += 15;

    if (InCapital(bot))
        score += int32(sPlayerbotsConfig.RpgCapitalBoost);
    return score;
}

ObjectGuid ChooseNpcTarget(Player* bot)
{
    std::vector<Creature*> nearby;
    bot->GetCreatureListWithOptionsInGrid(nearby, 50.0f, FindCreatureOptions{});

    ObjectGuid best;
    int32 bestScore = 0;
    for (Creature* creature : nearby)
    {
        if (!creature || !creature->IsAlive() || creature->IsHostileTo(bot))
            continue;
        int32 score = ScoreNpcTarget(bot, creature);
        if (score > bestScore)
        {
            bestScore = score;
            best = creature->GetGUID();
        }
    }

    if (!best.IsEmpty())
        return best;

    if (HubSpot const* spot = HubIndex::NearestAny(bot, { HubKind::Repair, HubKind::Innkeeper, HubKind::Vendor,
        HubKind::Auctioneer, HubKind::Banker, HubKind::Mailbox }, 400.0f))
        if (Creature* creature = bot->FindNearestCreature(spot->Entry, 80.0f, true))
            return creature->GetGUID();

    return ObjectGuid::Empty;
}

bool TryInteract(Player* bot, WorldObject* object)
{
    if (!bot || !object)
        return false;

    if (bot->IsMounted())
        bot->CancelMountAura();

    if (GameObject* go = object->ToGameObject())
    {
        if (go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
            bot->SetFacingToObject(go);
        return true;
    }

    Creature* npc = object->ToCreature();
    if (!npc)
        return false;

    bot->SetFacingToObject(npc);
    OpenGossip(bot, npc);

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && DurabilityPct(bot) < sPlayerbotsConfig.RpgRepairDurabilityPct)
        bot->DurabilityRepairAll(true, 1.0f, false);

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
        bot->SetBindPoint(npc->GetGUID());

    return true;
}
}
