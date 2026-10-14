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

#ifndef PlayerbotsRotationFacts_h__
#define PlayerbotsRotationFacts_h__

#include "RotationScript.h"
#include <string>

class Player;
class Unit;

namespace Playerbots
{
class PlayerbotAI;
class RotationRunner;

/// Whether the name of a fact is one the engine implements (checked when a script is loaded)
bool IsKnownRotationFact(std::string const& name);

/// The facts of one bot and the target of one rotation entry. Cheap to make, computes what a condition asks for
class BotFacts final : public FactSource
{
public:
    BotFacts(PlayerbotAI& ai, RotationRunner& runner, Unit* target) : _ai(ai), _runner(runner), _target(target) { }

    bool Number(std::string const& name, double& value) const override;
    bool Flag(std::string const& name, bool& value) const override;

private:
    bool AuraFact(std::string const& name, double* number, bool* flag) const;

    PlayerbotAI& _ai;
    RotationRunner& _runner;
    Unit* _target;
};
}

#endif // PlayerbotsRotationFacts_h__
