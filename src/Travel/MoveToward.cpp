#include "MoveToward.h"
#include "MoveGeometry.h"
#include "CollectionMgr.h"
#include "DB2Stores.h"
#include "GridDefines.h"
#include "Log.h"
#include "Economy/RaceMounts.h"
#include "GameTime.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "Spawn/SpawnPolicy.h"
#include "PathRepair.h"
#include "PlayerJump.h"
#include "StuckDiagnosis.h"
#include "Random.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Playerbots
{
constexpr float IndoorSnapSlack = 40.0f;

bool TryGetStandZ(Player* bot, float x, float y, float hintZ, float& outZ, float lift)
{
    Map* map = bot->GetMap();
    if (!map)
        return false;

    // Playerbots.FarBotHeightNoGridLoad: this query is made at route points, wander targets and every candidate quest point, often far away; loading
    // the grid there costs ~34 MB and ~600 ms and was the module's own source of 28% of the grids created in the rotation benchmark. A bot nobody
    // is near (frozen, or simulated only by the activity rotation) reads the height only if the terrain is already there; GetHeight then
    // returns INVALID_HEIGHT and every caller falls back to its hint Z.
    bool loadGrid = true;
    if (sPlayerbotsConfig.FarBotHeightNoGridLoad)
    {
        PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
        if (ai && ai->IsFarBot() && !map->IsGridLoaded(x, y))
            loadGrid = false;
    }
    if (loadGrid)
        map->LoadGrid(x, y);

    float local = map->GetHeight(bot->GetPhaseShift(), x, y, hintZ + 2.0f, true, 50.0f);
    if (local > INVALID_HEIGHT && std::fabs(local - hintZ) <= IndoorSnapSlack)
    {
        outZ = local + lift;
        return true;
    }

    float wider = map->GetHeight(bot->GetPhaseShift(), x, y, hintZ + 2.0f, true, 80.0f);
    if (wider > INVALID_HEIGHT && std::fabs(wider - hintZ) <= IndoorSnapSlack)
    {
        outZ = wider + lift;
        return true;
    }

    return false;
}

float GetStandZ(Player* bot, float x, float y, float hintZ, float lift)
{
    float stand = hintZ;
    if (TryGetStandZ(bot, x, y, hintZ, stand, lift))
        return stand;
    return hintZ;
}

bool IsMovingOnFoot(Player const* bot)
{
    return bot && bot->movespline && !bot->movespline->Finalized();
}

bool PathIsWalkable(uint32 type)
{
    if (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
        return false;
    return (type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) != 0;
}

namespace
{
uint32 PickMountSpell(Player* bot)
{
    if (bot->GetLevel() < sPlayerbotsConfig.RpgMountMinLevel)
        return 0;

    // A random bot rides a mount of its own race, never "any mount it knows": collections are account wide and the bots
    // share Battle.net accounts across factions, so a goblin knows the Alliance horse. A bot that follows a player keeps
    // using whatever the character really has.
    PlayerbotAI* ai = sPlayerbotMgr->GetAI(bot);
    if (!ai || !ai->GetMaster())
        return RaceMounts::PickKnown(bot);

    uint32 ground = 0;
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (spell.state == PLAYERSPELL_REMOVED || !spell.active || spell.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || info->IsPassive() || !info->HasAura(SPELL_AURA_MOUNTED))
            continue;
        ground = spellId;
    }
    return ground;
}

float MountDistance(Player* bot)
{
    float reach = bot->GetCombatReach() + 5.0f;
    return std::max(10.5f, reach);
}

// Bots move only along the navmesh (user rule 2026-10-15): no VMAP-only steps and no raw-point splines, which walked
// bots through walls and left them on the wrong floor.
constexpr float SegmentMinProgress = 5.0f;   // a path that ends short of the target must get the bot at least this much closer
constexpr float MeshEndMaxZ = 3.5f;          // a mesh end point or a standing bot this far above/below the target is not "there"
constexpr float HopSpeed = 8.0f;             // yards per second along the ground
constexpr float HopMinHeight = 0.4f;
constexpr float HopMaxHeight = 1.3f;
constexpr uint32 HopCooldownMs = 4000;
constexpr float HopReach = 3.0f;             // a running bot this close to the start of a low obstacle jumps (the timing is estimated)
constexpr float HopMaxReach = 8.0f;          // the longest jump, a mounted run covers more in the air
constexpr float HopMaxLandingDz = 1.5f;
constexpr float HopLeadMs = 80.0f;           // the jump starts this much before the obstacle
constexpr float JumpAirSeconds = 0.824f;     // the standard jump of every race: 7.96 yd/s upward against gravity 19.29
constexpr float NearArriveSlack = 1.5f;      // an NPC behind a counter: the mesh ends here at best; the interaction check decides.
                                             // With the 3.5 yd arrival of rpg walks this stays inside the NPC interaction range
                                             // (the NPC's combat reach + 4 yd)

constexpr uint32 MountRetryMs = 30000;        // per bot between two mount attempts
constexpr float MountLegMin = 35.0f;          // a walk shorter than this is walked, not ridden

std::mutex sMountLock;
std::unordered_map<ObjectGuid, uint32> sMountRetryAt;

std::atomic<uint32> sMeshLaunched{ 0 };
std::atomic<uint32> sMeshNoPath{ 0 };
std::atomic<uint32> sPathsChecked{ 0 };
std::atomic<uint32> sPathsBlocked{ 0 };
std::atomic<uint32> sPathsKneeOnly{ 0 };
std::atomic<uint32> sPathsSteep{ 0 };
std::atomic<uint32> sPathsRepaired{ 0 };
std::atomic<uint32> sPathsRejected{ 0 };
std::atomic<uint32> sHops{ 0 };
std::mutex sHopLock;
std::unordered_map<ObjectGuid, uint32> sHopAt;   // game time ms: when a bot may hop again
std::atomic<uint32> sNextBlockedLog{ 0 };   // game time ms: at most one blocked-path line a second

bool LaunchWalkPath(Player* bot, Movement::PointsArray const& points)
{
    if (points.size() < 2)
        return false;

    bot->GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(bot);
    init.MovebyPath(points);
    init.Launch();
    return true;
}

constexpr float StoneMinProgress = 8.0f;        // a stepping stone must get the bot at least this much closer to the destination
constexpr float StoneSnapXY = 3.0f;             // the stone's mesh point may be this far from the sampled point (2D)
constexpr float StoneSnapZ = 3.0f;              // and this far from the bot's own height: a stone on another floor is no step
constexpr float HeightMergeYards = 1.5f;

// Playerbots.Move.Fallbacks, called when the direct path failed. First the same x,y at the other floors found there (a bridge above
// a road, a city on two levels): the first height with a complete mesh path that ends where it should wins. Then a walk in steps:
// points on cones toward the destination, snapped to the mesh at the bot's own height, the nearest one to the destination that
// gets the bot closer by StoneMinProgress. Each step is a mesh path, and the distance to the destination only shrinks, so the
// repeated calls cannot oscillate. Only for a bot on a loaded grid: a far bot is not walked around
enum class FallbackResult : uint8
{
    None,
    Moving,
    Arrived     // the bot already stands at the destination's x,y on another floor than the given Z
};

FallbackResult TryFallbackStep(Player* bot, Position const& dest, float arriveYards)
{
    Map* map = bot->GetMap();
    if (!map || !map->IsGridLoaded(dest.GetPositionX(), dest.GetPositionY()) || !map->IsGridLoaded(bot->GetPositionX(), bot->GetPositionY()))
        return FallbackResult::None;

    float x = dest.GetPositionX();
    float y = dest.GetPositionY();
    float z = dest.GetPositionZ();

    std::vector<float> heights = { z };
    for (float from : { z + 2.0f, z + 30.0f, z - 4.0f })
    {
        float h = map->GetHeight(bot->GetPhaseShift(), x, y, from, true, 50.0f);
        if (h > INVALID_HEIGHT)
            heights.push_back(h);
    }

    for (float height : DistinctHeights(heights, HeightMergeYards))
    {
        if (std::fabs(height - z) < 0.01f)
            continue;       // the given height is what just failed

        // already there, one floor off the Z the caller gave
        if (bot->GetExactDist2d(x, y) <= arriveYards && std::fabs(bot->GetPositionZ() - height) <= 2.5f)
            return FallbackResult::Arrived;

        PathGenerator path(bot);
        if (!path.CalculatePath(x, y, height, false) || !(path.GetPathType() & PATHFIND_NORMAL) || (path.GetPathType() & PATHFIND_INCOMPLETE))
            continue;

        G3D::Vector3 const& end = path.GetActualEndPosition();
        if (std::hypot(end.x - x, end.y - y) > 3.0f || std::fabs(end.z - height) > 2.5f)
            continue;

        if (path.GetPath().size() >= 2 && path.GetPathLength() > 1.0f && LaunchWalkPath(bot, path.GetPath()))
        {
            ++sMeshLaunched;
            return FallbackResult::Moving;
        }
    }

    std::vector<Point2> stones = ConeStones(bot->GetPositionX(), bot->GetPositionY(), x, y, { 25.0f, 50.0f }, 0.6f, 2);
    // nearest to the destination first among those that qualify; try the next one when the mesh has no point there
    for (int tries = 0; tries < 4; ++tries)
    {
        int pick = BestStone(stones, bot->GetPositionX(), bot->GetPositionY(), x, y, StoneMinProgress);
        if (pick < 0)
            return FallbackResult::None;

        Point2 stone = stones[pick];
        stones.erase(stones.begin() + pick);

        PathGenerator path(bot);
        if (!path.CalculatePath(stone.X, stone.Y, bot->GetPositionZ(), false) || !PathIsWalkable(path.GetPathType()) || path.GetPath().size() < 2)
            continue;

        G3D::Vector3 const& end = path.GetActualEndPosition();
        if (std::hypot(end.x - stone.X, end.y - stone.Y) > StoneSnapXY || std::fabs(end.z - bot->GetPositionZ()) > StoneSnapZ)
            continue;

        // the snapped point, not the sample, has to be the closer one
        if (std::hypot(x - end.x, y - end.y) > std::hypot(x - bot->GetPositionX(), y - bot->GetPositionY()) - StoneMinProgress)
            continue;

        if (LaunchWalkPath(bot, path.GetPath()))
        {
            ++sMeshLaunched;
            return FallbackResult::Moving;
        }
    }
    return FallbackResult::None;
}

MoveResult NoMeshPath(Player* bot, Position const& dest, uint32 type)
{
    ++sMeshNoPath;
    TC_LOG_DEBUG("server.playerbots.stuck", "Playerbots: {} has no mesh path on map {} from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}), path type {}",
        bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
        dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), type);
    return MoveResult::NoPath;
}
}

void UpdateMountForTravel(Player* bot, float distanceToDest, float mountAtOverride)
{
    if (!bot || bot->IsInCombat() || bot->IsInFlight() || bot->IsBeingTeleported() || !bot->IsAlive())
        return;

    // without riding skill a mount cast fails ("not here"); do not scan the spell book on every step for nothing
    if (!bot->IsMounted() && bot->GetSkillValue(SKILL_RIDING) == 0)
        return;

    float mountAt = mountAtOverride > 0.0f ? mountAtOverride : MountDistance(bot);
    float dismountAt = bot->GetCombatReach() + 2.0f;

    if (distanceToDest <= dismountAt)
    {
        if (bot->IsMounted())
            bot->CancelMountAura();
        return;
    }

    if (distanceToDest <= mountAt || bot->IsMounted() || bot->HasUnitState(UNIT_STATE_CASTING))
        return;

    // cities, caves and other rooms: the cast would fail at once and be tried again on the next tick
    if (!bot->IsOutdoors())
        return;

    // one attempt per bot every MountRetryMs, whether or not it works (a success leaves the bot mounted anyway)
    uint32 now = GameTime::GetGameTimeMS();
    {
        std::lock_guard lock(sMountLock);
        if (sMountRetryAt.size() > 4096)
            sMountRetryAt.clear();
        uint32& retryAt = sMountRetryAt[bot->GetGUID()];
        if (now < retryAt)
            return;
        retryAt = now + MountRetryMs;
    }

    uint32 spell = PickMountSpell(bot);
    if (!spell)
        return;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell, DIFFICULTY_NONE);
    if (!info || info->CheckLocation(bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(), bot) != SPELL_CAST_OK)
        return;

    bot->CastSpell(bot, spell);
}

MoveResult MoveAlongMesh(Player* bot, Position const& dest, float arriveYards, float standoff)
{
    if (!bot)
        return MoveResult::NoPath;

    // a split sanctuary's other side is not a walk target (the area id, or the Northrend Dalaran street box)
    if (SpawnPolicy::IsEnemyGroundAt(bot, bot->GetMapId(), dest))
        return MoveResult::NoPath;

    // in the air of a jump: nothing to plan until the bot has landed
    if (IsPlayerJumping(bot))
        return MoveResult::Moving;

    float dist = bot->GetExactDist2d(dest);
    float dz = std::fabs(bot->GetPositionZ() - dest.GetPositionZ());

    // mount up only for a standing start on a long leg (never while a walk is running: the spline would cancel the cast)
    if (!IsMovingOnFoot(bot))
        UpdateMountForTravel(bot, dist, MountLegMin);
    // 2D alone says "there" for a bot under a bridge and a target on it
    if (dist <= arriveYards && dz <= MeshEndMaxZ)
    {
        if (bot->IsMounted())
            bot->CancelMountAura();
        return MoveResult::Arrived;
    }

    // a mount cast and a path launched in the same call interrupt each other: wait for the cast
    if (bot->HasUnitState(UNIT_STATE_CASTING) || IsMovingOnFoot(bot))
        return MoveResult::Moving;

    if (bot->GetVictim())
        bot->AttackStop();

    PathGenerator path(bot);
    bool calculated = path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false);
    uint32 type = path.GetPathType();
    Movement::PointsArray const& points = path.GetPath();
    // The mesh cannot get closer, but the bot already stands next to a target that is off the mesh (an innkeeper behind a
    // counter): that is "there", and the handler's own interaction check decides whether it is close enough
    auto noPathOrNear = [&]()
    {
        if (dist <= arriveYards + NearArriveSlack && dz <= MeshEndMaxZ)
        {
            if (bot->IsMounted())
                bot->CancelMountAura();
            return MoveResult::Arrived;
        }
        if (sPlayerbotsConfig.MoveFallbacks)
        {
            FallbackResult fallback = TryFallbackStep(bot, dest, arriveYards);
            if (fallback == FallbackResult::Moving)
                return MoveResult::Moving;
            if (fallback == FallbackResult::Arrived)
            {
                if (bot->IsMounted())
                    bot->CancelMountAura();
                return MoveResult::Arrived;
            }
        }
        return NoMeshPath(bot, dest, type);
    };

    if (!calculated || !PathIsWalkable(type) || points.size() < 2 || path.GetPathLength() < 0.5f)
        return noPathOrNear();

    // The mesh decides where the walk ends (forceDest = false). That is the target when it lies on the mesh; if the path
    // ends short (a long route is cut at ~300 yards, or the target is off the mesh) it is accepted only as a segment that
    // gets the bot closer, so a target the mesh cannot reach fails instead of looping.
    G3D::Vector3 const& end = points.back();
    float endGap = std::hypot(end.x - dest.GetPositionX(), end.y - dest.GetPositionY());
    bool reaches = endGap <= arriveYards && std::fabs(end.z - dest.GetPositionZ()) <= MeshEndMaxZ;
    if (!reaches && dist - endGap < SegmentMinProgress)
        return noPathOrNear();

    // a stand-off ends the walk short of the target. Only a path that reaches the target is cut (a segment of a long route is not):
    // nothing left to walk means the bot already stands where it should
    Movement::PointsArray const* launch = &points;
    Movement::PointsArray standoffPath;
    if (standoff > 0.0f && reaches)
    {
        standoffPath = TrimPathToStandoff(points, dest.GetPositionX(), dest.GetPositionY(), standoff);
        if (standoffPath.size() < 2)
        {
            if (bot->IsMounted())
                bot->CancelMountAura();
            return MoveResult::Arrived;
        }
        launch = &standoffPath;
    }

    // Playerbots.Move.AvoidBlockedPaths: a path with a cliff segment (a mesh link no walker can take) is refused, one that runs through
    // geometry the mesh does not know gets a detour through other mesh points where one is found
    Movement::PointsArray repaired;
    Movement::PointsArray hopLeg;
    if (sPlayerbotsConfig.MoveAvoidBlockedPaths)
    {
        Movement::PointsArray const& walk = *launch;
        bool cliff = false;
        for (size_t i = 0; i + 1 < walk.size() && i < 60 && !cliff; ++i)
        {
            float run = std::hypot(walk[i + 1].x - walk[i].x, walk[i + 1].y - walk[i].y);
            float rise = std::fabs(walk[i + 1].z - walk[i].z);
            cliff = rise > SteepMinDz && rise > run * SteepSlope;
        }
        if (cliff)
        {
            ++sPathsRejected;
            return noPathOrNear();
        }

        repaired = walk;
        if (RepairBlockedPath(bot, repaired))
        {
            ++sPathsRepaired;
            launch = &repaired;
        }
    }

    // Playerbots.Move.HopLowObstacles: a low obstacle on level ground (a curb, a low fence, a crate) is jumped, on the run, the way a
    // player jumps it. The path is launched whole; a jump is timed for the moment the bot reaches the obstacle and lands one jump's
    // reach further along the path, so the bot keeps its running speed through the jump. Anything that stops the run first cancels it
    if (sPlayerbotsConfig.MoveHopLowObstacles && !bot->IsInWater() && !bot->IsUnderWater() && !bot->IsFalling())
    {
        Movement::PointsArray const& walk = *launch;
        size_t low = FirstLowSegment(bot, walk);
        if (low + 1 < walk.size())
        {
            float speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
            float toObstacle = 0.0f;
            for (size_t i = 0; i < low; ++i)
                toObstacle += std::hypot(walk[i + 1].x - walk[i].x, walk[i + 1].y - walk[i].y);

            // where one jump from the obstacle ends: the run speed times the air time along the path (the mesh point beyond it)
            float reach = std::clamp(speed * JumpAirSeconds, 1.5f, HopMaxReach);
            G3D::Vector3 landing = walk[low + 1];
            float remaining = reach;
            for (size_t i = low; i + 1 < walk.size(); ++i)
            {
                float length = std::hypot(walk[i + 1].x - walk[i].x, walk[i + 1].y - walk[i].y);
                if (length >= remaining && length > 0.01f)
                {
                    float t = remaining / length;
                    landing = walk[i] + (walk[i + 1] - walk[i]) * t;
                    break;
                }
                remaining -= length;
                landing = walk[i + 1];
            }
            if (std::fabs(landing.z - walk[low].z) > HopMaxLandingDz)
                landing = walk[low + 1];

            // a bump in the ground can look like a low obstacle: not a hop after hop
            uint32 now = GameTime::GetGameTimeMS();
            bool allowed = true;
            {
                std::lock_guard lock(sHopLock);
                if (sHopAt.size() > 4096)
                    sHopAt.clear();
                uint32& next = sHopAt[bot->GetGUID()];
                if (int32(now - next) < 0)
                    allowed = false;
                else
                    next = now + HopCooldownMs;
            }

            if (allowed)
            {
                Position from(walk[low].x, walk[low].y, walk[low].z);
                Position to(landing.x, landing.y, landing.z);
                uint32 delay = uint32(std::max(0.0f, toObstacle / speed * 1000.0f - HopLeadMs));
                bot->m_Events.AddEventAtOffset([bot, from, to]()
                {
                    // still running along this path and at the obstacle
                    if (!bot->IsInWorld() || !IsMovingOnFoot(bot) || bot->GetExactDist2d(from) > HopReach)
                        return;
                    if (StartPlayerJump(bot, to))
                        ++sHops;
                }, Milliseconds(delay));
            }
        }
    }

    // Playerbots.Move.LogBlockedPaths: measures how often the mesh leads through geometry it does not know (fences, doors, a Hero's Call board)
    if (sPlayerbotsConfig.MoveLogBlockedPaths)
    {
        ++sPathsChecked;
        PathVerdict verdict = JudgePath(bot, *launch);
        if (verdict.Result == PathVerdict::Kind::KneeOnly)
            ++sPathsKneeOnly;
        else if (verdict.Result == PathVerdict::Kind::EyeBlock || verdict.Result == PathVerdict::Kind::Steep)
        {
            ++(verdict.Result == PathVerdict::Kind::Steep ? sPathsSteep : sPathsBlocked);
            uint32 now = GameTime::GetGameTimeMS();
            if (int32(now - sNextBlockedLog.load(std::memory_order_relaxed)) >= 0)
            {
                sNextBlockedLog.store(now + 1000, std::memory_order_relaxed);
                TC_LOG_INFO("server.playerbots.stuck", "Playerbots: path of {} crosses geometry: map {}, {}; {}", bot->GetName(), bot->GetMapId(), verdict.Text, DescribeStuck(bot, nullptr));
            }
        }
    }

    if (!LaunchWalkPath(bot, *launch))
        return noPathOrNear();

    ++sMeshLaunched;
    return MoveResult::Moving;
}

bool MoveToward(Player* bot, Position const& dest, float arriveYards, float standoff)
{
    return MoveAlongMesh(bot, dest, arriveYards, standoff) == MoveResult::Moving;
}

MeshMoveStats GetMeshMoveStats()
{
    return { sMeshLaunched.load(), sMeshNoPath.load(), sPathsChecked.load(), sPathsBlocked.load(), sPathsKneeOnly.load(), sPathsSteep.load(), sPathsRepaired.load(), sPathsRejected.load(), sHops.load() };
}
}
