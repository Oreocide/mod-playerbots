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

#include "QuestTravel.h"
#include "Strategies/Rpg/NpcDispatch.h"
#include "AreaTriggerPackets.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GameObjectPackets.h"
#include "Db/PlayerbotRepository.h"
#include "Engine.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "QuestDef.h"
#include "Random.h"
#include "Travel/MoveToward.h"
#include "RandomBotMgr.h"
#include "Travel/PhaseGuard.h"
#include "Travel/TravelMgr.h"
#include "World/WorldActionQueue.h"
#include "Loot.h"
#include "LootPackets.h"
#include "Opcodes.h"
#include "WorldSession.h"
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Playerbots
{
namespace
{
constexpr float Relevance = 20.0f;              // above wandering (10), below fighting
constexpr uint32 SelectInterval = 8000;         // ms between looking for a new quest
constexpr uint32 NothingFoundInterval = 45000;
constexpr uint32 QuestTimeout = 30 * 60 * 1000; // give up on a quest after this long (travelling by route does not count)
constexpr uint32 StuckTimeout = 90 * 1000;      // no progress towards a destination
constexpr float ArriveDistance = 5.0f;
constexpr float NpcStandOffMax = 4.0f;           // a giver or turn-in NPC: the walk ends 1 .. this far short of it (service range is combat reach + 4)
constexpr float InteractDistance = 25.0f;       // giver/turn-in NPC must be this close (core allows 5.5 + reach, we walk up first)
constexpr float HuntSearchRange = 70.0f;
constexpr int32 MaxLevelsBelow = 6;             // quests further below the bot's level give too little XP
constexpr uint32 MaxSpawnsPerEntry = 64;
constexpr size_t MaxCandidateChecks = 40;

struct Spawn
{
    uint32 MapId = 0;
    Position Pos;
};

enum class StepKind : uint8
{
    Kill,
    UseGo,
    Talk,
    Explore
};

struct Step
{
    int32 ObjectiveId = 0;
    StepKind Kind = StepKind::Kill;
    bool Enter = false;   // type 19, credited at the trigger; type 10 uses the area-trigger opcode
    std::vector<uint32> Entries;
};

struct Plan
{
    uint32 QuestId = 0;
    std::vector<uint32> Givers;    // creature entries
    std::vector<uint32> Enders;
    std::vector<Step> Steps;
};

struct GiverSpot
{
    uint32 PlanIndex = 0;
    uint32 Entry = 0;
    Position Pos;
    uint32 MapId = 0;
    uint8 HostileMask = 0;   // 1: a town whose creatures attack Alliance bots, 2: Horde bots
};

struct Index
{
    bool Built = false;
    std::vector<Plan> Plans;
    std::unordered_map<uint32, uint32> PlanByQuest;
    std::unordered_map<uint32, std::vector<Spawn>> Spawns;          // creature entry -> spawns
    std::unordered_map<uint32, std::vector<Spawn>> GoSpawns;        // gameobject entry -> spawns
    std::unordered_map<uint32, std::vector<Spawn>> AtSpawns;        // area trigger id -> position
    std::unordered_map<uint32, std::vector<GiverSpot>> GiversByMap; // map -> quest giver spawns
};

Index sIndex;

std::vector<Spawn> const* StepSpawns(Step const& step, uint32 entry)
{
    std::unordered_map<uint32, std::vector<Spawn>> const* table = &sIndex.Spawns;
    if (step.Kind == StepKind::UseGo)
        table = &sIndex.GoSpawns;
    else if (step.Kind == StepKind::Explore)
        table = &sIndex.AtSpawns;
    auto itr = table->find(entry);
    return itr == table->end() ? nullptr : &itr->second;
}

constexpr float FarDistance = 250.0f;           // further than this (or on another map) the bot travels by route
constexpr uint32 MaxRouteFailures = 2;
constexpr float AssignedPenalty = 60.0f;        // seconds added per bot already on the same quest, to spread them

// How many bots work on each quest, so a quest giver is not swamped. Quest actions run on several map threads
std::mutex sAssignedMutex;
std::unordered_map<uint32, uint32> sAssigned;

uint32 AssignedCount(uint32 questId)
{
    std::lock_guard lock(sAssignedMutex);
    auto itr = sAssigned.find(questId);
    return itr == sAssigned.end() ? 0 : itr->second;
}

void Assign(uint32 questId)
{
    std::lock_guard lock(sAssignedMutex);
    ++sAssigned[questId];
}

void Release(uint32 questId)
{
    std::lock_guard lock(sAssignedMutex);
    auto itr = sAssigned.find(questId);
    if (itr != sAssigned.end() && --itr->second == 0)
        sAssigned.erase(itr);
}

bool IsSimpleKillQuest(Quest const* quest)
{
    if (!quest || quest->IsUnavailable() || quest->IsDailyOrWeekly() || quest->IsMonthly() || quest->IsSeasonal() || quest->IsRepeatable()
        || quest->IsWorldQuest() || quest->IsDFQuest() || quest->IsAutoPush() || quest->HasFlag(QUEST_FLAGS_TRACKING_EVENT)
        || quest->GetSuggestedPlayers() > 1 || quest->GetQuestPackageID())
        return false;

    QuestObjectives const& objectives = quest->GetObjectives();
    if (objectives.empty() || objectives.size() > 4)
        return false;

    bool any = false;
    for (QuestObjective const& objective : objectives)
    {
        if (objective.Flags & QUEST_OBJECTIVE_FLAG_OPTIONAL)
            continue;
        if (objective.ObjectID <= 0 || objective.Amount <= 0 || objective.Amount > 30)
            return false;
        if (objective.Type == QUEST_OBJECTIVE_MONSTER)
        {
            any = true;
            continue;
        }
        // an item objective is allowed only when collect or interact is on; BuildIndex still drops it unless something drops the item
        if (objective.Type == QUEST_OBJECTIVE_ITEM && (sPlayerbotsConfig.QuestCollect || sPlayerbotsConfig.QuestInteract))
        {
            any = true;
            continue;
        }
        if (sPlayerbotsConfig.QuestInteract && (objective.Type == QUEST_OBJECTIVE_GAMEOBJECT || objective.Type == QUEST_OBJECTIVE_TALKTO))
        {
            any = true;
            continue;
        }
        if (sPlayerbotsConfig.QuestExplore && (objective.Type == QUEST_OBJECTIVE_AREATRIGGER || objective.Type == QUEST_OBJECTIVE_AREA_TRIGGER_ENTER))
        {
            any = true;
            continue;
        }
        return false;
    }

    return any;
}

// a phase the bot did not have before: both lists are sorted
bool GainedPhase(std::vector<uint16> const& before, std::vector<uint16> const& after)
{
    return !std::ranges::includes(before, after);
}

bool IsHuntable(CreatureTemplate const& creature)
{
    return creature.Classification == CreatureClassifications::Normal || creature.Classification == CreatureClassifications::Trivial
        || creature.Classification == CreatureClassifications::MinusMob;
}

Position const* NearestSpawn(std::vector<Spawn> const& spawns, uint32 mapId, Position const& from, float* distance = nullptr)
{
    Position const* best = nullptr;
    float bestDist = 0.0f;
    for (Spawn const& spawn : spawns)
    {
        if (spawn.MapId != mapId)
            continue;

        float dist = from.GetExactDist2d(spawn.Pos);
        if (!best || dist < bestDist)
        {
            best = &spawn.Pos;
            bestDist = dist;
        }
    }

    if (distance)
        *distance = bestDist;
    return best;
}
}

namespace QuestTravel
{
bool IsBuilt() { return sIndex.Built; }
uint32 GetPlanCount() { return uint32(sIndex.Plans.size()); }

uint32 AbstractXp(Player* bot)
{
    if (!bot || !sIndex.Built || sIndex.Plans.empty())
        return 0;

    for (auto const& [questId, status] : bot->getQuestStatusMap())
    {
        if (status.Status != QUEST_STATUS_INCOMPLETE && status.Status != QUEST_STATUS_COMPLETE)
            continue;
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            if (uint32 xp = quest->XPValue(bot))
                return xp;
    }

    int32 level = bot->GetLevel();
    size_t start = size_t(bot->GetGUID().GetCounter() % sIndex.Plans.size());
    for (size_t n = 0; n < 40 && n < sIndex.Plans.size(); ++n)
    {
        Plan const& plan = sIndex.Plans[(start + n) % sIndex.Plans.size()];
        Quest const* quest = sObjectMgr->GetQuestTemplate(plan.QuestId);
        if (!quest || bot->GetQuestMinLevel(quest) > level || bot->GetQuestLevel(quest) < level - MaxLevelsBelow)
            continue;
        if (!bot->CanTakeQuest(quest, false))
            continue;
        return quest->XPValue(bot);
    }
    return 0;
}

bool PickPoi(Player* bot, uint32 questId, bool turnIn, Position& out)
{
    if (!bot)
        return false;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    QuestPOIData const* poi = sObjectMgr->GetQuestPOIData(int32(questId));
    if (!quest || !poi)
        return false;

    QuestStatus status = bot->GetQuestStatus(questId);
    if (turnIn && status != QUEST_STATUS_COMPLETE)
        return false;
    if (!turnIn && status != QUEST_STATUS_INCOMPLETE)
        return false;

    std::vector<Position> points;
    for (QuestPOIBlobData const& blob : poi->Blobs)
    {
        if (uint32(blob.MapID) != bot->GetMapId() || blob.Points.empty())
            continue;
        if (turnIn)
        {
            if (blob.ObjectiveIndex != -1)
                continue;
        }
        else
        {
            bool match = false;
            for (QuestObjective const& objective : quest->GetObjectives())
                if (!bot->IsQuestObjectiveComplete(questId, objective.ID) &&
                    (blob.QuestObjectiveID == int32(objective.ID) || blob.ObjectiveIndex == objective.StorageIndex))
                    match = true;
            if (!match)
                continue;
        }

        QuestPOIBlobPoint const& p = blob.Points[urand(0, uint32(blob.Points.size() - 1))];
        Position pos(float(p.X), float(p.Y), float(p.Z));
        if (p.Z == 0)
            pos.m_positionZ = GetStandZ(bot, pos.GetPositionX(), pos.GetPositionY(), bot->GetPositionZ());
        if (bot->GetExactDist2d(pos) < 1500.0f)
            points.push_back(pos);
    }

    if (points.empty())
        return false;
    out = points[urand(0, uint32(points.size() - 1))];
    return true;
}

bool NearestHunt(Player* bot, Position& out)
{
    if (!bot || !sIndex.Built)
        return false;

    Position const here = bot->GetPosition();
    Position const* best = nullptr;
    float bestDist = 800.0f;
    for (auto const& [entry, spawns] : sIndex.Spawns)
        if (Position const* pos = NearestSpawn(spawns, bot->GetMapId(), here, nullptr))
        {
            float dist = here.GetExactDist2d(*pos);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = pos;
            }
        }

    if (!best)
        return false;
    out = *best;
    return true;
}

void BuildIndex()
{
    sIndex = Index();
    PhaseGuard::Load();

    // creature entries that give kill credit for another entry (KillCredit1/2 of the template)
    std::unordered_map<uint32, std::vector<uint32>> creditors;
    for (auto const& [entry, creature] : sObjectMgr->GetCreatureTemplates())
        for (uint32 credit : creature.KillCredit)
            if (credit && IsHuntable(creature))
                creditors[credit].push_back(entry);

    std::vector<Plan> drafts;
    std::unordered_set<uint32> wanted;   // creature entries whose spawns we need
    std::unordered_set<uint32> wantedGo;

    // item id -> creature entries that drop it. Only rows that are a real item, not a loot reference.
    std::unordered_map<uint32, std::vector<uint32>> drops;
    if (sPlayerbotsConfig.QuestCollect)
        if (QueryResult loot = WorldDatabase.Query("SELECT Entry, Item FROM creature_loot_template WHERE Item > 0 AND ItemType = 0"))
        {
            do
            {
                Field* fields = loot->Fetch();
                drops[fields[1].GetUInt32()].push_back(fields[0].GetUInt32());
            }
            while (loot->NextRow());
        }

    // item id -> chest entries whose loot table contains it. Data1 is the chest loot id.
    std::unordered_map<uint32, std::vector<uint32>> chestDrops;
    if (sPlayerbotsConfig.QuestInteract)
        if (QueryResult loot = WorldDatabase.Query("SELECT gt.entry, gl.Item FROM gameobject_template gt "
            "JOIN gameobject_loot_template gl ON gl.Entry = gt.Data1 "
            "WHERE gt.type = 3 AND gl.Item > 0 AND gl.ItemType = 0"))
        {
            do
            {
                Field* fields = loot->Fetch();
                chestDrops[fields[1].GetUInt32()].push_back(fields[0].GetUInt32());
            }
            while (loot->NextRow());
        }

    // quest -> starters
    std::unordered_map<uint32, std::vector<uint32>> giversByQuest;
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMapHACK())
        giversByQuest[questId].push_back(entry);

    for (auto const& [questId, givers] : giversByQuest)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!IsSimpleKillQuest(quest) || sPlayerbotRepository->IsQuestBlacklisted(questId) || PhaseGuard::IsPhaseGatedQuest(questId))
            continue;

        Plan plan;
        plan.QuestId = questId;
        plan.Givers = givers;
        for (auto const& [quest_, ender] : sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(questId))
            plan.Enders.push_back(ender);

        if (plan.Enders.empty())
            continue;

        bool valid = true;
        for (QuestObjective const& objective : quest->GetObjectives())
        {
            if (objective.Flags & QUEST_OBJECTIVE_FLAG_OPTIONAL)
                continue;

            Step step;
            step.ObjectiveId = objective.ID;
            if (objective.Type == QUEST_OBJECTIVE_MONSTER)
            {
                step.Kind = StepKind::Kill;
                uint32 objectiveEntry = uint32(objective.ObjectID);
                if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(objectiveEntry); creature && IsHuntable(*creature))
                    step.Entries.push_back(objectiveEntry);
                if (auto itr = creditors.find(objectiveEntry); itr != creditors.end())
                    step.Entries.insert(step.Entries.end(), itr->second.begin(), itr->second.end());
            }
            else if (objective.Type == QUEST_OBJECTIVE_ITEM)
            {
                auto itr = drops.find(uint32(objective.ObjectID));
                if (itr != drops.end())
                    for (uint32 entry : itr->second)
                        if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry); creature && IsHuntable(*creature))
                            step.Entries.push_back(entry);
                if (step.Entries.empty())
                {
                    step.Kind = StepKind::UseGo;
                    auto chest = chestDrops.find(uint32(objective.ObjectID));
                    if (chest != chestDrops.end())
                        step.Entries = chest->second;
                }
            }
            else if (objective.Type == QUEST_OBJECTIVE_GAMEOBJECT)
            {
                step.Kind = StepKind::UseGo;
                step.Entries.push_back(uint32(objective.ObjectID));
            }
            else if (objective.Type == QUEST_OBJECTIVE_TALKTO)
            {
                step.Kind = StepKind::Talk;
                step.Entries.push_back(uint32(objective.ObjectID));
            }
            else if (objective.Type == QUEST_OBJECTIVE_AREATRIGGER || objective.Type == QUEST_OBJECTIVE_AREA_TRIGGER_ENTER)
            {
                step.Kind = StepKind::Explore;
                step.Enter = objective.Type == QUEST_OBJECTIVE_AREA_TRIGGER_ENTER;
                if (AreaTriggerEntry const* at = sAreaTriggerStore.LookupEntry(uint32(objective.ObjectID)))
                    if (at->ContinentID && PhaseGuard::IsUsableSpawnPhase(uint32(std::max(int16(0), at->PhaseID)), uint32(std::max(int16(0), at->PhaseGroupID))))
                    {
                        step.Entries.push_back(at->ID);
                        if (!sIndex.AtSpawns.contains(at->ID))
                            sIndex.AtSpawns[at->ID].push_back({ at->ContinentID, Position(at->Pos.X, at->Pos.Y, at->Pos.Z) });
                    }
            }
            else
            {
                valid = false;
                break;
            }

            if (step.Entries.empty())
            {
                valid = false;
                break;
            }

            plan.Steps.push_back(std::move(step));
        }

        if (!valid || plan.Steps.empty())
            continue;

        wanted.insert(plan.Givers.begin(), plan.Givers.end());
        wanted.insert(plan.Enders.begin(), plan.Enders.end());
        for (Step const& step : plan.Steps)
        {
            if (step.Kind == StepKind::UseGo)
                wantedGo.insert(step.Entries.begin(), step.Entries.end());
            else if (step.Kind != StepKind::Explore)
                wanted.insert(step.Entries.begin(), step.Entries.end());
        }
        drafts.push_back(std::move(plan));
    }

    // one pass over all spawns
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!wanted.contains(data.id) || !PhaseGuard::IsUsableSpawnPhase(data.phaseId, data.phaseGroup))
            continue;

        if (!data.spawnDifficulties.empty() && std::ranges::find(data.spawnDifficulties, DIFFICULTY_NONE) == data.spawnDifficulties.end())
            continue;

        MapEntry const* map = sMapStore.LookupEntry(data.mapId);
        if (!map || map->Instanceable())
            continue;

        std::vector<Spawn>& spawns = sIndex.Spawns[data.id];
        if (spawns.size() < MaxSpawnsPerEntry)
            spawns.push_back({ data.mapId, data.spawnPoint });
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGameObjectData())
    {
        if (!wantedGo.contains(data.id) || !PhaseGuard::IsUsableSpawnPhase(data.phaseId, data.phaseGroup))
            continue;

        if (!data.spawnDifficulties.empty() && std::ranges::find(data.spawnDifficulties, DIFFICULTY_NONE) == data.spawnDifficulties.end())
            continue;

        MapEntry const* map = sMapStore.LookupEntry(data.mapId);
        if (!map || map->Instanceable())
            continue;

        std::vector<Spawn>& spawns = sIndex.GoSpawns[data.id];
        if (spawns.size() < MaxSpawnsPerEntry)
            spawns.push_back({ data.mapId, data.spawnPoint });
    }

    // keep the plans whose giver, turn-in and every objective have a spawn near the giver
    for (Plan& plan : drafts)
    {
        auto has = [](uint32 entry) { return sIndex.Spawns.contains(entry); };
        std::erase_if(plan.Givers, [&](uint32 entry) { return !has(entry); });
        std::erase_if(plan.Enders, [&](uint32 entry) { return !has(entry); });
        for (Step& step : plan.Steps)
            std::erase_if(step.Entries, [&](uint32 entry) { return !StepSpawns(step, entry); });

        if (plan.Givers.empty() || plan.Enders.empty() || std::ranges::any_of(plan.Steps, [](Step const& step) { return step.Entries.empty(); }))
            continue;

        uint32 planIndex = uint32(sIndex.Plans.size());
        bool placed = false;
        for (uint32 giver : plan.Givers)
            for (Spawn const& spawn : sIndex.Spawns[giver])
            {
                // hunting grounds must be on the giver's map, not too far away
                bool reachable = std::ranges::all_of(plan.Steps, [&](Step const& step)
                {
                    return std::ranges::any_of(step.Entries, [&](uint32 entry)
                    {
                        std::vector<Spawn> const* spawns = StepSpawns(step, entry);
                        float dist = 0.0f;
                        return spawns && NearestSpawn(*spawns, spawn.MapId, spawn.Pos, &dist) && dist <= sPlayerbotsConfig.RandomQuestDistance;
                    });
                });
                if (!reachable)
                    continue;

                // a quest giver inside a town that attacks a team is no quest for that team
                uint8 hostile = 0;
                if (sTravelMgr->IsHostileGround(spawn.MapId, spawn.Pos.GetPositionX(), spawn.Pos.GetPositionY(), 1))
                    hostile |= 1;
                if (sTravelMgr->IsHostileGround(spawn.MapId, spawn.Pos.GetPositionX(), spawn.Pos.GetPositionY(), 2))
                    hostile |= 2;
                sIndex.GiversByMap[spawn.MapId].push_back({ planIndex, giver, spawn.Pos, spawn.MapId, hostile });
                placed = true;
            }

        if (!placed)
            continue;

        sIndex.PlanByQuest[plan.QuestId] = planIndex;
        sIndex.Plans.push_back(std::move(plan));
    }

    sIndex.Built = true;
    TC_LOG_INFO("server.playerbots", "Playerbots: quest travel knows {} quests ({} giver spawns)", sIndex.Plans.size(),
        [] { size_t n = 0; for (auto const& [map, spots] : sIndex.GiversByMap) n += spots.size(); return n; }());
}
}

namespace
{
enum class State : uint8
{
    Idle,
    ToGiver,
    Hunt,
    ToEnder
};

class QuestAction : public Action
{
public:
    char const* GetName() const override { return "quest"; }

    float GetRelevance(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (!sPlayerbotsConfig.RandomQuests || !sIndex.Built || ai.GetMaster() || bot->GetGroup())
            return 0.0f;

        // a bot that lives at a hotspot does town things until a rotation sends it out
        if (sRandomBotMgr->GetAnchor(bot->GetGUID()))
            return 0.0f;

        // test harness (.bot rpg): the forced rpg behavior wins over quests for a while
        if (uint32 forcedUntil = ai.GetRpg().ForcedUntil; forcedUntil && GameTime::GetGameTimeMS() < forcedUntil)
            return 0.0f;

        if (!bot->IsAlive() || !bot->IsInWorld() || bot->IsInCombat() || !bot->getAttackers().empty()
            || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_CASTING)
            || bot->InBattleground() || bot->InArena())
            return 0.0f;

        return Relevance;
    }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        uint32 now = GameTime::GetGameTimeMS();
        _currentMap = bot->GetMapId();   // destinations set without a map are on the map the bot is on

        // Moved (summon, teleport): drop the plan and look for something to do near the new position first
        if (_epoch != ai.GetInterruptEpoch())
        {
            _epoch = ai.GetInterruptEpoch();
            Reset();
            _skipLog = true;
            _nextSelect = 0;
        }

        // A route ran in between (the travel strategy takes over while it does): the quest clock waited for it
        uint32 routeEnded = ai.GetRoute().GetLastEnded();
        if (routeEnded && int32(routeEnded - _lastProgress) > 0)
            _lastProgress = routeEnded;
        if (routeEnded && int32(routeEnded - _questStarted) > 0)
            _questStarted = routeEnded;

        if (_state == State::Idle)
        {
            if (now < _nextSelect)
                return false;

            _nextSelect = now + SelectInterval;
            if (!Select(ai))
            {
                _nextSelect = now + NothingFoundInterval;
                return false;
            }
        }

        // The quest was dropped or completed by something else
        Quest const* quest = sObjectMgr->GetQuestTemplate(_questId);
        Plan const* plan = PlanFor(_questId);
        if (!quest || !plan)
        {
            Reset();
            return false;
        }

        if (now - _questStarted > QuestTimeout || now - _lastProgress > StuckTimeout)
        {
            TC_LOG_DEBUG("server.playerbots", "Playerbots: {} gives up quest {}", bot->GetName(), _questId);
            Abandon(bot);
            return false;
        }

        switch (_state)
        {
            case State::ToGiver:
                return DoToGiver(ai, quest, *plan, now);
            case State::Hunt:
                return DoHunt(ai, quest, *plan, now);
            case State::ToEnder:
                return DoToEnder(ai, quest, *plan, now);
            default:
                return false;
        }
    }

private:
    static Plan const* PlanFor(uint32 questId)
    {
        auto itr = sIndex.PlanByQuest.find(questId);
        return itr == sIndex.PlanByQuest.end() ? nullptr : &sIndex.Plans[itr->second];
    }

    void Reset()
    {
        if (_assignedQuest)
        {
            Release(_assignedQuest);
            _assignedQuest = 0;
        }

        _state = State::Idle;
        _questId = 0;
        _hasDest = false;
        _stepNotBefore = 0;
        _moveIssued = false;
        _routeFailures = 0;
        _routeAsked = false;
    }

    void Abandon(Player* bot)
    {
        _failed.insert(_questId);
        if (bot->GetQuestStatus(_questId) != QUEST_STATUS_NONE)
            bot->AbandonQuest(_questId);
        Reset();
        _nextSelect = GameTime::GetGameTimeMS() + SelectInterval;
    }

    void Progress(uint32 now) { _lastProgress = now; }

    void SetState(State state, uint32 now)
    {
        _state = state;
        _hasDest = false;
        _moveIssued = false;
        _hasProgressDist = false;
        Progress(now);
    }

    // Quest in the log first (survives relogs), then the nearest quest giver the bot may take a quest from
    bool Select(PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        uint32 now = GameTime::GetGameTimeMS();

        bool skipLog = _skipLog;
        _skipLog = false;

        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (skipLog)
                break;

            if ((status.Status != QUEST_STATUS_INCOMPLETE && status.Status != QUEST_STATUS_COMPLETE) || !PlanFor(questId) || _failed.contains(questId))
                continue;

            _questId = questId;
            _questStarted = now;
            SetState(status.Status == QUEST_STATUS_COMPLETE ? State::ToEnder : State::Hunt, now);
            return true;
        }

        // Every giver the bot can get to in time, best route first, with some luck and a penalty for quests that
        // other bots already work on so they do not all queue at one giver. Without the travel graph the bot
        // stays on its own map within QuestDistance, like before
        TravelMgr::BotReach reach = sTravelMgr->ReachFor(bot);
        Position const here = bot->GetPosition();
        int32 level = bot->GetLevel();
        uint8 team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
        float maxSeconds = float(sPlayerbotsConfig.QuestMaxRouteMinutes) * 60.0f;
        float maxDist = sPlayerbotsConfig.RandomQuestDistance;

        struct Candidate
        {
            float Score;
            GiverSpot const* Spot;
        };
        std::vector<Candidate> candidates;
        for (auto const& [mapId, spots] : sIndex.GiversByMap)
        {
            if (!reach.Valid && mapId != bot->GetMapId())
                continue;

            for (GiverSpot const& spot : spots)
            {
                Plan const& plan = sIndex.Plans[spot.PlanIndex];
                if ((spot.HostileMask & team) || _failed.contains(plan.QuestId) || PhaseGuard::IsPhaseGatedQuest(plan.QuestId))
                    continue;

                // the cheap checks before the route
                Quest const* candidateQuest = sObjectMgr->GetQuestTemplate(plan.QuestId);
                if (!candidateQuest || bot->GetQuestMinLevel(candidateQuest) > level || bot->GetQuestLevel(candidateQuest) < level - MaxLevelsBelow)
                    continue;

                float cost = 0.0f;
                if (reach.Valid)
                {
                    float seconds = 0.0f;
                    if (!sTravelMgr->Estimate(reach, { spot.MapId, spot.Pos.GetPositionX(), spot.Pos.GetPositionY(), spot.Pos.GetPositionZ() }, cost, seconds)
                        || seconds > maxSeconds)
                        continue;
                }
                else
                {
                    cost = here.GetExactDist2d(spot.Pos);
                    if (cost > maxDist)
                        continue;
                }

                candidates.push_back({ cost + float(urand(0, 90)) + AssignedPenalty * float(AssignedCount(plan.QuestId)), &spot });
            }
        }

        std::ranges::sort(candidates, {}, &Candidate::Score);

        size_t checked = 0;
        for (Candidate const& candidate : candidates)
        {
            Plan const& plan = sIndex.Plans[candidate.Spot->PlanIndex];
            Quest const* quest = sObjectMgr->GetQuestTemplate(plan.QuestId);
            if (!quest)
                continue;

            if (++checked > MaxCandidateChecks)
                break;

            if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
                continue;

            _questId = plan.QuestId;
            _questStarted = now;
            _assignedQuest = plan.QuestId;
            Assign(plan.QuestId);
            SetState(State::ToGiver, now);
            SetDest(candidate.Spot->Pos, candidate.Spot->MapId);
            return true;
        }

        return false;
    }

    // Starts the route to the destination, or finds it running. true = still on the way. The travel strategy
    // follows the route, this action does not run while it does
    bool RouteTo(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RouteFollower& route = ai.GetRoute();
        if (route.IsActive())
        {
            Progress(now);
            return true;
        }

        // the route we asked for has ended: what became of it?
        if (_routeAsked && route.GetLastEnded() && int32(route.GetLastEnded() - _routeAskedAt) >= 0)
        {
            _routeAsked = false;
            if (route.GetLastResult() == RouteStatus::Failed)
                ++_routeFailures;
        }

        if (_destMap == bot->GetMapId() && bot->GetExactDist2d(_dest) <= FarDistance)
            return false;   // close enough to walk the rest

        if (_routeFailures >= MaxRouteFailures)
            return false;

        if (!sTravelMgr->IsBuilt() || !route.Begin(ai, { _destMap, _dest.GetPositionX(), _dest.GetPositionY(), _dest.GetPositionZ() }, InteractDistance * 0.6f))
        {
            _routeFailures = MaxRouteFailures;
            return false;
        }

        _routeAsked = true;
        _routeAskedAt = now;
        Progress(now);
        return true;
    }

    // Walks to a position, true while still travelling
    // standoff > 0: the walk ends that many yards short of _dest (a giver or turn-in NPC: the bot stands near it, not on its spot)
    bool Travel(PlayerbotAI& ai, uint32 now, float arrive = ArriveDistance, float standoff = 0.0f)
    {
        Player* bot = ai.GetBot();
        if (_destMap != bot->GetMapId() || bot->GetExactDist2d(_dest) > FarDistance)
            return RouteTo(ai, now);

        float dist = bot->GetExactDist2d(_dest);
        if (dist <= arrive)
        {
            _moveIssued = false;
            return false;
        }

        if (IsMovingOnFoot(bot))
        {
            _moveIssued = true;
        }
        else
        {
            if (bot->GetVictim())
                bot->AttackStop();
            MoveToward(bot, _dest, arrive, standoff);
            _moveIssued = true;
        }

        // Making headway counts as progress
        if (!_hasProgressDist || dist + 3.0f < _bestDist)
        {
            _bestDist = dist;
            _hasProgressDist = true;
            Progress(now);
        }

        return true;
    }

    void SetDest(Position const& pos, uint32 mapId)
    {
        _dest = pos;
        _destMap = mapId;
        _hasDest = true;
        _hasProgressDist = false;
    }

    void SetDest(Position const& pos) { SetDest(pos, _currentMap); }

    // The best place to find one of these creatures: by route cost over the whole world, or the nearest on this
    // map without the travel graph
    bool BestSpawn(Player* bot, std::vector<uint32> const& entries, Position& out, uint32& outMap)
    {
        TravelMgr::BotReach reach = sTravelMgr->ReachFor(bot);
        float maxSeconds = float(sPlayerbotsConfig.QuestMaxRouteMinutes) * 60.0f;
        float bestCost = 0.0f;
        bool found = false;
        Position const here = bot->GetPosition();
        for (uint32 entry : entries)
        {
            auto itr = sIndex.Spawns.find(entry);
            if (itr == sIndex.Spawns.end())
                continue;

            for (Spawn const& spawn : itr->second)
            {
                float cost = 0.0f;
                if (reach.Valid)
                {
                    float seconds = 0.0f;
                    if (!sTravelMgr->Estimate(reach, { spawn.MapId, spawn.Pos.GetPositionX(), spawn.Pos.GetPositionY(), spawn.Pos.GetPositionZ() }, cost, seconds)
                        || seconds > maxSeconds)
                        continue;
                }
                else if (spawn.MapId == bot->GetMapId())
                    cost = here.GetExactDist2d(spawn.Pos);
                else
                    continue;

                if (!found || cost < bestCost)
                {
                    found = true;
                    bestCost = cost;
                    out = spawn.Pos;
                    outMap = spawn.MapId;
                }
            }
        }

        return found;
    }

    Position const* NearestOf(Player* bot, std::vector<uint32> const& entries)
    {
        Position const* best = nullptr;
        float bestDist = 0.0f;
        Position const here = bot->GetPosition();
        for (uint32 entry : entries)
        {
            auto itr = sIndex.Spawns.find(entry);
            if (itr == sIndex.Spawns.end())
                continue;

            float dist = 0.0f;
            if (Position const* pos = NearestSpawn(itr->second, bot->GetMapId(), here, &dist); pos && (!best || dist < bestDist))
            {
                best = pos;
                bestDist = dist;
            }
        }

        return best;
    }

    Creature* FindNearby(Player* bot, std::vector<uint32> const& entries, float range)
    {
        Creature* best = nullptr;
        for (uint32 entry : entries)
            if (Creature* creature = bot->FindNearestCreature(entry, range, true); creature && (!best || bot->GetExactDistSq(creature) < bot->GetExactDistSq(best)))
                best = creature;
        return best;
    }

    bool DoToGiver(PlayerbotAI& ai, Quest const* quest, Plan const& plan, uint32 now)
    {
        Player* bot = ai.GetBot();
        if (bot->GetQuestStatus(_questId) != QUEST_STATUS_NONE)
        {
            SetState(bot->GetQuestStatus(_questId) == QUEST_STATUS_COMPLETE ? State::ToEnder : State::Hunt, now);
            return true;
        }

        if (!_hasDest)
        {
            Position pos;
            uint32 mapId = 0;
            if (!BestSpawn(bot, plan.Givers, pos, mapId))
            {
                Abandon(bot);
                return false;
            }
            SetDest(pos, mapId);
            _standoff = sPlayerbotsConfig.RpgNpcStandOff ? frand(1.0f, NpcStandOffMax) : 0.0f;
        }

        if (Travel(ai, now, ArriveDistance, _standoff))
            return true;

        Creature* giver = FindNearby(bot, plan.Givers, InteractDistance);
        if (!giver || !bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
        {
            Abandon(bot);
            return false;
        }

        bot->GetMotionMaster()->Clear();
        std::vector<uint16> phasesBefore = PhaseGuard::ForeignPhases(bot);
        BeginInteraction(bot, giver, 4000);
        bot->AddQuestAndCheckCompletion(quest, giver);
        if (GainedPhase(phasesBefore, PhaseGuard::ForeignPhases(bot)))
        {
            // the quest put the bot into a world of its own: out of it, and never again
            PhaseGuard::BlockQuest(_questId, "accept changed the phase");
            Abandon(bot);
            return true;
        }
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} accepted quest {} from {}", bot->GetName(), _questId, giver->GetName());
        SetState(bot->GetQuestStatus(_questId) == QUEST_STATUS_COMPLETE ? State::ToEnder : State::Hunt, now);
        return true;
    }

    bool DoHunt(PlayerbotAI& ai, Quest const* quest, Plan const& plan, uint32 now)
    {
        Player* bot = ai.GetBot();
        QuestStatus status = bot->GetQuestStatus(_questId);
        if (status == QUEST_STATUS_COMPLETE)
        {
            SetState(State::ToEnder, now);
            return true;
        }

        if (status != QUEST_STATUS_INCOMPLETE)
        {
            Reset(); // failed, abandoned or rewarded elsewhere
            return false;
        }

        // first objective that is still open
        Step const* step = nullptr;
        for (QuestObjective const& objective : quest->GetObjectives())
        {
            if (objective.Flags & QUEST_OBJECTIVE_FLAG_OPTIONAL)
                continue;
            if (bot->GetQuestObjectiveData(objective) >= objective.Amount)
                continue;
            for (Step const& candidate : plan.Steps)
                if (uint32(candidate.ObjectiveId) == objective.ID)
                    step = &candidate;
            break;
        }

        if (!step)
            return false; // completes on the next credit update

        if (step->Kind != StepKind::Kill)
            return DoStep(ai, *step, now);

        std::vector<uint32> const* entries = &step->Entries;

        if (Creature* target = FindNearby(bot, *entries, HuntSearchRange))
        {
            if (bot->IsValidAttackTarget(target) && int32(target->GetLevelForTarget(bot)) <= int32(bot->GetLevel()) + 4)
            {
                // The attack action only accepts targets within 60 yards, walk closer first
                if (bot->GetExactDist(target) > 45.0f)
                {
                    SetDest(target->GetPosition());
                    Travel(ai, now, 30.0f);
                    return true;
                }

                ai.SetForcedTarget(target->GetGUID());
                Progress(now);
                _hasDest = false;
                return true;
            }
        }

        // nothing alive in reach: walk to the nearest spawn, then to another one
        if (!_hasDest || bot->GetExactDist2d(_dest) <= ArriveDistance)
        {
            std::vector<Position const*> spots;
            for (uint32 entry : *entries)
                if (auto itr = sIndex.Spawns.find(entry); itr != sIndex.Spawns.end())
                    for (Spawn const& spawn : itr->second)
                        if (spawn.MapId == bot->GetMapId() && bot->GetExactDist2d(spawn.Pos) > ArriveDistance * 2)
                            spots.push_back(&spawn.Pos);

            if (spots.empty())
            {
                // nothing to hunt on this map: the hunting grounds are elsewhere, go there
                Position far;
                uint32 farMap = 0;
                if (!BestSpawn(bot, *entries, far, farMap) || farMap == bot->GetMapId())
                {
                    Abandon(bot);
                    return false;
                }

                SetDest(far, farMap);
                return Travel(ai, now) || true;
            }

            std::ranges::sort(spots, {}, [bot](Position const* pos) { return bot->GetExactDist2d(*pos); });
            spots.resize(std::min<size_t>(spots.size(), 4));
            SetDest(*spots[urand(0, uint32(spots.size() - 1))]);
        }

        Travel(ai, now);
        return true;
    }

    bool DoToEnder(PlayerbotAI& ai, Quest const* quest, Plan const& plan, uint32 now)
    {
        Player* bot = ai.GetBot();
        if (bot->GetQuestStatus(_questId) != QUEST_STATUS_COMPLETE)
        {
            if (bot->GetQuestStatus(_questId) == QUEST_STATUS_INCOMPLETE)
                SetState(State::Hunt, now);
            else
                Reset();
            return true;
        }

        if (!_hasDest)
        {
            Position pos;
            uint32 mapId = 0;
            if (!BestSpawn(bot, plan.Enders, pos, mapId))
            {
                Abandon(bot);
                return false;
            }
            SetDest(pos, mapId);
            _standoff = sPlayerbotsConfig.RpgNpcStandOff ? frand(1.0f, NpcStandOffMax) : 0.0f;
        }

        if (Travel(ai, now, ArriveDistance, _standoff))
            return true;

        Creature* ender = FindNearby(bot, plan.Enders, InteractDistance);
        if (!ender)
        {
            Abandon(bot);
            return false;
        }

        LootItemType type = LootItemType::Item;
        uint32 rewardId = 0;
        if (!ChooseReward(bot, quest, type, rewardId) || !bot->CanRewardQuest(quest, type, rewardId, false))
        {
            Abandon(bot);
            return false;
        }

        bot->GetMotionMaster()->Clear();
        std::vector<uint16> phasesBefore = PhaseGuard::ForeignPhases(bot);
        BeginInteraction(bot, ender, 4000);
        bot->RewardQuest(quest, type, rewardId, ender);
        if (GainedPhase(phasesBefore, PhaseGuard::ForeignPhases(bot)))
            PhaseGuard::BlockQuest(_questId, "reward changed the phase");
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} turned in quest {} to {}", bot->GetName(), _questId, ender->GetName());

        uint32 nextId = quest->GetNextQuestInChain();
        ai.SetHome(bot->GetPosition());
        Reset();

        if (Quest const* next = sObjectMgr->GetQuestTemplate(nextId))
            if (!next->GetBreadcrumbForQuestId() && PlanFor(nextId) && bot->CanTakeQuest(next, false) && bot->CanAddQuest(next, false))
            {
                _questId = nextId;
                _questStarted = now;
                _assignedQuest = nextId;
                Assign(nextId);
                SetState(State::ToGiver, now);
                TC_LOG_DEBUG("server.playerbots", "Playerbots: {} follows quest {} with {}", bot->GetName(), quest->GetQuestId(), nextId);
                return true;
            }

        _nextSelect = now + 2000;
        return true;
    }

    bool DoStep(PlayerbotAI& ai, Step const& step, uint32 now)
    {
        Player* bot = ai.GetBot();
        if (now < _stepNotBefore)
            return true;

        float arrive = step.Kind == StepKind::Explore ? 1.0f : ArriveDistance;
        if (!_hasDest)
        {
            Position pos;
            uint32 mapId = 0;
            if (!BestStep(bot, step, pos, mapId))
            {
                Abandon(bot);
                return false;
            }
            SetDest(pos, mapId);
            // a talk-to step walks up to an NPC; explore points and objects are walked to as before
            _standoff = step.Kind == StepKind::Talk && sPlayerbotsConfig.RpgNpcStandOff ? frand(1.0f, NpcStandOffMax) : 0.0f;
        }

        if (Travel(ai, now, arrive, step.Kind == StepKind::Talk ? _standoff : 0.0f))
            return true;

        if (step.Kind == StepKind::Talk)
        {
            if (Creature* npc = FindNearby(bot, step.Entries, InteractDistance))
            {
                BeginInteraction(bot, npc, 4000);
                bot->TalkedToCreature(npc->GetEntry(), npc->GetGUID());
                Progress(now);
                _hasDest = false;
                _stepNotBefore = now + 4000;
                return true;
            }
        }
        else if (step.Kind == StepKind::UseGo)
        {
            FindGameObjectOptions options;
            std::vector<GameObject*> found;
            for (uint32 entry : step.Entries)
            {
                options.GameObjectId = entry;
                bot->GetGameObjectListWithOptionsInGrid(found, InteractDistance, options);
            }
            GameObject* go = nullptr;
            for (GameObject* candidate : found)
                if (candidate && (!go || bot->GetExactDistSq(candidate) < bot->GetExactDistSq(go)))
                    go = candidate;
            if (go)
            {
                WorldPackets::GameObject::GameObjUse use{ WorldPacket(CMSG_GAME_OBJ_USE) };
                use.Guid = go->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(use);
                ObjectGuid guid = go->GetGUID();
                sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), guid, [guid](Player* player)
                {
                    GameObject* chest = ObjectAccessor::GetGameObject(*player, guid);
                    if (!chest)
                        return;
                    Loot* loot = chest->GetLootForPlayer(player);
                    if (!loot)
                        return;
                    WorldSession* session = player->GetSession();
                    for (uint32 slot = 0; slot < loot->items.size() && slot < 255; ++slot)
                        if (!loot->items[slot].is_looted)
                            player->StoreLootItem(guid, uint8(slot), loot);
                    WorldPackets::Loot::LootRelease release{ WorldPacket(CMSG_LOOT_RELEASE) };
                    release.Unit = guid;
                    session->HandleLootReleaseOpcode(release);
                });
                Progress(now);
                _hasDest = false;
                _stepNotBefore = now + 4000;
                return true;
            }
        }
        else if (step.Kind == StepKind::Explore)
        {
            uint32 id = step.Entries.front();
            if (step.Enter)
                bot->UpdateQuestObjectiveProgress(QUEST_OBJECTIVE_AREA_TRIGGER_ENTER, int32(id), 1);
            else
            {
                WorldPackets::AreaTrigger::AreaTrigger packet{ WorldPacket(CMSG_AREA_TRIGGER) };
                packet.AreaTriggerID = int32(id);
                packet.Entered = true;
                packet.FromClient = true;
                bot->GetSession()->HandleAreaTriggerOpcode(packet);
            }
            Progress(now);
            _hasDest = false;
            _stepNotBefore = now + 4000;
            return true;
        }

        return true;
    }

    bool BestStep(Player* bot, Step const& step, Position& out, uint32& outMap)
    {
        TravelMgr::BotReach reach = sTravelMgr->ReachFor(bot);
        float maxSeconds = float(sPlayerbotsConfig.QuestMaxRouteMinutes) * 60.0f;
        float bestCost = 0.0f;
        bool found = false;
        Position const here = bot->GetPosition();
        for (uint32 entry : step.Entries)
        {
            std::vector<Spawn> const* spawns = StepSpawns(step, entry);
            if (!spawns)
                continue;
            for (Spawn const& spawn : *spawns)
            {
                float cost = 0.0f;
                if (reach.Valid)
                {
                    float seconds = 0.0f;
                    if (!sTravelMgr->Estimate(reach, { spawn.MapId, spawn.Pos.GetPositionX(), spawn.Pos.GetPositionY(), spawn.Pos.GetPositionZ() }, cost, seconds)
                        || seconds > maxSeconds)
                        continue;
                }
                else if (spawn.MapId == bot->GetMapId())
                    cost = here.GetExactDist2d(spawn.Pos);
                else
                    continue;

                if (!found || cost < bestCost)
                {
                    found = true;
                    bestCost = cost;
                    out = spawn.Pos;
                    outMap = spawn.MapId;
                }
            }
        }
        return found;
    }

    // Best usable item that can be picked, most valuable first; no choice needed for quests without one
    static bool ChooseReward(Player* bot, Quest const* quest, LootItemType& type, uint32& rewardId)
    {
        type = LootItemType::Item;
        rewardId = 0;
        if (!quest->GetRewChoiceItemsCount())
            return true;

        uint32 bestScore = 0;
        bool found = false;
        for (uint32 i = 0; i < quest->RewardChoiceItemId.size(); ++i)
        {
            uint32 id = quest->RewardChoiceItemId[i];
            if (!id || !bot->CanRewardQuest(quest, quest->RewardChoiceItemType[i], id, false))
                continue;

            uint32 score = 1;
            if (quest->RewardChoiceItemType[i] == LootItemType::Item)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
                {
                    if ((proto->IsWeapon() || proto->IsArmor()) && proto->IsUsableByLootSpecialization(bot, false) && bot->CanUseItem(proto) == EQUIP_ERR_OK)
                        score += 1000000;
                    score += proto->GetSellPrice() / 100;
                }

            if (!found || score > bestScore)
            {
                found = true;
                bestScore = score;
                type = quest->RewardChoiceItemType[i];
                rewardId = id;
            }
        }

        return found;
    }

    State _state = State::Idle;
    uint32 _questId = 0;
    Position _dest;
    float _standoff = 0.0f;          // yards short of a giver or turn-in NPC the walk ends, rolled when its spot is chosen
    uint32 _destMap = 0;
    uint32 _currentMap = 0;      // map of the bot when a destination without a map is set
    uint32 _assignedQuest = 0;
    uint32 _routeFailures = 0;
    bool _routeAsked = false;
    uint32 _routeAskedAt = 0;
    bool _hasDest = false;
    uint32 _stepNotBefore = 0;
    bool _moveIssued = false;
    bool _hasProgressDist = false;
    float _bestDist = 0.0f;
    uint32 _nextSelect = 0;
    uint32 _epoch = 0;
    bool _skipLog = false;
    uint32 _questStarted = 0;
    uint32 _lastProgress = 0;
    std::unordered_set<uint32> _failed;
};
}

std::unique_ptr<Strategy> CreateQuestStrategy()
{
    class QuestStrategy : public Strategy
    {
    public:
        QuestStrategy() : Strategy("quest") { AddAction(std::make_unique<QuestAction>()); }
    };

    return std::make_unique<QuestStrategy>();
}
}
