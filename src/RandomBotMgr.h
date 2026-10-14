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

#ifndef RandomBotMgr_h__
#define RandomBotMgr_h__

#include "ObjectGuid.h"
#include "Position.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class WorldSession;

namespace Playerbots
{
/// Population of ownerless bots. Same shape as AzerothCore's RandomPlayerbotMgr:
/// a pool of bot accounts, staggered login that keeps a faction ratio, and a
/// first-login randomize (level, specialization, outdoor continent; Chromie Time scaling).
/// Accounts are Battle.net game accounts under one bnet login, and characters
/// are created through the normal character-create handler so current races,
/// classes and customizations stay valid.
class RandomBotMgr
{
public:
    static RandomBotMgr* instance();

    void Update(uint32 diff);
    /// Drops a character-creation session. Bot logouts are PlayerbotMgr::LogoutAll
    void Shutdown();

    /// Level, specialization and placement for a bot that just entered the world
    void Prepare(Player* bot);
    /// The bot was moved elsewhere on purpose (summon, teleport): its area is now where it stands
    void UpdateHome(ObjectGuid guid, uint32 mapId, Position const& home);
    Position const* GetHome(ObjectGuid guid) const;

    /// A bot placed at a hotspot belongs there: it wanders and camps within Radius of Center instead of
    /// drifting wherever its last walk ended, until a rotation or a placement elsewhere releases it
    struct Anchor
    {
        uint32 MapId = 0;
        Position Center;
        float Radius = 60.0f;
    };
    Anchor const* GetAnchor(ObjectGuid guid) const;
    void ClearAnchor(ObjectGuid guid);
    /// Map and position a bot was placed at, false if unknown
    bool GetHomeLocation(ObjectGuid guid, uint32& mapId, Position& position) const;
    /// Playerbots.Random.Maps: an empty list allows every map (valid once BuildSpots ran)
    /// Characters ready for random bots (created so far)
    size_t GetCharacterCount() const { return _characters.size(); }
    bool IsMapAllowed(uint32 mapId) const { return _allowedMaps.empty() || _allowedMaps.contains(mapId); }

private:
    struct BnetEntry
    {
        uint32 Id = 0;
        std::string Email;
        uint32 GameAccounts = 0;
    };

    struct GameAccount
    {
        uint32 Id = 0;
        std::string Name;
        uint32 BnetId = 0;
        std::string Email;
        uint32 CharacterCount = 0;
    };

    struct BotCharacter
    {
        ObjectGuid Guid;
        uint32 AccountId = 0;
        uint8 Race = 0;
        uint8 Class = 0;
        bool Alliance = false;
    };

    struct RaceClass
    {
        uint8 Race = 0;
        uint8 Class = 0;
        bool Alliance = false;
    };

    struct Spot
    {
        uint32 MapId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        uint8 MinLevel = 1;
        uint8 MaxLevel = 1;
        uint32 Faction = 0; // 0 = either, otherwise ALLIANCE or HORDE
    };

    struct Creation
    {
        WorldSession* Session = nullptr;
        uint32 AccountId = 0;
        uint8 Race = 0;
        uint8 Class = 0;
        bool Alliance = false;
        bool Initialized = false;
        bool Submitted = false;
        bool Finished = false;
        uint32 Code = 0;
        ObjectGuid Guid;
        uint32 StartedAt = 0;
        std::string Name;
    };

    bool PrepareAccounts();
    bool CreateGameAccount();
    void LoadCharacters();
    void BuildRaceClasses();
    void BuildSpots();

    BotCharacter const* NextOfflineCharacter() const;
    bool WantAlliance() const;
    GameAccount* FindAccount(uint32 accountId);
    GameAccount* AccountWithRoom();

    void LoginOne();
    void BeginCreate();
    bool SubmitCreate();
    void PumpCreation(uint32 diff);
    void FinishCreation();
    bool AdvanceRandomize();
    void Place(Player* bot);
    static void SaveHome(ObjectGuid guid, uint32 mapId, Position const& home);
    static bool LoadHome(ObjectGuid guid, uint32& mapId, Position& home);
    bool HasHumanOnline() const;

    bool NeedsRandomizeFirst(Player const* bot) const;
    uint32 StartLevel(Player const* bot) const;
    Spot const* PickSpot(uint8 level, uint32 faction, ObjectGuid except) const;
    bool PlaceAtHotspot(Player* bot);

    /// Every loaded 533 yard grid costs ~100-175 MB (terrain, vmaps, nav mesh, all spawns), so the online bots may only
    /// stand in Playerbots.Random.MaxGrids different grids. Key = map + grid x/y; `except` is the bot being placed.
    static uint64 GridKeyOf(uint32 mapId, float x, float y);
    std::unordered_set<uint64> OccupiedGrids(ObjectGuid except) const;
    void SetAnchor(ObjectGuid guid, Anchor const& anchor);
    bool LoadAnchor(ObjectGuid guid, Anchor& anchor);
    /// Idle bots roll again for a hotspot or the open world, a small share per minute
    void RotateBots();

    bool _prepared = false;
    bool _active = false;
    uint32 _retryTimer = 0;
    uint32 _wait = 0;
    uint32 _createFailures = 0;

    uint32 CharactersPerAccountLimit() const;
    std::string EmailFor(uint32 index) const;

    std::vector<BnetEntry> _bnets;
    std::vector<GameAccount> _accounts;
    std::vector<BotCharacter> _characters;
    std::vector<RaceClass> _raceClasses;
    std::vector<Spot> _spots;
    std::unordered_set<ObjectGuid> _randomized;
    std::unordered_set<ObjectGuid> _pendingRandomize;
    std::unordered_map<ObjectGuid, Position> _homes;
    std::unordered_map<ObjectGuid, uint32> _homeMaps;
    std::unordered_map<ObjectGuid, Anchor> _anchors;
    uint32 _rotateTimer = 0;
    std::unique_ptr<Creation> _creation;
    std::unordered_set<uint32> _allowedMaps; // empty = every outdoor continent
    uint32 _humanTimer = 0;                  // ms with/without a real player, depending on _humansOnline
    bool _humansOnline = false;
};
}

#define sRandomBotMgr Playerbots::RandomBotMgr::instance()

#endif // RandomBotMgr_h__
