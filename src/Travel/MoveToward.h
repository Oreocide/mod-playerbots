#ifndef PlayerbotsTravelMove_h__
#define PlayerbotsTravelMove_h__

#include "Position.h"

class Player;

namespace Playerbots
{
/// Floor under hintZ (tunnels, Ironforge). Never use MAX_HEIGHT — that is the mountain roof.
float GetStandZ(Player* bot, float x, float y, float hintZ);
bool TryGetStandZ(Player* bot, float x, float y, float hintZ, float& outZ);

bool IsMovingOnFoot(Player const* bot);
void UpdateMountForTravel(Player* bot, float distanceToDest);

/// Walk using mmaps when they exist. Never bee-line through walls (Ironforge).
bool MoveToward(Player* bot, Position const& dest, float arriveYards = 5.0f);
}

#endif
