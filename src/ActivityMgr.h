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

#ifndef PlayerbotsActivityMgr_h__
#define PlayerbotsActivityMgr_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

class Battleground;
class Player;
class WorldPacket;

namespace Playerbots
{
class PlayerbotAI;

/// Group content for random bots: dungeon finder, battlegrounds and arenas.
///
/// Bots never start this on their own. When a real player waits in a queue, idle random bots join the same queue
/// to fill it (the roles missing in a dungeon group, the players missing for a battleground, the opposing team of
/// an arena group), and they answer whatever the queue asks of them: proposals, role checks, battleground
/// invitations. They go through the same core code paths a client uses. World thread only, except OnBotPacket.
class ActivityMgr
{
public:
    static ActivityMgr* instance();

    void Update(uint32 diff);
    void Shutdown();

    /// Server packets sent to a random bot. Only dungeon finder proposals matter, they are answered later from Update
    void OnBotPacket(ObjectGuid bot, WorldPacket const& packet);

    /// Accepted battleground invites since boot (#15): accepts held because the bot was being teleported, accepted ports sent
    /// again because they did not land, seats given up (the core keeps those counted until the match closes). .bot bg list
    struct InviteCounters
    {
        uint32 Held = 0;
        uint32 Resent = 0;
        uint32 Lost = 0;
    };
    InviteCounters const& GetInviteCounters() const { return _invites; }

    /// .bot bg test (docs/design/BG_PLAN.md §5, automated test): one normal battleground match the random bots fill and play
    /// with no real player in it. Until the match exists its queue and bracket count as watched, then only its instance. It
    /// ends with the match, with StopBgTest, or at UntilMs (world time, GameTime ms)
    struct BgTest
    {
        uint64 Queue = 0;           // BattlegroundQueueTypeId::GetPacked of the test queue; 0 = no test
        uint8 Bracket = 0;          // BattlegroundBracketId
        uint32 InstanceId = 0;
        uint32 StartedMs = 0;
        uint32 UntilMs = 0;
        uint32 Joined = 0;          // bots sent into its queue
        bool Active() const { return Queue != 0; }
    };
    /// False when the queue is not a valid normal battleground queue
    bool StartBgTest(uint64 packedQueue, uint8 bracket, uint32 minutes);
    void StopBgTest(char const* why);
    BgTest const& GetBgTest() const { return _bgTest; }
    /// The test match: its instance, or (not pinned yet) an instance of the test queue and bracket
    bool IsBgTest(uint32 instanceId) const;

private:
    struct Proposal
    {
        ObjectGuid Bot;
        uint32 Id = 0;
        uint32 AnswerAt = 0;
    };

    struct LfgFill
    {
        ObjectGuid Bot;
        uint32 Since = 0;
    };

    struct ArenaGroup
    {
        ObjectGuid Group;
        ObjectGuid Human;
        uint32 Since = 0;
    };

    struct BotRef
    {
        Player* Bot = nullptr;
        PlayerbotAI* AI = nullptr;
    };

    struct Trip
    {
        uint32 LastSendMs = 0;
        uint8 Sends = 0;
    };

    void AnswerProposals();
    void Scan();
    void ServeBots(std::vector<BotRef> const& bots);
    void FillLfg(std::vector<Player*> const& humans, std::vector<BotRef> const& bots);
    void FillBattlegrounds(std::vector<Player*> const& humans, std::vector<BotRef> const& bots);
    void FillArenas(std::vector<Player*> const& humans, std::vector<BotRef> const& bots);
    void CleanArenaGroups();
    bool FinishTrip(Player* bot, Battleground const* bg, uint32 now);
    void UpdateBgTest();

    std::atomic<uint32> _clock = 0;
    uint32 _scanTimer = 0;

    std::mutex _proposalMutex;
    std::vector<Proposal> _proposals;

    std::map<ObjectGuid, std::vector<LfgFill>> _lfgFills;         // queue owner (player or group) -> bots we queued for it
    std::map<ObjectGuid, std::set<ObjectGuid>> _lfgFailed;        // bots that could not join a queue, not retried
    std::map<ObjectGuid, uint32> _lastDungeonTeleport;
    std::vector<ArenaGroup> _arenaGroups;
    std::map<ObjectGuid, uint32> _backoff;                        // bot -> do not use before (after a failed join)
    std::map<ObjectGuid, uint32> _queueSince;                     // bot -> when it joined a battleground queue
    std::map<ObjectGuid, Trip> _trips;                            // bot -> its accepted battleground port that did not land
    InviteCounters _invites;
    BgTest _bgTest;
};
}

#define sActivityMgr Playerbots::ActivityMgr::instance()

#endif // PlayerbotsActivityMgr_h__
