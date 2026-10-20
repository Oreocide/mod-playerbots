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

#ifndef Playerbots_CustomStrategy_h__
#define Playerbots_CustomStrategy_h__

#include "Cmd/CommandText.h"
#include "ObjectGuid.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Playerbots
{
class Strategy;

/// Strategies a player writes for one bot in chat ("cs heal-first needs heal > heal"), stored per bot in the playerbots store
/// ("cs:<name>", up to MaxCustomDefinition characters) and active as "custom::<name>" (co +custom::heal-first)
namespace CustomStrategies
{
constexpr std::string_view Prefix = "custom::";

std::vector<std::string> Names(ObjectGuid bot);
std::vector<CustomLine> Load(ObjectGuid bot, std::string const& name);
/// false if the lines do not fit the store
bool Save(ObjectGuid bot, std::string const& name, std::vector<CustomLine> const& lines);
void Delete(ObjectGuid bot, std::string const& name);

/// "custom::<name>" as a strategy of this bot, nullptr if it has no such definition
std::unique_ptr<Strategy> Create(ObjectGuid bot, std::string_view fullName);
}
}

#endif
