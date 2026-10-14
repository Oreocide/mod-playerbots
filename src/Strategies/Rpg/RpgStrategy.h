#ifndef PlayerbotsRpgStrategy_h__
#define PlayerbotsRpgStrategy_h__

#include "Engine.h"
#include <memory>

namespace Playerbots
{
std::unique_ptr<Strategy> CreateRpgStrategy();
}

#endif
