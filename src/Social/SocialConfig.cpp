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

#include "SocialConfig.h"
#include "Config.h"
#include <algorithm>

namespace Playerbots::Social
{
Config& GetConfig()
{
    static Config config;
    return config;
}

void Config::Load()
{
    auto u32 = [](char const* key, uint32 def) { return uint32(std::max(0, sConfigMgr->GetIntDefault(key, int32(def)))); };
    auto pct = [&](char const* key, uint32 def) { return std::min<uint32>(100, u32(key, def)); };

    ChannelEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.Channel.Enable", false);
    ChannelCheckMs = std::max<uint32>(500, u32("Playerbots.Social.Channel.CheckMs", 2000));
    ChannelOneIn = std::max<uint32>(1, u32("Playerbots.Social.Channel.OneIn", 5));
    ChannelMinGapSec = u32("Playerbots.Social.Channel.MinGapSec", 30);
    ChannelExtraGapSec = u32("Playerbots.Social.Channel.ExtraGapSec", 60);
    ChanceQuest = std::min<uint32>(30000, u32("Playerbots.Social.Channel.ChanceQuest", 10000));
    ChanceSomething = std::min<uint32>(30000, u32("Playerbots.Social.Channel.ChanceSomething", 30000));
    ChanceInstance = std::min<uint32>(30000, u32("Playerbots.Social.Channel.ChanceInstance", 5000));
    ChanceSell = std::min<uint32>(30000, u32("Playerbots.Social.Channel.ChanceSell", 300));
    ChannelDungeons = sConfigMgr->GetBoolDefault("Playerbots.Social.Channel.Dungeons", true);
    ZoneMinGapMs = u32("Playerbots.Social.Channel.ZoneMinGapMs", 20000);   // user 11-13: one line per 20 s per zone
    ReplyEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.Channel.Replies", false);
    ReplyMaxBots = u32("Playerbots.Social.Channel.ReplyMaxBots", 0);
    ReplyMinDelayMs = u32("Playerbots.Social.Channel.ReplyMinDelayMs", 2000);
    ReplyMaxDelayMs = std::max(ReplyMinDelayMs, u32("Playerbots.Social.Channel.ReplyMaxDelayMs", 8000));

    InviteEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.Invite.Enable", false);
    InviteCheckMs = std::max<uint32>(500, u32("Playerbots.Social.Invite.CheckMs", 2000));
    InviteOneIn = std::max<uint32>(1, u32("Playerbots.Social.Invite.OneIn", 5));
    InviteLevelGap = u32("Playerbots.Social.Invite.LevelGap", 2);
    InviteRange = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Social.Invite.Range", 100.0f));
    InviteHumanCooldownMs = u32("Playerbots.Social.Invite.HumanCooldownMs", 1800000);   // user 11-13: once per 30 min per human
    InviteChat = sConfigMgr->GetBoolDefault("Playerbots.Social.Invite.Chat", false);
    InvitePassLead = sConfigMgr->GetBoolDefault("Playerbots.Social.Invite.PassLead", true);

    DuelEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.Duel.Enable", false);
    DuelCheckMs = std::max<uint32>(1000, u32("Playerbots.Social.Duel.CheckMs", 10000));
    DuelStarterPct = pct("Playerbots.Social.Duel.StarterPct", 25);
    DuelChallengePct = pct("Playerbots.Social.Duel.ChallengePct", 10);
    DuelRange = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Social.Duel.Range", 20.0f));
    DuelHumanCooldownMs = u32("Playerbots.Social.Duel.HumanCooldownMs", 1800000);   // user 11-13: once per 30 min per human
    DuelAcceptAnywhere = sConfigMgr->GetBoolDefault("Playerbots.Social.Duel.AcceptAnywhere", false);

    WarModeEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.WarMode.Enable", false);
    WarModeOneIn = std::max<uint32>(1, u32("Playerbots.Social.WarMode.OneIn", 3));
    WarModeMinLevel = u32("Playerbots.Social.WarMode.MinLevel", 20);
    WarModeLevelGap = u32("Playerbots.Social.WarMode.LevelGap", 10);
    WarModeRange = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Social.WarMode.Range", 30.0f));

    TradeGiftEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.Trade.Gift", false);
    TradeGiftCheckMs = std::max<uint32>(1000, u32("Playerbots.Social.Trade.GiftCheckMs", 30000));
    TradeGiftPct = pct("Playerbots.Social.Trade.GiftPct", 20);
    TradeGiftRange = std::max(2.0f, sConfigMgr->GetFloatDefault("Playerbots.Social.Trade.GiftRange", 10.0f));

    BankEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.Bank.Enable", false);
    BankCooldownMs = u32("Playerbots.Social.Bank.CooldownMs", 600000);
    BankBuyTab = sConfigMgr->GetBoolDefault("Playerbots.Social.Bank.BuyTab", false);
    GuildBankEnable = sConfigMgr->GetBoolDefault("Playerbots.Social.GuildBank.Enable", false);
    GuildBankKeepCopper = uint64(std::max<int64>(0, sConfigMgr->GetInt64Default("Playerbots.Social.GuildBank.KeepCopper", 1000000)));
    GuildBankDepositPct = pct("Playerbots.Social.GuildBank.DepositPct", 10);

    MageRefreshments = sConfigMgr->GetBoolDefault("Playerbots.Social.MageRefreshments", false);
    MageCheckMs = std::max<uint32>(1000, u32("Playerbots.Social.MageRefreshments.CheckMs", 5000));
    MageTableRange = std::max(5.0f, sConfigMgr->GetFloatDefault("Playerbots.Social.MageRefreshments.TableRange", 30.0f));
}
}
