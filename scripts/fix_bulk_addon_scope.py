from pathlib import Path

p = Path('BotInventoryMasterUI/BotInventoryMasterUI.lua')
s = p.read_text(encoding='utf-8')

old = '''local partyBotCount = 0\nlocal lastBotMoneyCopper = 0\n'''
new = '''local partyBotCount = 0\nlocal UpdateSelectionStatus\nlocal RenderBagGrid\nlocal pageText\nlocal lastBotMoneyCopper = 0\n'''
if s.count(old) != 1:
    raise RuntimeError('state scope marker missing/ambiguous')
s = s.replace(old, new, 1)

old = '''local function UpdateSelectionStatus(extra)\n'''
new = '''UpdateSelectionStatus = function(extra)\n'''
if s.count(old) != 1:
    raise RuntimeError('UpdateSelectionStatus marker missing/ambiguous')
s = s.replace(old, new, 1)

old = '''function RenderBagGrid()\n'''
new = '''RenderBagGrid = function()\n'''
if s.count(old) != 1:
    raise RuntimeError('RenderBagGrid marker missing/ambiguous')
s = s.replace(old, new, 1)

# Sorting should return to page 1 so the user immediately sees the strongest/trashiest edge.
old = '''    sortMode = sortMode == "best" and "trash" or "best"\n    self:SetText(sortMode == "best" and "Sort: Best" or "Sort: Trash")\n'''
new = '''    sortMode = sortMode == "best" and "trash" or "best"\n    currentPage = 1\n    self:SetText(sortMode == "best" and "Sort: Best" or "Sort: Trash")\n'''
if s.count(old) != 1:
    raise RuntimeError('sort page marker missing/ambiguous')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('addon lexical scope fixed')
