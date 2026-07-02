local addonName = "BotInventoryMasterUI"
BotInventoryMasterDB = BotInventoryMasterDB or {}

local f = CreateFrame("Frame", "BotInventoryMasterUIFrame", UIParent)
f:SetSize(790, 590)
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
local lastBotMoneyCopper = 0
local lastBotFreeSlots = 0
local lastBotName = nil
local lastBuybackId = nil

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

local status = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
status:SetPoint("TOPLEFT", 24, -42)
status:SetText("Target bot. Right-click equip. Shift-right take. Ctrl-right destroy ANY. Alt-right sell ANY. No warnings.")

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
    if selectedBag and selectedSlot then SendCmd(".botinv target equip " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Unequip", 265, -62, 72, 22, function()
    if selectedEquipSlot then SendCmd(".botinv target unequip " .. selectedEquipSlot) end
end)
MakeButton(f, "Take", 341, -62, 55, 22, function()
    if selectedBag and selectedSlot then SendCmd(".botinv target take " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Find Item", 400, -62, 80, 22, function()
    if selectedBag and selectedSlot then
        local btn
        for _, b in ipairs(bagButtons) do if b.bag == selectedBag and b.slot == selectedSlot then btn = b end end
        if btn and btn.itemId then SendCmd(".botinv target find " .. btn.itemId) end
    end
end)
MakeButton(f, "Set Vendor", 484, -62, 88, 22, function() SendCmd(".botinv vendor set") end)
MakeButton(f, "Sell Gray", 576, -62, 82, 22, function() StaticPopup_Show("BOTINV_SELL_GRAY_CONFIRM") end)
MakeButton(f, "Sell Sel", 662, -62, 75, 22, function()
    if selectedBag and selectedSlot then SendCmd(".botinv target sell " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Bank", 24, -86, 55, 22, function() SendCmd(".botinv bank") end)
MakeButton(f, "Destroy Sel", 83, -86, 90, 22, function()
    if selectedBag and selectedSlot then SendCmd(".botinv target destroy " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Equip Bag", 177, -86, 82, 22, function()
    if selectedBag and selectedSlot then SendCmd(".botinv target equipbag " .. selectedBag .. " " .. selectedSlot) end
end)
MakeButton(f, "Buybacks", 263, -86, 78, 22, function() SendCmd(".botinv target buyback list") end)
MakeButton(f, "Buy Last", 345, -86, 76, 22, function()
    if lastBuybackId then SendCmd(".botinv target buyback " .. lastBuybackId) end
end)
MakeButton(f, "Deposit", 425, -86, 75, 22, function() SendCmd(".botinv target deposit reagents") end)
MakeButton(f, "Destroy Gray Bot", 504, -86, 115, 22, function()
    StaticPopup_Show("BOTINV_DESTROY_GRAY_TARGET_CONFIRM")
end)
MakeButton(f, "Destroy Gray Party", 623, -86, 125, 22, function()
    StaticPopup_Show("BOTINV_DESTROY_GRAY_PARTY_CONFIRM")
end)

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
bagLabel:SetPoint("TOPLEFT", 26, -116)
bagLabel:SetText("Bot Bags")

local equipLabel = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
equipLabel:SetPoint("TOPLEFT", 500, -116)
equipLabel:SetText("Equipment")

local log = CreateFrame("ScrollingMessageFrame", nil, f)
log:SetPoint("TOPLEFT", 24, -456)
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

local function SelectBagButton(btn)
    selectedBag = btn.bag
    selectedSlot = btn.slot
    for _, other in ipairs(bagButtons) do
        if other ~= btn then other:SetChecked(false) end
    end
    btn:SetChecked(true)
    status:SetText("Selected bag " .. tostring(selectedBag) .. " slot " .. tostring(selectedSlot) .. " | " .. (btn.itemName or "item"))
end

local function ClearBagGrid()
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

    b:SetScript("OnClick", function(self, button)
        if not self.bag or not self.slot then return end
        if button == "RightButton" then
            SelectBagButton(self)
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
            SelectBagButton(self)
        end
    end)

    b:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        if self.itemId then
            GameTooltip:SetHyperlink("item:" .. self.itemId)
            GameTooltip:AddLine("Left-click select", 0.6, 0.9, 1)
            GameTooltip:AddLine("Right-click equip", 0.6, 0.9, 1)
            GameTooltip:AddLine("Shift-right take/trade to you", 0.6, 0.9, 1)
            GameTooltip:AddLine("Ctrl-right destroy selected item directly", 0.6, 0.9, 1)
            GameTooltip:AddLine("Alt-right sell selected item directly", 1, 0.8, 0.3)
            GameTooltip:AddLine("Equip Bag button for bigger bags", 0.6, 1, 0.6)
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
    b:SetPoint("TOPLEFT", 26 + col * 42, -138 - row * 42)
    b:Hide()
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
    row:SetPoint("TOPLEFT", 500, -138 - i * 16)
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

local bagInsertIndex = 1

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

    if p[2] == "BOTS" and p[3] == "BEGIN" then ClearLog("Bots / manageable characters"); return end
    if p[2] == "BOTS" and p[3] == "END" then AddLine("End of bots.", 0.7, 0.7, 0.7); return end
    if p[2] == "BOT" then
        AddLine(string.format("%s | guid %s | acct %s | lvl %s | class %s | free %s | manageable %s",
            p[3] or "?", p[4] or "?", p[5] or "?", p[6] or "?", p[7] or "?", p[8] or "?", p[9] == "1" and "yes" or "no"))
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
    if f:IsShown() then f:Hide() else f:Show() end
end
