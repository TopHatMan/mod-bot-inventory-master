from pathlib import Path


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)

cpp_path = Path('src/mod_bot_inventory_master.cpp')
lua_path = Path('BotInventoryMasterUI/BotInventoryMasterUI.lua')
readme_path = Path('README.md')

cpp = cpp_path.read_text(encoding='utf-8')

cpp = replace_once(cpp,
'''    struct BagSlotRef
    {
        uint8 Bag = 0;
        uint8 Slot = 0;
    };
''',
'''    struct BagSlotRef
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
''', 'owned slot struct')

party_cpp = r'''

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
'''

cpp = replace_once(cpp,
'''    static void ListBots(ChatHandler* handler, Player* manager)
''',
party_cpp + '''

    static void ListBots(ChatHandler* handler, Player* manager)
''', 'party C++ helpers')

cpp = replace_once(cpp,
'''        SendProtocol(handler, "BOTINV:USAGE:.botinv bots | use <botName> | vendor set | target bags | target equipment | target sellbatch <bag,slot;...> confirm | target destroybatch <bag,slot;...> confirm | target equip <bag> <slot> | target equipbag <bag> <slot> | target unequip <equipSlot> | target take <bag> <slot> | target find <itemEntry> | target sell gray confirm | target sell <bag> <slot> [confirm] | target buyback list | target buyback <id> | target destroy gray confirm | target destroy <bag> <slot> [confirm] | party destroy gray confirm | target deposit reagents | party deposit reagents | bank");
''',
'''        SendProtocol(handler, "BOTINV:USAGE:.botinv bots | use <botName> | vendor set | target bags | party bags | target sellbatch <bag,slot;...> confirm | target destroybatch <bag,slot;...> confirm | party sellbatch <Bot,bag,slot;...> confirm [refresh] | party destroybatch <Bot,bag,slot;...> confirm [refresh] | target equipment | target equip <bag> <slot> | target equipbag <bag> <slot> | target unequip <equipSlot> | target take <bag> <slot> | target find <itemEntry> | target sell gray confirm | target sell <bag> <slot> [confirm] | target buyback list | target buyback <id> | target destroy gray confirm | target destroy <bag> <slot> [confirm] | party destroy gray confirm | target deposit reagents | party deposit reagents | bank");
''', 'party usage protocol')

cpp = replace_once(cpp,
'''        handler->SendSysMessage(".botinv target bags");
        handler->SendSysMessage(".botinv target equipment");
''',
'''        handler->SendSysMessage(".botinv target bags");
        handler->SendSysMessage(".botinv party bags");
        handler->SendSysMessage(".botinv target equipment");
''', 'party usage bags')

cpp = replace_once(cpp,
'''        handler->SendSysMessage(".botinv target destroybatch <bag,slot;bag,slot;...> confirm");
        handler->SendSysMessage(".botinv party destroy gray confirm");
''',
'''        handler->SendSysMessage(".botinv target destroybatch <bag,slot;bag,slot;...> confirm");
        handler->SendSysMessage(".botinv party sellbatch <Bot,bag,slot;...> confirm [refresh]");
        handler->SendSysMessage(".botinv party destroybatch <Bot,bag,slot;...> confirm [refresh]");
        handler->SendSysMessage(".botinv party destroy gray confirm");
''', 'party usage batch')

cpp = replace_once(cpp,
'''        if (command == "party")
        {
            if (tokens.size() >= 4 &&
''',
'''        if (command == "party")
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
''', 'party parser')

cpp_path.write_text(cpp, encoding='utf-8')

lua = lua_path.read_text(encoding='utf-8')

lua = replace_once(lua,
'''local sortMode = "best"
local lastBotMoneyCopper = 0
''',
'''local sortMode = "best"
local partyView = false
local currentPage = 1
local PAGE_SIZE = 80
local partyBotCount = 0
local lastBotMoneyCopper = 0
''', 'lua party state')

lua = replace_once(lua,
'''local function ItemKey(item)
    return tostring(item.bag) .. "," .. tostring(item.slot)
end
''',
'''local function ItemKey(item)
    return tostring(item.ownerName or lastBotName or "?") .. ":" .. tostring(item.bag) .. "," .. tostring(item.slot)
end
''', 'lua owned item key')

lua = replace_once(lua,
'''        if b.bag and b.slot then b:SetChecked(selectedItems[tostring(b.bag) .. "," .. tostring(b.slot)] and true or false) end
''',
'''        if b.item then b:SetChecked(selectedItems[ItemKey(b.item)] and true or false) end
''', 'lua selection button keys')

old_sendbulk = '''local function SendSelectedBulk(action)
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
'''
new_sendbulk = '''local function SendSelectedBulk(action)
    local refs = {}
    for _, item in ipairs(bagItems) do
        if selectedItems[ItemKey(item)] and IsBulkSelectable(item) then
            if partyView then
                table.insert(refs, tostring(item.ownerName or "?") .. "," .. tostring(item.bag) .. "," .. tostring(item.slot))
            else
                table.insert(refs, tostring(item.bag) .. "," .. tostring(item.slot))
            end
        end
    end
    if #refs == 0 then
        UpdateSelectionStatus("nothing bulk-safe selected")
        return
    end

    -- Stay below the 3.3.5 chat message limit. Party refs include a bot name and use smaller chunks.
    local index = 1
    local commands = 0
    local chunkSize = partyView and 10 or 18
    local staged = {}
    while index <= #refs do
        local chunk = {}
        for _ = 1, chunkSize do
            if index > #refs then break end
            table.insert(chunk, refs[index])
            index = index + 1
        end
        table.insert(staged, chunk)
    end

    for i, chunk in ipairs(staged) do
        if partyView then
            local suffix = i == #staged and " confirm refresh" or " confirm"
            QueueBulkCmd(".botinv party " .. action .. "batch " .. table.concat(chunk, ";") .. suffix)
        else
            QueueBulkCmd(".botinv target " .. action .. "batch " .. table.concat(chunk, ";") .. " confirm")
        end
        commands = commands + 1
    end
    selectedItems = {}
    UpdateSelectionStatus("queued " .. tostring(#refs) .. " stack(s) in " .. tostring(commands) .. " batch command(s)")
end
'''
lua = replace_once(lua, old_sendbulk, new_sendbulk, 'lua party bulk sender')

lua = replace_once(lua,
'''MakeButton(f, "Clear", 412, -112, 54, 22, ClearSelection)

local botLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
botLabel:SetPoint("TOPLEFT", 472, -118)
botLabel:SetText("Bots: click Bots to refresh")
''',
'''MakeButton(f, "Clear", 412, -112, 54, 22, ClearSelection)
MakeButton(f, "Party Bags", 470, -112, 78, 22, function() SendCmd(".botinv party bags") end)

local botLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
botLabel:SetPoint("TOPLEFT", 552, -118)
botLabel:SetText("Bots")
''', 'lua party bags button')

lua = replace_once(lua,
'''        if button == "RightButton" then
            SelectBagButton(self, false)
            if IsShiftKeyDown() then
''',
'''        if button == "RightButton" then
            SelectBagButton(self, false)
            if partyView then
                UpdateSelectionStatus("right-click actions need a single bot; click that bot button first")
                return
            end
            if IsShiftKeyDown() then
''', 'lua party right click guard')

lua = replace_once(lua,
'''            GameTooltip:AddLine("Bot bag " .. tostring(self.bag) .. " slot " .. tostring(self.slot), 0.6, 0.9, 1)
''',
'''            if self.item and self.item.ownerName then GameTooltip:AddLine("Owner: " .. self.item.ownerName, 1, 0.82, 0.2) end
            GameTooltip:AddLine("Bot bag " .. tostring(self.bag) .. " slot " .. tostring(self.slot), 0.6, 0.9, 1)
''', 'lua owner tooltip')

old_render = '''    local sorted = SortedBagItems()
    for i, item in ipairs(sorted) do
        if i > #bagButtons then break end
        local b = GetBagButton(i)
        b.item = item
'''
new_render = '''    local sorted = SortedBagItems()
    local totalPages = math.max(1, math.ceil(#sorted / PAGE_SIZE))
    if currentPage > totalPages then currentPage = totalPages end
    if currentPage < 1 then currentPage = 1 end
    local startIndex = (currentPage - 1) * PAGE_SIZE + 1
    local endIndex = math.min(#sorted, startIndex + PAGE_SIZE - 1)
    local buttonIndex = 1
    for i = startIndex, endIndex do
        local item = sorted[i]
        local b = GetBagButton(buttonIndex)
        buttonIndex = buttonIndex + 1
        b.item = item
'''
lua = replace_once(lua, old_render, new_render, 'lua paged render start')

lua = replace_once(lua,
'''        b:SetChecked(selectedItems[ItemKey(item)] and true or false)
        b:Show()
    end
end
''',
'''        b:SetChecked(selectedItems[ItemKey(item)] and true or false)
        b:Show()
    end
    if pageText then pageText:SetText("Page " .. tostring(currentPage) .. "/" .. tostring(totalPages) .. " | " .. tostring(#sorted) .. " stacks") end
end
''', 'lua paged render end')

lua = replace_once(lua,
'''local bagLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
bagLabel:SetPoint("TOPLEFT", 26, -164)
bagLabel:SetText("Bot Bags")

local equipLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
''',
'''local bagLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
bagLabel:SetPoint("TOPLEFT", 26, -164)
bagLabel:SetText("Bot Bags")

pageText = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
pageText:SetPoint("TOPLEFT", 300, -166)
pageText:SetText("Page 1/1")
MakeButton(f, "<", 406, -158, 28, 20, function()
    if currentPage > 1 then currentPage = currentPage - 1; RenderBagGrid() end
end)
MakeButton(f, ">", 438, -158, 28, 20, function()
    currentPage = currentPage + 1
    RenderBagGrid()
end)

local equipLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
''', 'lua page controls')

lua = replace_once(lua,
'''local function ClearBagGrid()
    for i = 1, #bagButtons do
''',
'''local function ClearBagGrid()
    currentPage = 1
    for i = 1, #bagButtons do
''', 'lua page reset')

lua = replace_once(lua,
'''    if p[2] == "BAG" and p[3] == "BEGIN" then
        ClearBagGrid()
''',
'''    if p[2] == "BAG" and p[3] == "BEGIN" then
        partyView = false
        ClearBagGrid()
''', 'lua target mode begin')

lua = replace_once(lua,
'''        table.insert(bagItems, {
            bag = tonumber(p[5]) or 0,
''',
'''        table.insert(bagItems, {
            ownerName = lastBotName,
            bag = tonumber(p[5]) or 0,
''', 'lua target item owner')

pbag_protocol = r'''

    if p[2] == "PBAG" and p[3] == "BEGIN" then
        partyView = true
        ClearBagGrid()
        lastBotName = "Party"
        lastBotFreeSlots = 0
        lastBotMoneyCopper = 0
        lastBulkMaxQuality = tonumber(p[4]) or 2
        partyBotCount = 0
        bagLabel:SetText("Party Bags")
        UpdateSelectionStatus("loading all manageable group bots")
        return
    end

    if p[2] == "PBAG" and p[3] == "BOT" then
        partyBotCount = partyBotCount + 1
        lastBotFreeSlots = lastBotFreeSlots + (tonumber(p[5]) or 0)
        lastBotMoneyCopper = lastBotMoneyCopper + (tonumber(p[6]) or 0)
        return
    end

    if p[2] == "PBAG" and p[3] == "ITEM" then
        table.insert(bagItems, {
            ownerName = p[4] or "?",
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

    if p[2] == "PBAG" and p[3] == "END" then
        partyBotCount = tonumber(p[4]) or partyBotCount
        bagLabel:SetText("Party Bags - " .. tostring(partyBotCount) .. " bots")
        RenderBagGrid()
        UpdateSelectionStatus(sortMode == "best" and "party epic -> trash" or "party trash -> epic")
        AddLine("Party bags loaded: " .. tostring(partyBotCount) .. " manageable bots, " .. tostring(#bagItems) .. " occupied stacks.", 0.4, 0.9, 1.0)
        return
    end
'''

lua = replace_once(lua,
'''    if p[2] == "EQUIP" and p[3] == "BEGIN" then
''',
pbag_protocol + '''

    if p[2] == "EQUIP" and p[3] == "BEGIN" then
''', 'lua party protocol')

# Guard single-slot toolbar actions while looking at aggregate party bags.
for old, new, label in [
('''MakeButton(f, "Equip", 201, -62, 60, 22, function()\n    if selectedBag and selectedSlot then SendCmd(".botinv target equip " .. selectedBag .. " " .. selectedSlot) end\nend)\n''',
 '''MakeButton(f, "Equip", 201, -62, 60, 22, function()\n    if partyView then UpdateSelectionStatus("click a bot button before single-item equip"); return end\n    if selectedBag and selectedSlot then SendCmd(".botinv target equip " .. selectedBag .. " " .. selectedSlot) end\nend)\n''', 'lua equip guard'),
('''MakeButton(f, "Take", 341, -62, 55, 22, function()\n    if selectedBag and selectedSlot then SendCmd(".botinv target take " .. selectedBag .. " " .. selectedSlot) end\nend)\n''',
 '''MakeButton(f, "Take", 341, -62, 55, 22, function()\n    if partyView then UpdateSelectionStatus("click a bot button before single-item take"); return end\n    if selectedBag and selectedSlot then SendCmd(".botinv target take " .. selectedBag .. " " .. selectedSlot) end\nend)\n''', 'lua take guard'),
('''MakeButton(f, "Find Item", 400, -62, 80, 22, function()\n    if selectedBag and selectedSlot then\n''',
 '''MakeButton(f, "Find Item", 400, -62, 80, 22, function()\n    if partyView then UpdateSelectionStatus("click a bot button before single-item find"); return end\n    if selectedBag and selectedSlot then\n''', 'lua find guard'),
('''MakeButton(f, "Equip Bag", 187, -86, 82, 22, function()\n    if selectedBag and selectedSlot then SendCmd(".botinv target equipbag " .. selectedBag .. " " .. selectedSlot) end\nend)\n''',
 '''MakeButton(f, "Equip Bag", 187, -86, 82, 22, function()\n    if partyView then UpdateSelectionStatus("click a bot button before single-item bag equip"); return end\n    if selectedBag and selectedSlot then SendCmd(".botinv target equipbag " .. selectedBag .. " " .. selectedSlot) end\nend)\n''', 'lua bag equip guard')]:
    lua = replace_once(lua, old, new, label)

lua_path.write_text(lua, encoding='utf-8')

readme = readme_path.read_text(encoding='utf-8')
readme += r'''

### Party Bags: all managed bots in one view

Use the new **Party Bags** button (or `.botinv party bags`) to load the occupied bag stacks from every manageable online bot in your current group into one addon view.

- sorting and Gray/White/Green/Food selectors work across the whole party view
- selections can span multiple bots
- **Sell Checked** groups selected stacks by owner and credits each bot's own money
- **Delete Checked** removes the selected stacks from their actual owners
- the view is paged at 80 occupied stacks per page, while selection persists across pages
- each tooltip shows the owning bot
- direct right-click/equip/take actions are disabled in the aggregate view; click that bot's quick button first for one-character operations
- party batch commands refresh the combined view only after the final queued chunk, avoiding thousands of redundant protocol lines on large cleanups

Server protocol/commands:

```text
.botinv party bags
.botinv party sellbatch <BotName,bag,slot;...> confirm [refresh]
.botinv party destroybatch <BotName,bag,slot;...> confirm [refresh]
```
'''
readme_path.write_text(readme, encoding='utf-8')

print('party-wide bulk inventory view patch applied')
