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

// Mail orders, ported from AzerothCore's MailAction ("mail": what is in the mailbox, "mail take") and SendMailAction
// ("sendmail <items> [gold] [to <name>]", to the master by default). Both need a mailbox next to the bot, like a player's
// client; the mail handlers are THREADUNSAFE and run on the world queue.

#include "Cmd/CommandText.h"
#include "Engine/AiRegistry.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Item.h"
#include "Mail.h"
#include "MailPackets.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Social/SocialShared.h"
#include "Social/SocialText.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"

namespace Playerbots::Social
{
namespace
{
constexpr float MailboxReach = 10.0f;

GameObject* NearestMailbox(Player* bot)
{
    GameObject* best = nullptr;
    auto check = [bot](GameObject* go)
    {
        return go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX && go->isSpawned() && bot->IsWithinDistInMap(go, MailboxReach);
    };
    std::vector<GameObject*> found;
    Trinity::GameObjectListSearcher searcher(bot, found, check);
    Cell::VisitGridObjects(bot, searcher, MailboxReach);
    for (GameObject* go : found)
        if (!best || bot->GetExactDistSq(go) < bot->GetExactDistSq(best))
            best = go;
    return best;
}

bool Deliverable(Mail const* mail, time_t now)
{
    return mail && mail->state != MAIL_STATE_DELETED && mail->deliver_time <= now;
}

void TakeAll(Player* bot, ObjectGuid mailbox)
{
    WorldSession* session = bot->GetSession();
    WorldPackets::Mail::MailGetList list{ WorldPacket(CMSG_MAIL_GET_LIST) };
    list.Mailbox = mailbox;
    session->HandleGetMailList(list);

    struct Row
    {
        uint64 Id;
        uint64 Money;
        std::vector<uint64> Items;
    };
    std::vector<Row> rows;
    time_t now = GameTime::GetGameTime();
    for (Mail* mail : bot->GetMails())
    {
        if (!Deliverable(mail, now) || mail->COD)
            continue;
        Row row{ mail->messageID, mail->money, {} };
        for (MailItemInfo const& info : mail->items)
            row.Items.push_back(info.item_guid);
        if (row.Money || !row.Items.empty())
            rows.push_back(std::move(row));
    }

    for (Row const& row : rows)
    {
        if (row.Money)
        {
            WorldPackets::Mail::MailTakeMoney money{ WorldPacket(CMSG_MAIL_TAKE_MONEY) };
            money.Mailbox = mailbox;
            money.MailID = row.Id;
            money.Money = row.Money;
            session->HandleMailTakeMoney(money);
        }
        for (uint64 itemGuid : row.Items)
        {
            WorldPackets::Mail::MailTakeItem item{ WorldPacket(CMSG_MAIL_TAKE_ITEM) };
            item.Mailbox = mailbox;
            item.MailID = row.Id;
            item.AttachID = itemGuid;
            session->HandleMailTakeItem(item);
        }
    }
}

bool MailOrder(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    std::string word = ToLower(Trim(param));

    if (word == "take" || word == "get" || word == "all")
    {
        GameObject* mailbox = NearestMailbox(bot);
        if (!mailbox)
        {
            ai.TellMaster("There is no mailbox next to me.");
            return true;
        }
        ObjectGuid mailboxGuid = mailbox->GetGUID();
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), mailboxGuid, [mailboxGuid](Player* player)
        {
            TakeAll(player, mailboxGuid);
        });
        ai.TellMaster("Taking everything from the mailbox.");
        return true;
    }

    time_t now = GameTime::GetGameTime();
    uint32 count = 0;
    uint64 money = 0;
    uint32 items = 0;
    std::string lines;
    for (Mail* mail : bot->GetMails())
    {
        if (!Deliverable(mail, now))
            continue;
        ++count;
        money += mail->money;
        items += uint32(mail->items.size());
        if (count <= 5)
            lines += (lines.empty() ? "" : "; ") + (mail->subject.empty() ? std::string("(no subject)") : mail->subject);
    }
    if (!count)
        ai.TellMaster("I have no mail.");
    else
        ai.TellMaster(std::to_string(count) + " mails, " + std::to_string(items) + " items, " + SocialText::FormatMoney(money) + ": " + lines
            + ". \"mail take\" at a mailbox takes them.");
    return true;
}

bool SendMailOrder(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    ItemSelection selection = ParseItemSelection(param);
    uint64 money = 0;
    for (std::string const& word : selection.Words)
        money += SocialText::ParseMoneyWord(word);

    std::string target = SocialText::ReceiverAfterTo(selection.Words);
    if (target.empty())
    {
        Player* master = ai.GetMaster();
        if (!master)
            return false;
        target = master->GetName();
    }

    if (selection.Items.empty() && !money)
    {
        ai.TellMaster("sendmail <item links> [5g] [to <name>]: I mail them (to you unless a name follows \"to\"), at a mailbox.");
        return true;
    }

    GameObject* mailbox = NearestMailbox(bot);
    if (!mailbox)
    {
        ai.TellMaster("There is no mailbox next to me.");
        return true;
    }

    std::vector<ObjectGuid> items;
    for (uint32 itemId : selection.Items)
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            if (item->GetEntry() == itemId && item->CanBeTraded(true) && std::ranges::find(items, item->GetGUID()) == items.end())
            {
                items.push_back(item->GetGUID());
                return ItemSearchCallbackResult::Stop;
            }
            return ItemSearchCallbackResult::Continue;
        });
    if (items.size() > MAX_CLIENT_MAIL_ITEMS)
        items.resize(MAX_CLIENT_MAIL_ITEMS);
    money = std::min<uint64>(money, bot->GetMoney());
    if (items.empty() && !money)
    {
        ai.TellMaster("I have none of them to mail.");
        return true;
    }

    ObjectGuid mailboxGuid = mailbox->GetGUID();
    std::string subject = items.empty() ? "Gold" : "Items";
    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), mailboxGuid, [mailboxGuid, target, subject, items, money](Player* player)
    {
        WorldPackets::Mail::SendMail mail{ WorldPacket(CMSG_SEND_MAIL) };
        mail.Info.Mailbox = mailboxGuid;
        mail.Info.Target = target;
        mail.Info.Subject = subject;
        mail.Info.Body = "";
        mail.Info.SendMoney = int64(money);
        uint8 position = 0;
        for (ObjectGuid const& guid : items)
            mail.Info.Attachments.push_back({ position++, guid });
        player->GetSession()->HandleSendMail(mail);
    });
    ai.TellMaster("Mailing it to " + target + ".");
    return true;
}
}

void RegisterMailOrders(AiRegistry& /*registry*/)
{
    AddSocialCommand("mail", "mail [take]: what is in my mailbox, or take it all at a mailbox.", MailOrder, true);
    AddSocialCommand("sendmail", "sendmail <item links> [5g] [to <name>]: I mail them to you (or the name), at a mailbox.", SendMailOrder, true);
}
}
