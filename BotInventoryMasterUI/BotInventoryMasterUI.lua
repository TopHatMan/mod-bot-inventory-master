local addonName = "BotInventoryMasterUI"
BotInventoryMasterDB = BotInventoryMasterDB or {}

local f = CreateFrame("Frame", "BotInventoryMasterUIFrame", UIParent)
f:SetSize(790, 680)
f:SetPoint("CENTER")
f:SetMovable(true)
f:EnableMouse(true)
f:RegisterForDrag("LeftButton")
f:SetScript("OnDragStart", f.StartMoving)
f:SetScript("OnDragStop", f.StopMovingOrSizing)
f:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 }
})
f:Hide()

local title = f:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", 0, -14)
title:SetText("Bot Inventory Master")

local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -6, -6)

local selectedBag, selectedSlot = nil, nil
local selectedEquipSlot = nil
local bagButtons = {}
local equipRows = {}
local botButtons = {}
local bagItems = {}
local selectedItems = {}
local sortMode = "best"
local partyView = false
local currentPage = 1
local PAGE_SIZE = 80
local partyBotCount = 0
local UpdateSelectionStatus
local RenderBagGrid
local pageText
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

local function FormatMoney(copper)
    copper = tonumber(copper) or 0
    local gold = math.floor(copper / 10000)
    local silver = math.floor((copper % 10000) / 100)
    local c = copper % 100
    return string.format("%dg %ds %dc", gold, silver, c)
end


local function SendCmd(cmd)
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

local status = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
status:SetPoint("TOPLEFT", 24, -42)
status:SetText("Click items to multi-select. Bulk mode protects quest items, bags, key utilities, and gear above the configured quality ceiling.")

local function MakeButton(parent, text, x, y, w, h, onclick)
    local b = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    b:SetSize(w or 90, h or 22)
    b:SetPoint("TOPLEFT", x, y)
    b:SetText(text)
    b:SetScript("OnClick", onclick)
    return b
end

MakeButton(f, "Bots", 24, -62, 55, 22, function() SendCmd(".botinv bots") end)
MakeButton(f, "Bags", 83, -62, 55, 22, function() SendCmd(".botinv target bags") end)
MakeButton(f, "Gear", 142, -62, 55, 22, function() SendCmd(".botinv target equipment") end)
MakeButton(f, "Equip", 201, -62, 60, 22, function()
    if partyView then UpdateSelectionStatus("click a bot button before single-item equip"); return end
    if selectedBag and selectedSlot then SendCmd(".botinv target equip " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Unequip", 265, -62, 72, 22, function()
    if selectedEquipSlot then SendCmd(".botinv target unequip " .. selectedEquipSlot) end
end)
MakeButton(f, "Take", 341, -62, 55, 22, function()
    if partyView then UpdateSelectionStatus("click a bot button before single-item take"); return end
    if selectedBag and selectedSlot then SendCmd(".botinv target take " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Find Item", 400, -62, 80, 22, function()
    if partyView then UpdateSelectionStatus("click a bot button before single-item find"); return end
    if selectedBag and selectedSlot then
        local btn
        for _, b in ipairs(bagButtons) do if b.bag == selectedBag and b.slot == selectedSlot then btn = b end end
        if btn and btn.itemId then SendCmd(".botinv target find " .. btn.itemId) end
    end
end)
MakeButton(f, "Set Vendor", 484, -62, 88, 22, function() SendCmd(".botinv vendor set") end)
MakeButton(f, "Sell Gray", 576, -62, 82, 22, function() StaticPopup_Show("BOTINV_SELL_GRAY_CONFIRM") end)
local sellCheckedButton = MakeButton(f, "Sell Checked", 652, -62, 95, 22, function() end)
MakeButton(f, "Bank", 24, -86, 55, 22, function() SendCmd(".botinv bank") end)
local destroyCheckedButton = MakeButton(f, "Delete Checked", 83, -86, 100, 22, function() end)
MakeButton(f, "Equip Bag", 187, -86, 82, 22, function()
    if partyView then UpdateSelectionStatus("click a bot button before single-item bag equip"); return end
    if selectedBag and selectedSlot then SendCmd(".botinv target equipbag " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Buybacks", 273, -86, 78, 22, function() SendCmd(".botinv target buyback list") end)
MakeButton(f, "Buy Last", 355, -86, 76, 22, function()
    if lastBuybackId then SendCmd(".botinv target buyback " .. lastBuybackId) end
end)
MakeButton(f, "Deposit", 435, -86, 75, 22, function() SendCmd(".botinv target deposit reagents") end)
MakeButton(f, "Destroy Gray Bot", 514, -86, 110, 22, function()
    StaticPopup_Show("BOTINV_DESTROY_GRAY_TARGET_CONFIRM")
end)
MakeButton(f, "Destroy Gray Party", 628, -86, 120, 22, function()
    StaticPopup_Show("BOTINV_DESTROY_GRAY_PARTY_CONFIRM")
end)



local sortButton
local function ItemKey(item)
    return tostring(item.ownerName or lastBotName or "?") .. ":" .. tostring(item.bag) .. "," .. tostring(item.slot)
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

UpdateSelectionStatus = function(extra)
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
        if b.item then b:SetChecked(selectedItems[ItemKey(b.item)] and true or false) end
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

sellCheckedButton:SetScript("OnClick", function() SendSelectedBulk("sell") end)
destroyCheckedButton:SetScript("OnClick", function() SendSelectedBulk("destroy") end)

sortButton = MakeButton(f, "Sort: Best", 24, -112, 82, 22, function(self)
    sortMode = sortMode == "best" and "trash" or "best"
    currentPage = 1
    self:SetText(sortMode == "best" and "Sort: Best" or "Sort: Trash")
    if RenderBagGrid then RenderBagGrid() end
end)
MakeButton(f, "Gray", 110, -112, 52, 22, function() SelectWhere(function(i) return i.quality == 0 end) end)
MakeButton(f, "White", 166, -112, 52, 22, function() SelectWhere(function(i) return i.quality == 1 end) end)
MakeButton(f, "Green", 222, -112, 54, 22, function() SelectWhere(function(i) return i.quality == 2 end) end)
MakeButton(f, "<= Green", 280, -112, 70, 22, function() SelectWhere(function(i) return (i.quality or 0) <= 2 end) end)
MakeButton(f, "Food", 354, -112, 54, 22, function() SelectWhere(IsFood) end)
MakeButton(f, "Clear", 412, -112, 54, 22, ClearSelection)
MakeButton(f, "Party Bags", 470, -112, 78, 22, function() SendCmd(".botinv party bags") end)

local botLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
botLabel:SetPoint("TOPLEFT", 552, -118)
botLabel:SetText("Bots")

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


StaticPopupDialogs["BOTINV_DESTROY_CONFIRM"] = {
    text = "Destroy the selected gray item from the bot's bag?",
    button1 = "Destroy",
    button2 = "Cancel",
    OnAccept = function()
        if selectedBag and selectedSlot then
            SendCmd(".botinv target destroy " .. selectedBag .. " " .. selectedSlot .. " confirm")
        end
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}


StaticPopupDialogs["BOTINV_DESTROY_GRAY_TARGET_CONFIRM"] = {
    text = "Destroy ALL gray items from the targeted bot? This does not sell them.",
    button1 = "Destroy Gray",
    button2 = "Cancel",
    OnAccept = function()
        SendCmd(".botinv target destroy gray confirm")
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

StaticPopupDialogs["BOTINV_DESTROY_GRAY_PARTY_CONFIRM"] = {
    text = "Destroy ALL gray items from all manageable party bots? This does not sell them.",
    button1 = "Destroy Party Gray",
    button2 = "Cancel",
    OnAccept = function()
        SendCmd(".botinv party destroy gray confirm")
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

StaticPopupDialogs["BOTINV_SELL_GRAY_CONFIRM"] = {
    text = "Sell all gray items from the targeted bot to the selected vendor? Requires .botinv vendor set first.",
    button1 = "Sell Gray",
    button2 = "Cancel",
    OnAccept = function()
        SendCmd(".botinv target sell gray confirm")
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}


StaticPopupDialogs["BOTINV_SELL_SELECTED_CONFIRM"] = {
    text = "Sell selected item? v0.8 normally sends this directly; this popup is unused unless called by old code.",
    button1 = "Sell Item",
    button2 = "Cancel",
    OnAccept = function()
        if selectedBag and selectedSlot then
            SendCmd(".botinv target sell " .. selectedBag .. " " .. selectedSlot)
        end
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

local bagLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
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
equipLabel:SetPoint("TOPLEFT", 500, -164)
equipLabel:SetText("Equipment")

local log = CreateFrame("ScrollingMessageFrame", nil, f)
log:SetPoint("TOPLEFT", 24, -528)
log:SetPoint("BOTTOMRIGHT", -24, 26)
log:SetFontObject(GameFontHighlightSmall)
log:SetJustifyH("LEFT")
log:SetFading(false)
log:SetMaxLines(500)
log:EnableMouseWheel(true)
log:SetScript("OnMouseWheel", function(self, delta)
    if delta > 0 then self:ScrollUp() else self:ScrollDown() end
end)

local function AddLine(msg, r, g, b)
    log:AddMessage(msg, r or 1, g or 1, b or 1)
end

local function ClearLog(header)
    log:Clear()
    AddLine(header, 0.4, 0.9, 1.0)
end

local function SelectBagButton(btn, toggleBulk)
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

local function ClearBagGrid()
    currentPage = 1
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

local function GetBagButton(index)
    if bagButtons[index] then return bagButtons[index] end

    local b = CreateFrame("CheckButton", nil, f)
    b:SetSize(36, 36)
    b:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    b:SetNormalTexture("Interface\\Buttons\\UI-Quickslot2")
    b:SetPushedTexture("Interface\\Buttons\\UI-Quickslot-Depress")
    b:SetCheckedTexture("Interface\\Buttons\\CheckButtonHilight")
    b.icon = b:CreateTexture(nil, "ARTWORK")
    b.icon:SetPoint("CENTER")
    b.icon:SetSize(30, 30)
    b.count = b:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
    b.count:SetPoint("BOTTOMRIGHT", -2, 2)
    b.qualityText = b:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    b.qualityText:SetPoint("TOPLEFT", 2, -2)

    b:SetScript("OnClick", function(self, button)
        if not self.bag or not self.slot then return end
        if button == "RightButton" then
            SelectBagButton(self, false)
            if partyView then
                UpdateSelectionStatus("right-click actions need a single bot; click that bot button first")
                return
            end
            if IsShiftKeyDown() then
                SendCmd(".botinv target take " .. self.bag .. " " .. self.slot)
            elseif IsControlKeyDown() then
                SendCmd(".botinv target destroy " .. self.bag .. " " .. self.slot)
            elseif IsAltKeyDown() then
                SendCmd(".botinv target sell " .. self.bag .. " " .. self.slot)
            else
                SendCmd(".botinv target equip " .. self.bag .. " " .. self.slot)
            end
        else
            SelectBagButton(self, true)
        end
    end)

    b:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        if self.itemId then
            GameTooltip:SetHyperlink("item:" .. self.itemId)
            GameTooltip:AddLine("Left-click toggle bulk selection", 0.6, 0.9, 1)
            GameTooltip:AddLine("Bulk cleanup is capped by server quality/protection rules", 0.5, 1, 0.5)
            GameTooltip:AddLine("Right-click equip", 0.6, 0.9, 1)
            GameTooltip:AddLine("Shift-right take/trade to you", 0.6, 0.9, 1)
            GameTooltip:AddLine("Ctrl-right destroy selected item directly", 0.6, 0.9, 1)
            GameTooltip:AddLine("Alt-right sell selected item directly", 1, 0.8, 0.3)
            GameTooltip:AddLine("Equip Bag button for bigger bags", 0.6, 1, 0.6)
            if self.item and self.item.ownerName then GameTooltip:AddLine("Owner: " .. self.item.ownerName, 1, 0.82, 0.2) end
            GameTooltip:AddLine("Bot bag " .. tostring(self.bag) .. " slot " .. tostring(self.slot), 0.6, 0.9, 1)
        end
        GameTooltip:Show()
    end)
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)

    bagButtons[index] = b
    return b
end

for i = 1, 80 do
    local b = GetBagButton(i)
    local col = (i - 1) % 10
    local row = math.floor((i - 1) / 10)
    b:SetPoint("TOPLEFT", 26 + col * 42, -190 - row * 42)
    b:Hide()
end

RenderBagGrid = function()
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
    if pageText then pageText:SetText("Page " .. tostring(currentPage) .. "/" .. tostring(totalPages) .. " | " .. tostring(#sorted) .. " stacks") end
end

local slotNames = {
    [0] = "Head", [1] = "Neck", [2] = "Shoulder", [3] = "Shirt", [4] = "Chest",
    [5] = "Waist", [6] = "Legs", [7] = "Feet", [8] = "Wrist", [9] = "Hands",
    [10] = "Finger 1", [11] = "Finger 2", [12] = "Trinket 1", [13] = "Trinket 2",
    [14] = "Back", [15] = "Main Hand", [16] = "Off Hand", [17] = "Ranged", [18] = "Tabard"
}

local function ClearEquip()
    for i = 0, 18 do
        local row = equipRows[i]
        if row then
            row.itemId = nil
            row.itemName = nil
            row.text:SetText((slotNames[i] or ("Slot " .. i)) .. ": empty")
            row:SetBackdropColor(0, 0, 0, 0)
        end
    end
    selectedEquipSlot = nil
end

for i = 0, 18 do
    local row = CreateFrame("Button", nil, f)
    row:SetSize(255, 16)
    row:SetPoint("TOPLEFT", 500, -190 - i * 16)
    row:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
    row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.text:SetPoint("LEFT", 2, 0)
    row.text:SetText((slotNames[i] or ("Slot " .. i)) .. ": empty")
    row.slot = i
    row:SetScript("OnClick", function(self, button)
        selectedEquipSlot = self.slot
        status:SetText("Selected equipment slot " .. tostring(selectedEquipSlot) .. " | " .. (self.itemName or "empty"))
        if button == "RightButton" and self.itemId then
            SendCmd(".botinv target unequip " .. self.slot)
        end
    end)
    row:SetScript("OnEnter", function(self)
        if self.itemId then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink("item:" .. self.itemId)
            GameTooltip:AddLine("Right-click to unequip", 0.6, 0.9, 1)
            GameTooltip:Show()
        end
    end)
    row:SetScript("OnLeave", function() GameTooltip:Hide() end)
    equipRows[i] = row
end


local function Split(msg)
    local parts = {}
    for token in string.gmatch(msg, "([^:]+)") do table.insert(parts, token) end
    return parts
end

local function OnProtocol(msg)
    local p = Split(msg)

    if p[2] == "OK" then AddLine("OK: " .. string.sub(msg, 11), 0.3, 1, 0.3); return end
    if p[2] == "ERR" then AddLine("ERROR: " .. string.sub(msg, 12), 1, 0.2, 0.2); return end
    if p[2] == "VENDOR" and p[3] == "SET" then status:SetText("Vendor set: " .. (p[5] or ("entry " .. tostring(p[4])))); return end

    if p[2] == "BAG" and p[3] == "BEGIN" then
        partyView = false
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
            ownerName = lastBotName,
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


    if p[2] == "EQUIP" and p[3] == "BEGIN" then
        ClearEquip()
        lastBotName = p[4] or "?"
        lastBotMoneyCopper = tonumber(p[5]) or lastBotMoneyCopper
        status:SetText("Equipment view: " .. lastBotName .. " | money " .. FormatMoney(lastBotMoneyCopper))
        AddLine("Refreshing equipment: " .. lastBotName .. " | money " .. FormatMoney(lastBotMoneyCopper), 0.4, 0.9, 1.0)
        return
    end

    if p[2] == "EQUIP" and p[3] == "ITEM" then
        local slot = tonumber(p[5])
        local row = slot and equipRows[slot]
        if row then
            row.itemId = tonumber(p[7])
            row.itemName = p[11] or ("item " .. tostring(p[7]))
            row.text:SetText((slotNames[slot] or ("Slot " .. slot)) .. ": " .. row.itemName)
        end
        return
    end

    if p[2] == "EQUIP" and p[3] == "END" then AddLine("Loaded equipment.", 0.7, 0.7, 0.7); return end

    if p[2] == "BOTS" and p[3] == "BEGIN" then ClearLog("Bots / manageable characters"); ClearBotButtons(); return end
    if p[2] == "BOTS" and p[3] == "END" then AddLine("End of bots.", 0.7, 0.7, 0.7); return end
    if p[2] == "BOT" then
        AddLine(string.format("%s | guid %s | acct %s | lvl %s | class %s | free %s | manageable %s",
            p[3] or "?", p[4] or "?", p[5] or "?", p[6] or "?", p[7] or "?", p[8] or "?", p[9] == "1" and "yes" or "no"))
        AddBotButton(p[3], p[9])
        return
    end


    if p[2] == "FIND" and p[3] == "BEGIN" then
        ClearLog("Find item " .. (p[5] or "?") .. " on " .. (p[4] or "?"))
        return
    end
    if p[2] == "FIND" and p[3] == "ITEM" then
        AddLine(string.format("Found on %s: %s bag/slot %s/%s item %s x%s", p[4] or "?", p[5] or "?", p[6] or "?", p[7] or "?", p[8] or "?", p[9] or "?"), 0.8, 1, 0.8)
        return
    end
    if p[2] == "FIND" and p[3] == "END" then
        AddLine("End find.", 0.7, 0.7, 0.7)
        return
    end


    if p[2] == "DESTROY" and p[3] == "GRAY" then
        AddLine(string.format("%s destroyed %s gray item(s), %s stack(s).", p[4] or "?", p[5] or "0", p[6] or "0"), 1, 0.8, 0.4)
        return
    end

    if p[2] == "BULK" and p[3] == "DESTROY" then
        AddLine(string.format("%s bulk-deleted %s item(s) / %s stack(s); skipped %s, failed %s.", p[4] or "?", p[5] or "0", p[6] or "0", p[7] or "0", p[8] or "0"), 1, 0.6, 0.3)
        return
    end

    if p[2] == "BULK" and p[3] == "SELL" then
        AddLine(string.format("%s bulk-sold %s item(s) / %s stack(s); skipped %s, failed %s, %s copper.", p[4] or "?", p[5] or "0", p[6] or "0", p[7] or "0", p[8] or "0", p[9] or "0"), 0.5, 1, 0.5)
        return
    end

    if p[2] == "SELL" and p[3] == "GRAY" then
        AddLine(string.format("%s sold %s gray item(s), %s stack(s), failed %s, %s copper.", p[4] or "?", p[5] or "0", p[6] or "0", p[7] or "0", p[8] or "0"), 0.8, 1, 0.8)
        return
    end

    if p[2] == "SELL" and p[3] == "ITEM" then
        lastBuybackId = tonumber(p[5]) or lastBuybackId
        AddLine(string.format("%s sold item %s x%s for %s copper. Buyback id %s. %s", p[4] or "?", p[6] or "?", p[7] or "?", p[9] or "0", p[5] or "?", p[10] or ""), 1, 0.9, 0.4)
        return
    end

    if p[2] == "BUYBACK" and p[3] == "BEGIN" then
        AddLine("Buyback list for " .. (p[4] or "?") .. " | money " .. FormatMoney(tonumber(p[5]) or 0), 0.4, 0.9, 1)
        return
    end

    if p[2] == "BUYBACK" and p[3] == "ITEM" then
        lastBuybackId = tonumber(p[5]) or lastBuybackId
        AddLine(string.format("Buyback %s: item %s x%s quality %s cost %s | %s", p[5] or "?", p[6] or "?", p[7] or "?", p[8] or "?", p[9] or "0", p[10] or ""), 1, 0.9, 0.4)
        return
    end

    if p[2] == "BUYBACK" and p[3] == "OK" then
        AddLine(string.format("%s bought back item %s x%s for %s copper.", p[4] or "?", p[6] or "?", p[7] or "?", p[8] or "0"), 0.8, 1, 0.8)
        return
    end

    if p[2] == "BUYBACK" and p[3] == "END" then
        AddLine("End buyback list.", 0.7, 0.7, 0.7)
        return
    end

    if p[2] == "TAKE" then
        AddLine(string.format("Took %s x%s from %s | %s", p[5] or "?", p[6] or "?", p[3] or "?", p[7] or ""), 0.8, 1, 0.8)
        return
    end

    if p[2] == "BANK" and p[3] == "BEGIN" then ClearLog("Virtual account bank"); return end
    if p[2] == "BANK" and p[3] == "ITEM" then AddLine("item " .. (p[4] or "?") .. " x" .. (p[6] or "?") .. " | " .. (p[7] or "?")); return end
    if p[2] == "BANK" and p[3] == "END" then AddLine("End bank.", 0.7, 0.7, 0.7); return end

    AddLine(msg, 0.9, 0.9, 0.9)
end

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("CHAT_MSG_SYSTEM")
eventFrame:SetScript("OnEvent", function(self, event, msg)
    if type(msg) == "string" and string.sub(msg, 1, 7) == "BOTINV:" then OnProtocol(msg) end
end)

SLASH_BOTINVENTORYMASTERUI1 = "/botinvui"
SLASH_BOTINVENTORYMASTERUI2 = "/bim"
SlashCmdList["BOTINVENTORYMASTERUI"] = function()
    if f:IsShown() then f:Hide() else f:Show(); SendCmd(".botinv bots") end
end
