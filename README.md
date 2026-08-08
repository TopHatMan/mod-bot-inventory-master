# mod-bot-inventory-master

AzerothCore module prototype for controlling inventory of online playerbot/alt characters.

## Phase 3

This first version is intentionally server-command-first. The addon UI can be built on top of the emitted `BOTINV:*` protocol lines later.

Commands:

```text
.botinv
.botinv bots
.botinv target bags
.botinv target equipment
.botinv target equip <bag> <slot>
.botinv target unequip <equipSlot>
.botinv target take <bag> <slot>
.botinv target find <itemEntry>
.botinv vendor set
.botinv target sell gray confirm
.botinv target destroy gray confirm
.botinv party destroy gray confirm
.botinv target destroy <bag> <slot> confirm
.botinv target deposit reagents
.botinv party deposit reagents
.botinv bank
```

## Security model

By default, the manager can only control online characters on the same account.

Future table already exists for account linking:

```sql
mod_bot_inventory_master_account_link
```

but link/accept commands are not enabled yet.

## Storage

This module does **not** use or modify the standalone ReagentBankAccount module.

Deposited reagents go into:

```sql
characters.mod_bot_inventory_master_bank
```

## Install

Copy folder to:

```text
Z:\ac\modules\mod-bot-inventory-master
```

This simple-module package intentionally does **not** include a `CMakeLists.txt`; use the loader/source pattern your tree already supports.

Rebuild, then copy the conf dist into your server config folder as needed.

## Test

Target an online bot/alt on your account:

```text
.botinv bots
.botinv target bags
.botinv target equipment
.botinv target equip <bag> <slot>
.botinv target unequip <equipSlot>
.botinv target take <bag> <slot>
.botinv target find <itemEntry>
.botinv vendor set
.botinv target sell gray confirm
.botinv target destroy gray confirm
.botinv party destroy gray confirm
.botinv target destroy <bag> <slot> confirm
.botinv target deposit reagents
.botinv bank
```

With group bots:

```text
.botinv party deposit reagents
.botinv bank
```

## Next phases

- preview mode
- sell gray
- destroy gray with confirmation
- equipment scan
- equip/unequip commands
- account-link accept/revoke commands
- addon UI


## Phase 3 notes

- Equip uses normal server validation. If replacing an occupied slot fails, unequip the old slot first.
- Destroy is intentionally locked to gray-quality non-quest, non-container bag items.
- Equipment and bag records are sent as `BOTINV:EQUIP:*` and `BOTINV:BAG:*` for the addon UI.

## Phase 3 notes

No-cheat vendor selling:
- Target a real vendor NPC first and run `.botinv vendor set`.
- Target your bot/alt and run `.botinv target sell gray confirm
.botinv target destroy gray confirm
.botinv party destroy gray confirm`.
- The manager and bot must both be within configured trade/vendor distance of that vendor.
- Gray item sell copper is added to the bot, not magically to the manager.

Trade-like taking:
- `.botinv target take <bag> <slot>
.botinv target find <itemEntry>` moves a non-soulbound, non-quest, non-container bag item from the bot to your character.
- You must be near the bot and have bag space.

## Phase 4 safety patch

- Equip now verifies `EquipItem` returned an equipped item.
- If equip fails after removal, it attempts to roll the same Item* back into the bot's bags.
- Unequip and take now also verify StoreItem and attempt rollback.
- Added `.botinv target find <itemEntry>` to locate an item in the targeted bot's bags/equipment after testing.

## Phase 5.1 cleanup focus

This version focuses on the real pain point: freeing bot bags.

- `.botinv target destroy gray confirm` destroys all gray-quality items from the targeted bot.
- `.botinv party destroy gray confirm` destroys gray-quality items from all manageable group bots.
- `.botinv target sell gray confirm` now verifies each removed gray stack before adding vendor copper.
- If a stack is still present after destroy/sell, the command reports a verification failure instead of pretending it worked.

## Phase 5.1 targeting and refresh fix

- Scanning a bot's bags/equipment now remembers that bot server-side per manager.
- You can scan a rogue/druid, target a vendor, run `.botinv vendor set`, then sell gray without retargeting the bot.
- Bag begin protocol now includes bot money: `BOTINV:BAG:BEGIN:<bot>:<freeSlots>:<moneyCopper>`.
- Equipment begin protocol now includes bot money too.


## v7 vendor safety update

New commands:

```text
.botinv target sell <bag> <slot> confirm
.botinv target buyback list
.botinv target buyback <id>
.botinv target equipbag <bag> <slot>
```

Selected item selling:
- Requires `.botinv vendor set`.
- Requires bot and manager near the selected vendor.
- Blocks quest items.
- Blocks bags/containers from vendor selling.
- Blocks items with no vendor sell price.
- Adds an in-memory module buyback record.

Buyback:
- Restores item entry/count only.
- Does not preserve enchants, gems, random properties, durability, or unique item instance state.
- Intended as a safety net for accidental non-gray vendor sells.

Equip bag:
- Only allows container items.
- Uses the same loss-safe equip path as normal equip.
- The server's normal inventory checks decide whether the bag can be placed/replaced.


## v7 compilefix1

This AzerothCore branch's `Player::AddItem(entry, count)` returns `bool`, not `Item*`.
The module buyback restore path now checks the boolean result directly.


## v8 danger/no-warning item control

This version intentionally allows direct control over cluttered bot bags.

New behavior:
- `.botinv target sell <bag> <slot>` can sell white/green/blue items and bags.
- `.botinv target destroy <bag> <slot>` can destroy white/green/blue items and bags.
- The literal `confirm` word is optional when `BotInventoryMaster.Danger.RequireConfirm = 0`.
- Buyback records are created even for zero-copper vendor removes.

Danger notes:
- Quest items can be destroyed/sold if danger mode is enabled.
- Bags/containers can be destroyed/sold if danger mode is enabled.
- Destroying or selling a bag may remove the bag and whatever is inside it depending on core behavior.
- Equipment moving remains loss-safe: equip/unequip still validates and attempts rollback if storage/equip fails.


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
