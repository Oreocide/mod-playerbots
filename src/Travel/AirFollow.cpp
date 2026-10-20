/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AirFollow.h"
#include "AirFollowGeometry.h"
#include "Economy/FlyingMounts.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "MoveToward.h"
#include "NavSnap.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace Playerbots
{
namespace AirFollow
{
namespace
{
constexpr float TrailStepYards = 6.0f;        // the master's position is added to the trail after it moved this far
constexpr size_t TrailLength = 160;
constexpr float TrailSpacing = 25.0f;         // yards between the trail points a bot flies through
constexpr size_t TrailPoints = 8;
constexpr uint32 LaunchEveryMs = 900;
constexpr float RelaunchYards = 12.0f;        // the place around the master moved this far: a new spline
constexpr float AtPlaceYards = 4.0f;
constexpr float GroundClearance = 3.0f;       // the lowest a bot flies above the terrain under it
constexpr uint32 MountTries = 4;
constexpr uint32 MountWaitMs = 3500;
constexpr uint32 GiveUpMs = 5 * 60 * 1000;    // a bot that could not get a flying mount follows on the ground for this long
constexpr uint32 LandingGraceMs = 25000;
constexpr uint32 MasterDownMs = 2500;         // a touchdown shorter than this does not bring the bots down
constexpr uint32 MasterOddMs = 60000;         // the master falls, is dead, on a taxi or a transport: the bots hover this long, then land under themselves

enum class Phase : uint8 { None, Boarding, Following, Landing };   // Boarding is on the ground: not "in the air" yet

struct Follower
{
    Phase Stage = Phase::None;
    uint32 NextLaunch = 0;
    uint32 MountWaitUntil = 0;
    uint32 GiveUpUntil = 0;
    uint32 LagSince = 0;
    uint32 MasterDownSince = 0;      // when the master was first seen not flying; the bot lands only after it stayed down a few seconds
    uint32 LandStarted = 0;
    uint32 Tries = 0;
    Vec Target;
    Position Landing;
};

struct Trail
{
    std::deque<Vec> Points;
};

std::mutex Lock;
std::unordered_map<ObjectGuid, Follower> Followers;
std::unordered_map<ObjectGuid, Trail> Trails;

Vec ToVec(Position const& p) { return { p.GetPositionX(), p.GetPositionY(), p.GetPositionZ() }; }

Follower& State(ObjectGuid bot)
{
    if (Followers.size() > 4096)
        Followers.clear();
    return Followers[bot];
}

void RecordTrail(Player* master)
{
    Vec now = ToVec(master->GetPosition());
    std::lock_guard lock(Lock);
    if (Trails.size() > 256)
        Trails.clear();
    Trail& trail = Trails[master->GetGUID()];
    if (trail.Points.empty() || Dist3d(trail.Points.back(), now) >= TrailStepYards)
    {
        trail.Points.push_back(now);
        if (trail.Points.size() > TrailLength)
            trail.Points.pop_front();
    }
}

std::vector<Vec> TrailOf(ObjectGuid master)
{
    std::lock_guard lock(Lock);
    auto itr = Trails.find(master);
    return itr == Trails.end() ? std::vector<Vec>() : std::vector<Vec>(itr->second.Points.begin(), itr->second.Points.end());
}

float GroundAt(Player* bot, float x, float y, float z)
{
    return bot->GetMap()->GetHeight(bot->GetPhaseShift(), x, y, z + 100.0f, true, 600.0f);
}

bool Clear(Player* bot, Vec a, Vec b)
{
    return bot->GetMap()->isInLineOfSight(bot->GetPhaseShift(), a.X, a.Y, a.Z + 2.0f, b.X, b.Y, b.Z + 2.0f, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
}

// a point is never planned lower than GroundClearance above the terrain under it
Vec Lifted(Player* bot, Vec p)
{
    float ground = GroundAt(bot, p.X, p.Y, p.Z);
    if (ground > INVALID_HEIGHT && p.Z < ground + GroundClearance)
        p.Z = ground + GroundClearance;
    return p;
}

// the bot's own position first, points less than a yard apart dropped (the spline validator refuses a path with two points that close)
bool Launch(Player* bot, std::vector<Vec> const& route, float speed)
{
    Movement::PointsArray points;
    points.emplace_back(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    for (Vec const& v : route)
    {
        G3D::Vector3 p(v.X, v.Y, v.Z);
        if ((p - points.back()).length() >= 1.0f)
            points.push_back(p);
    }
    if (points.size() < 2)
        return false;

    bot->GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(bot);
    init.MovebyPath(points);
    init.SetFly();
    init.SetUncompressed();
    init.SetVelocity(speed);
    return init.Launch() > 0;
}

bool IsUp(Player* bot)
{
    if (!bot->IsMounted() || !bot->CanFly())
        return false;
    float ground = GroundAt(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    return ground <= INVALID_HEIGHT || bot->GetPositionZ() - ground > GroundClearance;
}

// The ground under a point, probed from above: what a bot may be put down on when nothing walkable is near
Position GroundPoint(Player* bot, float x, float y, float z)
{
    float ground = GroundAt(bot, x, y, z);
    return Position(x, y, ground > INVALID_HEIGHT ? ground + 0.5f : z);
}

// Straight down onto the navmesh point under the bot (or the ground under it when the mesh has nothing there). A frozen bot is placed:
// nobody sees it, and the frozen branch of PlayerbotAI::Update would cancel a descent spline
bool GroundNow(PlayerbotAI& ai, bool frozen, char const* reason)
{
    Player* bot = ai.GetBot();
    if (!bot->IsInWorld() || bot->IsBeingTeleported())
        return false;

    if (!bot->IsMounted() || !bot->CanFly())
    {
        Forget(bot->GetGUID());
        return false;
    }

    float ground = GroundAt(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (ground <= INVALID_HEIGHT || bot->GetPositionZ() - ground <= GroundClearance)
    {
        bot->CancelMountAura();
        bot->SetFallInformation(0, bot->GetPositionZ());
        Forget(bot->GetGUID());
        return false;
    }

    ai.NoteExpectedMove();
    if (!frozen && IsMovingOnFoot(bot))
        return true;

    Position below;
    if (!SnapPointToNavMesh(bot, Position(bot->GetPositionX(), bot->GetPositionY(), ground), below, 12.0f, 8.0f))
        below = GroundPoint(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    TC_LOG_INFO("server.playerbots.air", "Playerbots: {} comes down ({}) at ({:.0f}, {:.0f}, {:.0f})", bot->GetName(), reason, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (frozen)
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        bot->CancelMountAura();
        bot->NearTeleportTo(below.GetPositionX(), below.GetPositionY(), below.GetPositionZ(), bot->GetOrientation());
        bot->SetFallInformation(0, below.GetPositionZ());
        Forget(bot->GetGUID());
        return false;
    }

    {
        std::lock_guard lock(Lock);
        Follower& follower = State(bot->GetGUID());
        follower.Stage = Phase::Landing;
        follower.Landing = below;
        follower.LandStarted = GameTime::GetGameTimeMS();
        follower.Tries = 0;
    }
    return Launch(bot, { ToVec(below) }, bot->GetSpeed(MOVE_FLIGHT));
}

void FollowInAir(PlayerbotAI& ai, Player* master, Follower& follower, uint32 now)
{
    Player* bot = ai.GetBot();
    ai.NoteExpectedMove();

    Vec place = ToVec(master->GetPosition());
    Vec offset = FormationOffset(bot->GetGUID().GetCounter(), master->GetOrientation());
    place = { place.X + offset.X, place.Y + offset.Y, place.Z + offset.Z };
    Vec here = ToVec(bot->GetPosition());
    float distance = Dist3d(here, ToVec(master->GetPosition()));

    // far behind for a while: placed in the air behind the master, still mounted (a summon)
    if (distance > float(sPlayerbotsConfig.AirFollowCatchUpYards))
    {
        if (!follower.LagSince)
            follower.LagSince = now;
        else if (now - follower.LagSince > sPlayerbotsConfig.AirFollowCatchUpSeconds * 1000)
        {
            follower.LagSince = 0;
            Vec lifted = Lifted(bot, place);
            TC_LOG_DEBUG("server.playerbots.air", "Playerbots: {} placed behind its master in the air ({:.0f} yd behind)", bot->GetName(), distance);
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            bot->NearTeleportTo(lifted.X, lifted.Y, lifted.Z, master->GetOrientation());
            follower.NextLaunch = now + LaunchEveryMs;
            follower.Target = lifted;
            return;
        }
    }
    else
        follower.LagSince = 0;

    bool moving = IsMovingOnFoot(bot);
    if (now < follower.NextLaunch)
        return;
    if (moving && Dist3d(place, follower.Target) <= RelaunchYards)
        return;
    if (!moving && Dist3d(here, place) <= AtPlaceYards)
    {
        follower.Target = place;
        return;
    }

    // the way: straight to the place when it is close and clear, otherwise along the master's own flight path
    std::vector<Vec> route;
    Vec lifted = Lifted(bot, place);
    if (distance < 60.0f && Clear(bot, here, lifted))
        route.push_back(lifted);
    else
    {
        for (Vec const& point : TrailAhead(TrailOf(master->GetGUID()), here, TrailSpacing, TrailPoints))
            route.push_back(Lifted(bot, point));
        if (route.empty() || Clear(bot, route.back(), lifted))
            route.push_back(lifted);
    }

    float speed = bot->GetSpeed(MOVE_FLIGHT) * (distance > 60.0f ? 1.15f : 1.0f);
    follower.NextLaunch = now + LaunchEveryMs;
    follower.Target = place;
    Launch(bot, route, speed);
}

bool Land(PlayerbotAI& ai, Player* master, Follower& follower, uint32 now)
{
    Player* bot = ai.GetBot();
    ai.NoteExpectedMove();

    if (!bot->IsMounted() || !bot->CanFly())
    {
        follower = Follower();
        return false;
    }

    if (follower.Stage != Phase::Landing)
    {
        // the master is down: a navmesh point next to it (not on it), or on it when nothing next to it is walkable
        Vec offset = FormationOffset(bot->GetGUID().GetCounter(), master->GetOrientation(), 2.0f, 6.0f);
        Position wanted(master->GetPositionX() + offset.X, master->GetPositionY() + offset.Y, master->GetPositionZ());
        Position snapped;
        if (!SnapPointToNavMesh(bot, wanted, snapped, 6.0f, 10.0f) && !SnapPointToNavMesh(bot, master->GetPosition(), snapped, 8.0f, 12.0f))
            snapped = GroundPoint(bot, wanted.GetPositionX(), wanted.GetPositionY(), master->GetPositionZ());   // the ground next to the master, probed from above

        std::vector<Vec> route;
        Vec target = ToVec(snapped);
        if (bot->GetPositionZ() > target.Z + 30.0f)
            route.push_back({ target.X, target.Y, target.Z + 25.0f });     // over the spot first, then straight down
        route.push_back(target);
        follower.Stage = Phase::Landing;
        follower.Landing = snapped;
        follower.LandStarted = now;
        follower.Tries = 0;
        if (!Launch(bot, route, bot->GetSpeed(MOVE_FLIGHT)))
        {
            // the spline was refused (the bot is already over the spot): it is put on the ground point itself
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            bot->CancelMountAura();
            bot->NearTeleportTo(snapped.GetPositionX(), snapped.GetPositionY(), snapped.GetPositionZ(), master->GetOrientation());
            follower = Follower();
            return false;
        }
        return true;
    }

    if (IsMovingOnFoot(bot) && now - follower.LandStarted < LandingGraceMs)
        return true;

    // the descent ended: it counts only on the landing point; otherwise once more, then the bot is placed there
    if (bot->GetExactDist2d(follower.Landing) > 3.0f || std::fabs(bot->GetPositionZ() - follower.Landing.GetPositionZ()) > 3.0f)
    {
        if (++follower.Tries <= 1 && Launch(bot, { ToVec(follower.Landing) }, bot->GetSpeed(MOVE_FLIGHT)))
            return true;
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        bot->NearTeleportTo(follower.Landing.GetPositionX(), follower.Landing.GetPositionY(), follower.Landing.GetPositionZ(), master->GetOrientation());
    }

    bot->CancelMountAura();
    bot->SetFallInformation(0, bot->GetPositionZ());
    follower = Follower();
    return false;
}
}

bool Active(ObjectGuid bot)
{
    std::lock_guard lock(Lock);
    auto itr = Followers.find(bot);
    return itr != Followers.end() && itr->second.Stage != Phase::None;
}

void Forget(ObjectGuid bot)
{
    std::lock_guard lock(Lock);
    Followers.erase(bot);
}

bool Update(PlayerbotAI& ai, Player* master)
{
    if (!sPlayerbotsConfig.AirFollow || !master)
        return false;

    Player* bot = ai.GetBot();
    if (!bot->IsInWorld() || !master->IsInWorld() || master->GetMap() != bot->GetMap() || bot->GetMap()->Instanceable())
        return false;       // Tick brings a bot that is up and has lost its master down

    uint32 now = GameTime::GetGameTimeMS();
    bool masterUp = master->IsMounted() && master->IsFlying() && !master->IsInFlight() && !master->GetTransport();
    if (masterUp)
        RecordTrail(master);

    Follower follower;
    {
        std::lock_guard lock(Lock);
        if (auto itr = Followers.find(bot->GetGUID()); itr != Followers.end())
            follower = itr->second;
    }
    bool botUp = follower.Stage == Phase::Following || follower.Stage == Phase::Landing;
    bool boarding = follower.Stage == Phase::Boarding;
    if (!masterUp && !botUp && !boarding)
    {
        std::lock_guard lock(Lock);
        Trails.erase(master->GetGUID());
        if (auto itr = Followers.find(bot->GetGUID()); itr != Followers.end() && itr->second.GiveUpUntil <= now)
            Followers.erase(itr);
        return false;
    }
    if (!bot->IsAlive() || bot->IsBeingTeleported())
        return botUp;

    bool handled = true;
    if (masterUp)
    {
        follower.MasterDownSince = 0;
        if (follower.Stage == Phase::Landing)
            follower.Stage = Phase::Following;      // the master took off again while the bots came down: they go back up (a spline is relaunched)
        if (!(bot->IsMounted() && bot->CanFly()))
        {
            if (now < follower.GiveUpUntil)
                handled = false;           // no flying mount for this bot: it waits on the ground (the ordinary follow)
            else if (now < follower.MountWaitUntil)
                handled = true;
            else
            {
                follower.Stage = Phase::Boarding;
                FlyingMounts::MountState mount = FlyingMounts::EnsureMounted(bot, sPlayerbotsConfig.AirFollowGrant);
                if (mount == FlyingMounts::MountState::Cannot || follower.Tries >= MountTries)
                {
                    TC_LOG_INFO("server.playerbots.air", "Playerbots: {} cannot follow its master into the air ({})", bot->GetName(),
                        mount == FlyingMounts::MountState::Cannot ? "no flying mount here" : "the mount gave no flight");
                    follower.GiveUpUntil = now + GiveUpMs;
                    follower.Tries = 0;
                    follower.Stage = Phase::None;
                    handled = false;
                }
                else
                {
                    ++follower.Tries;
                    follower.MountWaitUntil = now + MountWaitMs;
                }
            }
        }
        else
        {
            follower.Tries = 0;
            follower.Stage = Phase::Following;
            FollowInAir(ai, master, follower, now);
        }
    }
    else if (boarding)
    {
        follower = Follower();      // the master came down before the bot was up: nothing to follow
        handled = false;
    }
    else
    {
        // The master is not flying. Down for good (alive, not falling, not on a taxi or a transport) for a few seconds: the bot lands next
        // to it. A touchdown shorter than that, or a master that falls, is dead, rides a taxi or a transport: the bot hovers (a minute at most)
        bool masterGrounded = master->IsAlive() && !master->IsFalling() && !master->IsInFlight() && !master->GetTransport();
        if (!follower.MasterDownSince)
            follower.MasterDownSince = now ? now : 1;
        if (follower.Stage == Phase::Landing)
            handled = Land(ai, master, follower, now);
        else if (masterGrounded && now - follower.MasterDownSince >= MasterDownMs)
            handled = Land(ai, master, follower, now);
        else if (now - follower.MasterDownSince >= MasterOddMs)
        {
            follower.Stage = Phase::Following;
            handled = GroundNow(ai, false, "the master is not on the ground");
            follower = Follower();
        }
        else
            ai.NoteExpectedMove();
    }

    std::lock_guard lock(Lock);
    bool keep = follower.Stage != Phase::None || follower.GiveUpUntil > now;
    if (!keep)
        Followers.erase(bot->GetGUID());
    else
        State(bot->GetGUID()) = follower;
    return handled;
}

void Tick(PlayerbotAI& ai, bool frozen)
{
    Player* bot = ai.GetBot();
    if (!Active(bot->GetGUID()))
        return;

    if (!bot->IsAlive())
    {
        Forget(bot->GetGUID());
        return;
    }

    Player* master = ai.GetMaster();
    bool orphan = frozen || !master || !master->IsInWorld() || master->GetMap() != bot->GetMap() || bot->GetMap()->Instanceable();
    if (orphan)
        GroundNow(ai, frozen, frozen ? "frozen in the air" : "its master is gone");
}

bool EmergencyLand(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!bot->IsInWorld() || !bot->IsMounted() || !bot->CanFly())
        return false;

    float ground = GroundAt(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (ground <= INVALID_HEIGHT || bot->GetPositionZ() - ground <= GroundClearance)
    {
        Forget(bot->GetGUID());
        return false;           // on the ground: the caller dismounts
    }

    ai.NoteExpectedMove();
    if (IsMovingOnFoot(bot))
        return true;

    Position below;
    if (!SnapPointToNavMesh(bot, Position(bot->GetPositionX(), bot->GetPositionY(), ground), below, 12.0f, 8.0f))
        return false;           // nothing walkable below: the old behaviour (dismount) is all that is left

    {
        std::lock_guard lock(Lock);
        Follower& follower = State(bot->GetGUID());
        follower.Stage = Phase::Landing;
        follower.Landing = below;
        follower.LandStarted = GameTime::GetGameTimeMS();
    }
    return Launch(bot, { ToVec(below) }, bot->GetSpeed(MOVE_FLIGHT));
}
}
}
