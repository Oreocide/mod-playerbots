-- Isle of Conquest (map 628) for the playerbots battleground planner (docs/design/BG_PLAN.md phase 5.1/5.2). Data: the script
-- battleground_isle_of_conquest.cpp, isle_of_conquest.h and the world DB of map 628 (worktrees/bg-ioc/IOC_DATA.md, 2026-11-13).
-- Nodes and keeps are banners with lock 1479 ("Opening" 21651, 8 s); ws_*_ctrl = held by that team, ws_*_assault = that team's
-- assault running ("In Conflict Alliance" = the Alliance attacks). A keep banner can be clicked only once one of that keep's gates is
-- down (requires: any one of its three walls). Gates are kind wall: ws_alive = the gate's "closed" world state (1 while it stands,
-- default 1); weights 0 (foot soldiers cannot hurt a gate; Battleground/BgIsle.cpp sends Demolishers). Bosses spawn only when a gate
-- of their keep is down (spawn groups 1238/1239); leash_r = their 25 yd Rage radius. Rally rows: side = the team that gathers there,
-- the siege spots AzerothCore uses in front of each front gate (IC_GATE_ATTACK_POS_*).
DELETE FROM `playerbots_bg_node` WHERE `map` = 628;
INSERT INTO `playerbots_bg_node` (`map`, `node`, `kind`, `side`, `name`, `x`, `y`, `z`, `radius`, `creature_entry`, `ws_a_ctrl`, `ws_a_assault`, `ws_h_ctrl`, `ws_h_assault`, `ws_alive`, `requires`, `ally_weight`, `horde_weight`, `min_group`, `rally`, `leash_r`, `aliases`, `comment`) VALUES
(628, 'workshop', 'capture', 0, 'Workshop', 776.2, -804.3, 6.5, 12, 0, 4229, 4228, 4230, 4293, 0, '', 9, 9, 0, '', 0, 'workshop,ws', 'banner 195133 neutral; its owner gets 4 Demolishers, the Siege Engine and Seaforium bombs; graveyard 1482'),
(628, 'docks', 'capture', 0, 'Docks', 726.4, -360.2, 17.8, 12, 0, 4304, 4305, 4303, 4302, 0, '', 6, 6, 0, '', 0, 'docks', 'banner 195157; Catapults and Glaive Throwers; graveyard 1480'),
(628, 'hangar', 'capture', 0, 'Hangar', 807.8, -1000.1, 132.4, 12, 0, 4299, 4300, 4298, 4297, 0, '', 4, 4, 0, '', 0, 'hangar,airport', 'banner 195158; gunship portals (bots leave the gunship alone); graveyard 1481'),
(628, 'quarry', 'capture', 0, 'Quarry', 251.0, -1159.3, 17.2, 12, 0, 4309, 4310, 4308, 4307, 0, '', 7, 3, 0, '', 0, 'quarry', 'banner 195338; +1 reinforcement every 45 s while held; no graveyard; near the Alliance keep'),
(628, 'refinery', 'capture', 0, 'Refinery', 1269.5, -400.8, 37.6, 12, 0, 4314, 4315, 4313, 4312, 0, '', 3, 7, 0, '', 0, 'refinery,oil', 'banner 195343; +1 reinforcement every 45 s while held; no graveyard; near the Horde keep'),
(628, 'keep_a', 'capture', 1, 'Alliance Keep', 299.2, -784.6, 48.9, 12, 0, 4339, 4342, 4340, 4343, 0, '1:wall_a_front,wall_a_west,wall_a_east', 4, 3, 0, '', 0, 'alliance keep,keep a', 'banner 195396, NOT_SELECTABLE until a gate falls; graveyard 1485; the attacker''s weight stays low: the boss ends the match'),
(628, 'keep_h', 'capture', 2, 'Horde Keep', 1284.8, -705.7, 48.9, 12, 0, 4344, 4347, 4345, 4348, 0, '1:wall_h_front,wall_h_west,wall_h_east', 3, 4, 0, '', 0, 'horde keep,keep h', 'banner 195393, NOT_SELECTABLE until a gate falls; graveyard 1486; the attacker''s weight stays low: the boss ends the match'),
(628, 'wall_a_front', 'wall', 1, 'Alliance front gate', 413.5, -834.0, 48.5, 10, 195698, 0, 0, 0, 0, 4328, '', 0, 0, 0, '', 0, 'afront', 'open (destroyed) world state 4323; outside is x > 413.5'),
(628, 'wall_a_west', 'wall', 1, 'Alliance west gate', 351.6, -762.8, 48.9, 10, 195699, 0, 0, 0, 0, 4327, '', 0, 0, 0, '', 0, 'awest', 'destroyed 4324; outside is y > -762.8'),
(628, 'wall_a_east', 'wall', 1, 'Alliance east gate', 351.0, -903.3, 48.9, 10, 195700, 0, 0, 0, 0, 4326, '', 0, 0, 0, '', 0, 'aeast', 'destroyed 4325; outside is y < -903.3'),
(628, 'wall_h_front', 'wall', 2, 'Horde front gate', 1150.9, -762.6, 47.5, 10, 195494, 0, 0, 0, 0, 4317, '', 0, 0, 0, '', 0, 'hfront', 'destroyed 4322; outside is x < 1150.9'),
(628, 'wall_h_west', 'wall', 2, 'Horde west gate', 1217.9, -676.9, 47.6, 10, 195495, 0, 0, 0, 0, 4318, '', 0, 0, 0, '', 0, 'hwest', 'destroyed 4321; outside is y > -676.9'),
(628, 'wall_h_east', 'wall', 2, 'Horde east gate', 1218.7, -851.2, 48.3, 10, 195496, 0, 0, 0, 0, 4319, '', 0, 0, 0, '', 0, 'heast', 'destroyed 4320; outside is y < -851.2'),
(628, 'agmar', 'kill_general', 2, 'Overlord Agmar', 1295.4, -765.7, 70.1, 0, 34922, 0, 0, 0, 0, 0, '1:wall_h_front,wall_h_west,wall_h_east', 10, 0, 10, 'rally_agmar', 25, 'agmar,overlord,boss h', 'killed: the Alliance wins; 8 Kor''kron Guards; a vehicle that hits him dies'),
(628, 'halford', 'kill_general', 1, 'High Commander Halford Wyrmbane', 225.0, -831.6, 60.9, 0, 34924, 0, 0, 0, 0, 0, '1:wall_a_front,wall_a_west,wall_a_east', 0, 10, 10, 'rally_halford', 25, 'halford,wyrmbane,boss a', 'killed: the Horde wins; 8 7th Legion Infantry; a vehicle that hits him dies'),
(628, 'rally_agmar', 'rally', 1, 'Agmar rally', 1091.3, -763.6, 42.4, 0, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', '60 yd in front of the Horde front gate (AzerothCore IC_GATE_ATTACK_POS_ALLIANCE)'),
(628, 'rally_halford', 'rally', 2, 'Halford rally', 506.8, -828.6, 24.3, 0, 0, 0, 0, 0, 0, 0, '', 0, 0, 0, '', 0, '', '93 yd in front of the Alliance front gate (AzerothCore IC_GATE_ATTACK_POS_HORDE)');
