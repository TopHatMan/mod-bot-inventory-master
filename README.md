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
.botinv vendor set
.botinv target sell gray confirm
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
.botinv vendor set
.botinv target sell gray confirm
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
- Target your bot/alt and run `.botinv target sell gray confirm`.
- The manager and bot must both be within configured trade/vendor distance of that vendor.
- Gray item sell copper is added to the bot, not magically to the manager.

Trade-like taking:
- `.botinv target take <bag> <slot>` moves a non-soulbound, non-quest, non-container bag item from the bot to your character.
- You must be near the bot and have bag space.
