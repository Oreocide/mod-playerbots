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

#ifndef PlayerbotsGearFactory_h__
#define PlayerbotsGearFactory_h__

#include "Define.h"
#include <vector>

class Player;
struct ItemTemplate;

namespace Playerbots
{
/// Dresses random bots with level appropriate gear.
///
/// The candidate tables are built once (world thread, before any bot logs in) and are read-only afterwards,
/// so Equip()/Supply() can safely run in the map thread of a bot.
/// Suitability is decided by the core: ItemTemplate::IsUsableByLootSpecialization (armor type and primary
/// stat of the active specialization) and Player::CanEquipNewItem (class/race/skill/level restrictions).
class GearFactory
{
public:
    static GearFactory* instance();

    /// Scans all item templates, safe to call repeatedly
    void Build();
    bool IsBuilt() const { return _built; }

    /// true if the bot is missing gear or wears gear far below its level
    bool NeedsGear(Player const* bot) const;

    /// Replaces the bot's equipment with the best gear it can use at its level (shirt and tabard are kept)
    void Equip(Player* bot) const;

    /// Tops up food and drink appropriate for the level so the consume strategy has something to use
    void Supply(Player* bot) const;

    /// Bags in the empty bag slots and pocket money of a level appropriate size (never takes anything away)
    void Provision(Player* bot) const;

private:
    struct Candidate
    {
        ItemTemplate const* Proto = nullptr;
        int32 RequiredLevel = 0;
        uint32 Quality = 0;
        uint32 ItemLevel = 0;
    };

    struct Group
    {
        std::vector<uint8> InventoryTypes;
        std::vector<uint8> Slots;
        std::vector<Candidate> Candidates; // best (highest level) first
    };

    struct Consumable
    {
        uint32 ItemId = 0;
        int32 RequiredLevel = 0;
    };

    bool EquipSlot(Player* bot, Group const& group, uint8 slot, uint32 excludedItem) const;
    static uint32 PickConsumable(std::vector<Consumable> const& list, uint32 level);

    bool _built = false;
    std::vector<Group> _groups;
    struct BagItem
    {
        uint32 ItemId = 0;
        int32 RequiredLevel = 0;
        uint32 Slots = 0;
    };

    std::vector<BagItem> _bags;
    std::vector<Consumable> _food;
    std::vector<Consumable> _drink;
};
}

#define sGearFactory Playerbots::GearFactory::instance()

#endif // PlayerbotsGearFactory_h__
