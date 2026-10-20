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

#ifndef PlayerbotMgr_h__
#define PlayerbotMgr_h__

#include "ObjectGuid.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Map;
class Player;
class WorldSession;

namespace Playerbots
{
class PlayerbotAI;

enum class AddBotResult
{
    Ok,
    Disabled,
    NotFound,
    IsSelf,
    WrongAccount,
    AlreadyOnline,
    TooManyBots
};

/// Owns the bot sessions. Bot sessions are not registered in World (a bot shares the
/// account of its master, and World keys sessions by account), so they are updated here.
/// Everything except GetAI() must be called from the world thread.
class PlayerbotMgr
{
public:
    static PlayerbotMgr* instance();

    /// Logs in a character of the master's account as a bot
    AddBotResult AddBot(Player* master, std::string const& characterName);
    /// Logs in a character as ownerless random bot (accountName/bnetAccountId identify its game account)
    bool AddRandomBot(uint32 accountId, std::string const& accountName, uint32 bnetAccountId, ObjectGuid character);
    bool IsBotOnline(ObjectGuid character) const { return _bots.contains(character); }
    size_t GetRandomBotCount() const;
    /// Calls fn for every logged in ownerless bot (world thread)
    void ForEachRandomBot(std::function<void(Player*, PlayerbotAI*)> const& fn) const;
    /// Logs the bot out on the next update, false if it is not a bot
    bool RemoveBot(ObjectGuid bot);
    void RemoveBotsOf(ObjectGuid master);
    /// Immediately logs out every bot (server shutdown)
    void LogoutAll();
    /// Logs out ownerless random bots, leaving master-controlled bots alone
    void LogoutRandomBots();

    void Update(uint32 diff);
    /// Runs the AI of the bots on a world map. Called by that map's script on the map's update thread
    void TickMapBots(Map* map, uint32 diff);
    /// Runs the AI of the bots in dungeons, battlegrounds and other maps that cannot have a map script of their own.
    /// World thread, after the maps were updated
    void TickOtherMapBots(uint32 diff);
    /// World-thread tick: records DIFF and rebuilds the nearby-player snapshot
    void NoteWorldTick(uint32 diff);

    struct NearbyHuman
    {
        ObjectGuid Guid;
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        ObjectGuid::LowType GuildId = 0;
    };

    std::shared_ptr<std::vector<NearbyHuman> const> GetNearbyHumans() const { return _nearbyHumans.load(); }
    /// A real player on the bot's map within distance yards (from the snapshot taken once a second)
    bool HasHumanWithin(Player const* bot, float distance) const;
    uint32 GetWorldDiff() const { return _worldDiff.load(std::memory_order_relaxed); }
    /// Slowest world tick of the last WorldDiffWindowSeconds: one slow tick (a save, a spawn wave) must not freeze
    /// bots, a lasting load must
    uint32 GetWorldDiffMax() const { return _worldDiffMax.load(std::memory_order_relaxed); }
    bool HasRealPlayerOnline() const;

    /// AI of a logged in bot, nullptr for other players. Safe from map threads
    /// (the bot list only changes in the world thread, outside of map updates)
    PlayerbotAI* GetAI(Player const* player) const;
    std::vector<Player*> GetBotsOf(ObjectGuid master) const;
    size_t GetBotCount(ObjectGuid master) const;

private:
    struct BotHolder
    {
        WorldSession* Session = nullptr;
        ObjectGuid Master;          // empty for random bots
        uint8 FormationIndex = 0;
        uint32 LoginTimer = 0;
        bool Cold = false;          // frozen and idle: visited only every Playerbots.ManagerSliceMs
        uint32 ColdAccum = 0;
        uint32 IdleSessionAccum = 0;
        bool Removing = false;
        std::unique_ptr<PlayerbotAI> AI;
    };

    void OnBotLoggedIn(ObjectGuid guid, BotHolder& holder);
    static void CompleteFarTeleport(WorldSession* session);
    uint8 GetFreeFormationIndex(ObjectGuid master) const;
    void RefreshNearbyHumans();

    std::unordered_map<ObjectGuid, BotHolder> _bots;
    std::atomic<std::shared_ptr<std::vector<NearbyHuman> const>> _nearbyHumans;
    static constexpr uint32 WorldDiffWindowSeconds = 10;

    std::atomic<uint32> _worldDiff{ 50 };
    std::atomic<uint32> _worldDiffMax{ 50 };
    uint32 _diffBuckets[WorldDiffWindowSeconds] = {};
    uint32 _diffBucketIndex = 0;
    uint32 _diffBucketTimer = 0;
    uint32 _nearbyTimer = 0;
};
}

#define sPlayerbotMgr Playerbots::PlayerbotMgr::instance()

#endif // PlayerbotMgr_h__
