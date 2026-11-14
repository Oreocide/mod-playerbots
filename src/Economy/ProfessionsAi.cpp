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

#include "ProfessionsAi.h"
#include "AiRegistry.h"
#include "BotCrafting.h"
#include "BotFishing.h"
#include "BotLootWatch.h"
#include "BotOutfits.h"
#include "Cmd/ChatCommands.h"
#include "Cmd/ChatParser.h"
#include "ContextStrategies.h"
#include "Engine.h"
#include "ProfessionsConfig.h"
#include "ScriptMgr.h"

namespace Playerbots::Prof
{
namespace
{
class ProfessionsStrategy final : public Strategy
{
public:
    ProfessionsStrategy() : Strategy("professions") { }

    uint8_t GetStates() const override { return StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        for (TriggerNode& node : CraftingTriggerNodes())
            triggers.push_back(std::move(node));
        for (TriggerNode& node : Fishing::FishingTriggerNodes())
            triggers.push_back(std::move(node));
    }
};

using OrderFn = bool (*)(PlayerbotAI& ai, std::string const& param);

// Cmd/ChatCommands.cpp's AddCommand, which is file-local there
class OrderAction final : public Action
{
public:
    OrderAction(std::string name, OrderFn fn) : _name(std::move(name)), _fn(fn) { }

    char const* GetName() const override { return _name.c_str(); }
    bool Execute(PlayerbotAI& ai) override { return _fn(ai, GetParam()); }

private:
    std::string _name;
    OrderFn _fn;
};

void AddOrder(AiRegistry& registry, char const* name, char const* help, OrderFn fn, bool persist, bool whisperOnly)
{
    std::string action = std::string("cmd ") + name;
    registry.AddAction(action, [action, fn](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<OrderAction>(action, fn); });

    ChatCommandInfo info;
    info.Action = action;
    info.Help = help;
    info.Persist = persist;
    info.WhisperOnly = whisperOnly;
    GetChatCommands().Add(name, info);
}
}

void RegisterProfessionsAi(AiRegistry& registry)
{
    RegisterCraftingAi(registry);
    Fishing::RegisterFishingAi(registry);
    registry.AddStrategy<ProfessionsStrategy>("professions");

    // every bot, random or in a party, outside battlegrounds and arenas; each behaviour checks its own key
    sContextStrategies.Register({ [](BotContext const& context)
    {
        return context.Place != PlaceKind::Battleground && context.Place != PlaceKind::Arena;
    }, { "professions" } });

    AddOrder(registry, "outfit", "outfit [list | save <name> | <name> | delete <name> | show <name>]: named gear sets (Playerbots.Outfits.Enable).",
        Outfits::Order, false, true);
    AddOrder(registry, "fish", "fish [stop]: fish a few casts at the nearest shore (Playerbots.Fishing.Enable).", Fishing::Order, false, true);
    AddOrder(registry, "open", "Open the lockboxes and clams in my bags; a rogue picks a locked one (Playerbots.Lockbox.Enable).",
        OpenItemsOrder, false, true);
}
}

namespace
{
class playerbots_professions_world : public WorldScript
{
public:
    playerbots_professions_world() : WorldScript("playerbots_professions_world")
    {
        // OnConfigLoad(false) runs before scripts are registered
        sProfConfig.Load();
    }

    void OnConfigLoad(bool /*reload*/) override
    {
        sProfConfig.Load();
    }

    // after the map updates (World::Update waits for them), in the world thread: the loot opcodes are THREADUNSAFE
    void OnUpdate(uint32 /*diff*/) override
    {
        Playerbots::Prof::LootWatch::Update();
    }
};
}

void AddSC_playerbots_professions()
{
    new playerbots_professions_world();
}
