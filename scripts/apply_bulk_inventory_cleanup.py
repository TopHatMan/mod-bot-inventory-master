from pathlib import Path


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)

cpp_path = Path("src/mod_bot_inventory_master.cpp")
lua_path = Path("BotInventoryMasterUI/BotInventoryMasterUI.lua")
conf_path = Path("conf/BotInventoryMaster.conf.dist")
readme_path = Path("README.md")

cpp = cpp_path.read_text(encoding="utf-8")

cpp = replace_once(cpp,
'''    static bool g_allowSellSelected = true;
    static bool g_allowBuyback = true;
    static uint32 g_buybackMaxRecordsPerBot = 12;
''',
'''    static bool g_allowSellSelected = true;
    static bool g_allowBuyback = true;
    static uint32 g_buybackMaxRecordsPerBot = 12;

    // Bulk cleanup intentionally has stricter guardrails than the old single-item danger mode.
    // The addon can select many white/green trash items quickly without risking rare/epic gear,
    // quest items, bags, Hearthstones, or class totems in one accidental click.
    static uint32 g_bulkMaxQuality = ITEM_QUALITY_UNCOMMON;
    static uint32 g_bulkMaxItemsPerCommand = 24;
''', "cpp bulk globals")

cpp = replace_once(cpp,
'''    struct CleanupStats
    {
        uint32 Items = 0;
        uint32 Stacks = 0;
        uint32 FailedStacks = 0;
        uint64 Copper = 0;
    };
''',
'''    struct CleanupStats
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
''', "cpp bulk structs")

cpp = replace_once(cpp,
'''        g_allowSellSelected = sConfigMgr->GetOption<bool>("BotInventoryMaster.Vendor.AllowSellSelected", true);
        g_allowBuyback = sConfigMgr->GetOption<bool>("BotInventoryMaster.Vendor.AllowBuyback", true);
        g_buybackMaxRecordsPerBot = sConfigMgr->GetOption<uint32>("BotInventoryMaster.Vendor.BuybackMaxRecordsPerBot", 12);

        g_dangerAllowAnySell = sConfigMgr->GetOption<bool>("BotInventoryMaster.Danger.AllowAnySell", true);
''',
'''        g_allowSellSelected = sConfigMgr->GetOption<bool>("BotInventoryMaster.Vendor.AllowSellSelected", true);
        g_allowBuyback = sConfigMgr->GetOption<bool>("BotInventoryMaster.Vendor.AllowBuyback", true);
        g_buybackMaxRecordsPerBot = sConfigMgr->GetOption<uint32>("BotInventoryMaster.Vendor.BuybackMaxRecordsPerBot", 12);
        g_bulkMaxQuality = sConfigMgr->GetOption<uint32>("BotInventoryMaster.Bulk.MaxQuality", uint32(ITEM_QUALITY_UNCOMMON));
        g_bulkMaxItemsPerCommand = sConfigMgr->GetOption<uint32>("BotInventoryMaster.Bulk.MaxItemsPerCommand", 24);

        g_dangerAllowAnySell = sConfigMgr->GetOption<bool>("BotInventoryMaster.Danger.AllowAnySell", true);
''', "cpp bulk config load")

cpp = replace_once(cpp,
'''        if (g_buybackMaxRecordsPerBot > 40)
            g_buybackMaxRecordsPerBot = 40;

        if (g_requiredTradeDistance < 1.0f)
''',
'''        if (g_buybackMaxRecordsPerBot > 40)
            g_buybackMaxRecordsPerBot = 40;

        if (g_bulkMaxQuality > ITEM_QUALITY_EPIC)
            g_bulkMaxQuality = ITEM_QUALITY_EPIC;
        if (g_bulkMaxItemsPerCommand < 1)
            g_bulkMaxItemsPerCommand = 1;
        if (g_bulkMaxItemsPerCommand > 24)
            g_bulkMaxItemsPerCommand = 24;

        if (g_requiredTradeDistance < 1.0f)
''', "cpp bulk config clamp")

cpp = replace_once(cpp,
'''        SendProtocol(handler, Acore::StringFormat(
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
''',
'''        // Keep the original first fields stable for older addon builds, then append richer
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
''', "cpp richer item protocol")

cpp = replace_once(cpp,
'''        SendProtocol(handler, Acore::StringFormat("BOTINV:BAG:BEGIN:{}:{}:{}", Sanitize(target->GetName()), CountFreeBagSlots(target), target->GetMoney()));
''',
'''        SendProtocol(handler, Acore::StringFormat("BOTINV:BAG:BEGIN:{}:{}:{}:{}", Sanitize(target->GetName()), CountFreeBagSlots(target), target->GetMoney(), g_bulkMaxQuality));
''', "cpp bag begin max quality")

bulk_helpers = r'''

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

    static void RemoveBulkItem(Player* target, BagSlotRef const& ref, bool selling, BulkCleanupStats& stats)
    {
        if (!target)
            return;

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
'''

cpp = replace_once(cpp,
'''    static void ListBots(ChatHandler* handler, Player* manager)
''',
bulk_helpers + '''

    static void ListBots(ChatHandler* handler, Player* manager)
''', "cpp bulk helpers insertion")

cpp = replace_once(cpp,
'''        SendProtocol(handler, "BOTINV:USAGE:.botinv bots | vendor set | target bags | target equipment | target equip <bag> <slot> | target equipbag <bag> <slot> | target unequip <equipSlot> | target take <bag> <slot> | target find <itemEntry> | target sell gray confirm | target sell <bag> <slot> [confirm] | target buyback list | target buyback <id> | target destroy gray confirm | target destroy <bag> <slot> [confirm] | party destroy gray confirm | target deposit reagents | party deposit reagents | bank");
''',
'''        SendProtocol(handler, "BOTINV:USAGE:.botinv bots | use <botName> | vendor set | target bags | target equipment | target sellbatch <bag,slot;...> confirm | target destroybatch <bag,slot;...> confirm | target equip <bag> <slot> | target equipbag <bag> <slot> | target unequip <equipSlot> | target take <bag> <slot> | target find <itemEntry> | target sell gray confirm | target sell <bag> <slot> [confirm] | target buyback list | target buyback <id> | target destroy gray confirm | target destroy <bag> <slot> [confirm] | party destroy gray confirm | target deposit reagents | party deposit reagents | bank");
''', "cpp usage protocol")

cpp = replace_once(cpp,
'''        handler->SendSysMessage(".botinv bots");
        handler->SendSysMessage(".botinv vendor set");
''',
'''        handler->SendSysMessage(".botinv bots");
        handler->SendSysMessage(".botinv use <botName>");
        handler->SendSysMessage(".botinv vendor set");
''', "cpp usage use bot")

cpp = replace_once(cpp,
'''        handler->SendSysMessage(".botinv target sell gray confirm");
        handler->SendSysMessage(".botinv target sell <bag> <slot> [confirm]");
''',
'''        handler->SendSysMessage(".botinv target sell gray confirm");
        handler->SendSysMessage(".botinv target sellbatch <bag,slot;bag,slot;...> confirm");
        handler->SendSysMessage(".botinv target sell <bag> <slot> [confirm]");
''', "cpp usage sellbatch")

cpp = replace_once(cpp,
'''        handler->SendSysMessage(".botinv target destroy gray confirm");
        handler->SendSysMessage(".botinv party destroy gray confirm");
''',
'''        handler->SendSysMessage(".botinv target destroy gray confirm");
        handler->SendSysMessage(".botinv target destroybatch <bag,slot;bag,slot;...> confirm");
        handler->SendSysMessage(".botinv party destroy gray confirm");
''', "cpp usage destroybatch")

cpp = replace_once(cpp,
'''        if (command == "bank")
        {
            BotInventoryMaster::SendBank(handler, manager);
            return true;
        }

        if (command == "vendor")
''',
'''        if (command == "bank")
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
''', "cpp use parser")

cpp = replace_once(cpp,
'''            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "destroy" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
''',
'''            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "destroybatch")
            {
                bool confirm = tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleDestroyBatchTarget(handler, manager, tokens[2], confirm);
            }

            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "destroy" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
''', "cpp destroybatch parser")

cpp = replace_once(cpp,
'''            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "sell" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
''',
'''            if (tokens.size() >= 3 && BotInventoryMaster::ToLower(tokens[1]) == "sellbatch")
            {
                bool confirm = tokens.size() >= 4 && BotInventoryMaster::ToLower(tokens[3]) == "confirm";
                return BotInventoryMaster::HandleSellBatchTarget(handler, manager, tokens[2], confirm);
            }

            if (tokens.size() >= 4 &&
                BotInventoryMaster::ToLower(tokens[1]) == "sell" &&
                BotInventoryMaster::ToLower(tokens[2]) == "gray")
''', "cpp sellbatch parser")

cpp_path.write_text(cpp, encoding="utf-8")

lua = lua_path.read_text(encoding="utf-8")

lua = replace_once(lua, 'f:SetSize(790, 590)', 'f:SetSize(790, 680)', 'lua frame size')

lua = replace_once(lua,
'''local selectedBag, selectedSlot = nil, nil
local selectedEquipSlot = nil
local bagButtons = {}
local equipRows = {}
local lastBotMoneyCopper = 0
local lastBotFreeSlots = 0
local lastBotName = nil
local lastBuybackId = nil
''',
'''local selectedBag, selectedSlot = nil, nil
local selectedEquipSlot = nil
local bagButtons = {}
local equipRows = {}
local botButtons = {}
local bagItems = {}
local selectedItems = {}
local sortMode = "best"
local lastBotMoneyCopper = 0
local lastBotFreeSlots = 0
local lastBotName = nil
local lastBuybackId = nil
local lastBulkMaxQuality = 2
local botButtonCount = 0
local bulkQueue = {}
local bulkQueueElapsed = 0

local HARD_PROTECTED = { [6948]=true, [5175]=true, [5176]=true, [5177]=true, [5178]=true, [46978]=true }
local ITEM_CLASS_CONTAINER = 1
local ITEM_CLASS_QUEST = 12
local ITEM_CLASS_CONSUMABLE = 0
local ITEM_SUBCLASS_FOOD_DRINK = 5
''', "lua state")

lua = replace_once(lua,
'''local function SendCmd(cmd)
    SendChatMessage(cmd, "SAY")
end
''',
'''local function SendCmd(cmd)
    SendChatMessage(cmd, "SAY")
end

local bulkSender = CreateFrame("Frame")
bulkSender:SetScript("OnUpdate", function(self, elapsed)
    if #bulkQueue == 0 then return end
    bulkQueueElapsed = bulkQueueElapsed + elapsed
    if bulkQueueElapsed < 0.30 then return end
    bulkQueueElapsed = 0
    local cmd = table.remove(bulkQueue, 1)
    if cmd then SendCmd(cmd) end
end)

local function QueueBulkCmd(cmd)
    table.insert(bulkQueue, cmd)
end
''', "lua command queue")

lua = replace_once(lua,
'''status:SetText("Target bot. Right-click equip. Shift-right take. Ctrl-right destroy ANY. Alt-right sell ANY. No warnings.")
''',
'''status:SetText("Click items to multi-select. Bulk mode protects quest items, bags, key utilities, and gear above the configured quality ceiling.")
''', "lua status text")

lua = replace_once(lua,
'''MakeButton(f, "Sell Sel", 662, -62, 75, 22, function()
    if selectedBag and selectedSlot then SendCmd(".botinv target sell " .. selectedBag .. " " .. selectedSlot) end
end)
''',
'''local sellCheckedButton = MakeButton(f, "Sell Checked", 652, -62, 95, 22, function() end)
''', "lua sell checked button")

lua = replace_once(lua,
'''MakeButton(f, "Destroy Sel", 83, -86, 90, 22, function()
    if selectedBag and selectedSlot then SendCmd(".botinv target destroy " .. selectedBag .. " " .. selectedSlot) end
end)
''',
'''local destroyCheckedButton = MakeButton(f, "Delete Checked", 83, -86, 100, 22, function() end)
''', "lua destroy checked button")

lua = replace_once(lua, 'MakeButton(f, "Equip Bag", 177, -86, 82, 22', 'MakeButton(f, "Equip Bag", 187, -86, 82, 22', 'lua equip bag shift')
lua = replace_once(lua, 'MakeButton(f, "Buybacks", 263, -86, 78, 22', 'MakeButton(f, "Buybacks", 273, -86, 78, 22', 'lua buybacks shift')
lua = replace_once(lua, 'MakeButton(f, "Buy Last", 345, -86, 76, 22', 'MakeButton(f, "Buy Last", 355, -86, 76, 22', 'lua buy last shift')
lua = replace_once(lua, 'MakeButton(f, "Deposit", 425, -86, 75, 22', 'MakeButton(f, "Deposit", 435, -86, 75, 22', 'lua deposit shift')
lua = replace_once(lua, 'MakeButton(f, "Destroy Gray Bot", 504, -86, 115, 22', 'MakeButton(f, "Destroy Gray Bot", 514, -86, 110, 22', 'lua destroy gray bot shift')
lua = replace_once(lua, 'MakeButton(f, "Destroy Gray Party", 623, -86, 125, 22', 'MakeButton(f, "Destroy Gray Party", 628, -86, 120, 22', 'lua destroy party shift')

controls = r'''

local sortButton
local function ItemKey(item)
    return tostring(item.bag) .. "," .. tostring(item.slot)
end

local function IsBulkSelectable(item)
    if not item then return false end
    if item.classId == ITEM_CLASS_QUEST or item.classId == ITEM_CLASS_CONTAINER then return false end
    if HARD_PROTECTED[item.itemId or 0] then return false end
    return (item.quality or 0) <= (lastBulkMaxQuality or 2)
end

local function IsFood(item)
    return item and item.classId == ITEM_CLASS_CONSUMABLE and item.subClassId == ITEM_SUBCLASS_FOOD_DRINK
end

local function SelectionCount()
    local n = 0
    for _ in pairs(selectedItems) do n = n + 1 end
    return n
end

local function UpdateSelectionStatus(extra)
    local selected = SelectionCount()
    local bot = lastBotName or "no bot"
    local base = bot .. " | selected " .. selected .. " | free " .. tostring(lastBotFreeSlots) .. " | " .. FormatMoney(lastBotMoneyCopper)
    if extra and extra ~= "" then base = base .. " | " .. extra end
    status:SetText(base)
end

local function ClearSelection()
    selectedItems = {}
    for _, b in ipairs(bagButtons) do b:SetChecked(false) end
    UpdateSelectionStatus("selection cleared")
end

local function SelectWhere(predicate)
    selectedItems = {}
    for _, item in ipairs(bagItems) do
        if IsBulkSelectable(item) and predicate(item) then selectedItems[ItemKey(item)] = true end
    end
    for _, b in ipairs(bagButtons) do
        if b.bag and b.slot then b:SetChecked(selectedItems[tostring(b.bag) .. "," .. tostring(b.slot)] and true or false) end
    end
    UpdateSelectionStatus()
end

local function SortedBagItems()
    local sorted = {}
    for i, item in ipairs(bagItems) do sorted[i] = item end
    table.sort(sorted, function(a, b)
        local aq, bq = a.quality or 0, b.quality or 0
        if aq ~= bq then
            if sortMode == "trash" then return aq < bq else return aq > bq end
        end
        local an, bn = string.lower(a.itemName or ""), string.lower(b.itemName or "")
        if an ~= bn then return an < bn end
        if a.bag ~= b.bag then return a.bag < b.bag end
        return a.slot < b.slot
    end)
    return sorted
end

local function SendSelectedBulk(action)
    local refs = {}
    for _, item in ipairs(bagItems) do
        if selectedItems[ItemKey(item)] and IsBulkSelectable(item) then
            table.insert(refs, tostring(item.bag) .. "," .. tostring(item.slot))
        end
    end
    if #refs == 0 then
        UpdateSelectionStatus("nothing bulk-safe selected")
        return
    end

    -- Stay well below the 3.3.5 chat message limit and the server's per-command limit.
    local index = 1
    local commands = 0
    while index <= #refs do
        local chunk = {}
        for _ = 1, 18 do
            if index > #refs then break end
            table.insert(chunk, refs[index])
            index = index + 1
        end
        QueueBulkCmd(".botinv target " .. action .. "batch " .. table.concat(chunk, ";") .. " confirm")
        commands = commands + 1
    end
    selectedItems = {}
    UpdateSelectionStatus("queued " .. tostring(#refs) .. " stack(s) in " .. tostring(commands) .. " batch command(s)")
end

sellCheckedButton:SetScript("OnClick", function() SendSelectedBulk("sell") end)
destroyCheckedButton:SetScript("OnClick", function() SendSelectedBulk("destroy") end)

sortButton = MakeButton(f, "Sort: Best", 24, -112, 82, 22, function(self)
    sortMode = sortMode == "best" and "trash" or "best"
    self:SetText(sortMode == "best" and "Sort: Best" or "Sort: Trash")
    if RenderBagGrid then RenderBagGrid() end
end)
MakeButton(f, "Gray", 110, -112, 52, 22, function() SelectWhere(function(i) return i.quality == 0 end) end)
MakeButton(f, "White", 166, -112, 52, 22, function() SelectWhere(function(i) return i.quality == 1 end) end)
MakeButton(f, "Green", 222, -112, 54, 22, function() SelectWhere(function(i) return i.quality == 2 end) end)
MakeButton(f, "<= Green", 280, -112, 70, 22, function() SelectWhere(function(i) return (i.quality or 0) <= 2 end) end)
MakeButton(f, "Food", 354, -112, 54, 22, function() SelectWhere(IsFood) end)
MakeButton(f, "Clear", 412, -112, 54, 22, ClearSelection)

local botLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
botLabel:SetPoint("TOPLEFT", 472, -118)
botLabel:SetText("Bots: click Bots to refresh")

local function ClearBotButtons()
    botButtonCount = 0
    for _, b in ipairs(botButtons) do b:Hide() end
end

local function AddBotButton(name, manageable)
    if manageable ~= "1" or not name or name == "" then return end
    botButtonCount = botButtonCount + 1
    if botButtonCount > 5 then return end
    local b = botButtons[botButtonCount]
    if not b then
        b = CreateFrame("Button", nil, f, "UIPanelButtonTemplate")
        b:SetSize(52, 20)
        botButtons[botButtonCount] = b
    end
    b:SetPoint("TOPLEFT", 472 + (botButtonCount - 1) * 55, -134)
    b:SetText(string.sub(name, 1, 7))
    b.botName = name
    b:SetScript("OnClick", function(self) SendCmd(".botinv use " .. self.botName) end)
    b:Show()
end
'''

lua = replace_once(lua,
'''StaticPopupDialogs["BOTINV_DESTROY_CONFIRM"] = {
''',
controls + '''

StaticPopupDialogs["BOTINV_DESTROY_CONFIRM"] = {
''', "lua bulk controls")

lua = replace_once(lua, 'bagLabel:SetPoint("TOPLEFT", 26, -116)', 'bagLabel:SetPoint("TOPLEFT", 26, -164)', 'lua bag label position')
lua = replace_once(lua, 'equipLabel:SetPoint("TOPLEFT", 500, -116)', 'equipLabel:SetPoint("TOPLEFT", 500, -164)', 'lua equip label position')
lua = replace_once(lua, 'log:SetPoint("TOPLEFT", 24, -456)', 'log:SetPoint("TOPLEFT", 24, -528)', 'lua log position')

lua = replace_once(lua,
'''local function SelectBagButton(btn)
    selectedBag = btn.bag
    selectedSlot = btn.slot
    for _, other in ipairs(bagButtons) do
        if other ~= btn then other:SetChecked(false) end
    end
    btn:SetChecked(true)
    status:SetText("Selected bag " .. tostring(selectedBag) .. " slot " .. tostring(selectedSlot) .. " | " .. (btn.itemName or "item"))
end
''',
'''local function SelectBagButton(btn, toggleBulk)
    selectedBag = btn.bag
    selectedSlot = btn.slot
    if toggleBulk then
        local item = btn.item
        if IsBulkSelectable(item) then
            local key = ItemKey(item)
            if selectedItems[key] then selectedItems[key] = nil else selectedItems[key] = true end
        else
            UpdateSelectionStatus("this item is protected from bulk cleanup")
        end
    end
    for _, other in ipairs(bagButtons) do
        if other.bag and other.slot then
            other:SetChecked(selectedItems[tostring(other.bag) .. "," .. tostring(other.slot)] and true or false)
        end
    end
    UpdateSelectionStatus((btn.itemName or "item") .. " @ " .. tostring(selectedBag) .. "/" .. tostring(selectedSlot))
end
''', "lua multi select")

lua = replace_once(lua,
'''local function ClearBagGrid()
    for i = 1, #bagButtons do
        local b = bagButtons[i]
        b.itemId, b.itemName, b.bag, b.slot = nil, nil, nil, nil
        b.icon:SetTexture(nil)
        b.count:SetText("")
        b:SetChecked(false)
        b:Hide()
    end
    selectedBag, selectedSlot = nil, nil
end
''',
'''local function ClearBagGrid()
    for i = 1, #bagButtons do
        local b = bagButtons[i]
        b.itemId, b.itemName, b.bag, b.slot, b.item = nil, nil, nil, nil, nil
        b.icon:SetTexture(nil)
        b.count:SetText("")
        if b.qualityText then b.qualityText:SetText("") end
        b:SetChecked(false)
        b:Hide()
    end
    bagItems = {}
    selectedItems = {}
    selectedBag, selectedSlot = nil, nil
end
''', "lua clear grid")

lua = replace_once(lua,
'''    b.count = b:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
    b.count:SetPoint("BOTTOMRIGHT", -2, 2)

    b:SetScript("OnClick", function(self, button)
''',
'''    b.count = b:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
    b.count:SetPoint("BOTTOMRIGHT", -2, 2)
    b.qualityText = b:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    b.qualityText:SetPoint("TOPLEFT", 2, -2)

    b:SetScript("OnClick", function(self, button)
''', "lua quality label")

lua = replace_once(lua,
'''        if button == "RightButton" then
            SelectBagButton(self)
''',
'''        if button == "RightButton" then
            SelectBagButton(self, false)
''', "lua right click primary")

lua = replace_once(lua,
'''        else
            SelectBagButton(self)
        end
''',
'''        else
            SelectBagButton(self, true)
        end
''', "lua left click toggle")

lua = replace_once(lua,
'''            GameTooltip:AddLine("Left-click select", 0.6, 0.9, 1)
''',
'''            GameTooltip:AddLine("Left-click toggle bulk selection", 0.6, 0.9, 1)
            GameTooltip:AddLine("Bulk cleanup is capped by server quality/protection rules", 0.5, 1, 0.5)
''', "lua tooltip bulk")

lua = replace_once(lua,
'''for i = 1, 80 do
    local b = GetBagButton(i)
    local col = (i - 1) % 10
    local row = math.floor((i - 1) / 10)
    b:SetPoint("TOPLEFT", 26 + col * 42, -138 - row * 42)
    b:Hide()
end
''',
'''for i = 1, 80 do
    local b = GetBagButton(i)
    local col = (i - 1) % 10
    local row = math.floor((i - 1) / 10)
    b:SetPoint("TOPLEFT", 26 + col * 42, -190 - row * 42)
    b:Hide()
end

function RenderBagGrid()
    for i = 1, #bagButtons do
        local b = bagButtons[i]
        b.itemId, b.itemName, b.bag, b.slot, b.item = nil, nil, nil, nil, nil
        b.icon:SetTexture(nil)
        b.count:SetText("")
        if b.qualityText then b.qualityText:SetText("") end
        b:SetChecked(false)
        b:Hide()
    end

    local sorted = SortedBagItems()
    for i, item in ipairs(sorted) do
        if i > #bagButtons then break end
        local b = GetBagButton(i)
        b.item = item
        b.bag = item.bag
        b.slot = item.slot
        b.itemId = item.itemId
        b.itemName = item.itemName
        b.quality = item.quality
        b.classId = item.classId
        b.subClassId = item.subClassId
        b.icon:SetTexture(GetItemIcon(item.itemId or 0))
        b.count:SetText((item.count or 0) > 1 and tostring(item.count) or "")
        local q = item.quality or 0
        local marks = { [0]="J", [1]="C", [2]="U", [3]="R", [4]="E" }
        b.qualityText:SetText(marks[q] or tostring(q))
        if GetItemQualityColor then
            local r, g, bl = GetItemQualityColor(q)
            b.qualityText:SetTextColor(r or 1, g or 1, bl or 1)
        end
        b:SetChecked(selectedItems[ItemKey(item)] and true or false)
        b:Show()
    end
end
''', "lua render sorted grid")

lua = replace_once(lua,
'''    row:SetPoint("TOPLEFT", 500, -138 - i * 16)
''',
'''    row:SetPoint("TOPLEFT", 500, -190 - i * 16)
''', "lua equipment position")

lua = replace_once(lua, 'local bagInsertIndex = 1\n', '', 'lua remove bag insert')

lua = replace_once(lua,
'''    if p[2] == "BAG" and p[3] == "BEGIN" then
        ClearBagGrid()
        bagInsertIndex = 1
        lastBotName = p[4] or "?"
        lastBotFreeSlots = tonumber(p[5]) or 0
        lastBotMoneyCopper = tonumber(p[6]) or 0
        status:SetText("Bag view: " .. lastBotName .. " | free slots " .. lastBotFreeSlots .. " | money " .. FormatMoney(lastBotMoneyCopper))
        AddLine("Refreshing bags: " .. lastBotName .. " | free " .. lastBotFreeSlots .. " | money " .. FormatMoney(lastBotMoneyCopper), 0.4, 0.9, 1.0)
        return
    end

    if p[2] == "BAG" and p[3] == "ITEM" then
        local b = GetBagButton(bagInsertIndex)
        bagInsertIndex = bagInsertIndex + 1
        b:Show()
        b.bag = tonumber(p[5])
        b.slot = tonumber(p[6])
        b.itemId = tonumber(p[7])
        b.quality = tonumber(p[9]) or 0
        b.itemName = p[11] or ("item " .. tostring(p[7]))
        b.icon:SetTexture(GetItemIcon(b.itemId or 0))
        local count = tonumber(p[8]) or 0
        b.count:SetText(count > 1 and tostring(count) or "")
        b:SetChecked(false)
        return
    end

    if p[2] == "BAG" and p[3] == "END" then
        AddLine("Bag grid updated for " .. (lastBotName or "?") .. " | free " .. tostring(lastBotFreeSlots) .. " | money " .. FormatMoney(lastBotMoneyCopper), 0.7, 0.7, 0.7)
        return
    end
''',
'''    if p[2] == "BAG" and p[3] == "BEGIN" then
        ClearBagGrid()
        lastBotName = p[4] or "?"
        lastBotFreeSlots = tonumber(p[5]) or 0
        lastBotMoneyCopper = tonumber(p[6]) or 0
        lastBulkMaxQuality = tonumber(p[7]) or 2
        bagLabel:SetText("Bot Bags - " .. lastBotName)
        UpdateSelectionStatus("bulk max quality " .. tostring(lastBulkMaxQuality))
        AddLine("Refreshing bags: " .. lastBotName .. " | free " .. lastBotFreeSlots .. " | money " .. FormatMoney(lastBotMoneyCopper), 0.4, 0.9, 1.0)
        return
    end

    if p[2] == "BAG" and p[3] == "ITEM" then
        table.insert(bagItems, {
            bag = tonumber(p[5]) or 0,
            slot = tonumber(p[6]) or 0,
            itemId = tonumber(p[7]) or 0,
            count = tonumber(p[8]) or 0,
            quality = tonumber(p[9]) or 0,
            sellPrice = tonumber(p[10]) or 0,
            itemName = p[11] or ("item " .. tostring(p[7])),
            classId = tonumber(p[12]) or -1,
            subClassId = tonumber(p[13]) or -1,
            inventoryType = tonumber(p[14]) or 0,
        })
        return
    end

    if p[2] == "BAG" and p[3] == "END" then
        RenderBagGrid()
        UpdateSelectionStatus(sortMode == "best" and "epic -> trash" or "trash -> epic")
        AddLine("Bag grid updated for " .. (lastBotName or "?") .. " | free " .. tostring(lastBotFreeSlots) .. " | money " .. FormatMoney(lastBotMoneyCopper), 0.7, 0.7, 0.7)
        return
    end
''', "lua bag protocol")

lua = replace_once(lua,
'''            row.itemId = tonumber(p[7])
            row.itemName = p[11] or ("item " .. tostring(p[7]))
''',
'''            row.itemId = tonumber(p[7])
            row.itemName = p[11] or ("item " .. tostring(p[7]))
''', "lua equip protocol stable marker")

lua = replace_once(lua,
'''    if p[2] == "BOTS" and p[3] == "BEGIN" then ClearLog("Bots / manageable characters"); return end
''',
'''    if p[2] == "BOTS" and p[3] == "BEGIN" then ClearLog("Bots / manageable characters"); ClearBotButtons(); return end
''', "lua clear bots")

lua = replace_once(lua,
'''    if p[2] == "BOT" then
        AddLine(string.format("%s | guid %s | acct %s | lvl %s | class %s | free %s | manageable %s",
            p[3] or "?", p[4] or "?", p[5] or "?", p[6] or "?", p[7] or "?", p[8] or "?", p[9] == "1" and "yes" or "no"))
        return
    end
''',
'''    if p[2] == "BOT" then
        AddLine(string.format("%s | guid %s | acct %s | lvl %s | class %s | free %s | manageable %s",
            p[3] or "?", p[4] or "?", p[5] or "?", p[6] or "?", p[7] or "?", p[8] or "?", p[9] == "1" and "yes" or "no"))
        AddBotButton(p[3], p[9])
        return
    end
''', "lua bot buttons")

lua = replace_once(lua,
'''    if p[2] == "SELL" and p[3] == "GRAY" then
''',
'''    if p[2] == "BULK" and p[3] == "DESTROY" then
        AddLine(string.format("%s bulk-deleted %s item(s) / %s stack(s); skipped %s, failed %s.", p[4] or "?", p[5] or "0", p[6] or "0", p[7] or "0", p[8] or "0"), 1, 0.6, 0.3)
        return
    end

    if p[2] == "BULK" and p[3] == "SELL" then
        AddLine(string.format("%s bulk-sold %s item(s) / %s stack(s); skipped %s, failed %s, %s copper.", p[4] or "?", p[5] or "0", p[6] or "0", p[7] or "0", p[8] or "0", p[9] or "0"), 0.5, 1, 0.5)
        return
    end

    if p[2] == "SELL" and p[3] == "GRAY" then
''', "lua bulk result protocol")

lua = replace_once(lua,
'''SlashCmdList["BOTINVENTORYMASTERUI"] = function()
    if f:IsShown() then f:Hide() else f:Show() end
end
''',
'''SlashCmdList["BOTINVENTORYMASTERUI"] = function()
    if f:IsShown() then f:Hide() else f:Show(); SendCmd(".botinv bots") end
end
''', "lua open refresh bots")

lua_path.write_text(lua, encoding="utf-8")

conf = conf_path.read_text(encoding="utf-8")
conf += r'''

#
# Bulk cleanup guardrails
#
# Multi-select sell/delete is intentionally safer than single-item Danger mode.
# 0=poor/gray, 1=common/white, 2=uncommon/green, 3=rare/blue, 4=epic/purple.
# Default 2 lets the addon bulk-clean gray/white/green clutter while protecting blue+ gear.
BotInventoryMaster.Bulk.MaxQuality = 2

# Hard cap per one chat command. The addon automatically chunks larger selections.
# Keep <=24 so commands remain safely below the WoW 3.3.5 chat-length limit.
BotInventoryMaster.Bulk.MaxItemsPerCommand = 24

# Bulk actions always protect quest items, bags/containers, Hearthstone, and Shaman totems even
# when Danger.AllowAnySell/Destroy is enabled. Single-item danger actions remain unchanged.
'''
conf_path.write_text(conf, encoding="utf-8")

readme = readme_path.read_text(encoding="utf-8")
readme += r'''

## v9 bulk inventory cleanup UI

This pass targets multi-box/playerbot bag clutter directly.

Addon changes:
- normal left-click now toggles **multi-selection** instead of forcing one selected item
- **Sell Checked** and **Delete Checked** send selected stacks in compact batch commands
- large selections are automatically chunked to stay under the WoW 3.3.5 chat-message limit
- **Sort: Best / Sort: Trash** changes only the addon view; it never physically rearranges bot bags
- quick selectors: Gray, White, Green, <= Green, Food, Clear
- quality marker on each bag icon: J/C/U/R/E
- `.botinv bots` now creates up to five clickable bot buttons; clicking one uses `.botinv use <name>` so a five-character party can be managed without retargeting every character in the world

Server commands added:

```text
.botinv use <botName>
.botinv target sellbatch <bag,slot;bag,slot;...> confirm
.botinv target destroybatch <bag,slot;bag,slot;...> confirm
```

Bulk safety is intentionally stricter than the existing v8 single-item Danger mode:
- quest items are always skipped
- bags/containers are always skipped
- Hearthstone and core Shaman totems are always skipped
- items above `BotInventoryMaster.Bulk.MaxQuality` are always skipped (default: green/uncommon and below)
- bulk vendor sell skips zero-price items rather than deleting them for zero copper
- every removed stack is verified before it counts as success
- bulk sell creates the same module buyback records used by selected-item selling

The existing right-click shortcuts remain single-item actions, so deliberate one-off danger-mode control still works independently of bulk cleanup.
'''
readme_path.write_text(readme, encoding="utf-8")

print("bulk inventory cleanup patch applied")
