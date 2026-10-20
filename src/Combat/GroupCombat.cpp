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

#include "Combat/GroupCombat.h"
#include "Combat/Positioning.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "Engine/AiRegistry.h"
#include "GameTime.h"
#include "Group.h"
#include "LFG.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "ThreatManager.h"
#include "Travel/BotMover.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Playerbots
{
namespace
{
constexpr uint32 MaxEnemies = 16;
constexpr uint32 PositionCooldownMs = 3000;
constexpr float GroupRadius = 45.0f;
constexpr float FrontHalfArc = Pi / 3.0f;

GroupRole ToGroupRole(BotRole role)
{
    switch (role)
    {
        case BotRole::Tank:
            return GroupRole::Tank;
        case BotRole::Healer:
            return GroupRole::Healer;
        case BotRole::RangedDps:
            return GroupRole::RangedDps;
        default:
            return GroupRole::MeleeDps;
    }
}

uint64 IdOf(Player const* player) { return player->GetGUID().GetCounter(); }

RoleSlot const* SlotOf(GroupRolesInfo const& info, uint64 id)
{
    for (RoleSlot const& slot : info.Slots)
        if (slot.Id == id)
            return &slot;
    return nullptr;
}

/// Enemies that fight the bot or a group member on its map, valid targets for the bot, nearest first, at most MaxEnemies
std::vector<ObjectGuid> CollectAttackers(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    std::vector<Unit*> found;
    std::unordered_set<ObjectGuid> seen;

    auto add = [&](Unit* unit)
    {
        if (unit && found.size() < MaxEnemies && ai.IsValidEnemy(unit) && seen.insert(unit->GetGUID()).second)
            found.push_back(unit);
    };

    for (Unit* attacker : bot->getAttackers())
        add(attacker);

    if (Group* group = bot->GetGroup())
        for (GroupReference const& ref : group->GetMembers())
        {
            Player* member = ref.GetSource();
            if (!member || member == bot || !member->IsInMap(bot))
                continue;
            for (Unit* attacker : member->getAttackers())
                add(attacker);
        }

    std::ranges::sort(found, {}, [bot](Unit* unit) { return bot->GetExactDistSq(unit); });

    std::vector<ObjectGuid> guids;
    guids.reserve(found.size());
    for (Unit* unit : found)
        guids.push_back(unit->GetGUID());
    return guids;
}

Position GroupCenter(Player* bot, Group* group, bool& found)
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 count = 0;
    for (GroupReference const& ref : group->GetMembers())
    {
        Player* member = ref.GetSource();
        if (!member || member == bot || !member->IsAlive() || !member->IsInMap(bot) || !bot->IsWithinDist(member, GroupRadius))
            continue;
        x += member->GetPositionX();
        y += member->GetPositionY();
        z += member->GetPositionZ();
        ++count;
    }
    found = count > 0;
    return count ? Position(x / count, y / count, z / count) : Position();
}

/// Snaps the wish to the navmesh and walks there as a combat move
bool StepTo(PlayerbotAI& ai, Pt wish, float z, MovePriority priority)
{
    Player* bot = ai.GetBot();
    Position snapped;
    if (!SnapToMesh(bot, Position(wish.X, wish.Y, z), snapped, 3.0f, 2.5f))
        return false;
    return MoveCombat(bot, snapped, 1.0f, priority) == MoveResult::Moving;
}

class ThreatMultiplier final : public Multiplier
{
public:
    float GetValue(PlayerbotAI& ai, Action* action) override
    {
        if (!sPlayerbotsConfig.CombatThreatLimitPct || std::strcmp(action->GetName(), "attack") != 0)
            return 1.0f;
        Unit* target = ai.SelectCombatTarget();
        return !target || GroupCombat::ThreatAllows(ai, target) ? 1.0f : 0.0f;
    }
};

class WaitForAttackMultiplier final : public Multiplier
{
public:
    float GetValue(PlayerbotAI& ai, Action* action) override
    {
        if (!sPlayerbotsConfig.CombatWaitForAttackMs || std::strcmp(action->GetName(), "attack") != 0)
            return 1.0f;
        return GroupCombat::WaitAllows(ai) ? 1.0f : 0.0f;
    }
};

class ThreatStrategy final : public Strategy
{
public:
    ThreatStrategy() : Strategy("threat") { }
    uint8_t GetStates() const override { return StrategyStates::Combat; }
    void InitMultipliers(std::vector<std::unique_ptr<Multiplier>>& multipliers) override { multipliers.push_back(std::make_unique<ThreatMultiplier>()); }
};

class WaitForAttackStrategy final : public Strategy
{
public:
    WaitForAttackStrategy() : Strategy("wait for attack") { }
    // the bot's first attack happens before it is in combat itself: active in both situations
    uint8_t GetStates() const override { return StrategyStates::Combat | StrategyStates::NonCombat; }
    void InitMultipliers(std::vector<std::unique_ptr<Multiplier>>& multipliers) override { multipliers.push_back(std::make_unique<WaitForAttackMultiplier>()); }
};
}

namespace GroupCombat
{
BotRole SpecRole(Player const* player)
{
    if (ChrSpecializationEntry const* spec = player->GetPrimarySpecializationEntry())
    {
        switch (spec->GetRole())
        {
            case ChrSpecializationRole::Tank:
                return BotRole::Tank;
            case ChrSpecializationRole::Healer:
                return BotRole::Healer;
            default:
                break;
        }

        if (spec->GetFlags().HasFlag(ChrSpecializationFlag::Ranged) || spec->GetFlags().HasFlag(ChrSpecializationFlag::Caster))
            return BotRole::RangedDps;

        return BotRole::MeleeDps;
    }

    switch (player->GetClass())
    {
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_PRIEST:
        case CLASS_HUNTER:
        case CLASS_EVOKER:
            return BotRole::RangedDps;
        default:
            return BotRole::MeleeDps;
    }
}

GroupRole RoleOf(Group const* group, Player* member)
{
    if (PlayerbotAI* ai = sPlayerbotMgr->GetAI(member))
        return ToGroupRole(ai->GetRole());

    // a player picked a role for the dungeon finder; "damage" says nothing about melee or ranged, the specialization does
    if (group)
    {
        uint8 roles = group->GetLfgRoles(member->GetGUID());
        if (roles & lfg::PLAYER_ROLE_TANK)
            return GroupRole::Tank;
        if (roles & lfg::PLAYER_ROLE_HEALER)
            return GroupRole::Healer;
    }
    return ToGroupRole(SpecRole(member));
}

GroupRolesInfo ComputeRoles(PlayerbotAI& ai)
{
    GroupRolesInfo info;
    Player* bot = ai.GetBot();
    Group* group = bot->GetGroup();
    if (!group)
        return info;

    std::vector<MemberView> members;
    ObjectGuid leader = group->GetLeaderGUID();
    for (GroupReference const& ref : group->GetMembers())
    {
        Player* member = ref.GetSource();
        if (!member || !member->IsInMap(bot))
            continue;

        MemberView view;
        view.Id = IdOf(member);
        view.Role = RoleOf(group, member);
        view.Alive = member->IsAlive();
        view.Leader = member->GetGUID() == leader;
        members.push_back(view);
        if (view.Alive)
            ++info.Size;
    }

    info.Slots = AssignRoles(members);
    uint64 myId = IdOf(bot);
    for (RoleSlot const& slot : info.Slots)
    {
        if (slot.MainTank)
        {
            for (GroupReference const& ref : group->GetMembers())
                if (ref.GetSource() && IdOf(ref.GetSource()) == slot.Id)
                    info.MainTank = ref.GetSource()->GetGUID();
        }
        if (slot.Id == myId)
        {
            info.MyIndex = slot.Index;
            info.MyRole = slot.Role;
            info.IAmMainTank = slot.MainTank;
        }
    }
    return info;
}

Unit* PickTarget(PlayerbotAI& ai)
{
    if (!sPlayerbotsConfig.CombatRoleTargeting)
        return nullptr;

    Player* bot = ai.GetBot();
    if (!bot->GetGroup())
        return nullptr;

    GroupRolesInfo roles = ai.GetContext().GetValue<GroupRolesInfo>("group roles", ai);
    if (roles.Size < 2)
        return nullptr;

    // a player who leads and no tank to follow: assist the player (the older rules)
    if (roles.MainTank.IsEmpty() && ai.GetMaster())
        return nullptr;

    std::vector<ObjectGuid> guids = ai.GetContext().GetValue<std::vector<ObjectGuid>>("attackers", ai);
    if (guids.empty())
        return nullptr;

    Player* mainTank = roles.MainTank.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, roles.MainTank);
    Unit* tankVictim = mainTank ? mainTank->GetVictim() : nullptr;

    std::vector<Unit*> units;
    std::vector<EnemyView> views;
    for (ObjectGuid const& guid : guids)
    {
        Unit* enemy = ObjectAccessor::GetUnit(*bot, guid);
        if (!enemy)
            continue;

        EnemyView view;
        view.Id = guid.GetCounter();
        Unit* victim = enemy->GetVictim();
        view.TargetsMe = victim == bot;
        if (victim && victim->GetTypeId() == TYPEID_PLAYER)
        {
            if (RoleSlot const* slot = SlotOf(roles, IdOf(victim->ToPlayer())))
            {
                view.TargetIsTank = slot->Role == GroupRole::Tank;
                view.TargetIsHealer = slot->Role == GroupRole::Healer;
            }
        }
        view.TankVictim = tankVictim == enemy;
        view.HpPct = enemy->GetHealthPct();
        view.Dist = bot->GetDistance(enemy);
        units.push_back(enemy);
        views.push_back(view);
    }

    int pick = roles.MyRole == GroupRole::Tank ? PickTankTarget(views) : PickDpsTarget(views);
    return pick >= 0 ? units[pick] : nullptr;
}

bool ThreatAllows(PlayerbotAI& ai, Unit* target)
{
    if (!sPlayerbotsConfig.CombatThreatLimitPct || !target)
        return true;

    Player* bot = ai.GetBot();
    GroupRolesInfo roles = ai.GetContext().GetValue<GroupRolesInfo>("group roles", ai);
    if (roles.IAmMainTank || roles.MyRole == GroupRole::Tank || roles.MainTank.IsEmpty())
        return true;

    Player* mainTank = ObjectAccessor::GetPlayer(*bot, roles.MainTank);
    if (!mainTank || !mainTank->IsAlive())
        return true;

    float mine = target->GetThreatManager().GetThreat(bot);
    float theirs = target->GetThreatManager().GetThreat(mainTank);
    return Playerbots::ThreatAllows(mine, theirs, false, float(sPlayerbotsConfig.CombatThreatLimitPct) / 100.0f);
}

bool WaitAllows(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    uint32& fightStart = ai.GroupFightStartMs();

    // a bot that is being attacked defends itself
    if (!bot->getAttackers().empty())
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    // only a group a player leads waits for that player's pull; the wait runs from the moment the leader or the main tank
    // is in combat, not from the moment this bot is
    Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
    bool humanLeads = leader && !sPlayerbotMgr->GetAI(leader);

    bool groupFighting = leader && leader->IsInMap(bot) && leader->IsInCombat();
    if (!groupFighting)
    {
        GroupRolesInfo roles = ai.GetContext().GetValue<GroupRolesInfo>("group roles", ai);
        if (!roles.MainTank.IsEmpty())
            if (Player* tank = ObjectAccessor::GetPlayer(*bot, roles.MainTank))
                groupFighting = tank->IsInCombat();
    }

    uint32 now = GameTime::GetGameTimeMS();
    if (!groupFighting && !bot->IsInCombat())
    {
        fightStart = 0;
        return true;
    }
    if (!fightStart)
        fightStart = now ? now : 1;

    return WaitAllowsAttack(now, fightStart, sPlayerbotsConfig.CombatWaitForAttackMs, humanLeads);
}

bool PositionStep(PlayerbotAI& ai, Unit* target)
{
    bool positioning = sPlayerbotsConfig.CombatPositioning;
    bool kite = sPlayerbotsConfig.CombatKite;
    if ((!positioning && !kite) || !target)
        return false;

    Player* bot = ai.GetBot();
    Group* group = bot->GetGroup();
    if (!group || !CanMove(bot))
        return false;

    uint32 now = GameTime::GetGameTimeMS();
    if (int32(ai.PositionCooldownUntil() - now) > 0)
        return false;

    GroupRolesInfo roles = ai.GetContext().GetValue<GroupRolesInfo>("group roles", ai);
    if (roles.Size < 2)
        return false;

    Pt enemy{ target->GetPositionX(), target->GetPositionY() };
    float orientation = target->GetOrientation();
    float z = target->GetPositionZ();
    float distance = bot->GetExactDist2d(target);
    bool targetsMe = target->GetVictim() == bot;
    bool ranged = roles.MyRole == GroupRole::RangedDps || roles.MyRole == GroupRole::Healer;

    auto done = [&](bool moved)
    {
        if (moved)
            ai.PositionCooldownUntil() = now + PositionCooldownMs;
        return moved;
    };

    // a ranged bot with a melee enemy on it steps away, never back toward where it just came from
    if (kite && ranged && targetsMe && distance < 5.0f)
    {
        std::vector<Hazard> hazards = { { enemy, 6.0f } };
        Pt me{ bot->GetPositionX(), bot->GetPositionY() };
        std::vector<Pt> candidates = SafeSpotCandidates(me, hazards, { 8.0f, 10.0f, 12.0f }, 16, 1.0f);
        int pick = PickSafeSpot(candidates, me, hazards, ai.GetFleeHistory(), now, {}, 3.0f);
        if (pick >= 0 && StepTo(ai, candidates[pick], bot->GetPositionZ(), MovePriority::Avoid))
        {
            NoteFlee(ai.GetFleeHistory(), std::atan2(candidates[pick].Y - me.Y, candidates[pick].X - me.X), now);
            return done(true);
        }
    }

    if (!positioning || target->isMoving())
        return false;

    bool haveCenter = false;
    Position center = GroupCenter(bot, group, haveCenter);

    if (roles.MyRole == GroupRole::Tank)
    {
        // the enemy faces the group: step to its far side so it turns, and its cleaves and breaths turn away
        if (targetsMe && roles.Size >= 3 && haveCenter && distance <= 8.0f
            && InFrontArc(enemy, orientation, { center.GetPositionX(), center.GetPositionY() }, FrontHalfArc))
        {
            float reach = std::max(1.5f, (bot->GetCombatReach() + target->GetCombatReach()) * 0.6f);
            return done(StepTo(ai, TankFacePoint(enemy, { center.GetPositionX(), center.GetPositionY() }, reach), z, MovePriority::Position));
        }
        return false;
    }

    if (roles.MyRole == GroupRole::MeleeDps)
    {
        // melee attackers stand behind an enemy that is not hitting them
        if (!targetsMe && distance < 10.0f && InFrontArc(enemy, orientation, { bot->GetPositionX(), bot->GetPositionY() }, FrontHalfArc))
        {
            float reach = std::clamp((bot->GetCombatReach() + target->GetCombatReach()) * 0.6f, 1.5f, 4.0f);
            return done(StepTo(ai, BehindPoint(enemy, orientation, reach), z, MovePriority::Position));
        }
        return false;
    }

    if (ranged)
    {
        // ranged bots stacked on each other spread over an arc around the enemy
        bool stacked = false;
        for (GroupReference const& ref : group->GetMembers())
        {
            Player* member = ref.GetSource();
            if (member && member != bot && member->IsAlive() && member->IsInMap(bot) && bot->GetExactDist2d(member) < 3.0f)
            {
                stacked = true;
                break;
            }
        }

        float range = std::min(sPlayerbotsConfig.RangedDistance, 30.0f) * 0.9f;
        if (stacked && distance <= range + 5.0f)
        {
            int slot = 0, count = 0;
            for (RoleSlot const& s : roles.Slots)
            {
                if (s.Index == 0xFF || (s.Role != GroupRole::RangedDps && s.Role != GroupRole::Healer))
                    continue;
                if (s.Id == IdOf(bot))
                    slot = count;
                ++count;
            }
            float bearing = haveCenter ? std::atan2(center.GetPositionY() - enemy.Y, center.GetPositionX() - enemy.X)
                : std::atan2(bot->GetPositionY() - enemy.Y, bot->GetPositionX() - enemy.X);
            return done(StepTo(ai, SpreadPoint(enemy, bearing, slot, std::max(count, 2), 0.5f, range), z, MovePriority::Position));
        }
    }
    return false;
}

void Register(AiRegistry& r)
{
    r.AddValue<GroupRolesInfo>("group roles", 2000, [](PlayerbotAI& ai) { return ComputeRoles(ai); });
    r.AddValue<std::vector<ObjectGuid>>("attackers", 0, [](PlayerbotAI& ai) { return CollectAttackers(ai); });
    r.AddStrategy<ThreatStrategy>("threat");
    r.AddStrategy<WaitForAttackStrategy>("wait for attack");
}
}
}
