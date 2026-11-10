-- Playerbots: the objective graph of the battleground team planner (docs/design/BG_PLAN.md §2 "Objective graph", phase 3).
-- One row per objective, rally point and gate of a planned map; Battleground/BgMapData.cpp reads it once at startup
-- (".bot bg reload" reads it again). ".bot bg rally set <node>" writes the game master's position into x, y, z: copy a tuned
-- position back into this file, a changed file is applied again and replaces the table.
--
-- kind: capture (a graveyard: owned while held), burn (a tower: gone once the other side's assault runs out), kill_captain,
--       kill_general (a boss, pushed through its rally row), rally (where the attackers gather before a boss), gate (where the
--       team waits while the gates are closed), mine (phase 4, Playerbots.Bg.Mines: killing its boss takes it; creature_entry is
--       the boss while the mine is neutral, an owner's bosses are in BgMapData.cpp MineBossEntry; ws_a_ctrl / ws_h_ctrl: who owns it)
-- side: 0 none, 1 Alliance, 2 Horde. Nodes: the owner at the start (a tower: who built it); bosses: their side; rally and gate:
--       the team that stands there
-- ws_*: the world states the battleground script writes for the node (battleground_alterac_valley.cpp BGAVNodeInfo, as written:
--       two pairs there are swapped against the enum names); ws_alive: 1 while the boss lives
-- requires: node keys the team should hold, destroy or kill first ("2:a,b,c" = any two of them); a soft gate that opens after
--       Playerbots.Bg.SoftGateMinutes or when the enemy score falls to Playerbots.Bg.SoftGateScorePct
-- ally_weight, horde_weight: for that team, how much the node matters: the attack order among nodes it does not hold (highest
--       first), the share of defenders among nodes it holds; 0 = the team never goes for it
-- min_group: the team needs at least this many attackers before it pushes this boss
-- leash_r: the boss evades this far (2D) from its home
-- The coordinator's columns (docs/design/BG_COORDINATOR.md §2.9, §4; read only by Playerbots.Bg.Coordinator): ally_value,
--       horde_value: a team's value of the node (its attack value where it attacks, its hold value where it holds; 0 = the
--       weight, which stays the old planner's); attack_quota, guards: the most bots on a Take, the guards on a running assault of
--       the team (0 = the mode's default); w_early, w_mid, w_late: the attack value's multiplier in each phase of the team
-- Banners are found near x, y by their script (go_av_capturable_object, go_av_contested_object); positions of the nodes are their
-- banners in world.gameobject, of the bosses their creature spawns. Rally spots: points of the AzerothCore mod-playerbots
-- (GPL-2) Alterac Valley road paths (BattleGroundTactics.cpp), 64-130 yd from the boss and away from creatures hostile to the
-- gathering team. Gates: the 180424 door objects; bots stop on their own side of them

DROP TABLE IF EXISTS `playerbots_bg_node`;
CREATE TABLE `playerbots_bg_node` (
  `map` int unsigned NOT NULL,
  `node` varchar(32) NOT NULL,
  `kind` varchar(16) NOT NULL,
  `side` tinyint unsigned NOT NULL DEFAULT '0',
  `name` varchar(64) NOT NULL DEFAULT '',
  `x` float NOT NULL DEFAULT '0',
  `y` float NOT NULL DEFAULT '0',
  `z` float NOT NULL DEFAULT '0',
  `radius` float NOT NULL DEFAULT '0',
  `creature_entry` int unsigned NOT NULL DEFAULT '0',
  `ws_a_ctrl` int unsigned NOT NULL DEFAULT '0',
  `ws_a_assault` int unsigned NOT NULL DEFAULT '0',
  `ws_h_ctrl` int unsigned NOT NULL DEFAULT '0',
  `ws_h_assault` int unsigned NOT NULL DEFAULT '0',
  `ws_alive` int unsigned NOT NULL DEFAULT '0',
  `requires` varchar(255) NOT NULL DEFAULT '',
  `ally_weight` float NOT NULL DEFAULT '0',
  `horde_weight` float NOT NULL DEFAULT '0',
  `min_group` tinyint unsigned NOT NULL DEFAULT '0',
  `rally` varchar(32) NOT NULL DEFAULT '',
  `leash_r` float NOT NULL DEFAULT '0',
  `aliases` varchar(255) NOT NULL DEFAULT '',
  `attack_quota` tinyint unsigned NOT NULL DEFAULT '0',
  `guards` tinyint unsigned NOT NULL DEFAULT '0',
  `w_early` float NOT NULL DEFAULT '1',
  `w_mid` float NOT NULL DEFAULT '1',
  `w_late` float NOT NULL DEFAULT '1',
  `ally_value` float NOT NULL DEFAULT '0',
  `horde_value` float NOT NULL DEFAULT '0',
  `comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`map`, `node`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Playerbots: battleground objective graph (BG_PLAN.md)';

INSERT INTO `playerbots_bg_node` (`map`, `node`, `kind`, `side`, `name`, `x`, `y`, `z`, `radius`, `creature_entry`, `ws_a_ctrl`, `ws_a_assault`, `ws_h_ctrl`, `ws_h_assault`, `ws_alive`, `requires`, `ally_weight`, `horde_weight`, `min_group`, `rally`, `leash_r`, `aliases`, `comment`) VALUES
-- graveyards
(30, 'aid_station', 'capture', 1, 'Stormpike Aid Station', 638.6, -32.4, 46.1, 12, 0, 1325, 1326, 1327, 1328, 0, '', 1, 3, 0, '', 0, 'aid,aid station,sp aid', 'node 0'),
(30, 'stormpike_gy', 'capture', 1, 'Stormpike Graveyard', 669.0, -294.1, 30.3, 12, 0, 1333, 1335, 1334, 1336, 0, '', 2, 5, 0, '', 0, 'spgy,stormpike gy,stormpike', 'node 1'),
(30, 'stonehearth_gy', 'capture', 1, 'Stonehearth Graveyard', 73.3, -425.8, 60.6, 12, 0, 1302, 1304, 1301, 1303, 0, '', 3, 6, 0, '', 0, 'shgy,stonehearth gy', 'node 2'),
(30, 'snowfall_gy', 'capture', 0, 'Snowfall Graveyard', -202.6, -112.8, 78.5, 12, 0, 1341, 1343, 1342, 1344, 0, '', 9, 9, 0, '', 0, 'sf,snowfall,snowfall gy', 'node 3, neutral at the start'),
(30, 'iceblood_gy', 'capture', 2, 'Iceblood Graveyard', -612.7, -396.7, 60.9, 12, 0, 1346, 1348, 1347, 1349, 0, '', 6, 3, 0, '', 0, 'ibgy,iceblood gy', 'node 4'),
(30, 'frostwolf_gy', 'capture', 2, 'Frostwolf Graveyard', -1082.5, -346.6, 55.0, 12, 0, 1337, 1339, 1338, 1340, 0, '', 5, 2, 0, '', 0, 'fwgy,frostwolf gy', 'node 5'),
(30, 'relief_hut', 'capture', 2, 'Frostwolf Relief Hut', -1402.2, -307.5, 89.4, 12, 0, 1329, 1331, 1330, 1332, 0, '', 3, 1, 0, '', 0, 'hut,relief hut,fw hut', 'node 6'),
-- towers: the banner is on top
(30, 'db_south', 'burn', 1, 'Dun Baldar South Bunker', 553.8, -78.7, 51.9, 12, 0, 1361, 1375, 1370, 1378, 0, '', 1, 4, 0, '', 0, 'dbs,south bunker', 'node 7'),
(30, 'db_north', 'burn', 1, 'Dun Baldar North Bunker', 674.0, -143.1, 63.7, 12, 0, 1362, 1374, 1371, 1379, 0, '', 1, 4, 0, '', 0, 'dbn,north bunker', 'node 8'),
(30, 'icewing', 'burn', 1, 'Icewing Bunker', 203.2, -360.3, 56.4, 12, 0, 1363, 1376, 1372, 1380, 0, '', 2, 7, 0, '', 0, 'iw,icewing', 'node 9'),
(30, 'stonehearth_bunker', 'burn', 1, 'Stonehearth Bunker', -152.4, -441.6, 40.4, 12, 0, 1364, 1377, 1373, 1381, 0, '', 3, 7, 0, '', 0, 'shb,stonehearth bunker', 'node 10'),
(30, 'iceblood_tower', 'burn', 2, 'Iceblood Tower', -571.9, -262.8, 75.0, 12, 0, 1368, 1390, 1385, 1395, 0, '', 7, 3, 0, '', 0, 'ibt,iceblood tower', 'node 11'),
(30, 'tower_point', 'burn', 2, 'Tower Point', -768.1, -362.7, 90.9, 12, 0, 1367, 1389, 1384, 1394, 0, '', 7, 2, 0, '', 0, 'tp,tower point', 'node 12'),
(30, 'fw_east', 'burn', 2, 'East Frostwolf Tower', -1302.9, -317.0, 113.9, 12, 0, 1366, 1388, 1383, 1393, 0, '', 4, 1, 0, '', 0, 'etower,east tower,east frostwolf', 'node 13'),
(30, 'fw_west', 'burn', 2, 'West Frostwolf Tower', -1297.7, -266.7, 114.2, 12, 0, 1365, 1387, 1382, 1392, 0, '', 4, 1, 0, '', 0, 'wtower,west tower,west frostwolf', 'node 14'),
-- bosses
(30, 'balinda', 'kill_captain', 1, 'Captain Balinda Stonehearth', -57.8, -286.6, 15.6, 0, 11949, 0, 0, 0, 0, 1351, '', 0, 8, 5, 'rally_balinda', 50, 'balinda,captain', 'killed: -100 Alliance, her buff 23693 ends'),
(30, 'galvangar', 'kill_captain', 2, 'Captain Galvangar', -545.2, -165.4, 57.8, 0, 11947, 0, 0, 0, 0, 1352, '', 8, 0, 5, 'rally_galvangar', 50, 'galv,galvangar,captain', 'killed: -100 Horde, his buff 22751 ends'),
(30, 'vanndar', 'kill_general', 1, 'Vanndar Stormpike', 722.4, -11.0, 50.7, 0, 11948, 0, 0, 0, 0, 602, '2:db_south,db_north,icewing,stonehearth_bunker', 0, 2, 10, 'rally_vanndar', 50, 'van,vanndar,boss,general', 'killed: the Horde wins; each bunker left standing keeps a marshal next to him'),
(30, 'drekthar', 'kill_general', 2, 'Drek''Thar', -1370.9, -220.2, 98.5, 0, 11946, 0, 0, 0, 0, 601, '2:iceblood_tower,tower_point,fw_east,fw_west', 2, 0, 10, 'rally_drekthar', 50, 'drek,drekthar,boss,general', 'killed: the Alliance wins; each tower left standing keeps a warmaster next to him'),
-- rally points (radius 0: Playerbots.Bg.Rally.Radius)
(30, 'rally_balinda', 'rally', 2, 'Balinda rally', -117.164, -263.636, 6.363, 0, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', '64 yd, road below Stonehearth Outpost (AC vPath_AV_AllianceCaptain_To_HordeCrossroad3)'),
(30, 'rally_galvangar', 'rally', 1, 'Galvangar rally', -445.352, -195.483, 35.300, 0, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', '104 yd, road below Iceblood Garrison (AC vPath_AV_SnowfallGraveyard_To_HordeCaptain); a Frost Wolf hostile to the Alliance stands at -483 -195'),
(30, 'rally_vanndar', 'rally', 2, 'Vanndar rally', 628.271, -100.943, 40.328, 0, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', '130 yd, road south of Dun Baldar (AC vPath_AV_AllianceFortress_To_AllianceCrossroad1), clear of the Aid Station guards'),
(30, 'rally_drekthar', 'rally', 1, 'Drek''Thar rally', -1346.880, -287.015, 91.066, 0, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', '71 yd, Frostwolf Keep courtyard (AC vPath_AV_HordeCrossroad1_To_HordeFortress)'),
-- gates (radius: the farthest a bot stops from the door)
(30, 'gate_alliance', 'gate', 1, 'Alliance gate', 794.6, -493.5, 99.8, 13, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', 'door 180424; the cave start 873 -491 is behind it'),
(30, 'gate_horde', 'gate', 2, 'Horde gate', -1382.1, -545.9, 54.9, 13, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', 'door 180424; the start -1437 -610 is behind it'),
-- mines (phase 4, reactive #13): x, y, z is the neutral boss; a team sends a mine group while the front stalls, to the mine with
-- its higher weight; the owner gets 1 reinforcement every 45 s (battleground_alterac_valley.cpp)
(30, 'irondeep_mine', 'mine', 0, 'Irondeep Mine', 863.1, -441.3, 50.6, 30, 11657, 1358, 0, 1359, 0, 0, '', 2, 1, 4, '', 0, 'irondeep,id mine,north mine', 'Morloch 11657 neutral; Umi Thorson 13078 for the Alliance, Keetar 13079 for the Horde'),
(30, 'coldtooth_mine', 'mine', 0, 'Coldtooth Mine', -851.6, -92.6, 68.5, 30, 11677, 1355, 0, 1356, 0, 0, '', 1, 2, 4, '', 0, 'coldtooth,ct mine,south mine', 'Taskmaster Snivvle 11677 neutral; Aggi Rumblestomp 13086 for the Alliance, Masha Swiftcut 13088 for the Horde');

-- The coordinator's values for Alterac Valley (docs/design/BG_COORDINATOR.md §4): a push-path tower 8, a captain 8 (mid phase), the
-- graveyards 6, the far towers 7, the last graveyard 4, the general 10 (late), Snowfall 3 with a quota of 4 and 1 guard. Hold
-- values on a team's own nodes: the graveyard nearest the front 3, the others 2, towers and bunkers 1. Tuning (M) until tested
UPDATE `playerbots_bg_node` SET `ally_value` = 3, `horde_value` = 3, `attack_quota` = 4, `guards` = 1, `w_early` = 1, `w_mid` = 0.3, `w_late` = 0 WHERE `map` = 30 AND `node` = 'snowfall_gy';
UPDATE `playerbots_bg_node` SET `ally_value` = 8, `horde_value` = 1, `attack_quota` = 12, `guards` = 2, `w_early` = 1, `w_mid` = 1, `w_late` = 0.5 WHERE `map` = 30 AND `node` = 'iceblood_tower';
UPDATE `playerbots_bg_node` SET `ally_value` = 8, `horde_value` = 1, `attack_quota` = 12, `guards` = 2, `w_early` = 1, `w_mid` = 1, `w_late` = 0.5 WHERE `map` = 30 AND `node` = 'tower_point';
UPDATE `playerbots_bg_node` SET `ally_value` = 8, `horde_value` = 0, `attack_quota` = 12, `guards` = 0, `w_early` = 0, `w_mid` = 1, `w_late` = 0.3 WHERE `map` = 30 AND `node` = 'galvangar';
UPDATE `playerbots_bg_node` SET `ally_value` = 6, `horde_value` = 3, `attack_quota` = 8, `guards` = 2, `w_early` = 0.3, `w_mid` = 1, `w_late` = 0.5 WHERE `map` = 30 AND `node` = 'iceblood_gy';
UPDATE `playerbots_bg_node` SET `ally_value` = 6, `horde_value` = 2, `attack_quota` = 8, `guards` = 2, `w_early` = 0, `w_mid` = 1, `w_late` = 0.8 WHERE `map` = 30 AND `node` = 'frostwolf_gy';
UPDATE `playerbots_bg_node` SET `ally_value` = 7, `horde_value` = 1, `attack_quota` = 10, `guards` = 2, `w_early` = 0, `w_mid` = 0.8, `w_late` = 1 WHERE `map` = 30 AND `node` = 'fw_east';
UPDATE `playerbots_bg_node` SET `ally_value` = 7, `horde_value` = 1, `attack_quota` = 10, `guards` = 2, `w_early` = 0, `w_mid` = 0.8, `w_late` = 1 WHERE `map` = 30 AND `node` = 'fw_west';
UPDATE `playerbots_bg_node` SET `ally_value` = 4, `horde_value` = 2, `attack_quota` = 6, `guards` = 1, `w_early` = 0, `w_mid` = 0.3, `w_late` = 0.8 WHERE `map` = 30 AND `node` = 'relief_hut';
UPDATE `playerbots_bg_node` SET `ally_value` = 10, `horde_value` = 0, `attack_quota` = 0, `guards` = 0, `w_early` = 0, `w_mid` = 0, `w_late` = 1 WHERE `map` = 30 AND `node` = 'drekthar';
UPDATE `playerbots_bg_node` SET `ally_value` = 1, `horde_value` = 8, `attack_quota` = 12, `guards` = 2, `w_early` = 1, `w_mid` = 1, `w_late` = 0.5 WHERE `map` = 30 AND `node` = 'stonehearth_bunker';
UPDATE `playerbots_bg_node` SET `ally_value` = 1, `horde_value` = 8, `attack_quota` = 12, `guards` = 2, `w_early` = 1, `w_mid` = 1, `w_late` = 0.5 WHERE `map` = 30 AND `node` = 'icewing';
UPDATE `playerbots_bg_node` SET `ally_value` = 0, `horde_value` = 8, `attack_quota` = 12, `guards` = 0, `w_early` = 0, `w_mid` = 1, `w_late` = 0.3 WHERE `map` = 30 AND `node` = 'balinda';
UPDATE `playerbots_bg_node` SET `ally_value` = 3, `horde_value` = 6, `attack_quota` = 8, `guards` = 2, `w_early` = 0.3, `w_mid` = 1, `w_late` = 0.5 WHERE `map` = 30 AND `node` = 'stonehearth_gy';
UPDATE `playerbots_bg_node` SET `ally_value` = 2, `horde_value` = 6, `attack_quota` = 8, `guards` = 2, `w_early` = 0, `w_mid` = 1, `w_late` = 0.8 WHERE `map` = 30 AND `node` = 'stormpike_gy';
UPDATE `playerbots_bg_node` SET `ally_value` = 1, `horde_value` = 7, `attack_quota` = 10, `guards` = 2, `w_early` = 0, `w_mid` = 0.8, `w_late` = 1 WHERE `map` = 30 AND `node` = 'db_north';
UPDATE `playerbots_bg_node` SET `ally_value` = 1, `horde_value` = 7, `attack_quota` = 10, `guards` = 2, `w_early` = 0, `w_mid` = 0.8, `w_late` = 1 WHERE `map` = 30 AND `node` = 'db_south';
UPDATE `playerbots_bg_node` SET `ally_value` = 2, `horde_value` = 4, `attack_quota` = 6, `guards` = 1, `w_early` = 0, `w_mid` = 0.3, `w_late` = 0.8 WHERE `map` = 30 AND `node` = 'aid_station';
UPDATE `playerbots_bg_node` SET `ally_value` = 0, `horde_value` = 10, `attack_quota` = 0, `guards` = 0, `w_early` = 0, `w_mid` = 0, `w_late` = 1 WHERE `map` = 30 AND `node` = 'vanndar';
