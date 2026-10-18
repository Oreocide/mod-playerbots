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
MoveResult MoveAlongMesh(Player* bot, Position const& dest, float arriveYards = 5.0f);

/// Compatibility wrapper: true while moving; false when arrived or when there is no mesh path (use MoveAlongMesh to tell them apart)
bool MoveToward(Player* bot, Position const& dest, float arriveYards = 5.0f);

struct MeshMoveStats
{
    uint32 Launched = 0;
    uint32 NoPath = 0;
};
MeshMoveStats GetMeshMoveStats();
}

#endif
