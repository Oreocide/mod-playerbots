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

#include "BotGuildMgr.h"
#include "Social/GuildNames.h"
#include "CharacterCache.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "RandomBotMgr.h"
#include "StringFormat.h"
#include "Random.h"
#include <algorithm>
#include <map>
#include <random>
#include <vector>

namespace Playerbots
{
namespace
{
constexpr uint32 Founders = 5;           // leader plus four, like the signatures of a charter
constexpr uint32 JoinsPerRun = 3;
constexpr uint32 NameTries = 20;

struct Candidate
{
    ObjectGuid Guid;
    uint32 Team = 0;
};

/// A random bot that may be asked into a guild: alone, alive, out of instances, not under a player's control
bool Eligible(Player* bot, PlayerbotAI* ai)
{
    return bot->IsInWorld() && bot->IsAlive() && bot->GetLevel() >= sPlayerbotsConfig.GuildMinLevel && !bot->GetGroup()
        && !bot->InBattleground() && !bot->InBattlegroundQueue() && !bot->isUsingLfg() && !bot->IsBeingTeleported()
        && !ai->GetMaster() && bot->GetMap() && !bot->GetMap()->Instanceable();
}

bool IsBotGuild(Guild const* guild)
{
    return guild && sRandomBotMgr->IsBotCharacter(guild->GetLeaderGUID());
}

std::string FreeName()
{
    for (uint32 attempt = 0; attempt < NameTries; ++attempt)
    {
        std::string name = MakeGuildName(urand(0, 1000000), urand(0, 1000000), urand(0, 2));
        if (!sGuildMgr->GetGuildByName(name) && !sObjectMgr->IsReservedName(name) && ObjectMgr::IsValidCharterName(name))
            return name;
    }
    return { };
}
}

BotGuildMgr* BotGuildMgr::instance()
{
    static BotGuildMgr instance;
    return &instance;
}

void BotGuildMgr::Update(uint32 diff)
{
    if (!sPlayerbotsConfig.GuildEnable)
        return;

    if (_timer > diff)
    {
        _timer -= diff;
        return;
    }
    _timer = sPlayerbotsConfig.GuildInterval;

    Run();
}

std::vector<std::string> BotGuildMgr::List() const
{
    std::vector<std::string> lines;
    for (auto const& [id, guild] : sGuildMgr->GetGuildStore())
        if (IsBotGuild(guild.get()))
            lines.push_back(Trinity::StringFormat("{} ({})", guild->GetName(), guild->GetMembersCount()));
    std::ranges::sort(lines);
    return lines;
}

uint32 BotGuildMgr::Purge()
{
    // Disband removes the guild from the store: collect the ids first
    std::vector<ObjectGuid::LowType> ids;
    for (auto const& [id, guild] : sGuildMgr->GetGuildStore())
        if (IsBotGuild(guild.get()))
            ids.push_back(id);

    uint32 disbanded = 0;
    for (ObjectGuid::LowType id : ids)
        if (Guild* guild = sGuildMgr->GetGuildById(id))
        {
            guild->Disband();
            ++disbanded;
        }
    return disbanded;
}

void BotGuildMgr::Run()
{
    std::vector<Candidate> free;
    uint32 eligible = 0;
    uint32 inBotGuilds = 0;
    sPlayerbotMgr->ForEachRandomBot([&](Player* bot, PlayerbotAI* ai)
    {
        if (!Eligible(bot, ai))
            return;

        ++eligible;
        if (!bot->GetGuildId())
            free.push_back({ bot->GetGUID(), uint32(Player::TeamForRace(bot->GetRace())) });
        else if (IsBotGuild(sGuildMgr->GetGuildById(bot->GetGuildId())))
            ++inBotGuilds;
    });

    if (!eligible || inBotGuilds * 100 >= eligible * sPlayerbotsConfig.GuildPercent || free.empty())
        return;

    std::ranges::shuffle(free, std::mt19937(urand(0, 0x7FFFFFFF)));

    // first the bot guilds with room: a few new members of the guild's own team
    uint32 joined = 0;
    for (auto const& [id, guildPtr] : sGuildMgr->GetGuildStore())
    {
        Guild* guild = guildPtr.get();
        if (joined >= JoinsPerRun)
            break;
        if (!IsBotGuild(guild) || guild->GetMembersCount() >= sPlayerbotsConfig.GuildMaxMembers)
            continue;

        // the leader is usually offline: the character cache knows the race
        CharacterCacheEntry const* leader = sCharacterCache->GetCharacterCacheByGuid(guild->GetLeaderGUID());
        uint32 team = leader ? uint32(Player::TeamForRace(leader->Race)) : 0;
        if (team != ALLIANCE && team != HORDE)
            continue;

        auto itr = std::ranges::find_if(free, [team](Candidate const& c) { return c.Team == team; });
        if (itr == free.end())
            continue;

        CharacterDatabaseTransaction trans(nullptr);
        if (guild->AddMember(trans, itr->Guid))
            ++joined;
        free.erase(itr);
    }

    if (joined)
    {
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} bots joined bot guilds ({} of {} eligible bots are in one)", joined, inBotGuilds, eligible);
        return;
    }

    // none had room: found one, four bots of the leader's team sign on
    std::map<uint32, std::vector<Candidate>> byTeam;
    for (Candidate const& candidate : free)
        byTeam[candidate.Team].push_back(candidate);

    for (auto& [team, pool] : byTeam)
    {
        // a neutral race (a starting Pandaren) cannot found a guild
        if ((team != ALLIANCE && team != HORDE) || pool.size() < Founders)
            continue;

        Player* leader = ObjectAccessor::FindConnectedPlayer(pool.front().Guid);
        std::string name = FreeName();
        if (!leader || name.empty())
            return;

        Guild* guild = new Guild;
        if (!guild->Create(leader, name))
        {
            delete guild;
            return;
        }
        sGuildMgr->AddGuild(guild);

        uint32 members = 0;
        for (uint32 i = 1; i < Founders; ++i)
        {
            CharacterDatabaseTransaction trans(nullptr);
            if (guild->AddMember(trans, pool[i].Guid))
                ++members;
        }

        TC_LOG_INFO("server.playerbots", "Playerbots: {} founded the guild \"{}\" with {} other bots", leader->GetName(), name, members);
        return;
    }
}
}
