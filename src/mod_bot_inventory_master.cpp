// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * AzerothCore module: Bot Inventory Master
 *
 * Goal:
 *   Let a player manage the inventory of online playerbot/alt characters that
 *   belong to the same account, without touching other accounts unless an
 *   explicit account-linking system is added later.
 *
 * Phase 1 commands:
 *   .botinv
 *   .botinv bots
 *   .botinv target bags
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
    static std::map<ObjectGuid::LowType, ObjectGuid> g_selectedVendorByManager;

    static constexpr uint32 MAX_STORED_AMOUNT = std::numeric_limits<uint32>::max();

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
        g_maxBankListRows = sConfigMgr->GetOption<uint32>("BotInventoryMaster.MaxBankListRows", 80);
        g_requiredTradeDistance = sConfigMgr->GetOption<float>("BotInventoryMaster.RequiredTradeDistance", 12.0f);

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

    static Player* GetSelectedPlayerBot(ChatHandler* handler, Player* manager)
    {
        if (!handler || !manager)
            return nullptr;

        Unit* selected = manager->GetSelectedUnit();
        if (!selected)
            return nullptr;

        return selected->ToPlayer();
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

        SendProtocol(handler, Acore::StringFormat(
            "{}:{}:{}:{}:{}:{}:{}:{}:{}",
            prefix,
            Sanitize(owner->GetName()),
            slotA,
            slotB,
            proto->ItemId,
            item->GetCount(),
            uint32(proto->Quality),
            proto->SellPrice,
            Sanitize(proto->Name1)));
    }

    static bool IsDestroyProtected(ItemTemplate const* proto, std::string& reason)
    {
        reason.clear();

        if (!proto)
        {
            reason = "Missing item template.";
            return true;
        }

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

        SendProtocol(handler, Acore::StringFormat("BOTINV:BAG:BEGIN:{}:{}", Sanitize(target->GetName()), CountFreeBagSlots(target)));

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

        SendProtocol(handler, Acore::StringFormat("BOTINV:EQUIP:BEGIN:{}", Sanitize(target->GetName())));

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

        uint16 dest = 0;
        InventoryResult msg = target->CanEquipItem(NULL_SLOT, dest, item, true);
        if (msg != EQUIP_ERR_OK)
        {
            target->SendEquipError(msg, item, nullptr);
            SendError(handler, "The bot cannot equip that item. Try unequipping the occupied slot first if needed.");
            return true;
        }

        target->RemoveItem(bagSlot, itemSlot, true);
        target->EquipItem(dest, item, true);

        SendOk(handler, Acore::StringFormat("{} equipped item {}.", Sanitize(target->GetName()), item->GetEntry()));
        SendEquipment(handler, manager, target);
        SendBags(handler, manager, target);
        return true;
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

        target->RemoveItem(INVENTORY_SLOT_BAG_0, equipSlot, true);
        target->StoreItem(dest, item, true);

        SendOk(handler, Acore::StringFormat("{} unequipped item {}.", Sanitize(target->GetName()), item->GetEntry()));
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

        if (!confirm)
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
        SendOk(handler, Acore::StringFormat("Selected vendor {} for bot selling.", Sanitize(vendor->GetName())));
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

    static uint32 SellGrayFromSlot(Player* source, uint8 bagSlot, uint8 itemSlot, uint64& copper, uint32& stacksSold)
    {
        if (!source)
            return 0;

        Item* item = source->GetItemByPos(bagSlot, itemSlot);
        if (!item)
            return 0;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Quality != ITEM_QUALITY_POOR)
            return 0;

        uint32 const count = item->GetCount();
        if (!count)
            return 0;

        copper += uint64(proto->SellPrice) * uint64(count);
        ++stacksSold;
        source->DestroyItem(bagSlot, itemSlot, true);
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
        uint32 itemsSold = 0;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            itemsSold += SellGrayFromSlot(target, INVENTORY_SLOT_BAG_0, slot, copper, stacksSold);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = target->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                itemsSold += SellGrayFromSlot(target, bagSlot, uint8(slot), copper, stacksSold);
        }

        if (copper > 0)
        {
            int64 money = target->GetMoney();
            uint64 capped = std::min<uint64>(copper, uint64(std::numeric_limits<int32>::max()));
            target->ModifyMoney(int32(capped));
        }

        SendProtocol(handler, Acore::StringFormat("BOTINV:SELL:GRAY:{}:{}:{}:{}", Sanitize(target->GetName()), itemsSold, stacksSold, copper));
        SendOk(handler, Acore::StringFormat("{} sold {} gray item(s) in {} stack(s) to {} for {} copper.",
            Sanitize(target->GetName()), itemsSold, stacksSold, Sanitize(vendor->GetName()), copper));

        SendBags(handler, manager, target);
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
        if (stored)
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
        SendProtocol(handler, "BOTINV:USAGE:.botinv bots | vendor set | target bags | target equipment | target equip <bag> <slot> | target unequip <equipSlot> | target take <bag> <slot> | target sell gray confirm | target destroy <bag> <slot> confirm | target deposit reagents | party deposit reagents | bank");
        handler->SendSysMessage("BotInventoryMaster commands:");
        handler->SendSysMessage(".botinv bots");
        handler->SendSysMessage(".botinv vendor set");
        handler->SendSysMessage(".botinv target bags");
        handler->SendSysMessage(".botinv target equipment");
        handler->SendSysMessage(".botinv target equip <bag> <slot>");
        handler->SendSysMessage(".botinv target unequip <equipSlot>");
        handler->SendSysMessage(".botinv target take <bag> <slot>");
        handler->SendSysMessage(".botinv target sell gray confirm");
        handler->SendSysMessage(".botinv target destroy <bag> <slot> confirm");
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

            if (tokens.size() >= 5 && BotInventoryMaster::ToLower(tokens[1]) == "destroy")
            {
                uint32 bag = 0;
                uint32 slot = 0;
                if (!BotInventoryMaster::TryParseUInt32(tokens[2], bag) || !BotInventoryMaster::TryParseUInt32(tokens[3], slot) ||
                    bag > std::numeric_limits<uint8>::max() || slot > std::numeric_limits<uint8>::max())
                {
                    BotInventoryMaster::SendError(handler, "Usage: .botinv target destroy <bag> <slot> confirm");
                    return true;
                }

                bool confirm = BotInventoryMaster::ToLower(tokens[4]) == "confirm";
                return BotInventoryMaster::HandleDestroyTarget(handler, manager, uint8(bag), uint8(slot), confirm);
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

            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "sell" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
            {
                bool confirm = BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleSellGrayTarget(handler, manager, confirm);
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
