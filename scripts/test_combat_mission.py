#!/usr/bin/env python3
"""
See A371 Bug11 — Automated combat mission lifecycle test.
Exercises the full Encounter mission flow via the AI command queue,
without requiring an EVE client login.

Usage:
    python3 scripts/test_combat_mission.py [--agent 3008938] [--char 90000000]

The script:
  1. Clears any existing encounter missions for the character
  2. Inserts a 'mission_test' command into ai_command_queue
  3. Polls for the result (up to 30s)
  4. Reports PASS/FAIL based on whether NPCs spawned and IsMissionComplete is FALSE
"""
import subprocess
import sys
import time
import argparse
import json

DOCKER_CMD = ["docker", "exec", "evemu_db", "mariadb", "-u", "evemu", "-pevemu", "evemu", "-e"]

def db_query(sql):
    """Run a SQL query against the EVEmu database and return raw output."""
    result = subprocess.run(DOCKER_CMD + [sql], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  DB ERROR: {result.stderr.strip()}")
        return None
    return result.stdout.strip()

def db_query_one(sql):
    """Run a SQL query and return the first non-header line."""
    out = db_query(sql)
    if out is None:
        return None
    lines = out.strip().split("\n")
    if len(lines) < 2:
        return None
    return lines[1].strip()

def main():
    parser = argparse.ArgumentParser(description="Test combat mission lifecycle")
    parser.add_argument("--agent", type=int, default=3008938, help="Agent ID (default: Toumzie Fusi)")
    parser.add_argument("--char", type=int, default=90000000, help="Character ID (default: Malcolm Reynolds)")
    parser.add_argument("--clean", action="store_true", help="Clean up existing missions first")
    args = parser.parse_args()

    agent_id = args.agent
    char_id = args.char

    print(f"=== Combat Mission Lifecycle Test ===")
    print(f"Agent: {agent_id}, Character: {char_id}")
    print()

    # Step 0: Check current mission state
    print("[Step 0] Checking current mission state...")
    existing = db_query(
        f"SELECT offerID, name, stateID, typeID, dungeonLocationID "
        f"FROM agtOffers WHERE characterID = {char_id} AND agentID = {agent_id} "
        f"AND stateID IN (1,2) ORDER BY offerID DESC LIMIT 1"
    )
    if existing and len(existing.split("\n")) > 1:
        print(f"  Active mission found: {existing.split(chr(10))[1]}")
        if args.clean:
            print("  --clean: Clearing active missions...")
            db_query(f"UPDATE agtOffers SET stateID = 5 WHERE characterID = {char_id} "
                     f"AND agentID = {agent_id} AND stateID IN (1,2)")
            print("  Cleared.")
        else:
            print("  (Use --clean to clear existing missions first)")
    else:
        print("  No active missions.")

    # Step 1: Insert mission_test command
    print()
    print("[Step 1] Inserting mission_test command into AI command queue...")
    params_json = json.dumps({"agentID": agent_id})
    # Escape single quotes for SQL
    params_sql = params_json.replace("'", "\\'")
    db_query(
        f"INSERT INTO ai_command_queue (charID, command, params, status) "
        f"VALUES ({char_id}, 'mission_test', '{params_sql}', 'pending')"
    )
    print("  Command inserted.")

    # Step 2: Poll for result (AI command queue processes every 5s)
    print()
    print("[Step 2] Waiting for server to process command (up to 30s)...")
    for i in range(12):
        time.sleep(3)
        result = db_query(
            f"SELECT status, result_msg FROM ai_command_queue "
            f"WHERE charID = {char_id} AND command = 'mission_test' "
            f"ORDER BY id DESC LIMIT 1"
        )
        if result and ("done" in result or "failed" in result):
            lines = result.strip().split("\n")
            if len(lines) >= 2:
                parts = lines[1].split("\t", 1)
                status = parts[0] if len(parts) > 0 else "?"
                msg = parts[1] if len(parts) > 1 else "?"
                print(f"  Status: {status}")
                print(f"  Result: {msg}")
                print()

                # Step 3: Analyze result
                print("[Step 3] Analysis:")
                if "CountNPCs=" in msg:
                    # Extract NPC count
                    npc_part = msg.split("CountNPCs=")[1].split(" ")[0].split(")")[0]
                    npc_count = int(npc_part) if npc_part.isdigit() else -1

                    if "FALSE(correct)" in msg and npc_count > 0:
                        print(f"  ✓ PASS: Mission accepted, {npc_count} NPCs spawned, IsMissionComplete=FALSE")
                        print(f"  The mission is working correctly server-side.")
                    elif "TRUE(BUG!)" in msg:
                        print(f"  ✗ FAIL: IsMissionComplete returned TRUE immediately!")
                        print(f"  NPCs: {npc_count}. This is the bug — mission shows as complete on accept.")
                    elif npc_count == 0:
                        print(f"  ✗ FAIL: 0 NPCs spawned! Check DoSpawnForMission logs.")
                    else:
                        print(f"  ? UNKNOWN result: {msg}")
                elif "EXISTING" in msg:
                    print(f"  ℹ Existing mission found. Use --clean to start fresh.")
                elif "non-encounter" in msg:
                    print(f"  ℹ Got a non-encounter mission (random selection). Run again.")
                else:
                    print(f"  ? Result: {msg}")

                # Step 4: Check mission in DB
                print()
                print("[Step 4] Final DB state:")
                final = db_query(
                    f"SELECT offerID, name, stateID, typeID, dungeonLocationID, dungeonSolarSystemID "
                    f"FROM agtOffers WHERE characterID = {char_id} AND agentID = {agent_id} "
                    f"AND stateID = 2 ORDER BY offerID DESC LIMIT 1"
                )
                if final:
                    print(f"  {final}")
                else:
                    print("  No accepted mission found.")

                # Also check server logs for related output
                print()
                print("[Step 5] Recent server logs (DoSpawnForMission):")
                log_result = subprocess.run(
                    ["docker", "logs", "--tail", "50", "evemu_server"],
                    capture_output=True, text=True
                )
                for line in (log_result.stdout + log_result.stderr).split("\n"):
                    if any(kw in line for kw in ["DoSpawnForMission", "SetupEncounter", "IsMissionComplete", "mission_test"]):
                        print(f"  {line.strip()}")

                return 0 if "PASS" in msg or "FALSE(correct)" in msg else 1

        sys.stdout.write(".")
        sys.stdout.flush()

    print()
    print("  TIMEOUT: Command not processed within 30s. Is the server running?")
    return 2

if __name__ == "__main__":
    sys.exit(main())
