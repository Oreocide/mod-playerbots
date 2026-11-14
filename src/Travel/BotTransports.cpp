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

#include "BotTransports.h"
#include "AiRegistry.h"
#include "Cmd/ChatCommands.h"
#include "Cmd/ChatParser.h"
#include "Config.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementPackets.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Transport.h"
#include "Travel/MoveToward.h"

namespace Playerbots::BotTransports
{
namespace
{
constexpr float MasterReach = 40.0f;      // a master farther than this on a transport is not followed aboard
constexpr float DeckStepYards = 12.0f;    // the last stretch from the dock onto the deck, the one move that is not a mesh path
constexpr float BoardYards = 2.5f;        // close enough to the master's spot on the deck to count as aboard
constexpr uint32 DeckStepTimeoutMs = 8000;
constexpr float BoardPriority = 70.0f;    // below eating (75) and healing (80), above everything that walks
constexpr float RidePriority = 65.0f;

Config sConfig;

/// Tells the clients around where the bot stands, transport included. A bot's movement status is never refreshed (no client), and
/// clients ignore a status without the unit's guid: the guid is filled for this packet alone (see Travel/PlayerJump.cpp)
void Broadcast(Player* bot)
{
    MovementInfo& info = bot->m_movementInfo;
    info.time = GameTime::GetGameTimeMS();
    info.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());
    info.guid = bot->GetGUID();
    WorldPackets::Movement::MoveUpdate update;
    update.Status = &info;
    bot->SendMessageToSet(update.Write(), true);
    info.guid.Clear();
}

GameObject* TransportObject(Player* player)
{
    TransportBase* base = player->GetTransport();
    if (!base)
        return nullptr;
    return player->GetMap()->GetGameObject(base->GetTransportGUID());
}

/// Boats and zeppelins are boarded only while they wait at the dock; elevators and trams any time the master stands near
bool Boardable(GameObject* go)
{
    if (!go || !go->ToTransportBase())
        return false;
    if (Transport* transport = go->ToTransport())
        return transport->IsStopped();
    return true;
}

void Board(Player* bot, GameObject* go)
{
    TransportBase* base = go->ToTransportBase();
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    base->AddPassenger(bot, base->GetPositionOffsetTo(bot->GetPosition()));
    Broadcast(bot);
    TC_LOG_DEBUG("server.playerbots", "Playerbots: {} boarded {}", bot->GetName(), go->GetName());
}

void Leave(Player* bot)
{
    if (TransportBase* base = bot->GetTransport())
    {
        base->RemovePassenger(bot);
        Broadcast(bot);
        TC_LOG_DEBUG("server.playerbots", "Playerbots: {} left its transport", bot->GetName());
    }
}

class BoardTransportAction final : public Action
{
public:
    char const* GetName() const override { return "board transport"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Player* master = ai.GetMaster();
        GameObject* go = master ? TransportObject(master) : nullptr;
        if (!go || !Boardable(go) || bot->GetTransport() == go->ToTransportBase())
            return false;

        if (bot->GetTransport())
            Leave(bot);

        uint32 now = GameTime::GetGameTimeMS();
        Position spot = master->GetPosition();
        float dist = bot->GetExactDist(spot);

        if (dist <= BoardYards)
        {
            Board(bot, go);
            _deckStepAt = 0;
            return true;
        }

        // on the dock: the deck step, a straight move of a few yards (the deck has no navmesh). Allowed exception to
        // mmaps-only movement (GAPS-bots G45): only onto a transport, never off one, never for far bots
        if (dist <= DeckStepYards)
        {
            if (!_deckStepAt || now - _deckStepAt > DeckStepTimeoutMs)
            {
                _deckStepAt = now;
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, spot, false);
            }
            else if (!bot->isMoving())
                Board(bot, go);     // the step ended a little short (the master moved on the deck)
            return true;
        }

        _deckStepAt = 0;
        MoveResult result = MoveAlongMesh(bot, spot, DeckStepYards - 1.0f);
        return result != MoveResult::NoPath;
    }

private:
    uint32 _deckStepAt = 0;
};

bool BoardTrigger(PlayerbotAI& ai)
{
    if (!sConfig.Board)
        return false;
    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    if (!master || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->IsInFlight() || bot->GetVehicle() || !bot->IsInMap(master))
        return false;
    GameObject* go = TransportObject(master);
    if (!go || bot->GetTransport() == go->ToTransportBase() || !Boardable(go))
        return false;
    return bot->GetExactDist(master) <= MasterReach;
}

class RideTransportAction final : public Action
{
public:
    char const* GetName() const override { return "ride transport"; }

    bool Execute(PlayerbotAI& ai) override
    {
        Player* bot = ai.GetBot();
        GameObject* go = TransportObject(bot);
        if (!go)
            return false;

        Player* master = ai.GetMaster();
        // IsInMap first: a master on another map belongs to another map thread (MapUpdate.Threads > 1)
        bool masterAboard = master && bot->IsInMap(master) && master->GetTransport() == bot->GetTransport();
        if (!masterAboard && Boardable(go))
        {
            // the master stepped off at a stop: off too, the follow walks on from the deck
            Leave(bot);
            return false;
        }
        return true;    // stand still on deck: the transport carries the bot, no follow or walk while aboard
    }
};

bool RideTrigger(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return sConfig.Board && bot->IsAlive() && bot->GetTransport() && ai.GetMaster() && TransportObject(bot);
}

class TransportStrategy final : public Strategy
{
public:
    TransportStrategy() : Strategy("transports") { }

    uint8 GetStates() const override { return StrategyStates::NonCombat; }

    void InitTriggers(std::vector<TriggerNode>& triggers) override
    {
        triggers.push_back({ "master on transport", { { "board transport", BoardPriority } }, StrategyStates::NonCombat });
        triggers.push_back({ "on transport", { { "ride transport", RidePriority } }, StrategyStates::NonCombat });
    }
};

// "board": the spike order (and the manual way): board the transport the master stands on, now, whatever the key says
bool BoardCommand(PlayerbotAI& ai, std::string const&)
{
    Player* bot = ai.GetBot();
    Player* master = ai.GetMaster();
    GameObject* go = master ? TransportObject(master) : nullptr;
    if (!go)
    {
        ai.TellMaster("You are not on a transport.");
        return true;
    }
    if (!bot->IsInMap(master) || bot->GetExactDist(master) > DeckStepYards)
    {
        ai.TellMaster("Too far from you to step on.");
        return true;
    }
    if (bot->GetTransport())
        Leave(bot);
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    Board(bot, go);
    ai.TellMaster(std::string("Aboard ") + go->GetName() + ".");
    return true;
}

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
}

Config const& GetConfig()
{
    return sConfig;
}

void LoadConfig()
{
    Config cfg;
    cfg.Board = sConfigMgr->GetBoolDefault("Playerbots.Transport.Board", false);
    sConfig = cfg;
}

void Register(AiRegistry& registry)
{
    registry.AddTrigger("master on transport", 500, BoardTrigger);
    registry.AddTrigger("on transport", 1000, RideTrigger);
    registry.AddAction<BoardTransportAction>("board transport");
    registry.AddAction<RideTransportAction>("ride transport");
    registry.AddStrategy<TransportStrategy>("transports");

    std::string action = "cmd board";
    registry.AddAction(action, [action](std::string const&) -> std::unique_ptr<Action> { return std::make_unique<OrderAction>(action, BoardCommand); });
    ChatCommandInfo info;
    info.Action = action;
    info.Help = "board: step onto the transport you stand on (boat, zeppelin, elevator, tram).";
    info.Persist = false;
    info.WhisperOnly = true;
    GetChatCommands().Add("board", info);
}
}
