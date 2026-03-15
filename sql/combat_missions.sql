-- See A371 §Phase1 (Combat Mission Implementation — Encounter Data SQL)
-- Creates qstEncounter table and seeds it with combat mission data.
-- Run: docker exec evemu_db mariadb -u evemu -pevemu evemu < /opt/evemu/sql/combat_missions.sql
-- Rollback: DROP TABLE IF EXISTS qstEncounter;

CREATE TABLE IF NOT EXISTS `qstEncounter` (
  `id` int(5) NOT NULL DEFAULT 0,
  `briefingID` int(5) NOT NULL DEFAULT 0,
  `name` text DEFAULT NULL,
  `level` tinyint(1) NOT NULL DEFAULT 0,
  `typeID` tinyint(1) NOT NULL DEFAULT 2,
  `sysRange` tinyint(2) NOT NULL DEFAULT 1,
  `important` bit(1) NOT NULL DEFAULT b'0',
  `storyline` bit(1) NOT NULL DEFAULT b'0',
  `raceID` tinyint(2) NOT NULL DEFAULT 0,
  `rewardISK` int(10) NOT NULL DEFAULT 0,
  `rewardItemID` int(11) NOT NULL DEFAULT 0,
  `rewardItemQty` int(11) NOT NULL DEFAULT 0,
  `bonusISK` int(11) NOT NULL DEFAULT 0,
  `bonusTime` int(5) NOT NULL DEFAULT 0,
  `npcCount` tinyint(3) NOT NULL DEFAULT 3,
  `dungeonID` int(10) NOT NULL DEFAULT 0,
  `npcGroupID` int(10) NOT NULL DEFAULT 818,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Seed encounter missions using existing briefingIDs from agtMissions typeID=2 entries.
-- Rewards scaled approximately 1.5-2x courier values for equivalent level.
-- npcGroupID: 818=Generic Frigates, 817=Generic Cruisers, 816=Generic Battleships, 829=Generic Destroyers, 828=Generic Battle Cruisers

-- Level 1 missions (4-5 frigates, 25k-50k ISK reward, 20min bonus)
INSERT INTO `qstEncounter` VALUES (60001, 131785, 'Seek and Destroy', 1, 2, 1, b'0', b'0', 0, 35000, 0, 0, 12000, 20, 4, 0, 818);
INSERT INTO `qstEncounter` VALUES (60002, 131796, 'Rogue Drone Harassment', 1, 2, 1, b'0', b'0', 0, 30000, 0, 0, 10000, 20, 3, 0, 818);
INSERT INTO `qstEncounter` VALUES (60003, 131804, 'Keeping Crime In Check', 1, 2, 1, b'0', b'0', 0, 28000, 0, 0, 9000, 20, 4, 0, 818);
INSERT INTO `qstEncounter` VALUES (60004, 131806, 'Unauthorized Military Presence', 1, 2, 1, b'0', b'0', 0, 32000, 0, 0, 11000, 20, 5, 0, 818);
INSERT INTO `qstEncounter` VALUES (60005, 131788, 'The Score', 1, 2, 1, b'0', b'0', 0, 40000, 0, 0, 15000, 25, 4, 0, 818);
INSERT INTO `qstEncounter` VALUES (60006, 131798, 'Supply Interdiction', 1, 2, 1, b'0', b'0', 0, 25000, 0, 0, 8000, 20, 3, 0, 818);
INSERT INTO `qstEncounter` VALUES (60007, 131800, 'Avenge a Fallen Comrade', 1, 2, 1, b'0', b'0', 0, 38000, 0, 0, 13000, 25, 5, 0, 818);
INSERT INTO `qstEncounter` VALUES (60008, 131790, 'Intercept The Saboteurs', 1, 2, 1, b'0', b'0', 0, 27000, 0, 0, 9000, 20, 3, 0, 818);
INSERT INTO `qstEncounter` VALUES (60009, 127529, 'The Disgruntled Employee', 1, 2, 1, b'0', b'0', 0, 45000, 0, 0, 18000, 30, 5, 0, 818);
INSERT INTO `qstEncounter` VALUES (60010, 131802, 'Smuggler Interception', 1, 2, 1, b'0', b'0', 0, 22000, 0, 0, 7000, 15, 3, 0, 818);

-- Level 2 missions (4-6 destroyers/frigates, 80k-200k ISK, 30min bonus)
INSERT INTO `qstEncounter` VALUES (60101, 131788, 'Seek and Destroy', 2, 2, 2, b'0', b'0', 0, 120000, 0, 0, 40000, 30, 5, 0, 829);
INSERT INTO `qstEncounter` VALUES (60102, 131790, 'Rogue Drone Harassment', 2, 2, 2, b'0', b'0', 0, 100000, 0, 0, 35000, 30, 4, 0, 829);
INSERT INTO `qstEncounter` VALUES (60103, 131794, 'Keeping Crime In Check', 2, 2, 2, b'0', b'0', 0, 90000, 0, 0, 30000, 30, 5, 0, 818);
INSERT INTO `qstEncounter` VALUES (60104, 131800, 'Unauthorized Military Presence', 2, 2, 2, b'0', b'0', 0, 150000, 0, 0, 50000, 35, 6, 0, 829);
INSERT INTO `qstEncounter` VALUES (60105, 131802, 'The Score', 2, 2, 2, b'0', b'0', 0, 180000, 0, 0, 60000, 35, 5, 0, 829);
INSERT INTO `qstEncounter` VALUES (60106, 131806, 'Supply Interdiction', 2, 2, 2, b'0', b'0', 0, 85000, 0, 0, 28000, 30, 4, 0, 818);
INSERT INTO `qstEncounter` VALUES (60107, 136824, 'Avenge a Fallen Comrade', 2, 2, 2, b'0', b'0', 0, 160000, 0, 0, 55000, 35, 6, 0, 829);
INSERT INTO `qstEncounter` VALUES (60108, 131808, 'Intercept The Saboteurs', 2, 2, 2, b'0', b'0', 0, 95000, 0, 0, 32000, 30, 5, 0, 818);
INSERT INTO `qstEncounter` VALUES (60109, 127530, 'The Disgruntled Employee', 2, 2, 2, b'0', b'0', 0, 200000, 0, 0, 70000, 40, 6, 0, 829);
INSERT INTO `qstEncounter` VALUES (60110, 131810, 'Smuggler Interception', 2, 2, 2, b'0', b'0', 0, 80000, 0, 0, 25000, 30, 4, 0, 818);

-- Level 3 missions (5-8 cruisers/destroyers, 250k-600k ISK, 40min bonus)
INSERT INTO `qstEncounter` VALUES (60201, 131790, 'Seek and Destroy', 3, 2, 3, b'0', b'0', 0, 400000, 0, 0, 130000, 40, 6, 0, 817);
INSERT INTO `qstEncounter` VALUES (60202, 131808, 'Rogue Drone Harassment', 3, 2, 3, b'0', b'0', 0, 350000, 0, 0, 115000, 40, 5, 0, 817);
INSERT INTO `qstEncounter` VALUES (60203, 131794, 'Keeping Crime In Check', 3, 2, 3, b'0', b'0', 0, 300000, 0, 0, 100000, 40, 6, 0, 817);
INSERT INTO `qstEncounter` VALUES (60204, 131810, 'Unauthorized Military Presence', 3, 2, 3, b'0', b'0', 0, 500000, 0, 0, 165000, 45, 7, 0, 817);
INSERT INTO `qstEncounter` VALUES (60205, 143457, 'The Blockade', 3, 2, 3, b'0', b'0', 0, 550000, 0, 0, 180000, 45, 8, 0, 817);
INSERT INTO `qstEncounter` VALUES (60206, 131812, 'Worlds Collide', 3, 2, 3, b'0', b'0', 0, 600000, 0, 0, 200000, 50, 8, 0, 817);
INSERT INTO `qstEncounter` VALUES (60207, 139011, 'Alluring Emanations', 3, 2, 3, b'0', b'0', 0, 450000, 0, 0, 150000, 45, 7, 0, 817);
INSERT INTO `qstEncounter` VALUES (60208, 143458, 'Retribution', 3, 2, 3, b'0', b'0', 0, 380000, 0, 0, 125000, 40, 6, 0, 817);

-- Level 4 missions (6-10 battleships/cruisers, 800k-2M ISK, 60min bonus)
INSERT INTO `qstEncounter` VALUES (60301, 131794, 'Seek and Destroy', 4, 2, 4, b'0', b'0', 0, 1200000, 0, 0, 400000, 60, 8, 0, 816);
INSERT INTO `qstEncounter` VALUES (60302, 131802, 'The Blockade', 4, 2, 4, b'0', b'0', 0, 1800000, 0, 0, 600000, 70, 10, 0, 816);
INSERT INTO `qstEncounter` VALUES (60303, 131810, 'Worlds Collide', 4, 2, 4, b'0', b'0', 0, 2000000, 0, 0, 700000, 75, 10, 0, 816);
INSERT INTO `qstEncounter` VALUES (60304, 143457, 'Unauthorized Military Presence', 4, 2, 4, b'0', b'0', 0, 1500000, 0, 0, 500000, 65, 9, 0, 816);
INSERT INTO `qstEncounter` VALUES (60305, 143458, 'Keeping Crime In Check', 4, 2, 4, b'0', b'0', 0, 1000000, 0, 0, 330000, 60, 7, 0, 828);
INSERT INTO `qstEncounter` VALUES (60306, 131812, 'Retribution', 4, 2, 4, b'0', b'0', 0, 900000, 0, 0, 300000, 55, 6, 0, 828);

-- Important L4 missions (storyline-tier, higher rewards)
INSERT INTO `qstEncounter` VALUES (60350, 131794, 'Break Their Will', 4, 2, 4, b'1', b'0', 0, 3000000, 0, 0, 1000000, 90, 12, 0, 816);
INSERT INTO `qstEncounter` VALUES (60351, 131802, 'Duo of Death', 4, 2, 4, b'1', b'0', 0, 2500000, 0, 0, 850000, 80, 10, 0, 816);
