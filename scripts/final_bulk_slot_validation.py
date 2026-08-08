from pathlib import Path

p = Path('src/mod_bot_inventory_master.cpp')
s = p.read_text(encoding='utf-8')

old = '''    static void RemoveBulkItem(Player* target, BagSlotRef const& ref, bool selling, BulkCleanupStats& stats)\n    {\n        if (!target)\n            return;\n\n        Item* item = target->GetItemByPos(ref.Bag, ref.Slot);\n'''
new = '''    static bool IsValidBulkBagPosition(Player* target, BagSlotRef const& ref)\n    {\n        if (!target)\n            return false;\n\n        // Backpack storage slots only. Explicitly reject equipment, bank, keyring, buyback, etc.\n        if (ref.Bag == INVENTORY_SLOT_BAG_0)\n            return ref.Slot >= INVENTORY_SLOT_ITEM_START && ref.Slot < INVENTORY_SLOT_ITEM_END;\n\n        // Contents of equipped inventory bags only. The bag item itself is never a bulk target.\n        if (ref.Bag >= INVENTORY_SLOT_BAG_START && ref.Bag < INVENTORY_SLOT_BAG_END)\n        {\n            Bag* bag = target->GetBagByPos(ref.Bag);\n            return bag && ref.Slot < bag->GetBagSize();\n        }\n\n        return false;\n    }\n\n    static void RemoveBulkItem(Player* target, BagSlotRef const& ref, bool selling, BulkCleanupStats& stats)\n    {\n        if (!target)\n            return;\n        if (!IsValidBulkBagPosition(target, ref))\n        {\n            ++stats.SkippedStacks;\n            return;\n        }\n\n        Item* item = target->GetItemByPos(ref.Bag, ref.Slot);\n'''
count = s.count(old)
if count != 1:
    raise RuntimeError(f'RemoveBulkItem marker expected once, found {count}')
s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8')
print('bulk storage-position validation added')
