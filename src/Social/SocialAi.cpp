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

#include "Social/SocialAi.h"
#include "BotSession.h"
#include "Channel.h"
#include "Cmd/ChatCommands.h"
#include "Cmd/ChatParser.h"
#include "Combat/GroupCombat.h"
#include "DB2Stores.h"
#include "Engine/AiRegistry.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ScriptMgr.h"
#include "Social/SocialConfig.h"
#include "Social/SocialShared.h"
#include "World.h"

namespace Playerbots::Social
{
namespace
{
class SocialCommandAction final : public Action
{
public:
    SocialCommandAction(std::string name, SocialCommandFn fn) : _name(std::move(name)), _fn(fn) { }

    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI& ai) override { return _fn(ai, GetParam()); }

private:
    std::string _name;
    SocialCommandFn _fn;
};

// Priorities next to the generic ones (Strategies/GenericStrategies.cpp: Attack 60, Travel 55, Loot 50, Buff 48, World 40,
// Pvp 30, Idle 10). The fights a duel or War Mode starts are fought by the "combat" strategy once the target is set.
constexpr float DuelAnswerPriority = 41.0f;     // above "world react", which only accepts in capitals
constexpr float WarModePriority = 42.0f;
constexpr float TradeFillPriority = 49.0f;     // the master is waiting at the trade window
constexpr float RefreshmentPriority = 47.0f;
constexpr float BankPriority = 20.0f;
constexpr float GuildBankPriority = 19.0f;
constexpr float GiftPriority = 18.0f;
constexpr float InvitePriority = 15.0f;
constexpr float DuelChallengePriority = 14.0f;
constexpr float ChannelPriority = 12.0f;

// One strategy for every bot: each node's trigger checks its own Playerbots.Social.* key and who the bot is
class SocialStrategy final : public Strategy
{
public:
    SocialStrategy() : Strategy("social") { }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        uint8 const nc = StrategyStates::NonCombat;
        triggers.push_back({ "social duel challenged", { { "social answer duel", DuelAnswerPriority } }, nc });
        triggers.push_back({ "social war mode enemy", { { "social war mode fight", WarModePriority } }, nc });
        triggers.push_back({ "social war mode due", { { "social war mode set", WarModePriority - 0.5f } }, nc });
        triggers.push_back({ "social trade fill due", { { "social trade fill", TradeFillPriority } }, nc });
        triggers.push_back({ "social refreshments due", { { "social refreshments", RefreshmentPriority } }, nc });
        triggers.push_back({ "social bank due", { { "social bank", BankPriority } }, nc });
        triggers.push_back({ "social guild bank due", { { "social guild bank", GuildBankPriority } }, nc });
        triggers.push_back({ "social gift due", { { "social gift", GiftPriority } }, nc });
        triggers.push_back({ "social invite due", { { "social invite", InvitePriority } }, nc });
        triggers.push_back({ "social duel due", { { "social challenge", DuelChallengePriority } }, nc });
        triggers.push_back({ "social channel due", { { "social channel post", ChannelPriority } }, nc });
    }
};
}

void AddSocialCommand(char const* name, char const* help, SocialCommandFn fn, bool whisperOnly)
{
    std::string action = std::string("cmd ") + name;
    sPlayerbotsAi.AddAction(action, [action, fn](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<SocialCommandAction>(action, fn); });

    ChatCommandInfo info;
    info.Action = action;
    info.Help = help;
    info.Persist = false;
    info.WhisperOnly = whisperOnly;
    GetChatCommands().Add(name, info);
}

bool InCapital(Player const* player)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId());
    return zone && zone->GetFlags().HasFlag(AreaFlags::LinkedChat);
}

bool IsHuman(Player const* player)
{
    return player && player->IsInWorld() && !IsBot(player);
}

std::string RoleWord(Player const* player)
{
    switch (GroupCombat::SpecRole(player))
    {
        case BotRole::Tank: return "tank";
        case BotRole::Healer: return "healer";
        default: return "dps";
    }
}

std::string ZoneName(Player const* player)
{
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId()))
        if (char const* name = zone->AreaName[sWorld->GetDefaultDbcLocale()]; name && *name)
            return name;
    return "somewhere";
}

bool IsFreeRandomBot(PlayerbotAI& ai)
{
    return ai.GetMasterGuid().IsEmpty();
}

void RegisterSocialAi(AiRegistry& registry)
{
    RegisterChannels(registry);
    RegisterInvites(registry);
    RegisterDuels(registry);
    RegisterWarMode(registry);
    RegisterTrades(registry);
    RegisterBanking(registry);
    RegisterMailOrders(registry);
    RegisterMageRefreshments(registry);
    registry.AddStrategy<SocialStrategy>("social");
}
}

namespace
{
class playerbots_social_world : public WorldScript
{
public:
    playerbots_social_world() : WorldScript("playerbots_social_world")
    {
        // OnConfigLoad(false) runs before scripts are registered
        sSocialConfig.Load();
    }

    void OnConfigLoad(bool /*reload*/) override
    {
        sSocialConfig.Load();
    }

    // after the map updates, in the world thread: the delayed channel answers
    void OnUpdate(uint32 diff) override
    {
        Playerbots::Social::UpdateChannels(diff);
    }
};

class playerbots_social_player : public PlayerScript
{
public:
    playerbots_social_player() : PlayerScript("playerbots_social_player") { }

    void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        if (channel && Playerbots::Social::IsHuman(player))
            Playerbots::Social::OnPlayerChannelChat(player, msg, channel);
    }

    void OnDuelStart(Player* player1, Player* player2) override
    {
        Playerbots::Social::OnDuelStarted(player1, player2);
    }

    void OnDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        Playerbots::Social::OnDuelEnded(winner, loser, type);
    }
};

class playerbots_social_group : public GroupScript
{
public:
    playerbots_social_group() : GroupScript("playerbots_social_group") { }

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        Playerbots::Social::OnGroupMemberAdded(group, guid);
    }
};
}

void AddSC_playerbots_social()
{
    new playerbots_social_world();
    new playerbots_social_player();
    new playerbots_social_group();
}
