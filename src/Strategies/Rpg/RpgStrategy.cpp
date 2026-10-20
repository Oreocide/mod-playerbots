#include "RpgStrategy.h"
#include "Ambient.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "DBCEnums.h"
#include "Economy/BotProfessions.h"
#include "Goal/GoalStore.h"
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
#include "Log.h"
#include "Travel/StuckDiagnosis.h"
#include "PlayerbotsConfig.h"
#include "QuestDef.h"
#include "QuestTravel.h"
#include "Random.h"
#include "RpgInfo.h"
#include "StatusPick.h"
#include "Travel/HubIndex.h"
#include "Travel/MoveToward.h"
#include "RandomBotMgr.h"
#include "Spawn/SpawnPolicy.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
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

// Each task lasts somewhere between half and all of its configured duration, so a city visit is not a fixed 5 minutes
uint32 RollRpgDuration(RpgStatus status)
{
    uint32 cap = sPlayerbotsConfig.RpgDurationMs[uint8(status)];
    if (cap <= 2000)
        return cap;
    return urand(cap / 2, cap);
}

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

// The hotspot a random bot belongs to (a copy), empty if it has none or has been moved away from it
Optional<RandomBotMgr::Anchor> CurrentAnchor(Player const* bot)
{
    Optional<RandomBotMgr::Anchor> anchor = sRandomBotMgr->GetAnchor(bot->GetGUID());
    if (!anchor)
        return {};

    if (bot->GetMapId() != anchor->MapId || bot->GetExactDist2d(anchor->Center) > anchor->Radius * 4.0f + 300.0f)
    {
        sRandomBotMgr->ClearAnchor(bot->GetGUID());
        return {};
    }

    return anchor;
}

// The AC weighted roll. excluded: statuses (bit per RpgStatus) not to roll, for a status that just found nothing to do so the roll
// does not pick it again at once. abstractMode: a frozen bot living the status as a timer (no pathing, no NPC, no quest log): only
// the statuses that mean something without a world around the bot are rolled (see AbstractLife)
RpgStatus ChooseStatus(PlayerbotAI& ai, uint32 excluded, bool abstractMode)
{
    Player* bot = ai.GetBot();

    std::vector<StatusWeight> weighted;

    // A bot placed at a hotspot lives there: town activities only. One that ended up far from it (summoned,
    // pushed to another map) is released
    Optional<RandomBotMgr::Anchor> anchor = CurrentAnchor(bot);

    auto add = [&](RpgStatus status)
    {
        uint32 weight = sPlayerbotsConfig.RpgWeight[uint8(status)];
        if (!weight || (excluded & (1u << uint8(status))))
            return;
        if (abstractMode && (status == RpgStatus::TravelFlight || status == RpgStatus::Gather || status == RpgStatus::Craft))
            return;
        if (anchor && (status == RpgStatus::DoQuest || status == RpgStatus::GoGrind || status == RpgStatus::TravelFlight || status == RpgStatus::Gather))
            return;
        if (status == RpgStatus::DoQuest && !FirstLogQuest(bot, true) && !QuestTravel::IsBuilt())
            return;
        if (abstractMode && status == RpgStatus::DoQuest && !QuestTravel::IsBuilt())
            return;
        if (status == RpgStatus::Gather && (bot->GetLevel() < 10 || !GatherNodes::Count() || !BotProfessions::CanGather(bot)))
            return;
        if (status == RpgStatus::Craft && (!InCapital(bot) || !bot->HasSkill(SKILL_BLACKSMITHING)))
            return;
        if (status == RpgStatus::TravelFlight)
        {
            if (bot->IsInFlight() || !bot->HasEnoughMoney(uint64(10000)) || !HubIndex::Nearest(bot, HubKind::FlightMaster, 800.0f))
                return;
        }
        if ((status == RpgStatus::WanderNpc || status == RpgStatus::GoCamp) && InCapital(bot) && bot->GetLevel() >= 10)
            weight += sPlayerbotsConfig.RpgCapitalBoost;
        weighted.emplace_back(uint8(status), weight);
    };

    add(RpgStatus::DoQuest);
    add(RpgStatus::WanderNpc);
    add(RpgStatus::TravelFlight);
    add(RpgStatus::GoGrind);
    add(RpgStatus::GoCamp);
    add(RpgStatus::Rest);
    add(RpgStatus::Gather);
    add(RpgStatus::Craft);
    add(RpgStatus::WanderRandom);

    uint32 sum = SumWeights(weighted);
    if (!sum)
        return RpgStatus::Rest;

    return RpgStatus(PickWeighted(weighted, urand(1, sum), uint8(RpgStatus::Rest)));
}

void RandomChangeStatus(PlayerbotAI& ai, uint32 now, uint32 excluded = 0)
{
    // the goal table first (near bots: only the kinds Playerbots.Goal.NearKinds allows), not after a status found nothing to do
    if (!excluded && GoalStore::TryBegin(ai, now))
        return;

    ai.GetRpg().Begin(ChooseStatus(ai, excluded, false), now);
}

constexpr float WalkerHoldRange = 8.0f;            // a walking NPC stops for a bot this close
constexpr uint32 WalkerHoldMs = 4000;
constexpr uint32 WalkerReaimMs = 800;              // a bot after a walking NPC plans again at most this often
constexpr float WalkerReaimYards = 4.0f;           // ... once the NPC has moved this far from where the path was aimed
constexpr float NpcArriveYards = 3.5f;             // service NPCs only answer within their combat reach + 4 yards: stop well inside that
constexpr uint32 NoPathBlockMs = 10 * 60 * 1000;   // an NPC the navmesh cannot reach is skipped this long
constexpr uint32 NoPathPauseMs = 3000;             // and the bot stands still this long before choosing again

// A bot walking up to an NPC stops a random 1 .. 6 yards short of it, not on its spot. Service NPCs only answer within their combat
// reach + 4 yards, so the roll is capped half a yard inside that: the bot stands at the rolled distance when it interacts
float StandOffFor(RpgInfo& rpg, WorldObject const* object)
{
    if (!sPlayerbotsConfig.RpgNpcStandOff)
        return 0.0f;
    if (rpg.StandOffTarget != object->GetGUID())
    {
        float cap = 4.0f;    // mailboxes and other objects
        if (Creature const* npc = object->ToCreature())
            cap = std::min(6.0f, npc->GetCombatReach() + 3.5f);
        rpg.StandOff = frand(1.0f, std::max(1.0f, cap));
        rpg.StandOffTarget = object->GetGUID();
    }
    return rpg.StandOff;
}

// Why a bot gave up on its target, for the stuck log: a walking NPC (Thomas Miller) that outpaces a bot's relaunches looks like "no headway"
void LogGaveUp(Player* bot, RpgInfo const& rpg, char const* why)
{
    if (rpg.Target.IsEmpty())
        return;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, rpg.Target);
    Unit const* unit = object ? object->ToUnit() : nullptr;
    TC_LOG_INFO("server.playerbots.stuck", "Playerbots: {} gives up on {} ({}): {}, {:.1f} yd away, npc moving {}, goal ({:.1f}, {:.1f}, {:.1f}); {}",
        bot->GetName(), object ? object->GetName() : "an unknown target", rpg.Target.ToString(), why, object ? bot->GetExactDist(object) : -1.0f,
        unit && unit->isMoving() ? "yes" : "no", rpg.Dest.GetPositionX(), rpg.Dest.GetPositionY(), rpg.Dest.GetPositionZ(), DescribeStuck(bot, nullptr));
}

bool WalkOrStuck(PlayerbotAI& ai, uint32 now, float standoff = 0.0f)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();
    if (!rpg.HasDest)
        return false;

    NoteProgress(rpg, bot, now);
    if (now - rpg.LastProgress >= sPlayerbotsConfig.RpgStuckTime)
    {
        // Stuck: stop and pick something else. No height-snap teleport: a bot must never be moved to a floor the navmesh
        // did not put it on (it fell through the world that way).
        LogGaveUp(bot, rpg, "no headway");
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        if (!rpg.Target.IsEmpty())
            rpg.BlockTarget(rpg.Target, now, NoPathBlockMs);
        rpg.ChangeToIdle();
        rpg.Started = now;
        return true;
    }

    MoveResult result = MoveAlongMesh(bot, rpg.Dest, std::max(NpcArriveYards, standoff + 0.5f), standoff);
    // within reach of the NPC but still walking the last yards to the stand-off point: the visit starts when the bot has stopped
    if (result == MoveResult::Arrived && standoff > 0.0f && IsMovingOnFoot(bot))
        return true;
    if (result == MoveResult::NoPath)
    {
        // The navmesh has no way there: skip this target for a while instead of waiting out the stuck timer
        LogGaveUp(bot, rpg, "no mesh path");
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        if (!rpg.Target.IsEmpty())
            rpg.BlockTarget(rpg.Target, now, NoPathBlockMs);
        rpg.ChangeToIdle();
        rpg.Started = now;
        rpg.WaitUntil = now + NoPathPauseMs;
        return true;
    }

    return result == MoveResult::Moving;
}

// a player who stops standing around stays on his feet, now and then waves or laughs, and only sits to eat or drink
bool DoRest(PlayerbotAI& ai, uint32 now)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();

    if (Ambient::TryChair(ai))
        return true;

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

    // a player standing around greets someone who comes close, otherwise emotes now and then and hops once in a while
    static constexpr Emote IdleEmotes[] = { EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_YES, EMOTE_ONESHOT_NO, EMOTE_ONESHOT_TALK };
    if (!Ambient::GreetNearbyPlayer(ai, now))
    {
        if (roll_chance(30))
            bot->HandleEmoteCommand(IdleEmotes[urand(0, std::size(IdleEmotes) - 1)]);
        Ambient::IdleHop(ai, now);
    }
    rpg.WaitUntil = now + urand(8000, 25000);
    return true;
}

// Anvil, Forge, Black Anvil, Black Forge (SpellFocusObject, build 12.1.0.69933)
bool IsForge(GameObject const* go)
{
    uint32 focus = go->GetGOInfo()->GetSpellFocusType();
    return focus == 1 || focus == 3 || focus == 543 || focus == 623;
}

uint32 PickCraftSpell(Player* bot)
{
    std::vector<uint32> spells;
    for (auto const& [id, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE);
        if (!info || !info->IsAbilityOfSkillType(SKILL_BLACKSMITHING))
            continue;

        bool makesItem = false;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
                makesItem = true;
        if (!makesItem)
            continue;

        bool canPay = true;
        for (size_t i = 0; i < info->Reagent.size(); ++i)
            if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0 && !bot->HasItemCount(uint32(info->Reagent[i]), uint32(info->ReagentCount[i])))
                canPay = false;
        if (canPay)
            spells.push_back(id);
    }

    if (spells.empty())
        return 0;
    return spells[urand(0, uint32(spells.size() - 1))];
}

bool DoCraft(PlayerbotAI& ai, uint32 now)
{
    Player* bot = ai.GetBot();
    RpgInfo& rpg = ai.GetRpg();

    if (!rpg.HasDest)
    {
        FindGameObjectOptions options;
        options.GameObjectType = GAMEOBJECT_TYPE_SPELL_FOCUS;
        std::vector<GameObject*> objects;
        bot->GetGameObjectListWithOptionsInGrid(objects, 80.0f, options);

        GameObject* best = nullptr;
        float bestDist = 80.0f;
        for (GameObject* go : objects)
        {
            if (!go || !IsForge(go))
                continue;
            if (SpawnPolicy::IsSplitDistrict(bot->GetMapId(), go->GetAreaId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), Player::TeamForRace(bot->GetRace())))
                continue;
            float dist = bot->GetExactDist2d(go);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = go;
            }
        }

        if (!best)
        {
            rpg.ChangeToIdle();
            return true;
        }

        rpg.Target = best->GetGUID();
        rpg.SetDest(best->GetPosition());
    }

    MoveResult result = MoveAlongMesh(bot, rpg.Dest, 3.0f);
    if (result == MoveResult::NoPath)
    {
        rpg.ChangeToIdle();
        rpg.Started = now;
        return true;
    }
    if (result == MoveResult::Moving)
        return true;

    if (!rpg.Working)
    {
        bot->SetEmoteState(EMOTE_STATE_WORK);
        rpg.Working = true;
        rpg.WaitUntil = now + urand(3000, 6000);
        return true;
    }

    // NodeEntry marks the one item this visit already tried to make
    if (!rpg.NodeEntry)
    {
        rpg.NodeEntry = 1;
        if (uint32 spell = PickCraftSpell(bot))
            bot->CastSpell(bot, spell);
        rpg.WaitUntil = now + urand(4000, 8000);
    }
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
        if (ai.GetMaster() || !bot->IsAlive() || bot->IsInCombat() || !bot->getAttackers().empty()
            || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_CASTING))
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

        Ambient::WalkingHop(ai, now);

        // whatever ended a gathering early (timeout, new activity) must not leave the bot kneeling
        if (rpg.Working && rpg.Status != RpgStatus::Gather && rpg.Status != RpgStatus::Craft)
        {
            bot->SetEmoteState(EMOTE_ONESHOT_NONE);
            rpg.Working = false;
        }

        if (rpg.Status != RpgStatus::Idle)
        {
            if (!rpg.DurationMs)
                rpg.DurationMs = RollRpgDuration(rpg.Status);
            if (rpg.TimedOut(now, rpg.DurationMs))
            {
                if (rpg.Status == RpgStatus::Rest)
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
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
            case RpgStatus::Craft:
                return DoCraft(ai, now);
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

        // a hop (or anything else) that stopped the walk early: carry on to the same point instead of choosing a new direction
        if (rpg.HasDest && bot->GetExactDist2d(rpg.Dest) > 3.0f)
        {
            MoveResult resumed = MoveAlongMesh(bot, rpg.Dest, 3.0f);
            if (resumed == MoveResult::Moving)
                return true;
        }

        Position dest = bot->GetPosition();
        float radius = std::max(5.0f, sPlayerbotsConfig.RandomWanderRadius);
        float hop = frand(radius * 0.25f, radius);
        float angle = frand(-float(M_PI), float(M_PI));

        // an anchored bot wanders around its hotspot: a hop that would leave it turns back towards the centre
        if (Optional<RandomBotMgr::Anchor> anchor = CurrentAnchor(bot))
        {
            if (bot->GetExactDist2d(anchor->Center) + hop > anchor->Radius)
                angle = bot->GetAbsoluteAngle(anchor->Center.GetPositionX(), anchor->Center.GetPositionY()) + frand(-float(M_PI) / 4.0f, float(M_PI) / 4.0f);
        }

        bot->MovePositionToFirstCollision(dest, hop, angle);
        rpg.SetDest(dest);
        rpg.LastProgress = now;
        if (MoveAlongMesh(bot, dest, 3.0f) == MoveResult::NoPath)
        {
            // a point the mesh cannot reach: try another one shortly
            rpg.HasDest = false;
            rpg.WaitUntil = now + 1000;
        }
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
                // no hunting ground in reach: another status, never the same grind roll again
                RandomChangeStatus(ai, now, (1u << uint8(RpgStatus::GoGrind)) | (1u << uint8(RpgStatus::DoQuest)));
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
            if (Optional<RandomBotMgr::Anchor> anchor = CurrentAnchor(bot))
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

        if (rpg.Target.IsEmpty() && rpg.ForceKind >= 0)
        {
            // test harness (.bot rpg <bot> innkeeper): walk to the nearest service of that kind, wherever it is
            HubSpot const* spot = HubIndex::Nearest(bot, HubKind(rpg.ForceKind), 4000.0f);
            if (!spot)
            {
                rpg.ChangeToIdle();
                rpg.Started = now;
                return true;
            }

            if (Creature* npc = bot->FindNearestCreature(spot->Entry, 60.0f, true))
                rpg.Target = npc->GetGUID();
            else
            {
                rpg.SetDest(spot->Pos);
                if (WalkOrStuck(ai, now))
                    return true;

                rpg.ChangeToIdle();   // arrived but the NPC is not there
                rpg.Started = now;
                return true;
            }
        }

        if (rpg.Target.IsEmpty())
            rpg.Target = ChooseNpcTarget(bot, &rpg.BadTargets, now);

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

        // An NPC that walks (a baker on his round): a person runs after him and stops him by talking to him. A path is only planned
        // when the last one has ended, so the bot would trail him; it re-aims when he has moved on, and within a few yards he holds still
        if (Creature* walker = object->ToCreature(); walker && walker->IsAlive() && !walker->IsInCombat()
            && (walker->isMoving() || walker->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE))
        {
            if (bot->GetExactDist(walker) <= WalkerHoldRange)
                walker->PauseMovement(WalkerHoldMs);
            else if (!IsMovingOnFoot(bot))
            {
                rpg.LaunchDest = walker->GetPosition();
                rpg.LaunchedAt = now;
            }
            else if (now - rpg.LaunchedAt > WalkerReaimMs && rpg.LaunchDest.GetExactDist2d(walker) > WalkerReaimYards)
            {
                bot->StopMoving();
                rpg.LaunchDest = walker->GetPosition();
                rpg.LaunchedAt = now;
            }
        }

        rpg.SetDest(object->GetPosition());
        if (WalkOrStuck(ai, now, StandOffFor(rpg, object)))
            return true;

        TryInteract(bot, object);
        rpg.StandOffTarget.Clear();   // a later visit to this NPC rolls its own distance
        // do not walk straight back to the same NPC; an innkeeper visit is a short stop
        rpg.BlockTarget(object->GetGUID(), now, urand(90 * 1000, 180 * 1000));
        uint32 stay = sPlayerbotsConfig.RpgNpcStayTime;
        if (Creature const* npc = object->ToCreature(); npc && npc->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            stay = urand(2000, 4000);
        else if (stay > 2000)
            stay = urand(stay / 2, stay);
        bool forced = rpg.ForceKind >= 0;
        rpg.WaitUntil = now + stay;
        rpg.Target.Clear();
        rpg.HasDest = false;
        if (forced)
        {
            // harness visit done: stand around like a player, then the normal rolls resume
            rpg.Begin(RpgStatus::Rest, now);
            rpg.WaitUntil = now + sPlayerbotsConfig.RpgNpcStayTime;
        }
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
        // the stand-off needs the flight master itself (its reach); on a long leg it is not in sight yet and the walk ends on the spot
        Creature* fmNear = bot->FindNearestCreature(fmSpot->Entry, 60.0f, true);
        if (WalkOrStuck(ai, now, fmNear ? StandOffFor(rpg, fmNear) : 0.0f))
            return true;
        rpg.StandOffTarget.Clear();

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
            // nothing in the quest log to work on: another status now, not Idle and the same roll again
            RandomChangeStatus(ai, now, 1u << uint8(RpgStatus::DoQuest));
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

RpgStatus PickAbstractRpgStatus(PlayerbotAI& ai)
{
    return ChooseStatus(ai, 0, true);
}

uint32 RollAbstractRpgDuration(RpgStatus status)
{
    return RollRpgDuration(status);
}
}
