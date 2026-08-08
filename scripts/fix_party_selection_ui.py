from pathlib import Path

p = Path('BotInventoryMasterUI/BotInventoryMasterUI.lua')
s = p.read_text(encoding='utf-8')

old = '''    for _, other in ipairs(bagButtons) do\n        if other.bag and other.slot then\n            other:SetChecked(selectedItems[tostring(other.bag) .. "," .. tostring(other.slot)] and true or false)\n        end\n    end\n'''
new = '''    for _, other in ipairs(bagButtons) do\n        if other.item then\n            other:SetChecked(selectedItems[ItemKey(other.item)] and true or false)\n        end\n    end\n'''
if s.count(old) != 1:
    raise RuntimeError(f'checkbox owner-key marker expected once, found {s.count(old)}')
s = s.replace(old, new, 1)

old = '''    if p[2] == "PBAG" and p[3] == "BEGIN" then\n        partyView = true\n        ClearBagGrid()\n        lastBotName = "Party"\n'''
new = '''    if p[2] == "PBAG" and p[3] == "BEGIN" then\n        partyView = true\n        ClearBagGrid()\n        ClearEquip()\n        equipLabel:SetText("Equipment (click a bot)")\n        lastBotName = "Party"\n'''
if s.count(old) != 1:
    raise RuntimeError(f'party begin marker expected once, found {s.count(old)}')
s = s.replace(old, new, 1)

old = '''    if p[2] == "BAG" and p[3] == "BEGIN" then\n        partyView = false\n        ClearBagGrid()\n        lastBotName = p[4] or "?"\n'''
new = '''    if p[2] == "BAG" and p[3] == "BEGIN" then\n        partyView = false\n        ClearBagGrid()\n        equipLabel:SetText("Equipment")\n        lastBotName = p[4] or "?"\n'''
if s.count(old) != 1:
    raise RuntimeError(f'target begin marker expected once, found {s.count(old)}')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('party selection UI fixed')
