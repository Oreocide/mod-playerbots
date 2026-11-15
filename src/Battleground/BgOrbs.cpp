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


#include "Battleground/BgOrbs.h"
#include "Battleground/BgPlanner.h"
#include "PlayerbotAI.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapReference.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>

// Data: battleground_temple_of_kotmogu.cpp, the world DB and DB2 of map 998 (worktrees/bg-kotmogu/KOTMOGU_DATA.md, 2026-11-14)
namespace Playerbots::Bg::Orbs
{
namespace
{
constexpr uint32 OrbMarker = 116524;        // "Power Orb": every carrier has it
constexpr uint32 PitAura = 112052;          // inside the central pit (the 6 point zone)
constexpr float UseReach = 4.5f;            // the pedestal's interaction distance is 5.56 yd to its GeoBox
constexpr float PickFight = 20.0f;          // a bot going for an orb first fights who stands at it
constexpr float EscortFight = 25.0f;
constexpr float PitFight = 18.0f;
constexpr uint32 UseRetryMs = 1500;         // the pedestal's own cooldown is 1 s
constexpr uint32 ClaimMs = 3000;            // a claim outlives a use and the next plan that sees the orb carried

struct OrbDef
{
    uint32 Entry;
    char const* Name;
    uint32 Aura;                            // the carrier aura of this orb
    uint32 WsInBase;
    uint32 WsHeldHorde;
    uint32 WsHeldAlliance;
    Position Pedestal;
};

std::array<OrbDef, 4> const OrbDefs =
{ {
    { 212091, "Blue", 121164, 6714, 6969, 6970, { 1716.95f, 1250.02f, 13.33f } },
    { 212094, "Orange", 121177, 6717, 6963, 6964, { 1850.17f, 1250.12f, 13.21f } },
    { 212093, "Green", 121176, 6716, 6965, 6966, { 1716.89f, 1416.62f, 13.21f } },
    { 212092, "Purple", 121175, 6715, 6967, 6968, { 1850.22f, 1416.82f, 13.34f } },
} };

Position const PitCentre = { 1783.41f, 1333.56f, 5.7f };   // the pit box x 1750-1816, y 1287-1381; floor z 5.7

OrbDef const* DefOf(uint32 entry)
{
    for (OrbDef const& def : OrbDefs)
        if (def.Entry == entry)
            return &def;
    return nullptr;
}

// the carriers of each instance, by orb (world thread only: battleground bots act there)
std::unordered_map<uint32, std::array<ObjectGuid, 4>>& Carriers()
{
    static std::unordered_map<uint32, std::array<ObjectGuid, 4>> carriers;
    return carriers;
}

std::unordered_map<ObjectGuid, uint32>& UseTimes()
{
    static std::unordered_map<ObjectGuid, uint32> uses;
    return uses;
}

// one taker per orb at a time: several uses of a pedestal in the same tick all get the carrier aura before the object is gone (the
// script then scores every one of them; seen in the first test match, 2026-11-14 16:54: 216 points in 61 s with no kills)
struct Claim
{
    ObjectGuid Bot;
    uint32 UntilMs = 0;
    uint32 Uses = 0;            // uses by this claimant that did not make it a carrier
};

constexpr uint32 MaxUsesPerClaim = 3;       // then the claim goes and the bot waits ClaimBackoffMs (a use that keeps failing)
constexpr uint32 ClaimBackoffMs = 10000;

std::unordered_map<ObjectGuid, uint32>& ClaimBackoff()
{
    static std::unordered_map<ObjectGuid, uint32> backoff;
    return backoff;
}

std::unordered_map<uint32, std::array<Claim, 4>>& Claims()
{
    static std::unordered_map<uint32, std::array<Claim, 4>> claims;
    return claims;
}

/// The bot may use this orb now: nobody else of the match holds a fresh claim on it; the claim lasts ClaimMs
bool ClaimOrb(Player const* bot, size_t orb, uint32 now)
{
    if (auto backoff = ClaimBackoff().find(bot->GetGUID()); backoff != ClaimBackoff().end() && int32(backoff->second - now) > 0)
        return false;
    Claim& claim = Claims()[bot->GetInstanceId()][orb];
    if (claim.Bot != bot->GetGUID() && !claim.Bot.IsEmpty() && int32(claim.UntilMs - now) > 0)
        return false;
    if (claim.Bot != bot->GetGUID() || int32(claim.UntilMs - now) <= 0)
        claim.Uses = 0;
    if (claim.Uses >= MaxUsesPerClaim)
    {
        claim = Claim();
        ClaimBackoff()[bot->GetGUID()] = now + ClaimBackoffMs;
        TC_LOG_INFO("server.playerbots", "Playerbots: {} could not take orb {} of Temple of Kotmogu in {} uses: lets another bot try", bot->GetName(),
            uint32(orb), MaxUsesPerClaim);
        return false;
    }
    claim.Bot = bot->GetGUID();
    claim.UntilMs = now + ClaimMs;
    ++claim.Uses;
    return true;
}

/// The orb object the bot may use now: at the pedestal, spawned, its own retry time over and the orb's claim free or its own. commit
/// false only asks (a trigger); true takes the claim and the retry time (the action that uses it)
GameObject* PedestalUse(Player* bot, OrbDef const& def, size_t orb, bool commit = true)
{
    if (bot->GetExactDist2d(def.Pedestal) > UseReach)
        return nullptr;
    GameObject* go = bot->FindNearestGameObject(def.Entry, 8.0f);
    if (!go || !go->isSpawned())
        return nullptr;
    uint32 now = GameTime::GetGameTimeMS();
    uint32& last = UseTimes()[bot->GetGUID()];
    if (last && now - last < UseRetryMs)
        return nullptr;
    if (!commit)
    {
        Claim const& claim = Claims()[bot->GetInstanceId()][orb];
        bool free = claim.Bot.IsEmpty() || claim.Bot == bot->GetGUID() || int32(claim.UntilMs - now) <= 0;
        auto backoff = ClaimBackoff().find(bot->GetGUID());
        return free && (backoff == ClaimBackoff().end() || int32(backoff->second - now) <= 0) ? go : nullptr;
    }
    if (!ClaimOrb(bot, orb, now))
        return nullptr;
    last = now;
    TC_LOG_INFO("server.playerbots", "Playerbots: {} takes the {} orb of Temple of Kotmogu instance {}", bot->GetName(), def.Name, bot->GetInstanceId());
    return go;
}

/// The bot's own spot in the pit: a few yards from the centre, its angle from its guid, so carriers do not stack
Position PitSpot(Player const* bot)
{
    uint64 counter = bot->GetGUID().GetCounter();
    float angle = float(counter % 360) * float(M_PI) / 180.0f;
    float radius = 4.0f + float(counter % 9);
    return Position(PitCentre.GetPositionX() + radius * std::cos(angle), PitCentre.GetPositionY() + radius * std::sin(angle), PitCentre.GetPositionZ());
}

/// Who may go for a free orb: not a carrier already (nothing stops a second orb), not a healer, not a rogue (Stealth drops it)
bool MayTake(Player const* bot, PlayerbotAI& ai)
{
    return !Carries(bot) && ai.GetRole() != BotRole::Healer && bot->GetClass() != CLASS_ROGUE;
}

Player* CarrierOf(Map* map, uint32 instanceId, size_t orb)
{
    auto it = Carriers().find(instanceId);
    if (it == Carriers().end() || orb >= it->second.size() || it->second[orb].IsEmpty())
        return nullptr;
    Player* carrier = ObjectAccessor::GetPlayer(map, it->second[orb]);
    return carrier && carrier->IsAlive() ? carrier : nullptr;
}
}

bool Carries(Player const* bot)
{
    return bot->HasAura(OrbMarker);
}

bool CarrierAway(Player const* bot, Position& spot)
{
    if (!bot->IsAlive() || !Carries(bot))
        return false;
    spot = PitSpot(bot);
    return !bot->HasAura(PitAura) || bot->GetExactDist2d(spot) > PitFight;
}

void ReadState(BgPlan& plan, Map* map)
{
    std::array<ObjectGuid, 4>& carriers = Carriers()[plan.InstanceId];
    carriers.fill(ObjectGuid::Empty);
    std::array<std::string, 4> names;
    std::array<uint32, 4> counts = { 0, 0, 0, 0 };
    uint32 markers[2] = { 0, 0 };
    for (MapReference const& ref : map->GetPlayers())
        if (Player* player = ref.GetSource(); player && player->IsAlive() && Carries(player))
        {
            if (TeamOf(player) == TEAM_ALLIANCE || TeamOf(player) == TEAM_HORDE)
                ++markers[TeamOf(player)];
            for (size_t i = 0; i < OrbDefs.size(); ++i)
                if (player->HasAura(OrbDefs[i].Aura))
                {
                    carriers[i] = player->GetGUID();
                    ++counts[i];
                    names[i] += (names[i].empty() ? "" : ",") + player->GetName();
                }
        }

    // diagnostics for the test matches: an orb never has two carriers on retail; the score per 30 s
    for (size_t i = 0; i < OrbDefs.size(); ++i)
        if (counts[i] > 1)
            TC_LOG_WARN("server.playerbots", "Playerbots: Temple of Kotmogu instance {}: the {} orb has {} carriers: {}", plan.InstanceId,
                OrbDefs[i].Name, counts[i], names[i]);
    static std::unordered_map<uint32, uint32> lastLog;
    uint32 now = GameTime::GetGameTimeMS();
    if (uint32& last = lastLog[plan.InstanceId]; !last || now - last >= 30000)
    {
        last = now;
        TC_LOG_INFO("server.playerbots", "Playerbots: Temple of Kotmogu instance {}: score A {} H {}; carriers with the marker A {} H {}; Blue {} Orange {} Green {} Purple {}",
            plan.InstanceId, map->GetWorldStateValue(6303), map->GetWorldStateValue(6304), markers[TEAM_ALLIANCE], markers[TEAM_HORDE],
            names[0].empty() ? "-" : names[0], names[1].empty() ? "-" : names[1], names[2].empty() ? "-" : names[2], names[3].empty() ? "-" : names[3]);
    }

    for (Objective& objective : plan.Objectives)
    {
        OrbDef const* def = DefOf(objective.Entry);
        if (!def)
            continue;
        size_t index = size_t(def - OrbDefs.data());
        bool alliance = map->GetWorldStateValue(int32(def->WsHeldAlliance)) == 1;
        bool horde = map->GetWorldStateValue(int32(def->WsHeldHorde)) == 1;
        objective.Owner = alliance ? TEAM_ALLIANCE : horde ? TEAM_HORDE : TEAM_NEUTRAL;
        objective.Assaulting = TEAM_NEUTRAL;
        objective.Destroyed = false;
        objective.Known = true;
        Player* carrier = carriers[index].IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(map, carriers[index]);
        objective.Pos = objective.Owner != TEAM_NEUTRAL && carrier ? carrier->GetPosition() : def->Pedestal;
    }
}

bool TakerAway(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job, Position& pedestal, GameObject*& use, bool commit)
{
    use = nullptr;
    if (!bot->IsAlive() || !MayTake(bot, ai) || job.Base != Job::Attack || job.Objective < 0 || size_t(job.Objective) >= plan.Objectives.size())
        return false;
    Objective const& objective = plan.Objectives[job.Objective];
    OrbDef const* def = DefOf(objective.Entry);
    if (!def || objective.Owner != TEAM_NEUTRAL)
        return false;
    pedestal = def->Pedestal;
    // at the pedestal in a fight: take the orb now (a plain use, no cast) rather than fight on beside it
    if (bot->GetExactDist2d(pedestal) <= UseReach)
    {
        use = PedestalUse(bot, *def, size_t(def - OrbDefs.data()), commit);
        return use != nullptr;
    }
    Unit const* victim = bot->GetVictim();
    return !victim || victim->GetExactDist2d(pedestal) > PickFight;
}

Step Decide(Player* bot, PlayerbotAI& ai, BgPlan const& plan, Assignment const& job)
{
    Step step;
    if (!bot->IsAlive())
        return step;
    step.Active = true;
    Map* map = bot->GetMap();
    TeamId us = TeamOf(bot);

    // a carrier: into the pit and stay there, fighting only who comes in (a chase out of the pit costs 4 points a tick)
    if (Carries(bot))
    {
        Position spot = PitSpot(bot);
        step.FightRange = PitFight;
        step.FightAt = bot->HasAura(PitAura) ? bot->GetPosition() : spot;
        step.WalkTo = spot;
        step.Arrive = bot->HasAura(PitAura) ? 6.0f : 2.0f;
        return step;
    }

    size_t index = job.Objective >= 0 ? size_t(job.Objective) : OrbDefs.size();
    OrbDef const* def = index < plan.Objectives.size() ? DefOf(plan.Objectives[index].Entry) : nullptr;
    if (!def)
    {
        // no orb job: hold the pit, where the carriers come and kills count
        step.FightRange = PitFight + 10.0f;
        step.FightAt = PitCentre;
        step.WalkTo = PitCentre;
        step.Arrive = 15.0f;
        return step;
    }
    size_t orb = size_t(def - OrbDefs.data());
    Objective const& objective = plan.Objectives[index];

    if (objective.Owner == TEAM_NEUTRAL)
    {
        // a free orb on its pedestal: fight who stands there, then take it (healers and rogues guard the pit instead)
        if (!MayTake(bot, ai))
        {
            step.FightRange = PitFight + 10.0f;
            step.FightAt = PitCentre;
            step.WalkTo = PitCentre;
            step.Arrive = 15.0f;
            return step;
        }
        step.FightRange = PickFight;
        step.FightAt = def->Pedestal;
        step.WalkTo = def->Pedestal;
        step.Arrive = 1.5f;
        step.ToPedestal = true;
        step.Use = PedestalUse(bot, *def, orb);
        return step;
    }

    Player* carrier = CarrierOf(map, plan.InstanceId, orb);
    if (!carrier)
    {
        step.WalkTo = objective.Pos;
        step.Arrive = 6.0f;
        return step;
    }
    if (objective.Owner == us)
    {
        // our carrier: stay with it and fight who comes at it
        step.FightRange = EscortFight;
        step.FightAt = carrier->GetPosition();
        step.WalkTo = carrier->GetPosition();
        step.Arrive = 6.0f;
        return step;
    }
    // theirs: go for the carrier
    step.Target = carrier->GetGUID();
    step.WalkTo = carrier->GetPosition();
    step.Arrive = 4.0f;
    return step;
}
}
