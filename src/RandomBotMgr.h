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
#include "Optional.h"
#include "Position.h"
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <algorithm>
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
    /// Copy, not a pointer: map threads write homes (UpdateHome) while other threads read them
    Optional<Position> GetHome(ObjectGuid guid) const;

    /// A bot placed at a hotspot belongs there: it wanders and camps within Radius of Center instead of
    /// drifting wherever its last walk ended, until a rotation or a placement elsewhere releases it
    struct Anchor
    {
        uint32 MapId = 0;
        Position Center;
        float Radius = 60.0f;
    };
    /// Random bots standing on ground that attacks their team (an enemy-only zone, an enemy town: summoned there,
    /// moved by a GM, left there by an old placement) go back to a valid spot. World thread, about once a minute.
    void EvictFromEnemyGround();
    Optional<Anchor> GetAnchor(ObjectGuid guid) const;
    void ClearAnchor(ObjectGuid guid);
    /// A far bot hangs over nothing at this point: the height it was given came from the base map, but the terrain swap every player is in
    /// (Teldrassil after the burning is ocean at Z -24, so the old Darnassus ground does not exist for them) has no ground there. The area is
    /// not used for placement any more. Any thread.
    void ReportVoid(uint32 mapId, float x, float y);
    bool IsVoid(uint32 mapId, float x, float y) const;
    /// Places the bot on a new valid spot. World thread only
    void PlaceAgain(Player* bot) { Place(bot); }
    /// Map and position a bot was placed at, false if unknown
    bool GetHomeLocation(ObjectGuid guid, uint32& mapId, Position& position) const;
    /// Playerbots.Random.Maps: an empty list allows every map (valid once BuildSpots ran)
    /// Characters ready for random bots (created so far)
    size_t GetCharacterCount() const { return _characters.size(); }
    /// Hire (Social/BotHire): takes the character out of the pool so it is not logged in again; false if it is not a random bot's.
    /// Hold keeps it aside, Restore puts it back (the hire failed), Forget drops it for good (the hire went through). World thread
    bool Hold(ObjectGuid guid);
    void Restore(ObjectGuid guid);
    void Forget(ObjectGuid guid);
    /// A character made for random bots (a guild led by one is a bot guild). World thread
    bool IsBotCharacter(ObjectGuid guid) const
    {
        return std::ranges::any_of(_characters, [guid](BotCharacter const& c) { return c.Guid == guid; });
    }
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
        uint32 ZoneId = 0;  // the graveyard's zone (its AreaTable row decides who owns the ground)
        mutable uint8 HostileMask = 0; // bit 0: inside a town that attacks Alliance bots, bit 1: Horde (filled when the hub index exists)
        mutable uint16 Weight = 1;     // how often this zone is chosen: capitals high, desolate graveyards 1
        mutable float Scatter = 80.0f; // yards around the graveyard a bot may stand, still inside the zone
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
    void MaybeBeginCreate(uint32 diff);
    int64 _bgCreateTimer = 0;
    void PrepareWarm();
    /// One step of Playerbots.Warm.Maps (create a map, or load its grid) per call; true while steps are left (bot logins wait)
    bool RunWarmStep();
    struct WarmStep { uint32 MapId = 0; bool LoadGrid = false; float X = 0.0f; float Y = 0.0f; bool Pin = false; };
    void AppendExtraWarm();
    std::deque<WarmStep> _warmSteps;

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
    /// A ground point around the graveyard, inside its zone. Under the grid cap, only a grid a bot already occupies.
    Position ScatterSpot(Spot const& spot, ObjectGuid except, Player* bot = nullptr) const;
    bool PlaceAtHotspot(Player* bot);

    /// Every loaded 533 yard grid costs ~100-175 MB (terrain, vmaps, nav mesh, all spawns), so the online bots may only
    /// stand in Playerbots.Random.MaxGrids different grids. Key = map + grid x/y; `except` is the bot being placed.
    static uint64 GridKeyOf(uint32 mapId, float x, float y);
    std::unordered_set<uint64> OccupiedGrids(ObjectGuid except) const;
    void SetAnchor(ObjectGuid guid, Anchor const& anchor);
    bool LoadAnchor(ObjectGuid guid, Anchor& anchor);
    /// Idle bots roll again for a hotspot or the open world, a small share per minute
    void RotateBots();
    /// Far bots: one-time level from the band weights, then a relocate every 1-5 hours with a small chance of +1 level.
    /// A corpse with nobody near is resurrected after 60-300 seconds and placed. World thread.
    void MaintainPopulation();
    uint8 RollBandLevel(uint32 floor, uint32 cap) const;
    void ApplyLevel(Player* bot, uint8 level, bool pickSpec);

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
    std::unordered_map<ObjectGuid, BotCharacter> _held;   // characters in the middle of a hire
    std::vector<RaceClass> _raceClasses;
    std::vector<Spot> _spots;
    std::unordered_set<ObjectGuid> _randomized;
    std::unordered_set<ObjectGuid> _pendingRandomize;
    // _homes, _homeMaps and _anchors are written from map threads (PlayerbotAI::OnInterrupted, rpg anchor checks)
    // and the world thread, so every access holds _homesLock
    mutable std::shared_mutex _homesLock;
    mutable std::mutex _voidLock;
    std::vector<std::tuple<uint32, float, float>> _voidSpots;   // map, x, y of places a bot hung over nothing
    std::unordered_map<ObjectGuid, Position> _homes;
    std::unordered_map<ObjectGuid, uint32> _homeMaps;
    std::unordered_map<ObjectGuid, Anchor> _anchors;
    uint32 _rotateTimer = 0;
    uint32 _populationTimer = 0;
    std::unordered_set<ObjectGuid> _banded;                 // world thread: level already drawn from the band weights
    std::unordered_map<ObjectGuid, uint32> _nextPopulationAt; // unix seconds of the next far relocate
    std::unordered_map<ObjectGuid, uint32> _reviveAt;       // game time ms when a far corpse may be raised
    mutable bool _spotHostilityKnown = false;   // Spot::HostileMask is filled
    std::unordered_map<ObjectGuid, uint32> _evictedAt;   // world thread only: when a bot was last moved off enemy ground (ms)
    std::unique_ptr<Creation> _creation;
    std::unordered_set<uint32> _allowedMaps; // empty = every outdoor continent
    uint32 _humanTimer = 0;                  // ms with/without a real player, depending on _humansOnline
    bool _humansOnline = false;
};
}

#define sRandomBotMgr Playerbots::RandomBotMgr::instance()

#endif // RandomBotMgr_h__
