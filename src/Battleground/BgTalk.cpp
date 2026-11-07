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

#include "Battleground/BgTalk.h"
#include "Battleground/BgConfig.h"
#include "BotSession.h"
#include "ChatPackets.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Util.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace Playerbots::Bg
{
namespace
{
constexpr uint32 BotCallMs = 20000;         // one line per bot per 20 s
constexpr uint32 TeamCallMs = 10000;        // one line per team per 10 s
constexpr uint32 SameCallMs = 30000;        // the same call not again within 30 s
constexpr uint32 IncomingHoldMs = 45000;    // "inc lm 3": the threat holds this long
constexpr uint32 OrderHoldMs = 90000;       // "def st", "go farm", "everyone to drek": the order holds this long
constexpr float AnswerRange = 80.0f;        // a bot this close to the caller answers "omw"

/// The usual short words of the code-table maps' objectives (the first is what a bot calls)
char const* CodeAliases(std::string_view name)
{
    static std::pair<char const*, char const*> const table[] =
    {
        { "Stables", "st,stables,stable" },
        { "Blacksmith", "bs,blacksmith,smith" },
        { "Farm", "farm" },
        { "Gold Mine", "gm,gold mine,mine" },
        { "Lumber Mill", "lm,lumber mill,lumber,mill" },
        { "Lighthouse", "lh,lighthouse" },
        { "Waterworks", "ww,waterworks,water" },
        { "Mines", "mines,mine" },
        { "Blood Elf Tower", "bet,be,blood elf,blood elf tower" },
        { "Fel Reaver Ruins", "frr,fr,fel reaver,fel reaver ruins" },
        { "Mage Tower", "mt,mage tower,mage" },
        { "Draenei Ruins", "dr,draenei ruins,draenei" },
    };
    for (auto const& [node, aliases] : table)
        if (name == node)
            return aliases;
    return "";
}

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::string_view FirstAlias(std::string_view list)
{
    for (std::string_view token : Trinity::Tokenize(list, ',', false))
    {
        while (!token.empty() && token.front() == ' ')
            token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ')
            token.remove_suffix(1);
        if (!token.empty())
            return token;
    }
    return {};
}
}

void Say(BgPlan& plan, uint32 team, Player* speaker, std::string const& line, std::string const& key, uint32 now)
{
    if (!sBgConfig.CallOuts || !speaker || !speaker->IsInWorld() || !speaker->IsAlive() || team > 1 || line.empty())
        return;
    // a team of bots says nothing: nobody would read it
    if (std::none_of(plan.Picture.begin(), plan.Picture.end(), [team](Seen const& seen) { return !seen.Bot && uint32(seen.Team) == team; }))
        return;
    TeamPlan& teamPlan = plan.Teams[team];
    if (int32(teamPlan.TalkNextMs - now) > 0)
        return;
    if (auto said = teamPlan.TalkSaid.find(key); said != teamPlan.TalkSaid.end() && now - said->second < SameCallMs)
        return;
    static std::unordered_map<ObjectGuid, uint32> botNext;     // world thread only
    if (botNext.size() > 4096)
        botNext.clear();
    if (auto next = botNext.find(speaker->GetGUID()); next != botNext.end() && int32(next->second - now) > 0)
        return;
    Group* group = speaker->GetGroup();
    if (!group || !group->isBGGroup())
        return;

    teamPlan.TalkNextMs = now + TeamCallMs;
    teamPlan.TalkSaid[key] = now;
    if (teamPlan.TalkSaid.size() > 64)
        std::erase_if(teamPlan.TalkSaid, [now](auto const& entry) { return now - entry.second >= SameCallMs; });
    botNext[speaker->GetGUID()] = now + BotCallMs;
    ++teamPlan.Lines;

    WorldPackets::Chat::Chat packet;
    packet.Initialize(CHAT_MSG_INSTANCE_CHAT, LANG_UNIVERSAL, speaker, nullptr, line);
    group->BroadcastPacket(packet.Write(), false);
}

std::string CallName(BgPlan const& plan, int8 objective)
{
    if (NodeDef const* row = RowOf(plan, objective))
        if (std::string_view first = FirstAlias(row->Aliases); !first.empty())
            return std::string(first);
    if (objective < 0 || size_t(objective) >= plan.Objectives.size())
        return "";
    std::string const& name = plan.Objectives[objective].Name;
    if (std::string_view first = FirstAlias(CodeAliases(name)); !first.empty())
        return std::string(first);
    return Lower(name);
}

std::vector<BgRules::Alias> AliasesOf(BgPlan const& plan, uint32 team)
{
    std::vector<BgRules::Alias> out;
    auto add = [&out](std::string_view list, int node)
    {
        for (std::string_view token : Trinity::Tokenize(list, ',', false))
            if (std::string word = Lower(token); !BgRules::CallWords(word).empty())
                out.push_back({ word, node });
    };
    if (!plan.Def)
        return out;

    if (plan.Def->Kind == MapKind::Flag)
    {
        add("base,fr,flag room,home,our base", int(team));
        add("their base,enemy base,efr,their fr", int(1 - team));
        return out;
    }

    // the enemy's bosses first: "everyone to general" means theirs
    std::vector<size_t> order;
    bool valley = plan.Def->Kind == MapKind::Valley;
    if (valley)
        for (size_t i = 0; i < plan.Def->Nodes.size() && i < plan.Objectives.size(); ++i)
            if (plan.Def->Nodes[i].IsBoss() && BgRules::TeamOfSide(plan.Def->Nodes[i].Side) != team)
                order.push_back(i);
    for (size_t i = 0; i < plan.Objectives.size(); ++i)
        if (std::find(order.begin(), order.end(), i) == order.end())
            order.push_back(i);

    for (size_t i : order)
    {
        if (valley)
        {
            if (i >= plan.Def->Nodes.size())
                continue;
            NodeDef const& row = plan.Def->Nodes[i];
            if (!row.IsNode() && !row.IsBoss())
                continue;
            add(row.Aliases, int(i));
            add(row.Name, int(i));
        }
        else
        {
            add(CodeAliases(plan.Objectives[i].Name), int(i));
            add(plan.Objectives[i].Name, int(i));
        }
    }
    return out;
}

bool HearCall(Player* player, uint32 type, std::string const& msg)
{
    if (!sBgConfig.HumanCalls || !player || IsBot(player))
        return false;
    if (type != CHAT_MSG_INSTANCE_CHAT && type != CHAT_MSG_INSTANCE_CHAT_LEADER)
        return false;
    if (!player->InBattleground() || !sBgConfig.Plans(player->GetMapId()))
        return false;
    BgPlan* plan = sBgPlanner.FindForEdit(player->GetMapId(), player->GetInstanceId());
    TeamId team = TeamOf(player);
    if (!plan || !plan->Def || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
        return false;

    BgRules::Call call = BgRules::ParseCall(msg, AliasesOf(*plan, team));
    TC_LOG_INFO("server.playerbots", "Playerbots: {} called \"{}\" in {} (instance {}): {}{}", player->GetName(), msg, plan->Def->Name,
        plan->InstanceId, BgRules::CallKindName(call.Kind), call.Node >= 0 ? " " + ObjectiveName(*plan, int8(call.Node)) : std::string());
    if (call.Kind == BgRules::CallKind::None || call.Node < 0)
        return false;

    uint32 now = GameTime::GetGameTimeMS();
    TeamPlan& teamPlan = plan->Teams[team];
    bool incoming = call.Kind == BgRules::CallKind::Incoming || call.Kind == BgRules::CallKind::Help;
    // a new call on the same objective replaces the old one of its kind
    std::erase_if(teamPlan.Calls, [&](CallOrder const& order)
    {
        return int32(order.UntilMs - now) <= 0 || (order.Objective == call.Node && order.Kind == call.Kind);
    });
    CallOrder order;
    order.Kind = call.Kind;
    order.Objective = int8(call.Node);
    order.Count = call.Count;
    order.UntilMs = now + (incoming ? IncomingHoldMs : OrderHoldMs);
    order.From = player->GetGUID();
    teamPlan.Calls.push_back(order);
    ++teamPlan.CallsHeard;

    // a bot near the caller answers, as a player would (#9)
    if (sBgConfig.CallOuts)
    {
        Player* best = nullptr;
        float bestDist = AnswerRange;
        for (Seen const& seen : plan->Picture)
        {
            if (!seen.Bot || !seen.Alive || seen.Team != team)
                continue;
            float dist = player->GetExactDist2d(seen.Pos);
            if (dist >= bestDist)
                continue;
            Player* bot = ObjectAccessor::GetPlayer(*player, seen.Guid);
            PlayerbotAI* ai = bot ? sPlayerbotMgr->GetAI(bot) : nullptr;
            if (!ai || ai->GetMaster())
                continue;
            best = bot;
            bestDist = dist;
        }
        if (best)
            Say(*plan, team, best, incoming ? "omw" : call.Kind == BgRules::CallKind::Defend ? "on it" : "ok", "omw:" + CallName(*plan, int8(call.Node)), now);
    }
    return true;
}
}
