-- mod-playerbots: one persona per bot, assigned once. Cleared with the character.

CREATE TABLE IF NOT EXISTS `playerbots_bot_persona` (
  `guid` INT UNSIGNED NOT NULL,
  `persona` TINYINT UNSIGNED NOT NULL COMMENT '0 quester, 1 crafter, 2 pvper, 3 socialite, 4 explorer, 5 idler',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
