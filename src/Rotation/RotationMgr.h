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

#ifndef PlayerbotsRotationMgr_h__
#define PlayerbotsRotationMgr_h__

#include "RotationScript.h"
#include "Define.h"
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace Playerbots
{
/// All rotations by specialization id. Immutable once built, replaced as a whole on reload
struct RotationTable
{
    std::unordered_map<uint32, std::unique_ptr<RotationSet>> Sets;
    uint32 Files = 0;
    std::vector<std::string> Errors;
};

/// Loads the *.rot files of a directory (Playerbots.Rotation.Path, by default the module's data/rotations next to the
/// source directory the server was built from) and hands them out. Bots keep the table they started a tick with
class RotationMgr
{
public:
    static RotationMgr* instance();

    /// (Re)reads the files; returns the number of specializations with a rotation. World thread
    uint32 Load();
    std::shared_ptr<RotationTable const> GetTable() const { return _table.load(); }
    std::string const& GetPath() const { return _path; }

private:
    std::atomic<std::shared_ptr<RotationTable const>> _table;
    std::string _path;
};
}

#define sRotationMgr Playerbots::RotationMgr::instance()

#endif // PlayerbotsRotationMgr_h__
