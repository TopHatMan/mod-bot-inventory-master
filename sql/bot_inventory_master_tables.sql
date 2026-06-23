-- Bot Inventory Master: manual table creation/checker.
-- The module creates these automatically on startup, but this file is useful for inspection.

CREATE TABLE IF NOT EXISTS `mod_bot_inventory_master_bank` (
    `account_id` INT UNSIGNED NOT NULL,
    `item_entry` INT UNSIGNED NOT NULL,
    `item_subclass` INT UNSIGNED NOT NULL,
    `amount` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`account_id`, `item_entry`),
    KEY `idx_mod_botinv_bank_subclass` (`account_id`, `item_subclass`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mod_bot_inventory_master_account_link` (
    `owner_account_id` INT UNSIGNED NOT NULL,
    `linked_account_id` INT UNSIGNED NOT NULL,
    `can_inventory` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `can_equipment` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `accepted` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`owner_account_id`, `linked_account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mod_bot_inventory_master_deposit_exclusion` (
    `item_entry` INT UNSIGNED NOT NULL,
    `comment` VARCHAR(255) NULL DEFAULT NULL,
    PRIMARY KEY (`item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

SELECT * FROM `mod_bot_inventory_master_bank`
ORDER BY `account_id`, `item_subclass`, `item_entry`
LIMIT 200;
