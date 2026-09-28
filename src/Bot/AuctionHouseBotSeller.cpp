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

#include "AuctionHouseBotSeller.h"
#include "AuctionHouseMgr.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DB2Stores.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace
{
enum class AuctionBotMarketSegment
{
    Commodity,
    Equipment,
    Other,
    Count
};

AuctionBotMarketSegment GetMarketSegmentForClass(uint32 itemClass)
{
    switch (itemClass)
    {
        case ITEM_CLASS_CONSUMABLE:
        case ITEM_CLASS_GEM:
        case ITEM_CLASS_REAGENT:
        case ITEM_CLASS_PROJECTILE:
        case ITEM_CLASS_TRADE_GOODS:
        case ITEM_CLASS_GLYPH:
            return AuctionBotMarketSegment::Commodity;
        case ITEM_CLASS_CONTAINER:
        case ITEM_CLASS_WEAPON:
        case ITEM_CLASS_ARMOR:
        case ITEM_CLASS_QUIVER:
            return AuctionBotMarketSegment::Equipment;
        default:
            return AuctionBotMarketSegment::Other;
    }
}

AuctionBotMarketSegment GetMarketSegment(ItemTemplate const* prototype)
{
    return GetMarketSegmentForClass(prototype->GetClass());
}

char const* GetMarketSegmentName(AuctionBotMarketSegment segment)
{
    switch (segment)
    {
        case AuctionBotMarketSegment::Commodity: return "commodity";
        case AuctionBotMarketSegment::Equipment: return "equipment";
        default: return "other";
    }
}

uint32 GetMarketSegmentCeiling(ItemTemplate const* prototype)
{
    switch (GetMarketSegment(prototype))
    {
        case AuctionBotMarketSegment::Commodity:
            return sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_COMMODITY_ITEM_CEILING);
        case AuctionBotMarketSegment::Equipment:
            return sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_EQUIPMENT_ITEM_CEILING);
        default:
            return sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_OTHER_ITEM_CEILING);
    }
}

uint32 GetMarketSegmentWeight(AuctionBotMarketSegment segment)
{
    switch (segment)
    {
        case AuctionBotMarketSegment::Commodity:
            return sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_COMMODITY_WEIGHT);
        case AuctionBotMarketSegment::Equipment:
            return sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_EQUIPMENT_WEIGHT);
        default:
            return sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_OTHER_WEIGHT);
    }
}

char const* GetMarketStackPolicyName(ItemTemplate const* prototype)
{
    if (!sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_PROFILE_ENABLED))
        return "legacy_class_random";

    switch (GetMarketSegment(prototype))
    {
        case AuctionBotMarketSegment::Commodity: return "commodity_shapes";
        case AuctionBotMarketSegment::Equipment: return "single";
        default: return "legacy_class_random";
    }
}
}

AuctionBotSeller::AuctionBotSeller()
{
    // Define faction for our main data class.
    for (uint8 i = 0; i < MAX_AUCTION_HOUSE_TYPE; ++i)
        _houseConfig[i].Initialize(AuctionHouseType(i));
}

AuctionBotSeller::~AuctionBotSeller()
{
}

bool AuctionBotSeller::Initialize()
{
    std::unordered_set<uint32> npcItems;
    std::unordered_set<uint32> lootItems;
    std::unordered_set<uint32> professionCraftedItems;
    std::unordered_set<uint32> includeItems;
    std::unordered_set<uint32> excludeItems;

    TC_LOG_DEBUG("ahbot", "AHBot seller filters:");

    {
        std::stringstream includeStream(sAuctionBotConfig->GetAHBotIncludes());
        std::string temp;
        while (std::getline(includeStream, temp, ','))
            includeItems.insert(atoi(temp.c_str()));
    }

    {
        std::stringstream excludeStream(sAuctionBotConfig->GetAHBotExcludes());
        std::string temp;
        while (std::getline(excludeStream, temp, ','))
            excludeItems.insert(atoi(temp.c_str()));
    }

    TC_LOG_DEBUG("ahbot", "Forced Inclusion %u items", (uint32)includeItems.size());
    TC_LOG_DEBUG("ahbot", "Forced Exclusion %u items", (uint32)excludeItems.size());

    TC_LOG_DEBUG("ahbot", "Loading npc vendor items for filter..");
    CreatureTemplateContainer const* creatures = sObjectMgr->GetCreatureTemplates();
    for (CreatureTemplateContainer::const_iterator it = creatures->begin(); it != creatures->end(); ++it)
        if (VendorItemData const* data = sObjectMgr->GetNpcVendorItemList(it->first))
            for (VendorItem const& it2 : data->m_items)
                npcItems.insert(it2.item);

    TC_LOG_DEBUG("ahbot", "Npc vendor filter has %u items", (uint32)npcItems.size());

    TC_LOG_DEBUG("ahbot", "Loading loot items for filter..");
    QueryResult result = WorldDatabase.PQuery(
        "SELECT `item` FROM `creature_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `disenchant_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `fishing_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `gameobject_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `item_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `milling_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `pickpocketing_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `prospecting_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `reference_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `skinning_loot_template` WHERE `Reference` = 0 UNION "
        "SELECT `item` FROM `spell_loot_template` WHERE `Reference` = 0");

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();

            uint32 entry = fields[0].GetUInt32();
            if (!entry)
                continue;

            lootItems.insert(entry);
        } while (result->NextRow());
    }

    TC_LOG_DEBUG("ahbot", "Loot filter has %u items", (uint32)lootItems.size());

    TC_LOG_DEBUG("ahbot", "Loading profession-crafted items for market catalog provenance..");
    std::unordered_set<uint32> professionSpells;
    for (SkillLineAbilityEntry const* skillLineAbility : sSkillLineAbilityStore)
        if (skillLineAbility && IsProfessionSkill(skillLineAbility->SkillLine))
            professionSpells.insert(skillLineAbility->Spell);

    for (uint32 spellId : professionSpells)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        for (SpellEffectInfo const& effect : spellInfo->Effects)
            if ((effect.IsEffect(SPELL_EFFECT_CREATE_ITEM) || effect.IsEffect(SPELL_EFFECT_CREATE_ITEM_2)) && effect.ItemType)
                professionCraftedItems.insert(effect.ItemType);
    }

    TC_LOG_DEBUG("ahbot", "Profession-crafted provenance has %u direct item outputs", (uint32)professionCraftedItems.size());
    TC_LOG_DEBUG("ahbot", "Sorting and cleaning items for AHBot seller...");

    std::ofstream marketCatalog;
    std::string const& marketCatalogFile = sAuctionBotConfig->GetMarketCatalogFile();
    if (!marketCatalogFile.empty())
    {
        marketCatalog.open(marketCatalogFile, std::ios::out | std::ios::trunc);
        if (marketCatalog.is_open())
            marketCatalog << "item_id,item_class,item_subclass,quality,item_level,required_level,required_skill,required_skill_rank,bonding,max_stack,buy_price,sell_price,market_segment,configured_segment_weight,configured_item_ceiling,supply_profile_enabled,stack_policy,source,is_vendor,is_loot,is_profession_crafted,seller_eligible,forced_include\n";
        else
            TC_LOG_ERROR("ahbot", "Unable to open AHBot market catalog file '%s' for writing.", marketCatalogFile.c_str());
    }

    uint32 itemsAdded = 0;
    uint32 craftedCandidatesAdded = 0;
    std::unordered_set<uint32> catalogedItems;
    auto writeCatalogItem = [&](uint32 itemId, ItemTemplate const* prototype, bool sellerEligible, bool forcedInclude)
    {
        if (!marketCatalog.is_open())
            return;

        bool const isVendorItem = npcItems.count(itemId) > 0;
        bool const isLootItem = lootItems.count(itemId) > 0;
        bool const isProfessionCraftedItem = professionCraftedItems.count(itemId) > 0;
        std::string source;
        if (isVendorItem)
            source = "vendor";
        if (isLootItem)
            source += source.empty() ? "loot" : "+loot";
        if (isProfessionCraftedItem)
            source += source.empty() ? "profession_crafted" : "+profession_crafted";
        if (source.empty())
            source = "other";

        marketCatalog << itemId << ','
            << prototype->GetClass() << ','
            << prototype->GetSubClass() << ','
            << prototype->GetQuality() << ','
            << prototype->GetBaseItemLevel() << ','
            << prototype->GetRequiredLevel() << ','
            << prototype->GetRequiredSkill() << ','
            << prototype->GetRequiredSkillRank() << ','
            << uint32(prototype->GetBonding()) << ','
            << prototype->GetMaxStackSize() << ','
            << prototype->GetBuyPrice() << ','
            << prototype->GetSellPrice() << ','
            << GetMarketSegmentName(GetMarketSegment(prototype)) << ','
            << GetMarketSegmentWeight(GetMarketSegment(prototype)) << ','
            << GetMarketSegmentCeiling(prototype) << ','
            << (sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_PROFILE_ENABLED) ? 1 : 0) << ','
            << GetMarketStackPolicyName(prototype) << ','
            << source << ','
            << (isVendorItem ? 1 : 0) << ','
            << (isLootItem ? 1 : 0) << ','
            << (isProfessionCraftedItem ? 1 : 0) << ','
            << (sellerEligible ? 1 : 0) << ','
            << (forcedInclude ? 1 : 0) << '\n';

        catalogedItems.insert(itemId);
    };

    auto addEligibleItem = [&](uint32 itemId, ItemTemplate const* prototype, bool forcedInclude)
    {
        _itemPool[prototype->GetQuality()][prototype->GetClass()].push_back(itemId);
        ++itemsAdded;
        writeCatalogItem(itemId, prototype, true, forcedInclude);
    };

    for (uint32 itemId = 0; itemId < sItemStore.GetNumRows(); ++itemId)
    {
        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemId);
        if (!prototype)
            continue;

        // skip items with too high quality (code can't properly work with its)
        if (prototype->GetQuality() >= MAX_AUCTION_QUALITY)
            continue;

        // forced exclude filter
        if (excludeItems.count(itemId))
            continue;

        // forced include filter
        if (includeItems.count(itemId))
        {
            addEligibleItem(itemId, prototype, true);
            continue;
        }

        // bounding filters
        switch (prototype->GetBonding())
        {
            case BIND_NONE:
                if (!sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BIND_NO))
                    continue;
                break;
            case BIND_ON_ACQUIRE:
                if (!sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BIND_PICKUP))
                    continue;
                break;
            case BIND_ON_EQUIP:
                if (!sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BIND_EQUIP))
                    continue;
                break;
            case BIND_ON_USE:
                if (!sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BIND_USE))
                    continue;
                break;
            case BIND_QUEST:
                if (!sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BIND_QUEST))
                    continue;
                break;
            default:
                continue;
        }

        bool allowZero = false;
        switch (prototype->GetClass())
        {
            case ITEM_CLASS_CONSUMABLE:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_CONSUMABLE_ALLOW_ZERO); break;
            case ITEM_CLASS_CONTAINER:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_CONTAINER_ALLOW_ZERO); break;
            case ITEM_CLASS_WEAPON:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_WEAPON_ALLOW_ZERO); break;
            case ITEM_CLASS_GEM:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GEM_ALLOW_ZERO); break;
            case ITEM_CLASS_ARMOR:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_ARMOR_ALLOW_ZERO); break;
            case ITEM_CLASS_REAGENT:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_REAGENT_ALLOW_ZERO); break;
            case ITEM_CLASS_PROJECTILE:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_PROJECTILE_ALLOW_ZERO); break;
            case ITEM_CLASS_TRADE_GOODS:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_TRADEGOOD_ALLOW_ZERO); break;
            case ITEM_CLASS_RECIPE:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RECIPE_ALLOW_ZERO); break;
            case ITEM_CLASS_QUIVER:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_QUIVER_ALLOW_ZERO); break;
            case ITEM_CLASS_QUEST:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_QUEST_ALLOW_ZERO); break;
            case ITEM_CLASS_KEY:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_KEY_ALLOW_ZERO); break;
            case ITEM_CLASS_MISCELLANEOUS:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_MISC_ALLOW_ZERO); break;
            case ITEM_CLASS_GLYPH:
                allowZero = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GLYPH_ALLOW_ZERO); break;
            default:
                allowZero = false;
        }

        // Filter out items with no buy/sell price unless otherwise flagged in the config.
        if (!allowZero)
        {
            if (sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BUYPRICE_SELLER))
            {
                if (prototype->GetSellPrice() == 0)
                    continue;
            }
            else
            {
                if (prototype->GetBuyPrice() == 0)
                    continue;
            }
        }

        bool const allowAsCrafted = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEMS_CRAFTED) && professionCraftedItems.count(itemId);

        // A recognized profession output may opt into the pool independently
        // of the legacy vendor/loot/misc source filters.  With Crafted = 0,
        // this preserves the legacy filtering behavior exactly.
        if (!allowAsCrafted && !sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEMS_VENDOR))
        {
            if (npcItems.count(itemId))
                continue;
        }

        // loot filter
        if (!allowAsCrafted && !sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEMS_LOOT))
        {
            if (lootItems.count(itemId))
                continue;
        }

        // not vendor/loot filter
        if (!allowAsCrafted && !sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEMS_MISC))
        {
            bool const isVendorItem = npcItems.count(itemId) > 0;
            bool const isLootItem = lootItems.count(itemId) > 0;

            if (!isLootItem && !isVendorItem)
                continue;
        }

        // item class/subclass specific filters
        switch (prototype->GetClass())
        {
            case ITEM_CLASS_ARMOR:
            case ITEM_CLASS_WEAPON:
            {
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MIN_ITEM_LEVEL))
                    if (prototype->GetBaseItemLevel() < value)
                        continue;
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MAX_ITEM_LEVEL))
                    if (prototype->GetBaseItemLevel() > value)
                        continue;
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MIN_REQ_LEVEL))
                    if (prototype->GetRequiredLevel() < value)
                        continue;
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MAX_REQ_LEVEL))
                    if (prototype->GetRequiredLevel() > value)
                        continue;
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MIN_SKILL_RANK))
                    if (prototype->GetRequiredSkillRank() < value)
                        continue;
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MAX_SKILL_RANK))
                    if (prototype->GetRequiredSkillRank() > value)
                        continue;
                break;
            }
            case ITEM_CLASS_RECIPE:
            case ITEM_CLASS_CONSUMABLE:
            case ITEM_CLASS_PROJECTILE:
            {
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MIN_REQ_LEVEL))
                    if (prototype->GetRequiredLevel() < value)
                        continue;
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MAX_REQ_LEVEL))
                    if (prototype->GetRequiredLevel() > value)
                        continue;
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MIN_SKILL_RANK))
                    if (prototype->GetRequiredSkillRank() < value)
                        continue;
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_ITEM_MAX_SKILL_RANK))
                    if (prototype->GetRequiredSkillRank() > value)
                        continue;
                break;
            }
            case ITEM_CLASS_MISCELLANEOUS:
                if (prototype->GetSubClass() == ITEM_SUBCLASS_JUNK_MOUNT)
                {
                    if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_MISC_MOUNT_MIN_REQ_LEVEL))
                        if (prototype->GetRequiredLevel() < value)
                            continue;
                    if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_MISC_MOUNT_MAX_REQ_LEVEL))
                        if (prototype->GetRequiredLevel() > value)
                            continue;
                    if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_MISC_MOUNT_MIN_SKILL_RANK))
                        if (prototype->GetRequiredSkillRank() < value)
                            continue;
                    if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_MISC_MOUNT_MAX_SKILL_RANK))
                        if (prototype->GetRequiredSkillRank() > value)
                            continue;
                }

                if (prototype->GetFlags() & ITEM_FLAG_HAS_LOOT)
                {
                    // skip any not locked lootable items (mostly quest specific or reward cases)
                    if (!prototype->GetLockID())
                        continue;

                    if (!sAuctionBotConfig->GetConfig(CONFIG_AHBOT_LOCKBOX_ENABLED))
                        continue;
                }

                break;
            case ITEM_CLASS_GLYPH:
            {
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GLYPH_MIN_REQ_LEVEL))
                    if (prototype->GetRequiredLevel() < value)
                        continue;
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GLYPH_MAX_REQ_LEVEL))
                    if (prototype->GetRequiredLevel() > value)
                        continue;
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GLYPH_MIN_ITEM_LEVEL))
                    if (prototype->GetRequiredLevel() < value)
                        continue;
                if (int32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GLYPH_MAX_ITEM_LEVEL))
                    if (prototype->GetRequiredLevel() > value)
                        continue;
                break;
            }
            case ITEM_CLASS_TRADE_GOODS:
            {
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_TRADEGOOD_MIN_ITEM_LEVEL))
                    if (prototype->GetBaseItemLevel() < value)
                        continue;
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_TRADEGOOD_MAX_ITEM_LEVEL))
                    if (prototype->GetBaseItemLevel() > value)
                        continue;
                break;
            }
            case ITEM_CLASS_CONTAINER:
            case ITEM_CLASS_QUIVER:
            {
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_CONTAINER_MIN_ITEM_LEVEL))
                    if (prototype->GetBaseItemLevel() < value)
                        continue;
                if (uint32 value = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_CONTAINER_MAX_ITEM_LEVEL))
                    if (prototype->GetBaseItemLevel() > value)
                        continue;
                break;
            }
        }

        addEligibleItem(itemId, prototype, false);
    }

    if (marketCatalog.is_open())
    {
        // Expose crafted outputs that the active filters excluded without
        // adding them to _itemPool.  The seller_eligible column is the boundary
        // between observation and the live posting pool.
        for (uint32 itemId = 0; itemId < sItemStore.GetNumRows(); ++itemId)
        {
            if (!professionCraftedItems.count(itemId) || catalogedItems.count(itemId))
                continue;

            ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemId);
            if (!prototype)
                continue;

            writeCatalogItem(itemId, prototype, false, false);
            ++craftedCandidatesAdded;
        }

        TC_LOG_INFO("ahbot", "Wrote %u seller-eligible entries and %u additional profession-crafted candidates to AHBot market catalog '%s'.",
            itemsAdded, craftedCandidatesAdded, marketCatalogFile.c_str());
    }

    if (!itemsAdded)
    {
        TC_LOG_ERROR("ahbot", "AuctionHouseBot seller not have items, disabled.");
        sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ALLIANCE_ITEM_AMOUNT_RATIO, 0);
        sAuctionBotConfig->SetConfig(CONFIG_AHBOT_HORDE_ITEM_AMOUNT_RATIO, 0);
        sAuctionBotConfig->SetConfig(CONFIG_AHBOT_NEUTRAL_ITEM_AMOUNT_RATIO, 0);
        return false;
    }

    TC_LOG_DEBUG("ahbot", "AuctionHouseBot seller will use %u items to fill auction house (according your config choices)", itemsAdded);

    LoadConfig();

    TC_LOG_DEBUG("ahbot", "Items loaded \tGray\tWhite\tGreen\tBlue\tPurple\tOrange\tYellow");
    for (uint32 i = 0; i < MAX_ITEM_CLASS; ++i)
        TC_LOG_DEBUG("ahbot", "\t\t%u\t%u\t%u\t%u\t%u\t%u\t%u",
        (uint32)_itemPool[0][i].size(), (uint32)_itemPool[1][i].size(), (uint32)_itemPool[2][i].size(),
        (uint32)_itemPool[3][i].size(), (uint32)_itemPool[4][i].size(), (uint32)_itemPool[5][i].size(),
        (uint32)_itemPool[6][i].size());

    TC_LOG_DEBUG("ahbot", "AHBot seller configuration data loaded and initialized");
    return true;
}

void AuctionBotSeller::LoadConfig()
{
    for (uint8 i = 0; i < MAX_AUCTION_HOUSE_TYPE; ++i)
        if (sAuctionBotConfig->GetConfigItemAmountRatio(AuctionHouseType(i)))
            LoadSellerValues(_houseConfig[i]);
}

void AuctionBotSeller::LoadItemsQuantity(SellerConfiguration& config)
{
    uint32 ratio = sAuctionBotConfig->GetConfigItemAmountRatio(config.GetHouseType());

    for (uint32 i = 0; i < MAX_AUCTION_QUALITY; ++i)
    {
        uint32 amount = sAuctionBotConfig->GetConfig(AuctionBotConfigUInt32Values(CONFIG_AHBOT_ITEM_GRAY_AMOUNT + i));
        config.SetItemsAmountPerQuality(AuctionQuality(i), std::lroundf(amount * ratio / 100.f));
    }

    // Set Stack Quantities
    config.SetRandomStackRatioPerClass(ITEM_CLASS_CONSUMABLE, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_CONSUMABLE));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_CONTAINER, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_CONTAINER));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_WEAPON, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_WEAPON));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_GEM, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_GEM));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_ARMOR, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_ARMOR));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_REAGENT, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_REAGENT));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_PROJECTILE, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_PROJECTILE));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_TRADE_GOODS, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_TRADEGOOD));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_GENERIC, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_GENERIC));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_RECIPE, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_RECIPE));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_QUIVER, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_QUIVER));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_QUEST, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_QUEST));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_KEY, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_KEY));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_MISCELLANEOUS, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_MISC));
    config.SetRandomStackRatioPerClass(ITEM_CLASS_GLYPH, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RANDOMSTACKRATIO_GLYPH));

    // Set the best value to get nearest amount of items wanted
    auto getPriorityForClass = [](uint32 itemClass) -> uint32
    {
        AuctionBotConfigUInt32Values index;
        switch (itemClass)
        {
            case ITEM_CLASS_CONSUMABLE:
                index = CONFIG_AHBOT_CLASS_CONSUMABLE_PRIORITY; break;
            case ITEM_CLASS_CONTAINER:
                index = CONFIG_AHBOT_CLASS_CONTAINER_PRIORITY; break;
            case ITEM_CLASS_WEAPON:
                index = CONFIG_AHBOT_CLASS_WEAPON_PRIORITY; break;
            case ITEM_CLASS_GEM:
                index = CONFIG_AHBOT_CLASS_GEM_PRIORITY; break;
            case ITEM_CLASS_ARMOR:
                index = CONFIG_AHBOT_CLASS_ARMOR_PRIORITY; break;
            case ITEM_CLASS_REAGENT:
                index = CONFIG_AHBOT_CLASS_REAGENT_PRIORITY; break;
            case ITEM_CLASS_PROJECTILE:
                index = CONFIG_AHBOT_CLASS_PROJECTILE_PRIORITY; break;
            case ITEM_CLASS_TRADE_GOODS:
                index = CONFIG_AHBOT_CLASS_TRADEGOOD_PRIORITY; break;
            case ITEM_CLASS_GENERIC:
                index = CONFIG_AHBOT_CLASS_GENERIC_PRIORITY; break;
            case ITEM_CLASS_RECIPE:
                index = CONFIG_AHBOT_CLASS_RECIPE_PRIORITY; break;
            case ITEM_CLASS_QUIVER:
                index = CONFIG_AHBOT_CLASS_QUIVER_PRIORITY; break;
            case ITEM_CLASS_QUEST:
                index = CONFIG_AHBOT_CLASS_QUEST_PRIORITY; break;
            case ITEM_CLASS_KEY:
                index = CONFIG_AHBOT_CLASS_KEY_PRIORITY; break;
            case ITEM_CLASS_MISCELLANEOUS:
                index = CONFIG_AHBOT_CLASS_MISC_PRIORITY; break;
            case ITEM_CLASS_GLYPH:
                index = CONFIG_AHBOT_CLASS_GLYPH_PRIORITY; break;
            default:
                return 0;
        }

        return sAuctionBotConfig->GetConfig(index);
    };

    struct WeightedAllocation
    {
        uint32 Key;
        uint32 Amount;
        double Remainder;
    };

    auto allocateByWeight = [](std::vector<std::pair<uint32, uint32>> const& weightedKeys, uint32 totalAmount)
    {
        std::vector<WeightedAllocation> allocations;
        uint64 totalWeight = 0;
        for (std::pair<uint32, uint32> const& weightedKey : weightedKeys)
            totalWeight += weightedKey.second;

        if (!totalWeight)
            return allocations;

        uint32 allocatedAmount = 0;
        for (std::pair<uint32, uint32> const& weightedKey : weightedKeys)
        {
            if (!weightedKey.second)
                continue;

            double exactAmount = weightedKey.second / double(totalWeight) * totalAmount;
            uint32 baseAmount = uint32(std::floor(exactAmount));
            allocations.push_back({ weightedKey.first, baseAmount, exactAmount - baseAmount });
            allocatedAmount += baseAmount;
        }

        uint32 remainingAmount = totalAmount - allocatedAmount;
        std::sort(allocations.begin(), allocations.end(), [](WeightedAllocation const& left, WeightedAllocation const& right)
        {
            if (left.Remainder != right.Remainder)
                return left.Remainder > right.Remainder;

            return left.Key < right.Key;
        });

        ASSERT(remainingAmount <= allocations.size());
        for (uint32 i = 0; i < remainingAmount; ++i)
            ++allocations[i].Amount;

        return allocations;
    };

    for (uint32 j = 0; j < MAX_AUCTION_QUALITY; ++j)
    {
        uint32 qualityAmount = config.GetItemsAmountPerQuality(AuctionQuality(j));
        std::vector<std::pair<uint32, uint32>> classWeights;
        std::vector<std::vector<std::pair<uint32, uint32>>> segmentClassWeights;
        segmentClassWeights.resize(uint32(AuctionBotMarketSegment::Count));

        for (uint32 i = 0; i < MAX_ITEM_CLASS; ++i)
        {
            config.SetItemsAmountPerClass(AuctionQuality(j), ItemClass(i), 0);

            uint32 classPrio = getPriorityForClass(i);
            if (_itemPool[j][i].empty() || !classPrio)
                continue;

            classWeights.emplace_back(i, classPrio);
            uint32 segment = uint32(GetMarketSegmentForClass(i));
            segmentClassWeights[segment].emplace_back(i, classPrio);
        }

        if (classWeights.empty())
            continue;

        if (sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_PROFILE_ENABLED))
        {
            std::vector<std::pair<uint32, uint32>> segmentWeights;
            for (uint32 segment = 0; segment < uint32(AuctionBotMarketSegment::Count); ++segment)
            {
                if (!segmentClassWeights[segment].empty())
                    segmentWeights.emplace_back(segment, GetMarketSegmentWeight(AuctionBotMarketSegment(segment)));
            }

            std::vector<WeightedAllocation> segmentAllocations = allocateByWeight(segmentWeights, qualityAmount);
            if (!segmentAllocations.empty())
            {
                for (WeightedAllocation const& segmentAllocation : segmentAllocations)
                {
                    for (WeightedAllocation const& classAllocation : allocateByWeight(segmentClassWeights[segmentAllocation.Key], segmentAllocation.Amount))
                        config.SetItemsAmountPerClass(AuctionQuality(j), ItemClass(classAllocation.Key), classAllocation.Amount);
                }
                continue;
            }

            TC_LOG_ERROR("ahbot", "AHBot: Supply profile has no positive segment weight for quality %u; using legacy class priorities.", j);
        }

        // Legacy allocation remains the fallback and the exact behavior when
        // the supply profile is disabled.
        for (WeightedAllocation const& classAllocation : allocateByWeight(classWeights, qualityAmount))
            config.SetItemsAmountPerClass(AuctionQuality(j), ItemClass(classAllocation.Key), classAllocation.Amount);
    }

    // do some assert checking, GetItemAmount must always return 0 if selected _itemPool is empty
    for (uint32 j = 0; j < MAX_AUCTION_QUALITY; ++j)
    {
        for (uint32 i = 0; i < MAX_ITEM_CLASS; ++i)
        {
            if (_itemPool[j][i].empty())
                ASSERT(config.GetItemsAmountPerClass(AuctionQuality(j), ItemClass(i)) == 0);
        }
    }
}

void AuctionBotSeller::LoadSellerValues(SellerConfiguration& config)
{
    LoadItemsQuantity(config);
    uint32 ratio = sAuctionBotConfig->GetConfigPriceRatio(config.GetHouseType());

    for (uint32 i = 0; i < MAX_AUCTION_QUALITY; ++i)
    {
        uint32 amount = sAuctionBotConfig->GetConfig(AuctionBotConfigUInt32Values(CONFIG_AHBOT_ITEM_GRAY_PRICE_RATIO + i));
        config.SetPriceRatioPerQuality(AuctionQuality(i), std::lroundf(amount * ratio / 100.f));
    }

    config.SetPriceRatioPerClass(ITEM_CLASS_CONSUMABLE, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_CONSUMABLE_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_CONTAINER, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_CONTAINER_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_WEAPON, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_WEAPON_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_GEM, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GEM_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_ARMOR, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_ARMOR_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_REAGENT, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_REAGENT_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_PROJECTILE, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_PROJECTILE_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_TRADE_GOODS, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_TRADEGOOD_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_GENERIC, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GENERIC_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_RECIPE, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_RECIPE_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_MONEY, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_MONEY_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_QUIVER, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_QUIVER_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_QUEST, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_QUEST_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_KEY, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_KEY_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_PERMANENT, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_PERMANENT_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_MISCELLANEOUS, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_MISC_PRICE_RATIO));
    config.SetPriceRatioPerClass(ITEM_CLASS_GLYPH, sAuctionBotConfig->GetConfig(CONFIG_AHBOT_CLASS_GLYPH_PRICE_RATIO));

    //load min and max auction times
    config.SetMinTime(sAuctionBotConfig->GetConfig(CONFIG_AHBOT_MINTIME));
    config.SetMaxTime(sAuctionBotConfig->GetConfig(CONFIG_AHBOT_MAXTIME));
}

// Set static of items on one AH faction.
// Fill ItemInfos object with real content of AH.
uint32 AuctionBotSeller::SetStat(SellerConfiguration& config)
{
    AllItemsArray itemsSaved(MAX_AUCTION_QUALITY, std::vector<uint32>(MAX_ITEM_CLASS));

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config.GetHouseType());
    for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        AuctionEntry* auctionEntry = itr->second;
        Item* item = sAuctionMgr->GetAItem(auctionEntry->itemGUIDLow);
        if (item)
        {
            ItemTemplate const* prototype = item->GetTemplate();
            if (prototype)
                if (!auctionEntry->owner || sAuctionBotConfig->IsBotChar(auctionEntry->owner)) // Add only ahbot items
                    ++itemsSaved[prototype->GetQuality()][prototype->GetClass()];
        }
    }

    uint32 count = 0;
    for (uint32 j = 0; j < MAX_AUCTION_QUALITY; ++j)
    {
        for (uint32 i = 0; i < MAX_ITEM_CLASS; ++i)
        {
            config.SetMissedItemsPerClass((AuctionQuality)j, (ItemClass)i, itemsSaved[j][i]);
            count += config.GetMissedItemsPerClass((AuctionQuality)j, (ItemClass)i);
        }
    }

    TC_LOG_DEBUG("ahbot", "AHBot: Missed Item       \tGray\tWhite\tGreen\tBlue\tPurple\tOrange\tYellow");
    for (uint32 i = 0; i < MAX_ITEM_CLASS; ++i)
    {
        TC_LOG_DEBUG("ahbot", "AHBot: \t\t%u\t%u\t%u\t%u\t%u\t%u\t%u",
            config.GetMissedItemsPerClass(AUCTION_QUALITY_GRAY, (ItemClass)i),
            config.GetMissedItemsPerClass(AUCTION_QUALITY_WHITE, (ItemClass)i),
            config.GetMissedItemsPerClass(AUCTION_QUALITY_GREEN, (ItemClass)i),
            config.GetMissedItemsPerClass(AUCTION_QUALITY_BLUE, (ItemClass)i),
            config.GetMissedItemsPerClass(AUCTION_QUALITY_PURPLE, (ItemClass)i),
            config.GetMissedItemsPerClass(AUCTION_QUALITY_ORANGE, (ItemClass)i),
            config.GetMissedItemsPerClass(AUCTION_QUALITY_YELLOW, (ItemClass)i));
    }
    config.LastMissedItem = count;

    return count;
}

// getRandomArray is used to make viable the possibility to add any of missed item in place of first one to last one.
bool AuctionBotSeller::GetItemsToSell(SellerConfiguration& config, ItemsToSellArray& itemsToSellArray, AllItemsArray const& addedItem)
{
    itemsToSellArray.clear();
    bool found = false;

    for (uint32 j = 0; j < MAX_AUCTION_QUALITY; ++j)
    {
        for (uint32 i = 0; i < MAX_ITEM_CLASS; ++i)
        {
            // if _itemPool for chosen is empty, MissedItemsPerClass will return 0 here (checked at startup)
            if (config.GetMissedItemsPerClass(AuctionQuality(j), ItemClass(i)) > addedItem[j][i])
            {
                ItemToSell miss_item;
                miss_item.Color = j;
                miss_item.Itemclass = i;
                itemsToSellArray.emplace_back(std::move(miss_item));
                found = true;
            }
        }
    }

    return found;
}

// Set items price. All important value are passed by address.
void AuctionBotSeller::SetPricesOfItem(ItemTemplate const* itemProto, SellerConfiguration& config, uint32& buyp, uint32& bidp, uint32 stackCount)
{
    uint32 classRatio = config.GetPriceRatioPerClass(ItemClass(itemProto->GetClass()));
    uint32 qualityRatio = config.GetPriceRatioPerQuality(AuctionQuality(itemProto->GetQuality()));
    float priceRatio = (classRatio * qualityRatio) / 10000.0f;

    float buyPrice = itemProto->GetBuyPrice();
    float sellPrice = itemProto->GetSellPrice();

    if (buyPrice == 0)
    {
        if (sellPrice > 0)
            buyPrice = sellPrice * GetSellModifier(itemProto);
        else
        {
            float divisor = ((itemProto->GetClass() == ITEM_CLASS_WEAPON || itemProto->GetClass() == ITEM_CLASS_ARMOR) ? 284.0f : 80.0f);
            float tempLevel = (itemProto->GetBaseItemLevel() == 0 ? 1.0f : itemProto->GetBaseItemLevel());
            float tempQuality = (itemProto->GetQuality() == 0 ? 1.0f : itemProto->GetQuality());

            buyPrice = tempLevel * tempQuality * static_cast<float>(GetBuyModifier(itemProto))* tempLevel / divisor;
        }
    }

    if (sellPrice == 0)
        sellPrice = (buyPrice > 10 ? buyPrice / GetSellModifier(itemProto) : buyPrice);

    if (sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BUYPRICE_SELLER))
        buyPrice = sellPrice;

    float basePriceFloat = buyPrice * stackCount / (itemProto->GetClass() == 6 ? 200.0f : static_cast<float>(itemProto->GetBuyCount()));
    basePriceFloat *= priceRatio;

    float range = basePriceFloat * 0.04f;

    buyp = static_cast<uint32>(frand(basePriceFloat - range, basePriceFloat + range) + 0.5f);
    if (buyp == 0)
        buyp = 1;

    float bidPercentage = frand(sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BIDPRICE_MIN), sAuctionBotConfig->GetConfig(CONFIG_AHBOT_BIDPRICE_MAX));
    bidp = static_cast<uint32>(bidPercentage * buyp);
    if (bidp == 0)
        bidp = 1;
}

// Determines the stack size to use for the item
uint32 AuctionBotSeller::GetStackSizeForItem(ItemTemplate const* itemProto, SellerConfiguration& config) const
{
    if (sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_PROFILE_ENABLED))
    {
        AuctionBotMarketSegment segment = GetMarketSegment(itemProto);
        if (segment == AuctionBotMarketSegment::Equipment)
            return 1;

        if (segment == AuctionBotMarketSegment::Commodity)
        {
            uint32 maxStackSize = itemProto->GetMaxStackSize();
            if (maxStackSize <= 1)
                return 1;

            uint32 singleWeight = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_COMMODITY_STACK_SINGLE_WEIGHT);
            uint32 quarterWeight = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_COMMODITY_STACK_QUARTER_WEIGHT);
            uint32 halfWeight = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_COMMODITY_STACK_HALF_WEIGHT);
            uint32 fullWeight = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_COMMODITY_STACK_FULL_WEIGHT);
            uint32 totalWeight = singleWeight + quarterWeight + halfWeight + fullWeight;

            if (totalWeight)
            {
                uint32 roll = urand(1, totalWeight);
                if (roll <= singleWeight)
                    return 1;

                roll -= singleWeight;
                if (roll <= quarterWeight)
                    return std::min(maxStackSize, std::max(2u, (maxStackSize + 3) / 4));

                roll -= quarterWeight;
                if (roll <= halfWeight)
                    return std::min(maxStackSize, std::max(2u, (maxStackSize + 1) / 2));

                return maxStackSize;
            }
        }
    }

    if (config.GetRandomStackRatioPerClass(ItemClass(itemProto->GetClass())) > urand(0, 99))
        return urand(1, itemProto->GetMaxStackSize());
    else
        return 1;
}

// Determine the multiplier for the sell price of any weapon without a buy price.
uint32 AuctionBotSeller::GetSellModifier(ItemTemplate const* prototype)
{
    switch (prototype->GetClass())
    {
        case ITEM_CLASS_WEAPON:
        case ITEM_CLASS_ARMOR:
        case ITEM_CLASS_REAGENT:
        case ITEM_CLASS_PROJECTILE:
            return 5;
        default:
            return 4;
    }
}

// Return the modifier by which the item's level and quality will be modified by to derive a relatively accurate price.
uint32 AuctionBotSeller::GetBuyModifier(ItemTemplate const* prototype)
{
    switch (prototype->GetClass())
    {
        case ITEM_CLASS_CONSUMABLE:
        {
            switch (prototype->GetSubClass())
            {
            case ITEM_SUBCLASS_CONSUMABLE:
                return 100;
            case ITEM_SUBCLASS_FLASK:
                return 400;
            case ITEM_SUBCLASS_SCROLL:
                return 15;
            case ITEM_SUBCLASS_ITEM_ENHANCEMENT:
                return 250;
            case ITEM_SUBCLASS_BANDAGE:
                return 125;
            default:
                return 300;
            }
        }
        case ITEM_CLASS_WEAPON:
        {
            switch (prototype->GetSubClass())
            {
                case ITEM_SUBCLASS_WEAPON_AXE:
                case ITEM_SUBCLASS_WEAPON_MACE:
                case ITEM_SUBCLASS_WEAPON_SWORD:
                case ITEM_SUBCLASS_WEAPON_FIST_WEAPON:
                case ITEM_SUBCLASS_WEAPON_DAGGER:
                    return 1200;
                case ITEM_SUBCLASS_WEAPON_AXE2:
                case ITEM_SUBCLASS_WEAPON_MACE2:
                case ITEM_SUBCLASS_WEAPON_POLEARM:
                case ITEM_SUBCLASS_WEAPON_SWORD2:
                case ITEM_SUBCLASS_WEAPON_STAFF:
                    return 1500;
                case ITEM_SUBCLASS_WEAPON_THROWN:
                    return 350;
                default:
                    return 1000;
            }
        }
        case ITEM_CLASS_ARMOR:
        {
            switch (prototype->GetSubClass())
            {
                case ITEM_SUBCLASS_ARMOR_MISCELLANEOUS:
                case ITEM_SUBCLASS_ARMOR_CLOTH:
                    return 500;
                case ITEM_SUBCLASS_ARMOR_LEATHER:
                    return 600;
                case ITEM_SUBCLASS_ARMOR_MAIL:
                    return 700;
                case ITEM_SUBCLASS_ARMOR_PLATE:
                case ITEM_SUBCLASS_ARMOR_SHIELD:
                    return 800;
                default:
                    return 400;
            }
        }
        case ITEM_CLASS_REAGENT:
        case ITEM_CLASS_PROJECTILE:
            return 50;
        case ITEM_CLASS_TRADE_GOODS:
        {
            switch (prototype->GetSubClass())
            {
                case ITEM_SUBCLASS_TRADE_GOODS:
                case ITEM_SUBCLASS_PARTS:
                case ITEM_SUBCLASS_MEAT:
                    return 50;
                case ITEM_SUBCLASS_EXPLOSIVES:
                    return 250;
                case ITEM_SUBCLASS_DEVICES:
                    return 500;
                case ITEM_SUBCLASS_ELEMENTAL:
                case ITEM_SUBCLASS_TRADE_GOODS_OTHER:
                case ITEM_SUBCLASS_ENCHANTING:
                    return 300;
                default:
                    return 100;
            }
        }
        case ITEM_CLASS_QUEST: return 1000;
        case ITEM_CLASS_KEY: return 3000;
        default:
            return 500;
    }
}

void AuctionBotSeller::SetItemsRatio(uint32 al, uint32 ho, uint32 ne)
{
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ALLIANCE_ITEM_AMOUNT_RATIO, std::min(al, 10000u));
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_HORDE_ITEM_AMOUNT_RATIO, std::min(ho, 10000u));
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_NEUTRAL_ITEM_AMOUNT_RATIO, std::min(ne, 10000u));

    for (int i = 0; i < MAX_AUCTION_HOUSE_TYPE; ++i)
        LoadItemsQuantity(_houseConfig[i]);
}

void AuctionBotSeller::SetItemsRatioForHouse(AuctionHouseType house, uint32 val)
{
    val = std::min(val, 10000u); // apply same upper limit as used for config load

    switch (house)
    {
        case AUCTION_HOUSE_ALLIANCE: sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ALLIANCE_ITEM_AMOUNT_RATIO, val); break;
        case AUCTION_HOUSE_HORDE:    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_HORDE_ITEM_AMOUNT_RATIO, val); break;
        default:                     sAuctionBotConfig->SetConfig(CONFIG_AHBOT_NEUTRAL_ITEM_AMOUNT_RATIO, val); break;
    }

    LoadItemsQuantity(_houseConfig[house]);
}

void AuctionBotSeller::SetItemsAmount(uint32(&vals)[MAX_AUCTION_QUALITY])
{
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_GRAY_AMOUNT, vals[AUCTION_QUALITY_GRAY]);
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_WHITE_AMOUNT, vals[AUCTION_QUALITY_WHITE]);
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_GREEN_AMOUNT, vals[AUCTION_QUALITY_GREEN]);
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_BLUE_AMOUNT, vals[AUCTION_QUALITY_BLUE]);
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_PURPLE_AMOUNT, vals[AUCTION_QUALITY_PURPLE]);
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_ORANGE_AMOUNT, vals[AUCTION_QUALITY_ORANGE]);
    sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_YELLOW_AMOUNT, vals[AUCTION_QUALITY_YELLOW]);

    for (int i = 0; i < MAX_AUCTION_HOUSE_TYPE; ++i)
        LoadItemsQuantity(_houseConfig[i]);
}

void AuctionBotSeller::SetItemsAmountForQuality(AuctionQuality quality, uint32 val)
{
    switch (quality)
    {
        case AUCTION_QUALITY_GRAY:
            sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_GRAY_AMOUNT, val); break;
        case AUCTION_QUALITY_WHITE:
            sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_WHITE_AMOUNT, val); break;
        case AUCTION_QUALITY_GREEN:
            sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_GREEN_AMOUNT, val); break;
        case AUCTION_QUALITY_BLUE:
            sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_BLUE_AMOUNT, val); break;
        case AUCTION_QUALITY_PURPLE:
            sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_PURPLE_AMOUNT, val); break;
        case AUCTION_QUALITY_ORANGE:
            sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_ORANGE_AMOUNT, val); break;
        default:
            sAuctionBotConfig->SetConfig(CONFIG_AHBOT_ITEM_YELLOW_AMOUNT, val); break;
    }

    for (int i = 0; i < MAX_AUCTION_HOUSE_TYPE; ++i)
        LoadItemsQuantity(_houseConfig[i]);
}

// Add new auction to one of the factions.
// Faction and setting associated is defined passed argument ( config )
void AuctionBotSeller::AddNewAuctions(SellerConfiguration& config)
{
    uint32 count = 0;
    uint32 items = 0;

    // If there is large amount of items missed we can use boost value to get fast filled AH
    if (config.LastMissedItem > sAuctionBotConfig->GetItemPerCycleBoost())
    {
        items = sAuctionBotConfig->GetItemPerCycleBoost();
        TC_LOG_DEBUG("ahbot", "AHBot: Boost value used to fill AH! (if this happens often adjust both ItemsPerCycle in worldserver.conf)");
    }
    else
        items = sAuctionBotConfig->GetItemPerCycleNormal();

    uint32 houseid = 0;
    switch (config.GetHouseType())
    {
        case AUCTION_HOUSE_ALLIANCE:
            houseid = AUCTIONHOUSE_ALLIANCE;
            break;
        case AUCTION_HOUSE_HORDE:
            houseid = AUCTIONHOUSE_HORDE;
            break;
        default:
            houseid = AUCTIONHOUSE_NEUTRAL;
            break;
    }

    AuctionHouseEntry const* ahEntry = sAuctionHouseStore.LookupEntry(houseid);

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config.GetHouseType());

    std::unordered_map<uint32, uint32> botListingsPerItem;
    for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        AuctionEntry const* auctionEntry = itr->second;
        if (!auctionEntry->owner || sAuctionBotConfig->IsBotChar(auctionEntry->owner))
            ++botListingsPerItem[auctionEntry->itemEntry];
    }

    ItemPool availableItemPool[MAX_AUCTION_QUALITY][MAX_ITEM_CLASS];
    bool const supplyProfileEnabled = sAuctionBotConfig->GetConfig(CONFIG_AHBOT_SUPPLY_PROFILE_ENABLED);
    for (uint32 quality = 0; quality < MAX_AUCTION_QUALITY; ++quality)
    {
        for (uint32 itemClass = 0; itemClass < MAX_ITEM_CLASS; ++itemClass)
        {
            for (uint32 itemId : _itemPool[quality][itemClass])
            {
                ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemId);
                if (!prototype)
                    continue;

                uint32 ceiling = supplyProfileEnabled ? GetMarketSegmentCeiling(prototype) : 0;
                if (!ceiling || botListingsPerItem[itemId] < ceiling)
                    availableItemPool[quality][itemClass].push_back(itemId);
            }
        }
    }

    ItemsToSellArray itemsToSell;
    AllItemsArray allItems(MAX_AUCTION_QUALITY, std::vector<uint32>(MAX_ITEM_CLASS));
    // Main loop
    // getRandomArray will give what categories of items should be added (return true if there is at least 1 items missed)
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    while (GetItemsToSell(config, itemsToSell, allItems) && items > 0)
    {
        itemsToSell.erase(std::remove_if(itemsToSell.begin(), itemsToSell.end(), [&](ItemToSell const& itemToSell)
        {
            return availableItemPool[itemToSell.Color][itemToSell.Itemclass].empty();
        }), itemsToSell.end());

        if (itemsToSell.empty())
        {
            TC_LOG_WARN("ahbot", "AHBot: Supply profile ceilings left no eligible items for the remaining listing targets in house %u.", houseid);
            break;
        }

        --items;

        // Select random position from missed items table
        ItemToSell const& sellItem = Trinity::Containers::SelectRandomContainerElement(itemsToSell);

        // Set itemId with random item ID for selected categories and color, from _itemPool table
        ItemPool& availableItems = availableItemPool[sellItem.Color][sellItem.Itemclass];
        uint32 selectedIndex = urand(0, uint32(availableItems.size() - 1));
        uint32 itemId = availableItems[selectedIndex];
        ++allItems[sellItem.Color][sellItem.Itemclass]; // Helper table to avoid rescan from DB in this loop. (has we add item in random orders)

        if (!itemId)
        {
            TC_LOG_DEBUG("ahbot", "AHBot: Item entry 0 auction creating attempt.");
            continue;
        }

        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemId);
        if (!prototype)
        {
            TC_LOG_DEBUG("ahbot", "AHBot: Unknown item %u auction creating attempt.", itemId);
            continue;
        }

        uint32 stackCount = GetStackSizeForItem(prototype, config);

        Item* item = Item::CreateItem(itemId, stackCount);
        if (!item)
        {
            TC_LOG_ERROR("ahbot", "AHBot: Item::CreateItem() returned nullptr for item %u (stack: %u)", itemId, stackCount);
            return;
        }

        // Update the just created item so that if it needs random properties it has them.
        // Ex:  Notched Shortsword of Stamina will only generate as a Notched Shortsword without this.
        item->SetItemRandomProperties(GenerateItemRandomPropertyId(itemId));

        uint32 buyoutPrice;
        uint32 bidPrice = 0;

        // Price of items are set here
        SetPricesOfItem(prototype, config, buyoutPrice, bidPrice, stackCount);

        // Deposit time
        uint32 etime = urand(1, 3);
        switch (etime)
        {
            case 1:
                etime = DAY / 2;
                break;
            case 3:
                etime = 2 *DAY;
                break;
            case 2:
            default:
                etime = DAY;
                break;
        }

        AuctionEntry* auctionEntry = new AuctionEntry();
        auctionEntry->Id = sObjectMgr->GenerateAuctionID();
        auctionEntry->owner = sAuctionBotConfig->GetRandChar();
        auctionEntry->itemGUIDLow = item->GetGUID().GetCounter();
        auctionEntry->itemEntry = item->GetEntry();
        auctionEntry->startbid = bidPrice;
        auctionEntry->buyout = buyoutPrice;
        auctionEntry->houseId = houseid;
        auctionEntry->bidder = 0;
        auctionEntry->bid = 0;
        auctionEntry->deposit = sAuctionMgr->GetAuctionDeposit(ahEntry, etime, item, stackCount);
        auctionEntry->auctionHouseEntry = ahEntry;
        auctionEntry->expire_time = GameTime::GetGameTime() + urand(config.GetMinTime(), config.GetMaxTime()) * HOUR;

        item->SaveToDB(trans);
        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(auctionEntry);
        auctionEntry->SaveToDB(trans);

        uint32 itemListingCount = ++botListingsPerItem[itemId];
        uint32 itemListingCeiling = supplyProfileEnabled ? GetMarketSegmentCeiling(prototype) : 0;
        if (itemListingCeiling && itemListingCount >= itemListingCeiling)
        {
            availableItems[selectedIndex] = availableItems.back();
            availableItems.pop_back();
        }

        ++count;
    }
    CharacterDatabase.CommitTransaction(trans);

    TC_LOG_DEBUG("ahbot", "AHBot: Added %u items to auction", count);
}

bool AuctionBotSeller::Update(AuctionHouseType houseType)
{
    if (sAuctionBotConfig->GetConfigItemAmountRatio(houseType) > 0)
    {
        TC_LOG_DEBUG("ahbot", "AHBot: %s selling ...", AuctionBotConfig::GetHouseTypeName(houseType));
        if (SetStat(_houseConfig[houseType]))
            AddNewAuctions(_houseConfig[houseType]);
        return true;
    }
    else
        return false;
}
