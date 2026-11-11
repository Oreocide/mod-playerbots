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

#include "ActivityMgr.h"
#include "Social/LootRolls.h"
#include "Battleground/BgConfig.h"
#include "BotSession.h"
#include "ArenaTeam.h"
#include "BattlegroundMgr.h"
#include "BattlegroundPackets.h"
#include "BattlegroundQueue.h"
#include "ByteBuffer.h"
#include "DB2Stores.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "LFGPackets.h"
#include "LFGPacketsCommon.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "RandomBotMgr.h"
#include "Random.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace Playerbots
{
namespace
{
constexpr uint32 ScanInterval = 5000;
constexpr uint32 ProposalAnswerMin = 2000;   // bots take a moment to accept, like a person would
constexpr uint32 ProposalAnswerMax = 6000;
constexpr uint32 MaxLfgFillAge = 15 * 60 * 1000;
constexpr uint32 FailedJoinBackoff = 2 * 60 * 1000;
constexpr uint32 LfgLevelSpread = 4;
constexpr uint32 ArenaLevelSpread = 5;
constexpr uint32 EmptyQueueGrace = 60 * 1000;    // bots leave a queue that has no player in it after this long
constexpr uint32 DungeonTeleportRetry = 30 * 1000;
constexpr uint32 MaxBgBotsPerScan = 6;
constexpr uint32 RunningFillAhead = 2;          // bots per team waiting in the queue for a running match once its gates are open
constexpr uint32 TripResendMs = 10 * 1000;      // an accepted battleground port that did not land is sent again this often
constexpr uint8 TripResendMax = 5;              // then the bot gives up the seat
constexpr uint32 MaxLfgBotsPerScan = 2;

uint8 LfgRolesOf(PlayerbotAI const& ai)
{
    switch (ai.GetRole())
    {
        case BotRole::Tank: return lfg::PLAYER_ROLE_TANK;
        case BotRole::Healer: return lfg::PLAYER_ROLE_HEALER;
        default: return lfg::PLAYER_ROLE_DAMAGE;
    }
}

bool IsHuman(Player const* player)
{
    return player && !IsBot(player);
}

bool HasHuman(Group const* group)
{
    for (GroupReference const& ref : group->GetMembers())
        if (IsHuman(ref.GetSource()))
            return true;

    return false;
}

// Not busy with anything: can be sent into group content
bool IsIdle(Player* bot)
{
    return bot->IsAlive() && bot->IsInWorld() && !bot->IsInCombat() && !bot->IsBeingTeleported() && !bot->GetGroup()
        && !bot->InBattleground() && !bot->InBattlegroundQueue() && !bot->isUsingLfg() && !bot->IsDeserter()
        && !bot->GetMap()->IsDungeon() && sLFGMgr->GetState(bot->GetGUID()) == lfg::LFG_STATE_NONE;
}

bool QueueHasHuman(BattlegroundQueue& queue)
{
    for (auto const& [guid, info] : queue.m_QueuedPlayers)
        if (IsHuman(ObjectAccessor::FindConnectedPlayer(guid)))
            return true;

    return false;
}

uint32 InviteInstance(Player* bot, BattlegroundQueueTypeId queueId)
{
    BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueId);
    auto itr = queue.m_QueuedPlayers.find(bot->GetGUID());
    if (itr == queue.m_QueuedPlayers.end() || !itr->second.GroupInfo)
        return 0;
    return itr->second.GroupInfo->IsInvitedToBGInstanceGUID;
}

// A real player is inside this battleground, or still on the accept dialog for it (or it is the .bot bg test match)
bool HumanPresent(uint32 instanceId, BattlegroundQueueTypeId queueId)
{
    if (!instanceId)
        return false;

    if (sActivityMgr->IsBgTest(instanceId))
        return true;

    if (Battleground* bg = sBattlegroundMgr->GetBattleground(instanceId, queueId))
        for (auto const& entry : bg->GetPlayers())
            if (IsHuman(ObjectAccessor::FindConnectedPlayer(entry.first)))
                return true;

    std::shared_lock lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        if (player->IsInWorld() && IsHuman(player) && player->IsInvitedForBattlegroundInstance(instanceId))
            return true;

    return false;
}

// Players waiting in the queue that have not been invited yet, per team
void CountQueued(BattlegroundQueue& queue, BattlegroundBracketId bracket, uint32& alliance, uint32& horde, uint32& groups)
{
    alliance = horde = groups = 0;
    for (uint32 type = 0; type < BG_QUEUE_GROUP_TYPES_COUNT; ++type)
        for (GroupQueueInfo const* info : queue.m_QueuedGroups[bracket][type])
        {
            if (info->IsInvitedToBGInstanceGUID)
                continue;

            ++groups;
            (type == BG_QUEUE_PREMADE_ALLIANCE || type == BG_QUEUE_NORMAL_ALLIANCE ? alliance : horde) += uint32(info->Players.size());
        }
}

void AnswerBattlefieldPort(Player* bot, uint32 queueSlot, bool accept)
{
    WorldPackets::Battleground::BattlefieldPort port{ WorldPacket(CMSG_BATTLEFIELD_PORT) };
    port.Ticket.RequesterGuid = bot->GetGUID();
    port.Ticket.Id = queueSlot;
    port.AcceptedInvite = accept;
    bot->GetSession()->HandleBattleFieldPortOpcode(port);
}

void JoinBattlegroundQueue(Player* bot, BattlegroundQueueTypeId queueId)
{
    WorldPackets::Battleground::BattlemasterJoin join{ WorldPacket(CMSG_BATTLEMASTER_JOIN) };
    join.QueueIDs.push_back(queueId.GetPacked());
    bot->GetSession()->HandleBattlemasterJoinOpcode(join);
}

// ---- Playerbots.Bg.FillToMax (docs/design/BG_PLAN.md phase 2)

// A match the queue still sends players into: a normal, not rated, battleground from its preparation until it ends
bool TakesLateJoiners(Battleground const* bg)
{
    return bg && bg->isBattleground() && !bg->isRated() && bg->GetStatus() > STATUS_WAIT_QUEUE && bg->GetStatus() < STATUS_WAIT_LEAVE;
}

bool HasHumanInside(Battleground const* bg)
{
    if (sActivityMgr->IsBgTest(bg->GetInstanceID()))
        return true;

    for (auto const& entry : bg->GetPlayers())
        if (IsHuman(ObjectAccessor::FindConnectedPlayer(entry.first)))
            return true;

    return false;
}

// Playerbots.Bg.FillMaxPerTeam: the team size the fill stops at, never below what the match needs to start
uint32 FillCap(uint32 maxPerTeam, uint32 minPerTeam)
{
    uint32 cap = std::max<uint32>(maxPerTeam, 1);
    if (sBgConfig.FillMaxPerTeam)
        cap = std::min(cap, std::max(sBgConfig.FillMaxPerTeam, minPerTeam));
    return cap;
}

// The bot waits in this queue for a running match: one of its bracket that has a real player inside and a free slot.
// The human left the queue when they entered (HandleBattleFieldPortOpcode), so QueueHasHuman no longer sees them
bool RunningMatchWaitsFor(Player const* bot, BattlegroundQueueTypeId queueId)
{
    BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(BattlegroundTypeId(queueId.BattlemasterListId));
    if (!bgTemplate || bgTemplate->MapIDs.empty())
        return false;

    PVPDifficultyEntry const* bracket = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), bot->GetLevel());
    if (!bracket)
        return false;

    for (Battleground const* bg : sBattlegroundMgr->GetBGFreeSlotQueueStore(queueId))
        if (TakesLateJoiners(bg) && bg->GetBracketId() == bracket->GetBracketId() && HasHumanInside(bg))
            return true;

    return false;
}
}

ActivityMgr* ActivityMgr::instance()
{
    static ActivityMgr instance;
    return &instance;
}

void ActivityMgr::Shutdown()
{
    std::lock_guard lock(_proposalMutex);
    _proposals.clear();
    _lfgFills.clear();
    _lfgFailed.clear();
    _arenaGroups.clear();
    _queueSince.clear();
    _backoff.clear();
}

// Any thread (the packet sink of a bot session runs on whichever thread sends the packet)
void ActivityMgr::OnBotPacket(ObjectGuid bot, WorldPacket const& packet)
{
    if (packet.GetOpcode() != SMSG_LFG_PROPOSAL_UPDATE)
        return;

    try
    {
        WorldPacket data(packet);
        data.rpos(0);

        WorldPackets::LFG::RideTicket ticket;
        uint64 instanceId = 0;
        uint32 proposalId = 0;
        uint32 slot = 0;
        int8 state = 0;
        data >> ticket >> instanceId >> proposalId >> slot >> state;

        if (state != lfg::LFG_PROPOSAL_INITIATING)
            return;

        std::lock_guard lock(_proposalMutex);
        if (std::ranges::any_of(_proposals, [&](Proposal const& p) { return p.Bot == bot && p.Id == proposalId; }))
            return;

        _proposals.push_back({ bot, proposalId, _clock.load() + urand(ProposalAnswerMin, ProposalAnswerMax) });
    }
    catch (ByteBufferException const&)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: could not read the dungeon finder proposal sent to {}", bot.ToString());
    }
}

void ActivityMgr::Update(uint32 diff)
{
    LootRolls::Update(diff);
    _clock += diff;
    AnswerProposals();

    _scanTimer += diff;
    if (_scanTimer < ScanInterval)
        return;
    _scanTimer = 0;

    Scan();
}

// World thread, the proposal answer is not given from inside the packet callback because the
// dungeon finder is in the middle of updating its proposals when it sends them
void ActivityMgr::AnswerProposals()
{
    std::vector<Proposal> due;
    {
        std::lock_guard lock(_proposalMutex);
        uint32 now = _clock.load();
        for (auto itr = _proposals.begin(); itr != _proposals.end();)
        {
            if (itr->AnswerAt <= now)
            {
                due.push_back(*itr);
                itr = _proposals.erase(itr);
            }
            else
                ++itr;
        }
    }

    for (Proposal const& proposal : due)
        if (sPlayerbotMgr->IsBotOnline(proposal.Bot) && sLFGMgr->GetState(proposal.Bot) == lfg::LFG_STATE_PROPOSAL)
            sLFGMgr->UpdateProposal(proposal.Id, proposal.Bot, true);
}

void ActivityMgr::Scan()
{
    std::vector<BotRef> bots;
    sPlayerbotMgr->ForEachRandomBot([&bots](Player* bot, PlayerbotAI* ai) { bots.push_back({ bot, ai }); });
    if (bots.empty())
        return;

    UpdateBgTest();
    ServeBots(bots);

    std::vector<Player*> humans;
    {
        std::shared_lock lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (player->IsInWorld() && IsHuman(player))
                humans.push_back(player);
    }

    if (sPlayerbotsConfig.RandomLfg)
        FillLfg(humans, bots);
    if (sPlayerbotsConfig.RandomBattlegrounds)
        FillBattlegrounds(humans, bots);
    if (sPlayerbotsConfig.RandomArenas)
        FillArenas(humans, bots);

    CleanArenaGroups();
}

// What the queues ask of the bots that are in them
void ActivityMgr::ServeBots(std::vector<BotRef> const& bots)
{
    uint32 now = _clock.load();

    for (BotRef const& ref : bots)
    {
        Player* bot = ref.Bot;
        ObjectGuid guid = bot->GetGUID();
        Group* group = bot->GetGroup();
        lfg::LfgState state = sLFGMgr->GetState(guid);

        // ---- dungeon finder
        if (state == lfg::LFG_STATE_ROLECHECK && group)
            sLFGMgr->UpdateRoleCheck(group->GetGUID(), guid, LfgRolesOf(*ref.AI));

        if (group && group->isLFGGroup())
        {
            if (!HasHuman(group))
            {
                // The players left, nobody to follow. The dungeon finder teleports the bot out.
                group->RemoveMember(guid);
                continue;
            }

            // Normally the dungeon finder teleports the group in when it forms, this catches the bots that missed it
            if (state == lfg::LFG_STATE_DUNGEON && bot->IsAlive() && !bot->IsBeingTeleported() && !bot->InBattleground())
            {
                uint32 dungeonMap = sLFGMgr->GetDungeonMapId(group->GetGUID());
                if (dungeonMap && bot->GetMapId() != dungeonMap && now - _lastDungeonTeleport[guid] >= DungeonTeleportRetry)
                {
                    _lastDungeonTeleport[guid] = now;
                    sLFGMgr->TeleportPlayer(bot, false);
                }
            }
        }
        else if (!group && bot->IsInWorld() && !bot->IsBeingTeleported() && bot->GetMap()->IsDungeon() && now - _lastDungeonTeleport[guid] >= DungeonTeleportRetry)
        {
            // Left behind in an instance without a party: go back to where it came from, or home
            _lastDungeonTeleport[guid] = now;
            if (!bot->TeleportToBGEntryPoint())
            {
                uint32 mapId = 0;
                Position home;
                if (sRandomBotMgr->GetHomeLocation(guid, mapId, home))
                    bot->TeleportTo(mapId, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), home.GetOrientation());
            }
        }

        // ---- battlegrounds and arenas. A match with no real player is not played.
        Battleground const* botBattleground = bot->GetBattleground();

        // An accepted invite whose port has not landed. The core counts the seat as taken until the bot arrives, and leaving frees
        // only the seats of players who were inside, so a bot on its way never leaves from here: it arrives first (#15)
        if (botBattleground && (bot->GetMapId() != botBattleground->GetMapId() || bot->GetInstanceId() != botBattleground->GetInstanceID()))
        {
            FinishTrip(bot, botBattleground, now);
            continue;
        }
        _trips.erase(guid);

        if (bot->InBattleground() && !HumanPresent(bot->GetBattlegroundId(), botBattleground ? botBattleground->GetQueueID() : BATTLEGROUND_QUEUE_NONE))
        {
            TC_LOG_DEBUG("server.playerbots", "Playerbots: {} leaves the battleground, no player is in it", bot->GetName());
            bot->LeaveBattleground(true, true);
            continue;
        }

        bool queued = false;
        for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
        {
            BattlegroundQueueTypeId queueId = bot->GetBattlegroundQueueTypeId(slot);
            if (queueId == BATTLEGROUND_QUEUE_NONE)
                continue;

            queued = true;
            if (bot->IsInWorld() && bot->IsInvitedForBattlegroundQueueType(queueId))
            {
                bool accept = HumanPresent(InviteInstance(bot, queueId), queueId);
                // the port has to start at once: Player::TeleportTo refuses while another teleport runs, and the bot would then hold
                // a seat it never takes (its own teleport lands elsewhere and the core drops its battleground id there, #15). The
                // invite stays open 90 s, the next scan accepts. A decline is safe at any time: the core frees the seat
                if (accept && bot->IsBeingTeleported())
                {
                    ++_invites.Held;
                    TC_LOG_INFO("server.playerbots", "Playerbots: {} accepts its battleground invite later: it is being teleported (state {})",
                        bot->GetName(), uint32(bot->GetTeleportState()));
                    break;
                }
                // a route still running would take the bot back out of the battleground ("travel" outranks "pvp")
                if (accept && ref.AI)
                    ref.AI->GetRoute().Cancel();
                AnswerBattlefieldPort(bot, slot, accept);
                if (!accept)
                    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} declined a battleground with no player in it", bot->GetName());
                _queueSince.erase(guid);
                break;
            }

            uint32 since = _queueSince.try_emplace(guid, now).first->second;
            if (now - since > EmptyQueueGrace && !QueueHasHuman(sBattlegroundMgr->GetBattlegroundQueue(queueId))
                && !(sBgConfig.FillToMax && RunningMatchWaitsFor(bot, queueId)) && !(_bgTest.Active() && queueId.GetPacked() == _bgTest.Queue))
            {
                AnswerBattlefieldPort(bot, slot, false);
                _queueSince.erase(guid);
                break;
            }
        }

        if (!queued)
            _queueSince.erase(guid);
    }
}

// The bot accepted an invite to bg but stands somewhere else. While its port runs it is left alone; when the port was refused
// (Player::TeleportTo turns any teleport down while another one runs) it is sent again with the core's own battleground port, the
// one the accept handler uses. After TripResendMax tries, or once the match ends, it leaves: that clears its battleground id
// (else it would hunt players on a world map as if it were in the match), but the core keeps the seat counted (#15)
bool ActivityMgr::FinishTrip(Player* bot, Battleground const* bg, uint32 now)
{
    if (bot->IsBeingTeleported())
        return true;

    Trip& trip = _trips[bot->GetGUID()];
    if (trip.Sends && now - trip.LastSendMs < TripResendMs)
        return true;

    Team team = bot->GetBGTeam();
    if (trip.Sends >= TripResendMax || bg->GetStatus() == STATUS_WAIT_LEAVE || !bot->IsInWorld() || (team != ALLIANCE && team != HORDE))
    {
        ++_invites.Lost;
        TC_LOG_WARN("server.playerbots", "Playerbots: {} accepted {} (instance {}) but never got there ({} tries, match {}); it gives the seat up, "
            "the battleground keeps it counted as invited", bot->GetName(), bg->GetName(), bg->GetInstanceID(), trip.Sends, uint32(bg->GetStatus()));
        _trips.erase(bot->GetGUID());
        bot->LeaveBattleground(false, true);
        return false;
    }

    ++trip.Sends;
    trip.LastSendMs = now;
    ++_invites.Resent;
    TC_LOG_INFO("server.playerbots", "Playerbots: {} accepted {} (instance {}) but stands on map {}: its port is sent again ({} of {})",
        bot->GetName(), bg->GetName(), bg->GetInstanceID(), bot->GetMapId(), trip.Sends, TripResendMax);
    BattlegroundMgr::SendToBattleground(bot, bg);
    return true;
}

void ActivityMgr::FillLfg(std::vector<Player*> const& humans, std::vector<BotRef> const& bots)
{
    if (!sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER))
        return;

    uint32 now = _clock.load();
    std::set<ObjectGuid> activeKeys;

    for (Player* human : humans)
    {
        Group* group = human->GetGroup();
        if (group && group->GetLeaderGUID() != human->GetGUID())
            continue;

        ObjectGuid key = group ? group->GetGUID() : human->GetGUID();
        lfg::LfgState keyState = sLFGMgr->GetState(key);
        if (keyState == lfg::LFG_STATE_QUEUED || keyState == lfg::LFG_STATE_PROPOSAL || keyState == lfg::LFG_STATE_ROLECHECK)
            activeKeys.insert(key);

        if (keyState != lfg::LFG_STATE_QUEUED)
            continue;

        // roles already covered: the people in the queue entry and the bots we queued for it
        int32 need[3] = { 1, 1, 3 }; // tank, healer, damage
        auto cover = [&need](uint8 roles)
        {
            if ((roles & lfg::PLAYER_ROLE_TANK) && need[0] > 0)
                --need[0];
            else if ((roles & lfg::PLAYER_ROLE_HEALER) && need[1] > 0)
                --need[1];
            else if ((roles & lfg::PLAYER_ROLE_DAMAGE) && need[2] > 0)
                --need[2];
        };

        if (group)
        {
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource())
                    cover(sLFGMgr->GetRoles(member->GetGUID()));
        }
        else
            cover(sLFGMgr->GetRoles(human->GetGUID()));

        std::vector<LfgFill>& fills = _lfgFills[key];
        std::erase_if(fills, [&](LfgFill const& fill)
        {
            lfg::LfgState botState = sLFGMgr->GetState(fill.Bot);
            if (!sPlayerbotMgr->IsBotOnline(fill.Bot) || (botState != lfg::LFG_STATE_QUEUED && botState != lfg::LFG_STATE_PROPOSAL && botState != lfg::LFG_STATE_ROLECHECK))
                return true;

            if (now - fill.Since > MaxLfgFillAge)
            {
                sLFGMgr->LeaveLfg(fill.Bot);
                return true;
            }

            cover(sLFGMgr->GetRoles(fill.Bot));
            return false;
        });

        lfg::LfgDungeonSet dungeons = sLFGMgr->GetSelectedDungeons(key);
        if (dungeons.empty())
            continue;

        uint32 added = 0;
        for (int32 roleIndex = 0; roleIndex < 3 && added < MaxLfgBotsPerScan; ++roleIndex)
        {
            static constexpr BotRole wanted[3][2] = { { BotRole::Tank, BotRole::Tank }, { BotRole::Healer, BotRole::Healer }, { BotRole::MeleeDps, BotRole::RangedDps } };
            static constexpr uint8 lfgRole[3] = { lfg::PLAYER_ROLE_TANK, lfg::PLAYER_ROLE_HEALER, lfg::PLAYER_ROLE_DAMAGE };

            while (need[roleIndex] > 0 && added < MaxLfgBotsPerScan)
            {
                std::vector<BotRef> candidates;
                for (BotRef const& ref : bots)
                {
                    Player* bot = ref.Bot;
                    BotRole role = ref.AI->GetRole();
                    if (role != wanted[roleIndex][0] && role != wanted[roleIndex][1])
                        continue;

                    if (bot->GetTeam() != human->GetTeam() || !IsIdle(bot) || bot->GetLevel() + LfgLevelSpread < human->GetLevel() || bot->GetLevel() > human->GetLevel() + LfgLevelSpread)
                        continue;

                    if (auto backoff = _backoff.find(bot->GetGUID()); backoff != _backoff.end() && backoff->second > now)
                        continue;

                    if (_lfgFailed[key].contains(bot->GetGUID()))
                        continue;

                    candidates.push_back(ref);
                }

                if (candidates.empty())
                    break;

                Player* bot = candidates[urand(0, uint32(candidates.size() - 1))].Bot;
                lfg::LfgDungeonSet botDungeons = dungeons;
                sLFGMgr->JoinLfg(bot, lfgRole[roleIndex], botDungeons);

                if (sLFGMgr->GetState(bot->GetGUID()) == lfg::LFG_STATE_QUEUED)
                {
                    fills.push_back({ bot->GetGUID(), now });
                    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} joined the dungeon finder for {}", bot->GetName(), human->GetName());
                }
                else
                {
                    // locked out of the dungeon, level, faction: do not try this one again for this queue
                    _lfgFailed[key].insert(bot->GetGUID());
                    _backoff[bot->GetGUID()] = now + FailedJoinBackoff;
                    continue;
                }

                --need[roleIndex];
                ++added;
            }
        }
    }

    // Queues that ended (dungeon found, player gave up): the bots still waiting leave
    for (auto itr = _lfgFills.begin(); itr != _lfgFills.end();)
    {
        if (activeKeys.contains(itr->first))
        {
            ++itr;
            continue;
        }

        for (LfgFill const& fill : itr->second)
            if (sPlayerbotMgr->IsBotOnline(fill.Bot) && sLFGMgr->GetState(fill.Bot) == lfg::LFG_STATE_QUEUED)
                sLFGMgr->LeaveLfg(fill.Bot);

        _lfgFailed.erase(itr->first);
        itr = _lfgFills.erase(itr);
    }
}

void ActivityMgr::FillBattlegrounds(std::vector<Player*> const& humans, std::vector<BotRef> const& bots)
{
    uint32 now = _clock.load();

    // idle bots of a team that could join this bracket now (the joining loops below skip the same ones)
    auto eligible = [&](TeamId team, BattlegroundTemplate const* bgTemplate, BattlegroundBracketId bracketId)
    {
        uint32 n = 0;
        for (BotRef const& ref : bots)
        {
            Player* bot = ref.Bot;
            if (bot->GetTeamId() != team || !IsIdle(bot) || bot->GetLevel() < bgTemplate->GetMinLevel() || bot->GetLevel() > bgTemplate->GetMaxLevel())
                continue;
            if (auto backoff = _backoff.find(bot->GetGUID()); backoff != _backoff.end() && backoff->second > now)
                continue;
            PVPDifficultyEntry const* botBracket = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), bot->GetLevel());
            if (botBracket && botBracket->GetBracketId() == bracketId)
                ++n;
        }
        return n;
    };

    // Playerbots.Bg.FillToMax: a match with a real player inside keeps taking bots until each team is full. The core invites
    // them from the queue into its free slots (BattlegroundQueue::FillPlayersToBG): up to a whole team at once during the
    // preparation, one per team per queue update after the gates open, so only a few bots per team wait in the queue.
    // GetInvitedCount counts the players inside as well as those still on the accept dialog
    if (sBgConfig.FillToMax)
    {
        std::set<uint32> served;
        std::vector<Battleground*> running;
        for (Player* human : humans)
            running.push_back(human->GetBattleground());
        // .bot bg test: the test match is filled as if a player were inside
        if (_bgTest.InstanceId)
            running.push_back(sBattlegroundMgr->GetBattleground(_bgTest.InstanceId, BattlegroundQueueTypeId::FromPacked(_bgTest.Queue)));
        for (Battleground* bg : running)
        {
            if (!TakesLateJoiners(bg) || !served.insert(bg->GetInstanceID()).second)
                continue;

            BattlegroundQueueTypeId queueId = bg->GetQueueID();
            BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(BattlegroundTypeId(queueId.BattlemasterListId));
            if (queueId.TeamSize || !bgTemplate || bgTemplate->MapIDs.empty())
                continue;

            BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueId);
            uint32 queued[PVP_TEAMS_COUNT] = { };
            uint32 groups = 0;
            CountQueued(queue, bg->GetBracketId(), queued[TEAM_ALLIANCE], queued[TEAM_HORDE], groups);

            uint32 cap = FillCap(bg->GetMaxPlayersPerTeam(), bg->GetMinPlayersPerTeam());
            // Playerbots.Bg.Test.Balance: a test match gets no more bots per team than the smaller team can field (in it, queued,
            // or idle in the bracket), so arm comparisons are not 47 against 28 (2026-11-11 windows)
            if (sBgConfig.TestBalance && bg->GetInstanceID() == _bgTest.InstanceId)
                cap = std::min(cap, std::min(bg->GetInvitedCount(ALLIANCE) + queued[TEAM_ALLIANCE] + eligible(TEAM_ALLIANCE, bgTemplate, bg->GetBracketId()),
                    bg->GetInvitedCount(HORDE) + queued[TEAM_HORDE] + eligible(TEAM_HORDE, bgTemplate, bg->GetBracketId())));
            uint32 ahead = bg->GetStatus() == STATUS_WAIT_JOIN ? MaxBgBotsPerScan / 2 : RunningFillAhead;
            uint32 added = 0;
            for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            {
                uint32 taken = bg->GetInvitedCount(team == TEAM_ALLIANCE ? ALLIANCE : HORDE) + queued[team];
                for (BotRef const& ref : bots)
                {
                    if (taken >= cap || queued[team] >= ahead)
                        break;

                    Player* bot = ref.Bot;
                    if (bot->GetTeamId() != team || !IsIdle(bot) || bot->GetLevel() < bgTemplate->GetMinLevel() || bot->GetLevel() > bgTemplate->GetMaxLevel())
                        continue;

                    if (auto backoff = _backoff.find(bot->GetGUID()); backoff != _backoff.end() && backoff->second > now)
                        continue;

                    PVPDifficultyEntry const* botBracket = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), bot->GetLevel());
                    if (!botBracket || botBracket->GetBracketId() != bg->GetBracketId())
                        continue;

                    JoinBattlegroundQueue(bot, queueId);
                    if (bot->InBattlegroundQueueForBattlegroundQueueType(queueId))
                    {
                        ++queued[team];
                        ++taken;
                        ++added;
                        _queueSince[bot->GetGUID()] = now;
                    }
                    else
                        _backoff[bot->GetGUID()] = now + FailedJoinBackoff;
                }
            }

            // a join schedules a queue update by itself; bots that have waited since an earlier scan need one as well
            if (queued[TEAM_ALLIANCE] || queued[TEAM_HORDE])
                sBattlegroundMgr->ScheduleQueueUpdate(0, queueId, bg->GetBracketId());

            if (added)
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} bots joined battleground queue {} for the running match {}",
                    added, queueId.BattlemasterListId, bg->GetInstanceID());
            if (bg->GetInstanceID() == _bgTest.InstanceId)
                _bgTest.Joined += added;
        }
    }

    // bots of the bracket join the queue until each team has a full team waiting
    auto queueBots = [&](BattlegroundQueueTypeId queueId, BattlegroundTemplate const* bgTemplate, BattlegroundBracketId bracketId)
    {
        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueId);
        uint32 counts[PVP_TEAMS_COUNT] = { };
        uint32 groups = 0;
        CountQueued(queue, bracketId, counts[TEAM_ALLIANCE], counts[TEAM_HORDE], groups);

        uint32 cap = std::max<uint32>(bgTemplate->GetMaxPlayersPerTeam(), 1);
        if (sBgConfig.FillToMax)
            cap = FillCap(bgTemplate->GetMaxPlayersPerTeam(), bgTemplate->GetMinPlayersPerTeam());
        // Playerbots.Bg.Test.Balance: the test queue too (queued plus idle in the bracket, per team)
        if (sBgConfig.TestBalance && _bgTest.Active() && queueId.GetPacked() == _bgTest.Queue)
            cap = std::min(cap, std::min(counts[TEAM_ALLIANCE] + eligible(TEAM_ALLIANCE, bgTemplate, bracketId),
                counts[TEAM_HORDE] + eligible(TEAM_HORDE, bgTemplate, bracketId)));
        uint32 added = 0;
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            for (BotRef const& ref : bots)
            {
                if (counts[team] >= cap || added >= MaxBgBotsPerScan)
                    break;

                Player* bot = ref.Bot;
                if (bot->GetTeamId() != team || !IsIdle(bot) || bot->GetLevel() < bgTemplate->GetMinLevel() || bot->GetLevel() > bgTemplate->GetMaxLevel())
                    continue;

                if (auto backoff = _backoff.find(bot->GetGUID()); backoff != _backoff.end() && backoff->second > now)
                    continue;

                PVPDifficultyEntry const* botBracket = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), bot->GetLevel());
                if (!botBracket || botBracket->GetBracketId() != bracketId)
                    continue;

                JoinBattlegroundQueue(bot, queueId);
                if (bot->InBattlegroundQueueForBattlegroundQueueType(queueId))
                {
                    ++counts[team];
                    ++added;
                    _queueSince[bot->GetGUID()] = now;
                }
                else
                    _backoff[bot->GetGUID()] = now + FailedJoinBackoff;
            }
        }
        return added;
    };

    for (Player* human : humans)
    {
        for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
        {
            BattlegroundQueueTypeId queueId = human->GetBattlegroundQueueTypeId(slot);
            if (queueId == BATTLEGROUND_QUEUE_NONE || queueId.TeamSize || human->IsInvitedForBattlegroundQueueType(queueId) || !BattlegroundMgr::IsValidQueueId(queueId))
                continue;

            BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(BattlegroundTypeId(queueId.BattlemasterListId));
            if (!bgTemplate || bgTemplate->MapIDs.empty())
                continue;

            PVPDifficultyEntry const* bracket = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), human->GetLevel());
            if (!bracket)
                continue;

            if (uint32 added = queueBots(queueId, bgTemplate, bracket->GetBracketId()))
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} bots joined battleground queue {} for {}", added, queueId.BattlemasterListId, human->GetName());
        }
    }

    // .bot bg test: until the test match exists its queue is filled as if a player waited in it
    if (_bgTest.Active() && !_bgTest.InstanceId)
    {
        BattlegroundQueueTypeId queueId = BattlegroundQueueTypeId::FromPacked(_bgTest.Queue);
        BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(BattlegroundTypeId(queueId.BattlemasterListId));
        if (bgTemplate && !bgTemplate->MapIDs.empty())
            _bgTest.Joined += queueBots(queueId, bgTemplate, BattlegroundBracketId(_bgTest.Bracket));
    }
}

bool ActivityMgr::StartBgTest(uint64 packedQueue, uint8 bracket, uint32 minutes)
{
    BattlegroundQueueTypeId queueId = BattlegroundQueueTypeId::FromPacked(packedQueue);
    if (!BattlegroundMgr::IsValidQueueId(queueId) || queueId.TeamSize || queueId.Rated)
        return false;
    _bgTest = BgTest();
    _bgTest.Queue = packedQueue;
    _bgTest.Bracket = bracket;
    _bgTest.StartedMs = GameTime::GetGameTimeMS();
    _bgTest.UntilMs = _bgTest.StartedMs + minutes * MINUTE * IN_MILLISECONDS;
    TC_LOG_INFO("server.playerbots", "Playerbots: battleground test started: queue {} bracket {} for {} minutes", queueId.BattlemasterListId,
        uint32(bracket), minutes);
    return true;
}

void ActivityMgr::StopBgTest(char const* why)
{
    if (!_bgTest.Active())
        return;
    TC_LOG_INFO("server.playerbots", "Playerbots: battleground test stopped ({}): instance {}, {} bots sent, {} s", why, _bgTest.InstanceId,
        _bgTest.Joined, (GameTime::GetGameTimeMS() - _bgTest.StartedMs) / IN_MILLISECONDS);
    _bgTest = BgTest();
}

bool ActivityMgr::IsBgTest(uint32 instanceId) const
{
    if (!_bgTest.Active() || !instanceId)
        return false;
    if (_bgTest.InstanceId)
        return instanceId == _bgTest.InstanceId;
    // the first invites come before the match is pinned: any instance of the test queue and bracket
    Battleground* bg = sBattlegroundMgr->GetBattleground(instanceId, BattlegroundQueueTypeId::FromPacked(_bgTest.Queue));
    return bg && uint8(bg->GetBracketId()) == _bgTest.Bracket;
}

// Pins the test match once the queue made it, and ends the test with the match or at its time
void ActivityMgr::UpdateBgTest()
{
    if (!_bgTest.Active())
        return;
    if (int32(_bgTest.UntilMs - GameTime::GetGameTimeMS()) <= 0)
    {
        StopBgTest("time is up");
        return;
    }
    BattlegroundQueueTypeId queueId = BattlegroundQueueTypeId::FromPacked(_bgTest.Queue);
    if (!_bgTest.InstanceId)
    {
        for (Battleground* bg : sBattlegroundMgr->GetBGFreeSlotQueueStore(queueId))
            if (bg && uint8(bg->GetBracketId()) == _bgTest.Bracket && bg->GetStatus() < STATUS_WAIT_LEAVE)
            {
                _bgTest.InstanceId = bg->GetInstanceID();
                TC_LOG_INFO("server.playerbots", "Playerbots: battleground test match is instance {} ({})", _bgTest.InstanceId, bg->GetName());
                break;
            }
        return;
    }
    Battleground* bg = sBattlegroundMgr->GetBattleground(_bgTest.InstanceId, queueId);
    if (!bg || bg->GetStatus() == STATUS_WAIT_LEAVE)
        StopBgTest("the match ended");
}

// A player group waiting for an arena match gets a group of bots as opponents
void ActivityMgr::FillArenas(std::vector<Player*> const& humans, std::vector<BotRef> const& bots)
{
    uint32 now = _clock.load();

    BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(BATTLEGROUND_AA);
    if (!bgTemplate || bgTemplate->MapIDs.empty())
        return;

    for (Player* human : humans)
    {
        Group* humanGroup = human->GetGroup();
        if (!humanGroup || humanGroup->GetLeaderGUID() != human->GetGUID())
            continue;

        for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
        {
            BattlegroundQueueTypeId queueId = human->GetBattlegroundQueueTypeId(slot);
            if (queueId == BATTLEGROUND_QUEUE_NONE || !queueId.TeamSize || human->IsInvitedForBattlegroundQueueType(queueId) || !BattlegroundMgr::IsValidQueueId(queueId))
                continue;

            uint8 teamSizeIndex = queueId.TeamSize == ARENA_TEAM_2v2 ? 0 : queueId.TeamSize == ARENA_TEAM_3v3 ? 1 : queueId.TeamSize == ARENA_TEAM_5v5 ? 2 : 0xFF;
            if (teamSizeIndex == 0xFF || std::ranges::any_of(_arenaGroups, [&](ArenaGroup const& g) { return g.Human == human->GetGUID(); }))
                continue;

            PVPDifficultyEntry const* bracket = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), human->GetLevel());
            if (!bracket)
                continue;

            BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueId);
            uint32 alliance = 0, horde = 0, groups = 0;
            CountQueued(queue, bracket->GetBracketId(), alliance, horde, groups);
            if (groups >= 2)
                continue; // someone else is already waiting to play them

            // opposite faction first, the matchmaker pairs an alliance with a horde group
            std::vector<Player*> candidates;
            for (int pass = 0; pass < 2 && candidates.size() < queueId.TeamSize; ++pass)
            {
                for (BotRef const& ref : bots)
                {
                    Player* bot = ref.Bot;
                    bool opposite = bot->GetTeam() != human->GetTeam();
                    if ((pass == 0) != opposite || !IsIdle(bot))
                        continue;

                    if (bot->GetLevel() + ArenaLevelSpread < human->GetLevel() || bot->GetLevel() > human->GetLevel() + ArenaLevelSpread)
                        continue;

                    if (auto backoff = _backoff.find(bot->GetGUID()); backoff != _backoff.end() && backoff->second > now)
                        continue;

                    candidates.push_back(bot);
                    if (candidates.size() == queueId.TeamSize)
                        break;
                }

                // a group must be one faction: stay with the faction of the first pick
                if (pass == 0 && candidates.size() < queueId.TeamSize)
                    candidates.clear();
            }

            if (candidates.size() < queueId.TeamSize)
                continue;

            Player* leader = candidates.front();
            Group* group = new Group();
            if (!group->Create(leader))
            {
                delete group;
                continue;
            }
            sGroupMgr->AddGroup(group);
            for (size_t i = 1; i < candidates.size(); ++i)
                group->AddMember(candidates[i]);

            WorldPackets::Battleground::BattlemasterJoinArena join{ WorldPacket(CMSG_BATTLEMASTER_JOIN_ARENA) };
            join.TeamSizeIndex = teamSizeIndex;
            leader->GetSession()->HandleBattlemasterJoinArena(join);

            _arenaGroups.push_back({ group->GetGUID(), human->GetGUID(), now });
            for (Player* bot : candidates)
                _queueSince[bot->GetGUID()] = now;

            TC_LOG_DEBUG("server.playerbots", "Playerbots: {} bots queued as {}v{} arena opponents for {}", candidates.size(), uint32(queueId.TeamSize), uint32(queueId.TeamSize), human->GetName());
        }
    }
}

void ActivityMgr::CleanArenaGroups()
{
    uint32 now = _clock.load();
    std::erase_if(_arenaGroups, [now](ArenaGroup const& entry)
    {
        Group* group = sGroupMgr->GetGroupByGUID(entry.Group);
        if (!group)
            return true;

        Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        bool busy = leader && (leader->InBattlegroundQueue() || leader->InBattleground());
        if (busy || now - entry.Since < 30000)
            return false;

        group->Disband();
        return true;
    });
}
}
