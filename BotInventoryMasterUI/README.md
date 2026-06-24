# BotInventoryMasterUI v0.7

Updated for BotInventoryMaster v7 vendor safety.

Slash:

```text
/bim
/botinvui
```

New UI controls:

- `Sell Sel` sells the selected bot bag item to the selected vendor with a warning.
- `Buybacks` lists module buyback records for the remembered/targeted bot.
- `Buy Last` buys back the latest listed/sold item.
- `Equip Bag` attempts to equip the selected bag/container item on the bot.
- Alt-right-click a bag item opens the sell-selected warning.

Safety notes:

- The server blocks quest items, bags/containers from vendor selling, and no-sell-price items.
- Module buyback restores item entry/count only. It does not preserve enchants, gems, random stats, or unique item instance data.
