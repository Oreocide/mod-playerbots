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

#ifndef PlayerbotsBotGuildMgr_h__
#define PlayerbotsBotGuildMgr_h__

#include "Define.h"
#include <string>
#include <vector>

namespace Playerbots
{
/// Playerbots.Guild.Enable: random bots found guilds of their own and fill them, so the guild tab is not empty. Every
/// Guild.Interval (world thread) it founds at most one guild (a leader and four same-team bots, like a charter with its signatures)
/// and adds a few bots to the bot guilds that have room, until Guild.Percent of the eligible random bots are in one.
/// A guild led by a random bot is a bot guild; guilds of players are never touched.
class BotGuildMgr
{
public:
    static BotGuildMgr* instance();

    void Update(uint32 diff);

    /// The guilds led by a random bot, as "name (members)" lines (.bot guild list)
    std::vector<std::string> List() const;
    /// Disbands every guild led by a random bot, returns how many (.bot guild purge). The way back from Guild.Enable
    uint32 Purge();

private:
    void Run();
    uint32 _timer = 0;
};
}

#define sBotGuildMgr Playerbots::BotGuildMgr::instance()

#endif
