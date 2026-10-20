#ifndef PlayerbotsTravelMove_h__
#define PlayerbotsTravelMove_h__

#include "Define.h"
#include "Position.h"

class Player;

namespace Playerbots
{
/// Floor under hintZ (tunnels, Ironforge). Never use MAX_HEIGHT — that is the mountain roof.
float GetStandZ(Player* bot, float x, float y, float hintZ, float lift = 0.25f);
bool TryGetStandZ(Player* bot, float x, float y, float hintZ, float& outZ, float lift = 0.25f);

bool IsMovingOnFoot(Player const* bot);
/// A PathGenerator result a bot may follow: NORMAL or INCOMPLETE, never NOPATH/SHORTCUT/NOT_USING_PATH (those are straight lines)
bool PathIsWalkable(uint32 type);
/// Mounts for a trip of distanceToDest yards (mountAtOverride: shorter trips walk; 0 = the default, about 10 yards).
/// Tries at most every 30 seconds per bot, only outdoors where the mount spell is allowed.
void UpdateMountForTravel(Player* bot, float distanceToDest, float mountAtOverride = 0.0f);

enum class MoveResult : uint8
{
    Arrived,    // within arriveYards of the target
    Moving,     // a mesh path is running, or was just launched
    NoPath      // the navmesh has no path (or no progress) to the target: pick another target
};

/// Walk along the navmesh only. Never a straight line, never a VMAP-only step.
/// standoff > 0: the walk ends that many yards (2D) short of dest instead of on it (an NPC to talk to: a bot stands near it, not
/// on its spot). The walk is the mesh path cut where its distance to dest reaches the stand-off, never a new point; a path
/// that already ends farther away (an NPC behind a counter) is not cut. Arrived when the bot already is that close.
MoveResult MoveAlongMesh(Player* bot, Position const& dest, float arriveYards = 5.0f, float standoff = 0.0f);

/// Compatibility wrapper: true while moving; false when arrived or when there is no mesh path (use MoveAlongMesh to tell them apart)
bool MoveToward(Player* bot, Position const& dest, float arriveYards = 5.0f, float standoff = 0.0f);

struct MeshMoveStats
{
    uint32 Launched = 0;
    uint32 NoPath = 0;
    uint32 Checked = 0;   // paths tested for crossing geometry (Move.LogBlockedPaths)
    uint32 Blocked = 0;   // of those, paths with a segment blocked at eye height
    uint32 KneeOnly = 0;  // blocked at knee height only (stairs, curbs)
    uint32 Steep = 0;     // with a cliff segment
    uint32 Repaired = 0;  // paths given a detour around geometry (Move.AvoidBlockedPaths)
    uint32 Rejected = 0;  // paths refused for a cliff segment
    uint32 Hops = 0;      // jumps over low obstacles (Move.HopLowObstacles)
};
MeshMoveStats GetMeshMoveStats();
}

#endif
