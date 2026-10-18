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

#include "BotSocialMgr.h"
#include "BotSession.h"
#include "Group.h"
#include "GuildPackets.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PartyPackets.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
constexpr uint32 AnswerMin = 1500;          // a person needs a moment to click
constexpr uint32 AnswerMax = 4500;
constexpr uint32 WhisperReplyPause = 20000; // the same bot does not repeat itself to the same player
constexpr uint32 LevelRange = 10;           // GroupInvitations = 1
constexpr uint32 ReplyMin = 700;            // typing time of an answer
constexpr uint32 ReplyMax = 1500;
constexpr uint32 LineGap = 500;             // between two lines of the same bot
constexpr uint32 ScanInterval = 250;        // how often bots are checked for a pending invitation
}

BotSocialMgr* BotSocialMgr::instance()
{
    static BotSocialMgr instance;
    return &instance;
}

// A bot session gets no packets, the invitation shows as state on the player: a pending group or guild invite
void BotSocialMgr::ScanInvites()
{
    sPlayerbotMgr->ForEachRandomBot([this](Player* bot, PlayerbotAI*)
    {
        if (bot->GetGroupInvite())
            NoteInvite(bot->GetGUID(), InviteKind::Party);
        if (bot->GetGuildIdInvited())
            NoteInvite(bot->GetGUID(), InviteKind::Guild);
    });
}

void BotSocialMgr::NoteInvite(ObjectGuid bot, InviteKind kind)
{
    std::lock_guard lock(_mutex);
    if (std::ranges::any_of(_invites, [&](Invite const& invite) { return invite.Bot == bot && invite.Kind == kind; }))
        return;

    _invites.push_back({ bot, kind, _clock.load() + urand(AnswerMin, AnswerMax) });
}

void BotSocialMgr::QueueWhisper(ObjectGuid bot, ObjectGuid to, std::string text)
{
    std::lock_guard lock(_mutex);
    uint32 at = _clock.load() + urand(ReplyMin, ReplyMax);
    uint32& next = _nextSlot[bot];
    // the clock wraps after 49 days, a stale slot of an old line must not push a new one far out
    if (int32(next - at) > 0 && int32(next - at) < 60000)
        at = next;
    next = at + LineGap;

    if (_whispers.size() > 4000)
        _whispers.erase(_whispers.begin(), _whispers.begin() + 2000);
    _whispers.push_back({ bot, to, std::move(text), at });
}

void BotSocialMgr::SendDueWhispers()
{
    std::vector<Whisper> due;
    {
        std::lock_guard lock(_mutex);
        uint32 now = _clock.load();
        for (auto itr = _whispers.begin(); itr != _whispers.end();)
        {
            if (int32(now - itr->At) >= 0)
            {
                due.push_back(std::move(*itr));
                itr = _whispers.erase(itr);
            }
            else
                ++itr;
        }

        if (_nextSlot.size() > 2000)
            _nextSlot.clear();
    }

    std::ranges::stable_sort(due, [](Whisper const& a, Whisper const& b) { return int32(a.At - b.At) < 0; });
    for (Whisper const& whisper : due)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(whisper.Bot);
        Player* to = ObjectAccessor::FindConnectedPlayer(whisper.To);
        if (bot && to && bot->IsInWorld() && to->IsInWorld())
            bot->Whisper(whisper.Text, LANG_UNIVERSAL, to);
    }
}

void BotSocialMgr::Update(uint32 diff)
{
    _clock += diff;
    SendDueWhispers();

    _scanTimer += diff;
    if (_scanTimer >= ScanInterval)
    {
        _scanTimer = 0;
        ScanInvites();
    }

    std::vector<Invite> due;
    {
        std::lock_guard lock(_mutex);
        uint32 now = _clock.load();
        for (auto itr = _invites.begin(); itr != _invites.end();)
        {
            if (itr->AnswerAt <= now)
            {
                due.push_back(*itr);
                itr = _invites.erase(itr);
            }
            else
                ++itr;
        }
    }

    for (Invite const& invite : due)
    {
        if (!sPlayerbotMgr->IsBotOnline(invite.Bot))
            continue;

        Player* bot = ObjectAccessor::FindConnectedPlayer(invite.Bot);
        if (!bot || !bot->IsInWorld())
            continue;

        if (invite.Kind == InviteKind::Party)
            AnswerParty(bot);
        else
            AnswerGuild(bot);
    }
}

// Whether a random bot joins the party of `inviter`: only real players ask, and not while the bot is busy with
// group content of its own
bool BotSocialMgr::WantsParty(Player* bot, Player* inviter) const
{
    if (!inviter || IsBot(inviter) || sPlayerbotsConfig.RandomGroupInvitations == 0)
        return false;

    if (!bot->IsAlive() || bot->InBattleground() || bot->InBattlegroundQueue() || bot->isUsingLfg() || bot->GetGroup())
        return false;

    if (sPlayerbotsConfig.RandomGroupInvitations == 1)
    {
        int32 difference = int32(bot->GetLevel()) - int32(inviter->GetLevel());
        return std::abs(difference) <= int32(LevelRange);
    }

    return true;
}

void BotSocialMgr::AnswerParty(Player* bot)
{
    Group* group = bot->GetGroupInvite();
    if (!group)
        return;

    Player* inviter = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
    bool accept = WantsParty(bot, inviter);

    WorldPackets::Party::PartyInviteResponse response{ WorldPacket(CMSG_PARTY_INVITE_RESPONSE) };
    response.Accept = accept;
    bot->GetSession()->HandlePartyInviteResponseOpcode(response);

    if (accept && bot->GetGroup() && inviter)
        QueueWhisper(bot->GetGUID(), inviter->GetGUID(),
            Trinity::StringFormat("Hello {}! I will follow your lead. Whisper me \"help\" to see what I understand.", inviter->GetName()));
}

void BotSocialMgr::AnswerGuild(Player* bot)
{
    if (!bot->GetGuildIdInvited() || bot->GetGuildId())
        return;

    if (!sPlayerbotsConfig.RandomGuildInvitations)
    {
        WorldPackets::Guild::GuildDeclineInvitation decline{ WorldPacket(CMSG_GUILD_DECLINE_INVITATION) };
        bot->GetSession()->HandleGuildDeclineInvitation(decline);
        return;
    }

    WorldPackets::Guild::AcceptGuildInvite accept{ WorldPacket(CMSG_ACCEPT_GUILD_INVITE) };
    bot->GetSession()->HandleGuildAcceptInvite(accept);
}

void BotSocialMgr::OnWhisper(Player* bot, Player* from)
{
    if (!sPlayerbotsConfig.RandomWhisperReply || !bot->IsAlive())
        return;

    uint32 now = _clock.load();
    auto key = std::make_pair(bot->GetGUID(), from->GetGUID());
    auto itr = _lastReply.find(key);
    if (itr != _lastReply.end() && now - itr->second < WhisperReplyPause)
        return;

    if (_lastReply.size() > 2000)
        _lastReply.clear();
    _lastReply[key] = now;

    QueueWhisper(bot->GetGUID(), from->GetGUID(),
        Trinity::StringFormat("Hi {}! Invite me to your group and I will follow your lead, then whisper me \"help\".", from->GetName()));
}
}
