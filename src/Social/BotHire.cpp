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

#include "BotHire.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Db/PlayerbotRepository.h"
#include "GameTime.h"
#include "Goal/GoalStore.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "RandomBotMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
constexpr uint32 PollMs = 1000;
constexpr uint32 GiveUpMs = 60000;   // a bot that is still online after this long is not moved
}

BotHire* BotHire::instance()
{
    static BotHire instance;
    return &instance;
}

/// AzerothCore's price: (1 + level / 10) x 5000 x level copper, scaled by Hire.CostPercent (about 210g at level 60)
uint64 BotHire::PriceFor(uint32 level)
{
    uint64 base = uint64(1 + level / 10) * 5000 * level;
    return base * sPlayerbotsConfig.HireCostPercent / 100;
}

std::string BotHire::Check(Player* master, Player* bot, uint64& cost) const
{
    cost = 0;
    if (!sPlayerbotsConfig.HireEnable)
        return "Hiring is switched off.";

    if (!sRandomBotMgr->IsBotCharacter(bot->GetGUID()))
        return "I already belong to someone.";

    if (std::ranges::any_of(_pending, [bot](Pending const& pending) { return pending.Bot == bot->GetGUID(); }))
        return "I'm already on my way to you.";

    if (bot->GetLevel() > master->GetLevel())
        return "You cannot hire a character of a higher level than yours.";

    if (!bot->IsAlive() || bot->IsInCombat())
        return "Not right now.";

    if (Guild const* guild = sGuildMgr->GetGuildById(bot->GetGuildId()); guild && guild->GetLeaderGUID() == bot->GetGUID())
        return "I lead a guild, I cannot leave it.";

    uint32 account = master->GetSession()->GetAccountId();
    if (AccountMgr::GetCharactersCount(account) >= sWorld->getIntConfig(CONFIG_CHARACTERS_PER_REALM))
        return "You have no room for another character on your account.";

    cost = PriceFor(bot->GetLevel());
    if (!master->HasEnoughMoney(int64(cost)))
        return Trinity::StringFormat("I'd join you for {}g {}s {}c, you do not have that much.", cost / 10000, (cost / 100) % 100, cost % 100);

    return { };
}

bool BotHire::Start(Player* master, Player* bot, uint64 cost)
{
    Pending pending;
    pending.Bot = bot->GetGUID();
    pending.Master = master->GetGUID();
    pending.MasterAccount = master->GetSession()->GetAccountId();
    pending.Cost = cost;
    pending.StartedAt = GameTime::GetGameTimeMS();
    pending.Name = bot->GetName();

    if (!sRandomBotMgr->Hold(pending.Bot))
        return false;

    master->ModifyMoney(-int64(cost));
    _pending.push_back(std::move(pending));
    sPlayerbotMgr->RemoveBot(bot->GetGUID());
    TC_LOG_INFO("server.playerbots", "Playerbots: {} hires {} for {} copper", master->GetName(), _pending.back().Name, cost);
    return true;
}

void BotHire::Update(uint32 diff)
{
    if (_pending.empty())
        return;

    if (_timer > diff)
    {
        _timer -= diff;
        return;
    }
    _timer = PollMs;

    uint32 now = GameTime::GetGameTimeMS();
    std::vector<Pending> pending = std::move(_pending);
    _pending.clear();
    for (Pending& entry : pending)
    {
        if (!ObjectAccessor::FindConnectedPlayer(entry.Bot) && !sPlayerbotMgr->IsBotOnline(entry.Bot))
            Finish(entry);
        else if (now - entry.StartedAt > GiveUpMs)
            Fail(entry, "the character did not log out");
        else
            _pending.push_back(std::move(entry));
    }
}

void BotHire::Finish(Pending const& pending)
{
    CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByGuid(pending.Bot);
    if (!cached)
    {
        Fail(pending, "the character is not in the cache");
        return;
    }

    uint32 oldAccount = cached->AccountId;
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ACCOUNT_BY_GUID);
    stmt->setUInt32(0, pending.MasterAccount);
    stmt->setUInt64(1, pending.Bot.GetCounter());
    CharacterDatabase.DirectExecute(stmt);

    sWorld->UpdateRealmCharCount(oldAccount);
    sWorld->UpdateRealmCharCount(pending.MasterAccount);
    sCharacterCache->UpdateCharacterAccountId(pending.Bot, pending.MasterAccount);

    // what the random bot machinery knows about it
    sRandomBotMgr->Forget(pending.Bot);
    GoalStore::Forget(pending.Bot);
    sPlayerbotRepository->ForgetBot(pending.Bot);

    TC_LOG_INFO("server.playerbots", "Playerbots: {} moved from account {} to account {}", pending.Name, oldAccount, pending.MasterAccount);

    if (Player* master = ObjectAccessor::FindConnectedPlayer(pending.Master))
        ChatHandler(master->GetSession()).SendSysMessage(Trinity::StringFormat("{} is now your character. Log in with them after you log out, or use .bot add {}.", pending.Name, pending.Name).c_str());
}

void BotHire::Fail(Pending const& pending, char const* why)
{
    TC_LOG_ERROR("server.playerbots", "Playerbots: hire of {} failed: {}, the price is given back", pending.Name, why);
    sRandomBotMgr->Restore(pending.Bot);

    if (Player* master = ObjectAccessor::FindConnectedPlayer(pending.Master))
    {
        master->ModifyMoney(int64(pending.Cost));
        ChatHandler(master->GetSession()).SendSysMessage(Trinity::StringFormat("The hire of {} did not work, your gold is back.", pending.Name).c_str());
    }
    else
    {
        // the master logged out: the gold goes back through the database
        CharacterDatabase.PExecute("UPDATE characters SET money = money + {} WHERE guid = {}", pending.Cost, pending.Master.GetCounter());
        TC_LOG_ERROR("server.playerbots", "Playerbots: {} copper given back to the offline character {}", pending.Cost, pending.Master.ToString());
    }
}
}
