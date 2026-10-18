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
    Gather,
    Craft
};

inline constexpr uint32 RpgStatusCount = 10;

struct RpgInfo
{
    RpgStatus Status = RpgStatus::Idle;
    uint32 Started = 0;
    uint32 DurationMs = 0;             // rolled once per task, between half and all of the configured cap
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
    uint32 LastHopRoll = 0;            // ambient hop: last time the walking chance was rolled, earliest next hop (ms)
    uint32 NextHopAt = 0;
    uint32 NextGreetAt = 0;            // earliest next greeting emote at a nearby player (ms)
    uint32 ForcedUntil = 0;            // test harness (.bot rpg): until this time (ms) the quest action stays out of the way
    int8 ForceKind = -1;               // test harness (.bot rpg): HubKind of the service to walk to and use, -1 = none
    std::vector<std::pair<ObjectGuid, uint32>> BadTargets;   // NPCs the navmesh cannot reach: not picked again until the stored time (ms)
    std::vector<std::pair<ObjectGuid, uint32>> ReplyUntil;  // players this bot already answered, until the stored time (ms)

    bool RepliedTo(ObjectGuid guid, uint32 now) const
    {
        for (auto const& [who, until] : ReplyUntil)
            if (who == guid && until > now)
                return true;
        return false;
    }

    void NoteReply(ObjectGuid guid, uint32 now, uint32 forMs)
    {
        std::erase_if(ReplyUntil, [now](auto const& entry) { return entry.second <= now; });
        if (ReplyUntil.size() >= 8)
            ReplyUntil.erase(ReplyUntil.begin());
        ReplyUntil.emplace_back(guid, now + forMs);
    }

    void BlockTarget(ObjectGuid guid, uint32 now, uint32 forMs)
    {
        std::erase_if(BadTargets, [now](auto const& entry) { return entry.second <= now; });
        BadTargets.emplace_back(guid, now + forMs);
    }

    bool IsBlocked(ObjectGuid guid, uint32 now) const
    {
        for (auto const& [blocked, until] : BadTargets)
            if (blocked == guid && until > now)
                return true;
        return false;
    }

    void ChangeToIdle()
    {
        Status = RpgStatus::Idle;
        DurationMs = 0;
        HasDest = false;
        QuestId = 0;
        TaxiDestNode = 0;
        Target.Clear();
        WaitUntil = 0;
        BestDist = 0.0f;
        NodeSpawn = 0;
        NodeEntry = 0;
        ForceKind = -1;
    }

    void Begin(RpgStatus status, uint32 now)
    {
        Status = status;
        Started = now;
        DurationMs = 0;
        WaitUntil = 0;
        LastProgress = now;
        BestDist = 0.0f;
        HasDest = false;
        QuestId = 0;
        TaxiDestNode = 0;
        Target.Clear();
        NodeSpawn = 0;
        NodeEntry = 0;
        ForceKind = -1;
    }

    void SetDest(Position const& pos)
    {
        // the same target set again every tick must not restart the progress record (the stuck timer would never fire)
        if (!HasDest || Dest.GetExactDist2d(pos) > 1.0f)
            BestDist = 0.0f;
        Dest = pos;
        HasDest = true;
    }

    bool TimedOut(uint32 now, uint32 durationMs) const
    {
        return durationMs && now >= Started && (now - Started) >= durationMs;
    }
};
}
#endif
