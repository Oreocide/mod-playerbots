-- mod-playerbots: bounded rings. Slot 0 is the newest. A bot reset deletes the guid.

CREATE TABLE IF NOT EXISTS `playerbots_memory` (
  `guid` INT UNSIGNED NOT NULL,
  `kind` VARCHAR(16) NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL,
  `key` VARCHAR(64) NOT NULL DEFAULT '',
  `payload` VARCHAR(255) NOT NULL DEFAULT '',
  `at` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `kind`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
