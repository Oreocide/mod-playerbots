#include "RpgStrategy.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "DBCEnums.h"
#include "Economy/BotProfessions.h"
#include "Economy/GatherNodes.h"
#include "Engine.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Loot.h"
#include "LootPackets.h"
#include "MotionMaster.h"
#include "NpcDispatch.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "QuestDef.h"
#include "QuestTravel.h"
#include "Random.h"
#include "RpgInfo.h"
#include "Travel/HubIndex.h"
#include "Travel/MoveToward.h"
#include "RandomBotMgr.h"
#include "Travel/Taxi.h"
#include "UnitDefines.h"
#include "WorldSession.h"
#include <algorithm>
#include <vector>

namespace Playerbots
{
namespace
{
constexpr float Relevance = 11.0f;

bool InCapital(Player const* bot)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    return zone && zone->GetFlags().HasFlag(AreaFlags::LinkedChat);
}

uint32 FirstLogQuest(Player* bot, bool incompleteOnly)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 id = bot->GetQuestSlotQuestId(slot);
        if (!id)
            continue;
        QuestStatus status = bot->GetQuestStatus(id);
        if (incompleteOnly && status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
            continue;
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
            return id;
    }
    return 0;
}

void NoteProgress(RpgInfo& rpg, Player* bot, uint32 now)
{
    if (!rpg.HasDest)
        return;
    float dist = bot->GetExactDist2d(rpg.Dest);
    if (rpg.BestDist <= 0.0f || dist + 5.0f < rpg.BestDist)
    {
        rpg.BestDist = dist;
        rpg.LastProgress = now;
    }
}

// The hotspot a random bot belongs to, nullptr if it has none or has been moved away from it
RandomBotMgr::Anchor const* CurrentAnchor(Player const* bot)
{
    RandomBotMgr::Anchor const* anchor = sRandomBotMgr->GetAnchor(bot->GetGUID());
    if (!anchor)
        return nullptr;

    if (bot->GetMapId() != anchor->MapId || bot->GetExactDist2d(anchor->Center) > anchor->Radius * 4.0f + 300.0f)
    {
        sRandomBotMgr->ClearAnchor(bot->GetGUID());
        return nullptr;
    }

    return anchor;
}

void RandomChangeStatus(PlayerbotAI& ai, uint32 now)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();

    std::vector<std::pair<RpgStatus, uint32>> weighted;
    uint32 sum = 0;

    // A bot placed at a hotspot lives there: town activities only. One that ended up far from it (summoned,
    // pushed to another map) is released
    RandomBotMgr::Anchor const* anchor = CurrentAnchor(bot);

    auto add = [&](RpgStatus status)
    {
        uint32 weight = sPlayerbotsConfig.RpgWeight[uint8(status)];
        if (!weight)
            return;
        if (anchor && (status == RpgStatus::DoQuest || status == RpgStatus::GoGrind || status == RpgStatus::TravelFlight || status == RpgStatus::Gather))
            return;
        if (status == RpgStatus::DoQuest && !FirstLogQuest(bot, true) && !QuestTravel::IsBuilt())
            return;
        if (status == RpgStatus::Gather && (bot->GetLevel() < 10 || !GatherNodes::Count() || !BotProfessions::CanGather(bot)))
            return;
        if (status == RpgStatus::TravelFlight)
        {
            if (bot->IsInFlight() || !bot->HasEnoughMoney(uint64(10000)) || !HubIndex::Nearest(bot, HubKind::FlightMaster, 800.0f))
                return;
        }
        if ((status == RpgStatus::WanderNpc || status == RpgStatus::GoCamp) && InCapital(bot) && bot->GetLevel() >= 10)
            weight += sPlayerbotsConfig.RpgCapitalBoost;
        weighted.emplace_back(status, weight);
        sum += weight;
    };

    add(RpgStatus::DoQuest);
    add(RpgStatus::WanderNpc);
    add(RpgStatus::TravelFlight);
    add(RpgStatus::GoGrind);
    add(RpgStatus::GoCamp);
    add(RpgStatus::Rest);
    add(RpgStatus::Gather);
    add(RpgStatus::WanderRandom);

    if (weighted.empty() || !sum)
    {
        rpg.Begin(RpgStatus::Rest, now);
        return;
    }

    uint32 roll = urand(1, sum);
    uint32 acc = 0;
    RpgStatus chosen = RpgStatus::Rest;
    for (auto const& [status, weight] : weighted)
    {
        acc += weight;
        if (acc >= roll)
        {
            chosen = status;
            break;
        }
    }

    rpg.Begin(chosen, now);
}

bool WalkOrStuck(PlayerbotAI& ai, uint32 now)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();
    if (!rpg.HasDest)
        return false;

    NoteProgress(rpg, bot, now);
    if (now - rpg.LastProgress >= sPlayerbotsConfig.RpgStuckTime)
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        float z;
        if (TryGetStandZ(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), z))
            bot->NearTeleportTo(bot->GetPositionX(), bot->GetPositionY(), z, bot->GetOrientation());
        rpg.ChangeToIdle();
        rpg.Started = now;
        return true;
    }

    return MoveToward(bot, rpg.Dest, 5.0f);
}

// a player who stops standing around stays on his feet, now and then waves or laughs, and only sits to eat or drink
bool DoRest(PlayerbotAI& ai, uint32 now)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();

    bool needsRest = bot->GetHealthPct() < 70.0f
        || (bot->GetPowerType() == POWER_MANA && bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < 50.0f);
    if (needsRest)
    {
        if (bot->GetStandState() != UNIT_STAND_STATE_SIT)
            bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }

    if (bot->GetStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    static constexpr Emote IdleEmotes[] = { EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_YES, EMOTE_ONESHOT_NO, EMOTE_ONESHOT_TALK };
    if (roll_chance(30))
        bot->HandleEmoteCommand(IdleEmotes[urand(0, std::size(IdleEmotes) - 1)]);
    rpg.WaitUntil = now + urand(8000, 25000);
    return true;
}

class RpgAction : public Action
{
public:
    char const* GetName() const override { return "rpg"; }

    float GetRelevance(PlayerbotAI& ai) override
    {
        if (!sPlayerbotsConfig.RpgEnable)
            return 0.0f;
        Player* bot = ai.GetBot();
        if (ai.GetMaster() || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_CASTING))
            return 0.0f;
        return Relevance;
    }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();
        uint32 now = GameTime::GetGameTimeMS();

        if (rpg.WaitUntil && now < rpg.WaitUntil)
            return true;

        // whatever ended a gathering early (timeout, new activity) must not leave the bot kneeling
        if (rpg.Working && rpg.Status != RpgStatus::Gather)
        {
            bot->SetEmoteState(EMOTE_ONESHOT_NONE);
            rpg.Working = false;
        }

        if (rpg.Status != RpgStatus::Idle)
        {
            uint32 duration = sPlayerbotsConfig.RpgDurationMs[uint8(rpg.Status)];
            if (rpg.TimedOut(now, duration))
            {
                rpg.ChangeToIdle();
                rpg.Started = now;
            }
        }

        switch (rpg.Status)
        {
            case RpgStatus::Idle:
                RandomChangeStatus(ai, now);
                return true;
            case RpgStatus::Rest:
                return DoRest(ai, now);
            case RpgStatus::WanderRandom:
                return DoWander(ai, now);
            case RpgStatus::GoGrind:
                return DoGrind(ai, now);
            case RpgStatus::GoCamp:
                return DoCamp(ai, now);
            case RpgStatus::WanderNpc:
                return DoWanderNpc(ai, now);
            case RpgStatus::TravelFlight:
                return DoFlight(ai, now);
            case RpgStatus::Gather:
                return DoGather(ai, now);
            case RpgStatus::DoQuest:
                return DoQuest(ai, now);
        }
        return false;
    }

private:
    bool DoWander(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();
        if (!ai.HasHome())
            ai.SetHome(bot->GetPosition());
        if (IsMovingOnFoot(bot))
            return true;

        Position dest = bot->GetPosition();
        float radius = std::max(5.0f, sPlayerbotsConfig.RandomWanderRadius);
        float hop = frand(radius * 0.25f, radius);
        float angle = frand(-float(M_PI), float(M_PI));

        // an anchored bot wanders around its hotspot: a hop that would leave it turns back towards the centre
        if (RandomBotMgr::Anchor const* anchor = CurrentAnchor(bot))
        {
            if (bot->GetExactDist2d(anchor->Center) + hop > anchor->Radius)
                angle = bot->GetAbsoluteAngle(anchor->Center.GetPositionX(), anchor->Center.GetPositionY()) + frand(-float(M_PI) / 4.0f, float(M_PI) / 4.0f);
        }

        bot->MovePositionToFirstCollision(dest, hop, angle);
        dest.m_positionZ = GetStandZ(bot, dest.GetPositionX(), dest.GetPositionY(), ai.GetHome().GetPositionZ());
        rpg.SetDest(dest);
        rpg.LastProgress = now;
        MoveToward(bot, dest, 3.0f);
        return true;
    }

    // Gathering is scenery: a bot with herbalism or mining walks to a node near it and works at it with the matching
    // emote, like a player would. The node is NOT used, nothing is looted and the bot's skill does not matter, so the
    // world's nodes stay for the players and nothing a bot owns or can do depends on them. (Only bots that a player can
    // see run the rpg action at all, the others are frozen.)
    bool DoGather(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();

        auto remember = [&rpg](uint64 spawnId)
        {
            if (rpg.RecentNodes.size() >= 24)
                rpg.RecentNodes.erase(rpg.RecentNodes.begin());
            rpg.RecentNodes.push_back(spawnId);
        };

        // done working: stand up and look for the next node
        if (rpg.Working)
        {
            bot->SetEmoteState(EMOTE_ONESHOT_NONE);
            rpg.Working = false;
            remember(rpg.NodeSpawn);
            rpg.HasDest = false;
            rpg.WaitUntil = now + urand(1500, 4000);
            return true;
        }

        if (!rpg.HasDest)
        {
            bool herbs = bot->HasSkill(SKILL_HERBALISM);
            bool ore = bot->HasSkill(SKILL_MINING);
            GatherNodes::Node const* node = GatherNodes::Nearest(bot, float(sPlayerbotsConfig.RpgGatherDistance), [&](GatherNodes::Node const& candidate)
            {
                return (candidate.Herb ? herbs : ore)
                    && std::ranges::find(rpg.RecentNodes, candidate.SpawnId) == rpg.RecentNodes.end();
            });
            if (!node)
            {
                rpg.ChangeToIdle();
                rpg.Started = now;
                return true;
            }

            rpg.SetDest(Position(node->X, node->Y, node->Z));
            rpg.NodeSpawn = node->SpawnId;
            rpg.NodeEntry = node->Entry;
            rpg.LastProgress = now;
            return true;
        }

        if (bot->GetExactDist2d(rpg.Dest) > 3.0f)
        {
            if (!WalkOrStuck(ai, now))
            {
                remember(rpg.NodeSpawn);
                rpg.HasDest = false;
            }
            return true;
        }

        // at the node: face it and work for a few seconds
        if (bot->IsMounted())
            bot->CancelMountAura();
        bot->StopMoving();
        bot->SetFacingTo(bot->GetAbsoluteAngle(rpg.Dest.GetPositionX(), rpg.Dest.GetPositionY()));
        bool herb = bot->HasSkill(SKILL_HERBALISM) && (!bot->HasSkill(SKILL_MINING) || urand(0, 1));
        bot->SetEmoteState(herb ? EMOTE_STATE_KNEEL : EMOTE_STATE_WORK_MINING);
        rpg.Working = true;
        rpg.WaitUntil = now + urand(3000, 5500);
        return true;
    }

    bool DoGrind(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();
        if (!rpg.HasDest)
        {
            Position pos;
            if (!QuestTravel::NearestHunt(bot, pos))
            {
                rpg.ChangeToIdle();
                rpg.Started = now;
                return true;
            }
            rpg.SetDest(pos);
            rpg.LastProgress = now;
        }
        if (bot->GetExactDist2d(rpg.Dest) < 12.0f)
        {
            rpg.Begin(RpgStatus::WanderRandom, now);
            return true;
        }
        return WalkOrStuck(ai, now) || true;
    }

    bool DoCamp(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();
        if (!rpg.HasDest)
        {
            Position target;
            bool found = false;

            // an anchored bot camps at a service NPC of its own town, or at its marker
            if (RandomBotMgr::Anchor const* anchor = CurrentAnchor(bot))
            {
                found = true;
                target = anchor->Center;
                if (HubCluster const* cluster = HubIndex::NearestCluster(anchor->MapId, anchor->Center, anchor->Radius, Player::TeamForRace(bot->GetRace())))
                {
                    // one of the services within reach of the spot where the bot lives: auction house, bank, mailbox...
                    static constexpr HubKind Camps[] = { HubKind::Auctioneer, HubKind::Banker, HubKind::Mailbox, HubKind::Innkeeper,
                        HubKind::FlightMaster, HubKind::Vendor };
                    std::vector<Position> options;
                    for (HubKind kind : Camps)
                        if (HubSpot const* spot = HubIndex::NearestInCluster(*cluster, kind, anchor->Center))
                            if (spot->Pos.GetExactDist2d(anchor->Center) <= anchor->Radius * 1.25f)
                                options.push_back(spot->Pos);

                    if (!options.empty())
                        target = options[urand(0, uint32(options.size() - 1))];
                }
            }
            else if (HubSpot const* spot = HubIndex::NearestAny(bot, { HubKind::Innkeeper, HubKind::FlightMaster, HubKind::Vendor }, 4000.0f))
            {
                found = true;
                target = spot->Pos;
            }

            if (!found)
            {
                rpg.ChangeToIdle();
                rpg.Started = now;
                return true;
            }
            rpg.SetDest(target);
            rpg.LastProgress = now;
        }
        if (bot->GetExactDist2d(rpg.Dest) < 12.0f)
        {
            rpg.Begin(RpgStatus::WanderNpc, now);
            return true;
        }
        return WalkOrStuck(ai, now) || true;
    }

    bool DoWanderNpc(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();

        if (rpg.Target.IsEmpty())
            rpg.Target = ChooseNpcTarget(bot);

        WorldObject* object = !rpg.Target.IsEmpty() ? ObjectAccessor::GetWorldObject(*bot, rpg.Target) : nullptr;
        if (!object)
        {
            if (GameObject* mailbox = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, 40.0f))
                object = mailbox;
        }
        if (!object)
        {
            rpg.ChangeToIdle();
            rpg.Started = now;
            return true;
        }

        rpg.SetDest(object->GetPosition());
        if (WalkOrStuck(ai, now))
            return true;

        TryInteract(bot, object);
        rpg.WaitUntil = now + sPlayerbotsConfig.RpgNpcStayTime;
        rpg.Target.Clear();
        rpg.HasDest = false;
        return true;
    }

    bool DoFlight(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();
        if (bot->IsInFlight())
            return true;

        HubSpot const* fmSpot = HubIndex::Nearest(bot, HubKind::FlightMaster, 4000.0f);
        if (!fmSpot)
        {
            rpg.ChangeToIdle();
            rpg.Started = now;
            return true;
        }

        rpg.SetDest(fmSpot->Pos);
        if (WalkOrStuck(ai, now))
            return true;

        Creature* fm = bot->FindNearestCreature(fmSpot->Entry, 12.0f, true);
        if (!fm || !fm->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER))
        {
            rpg.ChangeToIdle();
            rpg.Started = now;
            return true;
        }

        uint32 from = sObjectMgr->GetNearestTaxiNode(fm->GetPositionX(), fm->GetPositionY(), fm->GetPositionZ(), fm->GetMapId(), bot->GetTeam());
        if (!rpg.TaxiDestNode)
            PickRandomTaxiDestination(bot, from, rpg.TaxiDestNode);

        if (rpg.TaxiDestNode && TryTakeTaxi(bot, fm, rpg.TaxiDestNode))
        {
            rpg.WaitUntil = now + 5000;
            return true;
        }

        rpg.ChangeToIdle();
        rpg.Started = now;
        return true;
    }

    bool DoQuest(PlayerbotAI& ai, uint32 now)
    {
        Player* bot = ai.GetBot();
        RpgInfo& rpg = ai.GetRpg();
        if (!rpg.QuestId)
            rpg.QuestId = FirstLogQuest(bot, true);

        if (!rpg.QuestId)
        {
            rpg.ChangeToIdle();
            rpg.Started = now;
            return true;
        }

        bool turnIn = bot->GetQuestStatus(rpg.QuestId) == QUEST_STATUS_COMPLETE;
        if (!rpg.HasDest)
        {
            Position poi;
            if (QuestTravel::PickPoi(bot, rpg.QuestId, turnIn, poi))
            {
                rpg.SetDest(poi);
                rpg.LastProgress = now;
            }
            else
            {
                rpg.WaitUntil = now + std::min(sPlayerbotsConfig.RpgPoiStayTime, 15000u);
                return true;
            }
        }

        if (bot->GetExactDist2d(rpg.Dest) < 8.0f)
        {
            rpg.HasDest = false;
            rpg.WaitUntil = now + 4000;
            return true;
        }
        return WalkOrStuck(ai, now) || true;
    }
};

class RpgStrategy : public Strategy
{
public:
    RpgStrategy() : Strategy("rpg") { AddAction(std::make_unique<RpgAction>()); }
};
}

std::unique_ptr<Strategy> CreateRpgStrategy()
{
    return std::make_unique<RpgStrategy>();
}
}
