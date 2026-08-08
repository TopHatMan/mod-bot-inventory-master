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
'''        if (IsHardBulkProtectedEntry(proto->ItemId))
        {
            reason = "hard-protected utility item";
            return true;
        }
        if (uint32(proto->Quality) > g_bulkMaxQuality)
''',
'''        if (IsHardBulkProtectedEntry(proto->ItemId))
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
''', 'cpp extra bulk guards')
cpp_path.write_text(cpp, encoding='utf-8')

lua = lua_path.read_text(encoding='utf-8')
lua = replace_once(lua,
'''sellCheckedButton:SetScript("OnClick", function() SendSelectedBulk("sell") end)
destroyCheckedButton:SetScript("OnClick", function() SendSelectedBulk("destroy") end)
''',
'''sellCheckedButton:SetScript("OnClick", function() SendSelectedBulk("sell") end)
destroyCheckedButton:SetScript("OnClick", function()
    if SelectionCount() > 0 then StaticPopup_Show("BOTINV_BULK_DESTROY_CONFIRM")
    else UpdateSelectionStatus("nothing selected") end
end)
''', 'lua delete confirmation trigger')

lua = replace_once(lua,
'''StaticPopupDialogs["BOTINV_DESTROY_CONFIRM"] = {
''',
'''StaticPopupDialogs["BOTINV_BULK_DESTROY_CONFIRM"] = {
    text = "Delete all currently checked bulk-safe stacks? Quest items, bags, key utilities, class tools, and items above the server quality limit are protected.",
    button1 = "Delete Checked",
    button2 = "Cancel",
    OnAccept = function() SendSelectedBulk("destroy") end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

StaticPopupDialogs["BOTINV_DESTROY_CONFIRM"] = {
''', 'lua bulk popup')
lua_path.write_text(lua, encoding='utf-8')

readme = readme_path.read_text(encoding='utf-8')
readme += '''\nBulk cleanup also refuses items flagged `ITEM_FLAG_NO_USER_DESTROY`, key/quest bag-family items, and items with a non-zero `TotemCategory` (class/profession tools). `Delete Checked` asks for one confirmation before dispatching the selected batches.\n'''
readme_path.write_text(readme, encoding='utf-8')

print('final bulk safety guardrails applied')
