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

// Group invites, ported from AzerothCore's InviteNearbyToGroupAction (the "group" strategy, "often") and LfgAction (the "lfg"
// order with GetRoleQuotas). A random bot invites a player of its level that stands near it; when the player joins, the bot
// hands over the lead, so it follows that player like any random bot in a player's party (PlayerbotAI::GetMaster).

#include "Cmd/CommandText.h"
#include "Combat/GroupCombat.h"
#include "Engine/AiRegistry.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PartyPackets.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "Social/BotSocialMgr.h"
#include "Social/SocialConfig.h"
#include "Social/SocialShared.h"
#include "Social/SocialText.h"
#include "World.h"
#include "World/WorldActionQueue.h"
#include "WorldSession.h"
#include "StringFormat.h"
#include "Util.h"
#include <map>
#include <mutex>

namespace Playerbots::Social
{
namespace
{
constexpr uint32 LeadPassWindowMs = 180000;   // the player joined within this long of the bot's invite
constexpr uint32 LfgLevelSpread = 4;          // AC LfgAction: requester and bot within 4 levels
constexpr uint32 LfgOrderRepeatMs = 3000;     // one "lfg" said in party chat reaches every bot: the first one fills

std::mutex g_inviteLock;
std::map<ObjectGuid, uint32> g_humanInvited;                         // human -> last invite by any bot
std::map<std::pair<ObjectGuid, ObjectGuid>, uint32> g_pairInvited;    // (bot, human) -> last invite
std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> g_inviter;        // human -> (bot, when)
std::map<ObjectGuid, uint32> g_lfgOrder;                             // master -> last "lfg" order

bool Within(uint32 since, uint32 now, uint32 window)
{
    return since && window && now - since < window;
}

bool InviteDue(PlayerbotAI& ai)
{
    if (!sSocialConfig.InviteEnable || !IsFreeRandomBot(ai))
        return false;
    Player* bot = ai.GetBot();
    if (!bot->IsAlive() || bot->InBattleground() || bot->InBattlegroundQueue() || bot->GetMap()->Instanceable() || !ai.IsSimulated())
        return false;
    // AC isUseful: no group, or the bot leads one that has room (bots do not convert to a raid here)
    if (Group* group = bot->GetGroup(); group && (group->GetLeaderGUID() != bot->GetGUID() || group->IsFull() || group->isRaidGroup()))
        return false;
    if (urand(1, sSocialConfig.InviteOneIn) != 1)
        return false;
    return sPlayerbotMgr->HasHumanWithin(bot, sSocialConfig.InviteRange);
}

class InviteAction final : public Action
{
public:
    char const* GetName() const override { return "social invite"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        auto humans = sPlayerbotMgr->GetNearbyHumans();
        if (!humans)
            return false;

        uint32 now = GameTime::GetGameTimeMS();
        float range = sSocialConfig.InviteRange;
        Player* pick = nullptr;
        for (PlayerbotMgr::NearbyHuman const& entry : *humans)
        {
            if (entry.MapId != bot->GetMapId() || bot->GetExactDist2dSq(entry.X, entry.Y) > range * range)
                continue;
            Player* human = ObjectAccessor::GetPlayer(*bot, entry.Guid);
            // AC: not grouped, not do-not-disturb, not on a loading screen, within 2 levels; the same team here
            if (!human || !IsHuman(human) || human->GetGroup() || human->isDND() || human->IsBeingTeleported() || human->IsInCombat()
                || human->GetTeam() != bot->GetTeam())
                continue;
            if (std::abs(int32(human->GetLevel()) - int32(bot->GetLevel())) > int32(sSocialConfig.InviteLevelGap))
                continue;
            {
                std::lock_guard lock(g_inviteLock);
                if (Within(g_humanInvited[human->GetGUID()], now, sSocialConfig.InviteHumanCooldownMs)
                    || Within(g_pairInvited[{ bot->GetGUID(), human->GetGUID() }], now, std::max<uint32>(sSocialConfig.InviteHumanCooldownMs, 60000)))
                    continue;
            }
            if (!pick || bot->GetExactDistSq(human) < bot->GetExactDistSq(pick))
                pick = human;
        }
        if (!pick)
            return false;

        ObjectGuid humanGuid = pick->GetGUID();
        std::string name = pick->GetName();
        {
            std::lock_guard lock(g_inviteLock);
            g_humanInvited[humanGuid] = now;
            g_pairInvited[{ bot->GetGUID(), humanGuid }] = now;
            g_inviter[humanGuid] = { bot->GetGUID(), now };
        }

        // AC InviteChat: "Hey <name>, do you want join my group?"
        if (sSocialConfig.InviteChat)
            sBotSocialMgr->QueueWhisper(bot->GetGUID(), humanGuid,
                SocialText::Fill(SocialText::Lines(SocialText::Category::JoinGroup)[0], { { "%player", name } }));

        // party opcodes are world-thread only
        sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [name](Player* player)
        {
            WorldPackets::Party::PartyInviteClient invite{ WorldPacket(CMSG_PARTY_INVITE) };
            invite.TargetName = name;
            player->GetSession()->HandlePartyInviteOpcode(invite);
        });
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} invites {} to a group", bot->GetName(), name);
        return true;
    }
};

struct RoleQuota
{
    uint32 Tank = 1;
    uint32 Healer = 1;
    uint32 Dps = 3;

    uint32& Of(BotRole role) { return role == BotRole::Tank ? Tank : role == BotRole::Healer ? Healer : Dps; }
};

// AC LfgAction::GetRoleQuotas
RoleQuota QuotaFor(uint32 size)
{
    switch (size)
    {
        case 40: return { 4, 16, 20 };
        case 25: return { 3, 7, 15 };
        case 20: return { 2, 5, 13 };
        case 10: return { 2, 3, 5 };
        default: return { 1, 1, 3 };
    }
}

char const* RoleText(BotRole role)
{
    return role == BotRole::Tank ? "tank" : role == BotRole::Healer ? "healer" : "dps";
}

/// World thread: random bots that fit the open roles join the master's group (the group is made first when there is none)
void FillGroup(Player* commander, ObjectGuid masterGuid, uint32 size)
{
    PlayerbotAI* commanderAi = sPlayerbotMgr->GetAI(commander);
    Player* master = ObjectAccessor::FindConnectedPlayer(masterGuid);
    if (!master || !master->IsInWorld() || !commanderAi)
        return;

    Group* group = master->GetGroup();
    if (group && (group->isBGGroup() || group->isBFGroup() || group->isLFGGroup() || group->GetLeaderGUID() != masterGuid))
    {
        commanderAi->TellMaster("Only the leader of a normal group can do that.");
        return;
    }

    RoleQuota open = QuotaFor(size);
    uint32 members = 1;
    if (group)
    {
        members = group->GetMembersCount();
        for (GroupReference const& ref : group->GetMembers())
            if (Player* member = ref.GetSource())
            {
                uint32& left = open.Of(GroupCombat::SpecRole(member));
                if (left)
                    --left;
            }
    }
    else
    {
        uint32& left = open.Of(GroupCombat::SpecRole(master));
        if (left)
            --left;
    }

    struct Candidate
    {
        Player* Bot;
        BotRole Role;
        float Distance;
    };
    std::vector<Candidate> candidates;
    bool masterCapped = master->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI* /*ai*/)
    {
        if (bot->GetGroup() || !bot->IsAlive() || bot->GetTeam() != master->GetTeam() || bot->InBattleground() || bot->InBattlegroundQueue()
            || bot->IsBeingTeleported() || bot->GetMap()->Instanceable() || sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
            return;
        // AC: a capped requester takes only capped bots, else within 4 levels
        if (masterCapped ? bot->GetLevel() < master->GetLevel()
            : std::abs(int32(bot->GetLevel()) - int32(master->GetLevel())) > int32(LfgLevelSpread))
            return;
        float distance = bot->GetMapId() == master->GetMapId() ? bot->GetExactDist(master) : 100000.0f;
        candidates.push_back({ bot, GroupCombat::SpecRole(bot), distance });
    });
    std::ranges::sort(candidates, {}, &Candidate::Distance);

    std::string joined;
    for (Candidate const& candidate : candidates)
    {
        if (members >= size)
            break;
        uint32& left = open.Of(candidate.Role);
        if (!left)
            continue;

        if (!group)
        {
            group = new Group();
            if (!group->Create(master))
            {
                delete group;
                return;
            }
            sGroupMgr->AddGroup(group);
        }
        if (group->IsFull())
        {
            if (size <= 5 || group->isRaidGroup())
                break;
            group->ConvertToRaid();
        }
        if (!group->AddMember(candidate.Bot))
            continue;

        --left;
        ++members;
        if (!joined.empty())
            joined += ", ";
        joined += candidate.Bot->GetName() + " (" + RoleText(candidate.Role) + ")";
    }

    if (joined.empty())
        commanderAi->TellMaster("Nobody free fits the open spots.");
    else
        commanderAi->TellMaster("Joined: " + joined + ".");
}

bool LfgOrder(PlayerbotAI& ai, std::string const& param)
{
    Player* master = ai.GetMaster();
    if (!master)
        return false;

    uint32 size = 5;
    std::string word(Trim(param));
    if (word == "10" || word == "20" || word == "25" || word == "40")
        size = uint32(std::stoul(word));
    else if (word.empty() && master->GetGroup() && master->GetGroup()->isRaidGroup())
        size = 25;   // AC: "Default to WotLK Raiding. Max size 25"
    else if (!word.empty() && word != "5")
    {
        ai.TellMaster("lfg [5|10|20|25|40]: fill your group with free bots by role.");
        return true;
    }

    uint32 now = GameTime::GetGameTimeMS();
    {
        std::lock_guard lock(g_inviteLock);
        uint32& last = g_lfgOrder[master->GetGUID()];
        if (Within(last, now, LfgOrderRepeatMs))
            return true;
        last = now;
    }

    ObjectGuid masterGuid = master->GetGUID();
    sWorldActionQueue->Enqueue(ai.GetBot()->GetGUID(), ai.GetBot()->GetMapId(), ObjectGuid::Empty, [masterGuid, size](Player* commander)
    {
        FillGroup(commander, masterGuid, size);
    });
    ai.TellMaster("Looking for people for a group of " + std::to_string(size) + ".");
    return true;
}
}

void OnGroupMemberAdded(Group* group, ObjectGuid guid)
{
    if (!sSocialConfig.InviteEnable || !sSocialConfig.InvitePassLead || !group || !guid.IsPlayer())
        return;

    ObjectGuid bot;
    {
        std::lock_guard lock(g_inviteLock);
        auto itr = g_inviter.find(guid);
        if (itr == g_inviter.end())
            return;
        if (Within(itr->second.second, GameTime::GetGameTimeMS(), LeadPassWindowMs))
            bot = itr->second.first;
        g_inviter.erase(itr);
    }
    if (bot.IsEmpty() || group->GetLeaderGUID() != bot)
        return;

    Player* leader = ObjectAccessor::FindConnectedPlayer(bot);
    if (!leader)
        return;
    // after AddMember has finished: the lead goes to the player, and the bot follows its new master
    sWorldActionQueue->Enqueue(bot, leader->GetMapId(), ObjectGuid::Empty, [guid](Player* player)
    {
        Group* current = player->GetGroup();
        if (current && current->GetLeaderGUID() == player->GetGUID() && current->IsMember(guid))
            current->ChangeLeader(guid);
    });
}

void RegisterInvites(AiRegistry& registry)
{
    registry.AddTrigger("social invite due", sSocialConfig.InviteCheckMs, InviteDue, IntervalMode::Gate, 500);
    registry.AddAction<InviteAction>("social invite");
    AddSocialCommand("lfg", "lfg [5|10|20|25|40]: free bots that fit the open roles join your group.", LfgOrder, true);
}
}
