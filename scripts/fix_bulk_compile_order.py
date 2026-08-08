from pathlib import Path

p = Path('src/mod_bot_inventory_master.cpp')
s = p.read_text(encoding='utf-8')
marker = '''    static bool IsHardBulkProtectedEntry(uint32 entry)\n'''
replacement = '''    // Forward declarations for vendor helpers implemented later in this file.\n    // Bulk cleanup lives near the item/protocol helpers but reuses the established vendor checks.\n    static bool IsTradeDistanceOk(Player* a, WorldObject* b);\n    static Creature* GetSelectedVendor(ChatHandler* handler, Player* manager, std::string& reason);\n\n    static bool IsHardBulkProtectedEntry(uint32 entry)\n'''
count = s.count(marker)
if count != 1:
    raise RuntimeError(f'bulk helper marker expected once, found {count}')
s = s.replace(marker, replacement, 1)
p.write_text(s, encoding='utf-8')
print('bulk compile-order declarations added')
