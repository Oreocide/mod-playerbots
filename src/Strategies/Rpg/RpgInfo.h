/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Ported from AzerothCore mod-playerbots NewRpgInfo (status + destination),
 * without the 3.3.5 TravelNode graph.
 */

#ifndef PlayerbotsRpgInfo_h__
#define PlayerbotsRpgInfo_h__

#include "ObjectGuid.h"
#include "Position.h"
#include <vector>

namespace Playerbots
{
enum class RpgStatus : uint8
{
    Idle = 0,
    DoQuest,
    WanderNpc,
    TravelFlight,
    GoGrind,
    GoCamp,
    Rest,
    WanderRandom,
    Gather
};

inline constexpr uint32 RpgStatusCount = 9;

struct RpgInfo
{
    RpgStatus Status = RpgStatus::Idle;
    uint32 Started = 0;
    uint32 WaitUntil = 0;
    uint32 QuestId = 0;
    uint32 TaxiDestNode = 0;
    uint32 LastProgress = 0;
    float BestDist = 0.0f;
    Position Dest;
    ObjectGuid Target;
    bool HasDest = false;
    uint64 NodeSpawn = 0;              // gathering: spawn id of the node walked to
    uint32 NodeEntry = 0;
    bool Working = false;              // a work emote is on (cleared by whoever ends the gathering)
    std::vector<uint64> RecentNodes;   // nodes just gathered or found missing, not picked again soon

    void ChangeToIdle()
    {
        Status = RpgStatus::Idle;
        HasDest = false;
        QuestId = 0;
        TaxiDestNode = 0;
        Target.Clear();
        WaitUntil = 0;
        BestDist = 0.0f;
        NodeSpawn = 0;
        NodeEntry = 0;
    }

    void Begin(RpgStatus status, uint32 now)
    {
        Status = status;
        Started = now;
        WaitUntil = 0;
        LastProgress = now;
        BestDist = 0.0f;
        HasDest = false;
        QuestId = 0;
        TaxiDestNode = 0;
        Target.Clear();
        NodeSpawn = 0;
        NodeEntry = 0;
    }

    void SetDest(Position const& pos)
    {
        Dest = pos;
        HasDest = true;
        BestDist = 0.0f;
    }

    bool TimedOut(uint32 now, uint32 durationMs) const
    {
        return durationMs && now >= Started && (now - Started) >= durationMs;
    }
};
}
#endif
