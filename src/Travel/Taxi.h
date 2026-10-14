#ifndef PlayerbotsTravelTaxi_h__
#define PlayerbotsTravelTaxi_h__

#include "Define.h"

class Creature;
class Player;

namespace Playerbots
{
bool TryTakeTaxi(Player* bot, Creature* flightMaster, uint32 destNode);
/// Flight from one taxi node to another without a flight master NPC (the bot stands at the node)
bool TakeTaxi(Player* bot, uint32 fromNode, uint32 destNode);
bool PickRandomTaxiDestination(Player* bot, uint32 fromNode, uint32& destNode);
}

#endif
