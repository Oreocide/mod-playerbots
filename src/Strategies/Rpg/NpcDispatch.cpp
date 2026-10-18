#include "NpcDispatch.h"
#include "AuctionHouse/AuctionHouseMgr.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DB2Stores.h"
#include "Economy/TrainerIndex.h"
#include "ItemPackets.h"
#include "DBCEnums.h"
#include "GameObject.h"
#include "GridNotifiersImpl.h"
#include "GossipDef.h"
#include "Log.h"
#include "Mail.h"
#include "NPCPackets.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "GameTime.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World/WorldActionQueue.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "AuctionHousePackets.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "MailPackets.h"
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

bool HasDeliverableMail(Player const* bot);
bool HasPostableItem(Player const* bot);

// AC ShouldHomeBindValue: the bind point is on another map or more than 1000 yards away
bool NeedsHomeBind(Player const* bot)
{
    WorldLocation const& home = bot->m_homebind;
    if (home.GetMapId() != bot->GetMapId())
        return true;
    return bot->GetExactDist2d(home) > 1000.0f;
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
        // AC adds +40 only while the bot still needs a home. A bound bot gets nothing extra, so an innkeeper
        // who also sells food scores like any other vendor and does not collect the whole city.
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER) && NeedsHomeBind(bot))
            score += 15;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) && InCapital(bot) && sPlayerbotsConfig.AhPost && HasPostableItem(bot))
            score += 50;
        if ((creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER) || creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER_CLASS))
            && sPlayerbotsConfig.RpgTrain && TrainerIndex::HasLearnable(bot, creature->GetTrainerId()))
            score += 35;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
            score += 20;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_BANKER) || creature->HasNpcFlag(UNIT_NPC_FLAG_MAILBOX))
            score += 15;
    }

    if (object->GetTypeId() == TYPEID_GAMEOBJECT)
        if (GameObject const* go = object->ToGameObject(); go && go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
            score += sPlayerbotsConfig.RpgMailbox && HasDeliverableMail(bot) ? 40 : 15;

    if (InCapital(bot))
        score += int32(sPlayerbotsConfig.RpgCapitalBoost);
    return score;
}

ObjectGuid ChooseNpcTarget(Player* bot, std::vector<std::pair<ObjectGuid, uint32>> const* blocked, uint32 now)
{
    auto isBlocked = [&](ObjectGuid guid)
    {
        if (!blocked)
            return false;
        for (auto const& [badGuid, until] : *blocked)
            if (badGuid == guid && until > now)
                return true;
        return false;
    };

    std::vector<Creature*> nearby;
    bot->GetCreatureListWithOptionsInGrid(nearby, 50.0f, FindCreatureOptions{});

    std::vector<ObjectGuid> best;
    int32 bestScore = 0;
    for (Creature* creature : nearby)
    {
        if (!creature || !creature->IsAlive() || creature->IsHostileTo(bot) || isBlocked(creature->GetGUID()))
            continue;
        int32 score = ScoreNpcTarget(bot, creature);
        if (score < bestScore)
            continue;
        if (score > bestScore)
        {
            bestScore = score;
            best.clear();
        }
        best.push_back(creature->GetGUID());
    }

    if (!best.empty())
        return best[urand(0, uint32(best.size() - 1))];

    if (HubSpot const* spot = HubIndex::NearestAny(bot, { HubKind::Repair, HubKind::Innkeeper, HubKind::Vendor,
        HubKind::Auctioneer, HubKind::Banker, HubKind::Mailbox }, 400.0f))
        if (Creature* creature = bot->FindNearestCreature(spot->Entry, 80.0f, true))
            if (!isBlocked(creature->GetGUID()))
                return creature->GetGUID();

    return ObjectGuid::Empty;
}

// The service sequences replay what the client does, through the client's own handlers: they check distance, faction and
// phase, charge the money and apply the reputation discount. All of them are INPLACE opcodes, so they run on the bot's map thread.
void RepairAtNpc(Player* bot, Creature* npc)
{
    WorldPackets::Item::RepairItem repair{ WorldPacket(CMSG_REPAIR_ITEM) };
    repair.NpcGUID = npc->GetGUID();   // no item guid: repair everything
    bot->GetSession()->HandleRepairItemOpcode(repair);
}

bool HasJunk(Player const* bot)
{
    bool found = false;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&found](Item const* item)
    {
        if (item->GetQuality() != ITEM_QUALITY_POOR || item->IsRefundable())
            return ItemSearchCallbackResult::Continue;
        found = true;
        return ItemSearchCallbackResult::Stop;
    });
    return found;
}

// The retail "sell all junk" button: every grey item that can be sold. Nothing to do when the bot carries no junk.
void SellJunkAtNpc(Player* bot, Creature* npc)
{
    if (!HasJunk(bot))
        return;

    WorldPackets::Item::SellAllJunkItems sell{ WorldPacket(CMSG_SELL_ALL_JUNK_ITEMS) };
    sell.VendorGUID = npc->GetGUID();
    bot->GetSession()->HandleSellAllJunkItems(sell);
}

// Learn what the trainer teaches that the bot can use and afford (profession recipes mostly): open the trainer list, which the
// core's buy handler requires, then buy each spell, then close
void TrainAtNpc(Player* bot, Creature* npc)
{
    uint32 trainerId = npc->GetTrainerId();
    if (!trainerId)
        return;

    std::vector<uint32> spells = TrainerIndex::Learnable(bot, trainerId, sPlayerbotsConfig.RpgTrainMaxPerVisit);
    if (spells.empty())
        return;

    WorldPackets::NPC::Hello list{ WorldPacket(CMSG_TRAINER_LIST) };
    list.Unit = npc->GetGUID();
    bot->GetSession()->HandleTrainerListOpcode(list);

    for (uint32 spell : spells)
    {
        WorldPackets::NPC::TrainerBuySpell buy{ WorldPacket(CMSG_TRAINER_BUY_SPELL) };
        buy.TrainerGUID = npc->GetGUID();
        buy.TrainerID = int32(trainerId);
        buy.SpellID = int32(spell);
        bot->GetSession()->HandleTrainerBuySpellOpcode(buy);
    }

    bot->PlayerTalkClass->SendCloseGossip();
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} asked trainer {} for {} spells", bot->GetName(), trainerId, spells.size());
}

// The innkeeper's "make this place your home": the client's binder request, so the core checks distance, faction and map
// and casts the bind spell (Player::SetBindPoint only sends the confirmation window a bot never answers).
// A bot that already lives within BindMinDistance of this inn keeps its bind point.
void BindAtInnkeeper(Player* bot, Creature* innkeeper)
{
    static constexpr float BindMinDistance = 100.0f;
    WorldLocation const& current = bot->m_homebind;
    if (current.GetMapId() == bot->GetMapId() && bot->GetExactDist2d(current) < BindMinDistance)
        return;

    if (!bot->GetNPCIfCanInteractWith(innkeeper->GetGUID(), UNIT_NPC_FLAG_INNKEEPER, UNIT_NPC_FLAG_2_NONE))
    {
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} cannot bind at {}: not in interaction range", bot->GetName(), innkeeper->GetGUID().ToString());
        return;
    }

    WorldPackets::NPC::Hello binder{ WorldPacket(CMSG_BINDER_ACTIVATE) };
    binder.Unit = innkeeper->GetGUID();
    bot->GetSession()->HandleBinderActivateOpcode(binder);
}

bool IsFoodOrDrink(ItemTemplate const& proto, bool& food, bool& drink)
{
    food = false;
    drink = false;
    if (proto.GetClass() != ITEM_CLASS_CONSUMABLE || proto.GetSubClass() != ITEM_SUBCLASS_FOOD_DRINK)
        return false;
    if (proto.IsConjuredConsumable() || proto.GetBuyPrice() == 0 || proto.GetRequiredSkill())
        return false;

    for (ItemEffectEntry const* effect : proto.Effects)
        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE))
        {
            food = food || spell->HasAura(SPELL_AURA_MOD_REGEN);
            drink = drink || spell->HasAura(SPELL_AURA_MOD_POWER_REGEN);
        }
    return food != drink;
}

// Buy the food (and drink, for a mana user) this vendor sells, through the client's buy request. GearFactory still
// conjures a stack at login; this is the visit a player would make once that stack is eaten down.
void BuyFoodAtNpc(Player* bot, Creature* npc)
{
    if (!sPlayerbotsConfig.RpgBuyFood)
        return;

    VendorItemData const* items = npc->GetVendorItems();
    if (!items)
        return;

    bool wantDrink = bot->GetMaxPower(POWER_MANA) > 0;
    bool boughtFood = false;
    bool boughtDrink = false;
    for (uint32 slot = 0; slot < items->GetItemCount() && !(boughtFood && (!wantDrink || boughtDrink)); ++slot)
    {
        VendorItem const* vendorItem = items->GetItem(slot);
        if (!vendorItem || vendorItem->Type != ITEM_VENDOR_TYPE_ITEM || vendorItem->ExtendedCost)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
        if (!proto || proto->GetBaseRequiredLevel() > bot->GetLevel())
            continue;

        bool food = false;
        bool drink = false;
        if (!IsFoodOrDrink(*proto, food, drink))
            continue;
        if (food && boughtFood)
            continue;
        if (drink && (!wantDrink || boughtDrink))
            continue;

        constexpr uint32 Want = 10;
        uint32 have = bot->GetItemCount(vendorItem->item);
        if (have >= Want)
        {
            if (food)
                boughtFood = true;
            if (drink)
                boughtDrink = true;
            continue;
        }

        uint32 count = Want - have;
        if (!bot->HasEnoughMoney(uint64(proto->GetBuyPrice()) * count))
            continue;

        WorldPackets::Item::BuyItem buy{ WorldPacket(CMSG_BUY_ITEM) };
        buy.VendorGUID = npc->GetGUID();
        buy.Item.ItemID = vendorItem->item;
        buy.Muid = slot + 1; // the handler subtracts one; the client sends the displayed slot
        buy.ItemType = ITEM_VENDOR_TYPE_ITEM;
        buy.Quantity = int32(count);
        buy.ContainerGUID = bot->GetGUID();
        buy.Slot = NULL_SLOT;
        bot->GetSession()->HandleBuyItemOpcode(buy);

        if (food)
            boughtFood = true;
        if (drink)
            boughtDrink = true;
    }
}

bool HasDeliverableMail(Player const* bot)
{
    time_t now = GameTime::GetGameTime();
    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED || mail->deliver_time > now || mail->COD)
            continue;
        if (mail->money || !mail->items.empty())
            return true;
    }
    return false;
}

void CollectMail(Player* bot, ObjectGuid mailbox)
{
    WorldPackets::Mail::MailGetList list{ WorldPacket(CMSG_MAIL_GET_LIST) };
    list.Mailbox = mailbox;
    bot->GetSession()->HandleGetMailList(list);

    struct Pending
    {
        uint64 Id = 0;
        uint64 Money = 0;
        std::vector<uint64> Items;
    };
    std::vector<Pending> pending;
    time_t now = GameTime::GetGameTime();
    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED || mail->deliver_time > now || mail->COD)
            continue;
        if (!mail->money && mail->items.empty())
            continue;
        Pending row;
        row.Id = mail->messageID;
        row.Money = mail->money;
        for (MailItemInfo const& info : mail->items)
            row.Items.push_back(info.item_guid);
        pending.push_back(std::move(row));
        if (pending.size() >= 8)
            break;
    }

    uint32 took = 0;
    for (Pending const& row : pending)
    {
        if (row.Money)
        {
            WorldPackets::Mail::MailTakeMoney money{ WorldPacket(CMSG_MAIL_TAKE_MONEY) };
            money.Mailbox = mailbox;
            money.MailID = row.Id;
            money.Money = row.Money;
            bot->GetSession()->HandleMailTakeMoney(money);
            ++took;
        }
        for (uint64 itemGuid : row.Items)
        {
            WorldPackets::Mail::MailTakeItem item{ WorldPacket(CMSG_MAIL_TAKE_ITEM) };
            item.Mailbox = mailbox;
            item.MailID = row.Id;
            item.AttachID = itemGuid;
            bot->GetSession()->HandleMailTakeItem(item);
            ++took;
        }
    }
    if (took)
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} took {} things from the mailbox", bot->GetName(), took);
}

uint64 AuctionPrice(uint64 sellPrice, uint32 quality)
{
    // Vendor sell price times a band. Higher qualities use a wider band so a purple is not listed like a grey.
    // This is not a market price.
    float low = 3.0f;
    float high = 6.0f;
    switch (quality)
    {
        case ITEM_QUALITY_POOR:     low = 2.0f;  high = 4.0f;  break;
        case ITEM_QUALITY_NORMAL:   low = 3.0f;  high = 6.0f;  break;
        case ITEM_QUALITY_UNCOMMON: low = 8.0f;  high = 16.0f; break;
        case ITEM_QUALITY_RARE:     low = 15.0f; high = 30.0f; break;
        case ITEM_QUALITY_EPIC:     low = 25.0f; high = 50.0f; break;
        default:                    low = 40.0f; high = 80.0f; break;
    }
    uint64 price = uint64(double(sellPrice) * frand(low, high));
    if (price < SILVER)
        price = SILVER;
    return (price / SILVER) * SILVER;
}

bool IsPostable(Item const* item, uint32& quality, uint64& unitPrice)
{
    if (!item || item->IsRefundable())
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->GetSellPrice() == 0)
        return false;
    quality = proto->GetQuality();
    if (!item->CanBeTraded() || item->IsNotEmptyBag() || proto->HasFlag(ITEM_FLAG_CONJURED) || *item->m_itemData->Expiration)
        return false;
    unitPrice = AuctionPrice(proto->GetSellPrice(), quality);
    return unitPrice != 0;
}

void PostAtAuctioneer(Player* bot, ObjectGuid auctioneer)
{
    WorldPackets::AuctionHouse::AuctionHelloRequest hello{ WorldPacket(CMSG_AUCTION_HELLO_REQUEST) };
    hello.Guid = auctioneer;
    bot->GetSession()->HandleAuctionHelloOpcode(hello);

    WorldPackets::AuctionHouse::AuctionBrowseQuery browse{ WorldPacket(CMSG_AUCTION_BROWSE_QUERY) };
    browse.Auctioneer = auctioneer;
    browse.MinLevel = 1;
    bot->GetSession()->HandleAuctionBrowseQuery(browse);

    uint32 posts = 0;
    uint32 const cap = sPlayerbotsConfig.AhMaxPostsPerVisit;
    uint32 const runtime = MIN_AUCTION_TIME / MINUTE;

    struct Offer
    {
        ObjectGuid Guid;
        uint32 Count = 0;
        uint64 UnitPrice = 0;
        bool Commodity = false;
    };
    std::vector<Offer> offers;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item const* item)
    {
        if (offers.size() >= cap)
            return ItemSearchCallbackResult::Stop;

        uint32 quality = ITEM_QUALITY_POOR;
        uint64 unitPrice = 0;
        if (!IsPostable(item, quality, unitPrice))
            return ItemSearchCallbackResult::Continue;

        bool commodity = item->GetTemplate()->GetMaxStackSize() > 1;
        if (!commodity && item->GetCount() != 1)
            return ItemSearchCallbackResult::Continue;

        offers.push_back({ item->GetGUID(), item->GetCount(), unitPrice, commodity });
        return ItemSearchCallbackResult::Continue;
    });

    for (Offer const& offer : offers)
    {
        if (offer.Commodity)
        {
            WorldPackets::AuctionHouse::AuctionSellCommodity sell{ WorldPacket(CMSG_AUCTION_SELL_COMMODITY) };
            sell.Auctioneer = auctioneer;
            sell.UnitPrice = offer.UnitPrice;
            sell.RunTime = runtime;
            WorldPackets::AuctionHouse::AuctionItemForSale row;
            row.Guid = offer.Guid;
            row.UseCount = offer.Count;
            sell.Items.push_back(std::move(row));
            bot->GetSession()->HandleAuctionSellCommodity(sell);
        }
        else
        {
            WorldPackets::AuctionHouse::AuctionSellItem sell{ WorldPacket(CMSG_AUCTION_SELL_ITEM) };
            sell.Auctioneer = auctioneer;
            sell.MinBid = offer.UnitPrice;
            sell.BuyoutPrice = offer.UnitPrice;
            sell.RunTime = runtime;
            WorldPackets::AuctionHouse::AuctionItemForSale row;
            row.Guid = offer.Guid;
            row.UseCount = 1;
            sell.Items.push_back(std::move(row));
            bot->GetSession()->HandleAuctionSellItem(sell);
        }
        ++posts;
    }
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} offered {} items at the auction house", bot->GetName(), posts);
}

bool HasPostableItem(Player const* bot)
{
    bool found = false;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&found](Item const* item)
    {
        uint32 quality = ITEM_QUALITY_POOR;
        uint64 price = 0;
        if (!IsPostable(item, quality, price))
            return ItemSearchCallbackResult::Continue;
        found = true;
        return ItemSearchCallbackResult::Stop;
    });
    return found;
}

void EnqueueMailbox(Player* bot, ObjectGuid mailbox)
{
    if (!sPlayerbotsConfig.RpgMailbox)
        return;
    if (!HasDeliverableMail(bot) && urand(0, 99) >= 5)
        return;

    ObjectGuid who = bot->GetGUID();
    uint32 mapId = bot->GetMapId();
    sWorldActionQueue->Enqueue(who, mapId, mailbox, [mailbox](Player* player) { CollectMail(player, mailbox); });
}

void EnqueueAuction(Player* bot, ObjectGuid auctioneer)
{
    if (!sPlayerbotsConfig.AhPost || !HasPostableItem(bot))
        return;
    if (!sWorldActionQueue->TryVisit(bot->GetGUID(), sPlayerbotsConfig.AhVisitCooldownMs))
        return;

    ObjectGuid who = bot->GetGUID();
    uint32 mapId = bot->GetMapId();
    sWorldActionQueue->Enqueue(who, mapId, auctioneer, [auctioneer](Player* player) { PostAtAuctioneer(player, auctioneer); });
}

ObjectGuid FindNearestNpcWithFlag(Player* bot, uint32 npcFlag, float range)
{
    std::vector<Creature*> nearby;
    bot->GetCreatureListWithOptionsInGrid(nearby, range, FindCreatureOptions{});

    Creature* best = nullptr;
    float bestDist = range;
    for (Creature* creature : nearby)
    {
        if (!creature || !creature->IsAlive() || creature->IsHostileTo(bot) || !creature->HasNpcFlag(NPCFlags(npcFlag)))
            continue;
        float dist = bot->GetExactDist2d(creature);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = creature;
        }
    }
    return best ? best->GetGUID() : ObjectGuid::Empty;
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
        {
            bot->SetFacingToObject(go);
            EnqueueMailbox(bot, go->GetGUID());
        }
        return true;
    }

    Creature* npc = object->ToCreature();
    if (!npc)
        return false;

    bot->SetFacingToObject(npc);
    OpenGossip(bot, npc);

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && DurabilityPct(bot) < sPlayerbotsConfig.RpgRepairDurabilityPct)
        RepairAtNpc(bot, npc);

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && sPlayerbotsConfig.RpgSellJunk)
        SellJunkAtNpc(bot, npc);

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_MASK) && sPlayerbotsConfig.RpgBuyFood)
        BuyFoodAtNpc(bot, npc);

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_TRAINER) && sPlayerbotsConfig.RpgTrain)
        TrainAtNpc(bot, npc);

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
        EnqueueAuction(bot, npc->GetGUID());

    if (npc->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
        BindAtInnkeeper(bot, npc);

    return true;
}
}
