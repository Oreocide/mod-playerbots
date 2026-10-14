#ifndef PlayerbotsNpcDispatch_h__
#define PlayerbotsNpcDispatch_h__

#include "ObjectGuid.h"

class Creature;
class Player;
class WorldObject;

namespace Playerbots
{
int32 ScoreNpcTarget(Player* bot, WorldObject* object);
ObjectGuid ChooseNpcTarget(Player* bot);
bool TryInteract(Player* bot, WorldObject* object);
}

#endif
