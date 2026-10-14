-- mod-playerbots: .bot command, available to every player (Role: Player Commands)
DELETE FROM `rbac_linked_permissions` WHERE `linkedId`=1010;
DELETE FROM `rbac_permissions` WHERE `id`=1010;
INSERT INTO `rbac_permissions` (`id`,`name`) VALUES
(1010,'Command: bot (mod-playerbots)');
INSERT INTO `rbac_linked_permissions` (`id`,`linkedId`) VALUES
(199,1010);
