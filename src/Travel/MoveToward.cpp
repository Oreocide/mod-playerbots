#include "MoveToward.h"
#include "BotPerf.h"
#include "CollectionMgr.h"
#include "DB2Stores.h"
#include "GridDefines.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotsConfig.h"
#include "Random.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

namespace Playerbots
{
constexpr float IndoorSnapSlack = 40.0f;

bool TryGetStandZ(Player* bot, float x, float y, float hintZ, float& outZ)
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
        uint8 cls = bot->GetSession()->GetBotPerfClass();
        if ((cls == BotPerf::CLS_BOT_FROZEN || cls == BotPerf::CLS_BOT_SIM_ROTATION) && !map->IsGridLoaded(x, y))
        {
            loadGrid = false;
            if (BotPerf::On())
                BotPerf::Add(BotPerf::GATE_HEIGHT_NO_LOAD, cls, 0, 1);
        }
    }
    if (loadGrid)
        map->LoadGrid(x, y);

    float local = map->GetHeight(bot->GetPhaseShift(), x, y, hintZ + 2.0f, true, 50.0f);
    if (local > INVALID_HEIGHT && std::fabs(local - hintZ) <= IndoorSnapSlack)
    {
        outZ = local + 0.25f;
        return true;
    }

    float wider = map->GetHeight(bot->GetPhaseShift(), x, y, hintZ + 2.0f, true, 80.0f);
    if (wider > INVALID_HEIGHT && std::fabs(wider - hintZ) <= IndoorSnapSlack)
    {
        outZ = wider + 0.25f;
        return true;
    }

    return false;
}

float GetStandZ(Player* bot, float x, float y, float hintZ)
{
    float stand = hintZ;
    if (TryGetStandZ(bot, x, y, hintZ, stand))
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
    float reach = bot->GetCombatReach() + 10.0f;
    return std::max(21.0f, reach);
}

bool PathIsWalkable(uint32 type)
{
    if (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
        return false;
    return (type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) != 0;
}

bool LaunchWalkPath(Player* bot, PathGenerator const& path)
{
    if (path.GetPath().size() < 2)
        return false;

    bot->GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(bot);
    init.MovebyPath(path.GetPath());
    init.Launch();
    return true;
}
}

void UpdateMountForTravel(Player* bot, float distanceToDest)
{
    if (!bot || bot->IsInCombat() || bot->IsInFlight() || bot->IsBeingTeleported() || !bot->IsAlive())
        return;

    float mountAt = MountDistance(bot);
    float dismountAt = bot->GetCombatReach() + 2.0f;

    if (distanceToDest <= dismountAt)
    {
        if (bot->IsMounted())
            bot->CancelMountAura();
        return;
    }

    if (distanceToDest <= mountAt || bot->IsMounted() || bot->HasUnitState(UNIT_STATE_CASTING))
        return;

    uint32 spell = PickMountSpell(bot);
    if (!spell)
        return;

    bot->CastSpell(bot, spell);
}

bool MoveToward(Player* bot, Position const& dest, float arriveYards)
{
    if (!bot)
        return false;

    float dist = bot->GetExactDist2d(dest);
    UpdateMountForTravel(bot, dist);
    if (dist <= arriveYards)
    {
        if (bot->IsMounted())
            bot->CancelMountAura();
        return false;
    }

    if (IsMovingOnFoot(bot))
        return true;

    if (bot->GetVictim())
        bot->AttackStop();

    float destZ = GetStandZ(bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());

    PathGenerator path(bot);
    if (path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), destZ, false)
        && PathIsWalkable(path.GetPathType()) && path.GetPathLength() >= 0.5f)
        return LaunchWalkPath(bot, path);

    // No off-mesh bee-line. Step as far as VMAP/mmaps allow toward the destination.
    float step = std::min(dist, 18.0f);
    Position pos = bot->GetPosition();
    float relAngle = bot->GetAbsoluteAngle(&dest) - bot->GetOrientation();
    bot->MovePositionToFirstCollision(pos, step, relAngle);
    pos.m_positionZ = GetStandZ(bot, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
    if (bot->GetExactDist2d(pos) < 1.0f)
        return true;

    if (!bot->IsWithinLOS(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()))
        return true;

    PathGenerator stepPath(bot);
    if (stepPath.CalculatePath(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), false)
        && PathIsWalkable(stepPath.GetPathType()) && stepPath.GetPathLength() >= 0.5f)
        return LaunchWalkPath(bot, stepPath);

    bot->GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(bot);
    init.MoveTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), true);
    init.Launch();
    return true;
}
}
