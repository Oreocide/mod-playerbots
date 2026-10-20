#!/bin/sh
# Builds and runs the standalone tests (engine framework, chat parser). No TrinityCore build needed,
# only its Define.h headers
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TC=${TRINITY_SRC:-$HERE/../../..}
SRC=$HERE/../src
OUT=${TMPDIR:-/tmp}
FLAGS="-std=c++20 -Wall -Wextra -I$SRC -I$SRC/Engine -I$TC/src/common"

g++ $FLAGS "$HERE/engine_test.cpp" "$SRC/Engine/Engine.cpp" "$SRC/Engine/AiContext.cpp" "$SRC/Engine/AiRegistry.cpp" -o "$OUT/playerbots_engine_test"
"$OUT/playerbots_engine_test"

g++ $FLAGS "$HERE/chat_parser_test.cpp" "$SRC/Cmd/ChatParser.cpp" -o "$OUT/playerbots_chat_parser_test"
"$OUT/playerbots_chat_parser_test"

g++ $FLAGS "$HERE/hub_cluster_test.cpp" "$SRC/Travel/HubClusters.cpp" -o "$OUT/playerbots_hub_cluster_test"
"$OUT/playerbots_hub_cluster_test"

g++ $FLAGS "$HERE/hotspot_test.cpp" "$SRC/Travel/Hotspots.cpp" -o "$OUT/playerbots_hotspot_test"
"$OUT/playerbots_hotspot_test"

g++ $FLAGS "$HERE/travel_graph_test.cpp" "$SRC/Travel/Graph/TravelGraph.cpp" -o "$OUT/playerbots_travel_graph_test"
"$OUT/playerbots_travel_graph_test"

g++ $FLAGS "$HERE/rotation_script_test.cpp" "$SRC/Rotation/RotationScript.cpp" -o "$OUT/playerbots_rotation_script_test"
"$OUT/playerbots_rotation_script_test"

g++ $FLAGS "$HERE/bot_mover_test.cpp" -o "$OUT/playerbots_bot_mover_test"
"$OUT/playerbots_bot_mover_test"

g++ $FLAGS "$HERE/tick_policy_test.cpp" -o "$OUT/playerbots_tick_policy_test"
"$OUT/playerbots_tick_policy_test"

g++ $FLAGS "$HERE/context_strategies_test.cpp" -o "$OUT/playerbots_context_strategies_test"
"$OUT/playerbots_context_strategies_test"

g++ $FLAGS "$HERE/move_geometry_test.cpp" -o "$OUT/playerbots_move_geometry_test"
"$OUT/playerbots_move_geometry_test"

g++ $FLAGS "$HERE/air_profile_test.cpp" -o "$OUT/playerbots_air_profile_test"
"$OUT/playerbots_air_profile_test"

g++ $FLAGS "$HERE/air_follow_test.cpp" -o "$OUT/playerbots_air_follow_test"
"$OUT/playerbots_air_follow_test"

g++ $FLAGS "$HERE/loot_roll_rules_test.cpp" -o "$OUT/playerbots_loot_roll_rules_test"
"$OUT/playerbots_loot_roll_rules_test"
