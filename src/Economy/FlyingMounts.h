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

#ifndef PlayerbotsFlyingMounts_h__
#define PlayerbotsFlyingMounts_h__

#include "Define.h"
#include <string>
#include <vector>

class Player;

namespace Playerbots
{
/// Flying mounts of the 12.x client data (air legs, Playerbots.Air.*). Nearly every flying mount is a hybrid: the mount type tries
/// a skyriding capability first (the player has the spell 376777 and does NOT have the steady-flight style aura 404468), then a
/// steady-flight one (riding skill 225 / 300 / 375 and the aura 404468), then a ground one. The module only ever wants steady flight:
/// a bot flies on a server-driven spline, there is no client to do skyriding physics.
namespace FlyingMounts
{
constexpr uint32 SteadyFlightStyleAura = 404468;   // "Flight Style: Steady Flight", chosen by the player in retail
constexpr uint32 MinRidingSkill = 225;             // Expert Riding: the lowest ReqRidingSkill of a steady flying capability

/// The mount spell's mount type has a flying capability in MountCapability.db2
bool IsFlyer(uint32 mountSpell);

/// Flying mount spells of the bot's account collection
std::vector<uint32> Known(Player const* bot);

/// The first hybrid flying mount of the client data (a mount spell nobody has to own: GetMountCapability does not look at ownership),
/// 0 if there is none. For the probe of a bot whose account holds no flyer.
uint32 AnyFlyer();

/// What the core's own mount logic (Unit::GetMountCapability) would give the bot on this mount, one line per fact. Runs on the bot's map
/// thread (GetMountCapability reads the bot's auras and liquid state). steadyTest: the style aura is added for the test and taken off again.
std::vector<std::string> Describe(Player* bot, bool steadyTest);

/// Every bot flies steady: the steady-flight style aura is on before a mount is cast, so a hybrid mount never resolves to skyriding (there is
/// no client to do its physics). Cheap: a bot that has it is left alone. On the bot's map thread.
void EnsureSteadyStyle(Player* bot);

/// Master riding (375) and a hybrid flying mount in the account collection, whatever is missing; true if something was added. On the bot's
/// map thread. The collection is shared by every character of the Battle.net account.
bool GrantNow(Player* bot);

enum class MountState : uint8
{
    Ready,      // mounted on something that gives flight
    Casting,    // the mount cast is under way
    Cannot      // no flying mount (and none granted), indoors, or the area does not allow flying
};
/// Gets the bot onto a flying mount: Ready, or Casting (call again), or Cannot. grantMissing: add the riding skill and a mount first.
/// On the bot's map thread.
MountState EnsureMounted(Player* bot, bool grantMissing);

/// `.bot air grant`: master riding skill (375) and a hybrid flying mount in the account collection, so a test bot can take to the air. On the
/// bot's map thread; logged. The mounts of an account are shared by all its characters.
void GrantAsync(Player* bot);

/// `.bot air probe`: queues Describe on the bot's map thread and logs the lines (server.playerbots), the console prints them
void ProbeAsync(Player* bot, bool steadyTest);
}
}

#endif
