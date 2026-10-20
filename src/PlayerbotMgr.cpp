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

#include "PlayerbotMgr.h"
#include "BotSession.h"
#include "Economy/BotProfessions.h"
#include "CharacterCache.h"
#include "GameTime.h"
#include "Map.h"
#include "MapRefManager.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "MovementPackets.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "ActivityMgr.h"
#include "Social/LootRolls.h"
#include "Social/BotSocialMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "RandomBotMgr.h"
#include "GenericStrategies.h"
#include "StringFormat.h"
#include "Random.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <memory>
#include <shared_mutex>

namespace Playerbots
{
PlayerbotMgr* PlayerbotMgr::instance()
{
    static PlayerbotMgr instance;
    return &instance;
}

AddBotResult PlayerbotMgr::AddBot(Player* master, std::string const& characterName)
{
    if (!sPlayerbotsConfig.Enabled)
        return AddBotResult::Disabled;

    std::string name = characterName;
    if (!normalizePlayerName(name))
        return AddBotResult::NotFound;

    CharacterCacheEntry const* character = sCharacterCache->GetCharacterCacheByName(name);
    if (!character || character->IsDeleted)
        return AddBotResult::NotFound;

    if (character->Guid == master->GetGUID())
        return AddBotResult::IsSelf;

    WorldSession* masterSession = master->GetSession();
    if (character->AccountId != masterSession->GetAccountId())
        return AddBotResult::WrongAccount;

    if (_bots.contains(character->Guid) || ObjectAccessor::FindConnectedPlayer(character->Guid))
        return AddBotResult::AlreadyOnline;

    if (GetBotCount(master->GetGUID()) >= sPlayerbotsConfig.MaxBotsPerPlayer)
        return AddBotResult::TooManyBots;

    WorldSession* session = CreateBotSession(masterSession->GetAccountId(), std::string(masterSession->GetAccountName()),
        masterSession->GetBattlenetAccountId(), std::string(), masterSession->GetAccountExpansion(), std::string(masterSession->GetOS()),
        masterSession->GetTimezoneOffset(), masterSession->GetClientBuild(), masterSession->GetClientBuildVariant(),
        masterSession->GetSessionDbLocaleIndex(),
        // a party bot sees loot rolls; dungeon finder proposals are not its business (it never queues alone)
        [guid = character->Guid](WorldPacket const& packet) { LootRolls::OnBotPacket(guid, packet); }, [guid = character->Guid](WorldSession* botSession) { botSession->LoginHeadlessPlayer(guid); });

    ObjectGuid guid = character->Guid;

    BotHolder& holder = _bots[guid];
    holder.Session = session;
    holder.Master = master->GetGUID();
    holder.FormationIndex = GetFreeFormationIndex(master->GetGUID());

    TC_LOG_INFO("server.playerbots", "Playerbots: {} ({}) is logging in {} ({}) as bot", master->GetName(), master->GetGUID().ToString(), name, guid.ToString());
    return AddBotResult::Ok;
}

bool PlayerbotMgr::AddRandomBot(uint32 accountId, std::string const& accountName, uint32 bnetAccountId, ObjectGuid character)
{
    if (!sPlayerbotsConfig.Enabled || _bots.contains(character) || ObjectAccessor::FindConnectedPlayer(character))
        return false;

    WorldSession* session = CreateBotSession(accountId, std::string(accountName), bnetAccountId, std::string(), uint8(sWorld->getIntConfig(CONFIG_EXPANSION)),
        "Win", Minutes(0), 0, ClientBuild::VariantId{}, LOCALE_enUS,
        // what a random bot reacts to: dungeon finder proposals (party and guild invitations are found by BotSocialMgr's scan)
        [character](WorldPacket const& packet)
        {
            sActivityMgr->OnBotPacket(character, packet);
            LootRolls::OnBotPacket(character, packet);
        },
        [character](WorldSession* botSession) { botSession->LoginHeadlessPlayer(character); });

    BotProfessions::SeedOffline(character);

    BotHolder& holder = _bots[character];
    holder.Session = session;
    return true;
}

size_t PlayerbotMgr::GetRandomBotCount() const
{
    return std::ranges::count_if(_bots, [](auto const& pair) { return pair.second.Master.IsEmpty() && !pair.second.Removing; });
}

bool PlayerbotMgr::HasHumanWithin(Player const* bot, float distance) const
{
    std::shared_ptr<std::vector<NearbyHuman> const> humans = _nearbyHumans.load();
    if (!humans)
        return false;

    uint32 mapId = bot->GetMapId();
    float distanceSq = distance * distance;
    for (NearbyHuman const& human : *humans)
    {
        if (human.MapId != mapId)
            continue;

        float dx = bot->GetPositionX() - human.X;
        float dy = bot->GetPositionY() - human.Y;
        if (dx * dx + dy * dy <= distanceSq)
            return true;
    }
    return false;
}

void PlayerbotMgr::ForEachRandomBot(std::function<void(Player*, PlayerbotAI*)> const& fn) const
{
    for (auto const& [guid, holder] : _bots)
    {
        if (!holder.Master.IsEmpty() || holder.Removing || !holder.AI)
            continue;

        Player* bot = holder.Session->GetPlayer();
        if (bot && bot->IsInWorld())
            fn(bot, holder.AI.get());
    }
}

bool PlayerbotMgr::RemoveBot(ObjectGuid bot)
{
    auto itr = _bots.find(bot);
    if (itr == _bots.end())
        return false;

    BotHolder& holder = itr->second;
    if (holder.Removing)
        return true;

    holder.Removing = true;

    if (Player* player = holder.Session->GetPlayer())
        if (Group* group = player->GetGroup())
            group->RemoveMember(player->GetGUID());

    return true;
}

void PlayerbotMgr::RemoveBotsOf(ObjectGuid master)
{
    std::vector<ObjectGuid> bots;
    for (auto const& [guid, holder] : _bots)
        if (holder.Master == master)
            bots.push_back(guid);

    for (ObjectGuid guid : bots)
        RemoveBot(guid);
}

void PlayerbotMgr::LogoutAll()
{
    for (auto& [guid, holder] : _bots)
    {
        holder.AI.reset();
        delete holder.Session; // logs out and saves the character
    }

    _bots.clear();
}

void PlayerbotMgr::NoteWorldTick(uint32 diff)
{
    _worldDiff.store(diff, std::memory_order_relaxed);

    // one bucket per second, the window is the ring of them
    _diffBuckets[_diffBucketIndex] = std::max(_diffBuckets[_diffBucketIndex], diff);
    _diffBucketTimer += diff;
    if (_diffBucketTimer >= 1000)
    {
        _diffBucketTimer = 0;
        _diffBucketIndex = (_diffBucketIndex + 1) % WorldDiffWindowSeconds;
        _diffBuckets[_diffBucketIndex] = 0;
    }
    _worldDiffMax.store(*std::max_element(std::begin(_diffBuckets), std::end(_diffBuckets)), std::memory_order_relaxed);

    _nearbyTimer += diff;
    if (_nearbyHumans.load() && _nearbyTimer < 1000)
        return;
    _nearbyTimer = 0;
    RefreshNearbyHumans();
}

void PlayerbotMgr::RefreshNearbyHumans()
{
    auto next = std::make_shared<std::vector<NearbyHuman>>();
    {
        std::shared_lock lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->IsInWorld() || !player->GetSession() || IsBotSession(player->GetSession()))
                continue;

            NearbyHuman& h = next->emplace_back();
            h.Guid = guid;
            h.MapId = player->GetMapId();
            h.ZoneId = player->GetZoneId();
            h.X = player->GetPositionX();
            h.Y = player->GetPositionY();
            h.GuildId = player->GetGuildId();
        }
    }
    _nearbyHumans.store(std::shared_ptr<std::vector<NearbyHuman> const>(std::move(next)));
}

bool PlayerbotMgr::HasRealPlayerOnline() const
{
    if (auto humans = _nearbyHumans.load())
        return !humans->empty();
    return false;
}

void PlayerbotMgr::Update(uint32 diff)
{
    uint32 const sliceMs = sPlayerbotsConfig.ManagerSliceMs;
    for (auto itr = _bots.begin(); itr != _bots.end();)
    {
        BotHolder& holder = itr->second;
        WorldSession* session = holder.Session;

        if (holder.Removing)
        {
            // deleting the session logs the player out and saves it
            holder.AI.reset();
            delete session;
            itr = _bots.erase(itr);
            continue;
        }

        // idle frozen bots are visited once per ManagerSliceMs with the time that passed (0 = every tick, as before)
        uint32 botDiff = diff;
        if (sliceMs && holder.Cold && !holder.Removing && holder.AI)
        {
            holder.ColdAccum += diff;
            if (holder.ColdAccum < sliceMs)
            {
                ++itr;
                continue;
            }
            botDiff = holder.ColdAccum;
        }
        holder.ColdAccum = 0;

        if (!holder.Removing)
        {
            if (holder.AI && !holder.Master.IsEmpty() && !ObjectAccessor::FindConnectedPlayer(holder.Master))
                RemoveBot(itr->first);
            else if (!holder.AI && (holder.LoginTimer += botDiff) > sPlayerbotsConfig.LoginTimeout)
            {
                TC_LOG_ERROR("server.playerbots", "Playerbots: login of bot {} timed out", itr->first.ToString());
                RemoveBot(itr->first);
            }
        }

        CompleteFarTeleport(session);

        bool runSession = holder.Removing || !holder.AI;
        bool idleTick = false;
        if (!runSession)
        {
            Player* bot = session->GetPlayer();
            bool busy = bot && bot->IsBeingTeleported();
            bool hot = busy || holder.AI->IsSimulated();
            holder.Cold = !hot;
            if (hot)
            {
                holder.IdleSessionAccum = 0;
                runSession = true;
            }
            else
            {
                holder.IdleSessionAccum += botDiff;
                if (holder.IdleSessionAccum >= 5000)
                {
                    holder.IdleSessionAccum = 0;
                    runSession = true;
                    idleTick = true;
                }
            }
        }

        if (!runSession)
        {
            ++itr;
            continue;
        }

        WorldSessionFilter filter(session);
        if (!session->Update(botDiff, filter))
        {
            // the session ended: kicked (a game master's .kick), or removed
            holder.AI.reset();
            delete session; // logs out and saves the character
            itr = _bots.erase(itr);
            continue;
        }

        if (!holder.AI && !holder.Removing)
            if (Player* bot = session->GetPlayer(); bot && bot->IsInWorld())
                OnBotLoggedIn(itr->first, holder);

        ++itr;
    }
}

// A client answers the far teleport handshake (suspend token, then world port ack), bots do it here
void PlayerbotMgr::CompleteFarTeleport(WorldSession* session)
{
    Player* bot = session->GetPlayer();
    if (!bot)
        return;

    if (bot->GetTeleportState() == TeleportState::WaitingForSuspendTokenResponse)
    {
        WorldPackets::Movement::SuspendTokenResponse response{ WorldPacket(CMSG_SUSPEND_TOKEN_RESPONSE) };
        session->HandleSuspendTokenResponse(response);
    }

    if (bot->GetTeleportState() == TeleportState::WaitingForWorldPortAck)
        session->HandleMoveWorldportAck();
}

void PlayerbotMgr::OnBotLoggedIn(ObjectGuid guid, BotHolder& holder)
{
    Player* bot = holder.Session->GetPlayer();
    if (holder.Master.IsEmpty())
    {
        sRandomBotMgr->Prepare(bot);
        holder.AI = std::make_unique<PlayerbotAI>(bot, ObjectGuid::Empty, 0);
        holder.AI->AddRandomBotStrategies();
        if (Optional<Position> home = sRandomBotMgr->GetHome(bot->GetGUID()))
            holder.AI->SetHome(*home);
        // zone channels (General, Trade, LFG...) are joined by the core on a zone change, a bot that logged in without one has none yet
        if (sPlayerbotsConfig.RandomJoinChannels && !bot->IsBeingTeleported() && bot->GetJoinedChannels().empty())
            bot->UpdateLocalChannels(bot->GetZoneId());
        TC_LOG_INFO("server.playerbots", "Playerbots: random bot {} ({}) logged in", bot->GetName(), guid.ToString());
        return;
    }

    Player* master = ObjectAccessor::FindConnectedPlayer(holder.Master);
    if (!master)
    {
        RemoveBot(guid);
        return;
    }

    holder.AI = std::make_unique<PlayerbotAI>(bot, holder.Master, holder.FormationIndex);
    holder.AI->RestoreState();

    Group* group = master->GetGroup();
    if (!group)
    {
        group = new Group();
        if (group->Create(master))
            sGroupMgr->AddGroup(group);
        else
        {
            delete group;
            group = nullptr;
        }
    }

    if (group && !group->IsMember(bot->GetGUID()))
    {
        if (bot->GetGroup())
            bot->RemoveFromGroup();

        if (group->IsFull() && !group->isRaidGroup() && group->IsLeader(master->GetGUID()))
            group->ConvertToRaid();

        if (!group->IsFull() && group->AddMember(bot))
            group->BroadcastGroupUpdate();
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: bot {} ({}) logged in for {}", bot->GetName(), guid.ToString(), master->GetName());
    holder.AI->TellMaster(Trinity::StringFormat("Hello, I'm your {}. Whisper me \"help\" for commands.", GetRoleName(holder.AI->GetRole())));
}

namespace
{
void RunBotAI(Player* bot, PlayerbotAI* ai, uint32 diff)
{
    // a frozen bot was taken out of Unit::Update by the core, its AI only needs a few ticks per second
    if (bot->GetSession()->IsDormant())
        if (uint32 frozenTickMs = sPlayerbotsConfig.FrozenTickMs; frozenTickMs && !ai->FrozenTickDue(diff, frozenTickMs, diff))
            return;

    ai->Update(diff);
}
}

void PlayerbotMgr::TickMapBots(Map* map, uint32 diff)
{
    // The AI may teleport the bot, which unlinks it from the map's player list: collect first
    thread_local std::vector<Player*> bots;
    bots.clear();
    for (MapReference const& ref : map->GetPlayers())
        if (Player* player = ref.GetSource(); player && IsBotSession(player->GetSession()))
            bots.push_back(player);

    for (Player* bot : bots)
        if (bot->IsInWorld() && bot->GetMap() == map)
            if (PlayerbotAI* ai = GetAI(bot))
                RunBotAI(bot, ai, diff);
}

void PlayerbotMgr::TickOtherMapBots(uint32 diff)
{
    for (auto const& [guid, holder] : _bots)
    {
        Player* bot = holder.Session->GetPlayer();
        if (!bot || !holder.AI || holder.Removing || !bot->IsInWorld())
            continue;

        Map* map = bot->GetMap();
        if (!map || map->GetEntry()->IsWorldMap())
            continue;

        RunBotAI(bot, holder.AI.get(), diff);
    }
}

void PlayerbotMgr::LogoutRandomBots()
{
    std::vector<ObjectGuid> bots;
    for (auto const& [guid, holder] : _bots)
        if (holder.Master.IsEmpty())
            bots.push_back(guid);

    for (ObjectGuid guid : bots)
        RemoveBot(guid);
}

PlayerbotAI* PlayerbotMgr::GetAI(Player const* player) const
{
    auto itr = _bots.find(player->GetGUID());
    if (itr == _bots.end() || itr->second.Session != player->GetSession())
        return nullptr;

    return itr->second.AI.get();
}

std::vector<Player*> PlayerbotMgr::GetBotsOf(ObjectGuid master) const
{
    std::vector<Player*> bots;
    for (auto const& [guid, holder] : _bots)
        if (holder.Master == master && holder.AI && !holder.Removing)
            if (Player* bot = holder.Session->GetPlayer())
                bots.push_back(bot);

    return bots;
}

size_t PlayerbotMgr::GetBotCount(ObjectGuid master) const
{
    return std::ranges::count_if(_bots, [master](auto const& pair) { return pair.second.Master == master && !pair.second.Removing; });
}

uint8 PlayerbotMgr::GetFreeFormationIndex(ObjectGuid master) const
{
    for (uint8 index = 0; ; ++index)
        if (std::ranges::none_of(_bots, [&](auto const& pair) { return pair.second.Master == master && pair.second.FormationIndex == index; }))
            return index;
}
}
