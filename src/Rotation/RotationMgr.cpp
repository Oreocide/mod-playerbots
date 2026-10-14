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

#include "RotationMgr.h"
#include "Config.h"
#include "Log.h"
#include "PlayerbotsConfig.h"
#include "StringFormat.h"
#include "RotationFacts.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Playerbots
{
RotationMgr* RotationMgr::instance()
{
    static RotationMgr instance;
    return &instance;
}

uint32 RotationMgr::Load()
{
    auto table = std::make_shared<RotationTable>();

    _path = sPlayerbotsConfig.RotationPath;
    if (_path.empty())
    {
        // the data directory of the module in the source tree the server runs from (the docker setup mounts it)
        std::string source = sConfigMgr->GetStringDefault("SourceDirectory", "");
        _path = (source.empty() ? std::string("modules") : source + "/modules") + "/mod-playerbots/data/rotations";
    }

    std::error_code code;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(_path, code))
        if (entry.is_regular_file() && entry.path().extension() == ".rot")
            files.push_back(entry.path());

    if (code)
    {
        TC_LOG_WARN("server.playerbots", "Playerbots: no rotations, cannot read {} ({})", _path, code.message());
        _table.store(std::move(table));
        return 0;
    }

    std::ranges::sort(files);
    for (std::filesystem::path const& file : files)
    {
        std::ifstream stream(file);
        std::stringstream text;
        text << stream.rdbuf();

        ParseResult parsed = ParseRotations(text.str(), file.filename().string());
        for (std::string const& error : parsed.Errors)
            table->Errors.push_back(error);

        for (std::unique_ptr<RotationSet>& set : parsed.Sets)
        {
            // facts nobody implements are false: say so now, not by a bot that never casts
            for (auto const& [listName, list] : set->Lists)
                for (RotationEntry const& entry : list.Entries)
                    if (entry.Condition)
                    {
                        std::vector<std::string> facts;
                        CollectFacts(*entry.Condition, facts);
                        for (std::string const& fact : facts)
                            if (!IsKnownRotationFact(fact))
                                table->Errors.push_back(Trinity::StringFormat("{}:{}: unknown fact '{}'", file.filename().string(), entry.Line, fact));
                    }

            if (!table->Sets.emplace(set->Spec, std::move(set)).second)
                table->Errors.push_back(file.filename().string() + ": a second rotation for a specialization that has one, ignored");
        }

        ++table->Files;
    }

    uint32 count = uint32(table->Sets.size());
    for (std::string const& error : table->Errors)
        TC_LOG_ERROR("server.playerbots", "Playerbots rotation: {}", error);

    TC_LOG_INFO("server.playerbots", "Playerbots: {} rotations from {} files in {} ({} problems)", count, table->Files, _path, table->Errors.size());
    _table.store(std::move(table));
    return count;
}
}
