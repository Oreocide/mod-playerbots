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

#include "PartyOrders.h"
#include "AiRegistry.h"
#include "CellImpl.h"
#include "Cmd/ChatCommands.h"
#include "Cmd/ChatParser.h"
#include "Cmd/CommandText.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ReputationMgr.h"
#include "StringFormat.h"
#include "Strategies/GenericStrategies.h"
#include "Travel/MoveToward.h"
#include "Travel/Taxi.h"
#include "Util.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>

namespace Playerbots::PartyOrders
{
namespace
{
constexpr float OrderPriority = 25.0f;      // above follow and stay (10), below looting (50) and every fight action
constexpr uint32 TaxiWaitMs = 120000;       // "taxi": how long a bot waits for its master to take off
constexpr float PositionReach = 2.0f;

enum class Formation : uint8
{
    Near,       // the default spread behind the master
    Melee,      // close behind
    Queue,      // single file
    Line,       // side by side behind
    Arrow,      // a wide V
    Circle,     // around the master
    Chaos,      // anywhere near
    Far         // the default spread, farther back
};

struct NamedSpot
{
    uint32 MapId = 0;
    Position Pos;
};

struct Settings
{
    Formation Form = Formation::Near;
    float Range = 0.0f;                     // 0 = Playerbots.RangedDistance
    std::vector<ObjectGuid> FocusHeal;      // empty = everyone
    uint8 SaveMana = 0;                     // 0..5
    uint32 Rti = 0;                         // icon bits, 0 = default
    std::map<std::string, NamedSpot> Spots;
    bool Holding = false;                   // walking to, or holding, Hold
    NamedSpot Hold;
    uint32 TaxiUntil = 0;                   // "taxi" given while the master was still on the ground
};

/// The bot's orders. The hooks read them on hot paths (every heal evaluation, per party member), so they are read by
/// reference, and not at all while nobody on the server ever gave an order (sAnyOrder)
class SettingsValue final : public Value<Settings>
{
public:
    Settings Get(PlayerbotAI& /*ai*/) override { return _value; }
    Settings const& Ref() const { return _value; }
    void Set(Settings value) { _value = std::move(value); }

private:
    Settings _value;
};

std::atomic<bool> sAnyOrder{ false };
Settings const sNoOrders;

SettingsValue* FindSettings(PlayerbotAI& ai)
{
    return dynamic_cast<SettingsValue*>(ai.GetContext().GetValueBase("party orders"));
}

/// For the hooks: the defaults without a lookup until some order was given
Settings const& Current(PlayerbotAI& ai)
{
    if (!sAnyOrder.load(std::memory_order_relaxed))
        return sNoOrders;
    SettingsValue const* value = FindSettings(ai);
    return value ? value->Ref() : sNoOrders;
}

Settings GetSettings(PlayerbotAI& ai)
{
    SettingsValue const* value = FindSettings(ai);
    return value ? value->Ref() : Settings{};
}

void Store(PlayerbotAI& ai, Settings const& settings)
{
    if (SettingsValue* value = FindSettings(ai))
    {
        value->Set(settings);
        sAnyOrder.store(true, std::memory_order_relaxed);
    }
}

struct FormationName
{
    char const* Name;
    Formation Form;
};

constexpr FormationName FormationNames[] = {
    { "near", Formation::Near }, { "default", Formation::Near }, { "melee", Formation::Melee }, { "queue", Formation::Queue },
    { "line", Formation::Line }, { "arrow", Formation::Arrow }, { "circle", Formation::Circle }, { "chaos", Formation::Chaos },
    { "far", Formation::Far } };

char const* NameOf(Formation form)
{
    for (FormationName const& entry : FormationNames)
        if (entry.Form == form)
            return entry.Name;
    return "near";
}

uint32 GroupSize(Player* bot)
{
    Group* group = bot->GetGroup();
    return group ? std::max<uint32>(2, group->GetMembersCount()) : 2;
}

// ---- orders ----

bool FormationCommand(PlayerbotAI& ai, std::string const& param)
{
    Settings settings = GetSettings(ai);
    std::string word = ToLower(Trim(param));
    if (word.empty() || word == "?")
    {
        ai.TellMaster(std::string("Formation: ") + NameOf(settings.Form) + ". formation near|melee|queue|line|arrow|circle|chaos|far");
        return true;
    }
    for (FormationName const& entry : FormationNames)
        if (word == entry.Name)
        {
            settings.Form = entry.Form;
            Store(ai, settings);
            ai.ClearChaseTarget();     // the follow generator is issued again with the new place
            ai.TellMaster(std::string("Formation ") + entry.Name + ".");
            return true;
        }
    ai.TellMaster("formation near|melee|queue|line|arrow|circle|chaos|far");
    return true;
}

bool RangeCommand(PlayerbotAI& ai, std::string const& param)
{
    Settings settings = GetSettings(ai);
    std::string word = ToLower(Trim(param));
    if (word.empty() || word == "?")
    {
        ai.TellMaster(settings.Range > 0.0f ? Trinity::StringFormat("Ranged distance {:.0f} yards.", settings.Range)
            : std::string("Ranged distance: the default."));
        return true;
    }
    if (word == "reset" || word == "default")
        settings.Range = 0.0f;
    else
    {
        float range = float(atof(word.c_str()));
        if (range < 5.0f || range > 40.0f)
        {
            ai.TellMaster("range <5-40> or range reset");
            return true;
        }
        settings.Range = range;
    }
    Store(ai, settings);
    ai.ClearChaseTarget();
    ai.TellMaster(settings.Range > 0.0f ? Trinity::StringFormat("I keep {:.0f} yards.", settings.Range) : std::string("Default range."));
    return true;
}

constexpr char const* IconNames[] = { "star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull" };

bool RtiCommand(PlayerbotAI& ai, std::string const& param)
{
    Settings settings = GetSettings(ai);
    std::string text = ToLower(Trim(param));
    if (text.empty() || text == "?")
    {
        std::string names;
        for (uint8 i = 0; i < 8; ++i)
            if (settings.Rti & (1u << i))
                names += std::string(names.empty() ? "" : ", ") + IconNames[i];
        ai.TellMaster(names.empty() ? std::string("Icons: the default.") : "I attack " + names + ".");
        return true;
    }
    if (text == "reset" || text == "default")
        settings.Rti = 0;
    else
    {
        uint32 mask = 0;
        for (std::string const& word : SplitChain(text, " "))
            for (uint8 i = 0; i < 8; ++i)
                if (word == IconNames[i] || (word == "x" && i == 6))
                    mask |= 1u << i;
        if (!mask)
        {
            ai.TellMaster("rti skull|cross|square|moon|triangle|diamond|circle|star (several allowed), or rti reset");
            return true;
        }
        settings.Rti = mask;
    }
    Store(ai, settings);
    ai.TellMaster(settings.Rti ? "I attack what is marked so." : "Icons: the default.");
    return true;
}

bool FocusHealCommand(PlayerbotAI& ai, std::string const& param)
{
    Settings settings = GetSettings(ai);
    std::string lower = ToLower(Trim(param));
    if (lower.starts_with("heal"))   // the order is "focus heal <names>"
        lower = std::string(Trim(std::string_view(lower).substr(4)));
    Player* bot = ai.GetBot();

    if (lower.empty() || lower == "?")
    {
        if (settings.FocusHeal.empty())
            ai.TellMaster("I heal everyone.");
        else
        {
            std::string names;
            for (ObjectGuid guid : settings.FocusHeal)
                if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
                    names += std::string(names.empty() ? "" : ", ") + member->GetName();
            ai.TellMaster("I heal " + names + " (and myself).");
        }
        return true;
    }

    if (lower == "none" || lower == "reset" || lower == "all")
        settings.FocusHeal.clear();
    else
    {
        std::vector<ObjectGuid> focus;
        Group* group = bot->GetGroup();
        for (std::string const& name : SplitChain(lower, " "))
        {
            Player* found = nullptr;
            if (name == "me" || name == "master")
                found = ai.GetMaster();
            else if (group)
                for (GroupReference const& ref : group->GetMembers())
                    if (Player* member = ref.GetSource(); member && ToLower(member->GetName()) == name)
                        found = member;
            if (found)
                focus.push_back(found->GetGUID());
        }
        if (focus.empty())
        {
            ai.TellMaster("focus heal <member names | me> or focus heal none");
            return true;
        }
        settings.FocusHeal = std::move(focus);
    }
    Store(ai, settings);
    ai.TellMaster(settings.FocusHeal.empty() ? std::string("I heal everyone.") : "Focus set on " + std::to_string(settings.FocusHeal.size()) + ".");
    return true;
}

bool SaveManaCommand(PlayerbotAI& ai, std::string const& param)
{
    Settings settings = GetSettings(ai);
    std::string word = ToLower(Trim(param));
    if (word.starts_with("mana"))    // the order is "save mana <level>"
        word = std::string(Trim(std::string_view(word).substr(4)));
    if (word.empty() || word == "?")
    {
        ai.TellMaster(Trinity::StringFormat("Save mana level {} (0-5).", settings.SaveMana));
        return true;
    }
    int32 level = word == "off" ? 0 : atoi(word.c_str());
    if (level < 0 || level > 5 || (level == 0 && word != "0" && word != "off"))
    {
        ai.TellMaster("save mana <0-5>: higher saves more mana (heals later, less damage).");
        return true;
    }
    settings.SaveMana = uint8(level);
    Store(ai, settings);
    ai.TellMaster(Trinity::StringFormat("Save mana {}.", level));
    return true;
}

bool PositionCommand(PlayerbotAI& ai, std::string const& param)
{
    Settings settings = GetSettings(ai);
    std::vector<std::string> words = SplitChain(ToLower(Trim(param)), " ");
    Player* master = ai.GetMaster();

    if (words.empty() || words[0] == "?" || words[0] == "list")
    {
        std::string names;
        for (auto const& [name, spot] : settings.Spots)
            names += std::string(names.empty() ? "" : ", ") + name;
        ai.TellMaster(names.empty() ? std::string("No spots. position <name> set: your spot, position <name>: go there, position clear.")
            : "Spots: " + names + ".");
        return true;
    }

    if (words[0] == "clear")
    {
        settings.Spots.clear();
        settings.Holding = false;
        Store(ai, settings);
        ai.TellMaster("Spots forgotten.");
        return true;
    }

    std::string name = words[0];
    if (words.size() > 1 && words[1] == "set")
    {
        if (!master)
            return false;
        settings.Spots[name] = { master->GetMapId(), master->GetPosition() };
        Store(ai, settings);
        ai.TellMaster("Spot " + name + " is where you stand.");
        return true;
    }
    if (words.size() > 1 && (words[1] == "clear" || words[1] == "remove"))
    {
        settings.Spots.erase(name);
        Store(ai, settings);
        ai.TellMaster("Spot " + name + " forgotten.");
        return true;
    }

    auto itr = settings.Spots.find(name);
    if (itr == settings.Spots.end())
    {
        ai.TellMaster("No spot " + name + ". position " + name + " set");
        return true;
    }
    if (itr->second.MapId != ai.GetBot()->GetMapId())
    {
        ai.TellMaster("That spot is on another map.");
        return true;
    }

    settings.Holding = true;
    settings.Hold = itr->second;
    Store(ai, settings);
    ai.GetRoute().Cancel();
    ai.GetEngine().RemoveStrategy("follow");
    ai.GetEngine().AddStrategy(CreateStrategy("stay"));
    ai.ClearChaseTarget();
    ai.TellMaster("Going to " + name + ".");
    return true;
}

bool TaxiCommand(PlayerbotAI& ai, std::string const&)
{
    Settings settings = GetSettings(ai);
    settings.TaxiUntil = GameTime::GetGameTimeMS() + TaxiWaitMs;
    Store(ai, settings);
    ai.TellMaster("I take the flight you take.");
    return true;
}

bool LosCommand(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    std::string what = ToLower(Trim(param));
    float range = 40.0f;

    std::vector<Creature*> creatures;
    auto creatureCheck = [&](Creature const* creature) { return creature->IsAlive() && bot->IsWithinDist(creature, range) && bot->IsWithinLOSInMap(creature); };
    Trinity::CreatureListSearcher creatureSearcher(bot, creatures, creatureCheck);
    Cell::VisitGridObjects(bot, creatureSearcher, range);

    std::vector<GameObject*> objects;
    auto goCheck = [&](GameObject const* go) { return go->isSpawned() && bot->IsWithinDist(go, range) && bot->IsWithinLOSInMap(go); };
    Trinity::GameObjectListSearcher goSearcher(bot, objects, goCheck);
    Cell::VisitGridObjects(bot, goSearcher, range);

    auto byDistance = [bot](WorldObject const* a, WorldObject const* b) { return bot->GetExactDistSq(a) < bot->GetExactDistSq(b); };
    std::ranges::sort(creatures, byDistance);
    std::ranges::sort(objects, byDistance);

    auto list = [](auto const& items, size_t max)
    {
        std::string out;
        for (size_t i = 0; i < items.size() && i < max; ++i)
            out += std::string(out.empty() ? "" : ", ") + items[i]->GetName();
        if (items.size() > max)
            out += Trinity::StringFormat(" and {} more", items.size() - max);
        return out.empty() ? std::string("none") : out;
    };

    std::vector<Creature*> enemies, friends;
    for (Creature* creature : creatures)
        (bot->IsValidAttackTarget(creature) ? enemies : friends).push_back(creature);

    if (what.empty() || what == "enemies" || what == "targets")
        ai.TellMaster("Enemies: " + list(enemies, 8) + ".");
    if (what.empty() || what == "npcs" || what == "friends")
        ai.TellMaster("NPCs: " + list(friends, 8) + ".");
    if (what.empty() || what == "objects" || what == "gos")
        ai.TellMaster("Objects: " + list(objects, 8) + ".");
    return true;
}

char const* RankName(ReputationRank rank)
{
    switch (rank)
    {
        case REP_HATED: return "Hated";
        case REP_HOSTILE: return "Hostile";
        case REP_UNFRIENDLY: return "Unfriendly";
        case REP_NEUTRAL: return "Neutral";
        case REP_FRIENDLY: return "Friendly";
        case REP_HONORED: return "Honored";
        case REP_REVERED: return "Revered";
        case REP_EXALTED: return "Exalted";
        default: return "?";
    }
}

bool ReputationCommand(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    std::string filter = ToLower(Trim(param));
    ReputationMgr const& reps = bot->GetReputationMgr();

    struct Line
    {
        std::string Name;
        int32 Standing;
        ReputationRank Rank;
    };
    std::vector<Line> lines;
    for (auto const& [listId, state] : reps.GetStateList())
    {
        FactionEntry const* faction = sFactionStore.LookupEntry(state.ID);
        if (!faction || state.Standing == 0)
            continue;
        std::string name = faction->Name[LOCALE_enUS];
        if (!filter.empty() && ToLower(name).find(filter) == std::string::npos)
            continue;
        lines.push_back({ name, reps.GetReputation(faction), reps.GetRank(faction) });
    }
    std::ranges::sort(lines, [](Line const& a, Line const& b) { return a.Standing > b.Standing; });

    if (lines.empty())
    {
        ai.TellMaster(filter.empty() ? std::string("No reputation yet.") : "No reputation with " + filter + ".");
        return true;
    }
    std::string out;
    for (size_t i = 0; i < lines.size() && i < 8; ++i)
        out += Trinity::StringFormat("{}{}: {} ({})", out.empty() ? "" : "; ", lines[i].Name, RankName(lines[i].Rank), lines[i].Standing);
    if (lines.size() > 8)
        out += Trinity::StringFormat("; {} more", lines.size() - 8);
    ai.TellMaster(out + ".");
    return true;
}

// ---- the actions that carry out position and taxi ----

bool OrderPendingTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    if (!sAnyOrder.load(std::memory_order_relaxed) || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported())
        return false;
    Settings const& settings = Current(ai);
    if (settings.TaxiUntil && int32(settings.TaxiUntil - GameTime::GetGameTimeMS()) > 0)
        return true;
    if (!settings.Holding || !ai.GetEngine().HasStrategy("stay") || settings.Hold.MapId != bot->GetMapId())
        return false;
    return bot->GetExactDist(settings.Hold.Pos) > PositionReach + 0.5f;
}

class CarryOutOrderAction final : public Action
{
public:
    char const* GetName() const override { return "carry out order"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Settings settings = GetSettings(ai);
        uint32 now = GameTime::GetGameTimeMS();

        if (settings.TaxiUntil)
        {
            Player* master = ai.GetMaster();
            if (int32(settings.TaxiUntil - now) <= 0 || !master)
            {
                settings.TaxiUntil = 0;
                Store(ai, settings);
            }
            // only on the bot's own map: this runs in the bot's map thread, and with MapUpdate.Threads > 1 another map's thread may
            // clear the master's taxi path while it is read
            else if (bot->IsInMap(master) && master->IsInFlight() && !bot->IsInFlight())
            {
                std::deque<uint32> const& path = master->m_taxi.GetPath();
                if (path.size() >= 2)
                {
                    TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(path.front());
                    if (from && from->ContinentID == bot->GetMapId() && bot->GetExactDist2d(from->Pos.X, from->Pos.Y) <= 60.0f
                        && TakeTaxi(bot, path.front(), path.back()))
                    {
                        settings.TaxiUntil = 0;
                        Store(ai, settings);
                        return true;
                    }
                }
            }
        }

        if (settings.Holding && settings.Hold.MapId == bot->GetMapId() && ai.GetEngine().HasStrategy("stay"))
        {
            // a long way off (the master moved on, a fight dragged the bot away): walk back along the mesh
            if (bot->GetExactDist(settings.Hold.Pos) > PositionReach + 0.5f)
            {
                MoveResult result = MoveAlongMesh(bot, settings.Hold.Pos, PositionReach);
                if (result == MoveResult::NoPath)
                {
                    settings.Holding = false;
                    Store(ai, settings);
                    ai.TellMaster("I can't get to that spot.");
                    return false;
                }
                return true;
            }
        }
        else if (settings.Holding && !ai.GetEngine().HasStrategy("stay"))
        {
            // "follow" (or anything that dropped "stay") ends the hold
            settings.Holding = false;
            Store(ai, settings);
        }
        return false;
    }
};

class PartyOrderStrategy final : public Strategy
{
public:
    PartyOrderStrategy() : Strategy("party orders") { }

    uint8 GetStates() const override { return StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "order pending", { { "carry out order", OrderPriority } }, StrategyStates::NonCombat });
    }
};

class OrderAction final : public Action
{
public:
    using Fn = bool (*)(PlayerbotAI& ai, std::string const& param);
    OrderAction(std::string name, Fn fn) : _name(std::move(name)), _fn(fn) { }
    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI& ai) override { return _fn(ai, GetParam()); }

private:
    std::string _name;
    Fn _fn;
};

void AddOrder(AiRegistry& registry, char const* name, char const* help, OrderAction::Fn fn, bool whisperOnly)
{
    std::string action = std::string("cmd ") + name;
    registry.AddAction(action, [action, fn](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<OrderAction>(action, fn); });
    ChatCommandInfo info;
    info.Action = action;
    info.Help = help;
    info.Persist = false;
    info.WhisperOnly = whisperOnly;     // with an empty Command.Prefix a party chat line that starts with the word would run it
    GetChatCommands().Add(name, info);
}

uint32 Hash(ObjectGuid guid, uint8 salt)
{
    uint64 x = guid.GetCounter() * 0x9E3779B97F4A7C15ull + salt;
    x ^= x >> 29;
    return uint32(x * 0xBF58476D1CE4E5B9ull >> 32);
}
}

bool FormationOffset(PlayerbotAI& ai, uint8 index, float baseDistance, float& distance, float& angle)
{
    Settings const& settings = Current(ai);
    float const step = float(M_PI) / 6.0f;
    float side = index % 2 ? 1.0f : -1.0f;
    switch (settings.Form)
    {
        case Formation::Near:
            return false;
        case Formation::Melee:
            distance = std::min(baseDistance, 1.0f);
            angle = float(M_PI) + side * step * float(1 + index / 2);
            return true;
        case Formation::Queue:
            distance = baseDistance + 1.5f * float(index);
            angle = float(M_PI);
            return true;
        case Formation::Line:
        {
            float lateral = side * 1.5f * float(1 + index / 2);
            float back = std::max(baseDistance, 2.0f);
            distance = std::hypot(back, lateral);
            angle = float(M_PI) + std::atan2(lateral, back);
            return true;
        }
        case Formation::Arrow:
            distance = baseDistance + 1.2f * float(index / 2);
            angle = float(M_PI) + side * (float(M_PI) / 8.0f) * float(1 + index / 2);
            return true;
        case Formation::Circle:
        {
            uint32 size = GroupSize(ai.GetBot());
            distance = std::max(baseDistance, 3.0f);
            angle = 2.0f * float(M_PI) * float(index % size) / float(size);
            return true;
        }
        case Formation::Chaos:
        {
            uint32 h = Hash(ai.GetBot()->GetGUID(), index);
            distance = 2.0f + float(h % 400) / 100.0f;
            angle = float(h % 6283) / 1000.0f;
            return true;
        }
        case Formation::Far:
            distance = baseDistance + 6.0f;
            angle = float(M_PI) + side * step * float(1 + index / 2);
            return true;
    }
    return false;
}

float RangedDistance(PlayerbotAI& ai, float fallback)
{
    float range = Current(ai).Range;
    return range > 0.0f ? range : fallback;
}

bool MayHeal(PlayerbotAI& ai, Unit const* unit)
{
    if (!unit || unit == ai.GetBot())
        return true;
    Settings const& settings = Current(ai);
    return settings.FocusHeal.empty() || std::ranges::find(settings.FocusHeal, unit->GetGUID()) != settings.FocusHeal.end();
}

float HealThreshold(PlayerbotAI& ai, float configured)
{
    // AC's save mana levels heal later at each step
    return std::max(25.0f, configured - 8.0f * float(Current(ai).SaveMana));
}

float DamageMinMana(PlayerbotAI& ai, float configured)
{
    return std::min(95.0f, configured + 7.0f * float(Current(ai).SaveMana));
}

uint32 RtiMask(PlayerbotAI& ai)
{
    return Current(ai).Rti;
}

void Register(AiRegistry& registry)
{
    registry.AddValue("party orders", [](std::string const&) -> std::unique_ptr<ValueBase> { return std::make_unique<SettingsValue>(); });
    registry.AddTrigger("order pending", 1000, OrderPendingTrigger);
    registry.AddAction<CarryOutOrderAction>("carry out order");
    registry.AddStrategy<PartyOrderStrategy>("party orders");

    AddOrder(registry, "formation", "formation near|melee|queue|line|arrow|circle|chaos|far: where I follow.", FormationCommand, false);
    AddOrder(registry, "range", "range <5-40>|reset: how far a ranged bot stands from its target.", RangeCommand, false);
    AddOrder(registry, "rti", "rti <icons>|reset: attack what carries these raid icons (skull, cross, ...).", RtiCommand, false);
    AddOrder(registry, "focus", "focus heal <names|me>|none: a healer heals only them (and itself).", FocusHealCommand, true);
    AddOrder(registry, "save", "save mana <0-5>: a healer heals later and deals less damage.", SaveManaCommand, true);
    AddOrder(registry, "position", "position <name> set|clear, position <name> (go and hold), position list|clear.", PositionCommand, true);
    AddOrder(registry, "taxi", "taxi: take the flight you take next (two minutes).", TaxiCommand, true);
    AddOrder(registry, "los", "los [enemies|npcs|objects]: what I see around me.", LosCommand, true);
    AddOrder(registry, "reputation", "reputation [faction]: my standings.", ReputationCommand, true);
    GetChatCommands().Alias("rep", "reputation");
}
}
