#!/usr/bin/env python3
"""
EVEmu Orbit Simulation Test Harness
See A371 §12 (Combat Desync Analysis)

Replicates the server's DestinyManager::Orbit() tick processor in Python
to diagnose position drift without needing the game client running.

Usage:
    python3 orbit_test_harness.py              # Run all scenarios
    python3 orbit_test_harness.py --scenario 1 # Run specific scenario
    python3 orbit_test_harness.py --verbose     # Print every tick
"""
import math
import sys
import argparse

# ============================================================================
# Constants — matching DestinyManager.cpp
# ============================================================================
Pi2 = 2.0 * math.pi          # EvE::Trig::Pi2
BUBBLE_RADIUS_METERS = 300000 # BubbleManager.h
NULL_ORIGIN = (0.0, 0.0, 0.0)


def deg2rad(deg):
    """EvE::Trig::Deg2Rad()"""
    return deg * math.pi / 180.0


def distance(a, b):
    return math.sqrt((a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2)


def vec_sub(a, b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])


def vec_add(a, b):
    return (a[0]+b[0], a[1]+b[1], a[2]+b[2])


def vec_scale(v, s):
    return (v[0]*s, v[1]*s, v[2]*s)


def vec_length(v):
    return math.sqrt(v[0]**2 + v[1]**2 + v[2]**2)


def vec_normalize(v):
    l = vec_length(v)
    if l < 1e-12:
        return (0.0, 0.0, 0.0)
    return (v[0]/l, v[1]/l, v[2]/l)


# ============================================================================
# Ship/Entity data class
# ============================================================================
class Entity:
    def __init__(self, name, entity_id, position, mass, radius,
                 max_ship_speed, agility, orbit_target_id=None,
                 orbit_distance=0):
        self.name = name
        self.entity_id = entity_id
        self.position = position
        self.mass = mass
        self.radius = radius
        self.max_ship_speed = max_ship_speed
        self.agility = agility

        # Orbit state
        self.ball_mode = 'STOP'  # STOP, ORBIT, FOLLOW, GOTO
        self.orbit_target_id = orbit_target_id
        self.orbit_distance = orbit_distance  # commanded distance (m_targetDistance)
        self.follow_distance = 0.0  # calculated orbit radius (m_followDistance)
        self.orbit_time = 0.0
        self.orbit_rad_tic = 0.0
        self.max_orbit_speed_fraction = 0.0
        self.orbit_center = NULL_ORIGIN
        self.orbiting = 0  # 0=none, 1=Orbiting, 2=Close, 3=Far, 4=TooClose, 5=TooFar
        self.state_stamp = 0  # tick when orbit started

        # Speed state
        self.user_speed_fraction = 0.0
        self.active_speed_fraction = 0.0
        self.velocity = (0.0, 0.0, 0.0)
        self.ship_heading = (0.0, 0.0, 0.0)

    def is_orbiting(self):
        return self.orbiting > 0

    def get_orbit_center(self):
        return self.orbit_center

    def get_max_velocity(self):
        return self.max_ship_speed


# ============================================================================
# Orbit Setup — matches DestinyManager::Orbit(SE*, distance)
# ============================================================================
def setup_orbit(entity, target, dist, current_tick):
    """Initialize orbit state — matches Orbit(SystemEntity*, uint32 distance)"""
    entity.ball_mode = 'ORBIT'
    entity.orbiting = 1  # Orbiting
    entity.orbit_target_id = target.entity_id
    entity.orbit_distance = float(dist)
    entity.orbit_center = target.position  # m_orbitCenter = pSE->GetPosition()
    entity.state_stamp = current_tick
    entity.user_speed_fraction = 1.0
    entity.active_speed_fraction = 1.0

    # Follow distance calculation — from Orbit(SE*, distance) lines 2440-2460
    Tr = target.radius
    Rc = (dist + 150 + entity.radius - (Tr / 12)) * 1.2
    Rc2 = Rc ** 2
    Vm2 = entity.max_ship_speed ** 2
    t2 = entity.agility ** 2

    # Scheulagh Santorine's orbital radius equation
    one = 108 * t2 * Vm2 * Rc2
    two = 12 * t2 * Vm2 * Rc**10
    three = 12 * math.sqrt(abs(81 * entity.agility**4 * entity.max_ship_speed**4 + two))
    four = 6 * (abs(one + 8 * Rc**6 + three) ** (1/3))
    five = (abs(three * Rc**8 + two)) ** (1/3)
    six = one + 8 * Rc2 + 12 * five

    if six > 0 and four > 0:
        entity.follow_distance = math.sqrt(abs(four + 24 * Rc**4 / six + 12 * Rc2)) / 6
    else:
        entity.follow_distance = dist + 500  # fallback

    velocity = entity.max_ship_speed * ((dist / entity.follow_distance) + 0.065)
    entity.max_orbit_speed_fraction = velocity / entity.max_ship_speed if entity.max_ship_speed > 0 else 0

    circ = Pi2 * entity.follow_distance
    entity.orbit_time = circ / velocity if velocity > 0 else 1.0
    entity.orbit_rad_tic = Pi2 / entity.orbit_time if entity.orbit_time > 0 else 0

    print(f"  [{entity.name}] Orbit setup: dist={dist}m, followDist={entity.follow_distance:.0f}m, "
          f"orbitTime={entity.orbit_time:.1f}s, radTic={entity.orbit_rad_tic:.5f}, "
          f"osf={entity.max_orbit_speed_fraction:.3f}")


# ============================================================================
# Orbit Tick Processor — matches DestinyManager::Orbit()
# ============================================================================
def orbit_tick(entity, entities_by_id, current_tick, verbose=False):
    """Process one tick of orbit — matches the C++ Orbit() tick processor"""
    if entity.ball_mode != 'ORBIT':
        return

    target = entities_by_id.get(entity.orbit_target_id)
    if target is None:
        return

    timestamp = current_tick - entity.state_stamp
    Tr = target.radius

    # === BUG 22.1 MUTUAL ORBIT DETECTION ===
    target_dm_exists = True  # always true in our sim
    raw_tp = None

    if (target_dm_exists and target.is_orbiting()
            and target.orbit_target_id == entity.entity_id):
        # Mutual orbit: use target's orbit CENTER
        raw_tp = target.get_orbit_center()
        mutual = True
    else:
        # Not mutual: use target's raw position
        raw_tp = target.position
        mutual = False

    # Rate clamp
    max_target_speed = 100.0
    if target_dm_exists:
        max_target_speed = max(max_target_speed, target.get_max_velocity() * 1.5)

    tp_delta = vec_sub(raw_tp, entity.orbit_center)
    tp_moved = vec_length(tp_delta)

    if tp_moved > max_target_speed and tp_moved > 0.01:
        tp_dir = vec_normalize(tp_delta)
        entity.orbit_center = vec_add(entity.orbit_center, vec_scale(tp_dir, max_target_speed))
    else:
        entity.orbit_center = raw_tp

    Tp = entity.orbit_center

    # Distance checks
    centers = distance(entity.position, Tp)
    edges = centers - entity.radius - Tr

    mPos_adj = 0.0
    if (edges / 2) > entity.follow_distance:
        # TooFar — entity keeps moving toward target
        entity.orbiting = 5  # TooFar
        # For TooFar: compute heading toward target+offset and MoveObject
        # Simplified: just move toward target
        heading = vec_normalize(vec_sub(Tp, entity.position))
        entity.ship_heading = heading
        speed = entity.max_ship_speed * entity.active_speed_fraction
        entity.velocity = vec_scale(heading, speed)
        entity.position = vec_add(entity.position, entity.velocity)
        return
    elif (centers + entity.orbit_distance / 3) < entity.follow_distance:
        entity.orbiting = 4  # TooClose
        heading = vec_normalize(vec_sub(entity.position, Tp))
        entity.ship_heading = heading
        speed = entity.max_ship_speed * entity.active_speed_fraction
        entity.velocity = vec_scale(heading, speed)
        entity.position = vec_add(entity.position, entity.velocity)
        return
    elif (edges - entity.orbit_distance / 4) > entity.follow_distance:
        entity.orbiting = 3  # Far
        mPos_adj = -entity.follow_distance / 25
    elif centers < entity.follow_distance:
        entity.orbiting = 2  # Close
        mPos_adj = entity.follow_distance / 25
    else:
        entity.orbiting = 1  # Orbiting

    # === ORBIT POSITION CALCULATION ===
    radius = entity.follow_distance + mPos_adj
    theta = Pi2 - deg2rad(360) - (entity.orbit_rad_tic * timestamp)
    inclination = 45.0
    period = (timestamp % entity.orbit_time) / entity.orbit_time if entity.orbit_time > 0 else 0
    c = math.cos(deg2rad(360 * period))
    phi = deg2rad(inclination * c)
    # s = math.sin(deg2rad(360 * period))  # mu not used in final position
    # mu = deg2rad(inclination * s)

    mPos = (
        radius * math.cos(theta),
        radius * phi,
        radius * math.sin(theta)
    )

    # Apply orbit center offset
    entity.position = vec_add(mPos, Tp)

    # NaN check
    if any(math.isnan(v) for v in entity.position):
        print(f"  [{entity.name}] NaN detected! Resetting to orbit center.")
        entity.position = Tp

    # Bug22: Position set directly, NO velocity addition (orbit states 1,2,3)
    # (This is handled by NOT adding velocity here)

    if verbose and (timestamp % 10 == 0 or timestamp < 5):
        drift = distance(entity.position, Tp)
        print(f"  t={timestamp:4d} [{entity.name}] pos=({entity.position[0]:.0f},{entity.position[1]:.0f},{entity.position[2]:.0f}) "
              f"orbit_center_moved={tp_moved:.1f}m mutual={mutual} orbit_state={entity.orbiting} drift_from_center={drift:.0f}m")


# ============================================================================
# Follow Tick Processor — simplified version of DestinyManager::Follow()
# ============================================================================
def follow_tick(entity, entities_by_id, current_tick, verbose=False):
    """Process one tick of follow — simplified version"""
    if entity.ball_mode != 'FOLLOW':
        return

    target = entities_by_id.get(entity.orbit_target_id)
    if target is None:
        return

    Tp = target.position
    dist_to_target = distance(entity.position, Tp)

    if dist_to_target <= entity.orbit_distance:
        # Close enough, stop
        return

    # Move toward target
    heading = vec_normalize(vec_sub(Tp, entity.position))
    speed = entity.max_ship_speed * entity.active_speed_fraction
    entity.velocity = vec_scale(heading, speed)
    entity.position = vec_add(entity.position, entity.velocity)
    entity.ship_heading = heading


# ============================================================================
# Simulation Runner
# ============================================================================
def run_simulation(scenario_name, entities, duration_ticks, verbose=False):
    """Run a simulation for N ticks and report results."""
    print(f"\n{'='*80}")
    print(f"SCENARIO: {scenario_name}")
    print(f"Duration: {duration_ticks} ticks ({duration_ticks}s)")
    print(f"{'='*80}")

    entities_by_id = {e.entity_id: e for e in entities}
    initial_positions = {e.entity_id: e.position for e in entities}
    bubble_center = entities[0].position  # use player's initial position

    max_drift = {e.entity_id: 0.0 for e in entities}

    for tick in range(1, duration_ticks + 1):
        # Process all entities
        for entity in entities:
            if entity.ball_mode == 'ORBIT':
                orbit_tick(entity, entities_by_id, tick, verbose)
            elif entity.ball_mode == 'FOLLOW':
                follow_tick(entity, entities_by_id, tick, verbose)

        # Track drift
        for entity in entities:
            drift = distance(entity.position, initial_positions[entity.entity_id])
            max_drift[entity.entity_id] = max(max_drift[entity.entity_id], drift)

            # Check for bubble ejection
            bubble_dist = distance(entity.position, bubble_center)
            if bubble_dist > BUBBLE_RADIUS_METERS and (tick % 10 == 0 or tick < 5):
                print(f"  *** BUBBLE EJECTION at t={tick}s: [{entity.name}] "
                      f"dist_from_bubble={bubble_dist/1000:.1f}km (max {BUBBLE_RADIUS_METERS/1000:.0f}km)")

    # Final report
    print(f"\n--- Results after {duration_ticks}s ---")
    for entity in entities:
        init_pos = initial_positions[entity.entity_id]
        final_pos = entity.position
        final_drift = distance(final_pos, init_pos)
        orbit_center_drift = distance(entity.orbit_center, init_pos) if entity.ball_mode == 'ORBIT' else 0
        print(f"  [{entity.name}] displacement: {final_drift/1000:.1f}km  |  "
              f"orbit_center_drift: {orbit_center_drift/1000:.1f}km  |  "
              f"max_drift: {max_drift[entity.entity_id]/1000:.1f}km")
    print()


# ============================================================================
# Scenarios
# ============================================================================

def scenario_1_mutual_orbit():
    """
    Scenario 1: Mutual orbit — Player orbits NPC, NPC orbits Player
    Replicates the 'Avenge a Fallen Comrade' conditions:
    - Player Punisher at bubble center
    - NPC Imai Kenon (21km orbit, 900 m/s) starts nearby and orbits player
    - Player orbits NPC at 5000m
    """
    # Player: Punisher
    player = Entity(
        name="Punisher", entity_id=140000155,
        position=(38695506917.0, -289901233047.0, 63071582303.0),
        mass=1047000, radius=34, max_ship_speed=287.0, agility=4.21
    )

    # NPC: Imai Kenon (typeID 17625, 21km orbit, 900 m/s)
    npc = Entity(
        name="Imai_Kenon", entity_id=750000003,
        position=(38695507000.0, -289901233000.0, 63071582000.0),  # ~500m from player
        mass=1970000, radius=45.98, max_ship_speed=900.0, agility=3.0
    )

    # Setup orbits
    setup_orbit(npc, player, 21000, 0)   # NPC orbits player at 21km
    setup_orbit(player, npc, 5000, 0)    # Player orbits NPC at 5km

    run_simulation(
        "Mutual Orbit: Player↔NPC (Imai Kenon 21km, 900m/s)",
        [player, npc], duration_ticks=120, verbose=True
    )


def scenario_2_non_mutual_orbit():
    """
    Scenario 2: Non-mutual — NPC follows player, player orbits NPC
    What if NPC is in FOLLOW mode (SetFollowing) instead of ORBIT?
    """
    player = Entity(
        name="Punisher", entity_id=140000155,
        position=(38695506917.0, -289901233047.0, 63071582303.0),
        mass=1047000, radius=34, max_ship_speed=287.0, agility=4.21
    )

    npc = Entity(
        name="NPC_Following", entity_id=750000003,
        position=(38695507000.0, -289901233000.0, 63071582000.0),
        mass=1970000, radius=45.98, max_ship_speed=900.0, agility=3.0
    )

    # NPC follows player (not orbiting!)
    npc.ball_mode = 'FOLLOW'
    npc.orbit_target_id = player.entity_id
    npc.orbit_distance = 15000
    npc.user_speed_fraction = 1.0
    npc.active_speed_fraction = 1.0

    # Player orbits NPC
    setup_orbit(player, npc, 5000, 0)

    run_simulation(
        "Non-Mutual: Player orbits NPC, NPC follows Player",
        [player, npc], duration_ticks=120, verbose=True
    )


def scenario_3_five_npcs():
    """
    Scenario 3: Full 'Avenge a Fallen Comrade' battle
    5 NPCs all orbiting the player, player orbits one of them
    """
    start_pos = (38695506917.0, -289901233047.0, 63071582303.0)

    player = Entity(
        name="Punisher", entity_id=140000155,
        position=start_pos,
        mass=1047000, radius=34, max_ship_speed=287.0, agility=4.21
    )

    # 5 NPCs, offset slightly from player
    npcs = [
        Entity("Merc_Fighter", 750000001, vec_add(start_pos, (200, 0, 300)),
               1970000, 45.98, 520.0, 3.0),
        Entity("Dari_Akell", 750000002, vec_add(start_pos, (-300, 100, 200)),
               1400000, 39.0, 520.0, 3.0),
        Entity("Imai_Kenon", 750000003, vec_add(start_pos, (400, -200, -100)),
               1970000, 45.98, 900.0, 3.0),
        Entity("Bounty_Rookie", 750000004, vec_add(start_pos, (-200, 300, -400)),
               1910000, 43.6, 520.0, 3.0),
        Entity("Guemo_Kajinn", 750000005, vec_add(start_pos, (100, -100, 500)),
               1200000, 38.0, 400.0, 3.0),
    ]

    orbit_distances = [15000, 2500, 21000, 10000, 15000]

    # All NPCs orbit player
    for npc, dist in zip(npcs, orbit_distances):
        setup_orbit(npc, player, dist, 0)

    # Player orbits Imai Kenon
    setup_orbit(player, npcs[2], 5000, 0)

    entities = [player] + npcs
    run_simulation(
        "Full Battle: 5 NPCs orbiting player, player orbits Imai Kenon",
        entities, duration_ticks=120, verbose=True
    )


def scenario_4_no_mutual_detection():
    """
    Scenario 4: REMOVE mutual orbit detection — show the bug without the fix
    This simulates what happens if targetDM->IsOrbiting() check is removed.
    """
    player = Entity(
        name="Punisher_NOFIX", entity_id=140000155,
        position=(38695506917.0, -289901233047.0, 63071582303.0),
        mass=1047000, radius=34, max_ship_speed=287.0, agility=4.21
    )

    npc = Entity(
        name="Imai_NOFIX", entity_id=750000003,
        position=(38695507000.0, -289901233000.0, 63071582000.0),
        mass=1970000, radius=45.98, max_ship_speed=900.0, agility=3.0
    )

    setup_orbit(npc, player, 21000, 0)
    setup_orbit(player, npc, 5000, 0)

    # Monkey-patch: override orbit_tick to always use raw position (no mutual detection)
    entities_by_id = {e.entity_id: e for e in [player, npc]}
    initial_positions = {e.entity_id: e.position for e in [player, npc]}
    bubble_center = player.position

    print(f"\n{'='*80}")
    print(f"SCENARIO: NO-FIX Mutual Orbit (raw position, no orbit center detection)")
    print(f"Duration: 120 ticks (120s)")
    print(f"{'='*80}")

    for tick in range(1, 121):
        for entity in [player, npc]:
            if entity.ball_mode != 'ORBIT':
                continue
            target = entities_by_id[entity.orbit_target_id]
            timestamp = tick - entity.state_stamp

            # ALWAYS use raw position (simulating no mutual orbit detection)
            raw_tp = target.position

            # Rate clamp still active
            max_target_speed = max(100.0, target.get_max_velocity() * 1.5)
            tp_delta = vec_sub(raw_tp, entity.orbit_center)
            tp_moved = vec_length(tp_delta)
            if tp_moved > max_target_speed and tp_moved > 0.01:
                tp_dir = vec_normalize(tp_delta)
                entity.orbit_center = vec_add(entity.orbit_center, vec_scale(tp_dir, max_target_speed))
            else:
                entity.orbit_center = raw_tp

            Tp = entity.orbit_center
            centers = distance(entity.position, Tp)
            edges = centers - entity.radius - target.radius

            mPos_adj = 0.0
            if (edges / 2) > entity.follow_distance:
                entity.orbiting = 5
                heading = vec_normalize(vec_sub(Tp, entity.position))
                speed = entity.max_ship_speed * entity.active_speed_fraction
                entity.position = vec_add(entity.position, vec_scale(heading, speed))
                continue
            elif (centers + entity.orbit_distance / 3) < entity.follow_distance:
                entity.orbiting = 4
                heading = vec_normalize(vec_sub(entity.position, Tp))
                speed = entity.max_ship_speed * entity.active_speed_fraction
                entity.position = vec_add(entity.position, vec_scale(heading, speed))
                continue
            elif (edges - entity.orbit_distance / 4) > entity.follow_distance:
                entity.orbiting = 3
                mPos_adj = -entity.follow_distance / 25
            elif centers < entity.follow_distance:
                entity.orbiting = 2
                mPos_adj = entity.follow_distance / 25
            else:
                entity.orbiting = 1

            radius = entity.follow_distance + mPos_adj
            theta = Pi2 - deg2rad(360) - (entity.orbit_rad_tic * timestamp)
            period = (timestamp % entity.orbit_time) / entity.orbit_time if entity.orbit_time > 0 else 0
            c_val = math.cos(deg2rad(360 * period))
            phi_val = deg2rad(45.0 * c_val)

            mPos = (radius * math.cos(theta), radius * phi_val, radius * math.sin(theta))
            entity.position = vec_add(mPos, Tp)

        # Report every 10 ticks
        if tick % 10 == 0 or tick < 5:
            for entity in [player, npc]:
                drift = distance(entity.position, initial_positions[entity.entity_id])
                bubble_dist = distance(entity.position, bubble_center)
                print(f"  t={tick:4d} [{entity.name}] drift={drift/1000:.1f}km "
                      f"bubble_dist={bubble_dist/1000:.1f}km "
                      f"orbit_state={entity.orbiting} "
                      f"oc_moved={distance(entity.orbit_center, initial_positions[entity.entity_id])/1000:.1f}km")
                if bubble_dist > BUBBLE_RADIUS_METERS:
                    print(f"         *** BUBBLE EJECTION ***")

    print(f"\n--- Final ---")
    for entity in [player, npc]:
        drift = distance(entity.position, initial_positions[entity.entity_id])
        print(f"  [{entity.name}] total_drift: {drift/1000:.1f}km")


def scenario_5_no_rate_clamp():
    """
    Scenario 5: NO rate clamp, NO mutual detection — the original bug
    Simulates what the server did before any Bug 22.1 fixes.
    """
    player = Entity(
        name="Punisher_ORIG", entity_id=140000155,
        position=(38695506917.0, -289901233047.0, 63071582303.0),
        mass=1047000, radius=34, max_ship_speed=287.0, agility=4.21
    )

    npc = Entity(
        name="Imai_ORIG", entity_id=750000003,
        position=(38695507000.0, -289901233000.0, 63071582000.0),
        mass=1970000, radius=45.98, max_ship_speed=900.0, agility=3.0
    )

    setup_orbit(npc, player, 21000, 0)
    setup_orbit(player, npc, 5000, 0)

    entities_by_id = {e.entity_id: e for e in [player, npc]}
    initial_positions = {e.entity_id: e.position for e in [player, npc]}
    bubble_center = player.position

    print(f"\n{'='*80}")
    print(f"SCENARIO: ORIGINAL BUG — No rate clamp, no mutual detection")
    print(f"Duration: 120 ticks (120s)")
    print(f"{'='*80}")

    for tick in range(1, 121):
        for entity in [player, npc]:
            if entity.ball_mode != 'ORBIT':
                continue
            target = entities_by_id[entity.orbit_target_id]
            timestamp = tick - entity.state_stamp

            # NO mutual detection, NO rate clamp — just raw position
            entity.orbit_center = target.position

            Tp = entity.orbit_center
            centers = distance(entity.position, Tp)
            edges = centers - entity.radius - target.radius

            mPos_adj = 0.0
            if (edges / 2) > entity.follow_distance:
                entity.orbiting = 5
                heading = vec_normalize(vec_sub(Tp, entity.position))
                speed = entity.max_ship_speed * entity.active_speed_fraction
                entity.position = vec_add(entity.position, vec_scale(heading, speed))
                continue
            elif (centers + entity.orbit_distance / 3) < entity.follow_distance:
                entity.orbiting = 4
                heading = vec_normalize(vec_sub(entity.position, Tp))
                speed = entity.max_ship_speed * entity.active_speed_fraction
                entity.position = vec_add(entity.position, vec_scale(heading, speed))
                continue
            elif (edges - entity.orbit_distance / 4) > entity.follow_distance:
                entity.orbiting = 3
                mPos_adj = -entity.follow_distance / 25
            elif centers < entity.follow_distance:
                entity.orbiting = 2
                mPos_adj = entity.follow_distance / 25
            else:
                entity.orbiting = 1

            radius = entity.follow_distance + mPos_adj
            theta = Pi2 - deg2rad(360) - (entity.orbit_rad_tic * timestamp)
            period = (timestamp % entity.orbit_time) / entity.orbit_time if entity.orbit_time > 0 else 0
            c_val = math.cos(deg2rad(360 * period))
            phi_val = deg2rad(45.0 * c_val)

            mPos = (radius * math.cos(theta), radius * phi_val, radius * math.sin(theta))
            entity.position = vec_add(mPos, Tp)

        if tick % 10 == 0 or tick < 3:
            for entity in [player, npc]:
                drift = distance(entity.position, initial_positions[entity.entity_id])
                print(f"  t={tick:4d} [{entity.name}] drift={drift/1000:.1f}km orbit_state={entity.orbiting}")
                if drift > BUBBLE_RADIUS_METERS:
                    print(f"         *** BUBBLE EJECTION ***")

    print(f"\n--- Final ---")
    for entity in [player, npc]:
        drift = distance(entity.position, initial_positions[entity.entity_id])
        print(f"  [{entity.name}] total_drift: {drift/1000:.1f}km")


# ============================================================================
# Main
# ============================================================================
def main():
    parser = argparse.ArgumentParser(description="EVEmu Orbit Simulation Test Harness")
    parser.add_argument('--scenario', type=int, help='Run specific scenario (1-5)')
    parser.add_argument('--verbose', action='store_true', help='Print every tick')
    args = parser.parse_args()

    scenarios = {
        1: scenario_1_mutual_orbit,
        2: scenario_2_non_mutual_orbit,
        3: scenario_3_five_npcs,
        4: scenario_4_no_mutual_detection,
        5: scenario_5_no_rate_clamp,
    }

    if args.scenario:
        if args.scenario in scenarios:
            scenarios[args.scenario]()
        else:
            print(f"Unknown scenario {args.scenario}. Available: {list(scenarios.keys())}")
    else:
        # Run all scenarios
        for num, fn in sorted(scenarios.items()):
            fn()


if __name__ == '__main__':
    main()
