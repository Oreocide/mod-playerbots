-- mod-playerbots: retune the hotspots that are derived from the service NPCs of the world.
-- Any derived hotspot whose centre lies within `radius` of (map, x, y) takes the values of the row.
-- NULL tier/team keep what was derived, enabled = 0 removes the hotspot (a place bots must not gather).
CREATE TABLE IF NOT EXISTS `playerbots_hotspot_override` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `map` INT UNSIGNED NOT NULL,
  `x` FLOAT NOT NULL,
  `y` FLOAT NOT NULL,
  `radius` FLOAT NOT NULL DEFAULT 500,
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 255,
  `tier` TINYINT UNSIGNED NULL DEFAULT NULL,
  `team` TINYINT UNSIGNED NULL DEFAULT NULL,
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `comment` VARCHAR(128) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

DELETE FROM `playerbots_hotspot_override` WHERE `id` BETWEEN 1 AND 99;
INSERT INTO `playerbots_hotspot_override` (`id`,`map`,`x`,`y`,`radius`,`min_level`,`max_level`,`tier`,`team`,`enabled`,`comment`) VALUES
(1, 0, 8505, -4707, 600, 80, 255, NULL, NULL, 1, 'Silvermoon City (Midnight): an end game hub, level 80+');
