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

#include "Dungeon/EncounterKit.h"
#include "Creature.h"
#include "Engine/AiRegistry.h"
#include "GameObject.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsConfig.h"
#include "Travel/BotMover.h"

// The Nexus (map 576), after AzerothCore's Ai/Dungeon/Nex with TrinityCore's entries and spell ids:
//  * the faction commander (normal: the Alliance or Horde Commander, heroic: Commander Kolurg or Stoutbeard): his Whirlwind is stepped away from.
//  * Grand Magus Telestra: in the first phase ranged bots and healers do not stand within 6 yards of each other (Firebomb); when she splits,
//    the three magi die in the order arcane, frost, fire (everybody but the healers).
//  * Anomalus: while his Rift Shield is up the Chaotic Rift is the target (everybody but the healers).
//  * Ormorok the Tree-Shaper: everybody but the tank stacks on him; the tank goes in when Crystal Spikes are out.
namespace Playerbots::Dungeon
{
namespace
{
constexpr uint32 MapNexus = 576;
constexpr uint32 NpcAllianceCommander = 27949;
constexpr uint32 NpcHordeCommander = 27947;
constexpr uint32 NpcKolurg = 26798;
constexpr uint32 NpcStoutbeard = 26796;
constexpr uint32 NpcTelestra = 26731;
constexpr uint32 NpcFireMagus = 26928;
constexpr uint32 NpcFrostMagus = 26930;
constexpr uint32 NpcArcaneMagus = 26929;
constexpr uint32 NpcAnomalus = 26763;
constexpr uint32 NpcChaoticRift = 26918;
constexpr uint32 NpcOrmorok = 26794;
constexpr uint32 GoCrystalSpike = 188537;
constexpr uint32 SpellWhirlwind = 38618;
constexpr uint32 SpellRiftShield = 47748;
constexpr float WhirlwindDistance = 10.0f;
constexpr float FirebombSpread = 6.0f;
constexpr float DodgeRelevance = 82.0f;

Creature* Commander(Player* bot)
{
    return FindBoss(bot, { NpcAllianceCommander, NpcHordeCommander, NpcKolurg, NpcStoutbeard }, 40.0f);
}

Unit* NexusTarget(PlayerbotAI& ai)
{
    if (ai.GetRole() == BotRole::Healer)
        return nullptr;

    Player* bot = ai.GetBot();
    // Telestra split: arcane, frost, fire
    if (Unit* magus = FirstAttacker(ai, { NpcArcaneMagus, NpcFrostMagus, NpcFireMagus }))
        return magus;

    if (Creature* anomalus = FindBoss(bot, { NpcAnomalus }, 80.0f); anomalus && anomalus->HasAura(SpellRiftShield))
        if (Creature* rift = bot->FindNearestCreature(NpcChaoticRift, 80.0f, true); rift && ai.IsValidEnemy(rift))
            return rift;

    return nullptr;
}

bool WhirlwindTrigger(PlayerbotAI& ai)
{
    Creature* boss = Commander(ai.GetBot());
    return boss && IsCasting(boss, { SpellWhirlwind }) && ai.GetBot()->GetExactDist2d(boss) < WhirlwindDistance;
}

bool FirebombTrigger(PlayerbotAI& ai)
{
    BotRole role = ai.GetRole();
    if (role != BotRole::RangedDps && role != BotRole::Healer)
        return false;
    return FindBoss(ai.GetBot(), { NpcTelestra }, 80.0f) != nullptr;
}

bool OrmorokTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Creature* boss = FindBoss(bot, { NpcOrmorok }, 80.0f);
    if (!boss || bot->GetExactDist2d(boss) < 2.0f)
        return false;

    // the tank only moves in when spikes are out; the others always stack
    if (ai.GetRole() == BotRole::Tank)
        return bot->FindNearestGameObject(GoCrystalSpike, 40.0f) != nullptr;
    return true;
}
}

void RegisterNexus(AiRegistry& r)
{
    r.AddTrigger("nex whirlwind", 0, WhirlwindTrigger);
    r.AddTrigger("nex firebomb", 0, FirebombTrigger);
    r.AddTrigger("nex ormorok", 0, OrmorokTrigger);

    AddAction(r, "nex leave whirlwind", [](PlayerbotAI& ai)
    {
        Creature* boss = Commander(ai.GetBot());
        return boss && StepAway(ai, boss->GetPositionX(), boss->GetPositionY(), WhirlwindDistance);
    });
    AddAction(r, "nex spread firebomb", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        if (Group* group = bot->GetGroup())
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource(); member && member != bot && member->IsAlive() && member->IsInMap(bot) && bot->GetExactDist2d(member) < FirebombSpread)
                    return StepAway(ai, member->GetPositionX(), member->GetPositionY(), FirebombSpread + 1.0f);
        return false;
    });
    AddAction(r, "nex stack ormorok", [](PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        Creature* boss = FindBoss(bot, { NpcOrmorok }, 80.0f);
        return boss && MoveCombat(bot, boss->GetPosition(), 1.5f, MovePriority::Avoid) != MoveResult::NoPath;
    });

    DungeonDef def;
    def.MapId = MapNexus;
    def.Name = "nexus";
    def.Target = NexusTarget;
    def.Triggers = {
        { "nex whirlwind", { { "nex leave whirlwind", DodgeRelevance } }, StrategyStates::Combat },
        { "nex firebomb", { { "nex spread firebomb", DodgeRelevance - 1.0f } }, StrategyStates::Combat },
        { "nex ormorok", { { "nex stack ormorok", DodgeRelevance - 1.0f } }, StrategyStates::Combat },
    };
    RegisterDungeon(r, std::move(def));
}
}
