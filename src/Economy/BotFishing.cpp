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

#include "BotFishing.h"
#include "AiRegistry.h"
#include "BotLootWatch.h"
#include "Engine.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ProfessionsConfig.h"
#include "Random.h"
#include "RpgInfo.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Travel/BotMover.h"
#include "Travel/MoveToward.h"
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace Playerbots::Prof::Fishing
{
namespace
{
// above questing (20), the rpg routine (11) and following (10); a fight, a heal, eating and looting come first
constexpr float FishRelevance = 21.0f;

// retail: "Fishing" (131474) is a dummy whose script (spell_gen_fishing) casts the bobber spell, 131476 without a pole, 131490 with one
constexpr uint32 SpellFishing = 131474;
constexpr uint32 SpellBobberNoPole = 131476;
constexpr uint32 SpellBobberPole = 131490;

constexpr uint32 BobberTimeoutMs = 35000;       // the channel lasts at most 30 s
constexpr uint32 MasterGraceMs = 20000;         // a master between two casts is still fishing
constexpr float MasterReach = 25.0f;            // a party bot fishes this close to its master
constexpr uint32 SessionMaxMs = 6 * MINUTE * IN_MILLISECONDS;

struct FishSpells
{
    uint32 Cast = 0;
    std::vector<uint32> Bobbers;
    float MinRange = 10.0f;
    float MaxRange = 20.0f;
};

void ReadRange(FishSpells& spells, uint32 spellId)
{
    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE))
    {
        auto [minRange, maxRange] = info->GetMinMaxRange(true);
        if (maxRange > 1.0f)
        {
            spells.MinRange = std::max(minRange, 2.0f);
            spells.MaxRange = std::max(maxRange, spells.MinRange + 2.0f);
        }
    }
}

bool SummonsBobber(SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (effect.Effect != SPELL_EFFECT_TRANS_DOOR)
            continue;
        GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(uint32(effect.MiscValue));
        if (go && go->type == GAMEOBJECT_TYPE_FISHINGNODE)
            return true;
    }
    return false;
}

FishSpells FindFishing(Player const* bot)
{
    FishSpells spells;
    if (bot->HasSpell(SpellFishing))
    {
        spells.Cast = SpellFishing;
        spells.Bobbers = { SpellBobberNoPole, SpellBobberPole };
        ReadRange(spells, SpellBobberNoPole);
        return spells;
    }
    // older or custom data: a known spell that summons the bobber itself
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (info && !info->IsPassive() && SummonsBobber(info))
        {
            spells.Cast = spellId;
            spells.Bobbers = { spellId };
            ReadRange(spells, spellId);
            return spells;
        }
    }
    return spells;
}

bool IsFishingSpell(SpellInfo const* info)
{
    return info && (info->Id == SpellFishing || info->Id == SpellBobberNoPole || info->Id == SpellBobberPole || SummonsBobber(info));
}

bool MasterFishingNow(Player* master)
{
    if (Spell* channel = master->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return IsFishingSpell(channel->GetSpellInfo());
    return false;
}

// ---- water ----

/// Water deep enough for the bobber at (x, y), the way TARGET_DEST_CASTER_FISHING checks it (more than 0.75 yd over the ground),
/// near the height of someone standing at refZ
bool FishableAt(Player* bot, float x, float y, float refZ, float& level)
{
    Map* map = bot->GetMap();
    LiquidData data;
    if (map->GetLiquidStatus(bot->GetPhaseShift(), x, y, refZ + 2.0f, {}, &data, bot->GetCollisionHeight()) == LIQUID_MAP_NO_WATER)
        return false;
    if (data.level > refZ + 2.0f || data.level < refZ - 25.0f)
        return false;
    float ground = map->GetHeight(bot->GetPhaseShift(), x, y, data.level + 1.0f, true, 100.0f);
    if (data.level <= ground + 1.0f)
        return false;
    level = data.level;
    return true;
}

/// From `stand`, facing `angle`: the core drops the bobber 17.5 degrees either side of the facing, between the spell's ranges. The
/// corners and the middle of that patch must all be water, and the middle in sight
bool FacesWater(Player* bot, Position const& stand, float angle, FishSpells const& spells)
{
    float const dists[] = { spells.MinRange + 0.5f, (spells.MinRange + spells.MaxRange) * 0.5f, spells.MaxRange - 0.5f };
    float const spread[] = { -0.28f, 0.0f, 0.28f };
    float midX = 0.0f, midY = 0.0f, midLevel = 0.0f;
    for (float dist : dists)
    {
        for (float delta : spread)
        {
            float x = stand.GetPositionX() + std::cos(angle + delta) * dist;
            float y = stand.GetPositionY() + std::sin(angle + delta) * dist;
            float level = 0.0f;
            if (!FishableAt(bot, x, y, stand.GetPositionZ(), level))
                return false;
            if (delta == 0.0f && dist == dists[1])
            {
                midX = x;
                midY = y;
                midLevel = level;
            }
        }
    }
    return bot->GetMap()->isInLineOfSight(bot->GetPhaseShift(), stand.GetPositionX(), stand.GetPositionY(), stand.GetPositionZ() + 2.0f,
        midX, midY, midLevel + 0.5f, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
}

constexpr uint32 Directions = 16;

/// A facing from `stand` that fishes, or a negative value
float FishingFacing(Player* bot, Position const& stand, FishSpells const& spells)
{
    float start = frand(0.0f, 2.0f * float(M_PI));
    for (uint32 i = 0; i < Directions; ++i)
    {
        float angle = Position::NormalizeOrientation(start + 2.0f * float(M_PI) * float(i) / float(Directions));
        if (FacesWater(bot, stand, angle, spells))
            return angle;
    }
    return -1.0f;
}

/// A shore spot on the navmesh within `searchYards` of the bot (and within MasterReach of `anchor` when there is one), with its facing
bool FindShore(Player* bot, FishSpells const& spells, float searchYards, Player* anchor, Position& spot, float& facing)
{
    Position here = bot->GetPosition();
    if (!anchor || here.GetExactDist2d(anchor) <= MasterReach)
    {
        float angle = FishingFacing(bot, here, spells);
        if (angle >= 0.0f)
        {
            spot = here;
            facing = angle;
            return true;
        }
    }

    float start = frand(0.0f, 2.0f * float(M_PI));
    for (float dist = 5.0f; dist <= searchYards + spells.MaxRange; dist += 3.0f)
    {
        for (uint32 i = 0; i < Directions; ++i)
        {
            float angle = Position::NormalizeOrientation(start + 2.0f * float(M_PI) * float(i) / float(Directions));
            float waterX = here.GetPositionX() + std::cos(angle) * dist;
            float waterY = here.GetPositionY() + std::sin(angle) * dist;
            float level = 0.0f;
            if (!FishableAt(bot, waterX, waterY, here.GetPositionZ(), level))
                continue;

            // water this way: step back towards the bot to a dry spot the bobber still reaches
            for (float back = spells.MinRange + 1.0f; back <= spells.MaxRange - 1.0f; back += 1.5f)
            {
                if (back > dist)
                    break;
                Position wanted(waterX - std::cos(angle) * back, waterY - std::sin(angle) * back, here.GetPositionZ());
                if (here.GetExactDist2d(&wanted) > searchYards || (anchor && anchor->GetExactDist2d(&wanted) > MasterReach))
                    continue;
                Position snapped;
                if (!SnapToMesh(bot, wanted, snapped, 2.5f, 6.0f))
                    continue;
                float standLevel = 0.0f;
                if (FishableAt(bot, snapped.GetPositionX(), snapped.GetPositionY(), snapped.GetPositionZ(), standLevel)
                    && standLevel > snapped.GetPositionZ() + 0.5f)
                    continue;   // standing in the water itself
                if (FacesWater(bot, snapped, angle, spells))
                {
                    spot = snapped;
                    facing = angle;
                    return true;
                }
            }
            break;  // one look per direction ring is enough: try the next distance
        }
    }
    return false;
}

// ---- sessions ----

enum class Phase : uint8
{
    Seek,
    Walk,
    Face,
    Cast,
    Wait
};

struct Session
{
    Phase Step = Phase::Seek;
    bool WithMaster = false;
    Position Spot;
    float Facing = 0.0f;
    uint32 CastsLeft = 0;
    uint32 WaitUntil = 0;
    uint32 Started = 0;
    uint32 MasterSeen = 0;
};

std::mutex Lock;
std::unordered_map<ObjectGuid, Session> Sessions;
std::unordered_map<ObjectGuid, uint32> NextCheck;

bool HasSession(ObjectGuid bot)
{
    std::lock_guard guard(Lock);
    return Sessions.contains(bot);
}

void StartSession(Player* bot, bool withMaster)
{
    Session session;
    session.WithMaster = withMaster;
    session.CastsLeft = urand(sProfConfig.FishingCastsMin, sProfConfig.FishingCastsMax);
    session.Started = GameTime::GetGameTimeMS();
    session.MasterSeen = session.Started;
    std::lock_guard guard(Lock);
    Sessions[bot->GetGUID()] = session;
}

void EndSession(Player* bot)
{
    std::lock_guard guard(Lock);
    Sessions.erase(bot->GetGUID());
    NextCheck[bot->GetGUID()] = GameTime::GetGameTimeMS() + sProfConfig.FishingCheckMs;
}

bool CheckDue(Player* bot)
{
    uint32 now = GameTime::GetGameTimeMS();
    std::lock_guard guard(Lock);
    auto [itr, inserted] = NextCheck.try_emplace(bot->GetGUID(), now + urand(sProfConfig.FishingCheckMs / 4, sProfConfig.FishingCheckMs));
    if (inserted || int32(now - itr->second) < 0)
        return false;
    itr->second = now + sProfConfig.FishingCheckMs;
    return true;
}

bool IdleMoment(PlayerbotAI& ai)
{
    switch (ai.GetRpg().Status)
    {
        case RpgStatus::Idle:
        case RpgStatus::Rest:
        case RpgStatus::WanderRandom:
        case RpgStatus::GoCamp:
            return true;
        default:
            return false;
    }
}

bool FishingDueTrigger(PlayerbotAI& ai)
{
    if (!sProfConfig.Fishing)
        return false;
    Player* bot = ai.GetBot();
    if (!bot->HasSkill(SKILL_FISHING) || !bot->IsAlive() || bot->IsInCombat())
        return false;
    if (HasSession(bot->GetGUID()))
        return true;
    if (bot->IsInFlight() || bot->GetVehicle() || bot->GetTransport() || !ai.IsSimulated())
        return false;

    if (Player* master = ai.GetMaster())
        return sProfConfig.FishingWithMaster && master->IsInWorld() && master->GetMapId() == bot->GetMapId()
            && bot->GetExactDist2d(master) <= 40.0f && MasterFishingNow(master);

    return IdleMoment(ai) && CheckDue(bot) && roll_chance(float(sProfConfig.FishingChancePct));
}

class FishAction final : public Action
{
public:
    char const* GetName() const override { return "fish"; }
    bool IsUseful(PlayerbotAI& ai) override { return ai.GetBot()->IsAlive() && !ai.GetBot()->IsInCombat(); }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        uint32 now = GameTime::GetGameTimeMS();

        Session session;
        bool known;
        {
            std::lock_guard guard(Lock);
            auto itr = Sessions.find(bot->GetGUID());
            known = itr != Sessions.end();
            if (known)
                session = itr->second;
        }
        if (!known)
        {
            StartSession(bot, ai.GetMaster() != nullptr);
            std::lock_guard guard(Lock);
            session = Sessions[bot->GetGUID()];
        }

        bool keep = Step(ai, session, now);
        if (!keep)
        {
            EndSession(bot);
            return false;
        }
        std::lock_guard guard(Lock);
        if (auto itr = Sessions.find(bot->GetGUID()); itr != Sessions.end())
            itr->second = session;
        return true;
    }

private:
    static bool Step(PlayerbotAI& ai, Session& session, uint32 now)
    {
        Player* bot = ai.GetBot();
        Player* master = session.WithMaster ? ai.GetMaster() : nullptr;
        if (session.WithMaster)
        {
            if (!master || master->GetMapId() != bot->GetMapId() || bot->GetExactDist2d(master) > MasterReach + 15.0f)
                return false;
            if (MasterFishingNow(master))
                session.MasterSeen = now;
            else if (now - session.MasterSeen > MasterGraceMs)
                return false;
        }
        else if (now - session.Started > SessionMaxMs || !ai.IsSimulated())
            return false;

        if (session.WaitUntil && int32(now - session.WaitUntil) < 0)
            return true;
        session.WaitUntil = 0;

        FishSpells spells = FindFishing(bot);
        if (!spells.Cast)
            return false;

        switch (session.Step)
        {
            case Phase::Seek:
            {
                if (!FindShore(bot, spells, sProfConfig.FishingSearchYards, master, session.Spot, session.Facing))
                    return false;
                session.Step = bot->GetExactDist2d(&session.Spot) > 1.5f ? Phase::Walk : Phase::Face;
                return true;
            }
            case Phase::Walk:
            {
                MoveResult result = MoveAlongMesh(bot, session.Spot, 1.0f);
                if (result == MoveResult::NoPath)
                    return false;
                if (result == MoveResult::Moving)
                    return true;
                // the mesh end can be a little off the wanted spot: check the water from where the bot really stands
                float facing = FacesWater(bot, bot->GetPosition(), session.Facing, spells) ? session.Facing : FishingFacing(bot, bot->GetPosition(), spells);
                if (facing < 0.0f)
                    return false;
                session.Facing = facing;
                session.Step = Phase::Face;
                return true;
            }
            case Phase::Face:
            {
                if (bot->IsMounted())
                    bot->CancelMountAura();
                if (bot->GetStandState() != UNIT_STAND_STATE_STAND)
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                bot->StopMoving();
                // the server places the bobber from the caster's orientation, the clients see the turn
                bot->SetOrientation(session.Facing);
                bot->SetFacingTo(session.Facing);
                session.Step = Phase::Cast;
                session.WaitUntil = now + urand(500, 1200);
                return true;
            }
            case Phase::Cast:
            {
                if (bot->HasUnitState(UNIT_STATE_CASTING))
                    return true;
                if (bot->isMoving() || bot->GetExactDist2d(&session.Spot) > 3.0f)
                {
                    session.Step = Phase::Seek;
                    return true;
                }
                bot->SetOrientation(session.Facing);
                if (bot->CastSpell(bot, spells.Cast) != SPELL_CAST_OK)
                    return false;
                LootWatch::WatchBobber(bot, spells.Bobbers, BobberTimeoutMs);
                session.Step = Phase::Wait;
                session.WaitUntil = now + 1500;
                return true;
            }
            case Phase::Wait:
            {
                if (LootWatch::IsFishing(bot->GetGUID()) || bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                    return true;
                if (!session.WithMaster && session.CastsLeft <= 1)
                    return false;
                if (session.CastsLeft > 1)
                    --session.CastsLeft;
                session.Step = Phase::Cast;
                session.WaitUntil = now + urand(1500, 4500);
                return true;
            }
        }
        return false;
    }
};
}

void RegisterFishingAi(AiRegistry& registry)
{
    registry.AddTrigger("fishing due", 0, FishingDueTrigger);
    registry.AddAction<FishAction>("fish");
}

std::vector<TriggerNode> FishingTriggerNodes()
{
    return { { "fishing due", { { "fish", FishRelevance } }, StrategyStates::NonCombat } };
}

bool Order(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    if (!sProfConfig.Fishing)
    {
        ai.TellMaster("Fishing is off (Playerbots.Fishing.Enable).");
        return true;
    }
    if (param == "stop")
    {
        EndSession(bot);
        if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL); channel && IsFishingSpell(channel->GetSpellInfo()))
            bot->InterruptSpell(CURRENT_CHANNELED_SPELL, false);
        ai.TellMaster("Done fishing.");
        return true;
    }
    if (!bot->HasSkill(SKILL_FISHING) || !FindFishing(bot).Cast)
    {
        ai.TellMaster("I don't know how to fish.");
        return true;
    }
    if (bot->IsInCombat())
    {
        ai.TellMaster("Not in a fight.");
        return true;
    }
    // an order works like a random bot's own idea: a few casts at the nearest shore, near the master if there is one
    StartSession(bot, false);
    ai.TellMaster("Going fishing.");
    return true;
}
}
