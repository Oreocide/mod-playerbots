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

// Parses every data/rotations/*.rot file with the rotation script parser and fails on any problem. Run by
// tests/run_engine_test.sh with the file names as arguments

#include "Rotation/RotationScript.h"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace Playerbots;

int main(int argc, char** argv)
{
    int sets = 0;
    int errors = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::ifstream in(argv[i]);
        std::stringstream text;
        text << in.rdbuf();
        ParseResult result = ParseRotations(text.str(), argv[i]);
        sets += int(result.Sets.size());
        for (std::string const& error : result.Errors)
        {
            std::printf("%s\n", error.c_str());
            ++errors;
        }
    }

    if (errors || !sets)
    {
        std::printf("rotation_files_test: %d problem(s), %d set(s)\n", errors, sets);
        return 1;
    }
    std::printf("rotation_files_test: ok (%d rotations)\n", sets);
    return 0;
}
