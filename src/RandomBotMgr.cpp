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

#include "RandomBotMgr.h"
#include "AccountMgr.h"
#include "BotPerf.h"
#include "Perf/BotPerfReporter.h"
#include "BotSession.h"
#include "BattlenetAccountMgr.h"
#include "BotSpellBook.h"
#include "ByteBuffer.h"
#include "CharacterCache.h"
#include "ClientBuildInfo.h"
#include "DB2Stores.h"
#include "DatabaseEnv.h"
#include "Db/PlayerbotRepository.h"
#include "GameTime.h"
#include "Economy/GatherNodes.h"
#include "GearFactory.h"
#include "IteratorPair.h"
#include "Log.h"
#include "GridDefines.h"
#include "HubIndex.h"
#include "Map.h"
#include "PhasingHandler.h"
#include "MapManager.h"
#include "Timer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <tuple>
#include <sstream>
#include <unordered_set>
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "QuestTravel.h"
#include "Talents/BotTalents.h"
#include "Economy/TrainerIndex.h"
#include "Spawn/SpawnPolicy.h"
#include "Travel/HotspotMgr.h"
#include "Travel/HubIndex.h"
#include "Travel/TravelMgr.h"
#include "RaceMask.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "CharacterPackets.h"
#include <algorithm>
#include <ranges>
#include <shared_mutex>

namespace
{
// Bump to put every random bot on a valid spot at its next login (open-world spots scatter inside
// the graveyard's zone, weighted so cities are crowded and desolate graveyards stay thin)
constexpr int32 PlacementVersion = 6;
constexpr uint32 RotateCheckMs = 60000;          // hotspot rotation looks at the bots once a minute
constexpr float RotateObservedDistance = 250.0f; // a bot a player can see is not moved

constexpr uint32 CreateTimeout = 60000;
constexpr uint32 CreateFailurePause = 300000;

bool IsUsableName(std::string& name)
{
    if (!normalizePlayerName(name))
        return false;

    if (name.size() > MAX_PLAYER_NAME)
        return false;

    if (ObjectMgr::CheckPlayerName(name, LOCALE_enUS, true) != CHAR_NAME_SUCCESS)
        return false;

    if (sObjectMgr->IsReservedName(name))
        return false;

    return !sCharacterCache->GetCharacterCacheByName(name);
}

std::string MakeName(uint8 race, uint8 gender)
{
    // playerbots_names: names are used up one by one, unusable ones (profanity, reserved, taken) are skipped
    for (uint32 attempt = 0; attempt < 50; ++attempt)
    {
        std::string name;
        if (!sPlayerbotRepository->PopName(race, gender, name))
            break;

        if (IsUsableName(name))
            return name;
    }

    static constexpr std::string_view Suffixes[] = { "", "a", "e", "i", "o", "n", "s", "is", "or", "yn", "el", "an" };
    for (uint32 attempt = 0; attempt < 12; ++attempt)
    {
        std::string base = sDB2Manager.GetNameGenEntry(race, gender);
        if (base.empty())
            break;

        std::string name = base;
        if (attempt > 0)
        {
            std::string_view suffix = Suffixes[attempt % std::size(Suffixes)];
            if (base.size() + suffix.size() > MAX_PLAYER_NAME)
                continue;
            name += suffix;
        }

        if (IsUsableName(name))
            return name;
    }

    static constexpr std::string_view Syllables[] = {
        "Al", "An", "Ar", "Bel", "Cor", "Dan", "El", "Fal", "Gar", "Hal", "Ian", "Jel",
        "Kal", "Lor", "Mar", "Nor", "Or", "Pel", "Ral", "Sar", "Tor", "Ul", "Val", "Wen", "Yor", "Zan"
    };

    for (uint32 attempt = 0; attempt < 20; ++attempt)
    {
        std::string name(Syllables[urand(0, std::size(Syllables) - 1)]);
        for (uint32 extra = urand(1, 2); extra > 0; --extra)
            name += Syllables[urand(0, std::size(Syllables) - 1)];

        if (name.size() > MAX_PLAYER_NAME)
            name.resize(MAX_PLAYER_NAME);

        if (IsUsableName(name))
            return name;
    }

    return {};
}

Trinity::IteratorPair<UF::ChrCustomizationChoice const*> Choices(std::vector<UF::ChrCustomizationChoice> const& picked)
{
    return { picked.data(), picked.data() + picked.size() };
}

bool FillCustomizations(WorldSession* session, uint8 race, uint8 playerClass, uint8 gender, WorldPackets::Array<UF::ChrCustomizationChoice, 250>& out)
{
    std::vector<ChrCustomizationOptionEntry const*> const* options = sDB2Manager.GetCustomiztionOptions(race, gender);
    if (!options || options->empty())
        return true;

    for (uint32 attempt = 0; attempt < 8; ++attempt)
    {
        std::vector<UF::ChrCustomizationChoice> picked;
        std::vector<ChrCustomizationOptionEntry const*> pending(options->begin(), options->end());
        std::ranges::sort(pending, std::ranges::less(), [](ChrCustomizationOptionEntry const* option) { return option->ID; });

        for (uint32 pass = 0; pass < 4 && !pending.empty(); ++pass)
        {
            for (auto itr = pending.begin(); itr != pending.end();)
            {
                ChrCustomizationOptionEntry const* option = *itr;
                auto selected = Choices(picked);
                if (option->ChrCustomizationReqID)
                    if (ChrCustomizationReqEntry const* requirement = sChrCustomizationReqStore.LookupEntry(option->ChrCustomizationReqID))
                        if (!session->MeetsChrCustomizationReq(requirement, Races(race), Classes(playerClass), false, selected))
                        {
                            itr = pending.erase(itr);
                            continue;
                        }

                std::vector<ChrCustomizationChoiceEntry const*> const* choices = sDB2Manager.GetCustomiztionChoices(option->ID);
                if (!choices || choices->empty())
                {
                    itr = pending.erase(itr);
                    continue;
                }

                std::vector<ChrCustomizationChoiceEntry const*> usable;
                for (ChrCustomizationChoiceEntry const* choice : *choices)
                {
                    if (choice->ChrCustomizationReqID)
                        if (ChrCustomizationReqEntry const* requirement = sChrCustomizationReqStore.LookupEntry(choice->ChrCustomizationReqID))
                            if (!session->MeetsChrCustomizationReq(requirement, Races(race), Classes(playerClass), true, selected))
                                continue;

                    usable.push_back(choice);
                }

                if (usable.empty())
                {
                    ++itr;
                    continue;
                }

                ChrCustomizationChoiceEntry const* choice = usable[urand(0, uint32(usable.size() - 1))];
                UF::ChrCustomizationChoice pickedChoice;
                pickedChoice.ChrCustomizationOptionID = option->ID;
                pickedChoice.ChrCustomizationChoiceID = choice->ID;
                picked.push_back(pickedChoice);
                itr = pending.erase(itr);
            }
        }

        std::ranges::sort(picked, std::ranges::less(), &UF::ChrCustomizationChoice::ChrCustomizationOptionID);
        auto tail = std::ranges::unique(picked, {}, &UF::ChrCustomizationChoice::ChrCustomizationOptionID);
        picked.erase(tail.begin(), tail.end());

        // never an empty list when the race has options: 12.1.5 hangs building a model from no choices, and the core
        // check for it is not something this module may rely on (no core edits)
        if (!picked.empty() && session->ValidateAppearance(Races(race), Classes(playerClass), Gender(gender), Choices(picked)))
        {
            for (UF::ChrCustomizationChoice const& choice : picked)
                out.push_back(choice);
            return true;
        }
    }

    return false;
}
}

namespace Playerbots
{
RandomBotMgr* RandomBotMgr::instance()
{
    static RandomBotMgr instance;
    return &instance;
}

void RandomBotMgr::Update(uint32 diff)
{
    bool enabled = sPlayerbotsConfig.Enabled && sPlayerbotsConfig.RandomEnabled && sPlayerbotsConfig.RandomCount > 0;
    if (!enabled)
    {
        if (_active)
        {
            Shutdown();
            sPlayerbotMgr->LogoutRandomBots();
            _active = false;
        }
        return;
    }

    if (!_prepared)
    {
        if (_retryTimer > diff)
        {
            _retryTimer -= diff;
            return;
        }

        _retryTimer = 0;
        if (!PrepareAccounts())
        {
            _retryTimer = 60000;
            return;
        }

        LoadCharacters();
        BuildRaceClasses();
        BuildSpots();
        PrepareWarm();
        HubIndex::Build();
        sHotspotMgr->BuildFromHubs();
        GatherNodes::Build();
        TrainerIndex::Build();
        if (sPlayerbotsConfig.TravelEnable)
            sTravelMgr->Build();
        if (sPlayerbotsConfig.RandomGear)
            sGearFactory->Build();
        if (sPlayerbotsConfig.RandomQuests)
            QuestTravel::BuildIndex();
        _prepared = true;
        AppendExtraWarm();
        TC_LOG_INFO("server.playerbots", "Playerbots: random bots ready, {} characters on {} game accounts, {} placement spots, keeping {} online",
            _characters.size(), _accounts.size(), _spots.size(), sPlayerbotsConfig.RandomCount);
    }

    _active = true;

    if (RunWarmStep())
        return;    // bots log in once the maps are warm

    {
        BotPerf::Scope perfHotspot(BotPerf::M_HOTSPOT);
        sHotspotMgr->Update(diff);
    }
    _rotateTimer += diff;
    if (_rotateTimer >= RotateCheckMs)
    {
        _rotateTimer = 0;
        RotateBots();
    }

    _populationTimer += diff;
    if (_populationTimer >= 10000)
    {
        _populationTimer = 0;
        MaintainPopulation();
    }

    if (sPlayerbotsConfig.RandomOnlyWithPlayers)
    {
        bool humans = HasHumanOnline();
        if (humans != _humansOnline)
        {
            _humansOnline = humans;
            _humanTimer = 0;
            if (humans)
                TC_LOG_INFO("server.playerbots", "Playerbots: a player is online, random bots log in after {} seconds", sPlayerbotsConfig.RandomPlayerLoginDelay);
            else if (sPlayerbotMgr->GetRandomBotCount())
                TC_LOG_INFO("server.playerbots", "Playerbots: no players left, random bots log out after {} seconds", sPlayerbotsConfig.RandomPlayerLogoutDelay);
        }
        else
            _humanTimer += diff;

        if (!humans)
        {
            if (sPlayerbotMgr->GetRandomBotCount() && _humanTimer >= sPlayerbotsConfig.RandomPlayerLogoutDelay * IN_MILLISECONDS)
            {
                sPlayerbotMgr->LogoutRandomBots();
                _humanTimer = 0;
            }

            if (_creation)
                PumpCreation(diff);
            else if (!AdvanceRandomize())
                MaybeBeginCreate(diff);
            return;
        }

        if (_humanTimer < sPlayerbotsConfig.RandomPlayerLoginDelay * IN_MILLISECONDS)
        {
            if (_creation)
                PumpCreation(diff);
            else if (!AdvanceRandomize())
                MaybeBeginCreate(diff);
            return;
        }
    }

    if (AdvanceRandomize())
        return;

    if (_creation)
    {
        PumpCreation(diff);
        return;
    }

    MaybeBeginCreate(diff);
    if (_creation)
        return;

    if (_wait > diff)
    {
        _wait -= diff;
        return;
    }
    _wait = 0;

    if (sPlayerbotMgr->GetRandomBotCount() >= sPlayerbotsConfig.RandomCount)
        return;

    // keeps a benchmark from pushing the host into swap (or the OOM killer into the database)
    if (sPlayerbotsConfig.RandomMaxRssMB)
    {
        static bool rssGuardLogged = false;
        uint32 rss = BotPerfReporter::ReadRssMB();
        if (rss > sPlayerbotsConfig.RandomMaxRssMB)
        {
            if (!rssGuardLogged)
                TC_LOG_ERROR("server.playerbots", "Playerbots: RSS {} MB is above Playerbots.Random.MaxRssMB {}, no more bot logins ({} online)", rss, sPlayerbotsConfig.RandomMaxRssMB, sPlayerbotMgr->GetRandomBotCount());
            rssGuardLogged = true;
            return;
        }
        rssGuardLogged = false;
    }

    if (NextOfflineCharacter())
    {
        for (uint32 i = 0; i < sPlayerbotsConfig.RandomLoginBatch && sPlayerbotMgr->GetRandomBotCount() < sPlayerbotsConfig.RandomCount; ++i)
            LoginOne();
    }
    else if (_characters.size() < sPlayerbotsConfig.RandomCount)
        BeginCreate();
}

void RandomBotMgr::Shutdown()
{
    if (_creation)
    {
        delete _creation->Session;
        _creation.reset();
    }
}

uint32 RandomBotMgr::CharactersPerAccountLimit() const
{
    uint32 limit = sPlayerbotsConfig.RandomCharactersPerAccount;
    limit = std::min(limit, sWorld->getIntConfig(CONFIG_CHARACTERS_PER_REALM));
    limit = std::min(limit, sWorld->getIntConfig(CONFIG_CHARACTERS_PER_ACCOUNT));
    return std::max<uint32>(limit, 1);
}

std::string RandomBotMgr::EmailFor(uint32 index) const
{
    std::string email = Trinity::StringFormat("{}{:04}@{}", sPlayerbotsConfig.RandomEmailPrefix, index, sPlayerbotsConfig.RandomEmailDomain);
    Utf8ToUpperOnlyLatin(email);
    return email;
}

bool RandomBotMgr::PrepareAccounts()
{
    std::string prefix = sPlayerbotsConfig.RandomEmailPrefix;
    std::string domain = sPlayerbotsConfig.RandomEmailDomain;
    Utf8ToUpperOnlyLatin(prefix);
    Utf8ToUpperOnlyLatin(domain);
    if (prefix.empty() || domain.empty())
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: Playerbots.Random.EmailPrefix / EmailDomain must not be empty");
        return false;
    }

    _bnets.clear();
    _accounts.clear();

    std::string like = prefix + "____@" + domain;
    LoginDatabase.EscapeString(like);
    if (QueryResult result = LoginDatabase.PQuery(
        "SELECT a.id, a.username, a.battlenet_account, b.email FROM account a "
        "JOIN battlenet_accounts b ON b.id = a.battlenet_account WHERE b.email LIKE '{}' ORDER BY b.email, a.battlenet_index", like))
    {
        do
        {
            Field* fields = result->Fetch();
            GameAccount account;
            account.Id = fields[0].GetUInt32();
            account.Name = fields[1].GetString();
            account.BnetId = fields[2].GetUInt32();
            account.Email = fields[3].GetString();

            auto bnet = std::ranges::find(_bnets, account.BnetId, &BnetEntry::Id);
            if (bnet == _bnets.end())
            {
                _bnets.push_back({ account.BnetId, account.Email, 0 });
                bnet = std::prev(_bnets.end());
            }
            ++bnet->GameAccounts;
            _accounts.push_back(std::move(account));
        } while (result->NextRow());
    }

    uint32 needed = (sPlayerbotsConfig.RandomCount + CharactersPerAccountLimit() - 1) / CharactersPerAccountLimit();
    while (_accounts.size() < needed)
        if (!CreateGameAccount())
            break;

    if (_accounts.empty())
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: random bots have no game accounts");
        return false;
    }

    for (GameAccount const& account : _accounts)
        sPlayerbotRepository->MarkRandomAccount(account.Id);

    TC_LOG_INFO("server.playerbots", "Playerbots: {} random bot game accounts on {} bnet emails ({} characters each)", _accounts.size(), _bnets.size(), CharactersPerAccountLimit());
    return true;
}

// Fills the latest bnet email up to GameAccountsPerEmail game accounts, then starts the next email
bool RandomBotMgr::CreateGameAccount()
{
    auto randomPassword = []
    {
        static constexpr std::string_view Alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::string password;
        for (uint32 i = 0; i < 16; ++i)
            password += Alphabet[urand(0, uint32(Alphabet.size() - 1))];
        return password;
    };

    BnetEntry* bnet = _bnets.empty() ? nullptr : &_bnets.back();
    if (bnet && (bnet->GameAccounts >= sPlayerbotsConfig.RandomGameAccountsPerEmail || Battlenet::AccountMgr::GetMaxIndex(bnet->Id) >= 255))
        bnet = nullptr;

    GameAccount account;
    if (!bnet)
    {
        uint32 emailIndex = uint32(_bnets.size()) + 1;
        std::string email = EmailFor(emailIndex);
        while (Battlenet::AccountMgr::GetId(email))
            email = EmailFor(++emailIndex);

        std::string gameName;
        AccountOpResult result = Battlenet::AccountMgr::CreateBattlenetAccount(email, randomPassword(), true, &gameName);
        uint32 bnetId = result == AccountOpResult::AOR_OK ? Battlenet::AccountMgr::GetId(email) : 0;
        if (!bnetId)
        {
            TC_LOG_ERROR("server.playerbots", "Playerbots: failed to create random bot bnet account {} ({})", email, uint32(result));
            return false;
        }

        _bnets.push_back({ bnetId, email, 1 });
        account.Id = AccountMgr::GetId(gameName);
        account.Name = gameName;
        account.BnetId = bnetId;
        account.Email = email;
        TC_LOG_INFO("server.playerbots", "Playerbots: created random bot email {} with game account {}", email, gameName);
    }
    else
    {
        uint8 index = Battlenet::AccountMgr::GetMaxIndex(bnet->Id) + 1;
        std::string name = Trinity::StringFormat("{}#{}", bnet->Id, uint32(index));
        AccountOpResult result = sAccountMgr->CreateAccount(name, randomPassword(), bnet->Email, bnet->Id, index);
        if (result != AccountOpResult::AOR_OK)
        {
            TC_LOG_ERROR("server.playerbots", "Playerbots: failed to create random bot game account {} ({})", name, uint32(result));
            return false;
        }

        account.Id = AccountMgr::GetId(name);
        account.Name = name;
        account.BnetId = bnet->Id;
        account.Email = bnet->Email;
        ++bnet->GameAccounts;
        TC_LOG_INFO("server.playerbots", "Playerbots: created random bot game account {}", name);
    }

    if (!account.Id)
        return false;

    _accounts.push_back(std::move(account));
    return true;
}

void RandomBotMgr::LoadCharacters()
{
    _characters.clear();
    for (GameAccount& account : _accounts)
    {
        account.CharacterCount = 0;
        QueryResult result = CharacterDatabase.PQuery("SELECT guid, race, class FROM characters WHERE account = {} AND deleteDate IS NULL", account.Id);
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            BotCharacter character;
            character.Guid = ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt64());
            character.AccountId = account.Id;
            character.Race = fields[1].GetUInt8();
            character.Class = fields[2].GetUInt8();
            character.Alliance = Player::TeamForRace(character.Race) == ALLIANCE;
            _characters.push_back(character);
            ++account.CharacterCount;
        } while (result->NextRow());
    }
}

void RandomBotMgr::BuildRaceClasses()
{
    _raceClasses.clear();
    uint8 expansion = uint8(sWorld->getIntConfig(CONFIG_EXPANSION));
    Trinity::RaceMask<uint64> disabledRaces{ sWorld->GetUInt64Config(CONFIG_CHARACTER_CREATING_DISABLED_RACEMASK) };
    uint32 disabledClasses = sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK);
    uint32 playableClasses = uint32(CLASSMASK_ALL_PLAYABLE);

    for (ChrRacesEntry const* race : sChrRacesStore)
    {
        if (!race || race->ID == 0 || race->ID > 255)
            continue;

        if (race->GetFlags().HasFlag(ChrRacesFlag::NPCOnly) || race->GetFlags().HasFlag(ChrRacesFlag::NotSelectable) || race->GetFlags().HasFlag(ChrRacesFlag::InternalOnly))
            continue;

        if (disabledRaces.HasRace(race->ID))
            continue;

        RaceUnlockRequirement const* unlock = sObjectMgr->GetRaceUnlockRequirement(uint8(race->ID));
        if (!unlock || unlock->Expansion > expansion)
            continue;

        Team team = Player::TeamForRace(uint8(race->ID));
        if (team != ALLIANCE && team != HORDE)
            continue;

        for (uint8 playerClass = CLASS_WARRIOR; playerClass < MAX_CLASSES; ++playerClass)
        {
            uint32 classMask = 1u << (playerClass - 1);
            if (!(classMask & playableClasses) || (classMask & disabledClasses))
                continue;

            if (playerClass == CLASS_DEMON_HUNTER || playerClass == CLASS_EVOKER)
                continue;

            if (!sObjectMgr->GetPlayerInfo(race->ID, playerClass))
                continue;

            if (ClassAvailability const* requirement = sObjectMgr->GetClassExpansionRequirement(uint8(race->ID), playerClass))
            {
                if (requirement->ActiveExpansionLevel > expansion || requirement->AccountExpansionLevel > expansion)
                    continue;
            }
            else if (ClassAvailability const* fallback = sObjectMgr->GetClassExpansionRequirementFallback(playerClass))
            {
                if (fallback->MinActiveExpansionLevel > expansion || fallback->AccountExpansionLevel > expansion)
                    continue;
            }
            else
                continue;

            _raceClasses.push_back({ uint8(race->ID), playerClass, team == ALLIANCE });
        }
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: {} playable race/class combinations for random bots", _raceClasses.size());
}

// Playerbots.Warm.Maps: "auto" = every map the bots can stand on (one grid each, from their placement spots), plus "mapId" / "mapId:x:y" entries. The first
// terrain query per map (VMAP tree, navmesh set-up) costs hundreds of MB and seconds of world-thread time, tens of seconds with a cold page cache; doing
// it here pays it once, one step per world tick (so the freeze watchdog never sees one long stall), before the first bot logs in.
// Playerbots.Random.CreateUpTo: background character creation. With OnlyWithPlayers the manager used to only finish a creation that was already running while nobody
// was online, and in the normal path it only created when fewer bots than Random.Count were online (and then logged each new one in). This starts a creation every
// Random.CreateDelay until CreateUpTo characters exist, whatever is online; the new characters stay offline until Random.Count asks for them.
void RandomBotMgr::MaybeBeginCreate(uint32 diff)
{
    uint32 target = sPlayerbotsConfig.RandomCreateUpTo;
    if (!target || _creation || _characters.size() >= target)
        return;

    _bgCreateTimer += diff;
    if (_bgCreateTimer < int64(sPlayerbotsConfig.RandomCreateDelay))
        return;

    _bgCreateTimer = 0;
    BeginCreate();
    if (!_creation)    // no account with room, no race/class: back off instead of logging an error every CreateDelay
        _bgCreateTimer = -int64(CreateFailurePause);
}

void RandomBotMgr::PrepareWarm()
{
    _warmSteps.clear();
    if (sPlayerbotsConfig.WarmMaps.empty())
        return;

    std::unordered_set<uint32> created;
    auto add = [this, &created](uint32 mapId, bool hasPos, float x, float y)
    {
        if (created.insert(mapId).second)
            _warmSteps.push_back({ mapId, false, 0.0f, 0.0f });
        if (hasPos)
            _warmSteps.push_back({ mapId, true, x, y });
    };

    std::stringstream list(sPlayerbotsConfig.WarmMaps);
    std::string token;
    while (std::getline(list, token, ','))
    {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }), token.end());
        if (token == "auto")
        {
            std::unordered_set<uint32> seen;
            for (Spot const& spot : _spots)
                if (seen.insert(spot.MapId).second)
                    add(spot.MapId, true, spot.X, spot.Y);
            continue;
        }

        uint32 mapId = 0;
        float x = 0.f, y = 0.f;
        int parsed = std::sscanf(token.c_str(), "%u:%f:%f", &mapId, &x, &y);
        if (parsed >= 1)
            add(mapId, parsed == 3, x, y);
    }
}

// Playerbots.Warm.Hubs / Warm.PlayerPositions: after the maps, load (with their navmesh tiles) the grids around the biggest hubs and around where the real
// characters last logged out, and pin them. A player's arrival in a cold area costs seconds of world-thread time (about 3 s per grid with a cold page cache,
// 29 s for nine grids in Silvermoon); this moves that cost to startup, before any bot or player is there.
void RandomBotMgr::AppendExtraWarm()
{
    uint32 hubs = sPlayerbotsConfig.WarmHubs, players = sPlayerbotsConfig.WarmPlayerPositions;
    if (!hubs && !players)
        return;

    float radius = sPlayerbotsConfig.WarmRadius;
    bool pin = sPlayerbotsConfig.WarmPin;
    std::unordered_set<uint32> created;
    for (WarmStep const& s : _warmSteps)
        if (!s.LoadGrid)
            created.insert(s.MapId);

    std::set<std::tuple<uint32, uint32, uint32>> grids;
    auto addAround = [&](uint32 mapId, float cx, float cy)
    {
        if (created.insert(mapId).second)
            _warmSteps.push_back({ mapId, false, 0.0f, 0.0f, false });
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
            {
                float px = cx + float(dx) * radius, py = cy + float(dy) * radius;
                if (!Trinity::IsValidMapCoord(px, py))
                    continue;
                GridCoord g = Trinity::ComputeGridCoord(px, py);
                if (grids.insert({ mapId, g.x_coord, g.y_coord }).second)
                    _warmSteps.push_back({ mapId, true, px, py, pin });
            }
    };

    uint32 hubCount = 0;
    if (hubs)
    {
        std::vector<Hotspot> all = sHotspotMgr->GetRegistry().Snapshot();
        std::stable_sort(all.begin(), all.end(), [](Hotspot const& a, Hotspot const& b) { return a.Tier != b.Tier ? a.Tier < b.Tier : a.Radius > b.Radius; });
        for (Hotspot const& hub : all)
        {
            if (hubCount >= hubs)
                break;
            addAround(hub.Map, hub.Center.X, hub.Center.Y);
            ++hubCount;
        }
    }

    uint32 playerCount = 0;
    if (players)
    {
        std::unordered_set<uint64> botGuids;
        for (BotCharacter const& c : _characters)
            botGuids.insert(c.Guid.GetCounter());

        if (QueryResult result = CharacterDatabase.Query("SELECT guid, map, position_x, position_y FROM characters WHERE deleteDate IS NULL ORDER BY logout_time DESC LIMIT 400"))
            do
            {
                Field* fields = result->Fetch();
                if (botGuids.contains(fields[0].GetUInt64()))
                    continue;
                addAround(fields[1].GetUInt16(), fields[2].GetFloat(), fields[3].GetFloat());
                if (++playerCount >= players)
                    break;
            } while (result->NextRow());
    }

    TC_LOG_INFO("server.playerbots", "Playerbots: warm-up: {} hubs and {} character positions queued, {} steps in total", hubCount, playerCount, _warmSteps.size());
}

namespace
{
// Creates a continent's base map before a player needs it, with public API only. MapManager::CreateMap wants a Player but reads it only for dungeons,
// battlegrounds and maps split by faction; a continent needs none of that, so a throwaway Player on a socketless bot session is enough. It is never
// added to the world and is deleted at once.
Map* WarmCreateMap(uint32 mapId)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry || !entry->IsWorldMap() || entry->IsSplitByFaction())
        return nullptr;

    if (Map* existing = sMapMgr->FindMap(mapId, 0))
        return existing;

    WorldSession* session = new WorldSession(0, std::string(), 0, std::string(), nullptr, SEC_PLAYER, uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "Win",
        Minutes(0), 0, ClientBuild::VariantId{}, LOCALE_enUS, 0, false);
    Player* dummy = new Player(session);
    Map* map = sMapMgr->CreateMap(mapId, dummy);
    delete dummy;
    delete session;
    return map;
}
}

bool RandomBotMgr::RunWarmStep()
{
    if (_warmSteps.empty())
        return false;

    WarmStep step = _warmSteps.front();
    _warmSteps.pop_front();
    uint32 start = getMSTime();
    if (!step.LoadGrid)
    {
        if (WarmCreateMap(step.MapId))
            TC_LOG_INFO("server.playerbots", "Playerbots: warm-up: map {} created in {} ms", step.MapId, GetMSTimeDiffToNow(start));
        else
        {
            TC_LOG_ERROR("server.playerbots", "Playerbots: warm-up: map {} cannot be created at startup (instanceable or unknown), skipping it", step.MapId);
            std::erase_if(_warmSteps, [&step](WarmStep const& s) { return s.MapId == step.MapId; });
        }
    }
    else if (Map* map = sMapMgr->FindMap(step.MapId, 0))
    {
        map->LoadGrid(step.X, step.Y);
        if (step.Pin)
        {
            map->SetUnloadLock(Trinity::ComputeGridCoord(step.X, step.Y), true);
            // LoadGrid does not load the navmesh tile while LazyMMap is on. Pinned hubs (capitals, saved
            // player positions) pay that tile here, during warm-up, so the first player in town does not.
            if (BotPerf::BotOpts::LazyMMap())
                map->EnsureMMapTile(step.X, step.Y);
        }
        TC_LOG_INFO("server.playerbots", "Playerbots: warm-up: map {} grid at ({:.0f}, {:.0f}) loaded in {} ms{}", step.MapId, step.X, step.Y, GetMSTimeDiffToNow(start), step.Pin ? " (pinned)" : "");
    }
    if (_warmSteps.empty())
        TC_LOG_INFO("server.playerbots", "Playerbots: warm-up finished");
    return true;
}

void RandomBotMgr::BuildSpots()
{
    _spots.clear();
    _spotHostilityKnown = false;
    _allowedMaps.clear();
    uint8 expansion = uint8(sWorld->getIntConfig(CONFIG_EXPANSION));

    for (std::string_view piece : Trinity::Tokenize(sPlayerbotsConfig.RandomMaps, ',', false))
        if (Optional<uint32> mapId = Trinity::StringTo<uint32>(piece))
            _allowedMaps.insert(*mapId);

    for (auto const& [zoneId, graveyard] : sObjectMgr->GraveyardStore)
    {
        WorldSafeLocsEntry const* safeLoc = sObjectMgr->GetWorldSafeLoc(graveyard.safeLocId);
        if (!safeLoc || safeLoc->TransportSpawnId)
            continue;

        MapEntry const* map = sMapStore.LookupEntry(safeLoc->Loc.GetMapId());
        if (!map || map->Instanceable() || map->InstanceType != MAP_COMMON || map->Expansion() > expansion)
            continue;

        if (!_allowedMaps.empty() && !_allowedMaps.contains(map->ID))
            continue;

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        if (!area || area->ContentTuningID <= 0)
            continue;

        if (area->GetFlags().HasFlag(AreaFlags::OnMapDungeon) || area->GetFlags2().HasFlag(AreaFlags2::IsGarrison))
            continue;

        Optional<ContentTuningLevels> levels = sDB2Manager.GetContentTuningData(uint32(area->ContentTuningID), {});
        if (!levels || levels->MaxLevel < 1)
            continue;

        // Chromie Time / retail scaling: MinLevel is often 1 and MaxLevel is the cap, so any level can quest there
        int32 minLevel = std::max<int32>(levels->MinLevel, 1);
        int32 maxLevel = std::max<int32>(levels->MaxLevel, minLevel);
        if (minLevel > 255)
            continue;

        Spot spot;
        spot.MapId = safeLoc->Loc.GetMapId();
        spot.X = safeLoc->Loc.GetPositionX();
        spot.Y = safeLoc->Loc.GetPositionY();
        spot.Z = safeLoc->Loc.GetPositionZ();
        spot.O = safeLoc->Loc.GetOrientation();
        spot.MinLevel = uint8(minLevel);
        spot.MaxLevel = uint8(std::min<int32>(maxLevel, 255));
        if ((area->FactionGroupMask & FACTION_MASK_ALLIANCE) && !(area->FactionGroupMask & FACTION_MASK_HORDE))
            spot.Faction = ALLIANCE;
        else if ((area->FactionGroupMask & FACTION_MASK_HORDE) && !(area->FactionGroupMask & FACTION_MASK_ALLIANCE))
            spot.Faction = HORDE;

        spot.ZoneId = zoneId;

        // The graveyard's own faction is a condition now (no team column): a Horde-only graveyard in a contested zone must
        // not take Alliance bots, and a graveyard whose condition disagrees with its zone is no spot for either team
        if (uint32 graveyardTeam = SpawnPolicy::TeamOfGraveyard(graveyard.Conditions))
        {
            if (spot.Faction && spot.Faction != graveyardTeam)
                continue;
            spot.Faction = graveyardTeam;
        }

        _spots.push_back(spot);
    }
}

RandomBotMgr::BotCharacter const* RandomBotMgr::NextOfflineCharacter() const
{
    BotPerf::Scope perfNext(BotPerf::M_NEXT_OFFLINE);
    uint32 totalRatio = sPlayerbotsConfig.RandomAllianceRatio + sPlayerbotsConfig.RandomHordeRatio;
    uint32 allowedAlliance = sPlayerbotsConfig.RandomCount * sPlayerbotsConfig.RandomAllianceRatio / totalRatio;

    uint32 onlineAlliance = 0;
    BotCharacter const* alliance = nullptr;
    BotCharacter const* horde = nullptr;
    for (BotCharacter const& character : _characters)
    {
        if (sPlayerbotMgr->IsBotOnline(character.Guid))
        {
            if (character.Alliance)
                ++onlineAlliance;
            continue;
        }

        if (ObjectAccessor::FindConnectedPlayer(character.Guid))
            continue;

        if (character.Alliance)
        {
            if (!alliance)
                alliance = &character;
        }
        else if (!horde)
            horde = &character;
    }

    if (onlineAlliance < allowedAlliance && alliance)
        return alliance;

    return horde ? horde : alliance;
}

bool RandomBotMgr::WantAlliance() const
{
    uint32 alliance = 0;
    uint32 horde = 0;
    for (BotCharacter const& character : _characters)
        ++(character.Alliance ? alliance : horde);

    return alliance * sPlayerbotsConfig.RandomHordeRatio <= horde * sPlayerbotsConfig.RandomAllianceRatio;
}

RandomBotMgr::GameAccount* RandomBotMgr::FindAccount(uint32 accountId)
{
    for (GameAccount& account : _accounts)
        if (account.Id == accountId)
            return &account;

    return nullptr;
}

RandomBotMgr::GameAccount* RandomBotMgr::AccountWithRoom()
{
    for (GameAccount& account : _accounts)
        if (account.CharacterCount < CharactersPerAccountLimit())
            return &account;

    return nullptr;
}

void RandomBotMgr::LoginOne()
{
    BotPerf::Scope perfLogin(BotPerf::M_LOGIN_ONE);
    BotCharacter const* character = NextOfflineCharacter();
    GameAccount* account = character ? FindAccount(character->AccountId) : nullptr;
    if (!character || !account || !sPlayerbotMgr->AddRandomBot(account->Id, account->Name, account->BnetId, character->Guid))
        TC_LOG_DEBUG("server.playerbots", "Playerbots: random bot login skipped");

    _wait = sPlayerbotsConfig.RandomLoginDelay;
}

void RandomBotMgr::BeginCreate()
{
    if (_raceClasses.empty())
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: no playable race/class combinations for random bots");
        _wait = CreateFailurePause;
        return;
    }

    GameAccount* account = AccountWithRoom();
    if (!account && CreateGameAccount())
        account = AccountWithRoom();

    if (!account)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: random bot character creation has no game account with room");
        _wait = CreateFailurePause;
        return;
    }

    WorldSession* session = CreateBotSession(account->Id, std::string(account->Name), account->BnetId, std::string(account->Email),
        uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), "Win", Minutes(0), 0, ClientBuild::VariantId{}, LOCALE_enUS);

    _creation = std::make_unique<Creation>();
    _creation->Session = session;
    _creation->AccountId = account->Id;
}

bool RandomBotMgr::SubmitCreate()
{
    bool wantAlliance = WantAlliance();
    std::vector<RaceClass const*> options;
    for (RaceClass const& combination : _raceClasses)
        if (combination.Alliance == wantAlliance)
            options.push_back(&combination);

    if (options.empty())
        for (RaceClass const& combination : _raceClasses)
            options.push_back(&combination);

    if (options.empty() || !_creation || !_creation->Session)
        return false;

    for (uint32 attempt = 0; attempt < 8; ++attempt)
    {
        RaceClass const* combination = options[urand(0, uint32(options.size() - 1))];
        uint8 gender = urand(0, 1) ? GENDER_MALE : GENDER_FEMALE;
        std::string name = MakeName(combination->Race, gender);
        if (name.empty())
            continue;

        // a name in use is refused by the core with a packet nobody reads here: do not wait for the timeout
        std::string normalized = name;
        if (!normalizePlayerName(normalized) || sCharacterCache->GetCharacterCacheByName(normalized))
            continue;

        WorldPackets::Character::CreateCharacter packet{ WorldPacket(CMSG_CREATE_CHARACTER) };
        packet.CreateInfo = std::make_shared<WorldPackets::Character::CharacterCreateInfo>();
        packet.CreateInfo->Race = combination->Race;
        packet.CreateInfo->Class = combination->Class;
        packet.CreateInfo->Sex = gender;
        packet.CreateInfo->Name = name;
        if (!FillCustomizations(_creation->Session, combination->Race, combination->Class, gender, packet.CreateInfo->Customizations))
            continue;

        _creation->Race = combination->Race;
        _creation->Class = combination->Class;
        _creation->Alliance = combination->Alliance;
        _creation->Name = name;
        _creation->Session->HandleCharCreateOpcode(packet);
        TC_LOG_INFO("server.playerbots", "Playerbots: creating random bot {} (race {} class {})", name, uint32(combination->Race), uint32(combination->Class));
        return true;
    }

    return false;
}

void RandomBotMgr::PumpCreation(uint32 /*diff*/)
{
    if (!_creation)
        return;

    if (UpdateBotSession(_creation->Session, 0))
        _creation->Initialized = true;

    if (_creation->Initialized && !_creation->Submitted)
    {
        if (SubmitCreate())
        {
            _creation->Submitted = true;
            _creation->StartedAt = GameTime::GetGameTimeMS();
        }
        else
        {
            _creation->Finished = true;
            _creation->Code = CHAR_CREATE_ERROR;
        }
    }

    // The core answers a creation with a packet, which a session without socket never gets. The character exists once
    // the character cache knows its name; a refusal shows as the timeout
    if (_creation->Submitted && !_creation->Finished)
    {
        std::string name = _creation->Name;
        normalizePlayerName(name);
        if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByName(name))
        {
            _creation->Guid = entry->Guid;
            _creation->Code = CHAR_CREATE_SUCCESS;
            _creation->Finished = true;
        }
    }

    uint32 now = GameTime::GetGameTimeMS();
    bool timedOut = _creation->Submitted && !_creation->Finished && _creation->StartedAt && now - _creation->StartedAt > CreateTimeout;
    if (!_creation->Finished && !timedOut)
        return;

    if (!_creation->Finished && timedOut)
        TC_LOG_ERROR("server.playerbots", "Playerbots: random bot character creation timed out ({})", _creation->Name);

    if (!_creation->Code)
        _creation->Code = CHAR_CREATE_ERROR;

    FinishCreation();
}

void RandomBotMgr::FinishCreation()
{
    if (!_creation)
        return;

    if (_creation->Code == CHAR_CREATE_SUCCESS && !_creation->Guid.IsEmpty())
    {
        BotCharacter character;
        character.Guid = _creation->Guid;
        character.AccountId = _creation->AccountId;
        character.Race = _creation->Race;
        character.Class = _creation->Class;
        character.Alliance = _creation->Alliance;
        _characters.push_back(character);
        if (GameAccount* account = FindAccount(_creation->AccountId))
            ++account->CharacterCount;

        _createFailures = 0;
        TC_LOG_INFO("server.playerbots", "Playerbots: random bot character {} created", _creation->Guid.ToString());
    }
    else
    {
        if (_creation->Code == CHAR_CREATE_SERVER_LIMIT)
            if (GameAccount* account = FindAccount(_creation->AccountId))
                account->CharacterCount = CharactersPerAccountLimit();

        ++_createFailures;
        TC_LOG_ERROR("server.playerbots", "Playerbots: random bot character creation failed ({}) for {} race {} class {}",
            _creation->Code, _creation->Name.empty() ? "?" : _creation->Name, uint32(_creation->Race), uint32(_creation->Class));
    }

    delete _creation->Session;
    _creation.reset();
    // Background creation (Random.CreateUpTo) paces itself with _bgCreateTimer; sharing the login timer starved logins (each creation re-armed it just as the next began)
    _wait = _createFailures >= 8 ? CreateFailurePause : (sPlayerbotsConfig.RandomCreateUpTo ? 0 : sPlayerbotsConfig.RandomCreateDelay);
    if (_createFailures >= 8)
        _createFailures = 0;
}

bool RandomBotMgr::NeedsRandomizeFirst(Player const* bot) const
{
    if (_randomized.contains(bot->GetGUID()) || sPlayerbotRepository->GetEvent(0, bot->GetGUID(), "randomize"))
        return false;

    // A character still at its creation level has never been through RandomizeFirst.
    // AzerothCore uses the same signal (level still at the class start).
    return bot->GetLevel() < sPlayerbotsConfig.RandomMinLevel || bot->GetLevel() <= StartLevel(bot);
}

uint32 RandomBotMgr::StartLevel(Player const* bot) const
{
    uint32 start = sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL);
    switch (bot->GetClass())
    {
        case CLASS_DEATH_KNIGHT:
            start = sWorld->getIntConfig(CONFIG_START_DEATH_KNIGHT_PLAYER_LEVEL);
            break;
        case CLASS_DEMON_HUNTER:
            start = sWorld->getIntConfig(CONFIG_START_DEMON_HUNTER_PLAYER_LEVEL);
            break;
        case CLASS_EVOKER:
            start = sWorld->getIntConfig(CONFIG_START_ALLIED_RACE_LEVEL);
            break;
        default:
            if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(bot->GetRace()))
                if (race->GetFlags().HasFlag(ChrRacesFlag::IsAlliedRace))
                    start = sWorld->getIntConfig(CONFIG_START_ALLIED_RACE_LEVEL);
            break;
    }

    return std::max<uint32>(start, 1);
}

uint64 RandomBotMgr::GridKeyOf(uint32 mapId, float x, float y)
{
    constexpr float GridSize = 533.33333f;
    constexpr float MapHalf = 17066.666f;
    uint32 gridX = uint32(std::clamp(int((x + MapHalf) / GridSize), 0, 0xFFFF));
    uint32 gridY = uint32(std::clamp(int((y + MapHalf) / GridSize), 0, 0xFFFF));
    return (uint64(mapId) << 32) | (uint64(gridX) << 16) | gridY;
}

std::unordered_set<uint64> RandomBotMgr::OccupiedGrids(ObjectGuid except) const
{
    std::unordered_set<uint64> grids;
    sPlayerbotMgr->ForEachRandomBot([&](Player* other, PlayerbotAI*)
    {
        if (other->GetGUID() != except && other->IsInWorld())
            grids.insert(GridKeyOf(other->GetMapId(), other->GetPositionX(), other->GetPositionY()));
    });
    return grids;
}

RandomBotMgr::Spot const* RandomBotMgr::PickSpot(uint8 level, uint32 faction, ObjectGuid except) const
{
    struct Candidate
    {
        Spot const* Place = nullptr;
        int32 Score = 0;
    };

    // inside a town that attacks the team: needs the hub index, which is built after the spots, so it is filled on first use
    if (!_spotHostilityKnown && HubIndex::IsBuilt())
    {
        for (Spot const& spot : _spots)
        {
            spot.HostileMask = 0;
            if (SpawnPolicy::IsEnemyGround(spot.MapId, spot.ZoneId, spot.X, spot.Y, spot.Z, ALLIANCE))
                spot.HostileMask |= 1;
            if (SpawnPolicy::IsEnemyGround(spot.MapId, spot.ZoneId, spot.X, spot.Y, spot.Z, HORDE))
                spot.HostileMask |= 2;

            // A graveyard inside a city is chosen often and the bots stay close. One out in the open is rare
            // and the few bots that land there stand hundreds of yards apart.
            spot.Weight = 1;
            spot.Scatter = 480.0f;
            if (HubCluster const* cluster = HubIndex::NearestCluster(spot.MapId, Position(spot.X, spot.Y, spot.Z), 600.0f))
            {
                if (Position(spot.X, spot.Y, spot.Z).GetExactDist2d(cluster->Center) <= cluster->Radius + 40.0f)
                {
                    switch (cluster->Tier)
                    {
                        case HubTier::Capital: spot.Weight = 24; spot.Scatter = 100.0f; break;
                        case HubTier::Town:    spot.Weight = 8;  spot.Scatter = 180.0f; break;
                        case HubTier::Outpost: spot.Weight = 3;  spot.Scatter = 280.0f; break;
                        default: break;
                    }
                }
            }
        }
        uint32 capitals = 0, towns = 0, outposts = 0, desolate = 0;
        for (Spot const& spot : _spots)
        {
            if (spot.Weight >= 24)
                ++capitals;
            else if (spot.Weight >= 8)
                ++towns;
            else if (spot.Weight >= 3)
                ++outposts;
            else
                ++desolate;
        }
        TC_LOG_INFO("server.playerbots", "Playerbots: spot density, {} capital graveyards, {} towns, {} outposts, {} desolate", capitals, towns, outposts, desolate);
        _spotHostilityKnown = true;
    }

    std::vector<Candidate> candidates;
    for (Spot const& spot : _spots)
    {
        if (level < spot.MinLevel || level > spot.MaxLevel)
            continue;

        if (spot.Faction && spot.Faction != faction)
            continue;

        if (spot.HostileMask & (faction == ALLIANCE ? 1 : 2))
            continue;

        candidates.push_back({ &spot, 0 });
    }

    // grid limit reached: only spots in a grid that already has a bot (it is loaded anyway)
    if (uint32 maxGrids = sPlayerbotsConfig.RandomMaxGrids)
    {
        std::unordered_set<uint64> occupied = OccupiedGrids(except);
        if (occupied.size() >= maxGrids)
        {
            std::erase_if(candidates, [&](Candidate const& candidate)
            {
                return !occupied.contains(GridKeyOf(candidate.Place->MapId, candidate.Place->X, candidate.Place->Y));
            });
        }
    }

    if (candidates.empty())
        return nullptr;

    uint32 total = 0;
    for (Candidate const& candidate : candidates)
        total += std::max<uint32>(candidate.Place->Weight, 1);
    uint32 roll = urand(1, total);
    for (Candidate const& candidate : candidates)
    {
        uint32 weight = std::max<uint32>(candidate.Place->Weight, 1);
        if (roll <= weight)
            return candidate.Place;
        roll -= weight;
    }
    return candidates.back().Place;
}

Position RandomBotMgr::ScatterSpot(Spot const& spot, ObjectGuid except) const
{
    Position at(spot.X, spot.Y, spot.Z, frand(0.0f, 2.0f * float(M_PI)));
    if (spot.Scatter < 5.0f)
        return at;

    Map* map = sMapMgr->FindMap(spot.MapId, 0);
    if (!map)
        return at;

    bool capped = false;
    std::unordered_set<uint64> occupied;
    if (uint32 maxGrids = sPlayerbotsConfig.RandomMaxGrids)
    {
        occupied = OccupiedGrids(except);
        capped = occupied.size() >= maxGrids;
    }

    PhaseShift const& phase = PhasingHandler::GetEmptyPhaseShift();
    uint32 const homeZone = map->GetZoneId(phase, spot.X, spot.Y, spot.Z);

    for (uint32 attempt = 0; attempt < 8; ++attempt)
    {
        float dist = std::sqrt(frand(0.0f, 1.0f)) * spot.Scatter;
        float angle = frand(0.0f, 2.0f * float(M_PI));
        float x = spot.X + dist * std::cos(angle);
        float y = spot.Y + dist * std::sin(angle);
        if (!Trinity::IsValidMapCoord(x, y))
            continue;

        float z = map->GetHeight(phase, x, y, spot.Z + 5.0f, true);
        if (z <= INVALID_HEIGHT)
            continue;
        if (std::fabs(z - spot.Z) > 80.0f)
            continue;
        if (homeZone && map->GetZoneId(phase, x, y, z) != homeZone)
            continue;
        if (capped && !occupied.contains(GridKeyOf(spot.MapId, x, y)))
            continue;

        at.Relocate(x, y, z, frand(0.0f, 2.0f * float(M_PI)));
        return at;
    }
    return at;
}

void RandomBotMgr::Prepare(Player* bot)
{
    if (!bot)
        return;

    if (NeedsRandomizeFirst(bot))
        _pendingRandomize.insert(bot->GetGUID());
    else
    {
        // bots randomized before the event table existed
        if (!sPlayerbotRepository->GetEvent(0, bot->GetGUID(), "randomize"))
            sPlayerbotRepository->SetEvent(0, bot->GetGUID(), "randomize", 1);

        BotSpellBook::StripJunk(bot);
        Optional<BotEvent> placed = sPlayerbotRepository->GetEvent(0, bot->GetGUID(), "placed");
        if (!placed || placed->Value < PlacementVersion)
        {
            TC_LOG_INFO("server.playerbots", "Playerbots: placing {} on a valid spot (placement version {})", bot->GetName(), PlacementVersion);
            Place(bot);
            sPlayerbotRepository->SetEvent(0, bot->GetGUID(), "placed", PlacementVersion);
        }
        else if (bot->GetMap() && bot->GetMap()->Instanceable())
            Place(bot);
        else if (sPlayerbotsConfig.SpawnEvict && SpawnPolicy::IsEnemyGround(bot))
        {
            // an old placement, a summon or a GM left it where its own faction is not welcome
            TC_LOG_INFO("server.playerbots", "Playerbots: {} logged in on enemy ground, placing it on a valid spot", bot->GetName());
            Place(bot);
        }
    }

    bool hasHome = false;
    bool hasAnchor = false;
    {
        std::shared_lock lock(_homesLock);
        hasHome = _homes.contains(bot->GetGUID());
        hasAnchor = _anchors.contains(bot->GetGUID());
    }

    if (!hasHome)
    {
        uint32 mapId = 0;
        Position home;
        if (!LoadHome(bot->GetGUID(), mapId, home))
        {
            home = bot->GetPosition();
            mapId = bot->GetMapId();
            SaveHome(bot->GetGUID(), mapId, home);
        }

        std::unique_lock lock(_homesLock);
        _homes[bot->GetGUID()] = home;
        _homeMaps[bot->GetGUID()] = mapId;
    }

    if (!hasAnchor)
    {
        Anchor anchor;
        if (LoadAnchor(bot->GetGUID(), anchor))
        {
            std::unique_lock lock(_homesLock);
            _anchors[bot->GetGUID()] = anchor;
        }
    }
}

// db_store key "home": map;x;y;z;o
void RandomBotMgr::SaveHome(ObjectGuid guid, uint32 mapId, Position const& home)
{
    sPlayerbotRepository->SetValue(guid, "home", Trinity::StringFormat("{};{};{};{};{}", mapId, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), home.GetOrientation()));
}

bool RandomBotMgr::LoadHome(ObjectGuid guid, uint32& mapId, Position& home)
{
    Optional<std::string> saved = sPlayerbotRepository->GetValue(guid, "home");
    if (!saved)
        return false;

    std::vector<std::string_view> parts = Trinity::Tokenize(*saved, ';', false);
    if (parts.size() != 5)
        return false;

    Optional<uint32> map = Trinity::StringTo<uint32>(parts[0]);
    Optional<float> x = Trinity::StringTo<float>(parts[1]);
    Optional<float> y = Trinity::StringTo<float>(parts[2]);
    Optional<float> z = Trinity::StringTo<float>(parts[3]);
    Optional<float> o = Trinity::StringTo<float>(parts[4]);
    if (!map || !x || !y || !z || !o)
        return false;

    mapId = *map;
    home.Relocate(*x, *y, *z, *o);
    return true;
}

bool RandomBotMgr::AdvanceRandomize()
{
    BotPerf::Scope perfRandomize(BotPerf::M_RANDOMIZE);
    while (!_pendingRandomize.empty())
    {
        ObjectGuid guid = *_pendingRandomize.begin();
        _pendingRandomize.erase(_pendingRandomize.begin());

        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot || !bot->IsInWorld())
            continue;

        uint32 maxLevel = std::min(sPlayerbotsConfig.RandomMaxLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        uint32 minLevel = std::min(sPlayerbotsConfig.RandomMinLevel, maxLevel);
        uint32 start = StartLevel(bot);
        uint32 floor = std::max<uint32>(bot->GetLevel(), minLevel);
        if (maxLevel > start && floor <= start)
            floor = start + 1;
        if (floor > maxLevel)
            floor = maxLevel;

        uint8 level = RollBandLevel(floor, maxLevel);
        ApplyLevel(bot, level, true);
        _banded.insert(bot->GetGUID());
        sPlayerbotRepository->SetEvent(0, bot->GetGUID(), "popband", 1);

        BotSpellBook::StripJunk(bot);
        Place(bot);
        sPlayerbotRepository->SetEvent(0, bot->GetGUID(), "placed", PlacementVersion);
        bot->SaveToDB();
        _randomized.insert(bot->GetGUID());
        sPlayerbotRepository->SetEvent(0, bot->GetGUID(), "randomize", 1);
        TC_LOG_INFO("server.playerbots", "Playerbots: randomized {} to level {} (race {} class {})", bot->GetName(), bot->GetLevel(), uint32(bot->GetRace()), uint32(bot->GetClass()));
        return true;
    }

    return false;
}

void RandomBotMgr::Place(Player* bot)
{
    if (sPlayerbotsConfig.HotspotProb > 0.0f && roll_chance(sPlayerbotsConfig.HotspotProb * 100.0f) && PlaceAtHotspot(bot))
        return;

    ClearAnchor(bot->GetGUID());

    if (Spot const* spot = PickSpot(bot->GetLevel(), Player::TeamForRace(bot->GetRace()), bot->GetGUID()))
    {
        Position at = ScatterSpot(*spot, bot->GetGUID());
        if (bot->TeleportTo(spot->MapId, at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), at.GetOrientation()))
        {
            UpdateHome(bot->GetGUID(), spot->MapId, at);
            sPlayerbotRepository->Remember(bot->GetGUID(), "place", std::to_string(spot->MapId), Trinity::StringFormat("{:.0f} {:.0f}", at.GetPositionX(), at.GetPositionY()));
        }
        return;
    }

    // No spot for this level: the start area of its race is always valid ground
    if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(bot->GetRace(), bot->GetClass()))
    {
        WorldLocation const& start = info->createPosition.Loc;
        if (bot->TeleportTo(start))
            UpdateHome(bot->GetGUID(), start.GetMapId(), start);
    }
}

bool RandomBotMgr::PlaceAtHotspot(Player* bot)
{
    std::optional<HotspotMgr::Destination> destination = sHotspotMgr->PickFor(bot);
    if (!destination)
        return false;

    HotspotPoint const& point = destination->Point;
    Hotspot const& hotspot = destination->Spot;

    // grid limit reached: a hotspot in a grid without bots would load a new one
    if (uint32 maxGrids = sPlayerbotsConfig.RandomMaxGrids)
    {
        std::unordered_set<uint64> occupied = OccupiedGrids(bot->GetGUID());
        if (occupied.size() >= maxGrids && !occupied.contains(GridKeyOf(hotspot.Map, point.X, point.Y)))
            return false;
    }

    // the same predicate as the eviction pass: a hotspot on ground the bot's team may not stand on is no hotspot for it
    if (SpawnPolicy::IsEnemyGroundAt(bot, hotspot.Map, Position(point.X, point.Y, point.Z)))
        return false;

    // a few yards off the shared point, so a crowd at one inn is not one model
    float orientation = frand(0.0f, 2.0f * float(M_PI));
    float x = point.X;
    float y = point.Y;
    float z = point.Z;
    float jitter = frand(4.0f, 16.0f);
    float angle = frand(0.0f, 2.0f * float(M_PI));
    float jx = point.X + jitter * std::cos(angle);
    float jy = point.Y + jitter * std::sin(angle);
    if (Map* map = sMapMgr->FindMap(hotspot.Map, 0))
    {
        float jz = map->GetHeight(PhasingHandler::GetEmptyPhaseShift(), jx, jy, point.Z + 5.0f, true);
        if (jz > INVALID_HEIGHT && std::fabs(jz - point.Z) <= 8.0f)
        {
            x = jx;
            y = jy;
            z = jz;
        }
    }
    if (!bot->TeleportTo(hotspot.Map, x, y, z, orientation))
        return false;

    sPlayerbotRepository->Remember(bot->GetGUID(), "place", std::to_string(hotspot.Map), Trinity::StringFormat("{:.0f} {:.0f}", x, y));

    ObjectGuid guid = bot->GetGUID();
    UpdateHome(guid, hotspot.Map, Position(x, y, z, orientation));

    Anchor anchor;
    anchor.MapId = hotspot.Map;
    // the bot stays around where it arrived (the auction house, the bank), not around the middle of the town
    anchor.Center = Position(x, y, z);
    anchor.Radius = sPlayerbotsConfig.HotspotWanderRadius;
    SetAnchor(guid, anchor);
    return true;
}

// db_store key "anchor": map;x;y;z;radius
void RandomBotMgr::SetAnchor(ObjectGuid guid, Anchor const& anchor)
{
    {
        std::unique_lock lock(_homesLock);
        _anchors[guid] = anchor;
    }
    sPlayerbotRepository->SetValue(guid, "anchor", Trinity::StringFormat("{};{};{};{};{}", anchor.MapId, anchor.Center.GetPositionX(),
        anchor.Center.GetPositionY(), anchor.Center.GetPositionZ(), anchor.Radius));
}

bool RandomBotMgr::LoadAnchor(ObjectGuid guid, Anchor& anchor)
{
    Optional<std::string> saved = sPlayerbotRepository->GetValue(guid, "anchor");
    if (!saved)
        return false;

    std::vector<std::string_view> parts = Trinity::Tokenize(*saved, ';', false);
    if (parts.size() != 5)
        return false;

    Optional<uint32> map = Trinity::StringTo<uint32>(parts[0]);
    Optional<float> x = Trinity::StringTo<float>(parts[1]);
    Optional<float> y = Trinity::StringTo<float>(parts[2]);
    Optional<float> z = Trinity::StringTo<float>(parts[3]);
    Optional<float> radius = Trinity::StringTo<float>(parts[4]);
    if (!map || !x || !y || !z || !radius)
        return false;

    anchor.MapId = *map;
    anchor.Center = Position(*x, *y, *z);
    anchor.Radius = *radius;
    return true;
}

void RandomBotMgr::ClearAnchor(ObjectGuid guid)
{
    bool erased = false;
    {
        std::unique_lock lock(_homesLock);
        erased = _anchors.erase(guid) != 0;
    }

    if (erased)
        sPlayerbotRepository->EraseValue(guid, "anchor");
}

Optional<RandomBotMgr::Anchor> RandomBotMgr::GetAnchor(ObjectGuid guid) const
{
    std::shared_lock lock(_homesLock);
    auto itr = _anchors.find(guid);
    if (itr == _anchors.end())
        return {};
    return itr->second;
}

uint8 RandomBotMgr::RollBandLevel(uint32 floor, uint32 cap) const
{
    struct Band
    {
        uint8 Min;
        uint8 Max;
    };

    // the continents these bots already use, then the cap as its own band
    static constexpr Band Bands[] =
    {
        { 1, 19 }, { 20, 39 }, { 40, 59 }, { 60, 69 }, { 70, 79 }, { 80, 84 }, { 85, 89 }, { 90, 90 }
    };

    struct Choice
    {
        uint8 Min;
        uint8 Max;
        uint32 Weight;
    };

    Choice choices[std::size(Bands)];
    uint32 count = 0;
    uint32 sum = 0;
    uint32 loFloor = std::min<uint32>(floor, 255);
    uint32 hiCap = std::min<uint32>(cap, 255);
    for (size_t i = 0; i < std::size(Bands); ++i)
    {
        uint8 lo = std::max<uint8>(Bands[i].Min, uint8(loFloor));
        uint8 hi = std::min<uint8>(Bands[i].Max, uint8(hiCap));
        if (lo > hi)
            continue;

        uint32 weight = sPlayerbotsConfig.PopulationWeight[i];
        if (!weight)
            continue;

        choices[count++] = { lo, hi, weight };
        sum += weight;
    }

    if (!sum || loFloor > hiCap)
        return uint8(std::clamp(floor, uint32(1), std::max<uint32>(cap, 1)));

    uint32 roll = urand(1, sum);
    uint32 acc = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        acc += choices[i].Weight;
        if (acc >= roll)
            return uint8(urand(choices[i].Min, choices[i].Max));
    }

    return uint8(hiCap);
}

void RandomBotMgr::ApplyLevel(Player* bot, uint8 level, bool pickSpec)
{
    if (bot->GetLevel() != level)
        bot->GiveLevel(level);

    bot->SetXP(0);
    if (bot->GetLevel() < MIN_SPECIALIZATION_LEVEL)
        return;

    if (pickSpec)
    {
        std::vector<ChrSpecializationEntry const*> specs;
        for (uint32 index = 0; index < 4; ++index)
            if (ChrSpecializationEntry const* spec = sDB2Manager.GetChrSpecializationByIndex(bot->GetClass(), index))
                if (!spec->IsPetSpecialization())
                    specs.push_back(spec);

        if (!specs.empty())
            bot->ActivateTalentGroup(specs[urand(0, uint32(specs.size() - 1))]);
    }

    bot->InitTalentForLevel();
    if (pickSpec)
        BotTalents::ApplyStarterBuild(bot);
}

namespace
{
bool LeftAlone(Player* bot, PlayerbotAI* ai)
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->GetGroup() || bot->InBattleground()
        || bot->InBattlegroundQueue() || bot->isUsingLfg() || ai->GetMaster() || !bot->GetMap() || bot->GetMap()->Instanceable()
        || bot->IsInFlight() || ai->GetRoute().IsActive())
        return true;

    return sPlayerbotMgr->HasHumanWithin(bot, sPlayerbotsConfig.BotActiveAloneRadius);
}
}

void RandomBotMgr::MaintainPopulation()
{
    uint32 nowSec = uint32(GameTime::GetGameTime());
    uint32 nowMs = GameTime::GetGameTimeMS();
    uint32 banded = 0;
    uint32 moved = 0;
    uint32 revived = 0;

    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI* ai)
    {
        ObjectGuid guid = bot->GetGUID();

        if (!bot->IsAlive())
        {
            if (bot->IsBeingTeleported() || bot->GetGroup() || bot->InBattleground() || bot->InBattlegroundQueue()
                || ai->GetMaster() || !bot->GetMap() || bot->GetMap()->Instanceable()
                || sPlayerbotMgr->HasHumanWithin(bot, sPlayerbotsConfig.BotActiveAloneRadius))
            {
                // a player is standing on the corpse: do not pull it out from under them
                _reviveAt.erase(guid);
                return;
            }

            if (revived >= 4)
                return;

            auto itr = _reviveAt.find(guid);
            if (itr == _reviveAt.end())
            {
                _reviveAt[guid] = nowMs + urand(sPlayerbotsConfig.PopulationReviveMin, sPlayerbotsConfig.PopulationReviveMax) * 1000;
                return;
            }
            if (nowMs < itr->second)
                return;

            bot->ResurrectPlayer(0.5f);
            Place(bot);
            _reviveAt.erase(guid);
            ++revived;
            TC_LOG_INFO("server.playerbots", "Playerbots: revived {} and placed it (nobody was near the corpse)", bot->GetName());
            return;
        }

        _reviveAt.erase(guid);
        if (LeftAlone(bot, ai))
            return;

        if (!_banded.contains(guid))
        {
            if (sPlayerbotRepository->GetEvent(0, guid, "popband"))
            {
                _banded.insert(guid);
            }
            else if (banded < 2)
            {
                uint32 maxLevel = std::min(sPlayerbotsConfig.RandomMaxLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
                uint32 minLevel = std::min(sPlayerbotsConfig.RandomMinLevel, maxLevel);
                uint32 start = StartLevel(bot);
                uint32 floor = std::max<uint32>(minLevel, start);
                if (maxLevel > start && floor <= start)
                    floor = start + 1;
                if (floor > maxLevel)
                    floor = maxLevel;

                ApplyLevel(bot, RollBandLevel(floor, maxLevel), false);
                Place(bot);
                bot->SaveToDB();
                sPlayerbotRepository->SetEvent(0, guid, "popband", 1);
                _banded.insert(guid);
                _nextPopulationAt[guid] = nowSec + urand(sPlayerbotsConfig.PopulationTeleportMin, sPlayerbotsConfig.PopulationTeleportMax);
                ++banded;
                return;
            }
        }

        if (moved >= 2)
            return;

        auto due = _nextPopulationAt.find(guid);
        if (due == _nextPopulationAt.end())
        {
            _nextPopulationAt[guid] = nowSec + urand(sPlayerbotsConfig.PopulationTeleportMin, sPlayerbotsConfig.PopulationTeleportMax);
            return;
        }
        if (nowSec < due->second)
            return;

        uint32 maxLevel = std::min(sPlayerbotsConfig.RandomMaxLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        if (bot->GetLevel() < maxLevel && roll_chance(sPlayerbotsConfig.PopulationLevelUpChance))
            ApplyLevel(bot, uint8(bot->GetLevel() + 1), false);

        Place(bot);
        due->second = nowSec + urand(sPlayerbotsConfig.PopulationTeleportMin, sPlayerbotsConfig.PopulationTeleportMax);
        ++moved;
    });
}

// About once per RotateMinutes each idle bot gets a new roll: a hotspot, or back to the open world if it was
// anchored. Bots someone can see, bots with a job (quest, flight) and grouped bots are left alone.
void RandomBotMgr::EvictFromEnemyGround()
{
    if (!sPlayerbotsConfig.SpawnEvict)
        return;

    std::vector<Player*> evict;
    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI* ai)
    {
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->GetGroup() || bot->InBattleground()
            || bot->InBattlegroundQueue() || bot->isUsingLfg() || ai->GetMaster() || !bot->GetMap() || bot->GetMap()->Instanceable()
            || bot->IsInFlight() || ai->GetRoute().IsActive())
            return;

        if (SpawnPolicy::IsEnemyGround(bot))
            evict.push_back(bot);
    });

    uint32 now = GameTime::GetGameTimeMS();
    for (Player* bot : evict)
    {
        // placement and eviction use the same predicate; a bot evicted twice within 10 minutes means they disagree somewhere
        auto last = _evictedAt.find(bot->GetGUID());
        if (last != _evictedAt.end() && now - last->second < 10 * 60 * 1000)
        {
            TC_LOG_WARN("server.playerbots", "Playerbots: {} was evicted from enemy ground twice within 10 minutes (map {}, {:.0f}, {:.0f}), leaving it",
                bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
            continue;
        }

        _evictedAt[bot->GetGUID()] = now;
        if (Optional<Position> side = SpawnPolicy::SafeDistrict(bot))
        {
            TC_LOG_INFO("server.playerbots", "Playerbots: {} stands in the other faction's district (map {}, {:.0f}, {:.0f}), moving it to its own side",
                bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
            if (bot->TeleportTo(bot->GetMapId(), side->GetPositionX(), side->GetPositionY(), side->GetPositionZ(), side->GetOrientation()))
                UpdateHome(bot->GetGUID(), bot->GetMapId(), *side);
            continue;
        }

        TC_LOG_INFO("server.playerbots", "Playerbots: {} stands on enemy ground (map {}, {:.0f}, {:.0f}), placing it on a valid spot",
            bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
        Place(bot);
    }
}

void RandomBotMgr::RotateBots()
{
    EvictFromEnemyGround();

    BotPerf::Scope perfRotate(BotPerf::M_ROTATE);
    uint32 minutes = sPlayerbotsConfig.HotspotRotateMinutes;
    if (!minutes || sPlayerbotsConfig.HotspotProb <= 0.0f || !sPlayerbotMgr->GetRandomBotCount())
        return;

    std::vector<Player*> due;
    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI* ai)
    {
        if (urand(1, minutes) != 1)
            return;

        RpgStatus status = ai->GetRpg().Status;
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->GetGroup() || bot->InBattleground()
            || bot->InBattlegroundQueue() || bot->isUsingLfg() || ai->GetMaster() || !bot->GetMap() || bot->GetMap()->Instanceable()
            || status == RpgStatus::DoQuest || status == RpgStatus::TravelFlight || bot->IsInFlight())
            return;

        if (sPlayerbotMgr->HasHumanWithin(bot, RotateObservedDistance))
            return;

        due.push_back(bot);
    });

    uint32 hotspots = 0;
    uint32 released = 0;
    for (Player* bot : due)
    {
        if (roll_chance(sPlayerbotsConfig.HotspotProb * 100.0f))
        {
            if (PlaceAtHotspot(bot))
                ++hotspots;
        }
        else if (GetAnchor(bot->GetGUID()))
        {
            Place(bot);   // its hotspot roll fails again most of the time, then it is an ordinary open world spot
            ++released;
        }
    }

    if (hotspots || released)
        TC_LOG_DEBUG("server.playerbots", "Playerbots: hotspot rotation, {} bots to hotspots, {} back to the open world", hotspots, released);
}

void RandomBotMgr::UpdateHome(ObjectGuid guid, uint32 mapId, Position const& home)
{
    {
        std::unique_lock lock(_homesLock);
        _homes[guid] = home;
        _homeMaps[guid] = mapId;
    }
    SaveHome(guid, mapId, home);
}

bool RandomBotMgr::HasHumanOnline() const
{
    return sPlayerbotMgr->HasRealPlayerOnline();
}

bool RandomBotMgr::GetHomeLocation(ObjectGuid guid, uint32& mapId, Position& position) const
{
    std::shared_lock lock(_homesLock);
    auto home = _homes.find(guid);
    auto map = _homeMaps.find(guid);
    if (home == _homes.end() || map == _homeMaps.end())
        return false;

    mapId = map->second;
    position = home->second;
    return true;
}

Optional<Position> RandomBotMgr::GetHome(ObjectGuid guid) const
{
    std::shared_lock lock(_homesLock);
    auto itr = _homes.find(guid);
    if (itr == _homes.end())
        return {};
    return itr->second;
}
}
