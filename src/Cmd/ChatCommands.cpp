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

#include "ChatCommands.h"
#include "BotCommands.h"
#include "CommandText.h"
#include "ItemCommands.h"
#include "Social/WhisperFallback.h"
#include "Db/PlayerbotRepository.h"
#include "AiRegistry.h"
#include "GearFactory.h"
#include "GenericStrategies.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "DB2Stores.h"
#include "Group.h"
#include "World/WorldActionQueue.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Travel/TravelMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include <algorithm>

namespace Playerbots
{
namespace
{
// ---- who answers ----

char const* GetClassName(Player const* bot)
{
    switch (bot->GetClass())
    {
        case CLASS_WARRIOR: return "warrior";
        case CLASS_PALADIN: return "paladin";
        case CLASS_HUNTER: return "hunter";
        case CLASS_ROGUE: return "rogue";
        case CLASS_PRIEST: return "priest";
        case CLASS_DEATH_KNIGHT: return "deathknight";
        case CLASS_SHAMAN: return "shaman";
        case CLASS_MAGE: return "mage";
        case CLASS_WARLOCK: return "warlock";
        case CLASS_MONK: return "monk";
        case CLASS_DRUID: return "druid";
        case CLASS_DEMON_HUNTER: return "demonhunter";
        case CLASS_EVOKER: return "evoker";
        default: return "";
    }
}

char const* GetFilterRole(BotRole role)
{
    switch (role)
    {
        case BotRole::Tank: return "tank";
        case BotRole::Healer: return "healer";
        case BotRole::MeleeDps: return "melee";
        case BotRole::RangedDps: return "ranged";
    }
    return "melee";
}

BotTraits GetTraits(PlayerbotAI& ai, Player* speaker)
{
    Player* bot = ai.GetBot();
    BotTraits traits;
    traits.Class = GetClassName(bot);
    traits.Role = GetFilterRole(ai.GetRole());
    traits.Group = bot->GetGroup() ? uint8(bot->GetSubGroup() + 1) : 0;
    traits.Level = uint8(std::min<uint32>(bot->GetLevel(), 255));
    traits.Distance = speaker && speaker->IsInWorld() && bot->IsInMap(speaker) ? bot->GetExactDist(speaker) : 100000.0f;
    return traits;
}

// ---- replies ----

/// Chat lines are short: cut a long list at the commas
void TellLines(PlayerbotAI& ai, std::string const& head, std::vector<std::string> const& items)
{
    constexpr size_t MaxLine = 220;
    std::string line = head;
    for (size_t i = 0; i < items.size(); ++i)
    {
        std::string piece = items[i] + (i + 1 < items.size() ? ", " : "");
        if (line.size() + piece.size() > MaxLine && !line.empty())
        {
            ai.TellMaster(line);
            line.clear();
        }
        line += piece;
    }

    if (!line.empty())
        ai.TellMaster(line);
}

char const* SpellName(SpellInfo const* spell)
{
    return (*spell->SpellName)[sWorld->GetDefaultDbcLocale()];
}

// ---- the commands ----

using CommandFn = bool (*)(PlayerbotAI& ai, std::string const& param);

class CommandAction final : public Action
{
public:
    CommandAction(std::string name, CommandFn fn) : _name(std::move(name)), _fn(fn) { }

    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI& ai) override { return _fn(ai, GetParam()); }

private:
    std::string _name;
    CommandFn _fn;
};

bool Follow(PlayerbotAI& ai, std::string const&)
{
    ai.GetRoute().Cancel();
    ai.GetEngine().RemoveStrategy("stay");
    ai.GetEngine().AddStrategy(CreateStrategy("follow"));
    ai.ClearChaseTarget();
    ai.TellMaster("Following you.");
    return true;
}

bool Stay(PlayerbotAI& ai, std::string const&)
{
    ai.GetRoute().Cancel();
    ai.GetEngine().RemoveStrategy("follow");
    ai.GetEngine().AddStrategy(CreateStrategy("stay"));
    ai.ClearChaseTarget();
    ai.TellMaster("Staying here.");
    return true;
}

bool Passive(PlayerbotAI& ai, std::string const&)
{
    ai.GetEngine().RemoveStrategy("combat");
    ai.ClearForcedTarget();
    ai.GetBot()->AttackStop();
    ai.TellMaster("I will not fight.");
    return true;
}

bool Assist(PlayerbotAI& ai, std::string const&)
{
    ai.GetEngine().AddStrategy(CreateStrategy("combat"));
    ai.TellMaster("I will fight your enemies.");
    return true;
}

bool Attack(PlayerbotAI& ai, std::string const&)
{
    Player* master = ai.GetMaster();
    Unit* target = master ? master->GetSelectedUnit() : nullptr;
    if (!target)
    {
        ai.TellMaster("Select an enemy first.");
        return true;
    }

    // say what is wrong, a bare refusal leaves the player guessing
    Player* bot = ai.GetBot();
    if (!target->IsAlive())
        ai.TellMaster(Trinity::StringFormat("{} is already dead.", target->GetName()));
    else if (!target->IsInMap(bot) || !bot->IsWithinDistInMap(target, 60.0f))
        ai.TellMaster(Trinity::StringFormat("{} is too far away.", target->GetName()));
    else if (!bot->IsValidAttackTarget(target))
        ai.TellMaster(Trinity::StringFormat("I can't attack {}.", target->GetName()));
    else
    {
        ai.GetEngine().AddStrategy(CreateStrategy("combat"));
        ai.SetForcedTarget(target->GetGUID());
        ai.TellMaster(Trinity::StringFormat("Attacking {}.", target->GetName()));
    }
    return true;
}

// Release the spirit: the game puts the ghost at the graveyard (nobody runs). Outside the world queue the repop handler is not safe to call
bool Release(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    if (bot->IsAlive() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        ai.TellMaster(bot->IsAlive() ? "I am alive." : "I already released.");
        return true;
    }

    sWorldActionQueue->Enqueue(bot->GetGUID(), bot->GetMapId(), ObjectGuid::Empty, [](Player* player)
    {
        if (player->IsAlive() || player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            return;
        if (PlayerbotAI* target = sPlayerbotMgr->GetAI(player))
            target->NoteExpectedMove();
        player->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);
        player->BuildPlayerRepop();
        player->RepopAtGraveyard();
    });
    ai.TellMaster("Releasing my spirit.");
    return true;
}

// Back to life by the bot's own routine: a resurrection offer, the spirit healer or, as before, on the spot
bool Revive(PlayerbotAI& ai, std::string const&)
{
    if (ai.GetBot()->IsAlive())
    {
        ai.TellMaster("I am alive.");
        return true;
    }
    ai.GetEngine().ExecuteAction(ai, "revive");
    ai.TellMaster("Coming back.");
    return true;
}

bool Flee(PlayerbotAI& ai, std::string const&)
{
    ai.GetBot()->AttackStop();
    ai.ClearForcedTarget();
    ai.ClearChaseTarget();
    ai.GetRoute().Cancel();
    ai.GetEngine().RemoveStrategy("stay");
    ai.GetEngine().AddStrategy(CreateStrategy("follow"));
    ai.TellMaster("Running to you.");
    return true;
}

bool Who(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    ChrSpecializationEntry const* spec = bot->GetPrimarySpecializationEntry();
    ai.TellMaster(Trinity::StringFormat("Level {} {} {}, {}.", bot->GetLevel(), spec ? spec->Name[sWorld->GetDefaultDbcLocale()] : "", GetClassName(bot),
        GetRoleName(ai.GetRole())));
    return true;
}

bool Attackers(PlayerbotAI& ai, std::string const&)
{
    std::vector<ObjectGuid> guids = ai.GetContext().GetValue<std::vector<ObjectGuid>>("attackers", ai);
    std::vector<std::string> names;
    for (ObjectGuid const& guid : guids)
        if (Unit* enemy = ObjectAccessor::GetUnit(*ai.GetBot(), guid))
            names.push_back(Trinity::StringFormat("{} {:.0f}%", enemy->GetName(), enemy->GetHealthPct()));
    if (names.empty())
        ai.TellMaster("Nothing is attacking us.");
    else
        TellLines(ai, "Enemies: ", names);
    return true;
}

bool TargetInfo(PlayerbotAI& ai, std::string const&)
{
    if (Unit* target = ai.SelectCombatTarget())
        ai.TellMaster(Trinity::StringFormat("Fighting {} ({:.0f}% health, {:.0f} yards).", target->GetName(), target->GetHealthPct(), ai.GetBot()->GetDistance(target)));
    else
        ai.TellMaster("No target.");
    return true;
}

bool Drink(PlayerbotAI& ai, std::string const&)
{
    ActionResult result = ai.GetEngine().ExecuteAction(ai, "eat/drink");
    ai.TellMaster(result == ActionResult::Ok ? "Eating and drinking." : "I have nothing to eat or drink, or I do not need it.");
    return true;
}

bool Rebuff(PlayerbotAI& ai, std::string const&)
{
    ai.GetEngine().ExecuteAction(ai, "buff");
    ai.TellMaster("Buffing.");
    return true;
}

bool GiveLeader(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    Group* group = bot->GetGroup();
    Player* master = ai.GetMaster();
    if (!group || !master || group->GetLeaderGUID() != bot->GetGUID() || !group->IsMember(master->GetGUID()))
        ai.TellMaster("I am not leading the group.");
    else
    {
        group->ChangeLeader(master->GetGUID());
        ai.TellMaster("You lead now.");
    }
    return true;
}

bool Status(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    ai.TellMaster(Trinity::StringFormat("Level {} {}, {:.0f}% health, {:.0f}% {}. {} damage / {} heal spells. Strategies: {}",
        bot->GetLevel(), GetRoleName(ai.GetRole()), bot->GetHealthPct(), bot->GetPowerPct(bot->GetPowerType()),
        bot->GetPowerType() == POWER_MANA ? "mana" : "power", ai.GetSpellBook().GetDamageSpells().size(),
        ai.GetSpellBook().GetHealSpells().size(), ai.GetEngine().GetStrategyList()));
    return true;
}

bool Gear(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    std::vector<std::string> items;
    uint32 empty = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        // shirt and tabard are not gear
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;

        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back(item->GetTemplate()->GetName(sWorld->GetDefaultDbcLocale()));
        else
            ++empty;
    }

    TellLines(ai, Trinity::StringFormat("Item level {:.0f}, {} empty slots: ", bot->GetAverageItemLevel(), empty), items);
    return true;
}

bool AutoGear(PlayerbotAI& ai, std::string const& param)
{
    GearFactory::GearOptions options;
    switch (sGearFactory->ParseGearArg(param, options))
    {
        case GearFactory::GearArgKind::Bad:
            ai.TellMaster("autogear [white|green|blue|purple|<ilvl>|match|reset]");
            return true;
        case GearFactory::GearArgKind::Match:
            if (Player* master = ai.GetMaster())
                options.ItemLevelCap = uint32(master->GetAverageItemLevel());
            else
            {
                ai.TellMaster("match needs a master. A GM can use .bot autogear <name> match.");
                return true;
            }
            break;
        default:
            break;
    }

    uint32 changed = sGearFactory->Equip(ai.GetBot(), options);
    ai.TellMaster(Trinity::StringFormat("Changed {} slots. Average item level {:.0f}.", changed, ai.GetBot()->GetAverageItemLevel()));
    return true;
}

bool Inventory(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    uint32 stacks = 0;
    uint32 junk = 0;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
    {
        ++stacks;
        if (item->GetTemplate()->GetQuality() == ITEM_QUALITY_POOR)
            ++junk;
        return ItemSearchCallbackResult::Continue;
    });

    uint64 money = bot->GetMoney();
    ai.TellMaster(Trinity::StringFormat("{} item stacks ({} grey), {}g {}s {}c.", stacks, junk, money / 10000, (money / 100) % 100, money % 100));
    return true;
}

bool Spells(PlayerbotAI& ai, std::string const&)
{
    std::vector<std::string> names;
    for (SpellInfo const* spell : ai.GetSpellBook().GetDamageSpells())
        names.push_back(SpellName(spell));
    TellLines(ai, "Damage: ", names);

    names.clear();
    for (SpellInfo const* spell : ai.GetSpellBook().GetHealSpells())
        names.push_back(SpellName(spell));
    if (!names.empty())
        TellLines(ai, "Healing: ", names);
    return true;
}

bool Quests(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    std::vector<std::string> titles;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(slot)))
            titles.push_back(quest->GetLogTitle());

    if (titles.empty())
        ai.TellMaster("My quest log is empty.");
    else
        TellLines(ai, Trinity::StringFormat("{} quests: ", titles.size()), titles);
    return true;
}

bool Summon(PlayerbotAI& ai, std::string const&)
{
    if (ai.GetEngine().ExecuteAction(ai, "teleport to master") != ActionResult::Ok)
        return false;

    ai.TellMaster("Here I am.");
    return true;
}

bool Reset(PlayerbotAI& ai, std::string const&)
{
    ai.ResetStrategies();
    sPlayerbotRepository->ForgetMemory(ai.GetBot()->GetGUID());
    ai.TellMaster("Back to my defaults.");
    return true;
}

bool Strategies(PlayerbotAI& ai, std::string const& param, uint8 states, char const* label)
{
    std::string reply = param.empty() ? "strategies: " + ai.GetEngine().GetStrategyList(states)
        : ai.GetEngine().ApplyStrategyString(ToLower(param), [&ai](std::string_view name) { return ai.MakeStrategy(name); }, states);
    ai.TellMaster(Trinity::StringFormat("{} {}", label, reply));
    return true;
}

bool Combat(PlayerbotAI& ai, std::string const& param) { return Strategies(ai, param, StrategyStates::Combat, "Combat"); }
bool NonCombat(PlayerbotAI& ai, std::string const& param) { return Strategies(ai, param, StrategyStates::NonCombat, "Non combat"); }
bool Dead(PlayerbotAI& ai, std::string const& param) { return Strategies(ai, param, StrategyStates::Dead, "Dead"); }
bool AllStrategies(PlayerbotAI& ai, std::string const& param) { return Strategies(ai, param, StrategyStates::All, "All"); }

// cast <spell id or name>, on the master's target if there is one, else on the bot itself
bool Cast(PlayerbotAI& ai, std::string const& param)
{
    Player* bot = ai.GetBot();
    if (param.empty())
    {
        ai.TellMaster("Cast what? Give a spell name or id.");
        return true;
    }

    uint32 wantedId = 0;
    std::string wanted = param;
    strToLower(wanted);
    if (std::ranges::all_of(param, [](unsigned char c) { return std::isdigit(c); }))
        wantedId = uint32(std::strtoul(param.c_str(), nullptr, 10));

    SpellInfo const* found = nullptr;
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (spell.state == PLAYERSPELL_REMOVED || !spell.active || spell.disabled)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || info->IsPassive())
            continue;

        std::string name = SpellName(info);
        strToLower(name);
        if ((wantedId && spellId == wantedId) || (!wantedId && name == wanted))
        {
            found = info;
            break;
        }
    }

    if (!found)
    {
        ai.TellMaster(Trinity::StringFormat("I don't know {}.", param));
        return true;
    }

    Player* master = ai.GetMaster();
    Unit* target = master ? master->GetSelectedUnit() : nullptr;
    if (!target)
        target = bot;

    if (!BotSpellBook::CastFirstUsable(bot, target, { found }, 1))
        ai.TellMaster(Trinity::StringFormat("I can't cast {} right now.", SpellName(found)));
    return true;
}

// goto <flight master or town name or node id>: a route through the travel graph, by foot, flight path and portal
bool Goto(PlayerbotAI& ai, std::string const& param)
{
    if (!sTravelMgr->IsBuilt())
    {
        ai.TellMaster("I have no map of the world yet.");
        return true;
    }

    if (param.empty())
    {
        ai.TellMaster("Where to? goto <name or node id>, find nodes with .bot travel nodes <text>.");
        return true;
    }

    uint32 nodeId = UINT32_MAX;
    if (std::ranges::all_of(param, [](unsigned char c) { return std::isdigit(c); }))
        nodeId = uint32(std::strtoul(param.c_str(), nullptr, 10));
    else if (std::vector<uint32> found = sTravelMgr->FindNodes(param, 1); !found.empty())
        nodeId = found.front();

    if (nodeId >= sTravelMgr->GetGraph().NodeCount())
    {
        ai.TellMaster(Trinity::StringFormat("I don't know a place called {}.", param));
        return true;
    }

    TravelNode const& node = sTravelMgr->GetGraph().GetNode(nodeId);
    if (!ai.GetRoute().Begin(ai, { node.Map, node.X, node.Y, node.Z }))
    {
        ai.TellMaster(Trinity::StringFormat("I can't find a way to {}.", node.Name));
        return true;
    }

    ai.TellMaster(Trinity::StringFormat("Going to {}, {}.", node.Name, ai.GetRoute().Summary()));
    return true;
}

bool Help(PlayerbotAI& ai, std::string const& param)
{
    ChatCommandTable const& table = GetChatCommands();
    if (!param.empty())
    {
        ChatCommandInfo const* info = table.Find(param);
        ai.TellMaster(info ? Trinity::StringFormat("{}: {}", param, info->Help) : Trinity::StringFormat("No command \"{}\".", param));
        return true;
    }

    TellLines(ai, "Commands: ", table.GetNames());
    ai.TellMaster("Start with @filters to pick bots (@tank @healer @dps @melee @ranged @warrior @group2 @nearby @level60-70), "
        "chain commands with the separator. \"help <command>\" explains one.");
    return true;
}

void AddCommand(char const* name, char const* help, CommandFn fn, bool persist = false, bool whisperOnly = false)
{
    std::string action = std::string("cmd ") + name;
    sPlayerbotsAi.AddAction(action, [action, fn](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<CommandAction>(action, fn); });

    ChatCommandInfo info;
    info.Action = action;
    info.Help = help;
    info.Persist = persist;
    info.WhisperOnly = whisperOnly;
    GetChatCommands().Add(name, info);
}
}

ChatCommandTable& GetChatCommands()
{
    static ChatCommandTable table;
    return table;
}

void RegisterChatCommands()
{
    ChatCommandTable& table = GetChatCommands();

    AddCommand("follow", "Follow the master.", Follow, true);
    AddCommand("stay", "Stay where I am.", Stay, true);
    AddCommand("passive", "Do not fight.", Passive, true);
    AddCommand("assist", "Fight the master's enemies.", Assist, true);
    AddCommand("attack", "Attack the master's target.", Attack, true);
    AddCommand("status", "Level, health, spells and strategies.", Status);
    AddCommand("who", "My level, specialization and role.", Who);
    AddCommand("release", "Release my spirit.", Release);
    AddCommand("revive", "Come back to life.", Revive);
    AddCommand("flee", "Stop fighting and run to the master.", Flee, true);
    AddCommand("attackers", "The enemies fighting the group.", Attackers);
    AddCommand("target", "What I am fighting.", TargetInfo);
    AddCommand("drink", "Eat and drink now.", Drink);
    AddCommand("rebuff", "Cast my buffs now.", Rebuff);
    AddCommand("leader", "Give the group lead to the master.", GiveLeader);
    AddCommand("gear", "Item level and equipped items.", Gear);
    AddCommand("autogear", "Re-gear: white, green, blue, purple, an item level, match, or reset.", AutoGear);
    AddCommand("inventory", "Bag contents and money.", Inventory);
    AddCommand("spells", "The damage and healing spells I use.", Spells);
    AddCommand("quests", "My quest log.", Quests);
    AddCommand("spec", "spec [name]: my specializations, or switch to one.", SpecCommand, false, true);
    AddCommand("hire", "Join your account for good, for gold (Playerbots.Hire.Enable).", HireCommand, false, true);
    AddCommand("maintenance", "Free repair and food and drink (Playerbots.Command.Maintenance).", MaintenanceCommand, false, true);
    AddCommand("cs", "cs [name [trigger > action > ...]]: custom strategies, then co +custom::name.", CustomStrategyCommand, true, true);
    AddCommand("equip", "equip <item links>: wear items from my bags.", EquipItems, false, true);
    AddCommand("unequip", "unequip <item links>: take items off.", UnequipItems, false, true);
    AddCommand("use", "use <item link>: use an item (on your target if it needs one).", UseItems, false, true);
    AddCommand("sell", "sell <item links | grey>: to the vendor you have selected, I must stand next to it.", SellItems, false, true);
    AddCommand("buy", "buy <item links> [count]: from the vendor you have selected, I must stand next to it.", BuyItems, false, true);
    AddCommand("destroy", "destroy <item links>: delete items from my bags.", DestroyItems, false, true);
    AddCommand("summon", "Teleport to the master.", Summon);
    AddCommand("reset", "Back to the default strategies.", Reset, true);
    AddCommand("co", "Combat strategies: co +name,-name,~name,? .", Combat, true);
    AddCommand("nc", "Non combat strategies: nc +name,-name,~name,? .", NonCombat, true);
    AddCommand("de", "Strategies while dead: de +name,-name,~name,? .", Dead, true);
    AddCommand("strategies", "All active strategies.", AllStrategies);
    AddCommand("cast", "cast <spell name or id> on the master's target, or myself.", Cast);
    AddCommand("goto", "goto <place or node id>: travel there by foot, flight path and portal.", Goto, true);
    AddCommand("help", "help [command].", Help);

    // leaving is done by the caller, the AI does not survive it
    ChatCommandInfo leave;
    leave.Help = "Log out.";
    leave.Leave = true;
    leave.Persist = false;
    table.Add("leave", leave);

    table.Alias("aggressive", "assist");
    table.Alias("stats", "status");
    table.Alias("inv", "inventory");
    table.Alias("i", "inventory");
    table.Alias("q", "quests");
    table.Alias("ss", "strategies");
    table.Alias("logout", "leave");
    table.Alias("?", "help");
}

ChatOutcome HandleBotChat(PlayerbotAI& ai, Player* speaker, std::string text, bool group)
{
    ChatOutcome outcome;
    if (group && !StripPrefix(text, sPlayerbotsConfig.CommandPrefix))
        return outcome;

    // a line addressed to the bot is answered by the next AI tick, not after an idle delay (Playerbots.DynamicTick)
    ai.WakeUp();

    ChatCommandTable const& table = GetChatCommands();
    BotTraits traits = GetTraits(ai, speaker);
    bool claimed = false;   // a chat module took the whisper, tried once however many parts it has

    for (std::string const& part : SplitChain(text, sPlayerbotsConfig.CommandSeparator))
    {
        FilterResult filtered = ApplyChatFilters(part, traits, sPlayerbotsConfig.CommandNearbyDistance);
        if (!filtered.Matches || filtered.Rest.empty())
            continue;

        std::optional<ParsedCommand> parsed = ParseCommand(table, filtered.Rest);
        if (!parsed)
        {
            // party chat is mostly people talking
            if (!group)
            {
                if (!claimed && !outcome.Known)
                    claimed = CallWhisperFallback(ai.GetBot(), speaker, text);
                if (!claimed)
                    ai.TellMaster("Unknown command, whisper \"help\".");
            }
            continue;
        }

        // an ordinary party line that starts with a command word is not an order when anything may start one
        if (group && parsed->Info->WhisperOnly && sPlayerbotsConfig.CommandPrefix.empty())
            continue;

        outcome.Known = true;
        if (parsed->Info->Leave)
        {
            outcome.Leave = true;
            break;
        }

        switch (ai.GetEngine().ExecuteAction(ai, parsed->Info->Action, parsed->Param))
        {
            case ActionResult::Ok:
                if (parsed->Info->Persist)
                    ai.SaveState();
                break;
            case ActionResult::Failed:
                ai.TellMaster("That did not work.");
                break;
            case ActionResult::Impossible:
            case ActionResult::Useless:
                ai.TellMaster("I can't do that right now.");
                break;
            case ActionResult::Unknown:
                TC_LOG_ERROR("server.playerbots", "Playerbots: chat command {} has no action {}", parsed->Name, parsed->Info->Action);
                break;
        }
    }

    return outcome;
}
}
