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

#include "Social/LootRolls.h"
#include "GearFactory.h"
#include "ItemPacketsCommon.h"
#include "Loot.h"
#include "LootPackets.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "Social/LootRollRules.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <vector>

namespace Playerbots::LootRolls
{
namespace
{
struct Pending
{
    ObjectGuid Bot;
    ObjectGuid LootObj;
    uint8 Slot = 0;
    uint32 ItemId = 0;
    uint8 ValidRolls = ROLL_ALL_TYPE_NO_DISENCHANT;   // the vote types the core allows this bot (RollMask)
    uint32 DueMs = 0;
};

std::mutex sLock;
std::vector<Pending> sPending;
uint32 sClock = 0;
constexpr size_t MaxPending = 512;

RollVote ToVote(RollChoice choice)
{
    switch (choice)
    {
        case RollChoice::Need:
            return RollVote::Need;
        case RollChoice::Greed:
            return RollVote::Greed;
        default:
            return RollVote::Pass;
    }
}

void Answer(Pending const& pending)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(pending.Bot);
    if (!bot || !bot->IsInWorld())
        return;

    // the roll is over (everybody voted, or the time ran out)
    if (!bot->GetLootRoll(pending.LootObj, pending.Slot))
        return;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(pending.ItemId);
    GearFactory::UpgradeInfo upgrade = sGearFactory->GetUpgradeInfo(bot, proto);

    RollInput input;
    input.Equippable = upgrade.Equippable;
    input.Usable = upgrade.Usable;
    input.UpgradeMargin = upgrade.Margin;
    input.SellsForMoney = !proto || proto->GetSellPrice() > 0;
    RollChoice choice = ChooseRoll(input);

    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} rolls {} on item {} (equippable {}, usable {}, margin {:.1f})",
        bot->GetName(), uint32(choice), pending.ItemId, upgrade.Equippable, upgrade.Usable, upgrade.Margin);

    // a vote the core does not allow is dropped and the roll waits for the full timer on this bot: step down to one that is allowed
    RollVote vote = ToVote(choice);
    if (vote == RollVote::Need && !(pending.ValidRolls & ROLL_FLAG_TYPE_NEED))
        vote = RollVote::Greed;
    if (vote == RollVote::Greed && !(pending.ValidRolls & ROLL_FLAG_TYPE_GREED))
        vote = RollVote::Pass;

    LootRoll* roll = bot->GetLootRoll(pending.LootObj, pending.Slot);
    if (roll && !roll->PlayerVote(bot, vote) && vote != RollVote::Pass)
        roll->PlayerVote(bot, RollVote::Pass);
}
}

void OnBotPacket(ObjectGuid bot, WorldPacket const& packet)
{
    if (!sPlayerbotsConfig.LootRoll || packet.GetOpcode() != SMSG_START_LOOT_ROLL)
        return;

    try
    {
        WorldPacket data(packet);
        data.rpos(0);

        // LootObj, MapID, then LootItemData: 2 + 3 + 1 bits, quantity, loot item type, list id, the item; then the roll time
        ObjectGuid lootObj;
        int32 mapId = 0;
        data >> lootObj >> mapId;
        data.ReadBits(2);
        data.ReadBits(3);
        data.ReadBit();
        data.ResetBitPos();

        uint32 quantity = 0;
        uint8 lootItemType = 0;
        uint8 listId = 0;
        data >> quantity >> lootItemType >> listId;

        WorldPackets::Item::ItemInstance item;
        data >> item;
        uint32 rollTimeMs = 0;
        uint8 validRolls = 0;
        data >> rollTimeMs >> validRolls;

        Pending pending;
        pending.Bot = bot;
        pending.LootObj = lootObj;
        pending.Slot = listId;
        pending.ItemId = item.ItemID;
        pending.ValidRolls = validRolls;

        std::lock_guard lock(sLock);
        if (sPending.size() >= MaxPending
            || std::ranges::any_of(sPending, [&](Pending const& p) { return p.Bot == bot && p.LootObj == lootObj && p.Slot == listId; }))
            return;
        pending.DueMs = sClock + RollAnswerDelayMs(rand_norm(), rollTimeMs);
        sPending.push_back(pending);
    }
    catch (ByteBufferException const&)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: could not read the loot roll sent to {}", bot.ToString());
    }
}

void Update(uint32 diff)
{
    std::vector<Pending> due;
    {
        std::lock_guard lock(sLock);
        sClock += diff;
        if (sPending.empty())
            return;

        for (auto itr = sPending.begin(); itr != sPending.end();)
        {
            if (int32(itr->DueMs - sClock) <= 0)
            {
                due.push_back(*itr);
                itr = sPending.erase(itr);
            }
            else
                ++itr;
        }
    }

    for (Pending const& pending : due)
        Answer(pending);
}
}
