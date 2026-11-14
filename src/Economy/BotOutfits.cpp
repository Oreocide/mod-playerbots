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

#include "BotOutfits.h"
#include "Db/PlayerbotRepository.h"
#include "EquipmentSetPackets.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ProfessionsConfig.h"
#include "ProfessionsRules.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>

namespace Playerbots::Prof::Outfits
{
namespace
{
constexpr char IndexKey[] = "outfits";

std::string PiecesKey(std::string const& name)
{
    return "outfit:" + name;
}

std::vector<std::string> Names(ObjectGuid bot)
{
    Optional<std::string> index = sPlayerbotRepository->GetValue(bot, IndexKey);
    return index ? Rules::SplitNames(*index) : std::vector<std::string>();
}

std::vector<Rules::OutfitPiece> Pieces(ObjectGuid bot, std::string const& name)
{
    Optional<std::string> text = sPlayerbotRepository->GetValue(bot, PiecesKey(name));
    return text ? Rules::DecodeOutfit(*text, EQUIPMENT_SET_SLOTS) : std::vector<Rules::OutfitPiece>();
}

std::string ItemName(Item const* item)
{
    return item->GetTemplate()->GetName(LOCALE_enUS);
}

bool Save(PlayerbotAI& ai, std::string const& name)
{
    Player* bot = ai.GetBot();
    std::vector<std::string> names = Names(bot->GetGUID());
    bool known = std::ranges::find(names, name) != names.end();
    if (!known && names.size() >= sProfConfig.OutfitsMax)
    {
        ai.TellMaster(Trinity::StringFormat("I already keep {} outfits, delete one first.", names.size()));
        return true;
    }

    std::vector<Rules::OutfitPiece> pieces;
    for (uint8 slot = 0; slot < EQUIPMENT_SET_SLOTS; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            pieces.push_back({ slot, item->GetGUID().GetCounter() });
    if (pieces.empty())
    {
        ai.TellMaster("I'm wearing nothing to save.");
        return true;
    }

    sPlayerbotRepository->SetValue(bot->GetGUID(), PiecesKey(name), Rules::EncodeOutfit(pieces));
    if (!known)
    {
        names.push_back(name);
        sPlayerbotRepository->SetValue(bot->GetGUID(), IndexKey, Rules::JoinNames(names));
    }
    ai.TellMaster(Trinity::StringFormat("Saved \"{}\" ({} pieces).", name, pieces.size()));
    return true;
}

bool Equip(PlayerbotAI& ai, std::string const& name)
{
    Player* bot = ai.GetBot();
    std::vector<Rules::OutfitPiece> pieces = Pieces(bot->GetGUID(), name);
    if (pieces.empty())
    {
        ai.TellMaster(Trinity::StringFormat("I have no outfit \"{}\".", name));
        return true;
    }
    if (bot->IsInCombat())
    {
        ai.TellMaster("Not in a fight.");
        return true;
    }

    // the client's request: a slot the set leaves alone carries this marker, WorldSession::HandleUseEquipmentSet skips it
    ObjectGuid ignored;
    ignored.SetRawValue(0x0C00040000000000, 0xFFFFFFFFFFFFFFFF);

    WorldPackets::EquipmentSet::UseEquipmentSet use{ WorldPacket(CMSG_USE_EQUIPMENT_SET) };
    for (uint8 slot = 0; slot < EQUIPMENT_SET_SLOTS; ++slot)
        use.Items[slot].Item = ignored;

    uint32 missing = 0;
    for (Rules::OutfitPiece const& piece : pieces)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Item>(piece.ItemLow);
        Item* item = bot->GetItemByGuid(guid);
        if (!item)
        {
            ++missing;
            continue;
        }
        use.Items[piece.Slot].Item = guid;
        use.Items[piece.Slot].ContainerSlot = item->GetBagSlot();
        use.Items[piece.Slot].Slot = item->GetSlot();
    }
    if (missing == pieces.size())
    {
        ai.TellMaster(Trinity::StringFormat("None of \"{}\" is left in my bags.", name));
        return true;
    }

    bot->GetSession()->HandleUseEquipmentSet(use);
    if (missing)
        ai.TellMaster(Trinity::StringFormat("Wearing \"{}\", {} pieces are gone.", name, missing));
    else
        ai.TellMaster(Trinity::StringFormat("Wearing \"{}\".", name));
    return true;
}

bool Delete(PlayerbotAI& ai, std::string const& name)
{
    Player* bot = ai.GetBot();
    std::vector<std::string> names = Names(bot->GetGUID());
    auto itr = std::ranges::find(names, name);
    if (itr == names.end())
    {
        ai.TellMaster(Trinity::StringFormat("I have no outfit \"{}\".", name));
        return true;
    }
    names.erase(itr);
    sPlayerbotRepository->EraseValue(bot->GetGUID(), PiecesKey(name));
    if (names.empty())
        sPlayerbotRepository->EraseValue(bot->GetGUID(), IndexKey);
    else
        sPlayerbotRepository->SetValue(bot->GetGUID(), IndexKey, Rules::JoinNames(names));
    ai.TellMaster(Trinity::StringFormat("Forgot \"{}\".", name));
    return true;
}

bool Show(PlayerbotAI& ai, std::string const& name)
{
    Player* bot = ai.GetBot();
    std::vector<Rules::OutfitPiece> pieces = Pieces(bot->GetGUID(), name);
    if (pieces.empty())
    {
        ai.TellMaster(Trinity::StringFormat("I have no outfit \"{}\".", name));
        return true;
    }
    std::string text;
    uint32 missing = 0;
    for (Rules::OutfitPiece const& piece : pieces)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(piece.ItemLow));
        if (!item)
        {
            ++missing;
            continue;
        }
        if (!text.empty())
            text += ", ";
        text += ItemName(item);
    }
    if (missing)
        text += Trinity::StringFormat(" ({} gone)", missing);
    ai.TellMaster(Trinity::StringFormat("\"{}\": {}.", name, text));
    return true;
}
}

bool Order(PlayerbotAI& ai, std::string const& param)
{
    if (!sProfConfig.Outfits)
    {
        ai.TellMaster("Outfits are off (Playerbots.Outfits.Enable).");
        return true;
    }

    Rules::OutfitOrder order = Rules::ParseOutfitOrder(param);
    if (order.Verb != Rules::OutfitVerb::List && order.Name.empty())
    {
        ai.TellMaster("Name the outfit: outfit save|equip|delete|show <name>.");
        return true;
    }

    switch (order.Verb)
    {
        case Rules::OutfitVerb::List:
        {
            std::vector<std::string> names = Names(ai.GetBot()->GetGUID());
            if (names.empty())
                ai.TellMaster("No outfits saved. Say \"outfit save <name>\" to keep what I wear.");
            else
            {
                std::string list;
                for (std::string const& name : names)
                    list += (list.empty() ? "" : ", ") + name;
                ai.TellMaster("Outfits: " + list + ".");
            }
            return true;
        }
        case Rules::OutfitVerb::Save:
            return Save(ai, order.Name);
        case Rules::OutfitVerb::Equip:
            return Equip(ai, order.Name);
        case Rules::OutfitVerb::Delete:
            return Delete(ai, order.Name);
        case Rules::OutfitVerb::Show:
            return Show(ai, order.Name);
    }
    return false;
}

bool IsOutfitItem(Player const* bot, ObjectGuid item)
{
    for (std::string const& name : Names(bot->GetGUID()))
        for (Rules::OutfitPiece const& piece : Pieces(bot->GetGUID(), name))
            if (piece.ItemLow == item.GetCounter())
                return true;
    return false;
}
}
