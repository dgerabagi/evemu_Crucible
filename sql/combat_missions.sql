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
  `briefingText` text DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- See A371 §Phase4 (Briefing Text Pipeline)
-- briefingText sent as PyString when present, falling back to PyInt(briefingID) for client BSD lookup.
-- Most encounter briefingIDs (131785-131812 range) have no text in the Crucible client BSD,
-- so server-side briefingText is required for proper display.
-- Also add briefingText to agtOffers for offer caching:
ALTER TABLE `agtOffers` ADD COLUMN IF NOT EXISTS `briefingText` TEXT DEFAULT NULL;

-- Seed encounter missions with server-side briefing text.
-- Rewards scaled approximately 1.5-2x courier values for equivalent level.
-- npcGroupID: 818=Generic Frigates, 817=Generic Cruisers, 816=Generic Battleships, 829=Generic Destroyers, 828=Generic Battle Cruisers

-- Level 1 missions (4-5 frigates, 25k-50k ISK reward, 20min bonus)
INSERT INTO `qstEncounter` VALUES (60001, 131785, 'Seek and Destroy', 1, 2, 1, b'0', b'0', 0, 35000, 0, 0, 12000, 20, 4, 0, 818, 'We have received intelligence reports of hostile forces operating in the area. A small group of frigates has been spotted harassing our patrols. Track them down and eliminate them.');
INSERT INTO `qstEncounter` VALUES (60002, 131796, 'Rogue Drone Harassment', 1, 2, 1, b'0', b'0', 0, 30000, 0, 0, 10000, 20, 3, 0, 818, 'Rogue drones have been detected harassing mining operations in a nearby system. We need you to clear them out before they cause any more disruption.');
INSERT INTO `qstEncounter` VALUES (60003, 131804, 'Keeping Crime In Check', 1, 2, 1, b'0', b'0', 0, 28000, 0, 0, 9000, 20, 4, 0, 818, 'Criminal elements have been growing bolder in this system. We need you to find and destroy a group of pirates that have been preying on local traffic.');
INSERT INTO `qstEncounter` VALUES (60004, 131806, 'Unauthorized Military Presence', 1, 2, 1, b'0', b'0', 0, 32000, 0, 0, 11000, 20, 5, 0, 818, 'An unauthorized military force has established a presence in our territory. A small frigate patrol needs to be dealt with. Engage and destroy them.');
INSERT INTO `qstEncounter` VALUES (60005, 131788, 'The Score', 1, 2, 1, b'0', b'0', 0, 40000, 0, 0, 15000, 25, 4, 0, 818, 'We have a score to settle with some pirates who have been preying on our convoys. A small gang of them has been located. Go there and make sure none of them escape.');
INSERT INTO `qstEncounter` VALUES (60006, 131798, 'Supply Interdiction', 1, 2, 1, b'0', b'0', 0, 25000, 0, 0, 8000, 20, 3, 0, 818, 'Enemy forces have been intercepting our supply lines, causing significant losses. Destroy the frigate patrol responsible for the attacks.');
INSERT INTO `qstEncounter` VALUES (60007, 131800, 'Avenge a Fallen Comrade', 1, 2, 1, b'0', b'0', 0, 38000, 0, 0, 13000, 25, 5, 0, 818, 'One of our agents was ambushed and killed while on assignment. We have located the pirates responsible. Go to the coordinates and destroy them all.');
INSERT INTO `qstEncounter` VALUES (60008, 131790, 'Intercept The Saboteurs', 1, 2, 1, b'0', b'0', 0, 27000, 0, 0, 9000, 20, 3, 0, 818, 'We have intercepted communications indicating a sabotage operation is being planned against one of our installations. Intercept the saboteurs before they reach their target.');
INSERT INTO `qstEncounter` VALUES (60009, 127529, 'The Disgruntled Employee', 1, 2, 1, b'0', b'0', 0, 45000, 0, 0, 18000, 30, 5, 0, 818, 'We have a bit of a problem on our hands with a recently fired employee. He was working as a deep cover agent for us, and now he is threatening to release all the information to DED. Eliminate this threat.');
INSERT INTO `qstEncounter` VALUES (60010, 131802, 'Smuggler Interception', 1, 2, 1, b'0', b'0', 0, 22000, 0, 0, 7000, 15, 3, 0, 818, 'Our patrol ships have detected smugglers moving contraband through the area. Intercept them and destroy their ships before they complete their delivery.');

-- Level 2 missions (4-6 destroyers/frigates, 80k-200k ISK, 30min bonus)
INSERT INTO `qstEncounter` VALUES (60101, 131788, 'Seek and Destroy', 2, 2, 2, b'0', b'0', 0, 120000, 0, 0, 40000, 30, 5, 0, 829, 'A well-organized group of hostiles has been spotted in a nearby system. They have destroyer-class vessels and are becoming a significant threat. We need them eliminated.');
INSERT INTO `qstEncounter` VALUES (60102, 131790, 'Rogue Drone Harassment', 2, 2, 2, b'0', b'0', 0, 100000, 0, 0, 35000, 30, 4, 0, 829, 'A large swarm of rogue drones has been disrupting operations in the area. These drones are more advanced than usual. Proceed with caution and destroy them all.');
INSERT INTO `qstEncounter` VALUES (60103, 131794, 'Keeping Crime In Check', 2, 2, 2, b'0', b'0', 0, 90000, 0, 0, 30000, 30, 5, 0, 818, 'Organized criminal gangs have set up operations in the area and their forces are well-armed. We need you to dismantle their operation by destroying all their ships.');
INSERT INTO `qstEncounter` VALUES (60104, 131800, 'Unauthorized Military Presence', 2, 2, 2, b'0', b'0', 0, 150000, 0, 0, 50000, 35, 6, 0, 829, 'A military force has crossed into our jurisdiction without authorization. They have brought destroyer-class ships. This cannot be tolerated. Remove them.');
INSERT INTO `qstEncounter` VALUES (60105, 131802, 'The Score', 2, 2, 2, b'0', b'0', 0, 180000, 0, 0, 60000, 35, 5, 0, 829, 'Pirates have been attacking our assets with impunity. It is time to send a strong message. Locate their destroyer patrol and wipe them out.');
INSERT INTO `qstEncounter` VALUES (60106, 131806, 'Supply Interdiction', 2, 2, 2, b'0', b'0', 0, 85000, 0, 0, 28000, 30, 4, 0, 818, 'Our supply routes are under sustained attack. The enemy has stepped up their operations with better-equipped ships. Destroy them and secure the route.');
INSERT INTO `qstEncounter` VALUES (60107, 136824, 'Avenge a Fallen Comrade', 2, 2, 2, b'0', b'0', 0, 160000, 0, 0, 55000, 35, 6, 0, 829, 'A colleague of ours was killed in action recently. The perpetrators have been tracked to a location not far from here. They have destroyer-class vessels. Make them pay.');
INSERT INTO `qstEncounter` VALUES (60108, 131808, 'Intercept The Saboteurs', 2, 2, 2, b'0', b'0', 0, 95000, 0, 0, 32000, 30, 5, 0, 818, 'A team of enemy saboteurs has been identified preparing for a major strike. They are better equipped than the last group. Intercept and destroy them before they can act.');
INSERT INTO `qstEncounter` VALUES (60109, 127530, 'The Disgruntled Employee', 2, 2, 2, b'0', b'0', 0, 200000, 0, 0, 70000, 40, 6, 0, 829, 'A former employee has gone rogue and is now selling classified information. He has hired mercenaries for protection. Eliminate the entire group.');
INSERT INTO `qstEncounter` VALUES (60110, 131810, 'Smuggler Interception', 2, 2, 2, b'0', b'0', 0, 80000, 0, 0, 25000, 30, 4, 0, 818, 'A major smuggling ring is moving high-value contraband through the area. They have upgraded their escort ships. Intercept and destroy them.');

-- Level 3 missions (5-8 cruisers/destroyers, 250k-600k ISK, 40min bonus)
INSERT INTO `qstEncounter` VALUES (60201, 131790, 'Seek and Destroy', 3, 2, 3, b'0', b'0', 0, 400000, 0, 0, 130000, 40, 6, 0, 817, 'A formidable enemy fleet has established a stronghold in the area. They have cruisers and battlecruisers on station. We need this threat neutralized immediately. Seek and destroy all targets.');
INSERT INTO `qstEncounter` VALUES (60202, 131808, 'Rogue Drone Harassment', 3, 2, 3, b'0', b'0', 0, 350000, 0, 0, 115000, 40, 5, 0, 817, 'An advanced rogue drone hive has been detected. The drones are cruiser-class and extremely aggressive. This is a serious threat that must be eliminated before it spreads.');
INSERT INTO `qstEncounter` VALUES (60203, 131794, 'Keeping Crime In Check', 3, 2, 3, b'0', b'0', 0, 300000, 0, 0, 100000, 40, 6, 0, 817, 'A powerful criminal syndicate has fortified their position in the area. They have heavy ships and will fight back hard. We need you to break their defenses and destroy them all.');
INSERT INTO `qstEncounter` VALUES (60204, 131810, 'Unauthorized Military Presence', 3, 2, 3, b'0', b'0', 0, 500000, 0, 0, 165000, 45, 7, 0, 817, 'A significant unauthorized military deployment has been confirmed in our space. They have brought battlecruiser-class vessels. Engage and destroy the entire force.');
INSERT INTO `qstEncounter` VALUES (60205, 143457, 'The Blockade', 3, 2, 3, b'0', b'0', 0, 550000, 0, 0, 180000, 45, 8, 0, 817, 'The enemy has established a blockade and is preventing all traffic from passing through. They have deployed a well-armed fleet. Break through the blockade and destroy every ship.');
INSERT INTO `qstEncounter` VALUES (60206, 131812, 'Worlds Collide', 3, 2, 3, b'0', b'0', 0, 600000, 0, 0, 200000, 50, 8, 0, 817, 'Two rival factions have been fighting in the area and their conflict is spilling into our territory. Both sides need to be eliminated. Move in and destroy all hostile forces.');
INSERT INTO `qstEncounter` VALUES (60207, 139011, 'Alluring Emanations', 3, 2, 3, b'0', b'0', 0, 450000, 0, 0, 150000, 45, 7, 0, 817, 'We have detected strange energy emanations from a nearby location. Investigation has revealed it is a trap set by pirates using the signal as bait. Destroy the ambush force.');
INSERT INTO `qstEncounter` VALUES (60208, 143458, 'Retribution', 3, 2, 3, b'0', b'0', 0, 380000, 0, 0, 125000, 40, 6, 0, 817, 'An act of aggression against our people demands retribution. A significant enemy force is stationed at the coordinates we are providing you. Make them regret their actions.');

-- Level 4 missions (6-10 battleships/cruisers, 800k-2M ISK, 60min bonus)
INSERT INTO `qstEncounter` VALUES (60301, 131794, 'Seek and Destroy', 4, 2, 4, b'0', b'0', 0, 1200000, 0, 0, 400000, 60, 8, 0, 816, 'Our intelligence has uncovered a major enemy staging area. They have deployed battleships and support vessels. This will be a difficult fight, but we need this threat removed. Destroy everything.');
INSERT INTO `qstEncounter` VALUES (60302, 131802, 'The Blockade', 4, 2, 4, b'0', b'0', 0, 1800000, 0, 0, 600000, 70, 10, 0, 816, 'The enemy has established a massive blockade using battleship-class vessels. All civilian and military traffic has been halted. We are authorizing you to use maximum force to break through and destroy their fleet.');
INSERT INTO `qstEncounter` VALUES (60303, 131810, 'Worlds Collide', 4, 2, 4, b'0', b'0', 0, 2000000, 0, 0, 700000, 75, 10, 0, 816, 'Multiple hostile factions have converged on this location, creating a warzone. Battleships from both sides are engaged. Enter the fray and ensure no hostile vessel survives.');
INSERT INTO `qstEncounter` VALUES (60304, 143457, 'Unauthorized Military Presence', 4, 2, 4, b'0', b'0', 0, 1500000, 0, 0, 500000, 65, 9, 0, 816, 'A full military task force has been deployed in our territory without authorization. They have battleships and heavy support. This is an act of war. Engage and destroy them all.');
INSERT INTO `qstEncounter` VALUES (60305, 143458, 'Keeping Crime In Check', 4, 2, 4, b'0', b'0', 0, 1000000, 0, 0, 330000, 60, 7, 0, 828, 'A major criminal organization has assembled a battle fleet. This is the largest concentration of criminal firepower we have seen in years. You are authorized to use whatever force is necessary.');
INSERT INTO `qstEncounter` VALUES (60306, 131812, 'Retribution', 4, 2, 4, b'0', b'0', 0, 900000, 0, 0, 300000, 55, 6, 0, 828, 'The time for surgical strikes is over. We are launching a full retribution operation against the enemy. A battleship fleet awaits you at the target coordinates. Show them no mercy.');

-- Important L4 missions (storyline-tier, higher rewards)
INSERT INTO `qstEncounter` VALUES (60350, 131794, 'Break Their Will', 4, 2, 4, b'1', b'0', 0, 3000000, 0, 0, 1000000, 90, 12, 0, 816, 'This is a critical priority mission. The enemy has assembled their strongest forces at a single location. We need you to break their will entirely. Expect overwhelming resistance from battleship-class vessels and heavy support. Only the most skilled pilots should attempt this mission.');
INSERT INTO `qstEncounter` VALUES (60351, 131802, 'Duo of Death', 4, 2, 4, b'1', b'0', 0, 2500000, 0, 0, 850000, 80, 10, 0, 816, 'Two of the most dangerous enemy commanders in the region have joined forces. They have a combined fleet of battleships at their disposal. This is a rare opportunity to take out both targets simultaneously. The rewards for success are substantial, but so is the danger.');
