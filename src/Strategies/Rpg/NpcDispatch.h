#ifndef PlayerbotsNpcDispatch_h__
#define PlayerbotsNpcDispatch_h__

#include "ObjectGuid.h"
#include <utility>
#include <vector>

class Creature;
class Player;
class WorldObject;

namespace Playerbots
{
int32 ScoreNpcTarget(Player* bot, WorldObject* object);
/// Best NPC within reach. `blocked` (NPCs the navmesh could not reach, with their expiry in ms) are skipped.
ObjectGuid ChooseNpcTarget(Player* bot, std::vector<std::pair<ObjectGuid, uint32>> const* blocked = nullptr, uint32 now = 0);
/// Nearest living NPC within range that has the npc flag bit (test harness: a trainer, wherever no hub kind exists)
ObjectGuid FindNearestNpcWithFlag(Player* bot, uint32 npcFlag, float range);
bool TryInteract(Player* bot, WorldObject* object);
}

#endif
