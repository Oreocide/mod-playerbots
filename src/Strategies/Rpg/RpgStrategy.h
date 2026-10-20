#ifndef PlayerbotsRpgStrategy_h__
#define PlayerbotsRpgStrategy_h__

#include "Engine.h"
#include "RpgInfo.h"
#include <memory>

namespace Playerbots
{
class PlayerbotAI;

std::unique_ptr<Strategy> CreateRpgStrategy();

/// A frozen bot's next status by AC's weights, as a timer only (Playerbots.Abstract.Enable): the statuses that mean something without
/// a world around the bot (DoQuest, GoGrind, WanderNpc, GoCamp, Rest, WanderRandom), none that path, use an NPC or work a node
RpgStatus PickAbstractRpgStatus(PlayerbotAI& ai);
/// How long such a status lasts: between half and all of Playerbots.Rpg.Duration.*
uint32 RollAbstractRpgDuration(RpgStatus status);
}

#endif
