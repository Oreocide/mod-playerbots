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
#include "ArenaTeam.h"
#include "BattlegroundMgr.h"
#include "BattlegroundPackets.h"
#include "BattlegroundQueue.h"
#include "ByteBuffer.h"
#include "DB2Stores.h"
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
    return player && !player->GetSession()->IsBot();
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

// Any thread
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

        // ---- battlegrounds and arenas
        bool queued = false;
        for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
        {
            BattlegroundQueueTypeId queueId = bot->GetBattlegroundQueueTypeId(slot);
            if (queueId == BATTLEGROUND_QUEUE_NONE)
                continue;

            queued = true;
            if (bot->IsInWorld() && bot->IsInvitedForBattlegroundQueueType(queueId))
            {
                AnswerBattlefieldPort(bot, slot, true);
                _queueSince.erase(guid);
                break;
            }

            uint32 since = _queueSince.try_emplace(guid, now).first->second;
            if (now - since > EmptyQueueGrace && !QueueHasHuman(sBattlegroundMgr->GetBattlegroundQueue(queueId)))
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

            BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueId);
            uint32 counts[PVP_TEAMS_COUNT] = { };
            uint32 groups = 0;
            CountQueued(queue, bracket->GetBracketId(), counts[TEAM_ALLIANCE], counts[TEAM_HORDE], groups);

            uint32 minPerTeam = std::max<uint32>(bgTemplate->GetMinPlayersPerTeam(), 1);
            uint32 added = 0;
            for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            {
                for (BotRef const& ref : bots)
                {
                    if (counts[team] >= minPerTeam || added >= MaxBgBotsPerScan)
                        break;

                    Player* bot = ref.Bot;
                    if (bot->GetTeamId() != team || !IsIdle(bot) || bot->GetLevel() < bgTemplate->GetMinLevel() || bot->GetLevel() > bgTemplate->GetMaxLevel())
                        continue;

                    if (auto backoff = _backoff.find(bot->GetGUID()); backoff != _backoff.end() && backoff->second > now)
                        continue;

                    PVPDifficultyEntry const* botBracket = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), bot->GetLevel());
                    if (!botBracket || botBracket->GetBracketId() != bracket->GetBracketId())
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

            if (added)
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} bots joined battleground queue {} for {}", added, queueId.BattlemasterListId, human->GetName());
        }
    }
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
