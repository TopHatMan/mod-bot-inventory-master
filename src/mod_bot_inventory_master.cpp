// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * AzerothCore module: Bot Inventory Master
 *
 * Goal:
 *   Let a player manage the inventory of online playerbot/alt characters that
 *   belong to the same account, without touching other accounts unless an
 *   explicit account-linking system is added later.
 *
 * Current commands:
 *   .botinv
 *   .botinv bots
 *   .botinv target bags
 *   .botinv target equipment
 *   .botinv target sell gray confirm
 *   .botinv target sell <bag> <slot> [confirm]
 *   .botinv target buyback list
 *   .botinv target buyback <id>
 *   .botinv target equipbag <bag> <slot>
 *   .botinv target destroy <bag> <slot> [confirm]
 *   .botinv target deposit reagents
 *   .botinv party deposit reagents
 *   .botinv bank
 *
 * Addon protocol:
 *   BOTINV:* system lines are intentionally simple to parse from a 3.3.5 addon.
 */

#include "Bag.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Unit.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;

namespace BotInventoryMaster
{
    static bool g_enabled = true;
    static bool g_requireSameAccount = true;
    static bool g_gmBypass = true;
    static bool g_requireTargetIsBot = false;
    static bool g_depositEnabled = true;
    static bool g_storageReady = false;
    static uint32 g_maxBankListRows = 80;
    static float g_requiredTradeDistance = 12.0f;
    static bool g_allowSellSelected = true;
    static bool g_allowBuyback = true;
    static uint32 g_buybackMaxRecordsPerBot = 12;

    // Bulk cleanup intentionally has stricter guardrails than the old single-item danger mode.
    // The addon can select many white/green trash items quickly without risking rare/epic gear,
    // quest items, bags, Hearthstones, or class totems in one accidental click.
    static uint32 g_bulkMaxQuality = ITEM_QUALITY_UNCOMMON;
    static uint32 g_bulkMaxItemsPerCommand = 24;

    // Danger mode is intentionally permissive because this is a single-player/bot
    // management tool. The addon can send actions directly with no warning popups.
    static bool g_dangerAllowAnySell = true;
    static bool g_dangerAllowAnyDestroy = true;
    static bool g_dangerRequireConfirm = false;

    static std::map<ObjectGuid::LowType, ObjectGuid> g_selectedVendorByManager;
    static std::map<ObjectGuid::LowType, ObjectGuid> g_selectedBotByManager;

    static constexpr uint32 MAX_STORED_AMOUNT = std::numeric_limits<uint32>::max();

    struct BuybackRecord
    {
        uint32 Id = 0;
        uint32 Entry = 0;
        uint32 Count = 0;
        uint32 Quality = 0;
        uint64 Cost = 0;
        std::string Name;
    };

    static uint32 g_nextBuybackId = 1;
    static std::map<ObjectGuid::LowType, std::vector<BuybackRecord>> g_buybackByBot;

    struct CategoryInfo
    {
        uint32 SubClass;
        char const* Name;
        uint32 SampleItem;
    };

    // Same general reagent buckets as the reference reagent-bank module, but
    // this module owns its own storage and commands.
    static constexpr std::array<CategoryInfo, 15> Categories =
    { {
        { ITEM_SUBCLASS_CLOTH,              "Cloth",             2589  },
        { ITEM_SUBCLASS_MEAT,               "Meat",              12208 },
        { ITEM_SUBCLASS_METAL_STONE,        "Metal & Stone",     2772  },
        { ITEM_SUBCLASS_ENCHANTING,         "Enchanting",        10940 },
        { ITEM_SUBCLASS_ELEMENTAL,          "Elemental",         7068  },
        { ITEM_SUBCLASS_PARTS,              "Parts",             4359  },
        { ITEM_SUBCLASS_TRADE_GOODS_OTHER,  "Other Trade Goods", 2604  },
        { ITEM_SUBCLASS_HERB,               "Herb",              2453  },
        { ITEM_SUBCLASS_LEATHER,            "Leather",           2318  },
        { ITEM_SUBCLASS_JEWELCRAFTING,      "Jewelcrafting",     1206  },
        { ITEM_SUBCLASS_EXPLOSIVES,         "Explosives",        4358  },
        { ITEM_SUBCLASS_DEVICES,            "Devices",           4388  },
        { ITEM_SUBCLASS_MATERIAL,           "Nether Material",   23572 },
        { ITEM_SUBCLASS_ARMOR_ENCHANTMENT,  "Armor Vellum",      38682 },
        { ITEM_SUBCLASS_WEAPON_ENCHANTMENT, "Weapon Vellum",     39349 }
    } };

    using ItemAmountMap = std::map<uint32, uint32>;

    struct CleanupStats
    {
        uint32 Items = 0;
        uint32 Stacks = 0;
        uint32 FailedStacks = 0;
        uint64 Copper = 0;
    };

    struct BulkCleanupStats
    {
        uint32 Items = 0;
        uint32 Stacks = 0;
        uint32 SkippedStacks = 0;
        uint32 FailedStacks = 0;
        uint64 Copper = 0;
    };

    struct BagSlotRef
    {
        uint8 Bag = 0;
        uint8 Slot = 0;
    };

    struct OwnedBagSlotRef
    {
        std::string BotName;
        uint8 Bag = 0;
        uint8 Slot = 0;
    };


    static std::string Sanitize(std::string text)
    {
        for (char& c : text)
        {
            if (c == '\r' || c == '\n' || c == ':' || c == ';')
                c = ' ';
        }

        return text;
    }

    static std::string ToLower(std::string value)
    {
        for (char& c : value)
            c = char(std::tolower(static_cast<unsigned char>(c)));

        return value;
    }

    static std::vector<std::string> Tokenize(char const* args)
    {
        std::vector<std::string> tokens;

        if (!args)
            return tokens;

        std::istringstream stream(args);
        std::string token;
        while (stream >> token)
            tokens.push_back(token);

        return tokens;
    }

    static bool TryParseUInt32(std::string const& text, uint32& value)
    {
        value = 0;

        if (text.empty())
            return false;

        for (char c : text)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;

        errno = 0;
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != '\0' || parsed > std::numeric_limits<uint32>::max())
            return false;

        value = uint32(parsed);
        return true;
    }

    static bool AddAmountChecked(uint32 current, uint32 add, uint32& out)
    {
        if (add > MAX_STORED_AMOUNT - current)
            return false;

        out = current + add;
        return true;
    }

    static bool AddToItemAmountMapChecked(ItemAmountMap& map, uint32 itemEntry, uint32 amount)
    {
        if (!itemEntry || !amount)
            return false;

        uint32 updated = 0;
        if (!AddAmountChecked(map[itemEntry], amount, updated))
            return false;

        map[itemEntry] = updated;
        return true;
    }

    static uint64 SumItemAmountMap(ItemAmountMap const& items)
    {
        uint64 total = 0;
        for (std::pair<uint32 const, uint32> const& pair : items)
            total += pair.second;

        return total;
    }

    static bool IsCategory(uint32 itemSubclass)
    {
        for (CategoryInfo const& category : Categories)
            if (category.SubClass == itemSubclass)
                return true;

        return false;
    }

    static char const* CategoryName(uint32 itemSubclass)
    {
        for (CategoryInfo const& category : Categories)
            if (category.SubClass == itemSubclass)
                return category.Name;

        return "Unknown";
    }

    static bool IsStorableReagent(ItemTemplate const* proto, uint32& itemEntry, uint32& itemSubclass)
    {
        if (!proto)
            return false;

        if (!(proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_GEM))
            return false;

        if (proto->GetMaxStackSize() <= 1)
            return false;

        itemEntry = proto->ItemId;
        itemSubclass = proto->Class == ITEM_CLASS_GEM ? ITEM_SUBCLASS_JEWELCRAFTING : proto->SubClass;

        return IsCategory(itemSubclass);
    }

    static void SendProtocol(ChatHandler* handler, std::string const& line)
    {
        if (handler)
            handler->SendSysMessage(line.c_str());
    }

    static void SendOk(ChatHandler* handler, std::string const& message)
    {
        SendProtocol(handler, Acore::StringFormat("BOTINV:OK:{}", Sanitize(message)));
    }

    static void SendError(ChatHandler* handler, std::string const& message)
    {
        SendProtocol(handler, Acore::StringFormat("BOTINV:ERR:{}", Sanitize(message)));
    }

    static void LoadConfig()
    {
        g_enabled = sConfigMgr->GetOption<bool>("BotInventoryMaster.Enable", true);
        g_requireSameAccount = sConfigMgr->GetOption<bool>("BotInventoryMaster.RequireSameAccount", true);
        g_gmBypass = sConfigMgr->GetOption<bool>("BotInventoryMaster.GMBypass", true);
        g_requireTargetIsBot = sConfigMgr->GetOption<bool>("BotInventoryMaster.RequireTargetIsBot", false);
        g_depositEnabled = sConfigMgr->GetOption<bool>("BotInventoryMaster.DepositReagents.Enable", true);
        g_allowSellSelected = sConfigMgr->GetOption<bool>("BotInventoryMaster.Vendor.AllowSellSelected", true);
        g_allowBuyback = sConfigMgr->GetOption<bool>("BotInventoryMaster.Vendor.AllowBuyback", true);
        g_buybackMaxRecordsPerBot = sConfigMgr->GetOption<uint32>("BotInventoryMaster.Vendor.BuybackMaxRecordsPerBot", 12);
        g_bulkMaxQuality = sConfigMgr->GetOption<uint32>("BotInventoryMaster.Bulk.MaxQuality", uint32(ITEM_QUALITY_UNCOMMON));
        g_bulkMaxItemsPerCommand = sConfigMgr->GetOption<uint32>("BotInventoryMaster.Bulk.MaxItemsPerCommand", 24);

        g_dangerAllowAnySell = sConfigMgr->GetOption<bool>("BotInventoryMaster.Danger.AllowAnySell", true);
        g_dangerAllowAnyDestroy = sConfigMgr->GetOption<bool>("BotInventoryMaster.Danger.AllowAnyDestroy", true);
        g_dangerRequireConfirm = sConfigMgr->GetOption<bool>("BotInventoryMaster.Danger.RequireConfirm", false);

        g_maxBankListRows = sConfigMgr->GetOption<uint32>("BotInventoryMaster.MaxBankListRows", 80);
        g_requiredTradeDistance = sConfigMgr->GetOption<float>("BotInventoryMaster.RequiredTradeDistance", 12.0f);

        if (g_buybackMaxRecordsPerBot < 1)
            g_buybackMaxRecordsPerBot = 1;
        if (g_buybackMaxRecordsPerBot > 40)
            g_buybackMaxRecordsPerBot = 40;

        if (g_bulkMaxQuality > ITEM_QUALITY_EPIC)
            g_bulkMaxQuality = ITEM_QUALITY_EPIC;
        if (g_bulkMaxItemsPerCommand < 1)
            g_bulkMaxItemsPerCommand = 1;
        if (g_bulkMaxItemsPerCommand > 24)
            g_bulkMaxItemsPerCommand = 24;

        if (g_requiredTradeDistance < 1.0f)
            g_requiredTradeDistance = 1.0f;
        if (g_requiredTradeDistance > 50.0f)
            g_requiredTradeDistance = 50.0f;

        if (g_maxBankListRows == 0)
            g_maxBankListRows = 80;
        if (g_maxBankListRows > 250)
            g_maxBankListRows = 250;
    }

    static void EnsureTables()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_bot_inventory_master_bank` ("
            "`account_id` INT UNSIGNED NOT NULL,"
            "`item_entry` INT UNSIGNED NOT NULL,"
            "`item_subclass` INT UNSIGNED NOT NULL,"
            "`amount` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`account_id`, `item_entry`),"
            "KEY `idx_mod_botinv_bank_subclass` (`account_id`, `item_subclass`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_bot_inventory_master_account_link` ("
            "`owner_account_id` INT UNSIGNED NOT NULL,"
            "`linked_account_id` INT UNSIGNED NOT NULL,"
            "`can_inventory` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`can_equipment` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`accepted` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`owner_account_id`, `linked_account_id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        WorldDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_bot_inventory_master_deposit_exclusion` ("
            "`item_entry` INT UNSIGNED NOT NULL,"
            "`comment` VARCHAR(255) NULL DEFAULT NULL,"
            "PRIMARY KEY (`item_entry`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    static bool IsDepositExcluded(uint32 itemEntry)
    {
        if (!itemEntry)
            return true;

        QueryResult result = WorldDatabase.Query(
            "SELECT 1 FROM `mod_bot_inventory_master_deposit_exclusion` WHERE `item_entry` = {} LIMIT 1",
            itemEntry);

        return bool(result);
    }

    static bool AccountsLinked(uint32 ownerAccount, uint32 linkedAccount)
    {
        if (!ownerAccount || !linkedAccount)
            return false;

        QueryResult result = CharacterDatabase.Query(
            "SELECT 1 FROM `mod_bot_inventory_master_account_link` "
            "WHERE `owner_account_id` = {} AND `linked_account_id` = {} "
            "AND `can_inventory` = 1 AND `accepted` = 1 LIMIT 1",
            ownerAccount, linkedAccount);

        return bool(result);
    }

    static bool HasGmBypass(Player* manager)
    {
        if (!manager || !manager->GetSession())
            return false;

        return g_gmBypass && manager->GetSession()->GetSecurity() > SEC_PLAYER;
    }

    static bool CanManageTarget(Player* manager, Player* target, std::string& reason)
    {
        reason.clear();

        if (!manager || !target)
        {
            reason = "No valid manager or target.";
            return false;
        }

        if (manager == target)
        {
            reason = "Target is yourself; this module is for managing bot/alt inventory.";
            return false;
        }

        WorldSession* managerSession = manager->GetSession();
        WorldSession* targetSession = target->GetSession();
        if (!managerSession || !targetSession)
        {
            reason = "Target has no active session.";
            return false;
        }

        if (g_requireTargetIsBot && !targetSession->IsBot())
        {
            reason = "Target is not a playerbot session.";
            return false;
        }

        if (HasGmBypass(manager))
            return true;

        uint32 const managerAccount = managerSession->GetAccountId();
        uint32 const targetAccount = targetSession->GetAccountId();

        if (g_requireSameAccount && managerAccount == targetAccount)
            return true;

        if (AccountsLinked(managerAccount, targetAccount))
            return true;

        reason = "Target is not on your account and has not linked inventory permission.";
        return false;
    }

    static void RememberSelectedBot(Player* manager, Player* target)
    {
        if (!manager || !target)
            return;

        g_selectedBotByManager[manager->GetGUID().GetCounter()] = target->GetGUID();
    }

    static Player* GetRememberedBot(Player* manager)
    {
        if (!manager)
            return nullptr;

        auto itr = g_selectedBotByManager.find(manager->GetGUID().GetCounter());
        if (itr == g_selectedBotByManager.end() || itr->second.IsEmpty())
            return nullptr;

        Player* remembered = ObjectAccessor::FindPlayer(itr->second);
        if (!remembered || !remembered->IsInWorld())
            return nullptr;

        return remembered;
    }

    static Player* GetSelectedPlayerBot(ChatHandler* handler, Player* manager)
    {
        if (!handler || !manager)
            return nullptr;

        Unit* selected = manager->GetSelectedUnit();
        if (selected)
        {
            if (Player* selectedPlayer = selected->ToPlayer())
            {
                RememberSelectedBot(manager, selectedPlayer);
                return selectedPlayer;
            }
        }

        // Important UI behavior:
        // After the addon scans a bot, it remembers that bot server-side.
        // That lets the player target a vendor, run .botinv vendor set, then still
        // sell/refresh the remembered bot without retargeting it.
        return GetRememberedBot(manager);
    }

    static uint32 CountFreeBagSlots(Player* player)
    {
        if (!player)
            return 0;

        uint32 freeSlots = 0;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++freeSlots;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = player->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (!bag->GetItemByPos(uint8(slot)))
                    ++freeSlots;
        }

        return freeSlots;
    }

    static void SendBotSummary(ChatHandler* handler, Player* manager, Player* target)
    {
        if (!handler || !manager || !target)
            return;

        std::string reason;
        bool manageable = CanManageTarget(manager, target, reason);
        uint32 accountId = target->GetSession() ? target->GetSession()->GetAccountId() : 0;

        SendProtocol(handler, Acore::StringFormat(
            "BOTINV:BOT:{}:{}:{}:{}:{}:{}:{}",
            Sanitize(target->GetName()),
            uint32(target->GetGUID().GetCounter()),
            accountId,
            uint32(target->GetLevel()),
            uint32(target->getClass()),
            CountFreeBagSlots(target),
            manageable ? 1 : 0));
    }

    static void SendItemRecord(ChatHandler* handler, char const* prefix, Player* owner, uint32 slotA, uint32 slotB, Item* item)
    {
        if (!handler || !prefix || !owner || !item)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return;

        // Keep the original first fields stable for older addon builds, then append richer
        // metadata used by the bulk-selection UI (class/subclass/inventory type).
        SendProtocol(handler, Acore::StringFormat(
            "{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            prefix,
            Sanitize(owner->GetName()),
            slotA,
            slotB,
            proto->ItemId,
            item->GetCount(),
            uint32(proto->Quality),
            proto->SellPrice,
            Sanitize(proto->Name1),
            uint32(proto->Class),
            uint32(proto->SubClass),
            uint32(proto->InventoryType)));
    }

    static void AddBuybackRecord(Player* target, uint32 entry, uint32 count, uint32 quality, uint64 cost, std::string const& name)
    {
        if (!target || !entry || !count || !g_allowBuyback)
            return;

        BuybackRecord record;
        record.Id = g_nextBuybackId++;
        if (!g_nextBuybackId)
            g_nextBuybackId = 1;

        record.Entry = entry;
        record.Count = count;
        record.Quality = quality;
        record.Cost = cost;
        record.Name = Sanitize(name);

        std::vector<BuybackRecord>& records = g_buybackByBot[target->GetGUID().GetCounter()];
        records.push_back(record);

        while (records.size() > g_buybackMaxRecordsPerBot)
            records.erase(records.begin());
    }

    static BuybackRecord* FindBuybackRecord(Player* target, uint32 buybackId)
    {
        if (!target || !buybackId)
            return nullptr;

        auto itr = g_buybackByBot.find(target->GetGUID().GetCounter());
        if (itr == g_buybackByBot.end())
            return nullptr;

        for (BuybackRecord& record : itr->second)
            if (record.Id == buybackId)
                return &record;

        return nullptr;
    }

    static void RemoveBuybackRecord(Player* target, uint32 buybackId)
    {
        if (!target || !buybackId)
            return;

        auto itr = g_buybackByBot.find(target->GetGUID().GetCounter());
        if (itr == g_buybackByBot.end())
            return;

        std::vector<BuybackRecord>& records = itr->second;
        records.erase(std::remove_if(records.begin(), records.end(), [buybackId](BuybackRecord const& r)
        {
            return r.Id == buybackId;
        }), records.end());
    }

    static void SendBuybackList(ChatHandler* handler, Player* manager, Player* target)
    {
        if (!handler || !manager || !target)
            return;

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return;
        }

        RememberSelectedBot(manager, target);

        SendProtocol(handler, Acore::StringFormat("BOTINV:BUYBACK:BEGIN:{}:{}", Sanitize(target->GetName()), target->GetMoney()));

        auto itr = g_buybackByBot.find(target->GetGUID().GetCounter());
        if (itr != g_buybackByBot.end())
        {
            for (BuybackRecord const& record : itr->second)
            {
                SendProtocol(handler, Acore::StringFormat("BOTINV:BUYBACK:ITEM:{}:{}:{}:{}:{}:{}:{}",
                    Sanitize(target->GetName()),
                    record.Id,
                    record.Entry,
                    record.Count,
                    record.Quality,
                    record.Cost,
                    Sanitize(record.Name)));
            }
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:BUYBACK:END:{}", Sanitize(target->GetName())));
    }

    static bool IsSellProtected(Item* item, std::string& reason)
    {
        reason.clear();

        if (!item || !item->GetTemplate())
        {
            reason = "No valid item.";
            return true;
        }

        ItemTemplate const* proto = item->GetTemplate();

        if (g_dangerAllowAnySell)
            return false;

        if (proto->Class == ITEM_CLASS_QUEST)
        {
            reason = "Quest items are protected from vendor selling.";
            return true;
        }

        if (proto->Class == ITEM_CLASS_CONTAINER)
        {
            reason = "Bags/containers are protected from vendor selling. Use Equip Bag instead.";
            return true;
        }

        if (!proto->SellPrice)
        {
            reason = "That item has no vendor sell price.";
            return true;
        }

        return false;
    }

    static bool IsDestroyProtected(ItemTemplate const* proto, std::string& reason)
    {
        reason.clear();

        if (!proto)
        {
            reason = "Missing item template.";
            return true;
        }

        if (g_dangerAllowAnyDestroy)
            return false;

        if (proto->Class == ITEM_CLASS_QUEST)
        {
            reason = "Quest items are protected.";
            return true;
        }

        if (proto->Class == ITEM_CLASS_CONTAINER)
        {
            reason = "Bags/containers are protected from destroy.";
            return true;
        }

        if (proto->Quality != ITEM_QUALITY_POOR)
        {
            reason = "Only gray-quality items can be destroyed by this first safety pass.";
            return true;
        }

        return false;
    }



    // Forward declarations for vendor helpers implemented later in this file.
    // Bulk cleanup lives near the item/protocol helpers but reuses the established vendor checks.
    static bool IsTradeDistanceOk(Player* a, WorldObject* b);
    static Creature* GetSelectedVendor(ChatHandler* handler, Player* manager, std::string& reason);
    static void SendBags(ChatHandler* handler, Player* manager, Player* target);

    static bool IsHardBulkProtectedEntry(uint32 entry)
    {
        switch (entry)
        {
        case 6948:  // Hearthstone
        case 5175:  // Earth Totem
        case 5176:  // Fire Totem
        case 5177:  // Water Totem
        case 5178:  // Air Totem
        case 46978: // Totem of the Earthen Ring
            return true;
        default:
            return false;
        }
    }

    static bool IsBulkProtected(Item* item, bool selling, std::string& reason)
    {
        reason.clear();
        if (!item || !item->GetTemplate())
        {
            reason = "missing item data";
            return true;
        }

        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class == ITEM_CLASS_QUEST)
        {
            reason = "quest item";
            return true;
        }
        if (proto->Class == ITEM_CLASS_CONTAINER)
        {
            reason = "bag/container";
            return true;
        }
        if (IsHardBulkProtectedEntry(proto->ItemId))
        {
            reason = "hard-protected utility item";
            return true;
        }
        if (proto->HasFlag(ITEM_FLAG_NO_USER_DESTROY))
        {
            reason = "item cannot normally be destroyed by a player";
            return true;
        }
        if ((proto->BagFamily & BAG_FAMILY_MASK_KEYS) || (proto->BagFamily & BAG_FAMILY_MASK_QUEST_ITEMS))
        {
            reason = "key/quest-family utility item";
            return true;
        }
        if (proto->TotemCategory != 0)
        {
            reason = "class/profession tool or totem-category item";
            return true;
        }
        if (uint32(proto->Quality) > g_bulkMaxQuality)
        {
            reason = "quality above bulk cleanup limit";
            return true;
        }
        if (selling && !proto->SellPrice)
        {
            reason = "no vendor sell price";
            return true;
        }
        return false;
    }

    static bool ParseBagSlotList(std::string const& text, std::vector<BagSlotRef>& refs, std::string& reason)
    {
        refs.clear();
        reason.clear();
        if (text.empty())
        {
            reason = "No bag slots were supplied.";
            return false;
        }

        std::set<uint16> seen;
        std::istringstream stream(text);
        std::string token;
        while (std::getline(stream, token, ';'))
        {
            if (token.empty())
                continue;
            size_t comma = token.find(',');
            if (comma == std::string::npos || token.find(',', comma + 1) != std::string::npos)
            {
                reason = "Bulk slot list must use bag,slot;bag,slot format.";
                return false;
            }

            uint32 bag = 0;
            uint32 slot = 0;
            if (!TryParseUInt32(token.substr(0, comma), bag) ||
                !TryParseUInt32(token.substr(comma + 1), slot) ||
                bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
            {
                reason = "Bulk slot list contains an invalid bag or slot.";
                return false;
            }

            uint16 key = uint16((bag << 8) | slot);
            if (!seen.insert(key).second)
                continue;

            refs.push_back({ uint8(bag), uint8(slot) });
            if (refs.size() > g_bulkMaxItemsPerCommand)
            {
                reason = Acore::StringFormat("Bulk command exceeds the configured {}-stack limit.", g_bulkMaxItemsPerCommand);
                return false;
            }
        }

        if (refs.empty())
        {
            reason = "No valid bag slots were supplied.";
            return false;
        }
        return true;
    }

    static bool IsValidBulkBagPosition(Player* target, BagSlotRef const& ref)
    {
        if (!target)
            return false;

        // Backpack storage slots only. Explicitly reject equipment, bank, keyring, buyback, etc.
        if (ref.Bag == INVENTORY_SLOT_BAG_0)
            return ref.Slot >= INVENTORY_SLOT_ITEM_START && ref.Slot < INVENTORY_SLOT_ITEM_END;

        // Contents of equipped inventory bags only. The bag item itself is never a bulk target.
        if (ref.Bag >= INVENTORY_SLOT_BAG_START && ref.Bag < INVENTORY_SLOT_BAG_END)
        {
            Bag* bag = target->GetBagByPos(ref.Bag);
            return bag && ref.Slot < bag->GetBagSize();
        }

        return false;
    }

    static void RemoveBulkItem(Player* target, BagSlotRef const& ref, bool selling, BulkCleanupStats& stats)
    {
        if (!target)
            return;
        if (!IsValidBulkBagPosition(target, ref))
        {
            ++stats.SkippedStacks;
            return;
        }

        Item* item = target->GetItemByPos(ref.Bag, ref.Slot);
        if (!item)
        {
            ++stats.SkippedStacks;
            return;
        }

        std::string reason;
        if (IsBulkProtected(item, selling, reason))
        {
            ++stats.SkippedStacks;
            return;
        }

        ItemTemplate const* proto = item->GetTemplate();
        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();
        uint32 const quality = proto ? uint32(proto->Quality) : 0;
        uint64 const stackCopper = selling && proto ? uint64(proto->SellPrice) * uint64(count) : 0;
        std::string const name = proto ? Sanitize(proto->Name1) : "";
        if (!count)
        {
            ++stats.SkippedStacks;
            return;
        }

        target->DestroyItem(ref.Bag, ref.Slot, true);
        Item* after = target->GetItemByPos(ref.Bag, ref.Slot);
        if (after && after->GetEntry() == entry)
        {
            ++stats.FailedStacks;
            LOG_ERROR("module", "BotInventoryMaster: bulk {} verification failed for item {} x{} on {} bag {} slot {}.",
                selling ? "sell" : "destroy", entry, count, target->GetGUID().ToString(), uint32(ref.Bag), uint32(ref.Slot));
            return;
        }

        stats.Items += count;
        ++stats.Stacks;
        if (selling)
        {
            stats.Copper += stackCopper;
            uint64 recordCost = std::min<uint64>(stackCopper, uint64(std::numeric_limits<int32>::max()));
            AddBuybackRecord(target, entry, count, quality, recordCost, name);
        }
    }

    static bool HandleDestroyBatchTarget(ChatHandler* handler, Player* manager, std::string const& listText, bool confirm)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target, scan, or choose an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }
        if (!confirm)
        {
            SendError(handler, "Bulk destroy requires confirm.");
            return true;
        }

        std::vector<BagSlotRef> refs;
        if (!ParseBagSlotList(listText, refs, reason))
        {
            SendError(handler, reason);
            return true;
        }

        BulkCleanupStats stats;
        for (BagSlotRef const& ref : refs)
            RemoveBulkItem(target, ref, false, stats);

        SendProtocol(handler, Acore::StringFormat("BOTINV:BULK:DESTROY:{}:{}:{}:{}:{}",
            Sanitize(target->GetName()), stats.Items, stats.Stacks, stats.SkippedStacks, stats.FailedStacks));
        SendOk(handler, Acore::StringFormat("{} bulk-deleted {} item(s) in {} stack(s); {} skipped, {} failed verification.",
            Sanitize(target->GetName()), stats.Items, stats.Stacks, stats.SkippedStacks, stats.FailedStacks));
        SendBags(handler, manager, target);
        return true;
    }

    static bool HandleSellBatchTarget(ChatHandler* handler, Player* manager, std::string const& listText, bool confirm)
    {
        if (!g_allowSellSelected)
        {
            SendError(handler, "Selected item selling is disabled in config.");
            return true;
        }

        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target, scan, or choose an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }
        if (!confirm)
        {
            SendError(handler, "Bulk sell requires confirm.");
            return true;
        }

        Creature* vendor = GetSelectedVendor(handler, manager, reason);
        if (!vendor)
        {
            SendError(handler, reason);
            return true;
        }
        if (!IsTradeDistanceOk(target, vendor))
        {
            SendError(handler, Acore::StringFormat("{} is too far from the selected vendor.", Sanitize(target->GetName())));
            return true;
        }

        std::vector<BagSlotRef> refs;
        if (!ParseBagSlotList(listText, refs, reason))
        {
            SendError(handler, reason);
            return true;
        }

        BulkCleanupStats stats;
        for (BagSlotRef const& ref : refs)
            RemoveBulkItem(target, ref, true, stats);

        uint64 creditedCopper = std::min<uint64>(stats.Copper, uint64(std::numeric_limits<int32>::max()));
        if (creditedCopper)
            target->ModifyMoney(int32(creditedCopper));
        stats.Copper = creditedCopper;

        SendProtocol(handler, Acore::StringFormat("BOTINV:BULK:SELL:{}:{}:{}:{}:{}:{}",
            Sanitize(target->GetName()), stats.Items, stats.Stacks, stats.SkippedStacks, stats.FailedStacks, stats.Copper));
        SendOk(handler, Acore::StringFormat("{} bulk-sold {} item(s) in {} stack(s) for {} copper; {} skipped, {} failed verification.",
            Sanitize(target->GetName()), stats.Items, stats.Stacks, stats.Copper, stats.SkippedStacks, stats.FailedStacks));
        SendBags(handler, manager, target);
        return true;
    }

    static bool HandleUseBot(ChatHandler* handler, Player* manager, std::string const& botName)
    {
        if (!handler || !manager || botName.empty())
            return false;

        Player* target = ObjectAccessor::FindPlayerByName(botName);
        if (!target || !target->IsInWorld())
        {
            SendError(handler, Acore::StringFormat("Online bot/alt '{}' was not found.", Sanitize(botName)));
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        RememberSelectedBot(manager, target);
        SendOk(handler, Acore::StringFormat("Managing {}.", Sanitize(target->GetName())));
        SendBags(handler, manager, target);
        return true;
    }




    static bool ParseOwnedBagSlotList(std::string const& text, std::vector<OwnedBagSlotRef>& refs, std::string& reason)
    {
        refs.clear();
        reason.clear();
        if (text.empty())
        {
            reason = "No party bag slots were supplied.";
            return false;
        }

        std::set<std::string> seen;
        std::istringstream stream(text);
        std::string token;
        while (std::getline(stream, token, ';'))
        {
            if (token.empty())
                continue;

            size_t comma1 = token.find(',');
            size_t comma2 = comma1 == std::string::npos ? std::string::npos : token.find(',', comma1 + 1);
            if (comma1 == std::string::npos || comma2 == std::string::npos || token.find(',', comma2 + 1) != std::string::npos)
            {
                reason = "Party bulk list must use BotName,bag,slot;BotName,bag,slot format.";
                return false;
            }

            std::string botName = token.substr(0, comma1);
            uint32 bag = 0;
            uint32 slot = 0;
            if (botName.empty() || !TryParseUInt32(token.substr(comma1 + 1, comma2 - comma1 - 1), bag) ||
                !TryParseUInt32(token.substr(comma2 + 1), slot) ||
                bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
            {
                reason = "Party bulk list contains an invalid bot, bag, or slot.";
                return false;
            }

            std::string key = ToLower(botName) + ":" + std::to_string(bag) + ":" + std::to_string(slot);
            if (!seen.insert(key).second)
                continue;

            refs.push_back({ botName, uint8(bag), uint8(slot) });
            if (refs.size() > g_bulkMaxItemsPerCommand)
            {
                reason = Acore::StringFormat("Party bulk command exceeds the configured {}-stack limit.", g_bulkMaxItemsPerCommand);
                return false;
            }
        }

        if (refs.empty())
        {
            reason = "No valid party bag slots were supplied.";
            return false;
        }
        return true;
    }

    static Player* FindManagedPartyBot(Player* manager, std::string const& botName, std::string& reason)
    {
        reason.clear();
        if (!manager || !manager->GetGroup())
        {
            reason = "You are not in a group.";
            return nullptr;
        }

        Player* target = ObjectAccessor::FindPlayerByName(botName);
        if (!target || !target->IsInWorld() || target == manager || target->GetGroup() != manager->GetGroup())
        {
            reason = Acore::StringFormat("{} is not an online member of your group.", Sanitize(botName));
            return nullptr;
        }

        if (!CanManageTarget(manager, target, reason))
            return nullptr;
        return target;
    }

    static void SendPartyBags(ChatHandler* handler, Player* manager)
    {
        if (!handler || !manager)
            return;

        Group* group = manager->GetGroup();
        if (!group)
        {
            SendError(handler, "You are not in a group.");
            return;
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:PBAG:BEGIN:{}", g_bulkMaxQuality));
        uint32 botCount = 0;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == manager || !member->IsInWorld())
                continue;

            std::string reason;
            if (!CanManageTarget(manager, member, reason))
                continue;

            ++botCount;
            SendProtocol(handler, Acore::StringFormat("BOTINV:PBAG:BOT:{}:{}:{}",
                Sanitize(member->GetName()), CountFreeBagSlots(member), member->GetMoney()));

            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            {
                if (Item* item = member->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    SendItemRecord(handler, "BOTINV:PBAG:ITEM", member, uint32(INVENTORY_SLOT_BAG_0), uint32(slot), item);
            }

            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            {
                Bag* bag = member->GetBagByPos(bagSlot);
                if (!bag)
                    continue;
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        SendItemRecord(handler, "BOTINV:PBAG:ITEM", member, uint32(bagSlot), slot, item);
            }
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:PBAG:END:{}", botCount));
    }

    static bool HandlePartyDestroyBatch(ChatHandler* handler, Player* manager, std::string const& listText,
        bool confirm, bool refresh)
    {
        if (!manager || !manager->GetGroup())
        {
            SendError(handler, "You are not in a group.");
            return true;
        }
        if (!confirm)
        {
            SendError(handler, "Party bulk destroy requires confirm.");
            return true;
        }

        std::string reason;
        std::vector<OwnedBagSlotRef> refs;
        if (!ParseOwnedBagSlotList(listText, refs, reason))
        {
            SendError(handler, reason);
            return true;
        }

        std::map<std::string, std::vector<BagSlotRef>> byBot;
        for (OwnedBagSlotRef const& ref : refs)
            byBot[ref.BotName].push_back({ ref.Bag, ref.Slot });

        BulkCleanupStats total;
        for (auto const& pair : byBot)
        {
            Player* target = FindManagedPartyBot(manager, pair.first, reason);
            if (!target)
            {
                total.SkippedStacks += uint32(pair.second.size());
                continue;
            }

            BulkCleanupStats stats;
            for (BagSlotRef const& ref : pair.second)
                RemoveBulkItem(target, ref, false, stats);

            total.Items += stats.Items;
            total.Stacks += stats.Stacks;
            total.SkippedStacks += stats.SkippedStacks;
            total.FailedStacks += stats.FailedStacks;
            SendProtocol(handler, Acore::StringFormat("BOTINV:BULK:DESTROY:{}:{}:{}:{}:{}",
                Sanitize(target->GetName()), stats.Items, stats.Stacks, stats.SkippedStacks, stats.FailedStacks));
        }

        SendOk(handler, Acore::StringFormat("Party bulk-delete removed {} item(s) in {} stack(s); {} skipped, {} failed verification.",
            total.Items, total.Stacks, total.SkippedStacks, total.FailedStacks));
        if (refresh)
            SendPartyBags(handler, manager);
        return true;
    }

    static bool HandlePartySellBatch(ChatHandler* handler, Player* manager, std::string const& listText,
        bool confirm, bool refresh)
    {
        if (!g_allowSellSelected)
        {
            SendError(handler, "Selected item selling is disabled in config.");
            return true;
        }
        if (!manager || !manager->GetGroup())
        {
            SendError(handler, "You are not in a group.");
            return true;
        }
        if (!confirm)
        {
            SendError(handler, "Party bulk sell requires confirm.");
            return true;
        }

        std::string reason;
        Creature* vendor = GetSelectedVendor(handler, manager, reason);
        if (!vendor)
        {
            SendError(handler, reason);
            return true;
        }

        std::vector<OwnedBagSlotRef> refs;
        if (!ParseOwnedBagSlotList(listText, refs, reason))
        {
            SendError(handler, reason);
            return true;
        }

        std::map<std::string, std::vector<BagSlotRef>> byBot;
        for (OwnedBagSlotRef const& ref : refs)
            byBot[ref.BotName].push_back({ ref.Bag, ref.Slot });

        BulkCleanupStats total;
        for (auto const& pair : byBot)
        {
            Player* target = FindManagedPartyBot(manager, pair.first, reason);
            if (!target || !IsTradeDistanceOk(target, vendor))
            {
                total.SkippedStacks += uint32(pair.second.size());
                continue;
            }

            BulkCleanupStats stats;
            for (BagSlotRef const& ref : pair.second)
                RemoveBulkItem(target, ref, true, stats);

            uint64 creditedCopper = std::min<uint64>(stats.Copper, uint64(std::numeric_limits<int32>::max()));
            if (creditedCopper)
                target->ModifyMoney(int32(creditedCopper));
            stats.Copper = creditedCopper;

            total.Items += stats.Items;
            total.Stacks += stats.Stacks;
            total.SkippedStacks += stats.SkippedStacks;
            total.FailedStacks += stats.FailedStacks;
            total.Copper += stats.Copper;
            SendProtocol(handler, Acore::StringFormat("BOTINV:BULK:SELL:{}:{}:{}:{}:{}:{}",
                Sanitize(target->GetName()), stats.Items, stats.Stacks, stats.SkippedStacks, stats.FailedStacks, stats.Copper));
        }

        SendOk(handler, Acore::StringFormat("Party bulk-sell removed {} item(s) in {} stack(s) for {} copper; {} skipped, {} failed verification.",
            total.Items, total.Stacks, total.Copper, total.SkippedStacks, total.FailedStacks));
        if (refresh)
            SendPartyBags(handler, manager);
        return true;
    }


    static void ListBots(ChatHandler* handler, Player* manager)
    {
        if (!handler || !manager)
            return;

        SendProtocol(handler, "BOTINV:BOTS:BEGIN");

        std::set<ObjectGuid> sent;

        if (Player* selected = GetSelectedPlayerBot(handler, manager))
        {
            sent.insert(selected->GetGUID());
            SendBotSummary(handler, manager, selected);
        }

        if (Group* group = manager->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member == manager)
                    continue;

                if (sent.find(member->GetGUID()) != sent.end())
                    continue;

                sent.insert(member->GetGUID());
                SendBotSummary(handler, manager, member);
            }
        }

        SendProtocol(handler, "BOTINV:BOTS:END");
    }

    static void SendBagItem(ChatHandler* handler, Player* target, uint8 bagSlot, uint8 itemSlot)
    {
        if (!handler || !target)
            return;

        Item* item = target->GetItemByPos(bagSlot, itemSlot);
        if (!item)
            return;

        SendItemRecord(handler, "BOTINV:BAG:ITEM", target, uint32(bagSlot), uint32(itemSlot), item);
    }

    static void SendBags(ChatHandler* handler, Player* manager, Player* target)
    {
        if (!handler || !manager || !target)
            return;

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return;
        }

        RememberSelectedBot(manager, target);

        SendProtocol(handler, Acore::StringFormat("BOTINV:BAG:BEGIN:{}:{}:{}:{}", Sanitize(target->GetName()), CountFreeBagSlots(target), target->GetMoney(), g_bulkMaxQuality));

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            SendBagItem(handler, target, INVENTORY_SLOT_BAG_0, slot);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = target->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                SendBagItem(handler, target, bagSlot, uint8(slot));
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:BAG:END:{}", Sanitize(target->GetName())));
    }

    static void FindItemOnTarget(ChatHandler* handler, Player* manager, Player* target, uint32 itemEntry)
    {
        if (!handler || !manager || !target || !itemEntry)
            return;

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return;
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:FIND:BEGIN:{}:{}", Sanitize(target->GetName()), itemEntry));

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && item->GetEntry() == itemEntry)
                SendProtocol(handler, Acore::StringFormat("BOTINV:FIND:ITEM:{}:equip:{}:0:{}:{}", Sanitize(target->GetName()), uint32(slot), itemEntry, item->GetCount()));
        }

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && item->GetEntry() == itemEntry)
                SendProtocol(handler, Acore::StringFormat("BOTINV:FIND:ITEM:{}:bag:{}:{}:{}:{}", Sanitize(target->GetName()), uint32(INVENTORY_SLOT_BAG_0), uint32(slot), itemEntry, item->GetCount()));
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = target->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            {
                Item* item = bag->GetItemByPos(uint8(slot));
                if (item && item->GetEntry() == itemEntry)
                    SendProtocol(handler, Acore::StringFormat("BOTINV:FIND:ITEM:{}:bag:{}:{}:{}:{}", Sanitize(target->GetName()), uint32(bagSlot), slot, itemEntry, item->GetCount()));
            }
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:FIND:END:{}:{}", Sanitize(target->GetName()), itemEntry));
    }

    static void SendEquipment(ChatHandler* handler, Player* manager, Player* target)
    {
        if (!handler || !manager || !target)
            return;

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return;
        }

        RememberSelectedBot(manager, target);

        SendProtocol(handler, Acore::StringFormat("BOTINV:EQUIP:BEGIN:{}:{}", Sanitize(target->GetName()), target->GetMoney()));

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            SendItemRecord(handler, "BOTINV:EQUIP:ITEM", target, uint32(slot), 0, item);
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:EQUIP:END:{}", Sanitize(target->GetName())));
    }

    static bool HandleEquipTarget(ChatHandler* handler, Player* manager, uint8 bagSlot, uint8 itemSlot)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        Item* item = target->GetItemByPos(bagSlot, itemSlot);
        if (!item)
        {
            SendError(handler, "No item found in that bot bag slot.");
            return true;
        }

        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();

        uint16 dest = 0;
        InventoryResult msg = target->CanEquipItem(NULL_SLOT, dest, item, true);
        if (msg != EQUIP_ERR_OK)
        {
            target->SendEquipError(msg, item, nullptr);
            SendError(handler, "The bot cannot equip that item. The item was not moved.");
            return true;
        }

        // Loss-safe equip: remove only after validation, then verify EquipItem returns an equipped item.
        // If it fails unexpectedly, roll the original Item* back into the bot's bags instead of dropping it.
        target->RemoveItem(bagSlot, itemSlot, true);
        Item* equipped = target->EquipItem(dest, item, true);
        if (!equipped)
        {
            ItemPosCountVec rollbackDest;
            InventoryResult storeMsg = target->CanStoreItem(bagSlot, itemSlot, rollbackDest, item, false);
            if (storeMsg != EQUIP_ERR_OK)
                storeMsg = target->CanStoreItem(NULL_BAG, NULL_SLOT, rollbackDest, item, false);

            if (storeMsg == EQUIP_ERR_OK)
            {
                target->StoreItem(rollbackDest, item, true);
                SendError(handler, Acore::StringFormat("Equip failed unexpectedly; item {} x{} was returned to {}'s bags.", entry, count, Sanitize(target->GetName())));
            }
            else
            {
                LOG_ERROR("module", "BotInventoryMaster: CRITICAL rollback failed while equipping item {} x{} for {} from bag {} slot {}.",
                    entry, count, target->GetGUID().ToString(), uint32(bagSlot), uint32(itemSlot));
                SendError(handler, Acore::StringFormat("CRITICAL: equip failed and rollback failed for item {}. Stop and check character_inventory/item_instance before continuing.", entry));
            }

            SendEquipment(handler, manager, target);
            SendBags(handler, manager, target);
            return true;
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:EQUIP:OK:{}:{}:{}", Sanitize(target->GetName()), entry, count));
        SendOk(handler, Acore::StringFormat("{} equipped item {} x{}.", Sanitize(target->GetName()), entry, count));
        SendEquipment(handler, manager, target);
        SendBags(handler, manager, target);
        return true;
    }

    static bool HandleEquipBagTarget(ChatHandler* handler, Player* manager, uint8 bagSlot, uint8 itemSlot)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target or scan an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        Item* item = target->GetItemByPos(bagSlot, itemSlot);
        if (!item || !item->GetTemplate())
        {
            SendError(handler, "No valid item found in that bot bag slot.");
            return true;
        }

        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_CONTAINER)
        {
            SendError(handler, "Selected item is not a bag/container.");
            return true;
        }

        return HandleEquipTarget(handler, manager, bagSlot, itemSlot);
    }

    static bool HandleUnequipTarget(ChatHandler* handler, Player* manager, uint8 equipSlot)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        if (equipSlot < EQUIPMENT_SLOT_START || equipSlot >= EQUIPMENT_SLOT_END)
        {
            SendError(handler, "Invalid equipment slot.");
            return true;
        }

        Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot);
        if (!item)
        {
            SendError(handler, "No item equipped in that slot.");
            return true;
        }

        ItemPosCountVec dest;
        InventoryResult msg = target->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
        if (msg != EQUIP_ERR_OK)
        {
            target->SendEquipError(msg, item, nullptr);
            SendError(handler, "The bot has no bag space to unequip that item.");
            return true;
        }

        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();

        target->RemoveItem(INVENTORY_SLOT_BAG_0, equipSlot, true);
        Item* stored = target->StoreItem(dest, item, true);
        if (!stored)
        {
            // Roll back to the same equipment slot. This should be rare because CanStoreItem already passed.
            target->EquipItem(equipSlot, item, true);
            SendError(handler, Acore::StringFormat("Unequip failed unexpectedly; item {} x{} was returned to equipment slot {}.", entry, count, uint32(equipSlot)));
            SendEquipment(handler, manager, target);
            SendBags(handler, manager, target);
            return true;
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:UNEQUIP:OK:{}:{}:{}", Sanitize(target->GetName()), entry, count));
        SendOk(handler, Acore::StringFormat("{} unequipped item {} x{}.", Sanitize(target->GetName()), entry, count));
        SendEquipment(handler, manager, target);
        SendBags(handler, manager, target);
        return true;
    }

    static bool HandleDestroyTarget(ChatHandler* handler, Player* manager, uint8 bagSlot, uint8 itemSlot, bool confirm)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        if (!confirm && g_dangerRequireConfirm)
        {
            SendError(handler, "Destroy requires confirm.");
            return true;
        }

        Item* item = target->GetItemByPos(bagSlot, itemSlot);
        if (!item)
        {
            SendError(handler, "No item found in that bot bag slot.");
            return true;
        }

        ItemTemplate const* proto = item->GetTemplate();
        if (IsDestroyProtected(proto, reason))
        {
            SendError(handler, reason);
            return true;
        }

        uint32 entry = item->GetEntry();
        uint32 count = item->GetCount();
        target->DestroyItem(bagSlot, itemSlot, true);

        SendOk(handler, Acore::StringFormat("Destroyed {} x{} from {}.", entry, count, Sanitize(target->GetName())));
        SendBags(handler, manager, target);
        return true;
    }

    static bool IsTradeDistanceOk(Player* a, WorldObject* b)
    {
        return a && b && a->IsInWorld() && b->IsInWorld() && a->IsWithinDistInMap(b, g_requiredTradeDistance);
    }

    static bool HandleVendorSet(ChatHandler* handler, Player* manager)
    {
        if (!handler || !manager)
            return false;

        Unit* selected = manager->GetSelectedUnit();
        Creature* vendor = selected ? selected->ToCreature() : nullptr;
        if (!vendor || !vendor->IsAlive())
        {
            SendError(handler, "Target a living vendor NPC first.");
            return true;
        }

        if (!vendor->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        {
            SendError(handler, "Target is not a vendor.");
            return true;
        }

        if (!IsTradeDistanceOk(manager, vendor))
        {
            SendError(handler, "You are too far from that vendor.");
            return true;
        }

        g_selectedVendorByManager[manager->GetGUID().GetCounter()] = vendor->GetGUID();
        Player* rememberedBot = GetRememberedBot(manager);

        if (rememberedBot)
            SendOk(handler, Acore::StringFormat("Selected vendor {} for remembered bot {}.", Sanitize(vendor->GetName()), Sanitize(rememberedBot->GetName())));
        else
            SendOk(handler, Acore::StringFormat("Selected vendor {}. Scan a bot's bags next, then sell.", Sanitize(vendor->GetName())));

        SendProtocol(handler, Acore::StringFormat("BOTINV:VENDOR:SET:{}:{}", uint32(vendor->GetEntry()), Sanitize(vendor->GetName())));
        return true;
    }

    static Creature* GetSelectedVendor(ChatHandler* handler, Player* manager, std::string& reason)
    {
        reason.clear();

        if (!manager || !manager->GetMap())
        {
            reason = "Manager is not on a valid map.";
            return nullptr;
        }

        auto itr = g_selectedVendorByManager.find(manager->GetGUID().GetCounter());
        if (itr == g_selectedVendorByManager.end() || itr->second.IsEmpty())
        {
            reason = "No vendor selected. Target a vendor and use .botinv vendor set first.";
            return nullptr;
        }

        Creature* vendor = manager->GetMap()->GetCreature(itr->second);
        if (!vendor || !vendor->IsAlive())
        {
            reason = "Selected vendor is no longer available.";
            return nullptr;
        }

        if (!vendor->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        {
            reason = "Selected creature is no longer a vendor.";
            return nullptr;
        }

        if (!IsTradeDistanceOk(manager, vendor))
        {
            reason = "You are too far from the selected vendor.";
            return nullptr;
        }

        return vendor;
    }

    static uint32 SellGrayFromSlot(Player* source, uint8 bagSlot, uint8 itemSlot, uint64& copper, uint32& stacksSold, uint32& failedStacks)
    {
        if (!source)
            return 0;

        Item* item = source->GetItemByPos(bagSlot, itemSlot);
        if (!item)
            return 0;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Quality != ITEM_QUALITY_POOR)
            return 0;

        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();
        uint32 const sellPrice = proto->SellPrice;
        if (!count)
            return 0;

        source->DestroyItem(bagSlot, itemSlot, true);

        // Verify the slot no longer contains the same item stack before adding money.
        Item* after = source->GetItemByPos(bagSlot, itemSlot);
        if (after && after->GetEntry() == entry)
        {
            ++failedStacks;
            LOG_ERROR("module", "BotInventoryMaster: failed to sell/destroy gray item {} x{} from {} bag {} slot {}; item still present.",
                entry, count, source->GetGUID().ToString(), uint32(bagSlot), uint32(itemSlot));
            return 0;
        }

        copper += uint64(sellPrice) * uint64(count);
        ++stacksSold;
        return count;
    }

    static uint32 DestroyGrayFromSlot(Player* source, uint8 bagSlot, uint8 itemSlot, uint32& stacksDestroyed, uint32& failedStacks)
    {
        if (!source)
            return 0;

        Item* item = source->GetItemByPos(bagSlot, itemSlot);
        if (!item)
            return 0;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Quality != ITEM_QUALITY_POOR)
            return 0;

        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();
        if (!count)
            return 0;

        source->DestroyItem(bagSlot, itemSlot, true);

        // Verify removal. This is the important anti-"said it destroyed but did not" check.
        Item* after = source->GetItemByPos(bagSlot, itemSlot);
        if (after && after->GetEntry() == entry)
        {
            ++failedStacks;
            LOG_ERROR("module", "BotInventoryMaster: failed to destroy gray item {} x{} from {} bag {} slot {}; item still present.",
                entry, count, source->GetGUID().ToString(), uint32(bagSlot), uint32(itemSlot));
            return 0;
        }

        ++stacksDestroyed;
        return count;
    }

    static bool HandleSellGrayTarget(ChatHandler* handler, Player* manager, bool confirm)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        if (!confirm)
        {
            SendError(handler, "Sell gray requires confirm.");
            return true;
        }

        Creature* vendor = GetSelectedVendor(handler, manager, reason);
        if (!vendor)
        {
            SendError(handler, reason);
            return true;
        }

        if (!IsTradeDistanceOk(target, vendor))
        {
            SendError(handler, Acore::StringFormat("{} is too far from the selected vendor.", Sanitize(target->GetName())));
            return true;
        }

        uint64 copper = 0;
        uint32 stacksSold = 0;
        uint32 failedStacks = 0;
        uint32 itemsSold = 0;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            itemsSold += SellGrayFromSlot(target, INVENTORY_SLOT_BAG_0, slot, copper, stacksSold, failedStacks);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = target->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                itemsSold += SellGrayFromSlot(target, bagSlot, uint8(slot), copper, stacksSold, failedStacks);
        }

        if (copper > 0)
        {
            uint64 capped = std::min<uint64>(copper, uint64(std::numeric_limits<int32>::max()));
            target->ModifyMoney(int32(capped));
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:SELL:GRAY:{}:{}:{}:{}:{}", Sanitize(target->GetName()), itemsSold, stacksSold, failedStacks, copper));

        if (failedStacks)
            SendError(handler, Acore::StringFormat("{} sold {} gray item(s), but {} stack(s) failed verification. Refresh bags and inspect before continuing.",
                Sanitize(target->GetName()), itemsSold, failedStacks));
        else
            SendOk(handler, Acore::StringFormat("{} sold {} gray item(s) in {} stack(s) to {} for {} copper.",
                Sanitize(target->GetName()), itemsSold, stacksSold, Sanitize(vendor->GetName()), copper));

        SendBags(handler, manager, target);
        return true;
    }


    static bool HandleSellItemTarget(ChatHandler* handler, Player* manager, uint8 bagSlot, uint8 itemSlot, bool confirm)
    {
        if (!g_allowSellSelected)
        {
            SendError(handler, "Selected item selling is disabled in config.");
            return true;
        }

        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target or scan an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        if (!confirm && g_dangerRequireConfirm)
        {
            SendError(handler, "Selling a selected item requires confirm.");
            return true;
        }

        Creature* vendor = GetSelectedVendor(handler, manager, reason);
        if (!vendor)
        {
            SendError(handler, reason);
            return true;
        }

        if (!IsTradeDistanceOk(target, vendor))
        {
            SendError(handler, Acore::StringFormat("{} is too far from the selected vendor.", Sanitize(target->GetName())));
            return true;
        }

        Item* item = target->GetItemByPos(bagSlot, itemSlot);
        if (!item)
        {
            SendError(handler, "No item found in that bot bag slot.");
            return true;
        }

        if (IsSellProtected(item, reason))
        {
            SendError(handler, reason);
            return true;
        }

        ItemTemplate const* proto = item->GetTemplate();
        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();
        uint32 const quality = proto ? uint32(proto->Quality) : 0;
        uint64 const copper = proto ? (uint64(proto->SellPrice) * uint64(count)) : 0;
        std::string const name = proto ? Sanitize(proto->Name1) : "";

        if (!count)
        {
            SendError(handler, "Item count was zero.");
            return true;
        }

        target->DestroyItem(bagSlot, itemSlot, true);

        Item* after = target->GetItemByPos(bagSlot, itemSlot);
        if (after && after->GetEntry() == entry)
        {
            SendError(handler, "Sell failed verification; item still appears in that slot.");
            SendBags(handler, manager, target);
            return true;
        }

        uint64 capped = std::min<uint64>(copper, uint64(std::numeric_limits<int32>::max()));
        target->ModifyMoney(int32(capped));

        AddBuybackRecord(target, entry, count, quality, capped, name);

        uint32 buybackId = 0;
        auto itr = g_buybackByBot.find(target->GetGUID().GetCounter());
        if (itr != g_buybackByBot.end() && !itr->second.empty())
            buybackId = itr->second.back().Id;

        SendProtocol(handler, Acore::StringFormat("BOTINV:SELL:ITEM:{}:{}:{}:{}:{}:{}:{}",
            Sanitize(target->GetName()), buybackId, entry, count, quality, capped, name));

        SendOk(handler, Acore::StringFormat("{} sold {} x{} to {} for {} copper. Buyback id: {}.",
            Sanitize(target->GetName()), entry, count, Sanitize(vendor->GetName()), capped, buybackId));

        SendBags(handler, manager, target);
        SendBuybackList(handler, manager, target);
        return true;
    }

    static bool HandleBuybackTarget(ChatHandler* handler, Player* manager, uint32 buybackId)
    {
        if (!g_allowBuyback)
        {
            SendError(handler, "Module buyback is disabled in config.");
            return true;
        }

        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target or scan an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        BuybackRecord* record = FindBuybackRecord(target, buybackId);
        if (!record)
        {
            SendError(handler, "Buyback record not found for that bot.");
            SendBuybackList(handler, manager, target);
            return true;
        }

        if (record->Cost > uint64(std::numeric_limits<int32>::max()))
        {
            SendError(handler, "Buyback cost is too large for this safety implementation.");
            return true;
        }

        if (target->GetMoney() < record->Cost)
        {
            SendError(handler, Acore::StringFormat("{} does not have enough money to buy back item {}.", Sanitize(target->GetName()), record->Entry));
            return true;
        }

        if (!target->AddItem(record->Entry, record->Count))
        {
            SendError(handler, Acore::StringFormat("{} has no room to buy back item {} x{}.", Sanitize(target->GetName()), record->Entry, record->Count));
            SendBags(handler, manager, target);
            return true;
        }

        target->ModifyMoney(-int32(record->Cost));

        SendProtocol(handler, Acore::StringFormat("BOTINV:BUYBACK:OK:{}:{}:{}:{}:{}",
            Sanitize(target->GetName()), record->Id, record->Entry, record->Count, record->Cost));

        SendOk(handler, Acore::StringFormat("{} bought back {} x{} for {} copper.",
            Sanitize(target->GetName()), record->Entry, record->Count, record->Cost));

        RemoveBuybackRecord(target, buybackId);

        SendBags(handler, manager, target);
        SendBuybackList(handler, manager, target);
        return true;
    }

    static CleanupStats DestroyGrayFromPlayer(Player* target)
    {
        CleanupStats stats;

        if (!target)
            return stats;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            stats.Items += DestroyGrayFromSlot(target, INVENTORY_SLOT_BAG_0, slot, stats.Stacks, stats.FailedStacks);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = target->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                stats.Items += DestroyGrayFromSlot(target, bagSlot, uint8(slot), stats.Stacks, stats.FailedStacks);
        }

        return stats;
    }

    static bool HandleDestroyGrayTarget(ChatHandler* handler, Player* manager, bool confirm)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        if (!confirm)
        {
            SendError(handler, "Destroy gray requires confirm.");
            return true;
        }

        CleanupStats stats = DestroyGrayFromPlayer(target);
        SendProtocol(handler, Acore::StringFormat("BOTINV:DESTROY:GRAY:{}:{}:{}", Sanitize(target->GetName()), stats.Items, stats.Stacks));

        if (stats.FailedStacks)
            SendError(handler, Acore::StringFormat("{} destroyed {} gray item(s), but {} stack(s) failed verification. Refresh bags and inspect.",
                Sanitize(target->GetName()), stats.Items, stats.FailedStacks));
        else
            SendOk(handler, Acore::StringFormat("{} destroyed {} gray item(s) in {} stack(s).",
                Sanitize(target->GetName()), stats.Items, stats.Stacks));

        SendBags(handler, manager, target);
        return true;
    }

    static bool HandleDestroyGrayParty(ChatHandler* handler, Player* manager, bool confirm)
    {
        if (!handler || !manager)
            return false;

        if (!confirm)
        {
            SendError(handler, "Party destroy gray requires confirm.");
            return true;
        }

        Group* group = manager->GetGroup();
        if (!group)
        {
            SendError(handler, "You are not in a group.");
            return true;
        }

        uint32 totalItems = 0;
        uint32 totalStacks = 0;
        uint32 totalFailed = 0;
        uint32 botCount = 0;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == manager)
                continue;

            std::string reason;
            if (!CanManageTarget(manager, member, reason))
                continue;

            CleanupStats stats = DestroyGrayFromPlayer(member);
            if (stats.Items || stats.Stacks || stats.FailedStacks)
            {
                ++botCount;
                totalItems += stats.Items;
                totalStacks += stats.Stacks;
                totalFailed += stats.FailedStacks;
                SendProtocol(handler, Acore::StringFormat("BOTINV:DESTROY:GRAY:{}:{}:{}", Sanitize(member->GetName()), stats.Items, stats.Stacks));
            }
        }

        if (totalFailed)
            SendError(handler, Acore::StringFormat("Party gray cleanup destroyed {} item(s) in {} stack(s), but {} stack(s) failed verification.",
                totalItems, totalStacks, totalFailed));
        else
            SendOk(handler, Acore::StringFormat("Party gray cleanup destroyed {} item(s) in {} stack(s) from {} manageable bot(s).",
                totalItems, totalStacks, botCount));

        return true;
    }

    static bool IsTakeProtected(Item* item, std::string& reason)
    {
        reason.clear();

        if (!item || !item->GetTemplate())
        {
            reason = "No valid item.";
            return true;
        }

        ItemTemplate const* proto = item->GetTemplate();

        if (g_dangerAllowAnySell)
            return false;

        if (proto->Class == ITEM_CLASS_QUEST)
        {
            reason = "Quest items are protected.";
            return true;
        }

        if (proto->Class == ITEM_CLASS_CONTAINER)
        {
            reason = "Bags/containers are protected from take in this safety pass.";
            return true;
        }

        if (item->IsSoulBound())
        {
            reason = "Soulbound items cannot be traded/taken.";
            return true;
        }

        return false;
    }

    static bool HandleTakeTarget(ChatHandler* handler, Player* manager, uint8 bagSlot, uint8 itemSlot)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        if (!IsTradeDistanceOk(manager, target))
        {
            SendError(handler, "You are too far from the bot to receive that item.");
            return true;
        }

        Item* item = target->GetItemByPos(bagSlot, itemSlot);
        if (!item)
        {
            SendError(handler, "No item found in that bot bag slot.");
            return true;
        }

        if (IsTakeProtected(item, reason))
        {
            SendError(handler, reason);
            return true;
        }

        ItemTemplate const* proto = item->GetTemplate();
        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();

        ItemPosCountVec dest;
        InventoryResult msg = manager->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
        if (msg != EQUIP_ERR_OK)
        {
            manager->SendEquipError(msg, item, nullptr);
            SendError(handler, "Your bags do not have room for that item.");
            return true;
        }

        target->RemoveItem(bagSlot, itemSlot, true);
        Item* stored = manager->StoreItem(dest, item, true);
        if (!stored)
        {
            ItemPosCountVec rollbackDest;
            InventoryResult backMsg = target->CanStoreItem(bagSlot, itemSlot, rollbackDest, item, false);
            if (backMsg != EQUIP_ERR_OK)
                backMsg = target->CanStoreItem(NULL_BAG, NULL_SLOT, rollbackDest, item, false);

            if (backMsg == EQUIP_ERR_OK)
                target->StoreItem(rollbackDest, item, true);
            else
                LOG_ERROR("module", "BotInventoryMaster: CRITICAL take rollback failed for item {} x{} from {}.", entry, count, target->GetGUID().ToString());

            SendError(handler, "Take failed unexpectedly; item rollback was attempted. Refresh bags and verify.");
            SendBags(handler, manager, target);
            return true;
        }

        manager->SendNewItem(stored, count, true, false);

        SendProtocol(handler, Acore::StringFormat("BOTINV:TAKE:{}:{}:{}:{}", Sanitize(target->GetName()), entry, count, proto ? Sanitize(proto->Name1) : ""));
        SendOk(handler, Acore::StringFormat("{} gave you {} x{}.", Sanitize(target->GetName()), entry, count));

        SendBags(handler, manager, target);
        return true;
    }

    static void SaveAccountBankItems(uint32 ownerAccount, std::map<uint32, std::pair<uint32, uint32>> const& changed)
    {
        if (!ownerAccount || changed.empty())
            return;

        auto trans = CharacterDatabase.BeginTransaction();

        for (std::pair<uint32 const, std::pair<uint32, uint32>> const& pair : changed)
        {
            uint32 const itemEntry = pair.first;
            uint32 const itemSubclass = pair.second.first;
            uint32 const amountToAdd = pair.second.second;

            if (!itemEntry || !amountToAdd)
                continue;

            trans->Append(
                "INSERT INTO `mod_bot_inventory_master_bank` "
                "(`account_id`, `item_entry`, `item_subclass`, `amount`) "
                "VALUES ({}, {}, {}, {}) "
                "ON DUPLICATE KEY UPDATE "
                "`amount` = LEAST(4294967295, `amount` + VALUES(`amount`)), "
                "`item_subclass` = VALUES(`item_subclass`)",
                ownerAccount, itemEntry, itemSubclass, amountToAdd);
        }

        CharacterDatabase.CommitTransaction(trans);
    }

    static uint32 DepositReagentFromSlot(Player* source, uint8 bagSlot, uint8 itemSlot, ItemAmountMap& deposited, std::map<uint32, std::pair<uint32, uint32>>& bankChanges, bool& overflowed)
    {
        if (!source)
            return 0;

        Item* item = source->GetItemByPos(bagSlot, itemSlot);
        if (!item)
            return 0;

        uint32 itemEntry = 0;
        uint32 itemSubclass = 0;
        ItemTemplate const* proto = item->GetTemplate();

        if (!IsStorableReagent(proto, itemEntry, itemSubclass))
            return 0;

        if (IsDepositExcluded(itemEntry))
            return 0;

        uint32 const count = item->GetCount();
        if (!count)
            return 0;

        if (!AddToItemAmountMapChecked(deposited, itemEntry, count))
        {
            overflowed = true;
            return 0;
        }

        std::pair<uint32, uint32>& change = bankChanges[itemEntry];
        change.first = itemSubclass;

        uint32 updated = 0;
        if (!AddAmountChecked(change.second, count, updated))
        {
            overflowed = true;
            return 0;
        }

        change.second = updated;

        source->DestroyItem(bagSlot, itemSlot, true);
        return count;
    }

    static uint32 DepositReagentsFromBot(Player* manager, Player* source, ItemAmountMap& deposited, bool& overflowed)
    {
        deposited.clear();
        overflowed = false;

        if (!manager || !source || !manager->GetSession())
            return 0;

        uint32 const ownerAccount = manager->GetSession()->GetAccountId();
        std::map<uint32, std::pair<uint32, uint32>> bankChanges;

        uint32 total = 0;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            uint32 amount = DepositReagentFromSlot(source, INVENTORY_SLOT_BAG_0, slot, deposited, bankChanges, overflowed);
            uint32 updated = 0;
            if (AddAmountChecked(total, amount, updated))
                total = updated;
            else
            {
                overflowed = true;
                break;
            }
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = source->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            {
                uint32 amount = DepositReagentFromSlot(source, bagSlot, uint8(slot), deposited, bankChanges, overflowed);
                uint32 updated = 0;
                if (AddAmountChecked(total, amount, updated))
                    total = updated;
                else
                {
                    overflowed = true;
                    break;
                }
            }

            if (overflowed)
                break;
        }

        SaveAccountBankItems(ownerAccount, bankChanges);
        return total;
    }

    static void SendDepositTransaction(ChatHandler* handler, Player* source, ItemAmountMap const& deposited)
    {
        if (!handler || !source)
            return;

        uint64 total = SumItemAmountMap(deposited);
        SendProtocol(handler, Acore::StringFormat("BOTINV:TX:BEGIN:deposit:{}:{}:{}", Sanitize(source->GetName()), total, uint32(deposited.size())));

        for (std::pair<uint32 const, uint32> const& item : deposited)
            SendProtocol(handler, Acore::StringFormat("BOTINV:TX:ITEM:{}:{}:{}", Sanitize(source->GetName()), item.first, item.second));

        SendProtocol(handler, Acore::StringFormat("BOTINV:TX:END:deposit:{}:{}:{}", Sanitize(source->GetName()), total, uint32(deposited.size())));
    }

    static bool HandleDepositTarget(ChatHandler* handler, Player* manager)
    {
        Player* target = GetSelectedPlayerBot(handler, manager);
        if (!target)
        {
            SendError(handler, "Target an online playerbot/alt first.");
            return true;
        }

        std::string reason;
        if (!CanManageTarget(manager, target, reason))
        {
            SendError(handler, reason);
            return true;
        }

        ItemAmountMap deposited;
        bool overflowed = false;
        uint32 total = DepositReagentsFromBot(manager, target, deposited, overflowed);

        SendDepositTransaction(handler, target, deposited);

        if (total)
            SendOk(handler, Acore::StringFormat("Deposited {} reagent(s) from {} into your bot inventory bank.", total, Sanitize(target->GetName())));
        else if (overflowed)
            SendError(handler, "Deposit failed because the virtual bank amount cap would be exceeded.");
        else
            SendOk(handler, Acore::StringFormat("{} had no matching reagents.", Sanitize(target->GetName())));

        return true;
    }

    static bool HandleDepositParty(ChatHandler* handler, Player* manager)
    {
        if (!handler || !manager)
            return false;

        Group* group = manager->GetGroup();
        if (!group)
        {
            SendError(handler, "You are not in a group.");
            return true;
        }

        uint32 totalAll = 0;
        uint32 botCount = 0;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == manager)
                continue;

            std::string reason;
            if (!CanManageTarget(manager, member, reason))
                continue;

            ItemAmountMap deposited;
            bool overflowed = false;
            uint32 total = DepositReagentsFromBot(manager, member, deposited, overflowed);
            SendDepositTransaction(handler, member, deposited);

            if (total)
            {
                ++botCount;
                uint32 updated = 0;
                if (AddAmountChecked(totalAll, total, updated))
                    totalAll = updated;
            }
        }

        SendOk(handler, Acore::StringFormat("Deposited {} reagent(s) from {} manageable group bot(s).", totalAll, botCount));
        return true;
    }

    static void SendBank(ChatHandler* handler, Player* manager)
    {
        if (!handler || !manager || !manager->GetSession())
            return;

        uint32 const accountId = manager->GetSession()->GetAccountId();

        QueryResult result = CharacterDatabase.Query(
            "SELECT `item_entry`, `item_subclass`, `amount` "
            "FROM `mod_bot_inventory_master_bank` "
            "WHERE `account_id` = {} "
            "ORDER BY `item_subclass`, `item_entry` "
            "LIMIT {}",
            accountId, g_maxBankListRows);

        SendProtocol(handler, Acore::StringFormat("BOTINV:BANK:BEGIN:{}", accountId));

        if (result)
        {
            do
            {
                uint32 itemEntry = (*result)[0].Get<uint32>();
                uint32 subclass = (*result)[1].Get<uint32>();
                uint32 amount = (*result)[2].Get<uint32>();

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
                std::string name = proto ? proto->Name1 : "";

                SendProtocol(handler, Acore::StringFormat(
                    "BOTINV:BANK:ITEM:{}:{}:{}:{}",
                    itemEntry, subclass, amount, Sanitize(name)));
            } while (result->NextRow());
        }

        SendProtocol(handler, "BOTINV:BANK:END");
    }

    static void SendUsage(ChatHandler* handler)
    {
        SendProtocol(handler, "BOTINV:USAGE:.botinv bots | use <botName> | vendor set | target bags | party bags | target sellbatch <bag,slot;...> confirm | target destroybatch <bag,slot;...> confirm | party sellbatch <Bot,bag,slot;...> confirm [refresh] | party destroybatch <Bot,bag,slot;...> confirm [refresh] | target equipment | target equip <bag> <slot> | target equipbag <bag> <slot> | target unequip <equipSlot> | target take <bag> <slot> | target find <itemEntry> | target sell gray confirm | target sell <bag> <slot> [confirm] | target buyback list | target buyback <id> | target destroy gray confirm | target destroy <bag> <slot> [confirm] | party destroy gray confirm | target deposit reagents | party deposit reagents | bank");
        handler->SendSysMessage("BotInventoryMaster commands:");
        handler->SendSysMessage(".botinv bots");
        handler->SendSysMessage(".botinv use <botName>");
        handler->SendSysMessage(".botinv vendor set");
        handler->SendSysMessage("Note: scanning target bags remembers that bot, so vendor actions can work while a vendor is targeted.");
        handler->SendSysMessage(".botinv target bags");
        handler->SendSysMessage(".botinv party bags");
        handler->SendSysMessage(".botinv target equipment");
        handler->SendSysMessage(".botinv target equip <bag> <slot>");
        handler->SendSysMessage(".botinv target equipbag <bag> <slot>");
        handler->SendSysMessage(".botinv target unequip <equipSlot>");
        handler->SendSysMessage(".botinv target take <bag> <slot>");
        handler->SendSysMessage(".botinv target find <itemEntry>");
        handler->SendSysMessage(".botinv target sell gray confirm");
        handler->SendSysMessage(".botinv target sellbatch <bag,slot;bag,slot;...> confirm");
        handler->SendSysMessage(".botinv target sell <bag> <slot> [confirm]");
        handler->SendSysMessage(".botinv target buyback list");
        handler->SendSysMessage(".botinv target buyback <id>");
        handler->SendSysMessage(".botinv target destroy gray confirm");
        handler->SendSysMessage(".botinv target destroybatch <bag,slot;bag,slot;...> confirm");
        handler->SendSysMessage(".botinv party sellbatch <Bot,bag,slot;...> confirm [refresh]");
        handler->SendSysMessage(".botinv party destroybatch <Bot,bag,slot;...> confirm [refresh]");
        handler->SendSysMessage(".botinv party destroy gray confirm");
        handler->SendSysMessage(".botinv target destroy <bag> <slot> [confirm]");
        handler->SendSysMessage(".botinv target deposit reagents");
        handler->SendSysMessage(".botinv party deposit reagents");
        handler->SendSysMessage(".botinv bank");
    }
}

class mod_bot_inventory_master_commandscript : public CommandScript
{
public:
    mod_bot_inventory_master_commandscript() : CommandScript("mod_bot_inventory_master_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "botinv", HandleBotInvCommand, SEC_PLAYER, Console::No }
        };

        return commandTable;
    }

private:
    static bool HandleBotInvCommand(ChatHandler* handler, char const* args)
    {
        Player* manager = handler && handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!manager)
            return false;

        if (!BotInventoryMaster::g_enabled)
        {
            BotInventoryMaster::SendError(handler, "Bot Inventory Master is disabled.");
            return true;
        }

        if (!BotInventoryMaster::g_storageReady)
        {
            BotInventoryMaster::SendError(handler, "Bot Inventory Master storage is not initialized yet.");
            return true;
        }

        std::vector<std::string> tokens = BotInventoryMaster::Tokenize(args);
        if (tokens.empty() || BotInventoryMaster::ToLower(tokens[0]) == "help")
        {
            BotInventoryMaster::SendUsage(handler);
            return true;
        }

        std::string command = BotInventoryMaster::ToLower(tokens[0]);

        if (command == "bots")
        {
            BotInventoryMaster::ListBots(handler, manager);
            return true;
        }

        if (command == "bank")
        {
            BotInventoryMaster::SendBank(handler, manager);
            return true;
        }

        if (command == "use")
        {
            if (tokens.size() < 2)
            {
                BotInventoryMaster::SendError(handler, "Usage: .botinv use <botName>");
                return true;
            }
            return BotInventoryMaster::HandleUseBot(handler, manager, tokens[1]);
        }

        if (command == "vendor")
        {
            if (tokens.size() >= 2 && BotInventoryMaster::ToLower(tokens[1]) == "set")
                return BotInventoryMaster::HandleVendorSet(handler, manager);

            BotInventoryMaster::SendUsage(handler);
            return true;
        }

        if (command == "target")
        {
            if (tokens.size() >= 2 && BotInventoryMaster::ToLower(tokens[1]) == "bags")
            {
                Player* target = BotInventoryMaster::GetSelectedPlayerBot(handler, manager);
                if (!target)
                {
                    BotInventoryMaster::SendError(handler, "Target an online playerbot/alt first.");
                    return true;
                }

                BotInventoryMaster::SendBags(handler, manager, target);
                return true;
            }

            if (tokens.size() >= 2 && BotInventoryMaster::ToLower(tokens[1]) == "equipment")
            {
                Player* target = BotInventoryMaster::GetSelectedPlayerBot(handler, manager);
                if (!target)
                {
                    BotInventoryMaster::SendError(handler, "Target an online playerbot/alt first.");
                    return true;
                }

                BotInventoryMaster::SendEquipment(handler, manager, target);
                return true;
            }

            if (tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[1]) == "equip")
            {
                uint32 bag = 0;
                uint32 slot = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], bag) || !BotInventoryMaster::TryParseUInt32(tokens[3], slot) ||
                    bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target equip <bag> <slot>");
                    return true;
                }

                return BotInventoryMaster::HandleEquipTarget(handler, manager, uint8(bag), uint8(slot));
            }

            if (tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[1]) == "equipbag")
            {
                uint32 bag = 0;
                uint32 slot = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], bag) || !BotInventoryMaster::TryParseUInt32(tokens[3], slot) ||
                    bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target equipbag <bag> <slot>");
                    return true;
                }

                return BotInventoryMaster::HandleEquipBagTarget(handler, manager, uint8(bag), uint8(slot));
            }

            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "unequip")
            {
                uint32 equipSlot = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], equipSlot) || equipSlot > std::numeric_limits<uint8>::max())
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target unequip <equipSlot>");
                    return true;
                }

                return BotInventoryMaster::HandleUnequipTarget(handler, manager, uint8(equipSlot));
            }

            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "destroybatch")
            {
                bool confirm = tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleDestroyBatchTarget(handler, manager, tokens[2], confirm);
            }

            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "destroy" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
            {
                bool confirm = BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleDestroyGrayTarget(handler, manager, confirm);
            }

            if (tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[1]) == "destroy")
            {
                uint32 bag = 0;
                uint32 slot = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], bag) || !BotInventoryMaster::TryParseUInt32(tokens[3], slot) ||
                    bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target destroy <bag> <slot> [confirm]");
                    return true;
                }

                bool confirm = tokens.size() >= 5 && BotInventoryMaster::ToLower(tokens[4]) == "confirm";
                return BotInventoryMaster::HandleDestroyTarget(handler, manager, uint8(bag), uint8(slot), confirm);
            }

            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "find")
            {
                uint32 itemEntry = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], itemEntry) || !itemEntry)
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target find <itemEntry>");
                    return true;
                }

                Player* target = BotInventoryMaster::GetSelectedPlayerBot(handler, manager);
                if (!target)
                {
                    BotInventoryMaster::SendError(handler, "Target an online playerbot/alt first.");
                    return true;
                }

                BotInventoryMaster::FindItemOnTarget(handler, manager, target, itemEntry);
                return true;
            }

            if (tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[1]) == "take")
            {
                uint32 bag = 0;
                uint32 slot = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], bag) || !BotInventoryMaster::TryParseUInt32(tokens[3], slot) ||
                    bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target take <bag> <slot>");
                    return true;
                }

                return BotInventoryMaster::HandleTakeTarget(handler, manager, uint8(bag), uint8(slot));
            }

            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "sellbatch")
            {
                bool confirm = tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleSellBatchTarget(handler, manager, tokens[2], confirm);
            }

            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "sell" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
            {
                bool confirm = BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleSellGrayTarget(handler, manager, confirm);
            }

            if (tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[1]) == "sell")
            {
                uint32 bag = 0;
                uint32 slot = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], bag) || !BotInventoryMaster::TryParseUInt32(tokens[3], slot) ||
                    bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target sell <bag> <slot> [confirm]");
                    return true;
                }

                bool confirm = tokens.size() >= 5 && BotInventoryMaster::ToLower(tokens[4]) == "confirm";
                return BotInventoryMaster::HandleSellItemTarget(handler, manager, uint8(bag), uint8(slot), confirm);
            }

            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "buyback")
            {
                if (BotInventoryMaster::ToLower(tokens[2]) == "list")
                {
                    Player* target = BotInventoryMaster::GetSelectedPlayerBot(handler, manager);
                    if (!target)
                    {
                        BotInventoryMaster::SendError(handler, "Target or scan an online playerbot/alt first.");
                        return true;
                    }

                    BotInventoryMaster::SendBuybackList(handler, manager, target);
                    return true;
                }

                uint32 buybackId = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], buybackId) || !buybackId)
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target buyback <id>");
                    return true;
                }

                return BotInventoryMaster::HandleBuybackTarget(handler, manager, buybackId);
            }

            if (tokens.size() >= 3 &&
                BotInventoryMaster::ToLower(tokens[1]) == "deposit" &&
                BotInventoryMaster::ToLower(tokens[2]) == "reagents")
            {
                if (!BotInventoryMaster::g_depositEnabled)
                {
                    BotInventoryMaster::SendError(handler, "Bot reagent deposit is disabled.");
                    return true;
                }

                return BotInventoryMaster::HandleDepositTarget(handler, manager);
            }

            BotInventoryMaster::SendUsage(handler);
            return true;
        }

        if (command == "party")
        {
            if (tokens.size() >= 2 && BotInventoryMaster::ToLower(tokens[1]) == "bags")
            {
                BotInventoryMaster::SendPartyBags(handler, manager);
                return true;
            }

            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "sellbatch")
            {
                bool confirm = tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                bool refresh = tokens.size() >= 5 && BotInventoryMaster::ToLower(tokens[4]) == "refresh";
                return BotInventoryMaster::HandlePartySellBatch(handler, manager, tokens[2], confirm, refresh);
            }

            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "destroybatch")
            {
                bool confirm = tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                bool refresh = tokens.size() >= 5 && BotInventoryMaster::ToLower(tokens[4]) == "refresh";
                return BotInventoryMaster::HandlePartyDestroyBatch(handler, manager, tokens[2], confirm, refresh);
            }

            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "destroy" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
            {
                bool confirm = BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleDestroyGrayParty(handler, manager, confirm);
            }

            if (tokens.size() >= 3 &&
                BotInventoryMaster::ToLower(tokens[1]) == "deposit" &&
                BotInventoryMaster::ToLower(tokens[2]) == "reagents")
            {
                if (!BotInventoryMaster::g_depositEnabled)
                {
                    BotInventoryMaster::SendError(handler, "Bot reagent deposit is disabled.");
                    return true;
                }

                return BotInventoryMaster::HandleDepositParty(handler, manager);
            }

            BotInventoryMaster::SendUsage(handler);
            return true;
        }

        BotInventoryMaster::SendUsage(handler);
        return true;
    }
};

class mod_bot_inventory_master_worldscript : public WorldScript
{
public:
    mod_bot_inventory_master_worldscript() : WorldScript("mod_bot_inventory_master_worldscript") {}

    void OnAfterConfigLoad(bool reload) override
    {
        BotInventoryMaster::LoadConfig();

        if (reload)
        {
            BotInventoryMaster::EnsureTables();
            BotInventoryMaster::g_storageReady = true;

            LOG_INFO("module", "BotInventoryMaster config reloaded. Enabled: {}, SameAccount: {}, GMBypass: {}, RequireBot: {}, Deposit: {}",
                BotInventoryMaster::g_enabled ? "yes" : "no",
                BotInventoryMaster::g_requireSameAccount ? "yes" : "no",
                BotInventoryMaster::g_gmBypass ? "yes" : "no",
                BotInventoryMaster::g_requireTargetIsBot ? "yes" : "no",
                BotInventoryMaster::g_depositEnabled ? "yes" : "no");
        }
    }

    void OnStartup() override
    {
        BotInventoryMaster::LoadConfig();
        BotInventoryMaster::EnsureTables();
        BotInventoryMaster::g_storageReady = true;

        LOG_INFO("module", "BotInventoryMaster loaded. Enabled: {}, SameAccount: {}, GMBypass: {}, RequireBot: {}, Deposit: {}",
            BotInventoryMaster::g_enabled ? "yes" : "no",
            BotInventoryMaster::g_requireSameAccount ? "yes" : "no",
            BotInventoryMaster::g_gmBypass ? "yes" : "no",
            BotInventoryMaster::g_requireTargetIsBot ? "yes" : "no",
            BotInventoryMaster::g_depositEnabled ? "yes" : "no");
    }
};

void AddSC_mod_bot_inventory_master()
{
    new mod_bot_inventory_master_worldscript();
    new mod_bot_inventory_master_commandscript();
}
