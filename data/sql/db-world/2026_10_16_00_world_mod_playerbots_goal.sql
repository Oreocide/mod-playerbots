-- mod-playerbots: what a bot is doing, and how each persona weights those kinds.
-- A missing table leaves today's status mix in place. Kind names are the C++ enum.

CREATE TABLE IF NOT EXISTS `playerbots_goal` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(64) NOT NULL,
  `kind` VARCHAR(16) NOT NULL,
  `weight` INT UNSIGNED NOT NULL DEFAULT 1,
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 255,
  `faction` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 both, 1 alliance, 2 horde',
  `class_mask` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bit (class id - 1), 0 = any',
  `race_mask` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bit (race id - 1), 0 = any',
  `map` INT NOT NULL DEFAULT 0 COMMENT '0 = any',
  `zone` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = any',
  `profession` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'skill line, 0 = any',
  `game_event` INT NOT NULL DEFAULT 0 COMMENT '0 = any',
  `hour_from` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `hour_to` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'equal to hour_from = all day',
  `weekday_mask` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bit 0 Sunday, 0 = any',
  `duration_min` INT UNSIGNED NOT NULL DEFAULT 5 COMMENT 'minutes',
  `duration_max` INT UNSIGNED NOT NULL DEFAULT 15,
  `cooldown_sec` INT UNSIGNED NOT NULL DEFAULT 0,
  `max_per_zone` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = unlimited, counts near bots only',
  `status` VARCHAR(16) NOT NULL DEFAULT '' COMMENT 'town: innkeeper, vendor, repair, banker, auctioneer, mailbox, flight',
  `chat_line` VARCHAR(128) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `playerbots_persona` (
  `persona` VARCHAR(16) NOT NULL,
  `kind` VARCHAR(16) NOT NULL,
  `multiplier` FLOAT NOT NULL DEFAULT 1,
  PRIMARY KEY (`persona`, `kind`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

DELETE FROM `playerbots_goal`;
INSERT INTO `playerbots_goal`
  (`name`,`kind`,`weight`,`min_level`,`max_level`,`duration_min`,`duration_max`,`cooldown_sec`,`max_per_zone`,`status`)
VALUES
  ('Innkeeper', 'town', 10, 1, 255, 1, 3, 600, 6, 'innkeeper'),
  ('Repair', 'town', 8, 10, 255, 1, 2, 900, 4, 'repair'),
  ('Kill quests', 'quest_hub', 20, 1, 89, 20, 40, 120, 0, ''),
  ('Fishing', 'fish', 6, 10, 255, 10, 20, 600, 0, ''),
  ('Afk', 'afk', 5, 1, 255, 5, 15, 300, 0, '');

UPDATE `playerbots_goal` SET `profession` = 356 WHERE `name` = 'Fishing';

DELETE FROM `playerbots_persona`;
INSERT INTO `playerbots_persona` (`persona`,`kind`,`multiplier`) VALUES
  ('quester', 'quest_hub', 2.0),
  ('quester', 'grind', 1.2),
  ('quester', 'town', 0.6),
  ('quester', 'afk', 0.4),
  ('crafter', 'town', 1.5),
  ('crafter', 'gather', 1.5),
  ('crafter', 'afk', 0.8),
  ('pvper', 'bg', 2.0),
  ('pvper', 'grind', 1.2),
  ('pvper', 'town', 0.6),
  ('socialite', 'town', 2.0),
  ('socialite', 'fish', 1.2),
  ('socialite', 'afk', 1.2),
  ('explorer', 'explore', 2.0),
  ('explorer', 'fish', 1.2),
  ('explorer', 'grind', 0.8),
  ('idler', 'afk', 2.0),
  ('idler', 'fish', 1.5),
  ('idler', 'town', 0.5);
