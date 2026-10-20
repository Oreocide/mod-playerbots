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
#include "ObjectGuid.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    /// A food, drink or potion that Supply hands out for free: a bot must not sell what it is given (auction house, vendor)
    bool IsStocked(uint32 itemId) const { return _stocked.contains(itemId); }

    /// Bags in the empty bag slots and pocket money of a level appropriate size (never takes anything away)
    void Provision(Player* bot) const;

    /// What a found item is worth to the bot: worn at all (some equipment slot takes it), usable by the bot (class, armor type,
    /// primary stat, level, skill) and how much its score beats the worst piece worn in the best slot for it (an empty slot = its
    /// whole score, 0 = no gain). Same scoring as Equip. Used for loot rolls
    struct UpgradeInfo
    {
        bool Equippable = false;
        bool Usable = false;
        float Margin = 0.0f;
    };
    UpgradeInfo GetUpgradeInfo(Player const* bot, ItemTemplate const* proto) const;

    /// Puts one usable upgrade that sits in the bags (a won roll, a traded item) on, replacing the worst piece worn in its slots.
    /// Returns how many items it equipped (0 or 1: the bag positions change after a swap)
    uint32 EquipBagUpgrade(Player* bot) const;

    /// Caps for one Equip() call. QualityCap 0 uses Playerbots.RandomGear.MaxQuality. ItemLevelCap 0 means no cap.
    struct GearOptions
    {
        uint32 QualityCap = 0;
        uint32 ItemLevelCap = 0;
        bool Reset = false;
    };

    enum class GearArgKind { Ok, Match, Bad };

    /// "white|green|blue|purple|<ilvl>|reset". Match means the caller fills ItemLevelCap from a player's average.
    GearArgKind ParseGearArg(std::string const& arg, GearOptions& options) const;

    /// How many slots changed. Keeps a worn piece that already scores at least as high, unless Reset stripped it first.
    uint32 Equip(Player* bot, GearOptions const& options) const;

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

    bool EquipSlot(Player* bot, Group const& group, uint8 slot, uint32 excludedItem, GearOptions const& options) const;
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
    std::vector<Consumable> _healthPotions;   // instant heal potions a bot may carry, best first (Economy/PotionRules.h decides)
    std::vector<Consumable> _manaPotions;
    std::unordered_set<uint32> _stocked;      // the ids of the four lists above

    // Bots that wore armor of a type their class cannot wear are re-geared once; a bot for whom no item of the right type
    // existed (the fallback pass) is left alone until this time (ms), so the re-gear does not repeat every minute
    mutable std::mutex _wrongTypeLock;
    mutable std::unordered_map<ObjectGuid, uint32> _wrongTypeRetryAt;
};
}

#define sGearFactory Playerbots::GearFactory::instance()

#endif // PlayerbotsGearFactory_h__
