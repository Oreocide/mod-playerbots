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

#include "BotLootWatch.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Loot.h"
#include "LootPackets.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace Playerbots::Prof::LootWatch
{
namespace
{
struct ExpectWatch
{
    ObjectGuid Bot;
    ObjectGuid Owner;
    uint32 Deadline = 0;
};

struct BobberWatch
{
    uint32 MapId = 0;
    std::vector<uint32> Spells;
    uint32 Started = 0;
    uint32 Deadline = 0;
    uint32 ReadyAt = 0;     // the splash was seen: the bot clicks the bobber at this time
};

std::mutex Lock;
std::vector<ExpectWatch> Expects;
std::unordered_map<ObjectGuid, BobberWatch> Bobbers;

// a reaction a player has to the splash: never instant, rarely slow (the bobber stays ready for FISHING_BOBBER_READY_TIME = 5 s)
constexpr uint32 ReactMinMs = 400;
constexpr uint32 ReactMaxMs = 1500;
// a cast that never produced a bobber (refused: no water in front, interrupted) is over once the channel is gone
constexpr uint32 NoBobberGraceMs = 3000;

void TakeFishingLoot(Player* bot)
{
    // the bobber's own loot, or a fishing pool's (GameObject::Use hands the click to the pool when the bobber lands in one)
    std::vector<ObjectGuid> owners;
    for (auto const& [lootGuid, loot] : bot->GetAELootView())
        if (loot && loot->GetOwnerGUID().IsGameObject())
            owners.push_back(loot->GetOwnerGUID());
    for (ObjectGuid const& owner : owners)
        CollectLoot(bot, owner);
}
}

bool CollectLoot(Player* bot, ObjectGuid owner)
{
    Loot* loot = bot->GetLootByWorldObjectGUID(owner);
    if (!loot)
        return false;

    WorldSession* session = bot->GetSession();
    // items first: the money request releases an item's loot that is empty (HandleLootMoneyOpcode), which destroys the item
    for (uint32 slot = 0; slot < loot->items.size() && slot < 255; ++slot)
        if (!loot->items[slot].is_looted)
            bot->StoreLootItem(owner, uint8(slot), loot);

    loot = bot->GetLootByWorldObjectGUID(owner);
    if (loot && loot->gold)
    {
        WorldPackets::Loot::LootMoney money{ WorldPacket(CMSG_LOOT_MONEY) };
        session->HandleLootMoneyOpcode(money);
    }

    if (bot->GetLootByWorldObjectGUID(owner))
    {
        WorldPackets::Loot::LootRelease release{ WorldPacket(CMSG_LOOT_RELEASE) };
        release.Unit = owner;
        session->HandleLootReleaseOpcode(release);
    }
    return true;
}

void Expect(Player* bot, ObjectGuid owner, uint32 timeoutMs)
{
    std::lock_guard guard(Lock);
    std::erase_if(Expects, [&](ExpectWatch const& watch) { return watch.Bot == bot->GetGUID() && watch.Owner == owner; });
    Expects.push_back({ bot->GetGUID(), owner, GameTime::GetGameTimeMS() + timeoutMs });
}

void WatchBobber(Player* bot, std::vector<uint32> bobberSpells, uint32 timeoutMs)
{
    uint32 now = GameTime::GetGameTimeMS();
    BobberWatch watch;
    watch.MapId = bot->GetMapId();
    watch.Spells = std::move(bobberSpells);
    watch.Started = now;
    watch.Deadline = now + timeoutMs;

    std::lock_guard guard(Lock);
    Bobbers[bot->GetGUID()] = std::move(watch);
}

bool IsFishing(ObjectGuid bot)
{
    std::lock_guard guard(Lock);
    return Bobbers.contains(bot);
}

uint32 Count()
{
    std::lock_guard guard(Lock);
    return uint32(Expects.size() + Bobbers.size());
}

void Update()
{
    std::vector<ExpectWatch> expects;
    std::vector<std::pair<ObjectGuid, BobberWatch>> bobbers;
    {
        std::lock_guard guard(Lock);
        if (Expects.empty() && Bobbers.empty())
            return;
        expects = Expects;
        bobbers.assign(Bobbers.begin(), Bobbers.end());
    }

    uint32 now = GameTime::GetGameTimeMS();
    std::vector<ExpectWatch> doneExpects;
    for (ExpectWatch const& watch : expects)
    {
        Player* bot = ObjectAccessor::FindPlayer(watch.Bot);
        if (!bot || !bot->IsInWorld() || !bot->GetSession())
        {
            doneExpects.push_back(watch);
            continue;
        }
        if (CollectLoot(bot, watch.Owner) || int32(now - watch.Deadline) >= 0)
            doneExpects.push_back(watch);
    }

    std::vector<ObjectGuid> doneBobbers;
    std::vector<std::pair<ObjectGuid, uint32>> readyTimes;
    for (auto& [guid, watch] : bobbers)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetMapId() != watch.MapId || !bot->IsAlive())
        {
            doneBobbers.push_back(guid);
            continue;
        }

        GameObject* bobber = nullptr;
        for (uint32 spell : watch.Spells)
            if ((bobber = bot->GetGameObject(spell)))
                break;

        if (!bobber)
        {
            bool channeling = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr;
            if ((!channeling && now - watch.Started > NoBobberGraceMs) || int32(now - watch.Deadline) >= 0)
                doneBobbers.push_back(guid);
            continue;
        }

        if (bobber->getLootState() != GO_READY)
        {
            if (int32(now - watch.Deadline) >= 0)
                doneBobbers.push_back(guid);
            continue;
        }

        if (!watch.ReadyAt)
        {
            readyTimes.emplace_back(guid, now + urand(ReactMinMs, ReactMaxMs));
            continue;
        }
        if (int32(now - watch.ReadyAt) < 0)
            continue;

        bobber->Use(bot);
        TakeFishingLoot(bot);
        doneBobbers.push_back(guid);
    }

    std::lock_guard guard(Lock);
    for (ExpectWatch const& done : doneExpects)
        std::erase_if(Expects, [&](ExpectWatch const& watch) { return watch.Bot == done.Bot && watch.Owner == done.Owner; });
    for (auto const& [guid, at] : readyTimes)
        if (auto itr = Bobbers.find(guid); itr != Bobbers.end())
            itr->second.ReadyAt = at;
    for (ObjectGuid const& guid : doneBobbers)
        Bobbers.erase(guid);
}
}
