-- Festival goals. A row matches only while its game_event is active.
-- eventEntry values are the live world.game_event ids.

INSERT INTO `playerbots_goal`
  (`name`,`kind`,`weight`,`min_level`,`max_level`,`game_event`,`duration_min`,`duration_max`,`cooldown_sec`)
VALUES
  ('Midsummer', 'event', 30, 1, 255, 1, 15, 30, 600),
  ('Winter Veil', 'event', 30, 1, 255, 2, 15, 30, 600),
  ('Darkmoon Faire', 'event', 30, 1, 255, 3, 20, 40, 600),
  ('Brewfest', 'event', 30, 1, 255, 24, 15, 30, 600),
  ('Hallow''s End', 'event', 30, 1, 255, 12, 15, 30, 600);
