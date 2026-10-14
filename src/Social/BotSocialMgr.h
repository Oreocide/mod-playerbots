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

#ifndef PlayerbotsBotSocialMgr_h__
#define PlayerbotsBotSocialMgr_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class Player;
class WorldPacket;

namespace Playerbots
{
/// How random bots deal with real players who talk to them: they answer party and guild invitations after a
/// moment, like a person would, and tell whoever whispers them how to get them to listen.
/// Packets arrive on map threads (OnBotPacket), the answers are given from the world thread (Update).
class BotSocialMgr
{
public:
    static BotSocialMgr* instance();

    void Update(uint32 diff);

    /// Server packets sent to a random bot: party and guild invitations
    void OnBotPacket(ObjectGuid bot, WorldPacket const& packet);

    /// A whisper from `from` to a random bot that does not follow `from`
    void OnWhisper(Player* bot, Player* from);

    /// A bot whispers `to` a moment from now. Sending at once would put the answer in the chat before the
    /// message it answers (the chat hook runs first), and a person needs time to type. Lines of one bot keep
    /// their order. Any thread
    void QueueWhisper(ObjectGuid bot, ObjectGuid to, std::string text);

private:
    enum class InviteKind : uint8
    {
        Party,
        Guild
    };

    struct Invite
    {
        ObjectGuid Bot;
        InviteKind Kind;
        uint32 AnswerAt;
    };

    void AnswerParty(Player* bot);
    void AnswerGuild(Player* bot);
    bool WantsParty(Player* bot, Player* inviter) const;

    struct Whisper
    {
        ObjectGuid Bot;
        ObjectGuid To;
        std::string Text;
        uint32 At;
    };

    void SendDueWhispers();

    std::atomic<uint32> _clock = 0;
    std::mutex _mutex;
    std::vector<Whisper> _whispers;
    std::map<ObjectGuid, uint32> _nextSlot;                           // bot -> earliest time of its next line
    std::vector<Invite> _invites;
    std::map<std::pair<ObjectGuid, ObjectGuid>, uint32> _lastReply;   // (bot, player) -> when the bot last answered a whisper
};
}

#define sBotSocialMgr Playerbots::BotSocialMgr::instance()

#endif // PlayerbotsBotSocialMgr_h__
