-- mod-playerbots: invisible hotspot markers. Random bots are sent to the area of a loaded marker like to a town.
-- Place one with ".npc add <entry>" (GM), tier/team/level come from playerbots_hotspot_marker.

CREATE TABLE IF NOT EXISTS `playerbots_hotspot_marker` (
  `entry` INT UNSIGNED NOT NULL,
  `tier` TINYINT UNSIGNED NOT NULL DEFAULT 2 COMMENT '1 capital, 2 town, 3 outpost, 4 flight master hub, 5 transport hub',
  `team` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 both, 1 Alliance, 2 Horde',
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 255,
  `race_mask` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bit (race id - 1), 0 = every race',
  `radius` FLOAT NOT NULL DEFAULT 40,
  `name` VARCHAR(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

DELETE FROM `playerbots_hotspot_marker` WHERE `entry` BETWEEN 9100001 AND 9100299;
INSERT INTO `playerbots_hotspot_marker` (`entry`,`tier`,`team`,`min_level`,`max_level`,`race_mask`,`radius`,`name`) VALUES
(9100001,1,0,1,255,0,40,'Capital'),
(9100002,2,0,1,40,0,40,'Town'),
(9100003,3,0,1,20,0,30,'Outpost'),
(9100004,4,0,5,255,0,30,'Flight Master Hub'),
(9100005,5,0,5,255,0,30,'Transport Hub'),
(9100101,1,1,1,255,0,40,'Alliance Capital'),
(9100102,2,1,1,40,0,40,'Alliance Town'),
(9100103,3,1,1,20,0,30,'Alliance Outpost'),
(9100104,4,1,5,255,0,30,'Alliance Flight Master Hub'),
(9100105,5,1,5,255,0,30,'Alliance Transport Hub'),
(9100201,1,2,1,255,0,40,'Horde Capital'),
(9100202,2,2,1,40,0,40,'Horde Town'),
(9100203,3,2,1,20,0,30,'Horde Outpost'),
(9100204,4,2,5,255,0,30,'Horde Flight Master Hub'),
(9100205,5,2,5,255,0,30,'Horde Transport Hub');

DELETE FROM `creature_template` WHERE `entry` BETWEEN 9100001 AND 9100299;
DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN 9100001 AND 9100299;
DELETE FROM `creature_template_difficulty` WHERE `Entry` BETWEEN 9100001 AND 9100299;
-- like Invisible Stalker 15214: friendly to all, not interactible, a trigger
INSERT INTO `creature_template` (`entry`,`name`,`subname`,`faction`,`npcflag`,`speed_walk`,`speed_run`,`scale`,`unit_class`,`unit_flags`,`unit_flags2`,`type`,`flags_extra`,`ScriptName`,`VerifiedBuild`) VALUES
(9100001,'Bot Hotspot: Capital','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100002,'Bot Hotspot: Town','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100003,'Bot Hotspot: Outpost','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100004,'Bot Hotspot: Flight Master Hub','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100005,'Bot Hotspot: Transport Hub','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100101,'Bot Hotspot: Alliance Capital','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100102,'Bot Hotspot: Alliance Town','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100103,'Bot Hotspot: Alliance Outpost','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100104,'Bot Hotspot: Alliance Flight Master Hub','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100105,'Bot Hotspot: Alliance Transport Hub','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100201,'Bot Hotspot: Horde Capital','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100202,'Bot Hotspot: Horde Town','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100203,'Bot Hotspot: Horde Outpost','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100204,'Bot Hotspot: Horde Flight Master Hub','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0),
(9100205,'Bot Hotspot: Horde Transport Hub','Playerbots',35,0,1,1.14286,1,1,33554432,2048,10,130,'npc_bot_hotspot',0);

INSERT INTO `creature_template_model` (`CreatureID`,`Idx`,`CreatureDisplayID`,`DisplayScale`,`Probability`,`VerifiedBuild`) VALUES
(9100001,0,1126,1,1,0),
(9100002,0,1126,1,1,0),
(9100003,0,1126,1,1,0),
(9100004,0,1126,1,1,0),
(9100005,0,1126,1,1,0),
(9100101,0,1126,1,1,0),
(9100102,0,1126,1,1,0),
(9100103,0,1126,1,1,0),
(9100104,0,1126,1,1,0),
(9100105,0,1126,1,1,0),
(9100201,0,1126,1,1,0),
(9100202,0,1126,1,1,0),
(9100203,0,1126,1,1,0),
(9100204,0,1126,1,1,0),
(9100205,0,1126,1,1,0);

INSERT INTO `creature_template_difficulty` (`Entry`,`DifficultyID`,`ContentTuningID`,`HealthScalingExpansion`,`TypeFlags`,`CreatureDifficultyID`,`VerifiedBuild`) VALUES
(9100001,0,328,11,1024,0,0),
(9100002,0,328,11,1024,0,0),
(9100003,0,328,11,1024,0,0),
(9100004,0,328,11,1024,0,0),
(9100005,0,328,11,1024,0,0),
(9100101,0,328,11,1024,0,0),
(9100102,0,328,11,1024,0,0),
(9100103,0,328,11,1024,0,0),
(9100104,0,328,11,1024,0,0),
(9100105,0,328,11,1024,0,0),
(9100201,0,328,11,1024,0,0),
(9100202,0,328,11,1024,0,0),
(9100203,0,328,11,1024,0,0),
(9100204,0,328,11,1024,0,0),
(9100205,0,328,11,1024,0,0);
