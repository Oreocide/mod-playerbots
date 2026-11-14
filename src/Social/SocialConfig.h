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

#ifndef Playerbots_SocialConfig_h__
#define Playerbots_SocialConfig_h__

#include "Define.h"

// The Playerbots.Social.* keys (AC parity group C: channels, invites, duels, War Mode, trades, banks, mail, mage tables).
// Kept out of PlayerbotsConfig so that header's layout does not change; loaded by the playerbots_social world script
// (SocialAi.cpp) at start and on .reload config. The defaults here equal conf/playerbots.conf.dist. Every behaviour is off
// until its Enable key is set; the tuning values are AzerothCore's (the source line is named next to each).
namespace Playerbots::Social
{
struct Config
{
    // ---- public channels (AC SuggestWhatToDoAction / BroadcastHelper) ----
    bool ChannelEnable = false;          // Playerbots.Social.Channel.Enable: General / Trade / LookingForGroup posts
    uint32 ChannelCheckMs = 2000;        // AC AiPlayerbot.RepeatDelay: how often the "often" trigger is rolled
    uint32 ChannelOneIn = 5;             // AC "often" = RandomTrigger probability 5: fires 1 roll in 5
    uint32 ChannelMinGapSec = 30;        // AC SuggestWhatToDoAction::isUseful: 30 s since the bot's last post
    uint32 ChannelExtraGapSec = 60;      // AC: "last said" is set to now + urand(1, 60)
    uint32 ChanceQuest = 10000;          // AC AiPlayerbot.BroadcastChanceSuggestQuest (of 30000)
    uint32 ChanceSomething = 30000;      // AC AiPlayerbot.BroadcastChanceSuggestSomething
    uint32 ChanceInstance = 5000;        // AC AiPlayerbot.BroadcastChanceSuggestInstance
    uint32 ChanceSell = 300;             // AC AiPlayerbot.BroadcastChanceSuggestSell
    bool ChannelDungeons = true;         // AC AiPlayerbot.RandomBotSuggestDungeons
    uint32 ZoneMinGapMs = 20000;         // not in AC: at most one bot post per zone (all channels) this often; user 11-13: 20 s (0 = no cap, as AC)
    bool ReplyEnable = false;            // Playerbots.Social.Channel.Replies: answer a player's LFG/LFM and WTB lines (AC ChatReplyAction)
    uint32 ReplyMaxBots = 0;             // bots that answer one line (0 = every bot with a match, as AC)
    uint32 ReplyMinDelayMs = 2000;       // not in AC (it answers at once): a person needs time to type
    uint32 ReplyMaxDelayMs = 8000;

    // ---- invites (AC InviteNearbyToGroupAction, "often") ----
    bool InviteEnable = false;           // Playerbots.Social.Invite.Enable (AC RandomBotGroupNearby + RandomBotInvitePlayer, both 0 in AC)
    uint32 InviteCheckMs = 2000;         // AC RepeatDelay
    uint32 InviteOneIn = 5;              // AC "often"
    uint32 InviteLevelGap = 2;           // AC: abs(level difference) > 2 is skipped
    float InviteRange = 100.0f;          // AC AiPlayerbot.SightDistance
    uint32 InviteHumanCooldownMs = 1800000;    // not in AC: per human, across all bots (user 11-13: 30 min; 0 = none, as AC)
    bool InviteChat = false;             // AC AiPlayerbot.InviteChat: whisper "Hey <name>, do you want join my group?"
    bool InvitePassLead = true;          // the bot hands the lead to the human who joined, so the bot follows (PlayerbotAI master rule)

    // ---- duels (AC "start duel" / RpgDuelTrigger, AcceptDuelAction) ----
    bool DuelEnable = false;             // Playerbots.Social.Duel.Enable: bots challenge players and bots near them
    uint32 DuelCheckMs = 10000;          // AC AiPlayerbot.RpgDelay (the rpg target cadence the duel rides on)
    uint32 DuelStarterPct = 25;          // AC AiFactory: "let 25% of free bots start duels"
    uint32 DuelChallengePct = 10;        // not a single AC number (one rpg sub-action among many): chance per check
    float DuelRange = 20.0f;
    uint32 DuelHumanCooldownMs = 1800000;      // not in AC: per human, across all bots (user 11-13: 30 min; 0 = none)
    bool DuelAcceptAnywhere = false;     // Playerbots.Social.Duel.AcceptAnywhere: accept outside cities too at >= 90% health (AC)

    // ---- War Mode ----
    bool WarModeEnable = false;          // Playerbots.Social.WarMode.Enable
    uint32 WarModeOneIn = 3;             // user decision 2026-11-13: about one bot in three (stable by guid)
    uint32 WarModeMinLevel = 20;         // user decision 2026-11-13
    uint32 WarModeLevelGap = 10;         // a War Mode bot only picks a fight within this many levels
    float WarModeRange = 30.0f;

    // ---- trades ----
    bool TradeGiftEnable = false;        // Playerbots.Social.Trade.Gift: hand an upgrade one cannot use to a bot that can (AC "rpg trade useful")
    uint32 TradeGiftCheckMs = 30000;
    uint32 TradeGiftPct = 20;
    float TradeGiftRange = 10.0f;

    // ---- banks ----
    bool BankEnable = false;             // Playerbots.Social.Bank.Enable: at a banker, store reagents and trade goods
    uint32 BankCooldownMs = 600000;
    bool BankBuyTab = false;             // buy the first character bank tab when the bot can pay for it
    bool GuildBankEnable = false;        // Playerbots.Social.GuildBank.Enable: at a guild vault, deposit gold over the keep amount
    uint64 GuildBankKeepCopper = 1000000; // 100 gold
    uint32 GuildBankDepositPct = 10;     // share of the gold over the keep amount

    // ---- mage refreshments ----
    bool MageRefreshments = false;       // Playerbots.Social.MageRefreshments
    uint32 MageCheckMs = 5000;
    float MageTableRange = 30.0f;

    void Load();
};

Config& GetConfig();
}

#define sSocialConfig Playerbots::Social::GetConfig()

#endif // Playerbots_SocialConfig_h__
