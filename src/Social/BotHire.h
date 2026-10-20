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

#ifndef PlayerbotsBotHire_h__
#define PlayerbotsBotHire_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Player;

namespace Playerbots
{
/// Playerbots.Hire.Enable: a random bot in your party joins your account for good (AzerothCore's "hire"). The price comes out of the
/// master's gold, the bot logs out, and once it is offline the character moves to the master's account the way the core's
/// ".character changeaccount" does (character row, realm character counts, character cache). Whisper-only command "hire".
class BotHire
{
public:
    static BotHire* instance();

    /// "" = the hire may go ahead, else the reason as the bot says it. cost is filled when the bot is hireable by price
    std::string Check(Player* master, Player* bot, uint64& cost) const;
    /// Charges the master, takes the bot out of the random pool and logs it out. Call after Check
    bool Start(Player* master, Player* bot, uint64 cost);
    /// World thread: moves the characters whose bots have logged out
    void Update(uint32 diff);

    static uint64 PriceFor(uint32 level);

private:
    struct Pending
    {
        ObjectGuid Bot;
        ObjectGuid Master;
        uint32 MasterAccount = 0;
        uint64 Cost = 0;
        uint32 StartedAt = 0;
        std::string Name;
    };

    void Finish(Pending const& pending);
    void Fail(Pending const& pending, char const* why);

    std::vector<Pending> _pending;
    uint32 _timer = 0;
};
}

#define sBotHire Playerbots::BotHire::instance()

#endif
