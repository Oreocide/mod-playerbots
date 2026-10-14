-- mod-playerbots: tables, same names/columns as AzerothCore mod-playerbots' acore_playerbots database, kept in `characters`.
-- Wired to code: playerbots_db_store, playerbots_random_bots, playerbots_account_type, playerbots_names, playerbots_quest_blacklist.
-- Created empty and not used by the module yet (their AzerothCore seed data is 3.3.5 specific): everything else.

-- ---- runtime state ----

CREATE TABLE IF NOT EXISTS `playerbots_db_store` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `guid` INT NOT NULL,
  `key` VARCHAR(32) NOT NULL,
  `value` VARCHAR(255) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Per bot persisted strategies and values';

CREATE TABLE IF NOT EXISTS `playerbots_random_bots` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `owner` INT NOT NULL,
  `bot` INT NOT NULL,
  `time` INT NOT NULL,
  `validIn` INT DEFAULT NULL,
  `event` VARCHAR(45) DEFAULT NULL,
  `value` INT DEFAULT NULL,
  `data` VARCHAR(255) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `owner` (`owner`),
  KEY `bot` (`bot`),
  KEY `event` (`event`),
  KEY `idx_owner_bot_event` (`owner`,`bot`,`event`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Random bot event and scheduler state';

CREATE TABLE IF NOT EXISTS `playerbots_account_type` (
  `account_id` INT UNSIGNED NOT NULL,
  `account_type` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = unassigned, 1 = RNDbot, 2 = AddClass',
  `assignment_date` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbot account type assignments';

CREATE TABLE IF NOT EXISTS `playerbots_account_links` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `account_id` INT NOT NULL,
  `linked_account_id` INT NOT NULL,
  `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `account_link` (`account_id`,`linked_account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_account_keys` (
  `account_id` INT NOT NULL,
  `security_key` VARCHAR(255) NOT NULL,
  `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_custom_strategy` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(255) NOT NULL,
  `idx` INT DEFAULT NULL,
  `owner` INT DEFAULT NULL,
  `action_line` VARCHAR(1024) NOT NULL,
  PRIMARY KEY (`id`),
  KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_guild_tasks` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `owner` INT NOT NULL,
  `guildid` INT NOT NULL,
  `time` INT NOT NULL,
  `validIn` INT DEFAULT NULL,
  `type` VARCHAR(45) DEFAULT NULL,
  `value` INT DEFAULT NULL,
  `data` VARCHAR(255) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `owner` (`owner`),
  KEY `guildid` (`guildid`),
  KEY `type` (`type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_preferred_mounts` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `guid` INT NOT NULL,
  `type` TINYINT NOT NULL COMMENT '0: Ground, 1: Flying',
  `spellid` INT NOT NULL,
  PRIMARY KEY (`id`),
  KEY `guid` (`guid`),
  KEY `type` (`type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---- static lists ----

CREATE TABLE IF NOT EXISTS `playerbots_names` (
  `name_id` INT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(12) NOT NULL,
  `gender` TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`name_id`),
  UNIQUE KEY `name` (`name`,`gender`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbot random bot names, `gender` is a name category: (race group * 2) + gender, groups 0 generic (human, undead, others), 1 gnome, 2 dwarf, 3 night elf, 4 draenei, 5 orc, 6 troll, 7 tauren, 8 blood elf';

CREATE TABLE IF NOT EXISTS `playerbots_guild_names` (
  `name_id` INT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(24) NOT NULL,
  PRIMARY KEY (`name_id`),
  UNIQUE KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbot guild names';

CREATE TABLE IF NOT EXISTS `playerbots_arena_team_names` (
  `name_id` MEDIUMINT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(24) NOT NULL,
  `type` TINYINT NOT NULL,
  PRIMARY KEY (`name_id`),
  UNIQUE KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbot arena team names';

CREATE TABLE IF NOT EXISTS `playerbots_quest_blacklist` (
  `quest_id` INT UNSIGNED NOT NULL,
  `reason` VARCHAR(64) DEFAULT NULL,
  PRIMARY KEY (`quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Quest IDs bots skip when questing';

CREATE TABLE IF NOT EXISTS `ai_playerbot_texts` (
  `id` SMALLINT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(255) NOT NULL COMMENT 'name - used in strategies/code as filter',
  `text` VARCHAR(1024) NOT NULL COMMENT 'text',
  `say_type` TINYINT NOT NULL DEFAULT 0 COMMENT '0 - say, 1 - yell',
  `reply_type` TINYINT NOT NULL DEFAULT 0 COMMENT 'if > 0 then can be filtered as a response to chat',
  `text_loc1` VARCHAR(1024) NOT NULL DEFAULT '',
  `text_loc2` VARCHAR(1024) NOT NULL DEFAULT '',
  `text_loc3` VARCHAR(1024) NOT NULL DEFAULT '',
  `text_loc4` VARCHAR(1024) NOT NULL DEFAULT '',
  `text_loc5` VARCHAR(1024) NOT NULL DEFAULT '',
  `text_loc6` VARCHAR(1024) NOT NULL DEFAULT '',
  `text_loc7` VARCHAR(1024) NOT NULL DEFAULT '',
  `text_loc8` VARCHAR(1024) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC COMMENT='Bot chatter lines';

CREATE TABLE IF NOT EXISTS `ai_playerbot_texts_chance` (
  `id` BIGINT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(255) NOT NULL,
  `probability` BIGINT NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_enchants` (
  `class` TINYINT NOT NULL,
  `spec` TINYINT NOT NULL,
  `spellid` INT NOT NULL,
  `slotid` TINYINT NOT NULL DEFAULT 1,
  `name` VARCHAR(255) NOT NULL COMMENT 'name of the enchant',
  PRIMARY KEY (`class`,`spec`,`spellid`,`slotid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_weightscales` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(32) NOT NULL,
  `class` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_weightscale_data` (
  `id` INT NOT NULL,
  `field` VARCHAR(18) NOT NULL,
  `val` SMALLINT UNSIGNED NOT NULL,
  KEY `id` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_bis_gear` (
  `class` TINYINT UNSIGNED NOT NULL,
  `tab` TINYINT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL,
  `faction` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `auto_gear_score_limit` SMALLINT UNSIGNED NOT NULL,
  `item_id` INT UNSIGNED NOT NULL,
  `phase` VARCHAR(32) NOT NULL DEFAULT '',
  `class_name` VARCHAR(16) NOT NULL,
  `spec_name` VARCHAR(32) NOT NULL,
  `slot_name` VARCHAR(16) NOT NULL,
  `faction_name` VARCHAR(8) NOT NULL DEFAULT 'Both',
  `item_name` VARCHAR(96) NOT NULL,
  PRIMARY KEY (`class`,`tab`,`slot`,`faction`,`auto_gear_score_limit`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_dungeon_suggestion_definition` (
  `id` TINYINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `slug` VARCHAR(255) NOT NULL,
  `name` VARCHAR(255) NOT NULL,
  `expansion` TINYINT NOT NULL,
  `difficulty` TINYINT NOT NULL,
  `min_level` TINYINT UNSIGNED DEFAULT NULL,
  `max_level` TINYINT UNSIGNED DEFAULT NULL,
  `comment` VARCHAR(255) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbot dungeon suggestion definitions';

CREATE TABLE IF NOT EXISTS `playerbots_dungeon_suggestion_abbrevation` (
  `id` TINYINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `definition_slug` VARCHAR(255) NOT NULL,
  `abbrevation` VARCHAR(16) NOT NULL,
  PRIMARY KEY (`id`),
  KEY `definition_slug` (`definition_slug`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbot dungeon suggestion abbrevations';

CREATE TABLE IF NOT EXISTS `playerbots_dungeon_suggestion_strategy` (
  `id` TINYINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `definition_slug` VARCHAR(255) NOT NULL,
  `strategy` VARCHAR(255) NOT NULL,
  `difficulty` TINYINT NOT NULL,
  PRIMARY KEY (`id`),
  KEY `definition_slug` (`definition_slug`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbot dungeon suggestion strategies';

-- ---- travel graph (AzerothCore ships a 3.3.5 graph, not valid for retail maps) ----

CREATE TABLE IF NOT EXISTS `playerbots_travelnode` (
  `id` MEDIUMINT NOT NULL,
  `name` VARCHAR(1024) NOT NULL,
  `map_id` MEDIUMINT NOT NULL,
  `x` FLOAT NOT NULL,
  `y` FLOAT NOT NULL,
  `z` FLOAT NOT NULL,
  `linked` TINYINT DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='PlayerbotAI travel node';

CREATE TABLE IF NOT EXISTS `playerbots_travelnode_link` (
  `node_id` MEDIUMINT NOT NULL,
  `to_node_id` MEDIUMINT NOT NULL,
  `type` TINYINT NOT NULL,
  `object` MEDIUMINT NOT NULL,
  `distance` FLOAT NOT NULL,
  `swim_distance` FLOAT NOT NULL,
  `extra_cost` FLOAT NOT NULL,
  `calculated` TINYINT NOT NULL,
  `max_creature_0` TINYINT NOT NULL,
  `max_creature_1` TINYINT NOT NULL,
  `max_creature_2` TINYINT NOT NULL,
  PRIMARY KEY (`node_id`,`to_node_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='PlayerbotAI travel node link';

CREATE TABLE IF NOT EXISTS `playerbots_travelnode_path` (
  `node_id` MEDIUMINT NOT NULL,
  `to_node_id` MEDIUMINT NOT NULL,
  `nr` MEDIUMINT NOT NULL,
  `map_id` MEDIUMINT NOT NULL,
  `x` FLOAT NOT NULL,
  `y` FLOAT NOT NULL,
  `z` FLOAT NOT NULL,
  PRIMARY KEY (`node_id`,`to_node_id`,`nr`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='PlayerbotAI travel node path';
