-- Classic 1-30 quest hubs. The picker prefers the rung that contains the bot's level
-- and, if that rung has no row, the rung just below. Zone 0 "Kill quests" covers the rest.

INSERT INTO `playerbots_goal`
  (`name`,`kind`,`weight`,`min_level`,`max_level`,`faction`,`map`,`zone`,`duration_min`,`duration_max`,`cooldown_sec`)
VALUES
  ('Elwynn quests', 'quest_hub', 20, 1, 10, 1, 0, 12, 20, 40, 120),
  ('Dun Morogh quests', 'quest_hub', 20, 1, 10, 1, 0, 1, 20, 40, 120),
  ('Westfall quests', 'quest_hub', 20, 10, 20, 1, 0, 40, 20, 40, 120),
  ('Loch Modan quests', 'quest_hub', 20, 10, 20, 1, 0, 38, 20, 40, 120),
  ('Redridge quests', 'quest_hub', 20, 15, 25, 1, 0, 44, 20, 40, 120),
  ('Duskwood quests', 'quest_hub', 20, 20, 30, 1, 0, 10, 20, 40, 120),
  ('Wetlands quests', 'quest_hub', 20, 20, 30, 1, 0, 11, 20, 40, 120),
  ('Teldrassil quests', 'quest_hub', 20, 1, 10, 1, 1, 141, 20, 40, 120),
  ('Darkshore quests', 'quest_hub', 20, 10, 20, 1, 1, 148, 20, 40, 120),
  ('Ashenvale quests', 'quest_hub', 20, 20, 30, 1, 1, 331, 20, 40, 120),
  ('Durotar quests', 'quest_hub', 20, 1, 10, 2, 1, 14, 20, 40, 120),
  ('Mulgore quests', 'quest_hub', 20, 1, 10, 2, 1, 215, 20, 40, 120),
  ('Barrens quests', 'quest_hub', 20, 10, 20, 2, 1, 17, 20, 40, 120),
  ('Stonetalon quests', 'quest_hub', 20, 20, 30, 2, 1, 406, 20, 40, 120),
  ('Tirisfal quests', 'quest_hub', 20, 1, 10, 2, 0, 85, 20, 40, 120),
  ('Silverpine quests', 'quest_hub', 20, 10, 20, 2, 0, 130, 20, 40, 120),
  ('Hillsbrad quests', 'quest_hub', 20, 20, 30, 2, 0, 267, 20, 40, 120);
