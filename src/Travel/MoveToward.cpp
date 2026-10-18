#include "MoveToward.h"
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

bool PathIsWalkable(uint32 type)
{
    if (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
        return false;
    return (type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) != 0;
}

// Bots move only along the navmesh (user rule 2026-10-15): no VMAP-only steps and no raw-point splines, which walked
// bots through walls and left them on the wrong floor.
constexpr float SegmentMinProgress = 5.0f;   // a path that ends short of the target must get the bot at least this much closer
constexpr float MeshEndMaxZ = 3.5f;          // a mesh end point or a standing bot this far above/below the target is not "there"
constexpr float NearArriveSlack = 1.5f;      // an NPC behind a counter: the mesh ends here at best; the interaction check decides.
                                             // With the 3.5 yd arrival of rpg walks this stays inside the NPC interaction range
                                             // (the NPC's combat reach + 4 yd)

constexpr uint32 MountRetryMs = 30000;        // per bot between two mount attempts
constexpr float MountLegMin = 35.0f;          // a walk shorter than this is walked, not ridden

std::mutex sMountLock;
std::unordered_map<ObjectGuid, uint32> sMountRetryAt;

std::atomic<uint32> sMeshLaunched{ 0 };
std::atomic<uint32> sMeshNoPath{ 0 };

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

MoveResult MoveAlongMesh(Player* bot, Position const& dest, float arriveYards)
{
    if (!bot)
        return MoveResult::NoPath;

    // a split sanctuary's other side is not a walk target (the area id, or the Northrend Dalaran street box)
    if (SpawnPolicy::IsEnemyGroundAt(bot, bot->GetMapId(), dest))
        return MoveResult::NoPath;

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

    if (!LaunchWalkPath(bot, points))
        return noPathOrNear();

    ++sMeshLaunched;
    return MoveResult::Moving;
}

bool MoveToward(Player* bot, Position const& dest, float arriveYards)
{
    return MoveAlongMesh(bot, dest, arriveYards) == MoveResult::Moving;
}

MeshMoveStats GetMeshMoveStats()
{
    return { sMeshLaunched.load(), sMeshNoPath.load() };
}
}
