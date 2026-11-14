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

#ifndef Playerbots_SocialAi_h__
#define Playerbots_SocialAi_h__

// AzerothCore parity, group C (docs/design/AC_PARITY.md): what bots do with and around other players.
//  - public channels: "anyone for <quest>", WTS links in Trade, LFM <dungeon> (AC SuggestWhatToDoAction), answers to a
//    player's LFG/LFM and WTB lines (AC ChatReplyAction);
//  - invites to a nearby player of the bot's level, the lead handed to the player who joins; the "lfg" order;
//  - duels: challenges, accepting at >= 90% health, fighting the duel to its end;
//  - War Mode for about a third of the bots, fights with hostile War Mode players near them;
//  - trades: the "trade" order, upgrades handed to bots that can use them;
//  - character bank and guild bank ("bank", "gb"), mail ("mail", "sendmail");
//  - mages conjure food and put down a refreshment table for a party that has none.
// Every behaviour is behind a Playerbots.Social.* key (SocialConfig.h), off by default, and runs only for bots a player can
// see (PlayerbotAI::IsSimulated). The orders are always registered (they only run when a master types them).
namespace Playerbots
{
class AiRegistry;

namespace Social
{
/// Actions, triggers, the "social" strategy and the chat orders. Call once from RegisterGenericAi
void RegisterSocialAi(AiRegistry& registry);
}
}

/// Config loader, chat/duel/group hooks (call from Addmod_playerbotsScripts)
void AddSC_playerbots_social();

#endif // Playerbots_SocialAi_h__
