/*
 * Vev gateway implementation — Boost.Beast WSS server.
 *
 * Vev-1.0 (verified 2026-05-27 on Catalyst): echo scaffold.
 * Vev-1.1 (verified 2026-05-27 on Catalyst): JSON envelope + dispatch
 *   table + stub handlers.
 * Vev-1.2 (this commit): real handlers against evemu's MariaDB.
 *   - getCharacter   ← SELECT from chrCharacters JOIN chrRaces JOIN chrBloodlines
 *   - createCharacter ← INSERT into chrCharacters with race-native solarSystemID
 *   - login           ← still stub (real Google OAuth lands in vev-1.3)
 *
 *   Wire shape (unchanged from vev-1.1):
 *     request   ← { "type": "...", "id": "...", "payload": {...} }
 *     response  → { "type": "...", "id": "...", "result":  {...} }
 *           or  → { "type": "...", "id": "...", "error":   { "code":..., "message":... } }
 *
 * Threading: handlers run on the gateway's boost::asio worker thread.
 * sDatabase is the global DBcore singleton (eve-core/database/dbcore.h);
 * its internal Mutex makes RunQuery / GetRow thread-safe across the
 * gateway worker + the main eve-server loop. Read-only queries hit
 * MariaDB directly from this thread — same pattern Python agents use
 * via bridge/db_reader.py.
 *
 * Future hooks (vev-1.3+):
 *   - World-mutating handlers (undock, warpTo, transferItem, etc.) write
 *     to ai_command_queue. Main eve-server loop drains the queue and
 *     applies mutations under the existing single-writer discipline.
 *   - Auth middleware: validate Google JWT or UUID API token on the WS
 *     upgrade handshake; mutate Session to carry the authed identity.
 */

#include "Gateway.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "dep/nlohmann/json.hpp"

// evemu DB layer — sDatabase singleton + DBQueryResult / DBResultRow.
#include "database/dbcore.h"
#include "utils/Deflate.h"   // InflateData/IsDeflated/Buffer — for getMailBody

// Vev tactical-grid layer: subscriber sessions + the main-thread→gateway
// snapshot handoff bridge. The gateway owns these globals' lifetime (set in
// Start, below); the eve-server-side projector (GridStreamer.cpp) reads them.
#include "GridSession.h"
#include "GridStreamer.h"

#include <atomic>
#include <cmath>
#include <deque>
#include <vector>
#include <set>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>

namespace beast     = boost::beast;
namespace http      = boost::beast::http;
namespace websocket = boost::beast::websocket;
namespace asio      = boost::asio;
using tcp           = boost::asio::ip::tcp;
using json          = nlohmann::json;

namespace vev {

// ─── Response helpers ─────────────────────────────────────────────────────

static json makeResult(const std::string& type, const json& id, json result) {
    return json{
        {"type",   type},
        {"id",     id},
        {"result", std::move(result)},
    };
}

static json makeError(const std::string& type, const json& id,
                      const std::string& code, const std::string& message) {
    return json{
        {"type",  type},
        {"id",    id},
        {"error", { {"code", code}, {"message", message} }},
    };
}

// ─── Race + bloodline mapping (Vev → evemu chrRaces / chrBloodlines) ─────
//
// evemu's race/bloodline IDs are bit-flag style (1/2/4/8) and small ints.
// The Vev browser sends string ids ('amarr', 'ni_kunni', etc.) — these
// helpers translate. Mapping table verified against live evemu MariaDB
// 2026-05-27 (`SELECT * FROM chrRaces; SELECT * FROM chrBloodlines;`).

static uint32_t raceIdForString(const std::string& race) {
    if (race == "caldari")  return 1;
    if (race == "minmatar") return 2;
    if (race == "amarr")    return 4;
    if (race == "gallente") return 8;
    return 0;
}

static uint32_t bloodlineIdForString(const std::string& bloodline) {
    if (bloodline == "deteis")    return 1;
    if (bloodline == "civire")    return 2;
    if (bloodline == "sebiestor") return 3;
    if (bloodline == "brutor")    return 4;
    if (bloodline == "amarr")     return 5;
    if (bloodline == "ni_kunni")  return 6;
    if (bloodline == "gallente")  return 7;
    if (bloodline == "intaki")    return 8;
    if (bloodline == "achura")    return 11;
    if (bloodline == "jin_mei")   return 12;
    if (bloodline == "khanid")    return 13;
    if (bloodline == "vherokior") return 14;
    return 0;
}

/** Race-native starting system ID. Verified against live evemu MariaDB
 *  (`SELECT solarSystemID, solarSystemName FROM mapSolarSystems WHERE
 *  solarSystemName IN ('Amarr','New Caldari','Luminaire','Pator')`). */
static uint32_t spawnSolarSystemForRace(uint32_t raceID) {
    switch (raceID) {
        case 4: return 30002187; // Amarr (Domain)
        case 1: return 30000145; // New Caldari (The Forge)
        case 8: return 30004967; // Luminaire (Essence)
        case 2: return 30002544; // Pator (Heimatar)
        default: return 30002187;
    }
}

/** Race-native starter station — the dock the character spawns docked at.
 *  Verified by inspecting staStations rows in each race-capital system. */
static uint32_t spawnStationForRace(uint32_t raceID) {
    switch (raceID) {
        case 4: return 60008494; // Amarr VIII (Oris) - Emperor Family Academy
        case 1: return 60003334; // New Caldari Prime - Chief Executive Panel Bureau
        case 8: return 60011749; // Luminaire - Federation Navy Assembly Plant
        case 2: return 60004747; // Pator V (Vakir) - Republic Fleet Logistic Support
        default: return 60008494;
    }
}

// ─── Handlers ─────────────────────────────────────────────────────────────

static json handleLogin(const json& payload) {
    // vev-1.3 will validate the Google ID token here (humans) or look up
    // the UUID API token in vev_api_tokens (agents). For now accept any
    // payload and hand back a stub token.
    if (!payload.is_object()) {
        throw std::runtime_error("payload must be an object");
    }
    return json{
        {"token",     "stub-vev-jwt-vev-1.2"},
        {"expiresIn", 3600},
        {"note",      "real Google OAuth + UUID API-token paths land in vev-1.3"},
    };
}

static json handleGetCharacter(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();

    // Joined query: chrCharacters + race + bloodline + system + region + station.
    // LEFT JOIN staStations because stationID=0 (in space) → no station row;
    // we still want the rest of the data back.
    // LEFT JOIN entity (shipE) on chrCharacters.shipID → the ACTIVE ship the
    // character is piloting: its typeID is the hull (drives the 2D sprite +
    // ship HUD client-side), itemName is the ship's name. NULL when shipID is
    // unset (→ 0 / "" out, client falls back to the racial rookie hull).
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT c.characterID, c.characterName, c.raceID, r.raceName, "
        "       c.bloodlineID, b.bloodlineName, c.gender, c.balance, "
        "       c.solarSystemID, sys.solarSystemName, sys.security, "
        "       reg.regionID, reg.regionName, "
        "       c.stationID, st.stationName, st.stationTypeID, "   // VEV_STATIONTYPE
        "       c.corporationID, c.skillPoints, c.online, "
        "       c.shipID, shipE.typeID, shipE.itemName, shipT.typeName "
        "FROM chrCharacters c "
        "LEFT JOIN chrRaces r ON c.raceID = r.raceID "
        "LEFT JOIN chrBloodlines b ON c.bloodlineID = b.bloodlineID "
        "LEFT JOIN mapSolarSystems sys ON c.solarSystemID = sys.solarSystemID "
        "LEFT JOIN mapRegions reg ON sys.regionID = reg.regionID "
        "LEFT JOIN staStations st ON c.stationID = st.stationID "
        "LEFT JOIN entity shipE ON c.shipID = shipE.itemID "
        "LEFT JOIN invTypes shipT ON shipE.typeID = shipT.typeID "   // VEV_SHIPTYPENAME
        "WHERE c.characterID = %u",
        characterID))
    {
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    }

    if (res.GetRowCount() == 0) {
        throw std::runtime_error("character not found");
    }

    DBResultRow row;
    res.GetRow(row);

    auto textOrEmpty = [&row](uint32_t i) -> std::string {
        const char* t = row.GetText(i);
        return t ? std::string(t) : "";
    };
    // VEV_NULLSAFE_GETCHAR (2026-06-12, belt-ratting track): a destroyed hull
    // leaves c.shipID dangling -> the shipE LEFT JOIN yields NULL columns ->
    // GetUInt(19) ran strtoul(NULL) and SEGFAULTED the server on the next
    // client poll of the dead pilot (gdb-confirmed: Gateway.cpp:230 via
    // dbcore.cpp:670 — the "rat kill crashes the server" class). Same guard
    // handleGetShipFitting already uses.
    auto uintOr0 = [&row](uint32_t i) -> uint32_t {
        return row.IsNull(i) ? 0u : row.GetUInt(i);
    };

    return json{
        {"characterID",     row.GetUInt(0)},
        {"name",            textOrEmpty(1)},
        {"raceID",          row.GetUInt(2)},
        {"raceName",        textOrEmpty(3)},
        {"bloodlineID",     row.GetUInt(4)},
        {"bloodlineName",   textOrEmpty(5)},
        {"gender",          row.GetInt(6) == 0 ? "Male" : "Female"},
        {"isk",             row.GetDouble(7)},
        {"solarSystemID",   row.GetUInt(8)},
        {"solarSystemName", textOrEmpty(9)},
        {"security",        row.IsNull(10) ? 0.0f : row.GetFloat(10)},
        {"regionID",        uintOr0(11)},
        {"regionName",      textOrEmpty(12)},
        {"stationID",       row.GetUInt(13)},
        {"stationName",     textOrEmpty(14)},
        // VEV_STATIONTYPE (2026-06-12, fleet-theater): hangar interior art keys
        // on the STATION's type — surface it so clients skip a second lookup.
        {"stationTypeID",   uintOr0(15)},
        {"corporationID",   row.GetUInt(16)},
        {"skillPoints",     row.GetInt64(17)},
        {"online",          row.GetInt(18) != 0},
        {"shipID",          uintOr0(19)},
        {"shipTypeID",      uintOr0(20)},
        {"shipName",        textOrEmpty(21)},
        // VEV_SHIPTYPENAME (2026-06-12): the TYPE name ("Rifter") — clients must
        // never slug the ITEM name ("Jayne Cobb's Rifter") into the sprite library.
        {"shipTypeName",    textOrEmpty(22)},
    };
}

// ─── AI command-queue injection ────────────────────────────────────────────
// World-mutating ops run through evemu's authoritative main loop (so the real
// in-space ball + station/Local presence side effects fire), NOT direct DB
// flips. INSERT a 'pending' row; EntityList::ProcessAICommandQueue drains it.
static bool enqueueAICommand(uint32_t charID, const std::string& command,
                             const std::string& params) {
    std::string escCmd, escParams;
    sDatabase.DoEscapeString(escCmd, command);
    sDatabase.DoEscapeString(escParams, params);
    DBerror err;
    if (!sDatabase.RunQuery(err,
        "INSERT INTO ai_command_queue (charID, command, params, status) "
        "VALUES (%u, '%s', '%s', 'pending')",
        charID, escCmd.c_str(), escParams.c_str()))
    {
        std::cerr << "[vev-gateway] enqueueAICommand(" << command
                  << ") failed: " << err.c_str() << std::endl;
        return false;
    }
    return true;
}

// ── VEV_XPL_SCAN: human 2D-client cosmic-signature scanning ──────────────────
// The human reuses the SAME EntityList scan verbs the AI pilots run
// (get_signatures / warp_to_signature / analyze), bridged via ai_command_queue.
// get_signatures + analyze need the RESULT back, so we enqueue with RunQueryLID to
// capture the row id and poll it to a terminal status (the eve-server loop drains the
// queue on the game thread). warp_to_signature is fire-and-forget like undock. The
// client parses the raw result_msg (same format the AI scan FSM regexes).
static uint32_t enqueueAICommandLID(uint32_t charID, const std::string& command,
                                    const std::string& params) {
    std::string escCmd, escParams;
    sDatabase.DoEscapeString(escCmd, command);
    sDatabase.DoEscapeString(escParams, params);
    DBerror err; uint32_t rowID = 0;
    if (!sDatabase.RunQueryLID(err, rowID,
        "INSERT INTO ai_command_queue (charID, command, params, status) "
        "VALUES (%u, '%s', '%s', 'pending')",
        charID, escCmd.c_str(), escParams.c_str())) {
        std::cerr << "[vev-gateway] enqueueAICommandLID(" << command
                  << ") failed: " << err.c_str() << std::endl;
        return 0;
    }
    return rowID;
}

// Poll an ai_command_queue row to a terminal status; returns result_msg (empty on
// timeout). Blocks the WS handler thread up to ~timeoutMs -- fine for a user click.
static std::string pollAIResult(uint32_t rowID, int timeoutMs, bool* okOut = nullptr) {
    if (okOut) *okOut = false;
    if (rowID == 0) return "";
    const int stepMs = 75;
    for (int waited = 0; waited <= timeoutMs; waited += stepMs) {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT status, result_msg FROM ai_command_queue WHERE id = %u", rowID)) {
            DBResultRow row;
            if (res.GetRow(row)) {
                std::string st = row.GetText(0) ? std::string(row.GetText(0)) : "";
                if (st == "done" || st == "failed") {
                    if (okOut) *okOut = (st == "done");
                    return row.GetText(1) ? std::string(row.GetText(1)) : "";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    return "";
}

// getSignatures — relic/data cosmic signatures in the player's current system. Works
// docked or in space (the verb reads chrCharacters.solarSystemID when no ship ball).
static json handleGetSignatures(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID"))
        throw std::runtime_error("payload requires { characterID }");
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t rid = enqueueAICommandLID(characterID, "get_signatures", "{}");
    bool ok = false;
    std::string raw = pollAIResult(rid, 4000, &ok);
    return json{{"ok", ok}, {"raw", raw}};
}

// warpToSignature — undock (idempotent) then warp the player's ship to a sig beacon.
// Async like undock: the ball moves on subsequent grid:snapshots. params {characterID,
// sigID, distance?}.
static json handleWarpToSignature(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("sigID"))
        throw std::runtime_error("payload requires { characterID, sigID }");
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    std::string sigID = payload.at("sigID").get<std::string>();
    enqueueAICommand(characterID, "login_docked", "{}");
    enqueueAICommand(characterID, "undock", "{}");
    json wp = json{{"sigID", sigID}};
    if (payload.contains("distance")) wp["distance"] = payload.at("distance");
    const bool ok = enqueueAICommandLID(characterID, "warp_to_signature", wp.dump()) != 0;
    return json{{"ok", ok}, {"queued", ok}};
}

// analyze -- run ONE Crucible analyzer cycle for the human. Resolve the player's fitted
// Codebreaker/Analyzer (Data Miners, groupID 538) and run the SAME skill-based % roll the
// AI pilots get -- NO force, NO minigame (pre-Odyssey hacking was a passive module cycle:
// a % chance every ~5s to crack the can). A winning roll drops real loot + consumes the
// site; a miss returns "cycle again" and the client re-cycles every 5s.
static json handleAnalyze(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("sigID"))
        throw std::runtime_error("payload requires { characterID, sigID }");
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    std::string sigID = payload.at("sigID").get<std::string>();
    uint32_t moduleID = 0;
    DBQueryResult mres;
    if (sDatabase.RunQuery(mres,
        "SELECT e.itemID FROM entity e JOIN invTypes t ON t.typeID = e.typeID "
        "WHERE e.ownerID = %u AND t.groupID = 538 AND e.flag BETWEEN 11 AND 34 LIMIT 1",
        characterID)) {
        DBResultRow mrow; if (mres.GetRow(mrow)) moduleID = mrow.GetUInt(0);
    }
    if (moduleID == 0)
        return json{{"ok", false}, {"raw", "no Data/Relic Analyzer fitted"}};
    json ap = json{{"sigID", sigID}, {"moduleID", moduleID}};
    if (payload.contains("containerID")) ap["containerID"] = payload.at("containerID").get<uint32_t>();  // VEV_XPL_PER_CAN
    uint32_t rid = enqueueAICommandLID(characterID, "analyze", ap.dump());
    bool ok = false;
    std::string raw = pollAIResult(rid, 5000, &ok);
    return json{{"ok", ok}, {"raw", raw}};
}

static json handleUndock(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    // Real undock via the queue: login_docked (presence) then undock (spawns
    // the AIShipSE ball, in-plane egress). Idempotent server-side. The ball
    // arrives on the next grid:snapshot, not in this reply.
    enqueueAICommand(characterID, "login_docked", "{}");
    enqueueAICommand(characterID, "undock", "{}");
    json result = handleGetCharacter(json{{"characterID", characterID}});
    result["undockQueued"] = true;
    return result;
}

static json handleDock(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("stationID")) {
        throw std::runtime_error("payload requires { characterID, stationID }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t stationID   = payload.at("stationID").get<uint32_t>();

    // Verify the station exists + is in the same solar system the character
    // is currently in. Don't allow random teleport-docking to anywhere.
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT st.solarSystemID, c.solarSystemID "
        "FROM staStations st, chrCharacters c "
        "WHERE st.stationID = %u AND c.characterID = %u",
        stationID, characterID))
    {
        throw std::runtime_error(std::string("dock check failed: ") + res.error.c_str());
    }
    if (res.GetRowCount() == 0) {
        throw std::runtime_error("station or character not found");
    }
    DBResultRow row;
    res.GetRow(row);
    if (row.GetUInt(0) != row.GetUInt(1)) {
        throw std::runtime_error("station is not in the character's current solar system");
    }

    // Real dock via the queue (despawns the ball, ship to hangar, re-adds the
    // station guest). NOT a DB flip.
    enqueueAICommand(characterID, "dock", json{{"stationID", stationID}}.dump());
    json result = handleGetCharacter(json{{"characterID", characterID}});
    result["dockQueued"] = true;
    return result;
}

static json handleListStationsInSystem(const json& payload) {
    if (!payload.is_object() || !payload.contains("solarSystemID")) {
        throw std::runtime_error("payload requires { solarSystemID: number }");
    }
    uint32_t solarSystemID = payload.at("solarSystemID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT stationID, stationName, corporationID, stationTypeID, x, y, z "
        "FROM staStations WHERE solarSystemID = %u ORDER BY stationName",
        solarSystemID))
    {
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    }
    json stations = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        stations.push_back(json{
            {"stationID",     row.GetUInt(0)},
            {"stationName",   row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"corporationID", row.GetUInt(2)},
            {"stationTypeID", row.GetUInt(3)},
            {"x", row.GetDouble(4)},
            {"y", row.GetDouble(5)},
            {"z", row.GetDouble(6)},
        });
    }
    return json{{"stations", stations}};
}

// Station guest list = every pilot currently online AND docked at this
// station (real 3D clients + 2D phantoms both set stationID + online). Lets
// the docked 2D client show who's in the station with them. params: { stationID }
static json handleGetStationGuests(const json& payload) {
    if (!payload.is_object() || !payload.contains("stationID")) {
        throw std::runtime_error("payload requires { stationID }");
    }
    uint32_t stationID = payload.at("stationID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT characterID, characterName, raceID FROM chrCharacters "
        "WHERE stationID = %u AND online = 1 ORDER BY characterName", stationID))
    {
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    }
    json guests = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        guests.push_back(json{
            {"characterID", row.GetUInt(0)},
            {"name", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"raceID", row.GetUInt(2)},
        });
    }
    return json{{"guests", guests}};
}

// Local-channel roster = every pilot online in this solar system (docked OR in
// space; real 3D clients + 2D phantoms). EVE's Local is per-system. Lets the
// 2D client's Local panel show the real system membership (incl. the 3D
// client), not just whoever's on the legacy socket. params: { solarSystemID }
static json handleGetLocalMembers(const json& payload) {
    if (!payload.is_object() || !payload.contains("solarSystemID")) {
        throw std::runtime_error("payload requires { solarSystemID }");
    }
    uint32_t solarSystemID = payload.at("solarSystemID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT characterID, characterName, raceID FROM chrCharacters "
        "WHERE solarSystemID = %u AND online = 1 ORDER BY characterName", solarSystemID))
    {
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    }
    json members = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        members.push_back(json{
            {"characterID", row.GetUInt(0)},
            {"name", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"raceID", row.GetUInt(2)},
        });
    }
    return json{{"members", members}};
}

// Agent Finder roster = every agent stationed here (agtAgents.locationID),
// with name (chrNPCCharacters) + division label (crpNPCDivisions). The 2D
// client renders a real CDN portrait per agent id. params: { stationID }
static json handleGetStationAgents(const json& payload) {
    if (!payload.is_object() || !payload.contains("stationID")) {
        throw std::runtime_error("payload requires { stationID }");
    }
    uint32_t stationID = payload.at("stationID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT a.agentID, c.characterName, a.level, d.divisionName "
        "FROM agtAgents a "
        "JOIN chrNPCCharacters c ON c.characterID = a.agentID "
        "LEFT JOIN crpNPCDivisions d ON d.divisionID = a.divisionID "
        "WHERE a.locationID = %u "
        "ORDER BY a.level DESC, c.characterName", stationID))
    {
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    }
    json agents = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        agents.push_back(json{
            {"id", row.GetUInt(0)},
            {"name", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"level", row.GetUInt(2)},
            {"division", row.GetText(3) ? std::string(row.GetText(3)) : ""},
        });
    }
    return json{{"agents", agents}};
}

/** getSystemSnapshot — everything visible in a solar system, projected to 2D.
 *  Per VEV_ROADMAP §7: Vev is 2D throughout; we drop the Y axis from evemu's
 *  3D positions and use X/Z as the 2D plane.
 *
 *  Sources:
 *    - mapDenormalize : sun, planets, moons, asteroid belts, stargates
 *      (group IDs: sun=6, planet=7, moon=8, asteroid_belt=9, stargate=10)
 *    - staStations    : dockable stations (separate table from mapDenormalize)
 *    - itemName parse : stargate destination system is encoded as
 *      "Stargate (DEST_NAME)" — we extract DEST_NAME and look up its
 *      solarSystemID for the navigation overlay.
 */
static json handleGetSystemSnapshot(const json& payload) {
    if (!payload.is_object() || !payload.contains("solarSystemID")) {
        throw std::runtime_error("payload requires { solarSystemID: number }");
    }
    uint32_t solarSystemID = payload.at("solarSystemID").get<uint32_t>();

    // System metadata (name, region, security).
    DBQueryResult metaRes;
    if (!sDatabase.RunQuery(metaRes,
        "SELECT s.solarSystemName, s.security, s.regionID, r.regionName "
        "FROM mapSolarSystems s LEFT JOIN mapRegions r ON s.regionID = r.regionID "
        "WHERE s.solarSystemID = %u", solarSystemID))
    {
        throw std::runtime_error(std::string("DB error (meta): ") + metaRes.error.c_str());
    }
    if (metaRes.GetRowCount() == 0) {
        throw std::runtime_error("solar system not found");
    }
    DBResultRow mrow;
    metaRes.GetRow(mrow);
    json meta = json{
        {"solarSystemID",   solarSystemID},
        {"solarSystemName", mrow.GetText(0) ? std::string(mrow.GetText(0)) : ""},
        {"security",        mrow.GetFloat(1)},
        {"regionID",        mrow.GetUInt(2)},
        {"regionName",      mrow.GetText(3) ? std::string(mrow.GetText(3)) : ""},
    };

    // Celestials from mapDenormalize. We drop the Y coordinate (3D→2D
    // projection per the Vev architecture). t.groupID gives us the
    // celestial category for client-side rendering choice.
    DBQueryResult celRes;
    if (!sDatabase.RunQuery(celRes,
        "SELECT d.itemID, d.itemName, d.typeID, t.groupID, d.x, d.z, d.radius "
        "FROM mapDenormalize d "
        "LEFT JOIN invTypes t ON d.typeID = t.typeID "
        "WHERE d.solarSystemID = %u "
        "ORDER BY t.groupID, d.itemID",
        solarSystemID))
    {
        throw std::runtime_error(std::string("DB error (celestials): ") + celRes.error.c_str());
    }

    json sun       = json(nullptr);
    json planets   = json::array();
    json moons     = json::array();
    json belts     = json::array();
    json stargates = json::array();

    DBResultRow row;
    while (celRes.GetRow(row)) {
        const uint32_t groupID = row.GetUInt(3);
        json item = json{
            {"itemID",   row.GetUInt(0)},
            {"itemName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"typeID",   row.GetUInt(2)},
            {"x",        row.GetDouble(4)},
            {"z",        row.GetDouble(5)},
            {"radius",   row.GetDouble(6)},
        };
        switch (groupID) {
            case 6:  sun = item; break;          // Sun
            case 7:  planets.push_back(item); break;
            case 8:  moons.push_back(item); break;
            case 9:  belts.push_back(item); break;
            case 10: stargates.push_back(item); break;
            default: break; // other types ignored for v1
        }
    }

    // Stations — separate table.
    DBQueryResult stRes;
    if (!sDatabase.RunQuery(stRes,
        "SELECT stationID, stationName, stationTypeID, corporationID, x, z "
        "FROM staStations WHERE solarSystemID = %u ORDER BY stationName",
        solarSystemID))
    {
        throw std::runtime_error(std::string("DB error (stations): ") + stRes.error.c_str());
    }
    json stations = json::array();
    while (stRes.GetRow(row)) {
        stations.push_back(json{
            {"stationID",     row.GetUInt(0)},
            {"stationName",   row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"stationTypeID", row.GetUInt(2)},
            {"corporationID", row.GetUInt(3)},
            {"x",             row.GetDouble(4)},
            {"z",             row.GetDouble(5)},
        });
    }

    // Stargate destinations — mapSolarSystemJumps gives connected systems.
    // We also list each destination's name + security so the client can
    // label stargate icons with where they go.
    DBQueryResult jmpRes;
    if (!sDatabase.RunQuery(jmpRes,
        "SELECT j.toSolarSystemID, s.solarSystemName, s.security, s.regionID, r.regionName "
        "FROM mapSolarSystemJumps j "
        "LEFT JOIN mapSolarSystems s ON j.toSolarSystemID = s.solarSystemID "
        "LEFT JOIN mapRegions r ON s.regionID = r.regionID "
        "WHERE j.fromSolarSystemID = %u",
        solarSystemID))
    {
        throw std::runtime_error(std::string("DB error (jumps): ") + jmpRes.error.c_str());
    }
    json jumps = json::array();
    while (jmpRes.GetRow(row)) {
        jumps.push_back(json{
            {"toSolarSystemID",   row.GetUInt(0)},
            {"toSolarSystemName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"toSecurity",        row.GetFloat(2)},
            {"toRegionID",        row.GetUInt(3)},
            {"toRegionName",      row.GetText(4) ? std::string(row.GetText(4)) : ""},
        });
    }

    return json{
        {"meta",      meta},
        {"sun",       sun},
        {"planets",   planets},
        {"moons",     moons},
        {"belts",     belts},
        {"stargates", stargates},
        {"stations",  stations},
        {"jumps",     jumps},
    };
}

/** getRegionMap — every solar system in a region + the jump connectivity
 *  graph. Returned as two arrays (systems + edges) so the client can lay
 *  it out (force-directed or just use the EVE coordinates). */
static json handleGetRegionMap(const json& payload) {
    if (!payload.is_object() || !payload.contains("regionID")) {
        throw std::runtime_error("payload requires { regionID: number }");
    }
    uint32_t regionID = payload.at("regionID").get<uint32_t>();

    DBQueryResult sysRes;
    if (!sDatabase.RunQuery(sysRes,
        "SELECT solarSystemID, solarSystemName, x, z, security, constellationID "
        "FROM mapSolarSystems WHERE regionID = %u",
        regionID))
    {
        throw std::runtime_error(std::string("DB error (systems): ") + sysRes.error.c_str());
    }
    json systems = json::array();
    DBResultRow row;
    while (sysRes.GetRow(row)) {
        systems.push_back(json{
            {"solarSystemID",   row.GetUInt(0)},
            {"solarSystemName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"x",               row.GetDouble(2)},
            {"z",               row.GetDouble(3)},
            {"security",        row.GetFloat(4)},
            {"constellationID", row.GetUInt(5)},
        });
    }

    // Edges within the region. We include cross-region jumps too so the
    // client can render gates pointing out of the region. fromRegionID
    // = our region; toRegionID may differ.
    DBQueryResult edgRes;
    if (!sDatabase.RunQuery(edgRes,
        "SELECT fromSolarSystemID, toSolarSystemID, toRegionID "
        "FROM mapSolarSystemJumps WHERE fromRegionID = %u",
        regionID))
    {
        throw std::runtime_error(std::string("DB error (edges): ") + edgRes.error.c_str());
    }
    json edges = json::array();
    while (edgRes.GetRow(row)) {
        edges.push_back(json{
            {"from", row.GetUInt(0)},
            {"to",   row.GetUInt(1)},
            {"toRegionID", row.GetUInt(2)},
        });
    }

    // Region metadata.
    DBQueryResult metaRes;
    if (!sDatabase.RunQuery(metaRes,
        "SELECT regionName FROM mapRegions WHERE regionID = %u", regionID))
    {
        throw std::runtime_error(std::string("DB error (region meta): ") + metaRes.error.c_str());
    }
    std::string regionName;
    if (metaRes.GetRowCount() > 0) {
        DBResultRow rrow;
        metaRes.GetRow(rrow);
        regionName = rrow.GetText(0) ? rrow.GetText(0) : "";
    }

    return json{
        {"regionID",   regionID},
        {"regionName", regionName},
        {"systems",    systems},
        {"edges",      edges},
    };
}

/** listAllRegions — for the star-map / region picker. */
static json handleListAllRegions(const json& payload) {
    (void)payload;
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT regionID, regionName, x, z FROM mapRegions ORDER BY regionName"))
    {
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    }
    json regions = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        regions.push_back(json{
            {"regionID",   row.GetUInt(0)},
            {"regionName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"x",          row.GetDouble(2)},
            {"z",          row.GetDouble(3)},
        });
    }
    return json{{"regions", regions}};
}

/** getUniverseMap — the WHOLE cluster: every solar system + every jump +
 *  region centroids for labels. ~7929 systems / ~14334 jumps. Rendered as
 *  a batched point cloud (not per-system widgets). Payload uses COMPACT
 *  ARRAYS to stay well under 1 MB and parse fast:
 *    systems[i] = [solarSystemID, name, x, z, security, regionID]
 *    edges[i]   = [fromSystemID, toSystemID]
 *  Names ARE included (for hover tooltips + click selection). Compact-array
 *  form keeps the k-space payload (~5431 systems) under 1 MB. Region labels
 *  come from the separate regions array. */
static json handleGetUniverseMap(const json& payload) {
    (void)payload;

    // K-space only (regionID < 11000000). W-space (J-systems) + abyssal
    // sit at extreme coordinates with no static gate connections — they'd
    // wreck the auto-fit and aren't on EVE's star map anyway. This leaves
    // the ~5431-system connected cluster.
    DBQueryResult sysRes;
    if (!sDatabase.RunQuery(sysRes,
        "SELECT solarSystemID, solarSystemName, x, z, security, regionID FROM mapSolarSystems "
        "WHERE regionID < 11000000"))
    {
        throw std::runtime_error(std::string("DB error (systems): ") + sysRes.error.c_str());
    }
    json systems = json::array();
    DBResultRow row;
    while (sysRes.GetRow(row)) {
        systems.push_back(json::array({
            row.GetUInt(0),
            row.GetText(1) ? std::string(row.GetText(1)) : "",
            row.GetDouble(2), row.GetDouble(3),
            row.GetFloat(4), row.GetUInt(5),
        }));
    }

    DBQueryResult edgRes;
    if (!sDatabase.RunQuery(edgRes,
        "SELECT fromSolarSystemID, toSolarSystemID FROM mapSolarSystemJumps"))
    {
        throw std::runtime_error(std::string("DB error (jumps): ") + edgRes.error.c_str());
    }
    json edges = json::array();
    while (edgRes.GetRow(row)) {
        edges.push_back(json::array({ row.GetUInt(0), row.GetUInt(1) }));
    }

    // Region centroids — for region labels at universe zoom. K-space only.
    DBQueryResult regRes;
    if (!sDatabase.RunQuery(regRes,
        "SELECT regionID, regionName, x, z FROM mapRegions WHERE regionID < 11000000"))
    {
        throw std::runtime_error(std::string("DB error (regions): ") + regRes.error.c_str());
    }
    json regions = json::array();
    while (regRes.GetRow(row)) {
        regions.push_back(json{
            {"regionID",   row.GetUInt(0)},
            {"regionName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"x",          row.GetDouble(2)},
            {"z",          row.GetDouble(3)},
        });
    }

    return json{
        {"systems", systems},
        {"edges",   edges},
        {"regions", regions},
    };
}

static json handleListCharacters(const json& payload) {
    // List all characters (Vev-1.2 stub — vev-1.3 filters by authed accountID
    // once the auth middleware lands). Returns the short shape for the
    // character-selection screen.
    (void)payload;
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT c.characterID, c.characterName, c.raceID, r.raceName, "
        "       c.bloodlineID, b.bloodlineName, c.solarSystemID, "
        "       c.logonDateTime "
        "FROM chrCharacters c "
        "LEFT JOIN chrRaces r ON c.raceID = r.raceID "
        "LEFT JOIN chrBloodlines b ON c.bloodlineID = b.bloodlineID "
        "ORDER BY c.logonDateTime DESC "
        "LIMIT 50"))
    {
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    }

    json characters = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        characters.push_back(json{
            {"characterID",   row.GetUInt(0)},
            {"name",          row.GetText(1)},
            {"raceID",        row.GetUInt(2)},
            {"raceName",      row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"bloodlineID",   row.GetUInt(4)},
            {"bloodlineName", row.GetText(5) ? std::string(row.GetText(5)) : ""},
            {"solarSystemID", row.GetUInt(6)},
            {"logonDateTime", row.GetInt64(7)},
        });
    }
    return json{{"characters", characters}};
}

// Rookie ship typeID per race — evemu EVE_Character.h Rookie::Ship.
//   Amarr → Impairor 596 · Caldari → Ibis 601 · Gallente → Velator 606
//   Minmatar → Reaper 588. Velator is evemu's own arbitrary default.
static uint32_t rookieShipForRace(uint32_t raceID) {
    switch (raceID) {
        case 4: return 596;  // Amarr
        case 1: return 601;  // Caldari
        case 8: return 606;  // Gallente
        case 2: return 588;  // Minmatar
        default: return 606;
    }
}

// EVE skill-point cost at a level: ceil(sqrt(32)^(level-1) * 250 * rank).
// Byte-for-byte mirror of EvEMath::Skill::PointsAtLevel
// (eve-common/utils/EvEMath.cpp) so the gateway's SQL seeding produces the
// same SP totals the eve-server would. `rank` is the skill's training-time
// constant (AttrSkillTimeConstant, attribute 275).
static uint32_t skillPointsForLevel(uint8_t level, double rank) {
    if (level > 5) level = 5;
    if (level < 1) return 0;
    double sp = std::pow(std::sqrt(32.0), static_cast<double>(level - 1)) * 250.0 * rank;
    return static_cast<uint32_t>(std::ceil(sp));
}

// Seed a freshly-created character with EVE's starter skill book + a racial
// rookie ship, then write the resulting skillPoints + shipID onto the
// chrCharacters row. This replicates — at the SQL level — what eve-server's
// CharUnboundMgrService::CreateCharacter does through the ItemFactory
// (CharacterDB::GetBaseSkills + GetSkillsByRace → SpawnSkill, and
// Client::SpawnNewRookieShip). The gateway links only the DB layer (no
// ItemFactory/Client), and Vev never runs the real EVE client, so writing
// `entity` + `entity_attributes` rows directly is both necessary and
// sufficient — future gateway reads (getCharacterAssets / getShipFitting)
// pick these rows up through the same SQL path.
//
// Item storage (eve-server/inventory/ItemDB.cpp + AttributeEnum.h + EVE_Flags.h):
//   skill = entity row, flag 7 (flagSkill), owner+location = charID,
//           + entity_attributes 280 (SkillLevel) & 276 (SkillPoints)
//   ship  = entity row, flag 4 (flagHangar), location = stationID, singleton
//   rank  = dgmTypeAttributes value where attributeID = 275 (SkillTimeConstant)
//
// NOT yet replicated (follow-ups): ancestry-derived character attributes
// (chrCharacterAttributes) and the rookie ship's fitted civilian
// miner/turret. Skills + hull are the parts the curator flagged.
static void seedStarterSkillsAndShip(uint32_t characterID, uint32_t raceID,
                                     uint32_t stationID, const std::string& charName) {
    DBQueryResult res;
    DBResultRow row;
    DBerror err;

    // 1. Merge base + race skill templates, capping each at level 5 —
    //    mirrors CharacterDB::GetBaseSkills + GetSkillsByRace.
    std::map<uint32_t, uint8_t> skills;
    if (sDatabase.RunQuery(res, "SELECT skillTypeID, level FROM sklBaseSkills")) {
        while (res.GetRow(row)) skills[row.GetUInt(0)] = static_cast<uint8_t>(row.GetUInt(1));
    }
    if (sDatabase.RunQuery(res, "SELECT skillTypeID, level FROM sklRaceSkills WHERE raceID = %u", raceID)) {
        while (res.GetRow(row)) {
            uint32_t typeID = row.GetUInt(0);
            uint8_t lvl = static_cast<uint8_t>(row.GetUInt(1));
            auto it = skills.find(typeID);
            if (it == skills.end()) {
                skills[typeID] = lvl;
            } else {
                uint8_t merged = it->second + lvl;
                skills[typeID] = merged > 5 ? 5 : merged;
            }
        }
    }

    // 2. Spawn each skill as an entity item + its level/SP attributes.
    uint64_t totalSP = 0;
    for (const auto& kv : skills) {
        const uint32_t skillTypeID = kv.first;
        const uint8_t level = kv.second;

        // Skill rank (training-time constant); default 1 if the attribute
        // row is missing or null.
        double rank = 1.0;
        if (sDatabase.RunQuery(res,
            "SELECT valueFloat, valueInt FROM dgmTypeAttributes "
            "WHERE typeID = %u AND attributeID = 275", skillTypeID) && res.GetRow(row)) {
            if (!row.IsNull(0))      rank = row.GetDouble(0);
            else if (!row.IsNull(1)) rank = static_cast<double>(row.GetInt(1));
        }
        const uint32_t sp = skillPointsForLevel(level, rank);

        uint32_t skillItemID = 0;
        if (!sDatabase.RunQueryLID(err, skillItemID,
            "INSERT INTO entity "
            "(itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
            "VALUES ('', %u, %u, %u, 7, 0, 0, 1, 0, 0, 0, '')",
            skillTypeID, characterID, characterID)) {
            // Don't abort the whole creation over one skill — log + skip.
            std::cerr << "[gateway] seed skill " << skillTypeID << " failed: " << err.c_str() << std::endl;
            continue;
        }
        sDatabase.RunQuery(err,
            "INSERT INTO entity_attributes (itemID, attributeID, valueInt, valueFloat) "
            "VALUES (%u, 280, %u, NULL), (%u, 276, %u, NULL)",
            skillItemID, static_cast<uint32_t>(level), skillItemID, sp);
        totalSP += sp;
    }

    // 3. Racial rookie ship, pre-assembled, into the station hangar.
    const uint32_t shipTypeID = rookieShipForRace(raceID);
    std::string shipNameEsc;
    sDatabase.DoEscapeString(shipNameEsc, charName + "'s Rookie Ship");
    uint32_t shipItemID = 0;
    sDatabase.RunQueryLID(err, shipItemID,
        "INSERT INTO entity "
        "(itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
        "VALUES ('%s', %u, %u, %u, 4, 0, 1, 1, 0, 0, 0, '')",
        shipNameEsc.c_str(), shipTypeID, characterID, stationID);

    // 4. Persist the totals onto the character.
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET skillPoints = %llu, shipID = %u WHERE characterID = %u",
        static_cast<unsigned long long>(totalSP), shipItemID, characterID);
}

static json handleCreateCharacter(const json& payload) {
    if (!payload.is_object()) {
        throw std::runtime_error("payload must be an object");
    }
    // Required fields from the Vev browser CharacterCreation screen.
    const std::string name      = payload.value("name", "");
    const std::string raceStr   = payload.value("race", "");
    const std::string bloodStr  = payload.value("bloodline", "");
    const std::string genderStr = payload.value("gender", "male");

    if (name.empty() || name.size() < 2 || name.size() > 30) {
        throw std::runtime_error("name must be 2-30 characters");
    }
    uint32_t raceID = raceIdForString(raceStr);
    if (raceID == 0) {
        throw std::runtime_error("race must be one of: amarr, caldari, gallente, minmatar");
    }
    uint32_t bloodlineID = bloodlineIdForString(bloodStr);
    if (bloodlineID == 0) {
        throw std::runtime_error("bloodline missing or unknown — see chrBloodlines for valid ids");
    }
    const int gender = (genderStr == "female") ? 1 : 0;

    // Escape name to defeat single-quote injection. DBcore's helper writes
    // the escaped form into the output string.
    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, name);

    const uint32_t solarSystemID = spawnSolarSystemForRace(raceID);
    const uint32_t stationID     = spawnStationForRace(raceID);

    // INSERT the character row; new characters start DOCKED at the race-native
    // starter station (Emperor Family Academy / Republic Fleet Logistic
    // Support / etc.). Schema defaults handle the corp-role columns.
    DBerror err;
    uint32 characterID = 0;
    if (!sDatabase.RunQueryLID(err, characterID,
        "INSERT INTO chrCharacters "
        "(characterName, raceID, bloodlineID, gender, balance, "
        " solarSystemID, stationID, createDateTime, startDateTime) "
        "VALUES "
        "('%s', %u, %u, %d, 5000000, %u, %u, UNIX_TIMESTAMP()*10000000+116444736000000000, UNIX_TIMESTAMP()*10000000+116444736000000000)",
        nameEsc.c_str(), raceID, bloodlineID, gender, solarSystemID, stationID))
    {
        throw std::runtime_error(std::string("DB insert failed: ") + err.c_str());
    }

    // Seed the EVE starter skill book + racial rookie ship (the "real
    // character creation pipeline" the original stub deferred). Writes
    // entity / entity_attributes rows + chrCharacters.skillPoints + .shipID.
    seedStarterSkillsAndShip(characterID, raceID, stationID, name);

    // Return the created character in the same shape getCharacter does so
    // the client can swap from "creating" → "loaded pilot" without a
    // round-trip.
    return handleGetCharacter(json{{"characterID", characterID}});
}

/** getIndustry — the Science & Industry window for one character:
 *    installations : assembly lines physically at the character's CURRENT
 *                    station (real evemu facility data; empty when undocked
 *                    since jobs install while docked). The client groups them
 *                    by activityID (1=Manufacturing, 3=TE research,
 *                    4=ME research, 5=Copy, 6=Invention, 7=Reverse Eng,
 *                    8=Reaction).
 *    jobs          : the character's active industry jobs (ramJobs).
 *    blueprints    : blueprints the character owns (invBlueprints + entity).
 *  Empty jobs/blueprints are expected for a fresh character — this stands up
 *  the data pipeline so the window lights up as soon as jobs are installed.
 *  Schema verified against eve-server/manufacturing/FactoryDB.cpp. */
static json handleGetIndustry(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t stationID = payload.contains("stationID") ? payload.at("stationID").get<uint32_t>() : 0;

    // Installations — assembly lines at the current station (real regardless
    // of the character). Empty when undocked: industry jobs install dockside.
    json installations = json::array();
    if (stationID != 0) {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT ral.assemblyLineID, ral.assemblyLineTypeID, alt.assemblyLineTypeName, "
            "       ral.activityID, ral.costInstall, ral.costPerHour, ral.nextFreeTime "
            "FROM ramAssemblyLines AS ral "
            "LEFT JOIN ramAssemblyLineTypes AS alt ON ral.assemblyLineTypeID = alt.assemblyLineTypeID "
            "WHERE ral.containerID = %u", stationID))
        {
            throw std::runtime_error(std::string("DB error (installations): ") + res.error.c_str());
        }
        DBResultRow row;
        while (res.GetRow(row)) {
            installations.push_back(json{
                {"assemblyLineID",     row.GetUInt(0)},
                {"assemblyLineTypeID", row.GetUInt(1)},
                {"typeName",           row.GetText(2) ? std::string(row.GetText(2)) : ""},
                {"activityID",         row.GetUInt(3)},
                {"costInstall",        row.GetDouble(4)},
                {"costPerHour",        row.GetDouble(5)},
                {"nextFreeTime",       row.GetInt64(6)},
            });
        }
    }

    // Active jobs owned by the character (completedStatusID = 0 = in progress).
    json jobs = json::array();
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT job.jobID, line.activityID, ii.typeID, t.typeName, job.runs, "
            "       job.installTime, job.endProductionTime, st.solarSystemID "
            "FROM ramJobs AS job "
            "LEFT JOIN ramAssemblyLines AS line ON job.assemblyLineID = line.assemblyLineID "
            "LEFT JOIN entity AS ii ON job.installedItemID = ii.itemID "
            "LEFT JOIN invTypes AS t ON ii.typeID = t.typeID "
            "LEFT JOIN ramAssemblyLineStations AS st ON line.containerID = st.stationID "
            "WHERE job.ownerID = %u AND job.completedStatusID = 0", characterID))
        {
            throw std::runtime_error(std::string("DB error (jobs): ") + res.error.c_str());
        }
        DBResultRow row;
        while (res.GetRow(row)) {
            jobs.push_back(json{
                {"jobID",             row.GetUInt(0)},
                {"activityID",        row.GetUInt(1)},
                {"typeID",            row.GetUInt(2)},
                {"typeName",          row.GetText(3) ? std::string(row.GetText(3)) : ""},
                {"runs",              row.GetInt(4)},
                {"installTime",       row.GetInt64(5)},
                {"endProductionTime", row.GetInt64(6)},
                {"solarSystemID",     row.GetUInt(7)},
            });
        }
    }

    // Blueprints the character owns (entity.ownerID = char), with type name +
    // ME/TE (mLevel/pLevel) + remaining runs + original/copy flag.
    json blueprints = json::array();
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT bp.itemID, e.typeID, t.typeName, bp.copy, bp.mLevel, bp.pLevel, bp.runs, "
            "       bt.productTypeID, pt.typeName AS productName "
            "FROM invBlueprints AS bp "
            "JOIN entity AS e ON bp.itemID = e.itemID "
            "LEFT JOIN invTypes AS t ON e.typeID = t.typeID "
            "LEFT JOIN invBlueprintTypes AS bt ON e.typeID = bt.blueprintTypeID "
            "LEFT JOIN invTypes AS pt ON bt.productTypeID = pt.typeID "
            "WHERE e.ownerID = %u", characterID))
        {
            throw std::runtime_error(std::string("DB error (blueprints): ") + res.error.c_str());
        }
        DBResultRow row;
        while (res.GetRow(row)) {
            blueprints.push_back(json{
                {"itemID",        row.GetUInt(0)},
                {"typeID",        row.GetUInt(1)},
                {"typeName",      row.GetText(2) ? std::string(row.GetText(2)) : ""},
                {"copy",          row.GetInt(3)},
                {"mLevel",        row.GetInt(4)},
                {"pLevel",        row.GetInt(5)},
                {"runs",          row.GetInt(6)},
                {"productTypeID", row.GetUInt(7)},
                {"productName",   row.GetText(8) ? std::string(row.GetText(8)) : ""},
            });
        }
    }

    return json{
        {"stationID",     stationID},
        {"installations", installations},
        {"jobs",          jobs},
        {"blueprints",    blueprints},
    };
}

/** getBlueprintDetail — the product + bill-of-materials for one blueprint type,
 *  for the Industry window's blueprint drill-down. Payload { blueprintTypeID }.
 *  Materials are the base manufacturing inputs (invTypeMaterials keyed by the
 *  PRODUCT typeID — same source as FactoryDB::GetOutpostMaterialCompositionOfItemType). */
static json handleGetBlueprintDetail(const json& payload) {
    if (!payload.is_object() || !payload.contains("blueprintTypeID")) {
        throw std::runtime_error("payload requires { blueprintTypeID: number }");
    }
    uint32_t bpTypeID = payload.at("blueprintTypeID").get<uint32_t>();

    // Resolve the product this blueprint manufactures.
    uint32_t productTypeID = 0;
    std::string productName;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT bt.productTypeID, t.typeName "
            "FROM invBlueprintTypes AS bt "
            "LEFT JOIN invTypes AS t ON bt.productTypeID = t.typeID "
            "WHERE bt.blueprintTypeID = %u", bpTypeID))
        {
            throw std::runtime_error(std::string("DB error (bp product): ") + res.error.c_str());
        }
        DBResultRow row;
        if (res.GetRow(row)) {
            productTypeID = row.GetUInt(0);
            productName = row.GetText(1) ? std::string(row.GetText(1)) : "";
        }
    }

    // Base material inputs for the product.
    json materials = json::array();
    if (productTypeID != 0) {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT m.materialTypeID, t.typeName, m.quantity "
            "FROM invTypeMaterials AS m "
            "LEFT JOIN invTypes AS t ON m.materialTypeID = t.typeID "
            "WHERE m.typeID = %u", productTypeID))
        {
            throw std::runtime_error(std::string("DB error (bp materials): ") + res.error.c_str());
        }
        DBResultRow row;
        while (res.GetRow(row)) {
            materials.push_back(json{
                {"typeID",   row.GetUInt(0)},
                {"typeName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"quantity", row.GetInt(2)},
            });
        }
    }

    return json{
        {"blueprintTypeID", bpTypeID},
        {"productTypeID",   productTypeID},
        {"productName",     productName},
        {"materials",       materials},
    };
}

// Active ship's fitted modules — entity items living INSIDE the ship
// (locationID = chrCharacters.shipID) at a slot flag. Slot + index are
// derived from the EVE inventory flag: LoSlot0-7 = 11-18, MedSlot0-7 = 19-26,
// HiSlot0-7 = 27-34, RigSlot0-7 = 92-99. Backs the Fitting window + the
// in-space ship-HUD slot row.
static json handleGetShipFitting(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();

    uint32_t shipID = 0, shipTypeID = 0;
    std::string shipName;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT c.shipID, e.typeID, e.itemName "
            "FROM chrCharacters c "
            "LEFT JOIN entity e ON c.shipID = e.itemID "
            "WHERE c.characterID = %u", characterID))
        {
            throw std::runtime_error(std::string("DB error (ship): ") + res.error.c_str());
        }
        DBResultRow row;
        if (res.GetRow(row)) {
            shipID     = row.GetUInt(0);
            shipTypeID = row.IsNull(1) ? 0 : row.GetUInt(1);
            shipName   = row.GetText(2) ? std::string(row.GetText(2)) : "";
        }
    }

    // Ship-level fitting capacities from the hull's dgmTypeAttributes:
    //   48 cpuOutput, 11 powerOutput, 1132 calibration, 482 capacitorCapacity,
    //   55 rechargeRate(ms). DB-authoritative (supersedes client hulls.ts).
    double shipCpu = 0, shipPg = 0, shipCalib = 0, shipCap = 0, shipRecharge = 0;
    if (shipTypeID != 0) {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT attributeID, COALESCE(valueFloat, valueInt) v "
            "FROM dgmTypeAttributes "
            "WHERE typeID = %u AND attributeID IN (48,11,1132,482,55)", shipTypeID))
        {
            DBResultRow row;
            while (res.GetRow(row)) {
                int attr = row.GetInt(0); double v = row.GetDouble(1);
                switch (attr) {
                    case 48:   shipCpu = v; break;
                    case 11:   shipPg = v; break;
                    case 1132: shipCalib = v; break;
                    case 482:  shipCap = v; break;
                    case 55:   shipRecharge = v; break;
                }
            }
        }
    }

    json modules = json::array();
    if (shipID != 0) {
        // Loaded charges are separate entities at the SAME slot flag (category
        // 8). Collect them keyed by flag so they nest under their module rather
        // than appear as phantom slot occupants (the "orphan crystal" bug).
        json chargeByFlag = json::object();
        {
            DBQueryResult cres;
            if (sDatabase.RunQuery(cres,
                "SELECT e.flag, e.itemID, e.typeID, t.typeName, e.quantity "
                "FROM entity e LEFT JOIN invTypes t ON e.typeID = t.typeID "
                "LEFT JOIN invGroups g ON t.groupID = g.groupID "
                "WHERE e.locationID = %u AND g.categoryID = 8 "
                "  AND ((e.flag BETWEEN 11 AND 34) OR (e.flag BETWEEN 92 AND 99))", shipID))
            {
                DBResultRow r;
                while (cres.GetRow(r)) {
                    chargeByFlag[std::to_string(r.GetInt(0))] = json{
                        {"itemID",   r.GetUInt(1)},
                        {"typeID",   r.GetUInt(2)},
                        {"typeName", r.GetText(3) ? std::string(r.GetText(3)) : ""},
                        {"quantity", r.GetUInt(4)},   // VEV_AMMO: live round count
                    };
                }
            }
        }

        DBQueryResult res;
        // Modules only (exclude charges) + online (attr 2; default 1) + load
        // pivot (50 cpu, 30 power, 6 capNeed, 73 duration ms, 1153 calibration).
        if (!sDatabase.RunQuery(res,
            "SELECT e.itemID, e.flag, e.typeID, t.typeName, g.groupName, "
            "       COALESCE(ea.valueInt, 1) AS online, "
            "       MAX(CASE WHEN da.attributeID=50   THEN COALESCE(da.valueFloat,da.valueInt) END) AS modcpu, "
            "       MAX(CASE WHEN da.attributeID=30   THEN COALESCE(da.valueFloat,da.valueInt) END) AS modpg, "
            "       MAX(CASE WHEN da.attributeID=6    THEN COALESCE(da.valueFloat,da.valueInt) END) AS capneed, "
            "       MAX(CASE WHEN da.attributeID=73   THEN COALESCE(da.valueFloat,da.valueInt) END) AS dur, "
            "       MAX(CASE WHEN da.attributeID=1153 THEN COALESCE(da.valueFloat,da.valueInt) END) AS calib, "
            "       MAX(CASE WHEN da.attributeID=54   THEN COALESCE(da.valueFloat,da.valueInt) END) AS optrange, "
            "       MAX(CASE WHEN da.attributeID=604  THEN COALESCE(da.valueFloat,da.valueInt) END) AS chargegrp, "
            "       MAX(CASE WHEN da.attributeID=128  THEN COALESCE(da.valueFloat,da.valueInt) END) AS chargesize "
            "FROM entity e "
            "LEFT JOIN invTypes t ON e.typeID = t.typeID "
            "LEFT JOIN invGroups g ON t.groupID = g.groupID "
            "LEFT JOIN entity_attributes ea ON ea.itemID = e.itemID AND ea.attributeID = 2 "
            "LEFT JOIN dgmTypeAttributes da ON da.typeID = e.typeID AND da.attributeID IN (50,30,6,73,1153,54,604,128) "
            "WHERE e.locationID = %u "
            "  AND ((e.flag BETWEEN 11 AND 34) OR (e.flag BETWEEN 92 AND 99)) "
            "  AND COALESCE(g.categoryID,7) <> 8 "
            "GROUP BY e.itemID, e.flag, e.typeID, t.typeName, g.groupName, online "
            "ORDER BY e.flag", shipID))
        {
            throw std::runtime_error(std::string("DB error (modules): ") + res.error.c_str());
        }
        DBResultRow row;
        while (res.GetRow(row)) {
            int flag = row.GetInt(1);
            const char* slot = "other"; int index = 0;
            if      (flag >= 27 && flag <= 34) { slot = "high"; index = flag - 27; }
            else if (flag >= 19 && flag <= 26) { slot = "mid";  index = flag - 19; }
            else if (flag >= 11 && flag <= 18) { slot = "low";  index = flag - 11; }
            else if (flag >= 92 && flag <= 99) { slot = "rig";  index = flag - 92; }
            json m = {
                {"itemID",    row.GetUInt(0)},
                {"flag",      flag},
                {"slot",      slot},
                {"index",     index},
                {"typeID",    row.GetUInt(2)},
                {"typeName",  row.GetText(3) ? std::string(row.GetText(3)) : ""},
                {"groupName", row.GetText(4) ? std::string(row.GetText(4)) : ""},
                {"online",    row.GetInt64(5) != 0},
            };
            if (!row.IsNull(6))  m["cpu"]         = row.GetDouble(6);
            if (!row.IsNull(7))  m["power"]       = row.GetDouble(7);
            if (!row.IsNull(8))  m["capNeed"]     = row.GetDouble(8);
            if (!row.IsNull(9))  m["duration"]    = row.GetDouble(9);
            if (!row.IsNull(10)) m["calibration"] = row.GetDouble(10);
            if (!row.IsNull(11)) m["optimalRange"] = row.GetDouble(11);
            // VEV_CHARGE_MATCH: which ammo GROUP fits this module (604) + the
            // size class it takes (128) — the Swap Ammo strict filter.
            if (!row.IsNull(12)) m["chargeGroup1"] = (uint32)row.GetDouble(12);
            if (!row.IsNull(13)) m["chargeSize"]   = (uint32)row.GetDouble(13);
            std::string fk = std::to_string(flag);
            if (chargeByFlag.contains(fk)) m["charge"] = chargeByFlag[fk];
            modules.push_back(m);
        }
    }


    // ── VEV_MODULE_LIVE: live ship attrs for the fitting window ───────────
    // The VEV_SIM_TOGGLE sim persists engaged-module attr changes to
    // entity_attributes; serve resist resonances + maxVelocity as overrides
    // over the hull's dgmTypeAttributes defaults so the client shows the
    // REAL current defense (DC/hardeners visibly move the numbers).
    json live = json::object();
    {
        std::map<uint16, double> vals;
        DBQueryResult lres;
        DBResultRow lrow;
        if (shipTypeID != 0 && sDatabase.RunQuery(lres,
            "SELECT attributeID, COALESCE(valueFloat, valueInt) FROM dgmTypeAttributes "
            "WHERE typeID = %u AND attributeID IN (37,4,271,272,273,274,267,268,269,270,113,111,109,110)",
            shipTypeID))
            while (lres.GetRow(lrow)) vals[(uint16)lrow.GetUInt(0)] = lrow.GetDouble(1);
        if (shipID != 0 && sDatabase.RunQuery(lres,
            "SELECT attributeID, COALESCE(valueFloat, valueInt) FROM entity_attributes "
            "WHERE itemID = %u AND attributeID IN (37,4,271,272,273,274,267,268,269,270,113,111,109,110)",
            shipID))
            while (lres.GetRow(lrow)) vals[(uint16)lrow.GetUInt(0)] = lrow.GetDouble(1);
        auto rz = [&](uint16 a) { return vals.count(a) ? vals[a] : 1.0; };
        // arrays in the client's EM / Thermal / Kinetic / Explosive order
        live = json{
            {"maxVelocity", vals.count(37) ? vals[37] : 0.0},
            {"shield", json::array({rz(271), rz(274), rz(273), rz(272)})},
            {"armor",  json::array({rz(267), rz(270), rz(269), rz(268)})},
            {"hull",   json::array({rz(113), rz(110), rz(109), rz(111)})},
        };
    }

    return json{
        {"live", live},
        {"shipItemID",      shipID},
        {"shipTypeID",      shipTypeID},
        {"shipName",        shipName},
        {"modules",         modules},
        {"shipCpuOutput",   shipCpu},
        {"shipPowerOutput", shipPg},
        {"shipCalibration", shipCalib},
        {"shipCapacitorGJ", shipCap},
        {"shipRechargeMs",  shipRecharge},
    };
}

/** shipModuleCmd — mutate one fitted module on the character's ACTIVE ship.
 *  payload { characterID, itemID, action: "unfit"|"online"|"offline" }.
 *    - unfit   : move the entity to the docked station's hangar (flag 4,
 *                locationID = stationID). Requires the pilot to be docked.
 *    - online  : set AttrOnline (attribute 2) valueInt = 1 in entity_attributes.
 *    - offline : set AttrOnline (attribute 2) valueInt = 0.
 *  Validates the item actually lives on the character's ship in a fittable
 *  slot before touching it, then returns the refreshed fitting (same shape as
 *  getShipFitting) so the client updates in one round-trip.
 *
 *  Direct-DB write, matching the other docked-side handlers (dock/undock):
 *  fitting happens while docked, where evemu has no in-space ModuleManager
 *  holding these items live, so the entity/attribute rows are the source of
 *  truth and are re-read on next ship load. */
static json handleShipModuleCmd(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("action")) {
        throw std::runtime_error("payload requires { characterID, action }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    std::string action   = payload.at("action").get<std::string>();
    if (action != "unfit" && action != "online" && action != "offline" && action != "fit"
        && action != "unload" && action != "load") {
        throw std::runtime_error("action must be unfit|online|offline|fit|unload|load");
    }
    // itemID identifies the fitted module for unfit/online/offline; fit uses
    // typeID + targetFlag instead (the hangar shows stacks, not itemIDs).
    uint32_t itemID = payload.contains("itemID") ? payload.at("itemID").get<uint32_t>() : 0;

    uint32_t shipID = 0, stationID = 0;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT shipID, stationID FROM chrCharacters WHERE characterID = %u", characterID))
        {
            throw std::runtime_error(std::string("DB error (char): ") + res.error.c_str());
        }
        DBResultRow row;
        if (!res.GetRow(row)) throw std::runtime_error("character not found");
        shipID    = row.GetUInt(0);
        stationID = row.IsNull(1) ? 0 : row.GetUInt(1);
    }
    if (shipID == 0) throw std::runtime_error("character has no active ship");

    // ── fit: split one module of `typeID` from the docked hangar into a slot ──
    if (action == "fit") {
        if (stationID == 0) throw std::runtime_error("must be docked to fit a module");
        if (!payload.contains("typeID") || !payload.contains("targetFlag")) {
            throw std::runtime_error("fit requires { typeID, targetFlag }");
        }
        uint32_t typeID = payload.at("typeID").get<uint32_t>();
        int targetFlag  = payload.at("targetFlag").get<int>();
        // Slot kind from the flag → the slot-power effect the module must have
        // (11 loPower, 12 hiPower, 13 medPower). Rigs aren't fittable here yet.
        int wantEffect = 0;
        if      (targetFlag >= 27 && targetFlag <= 34) wantEffect = 12;
        else if (targetFlag >= 19 && targetFlag <= 26) wantEffect = 13;
        else if (targetFlag >= 11 && targetFlag <= 18) wantEffect = 11;
        else throw std::runtime_error("fit supports high/mid/low slots only");
        // The module type must carry the matching slot-power effect.
        {
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT 1 FROM dgmTypeEffects WHERE typeID = %u AND effectID = %d", typeID, wantEffect))
            {
                throw std::runtime_error(std::string("DB error (fit-effect): ") + res.error.c_str());
            }
            DBResultRow row;
            if (!res.GetRow(row)) throw std::runtime_error("that module doesn't fit this slot type");
        }
        // Target slot must be empty (no module already there).
        {
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT COUNT(*) FROM entity e "
                "LEFT JOIN invTypes t ON e.typeID = t.typeID "
                "LEFT JOIN invGroups g ON t.groupID = g.groupID "
                "WHERE e.locationID = %u AND e.flag = %d AND COALESCE(g.categoryID,7) = 7",
                shipID, targetFlag))
            {
                throw std::runtime_error(std::string("DB error (fit-slot): ") + res.error.c_str());
            }
            DBResultRow row; res.GetRow(row);
            if (row.GetInt(0) > 0) throw std::runtime_error("that slot is already occupied");
        }
        // Find one stack of this module type in the station hangar.
        uint32_t stackID = 0; int qty = 0;
        {
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT e.itemID, e.quantity FROM entity e "
                "LEFT JOIN invTypes t ON e.typeID = t.typeID "
                "LEFT JOIN invGroups g ON t.groupID = g.groupID "
                "WHERE e.ownerID = %u AND e.locationID = %u AND e.flag = 4 AND e.typeID = %u "
                "  AND COALESCE(g.categoryID,7) = 7 LIMIT 1",
                characterID, stationID, typeID))
            {
                throw std::runtime_error(std::string("DB error (fit-stack): ") + res.error.c_str());
            }
            DBResultRow row;
            if (!res.GetRow(row)) throw std::runtime_error("no such module in your hangar");
            stackID = row.GetUInt(0);
            qty = row.IsNull(1) ? 1 : row.GetInt(1);
        }
        DBerror err;
        if (qty <= 1) {
            // Whole stack is one unit — just move + assemble it into the slot.
            if (!sDatabase.RunQuery(err,
                "UPDATE entity SET locationID = %u, flag = %d, singleton = 1 WHERE itemID = %u",
                shipID, targetFlag, stackID))
            {
                throw std::runtime_error(std::string("DB error (fit-move): ") + err.c_str());
            }
        } else {
            // Split one unit off the stack into a fresh fitted entity.
            if (!sDatabase.RunQuery(err,
                "UPDATE entity SET quantity = quantity - 1 WHERE itemID = %u", stackID))
            {
                throw std::runtime_error(std::string("DB error (fit-split): ") + err.c_str());
            }
            uint32_t newID = 0;
            if (!sDatabase.RunQueryLID(err, newID,
                "INSERT INTO entity (itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
                "VALUES ('', %u, %u, %u, %d, 0, 1, 1, 0, 0, 0, '')",
                typeID, characterID, shipID, targetFlag))
            {
                throw std::runtime_error(std::string("DB error (fit-spawn): ") + err.c_str());
            }
        }
        return handleGetShipFitting(json{{"characterID", characterID}});
    }

    // unfit / online / offline: the module must live on THIS ship in a slot.
    int flag = 0;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT flag FROM entity WHERE itemID = %u AND locationID = %u", itemID, shipID))
        {
            throw std::runtime_error(std::string("DB error (module): ") + res.error.c_str());
        }
        DBResultRow row;
        if (!res.GetRow(row)) throw std::runtime_error("module is not fitted to your active ship");
        flag = row.GetInt(0);
        const bool fittable = (flag >= 11 && flag <= 34) || (flag >= 92 && flag <= 99);
        if (!fittable) throw std::runtime_error("item is not in a fittable slot");
    }

    if (action == "unfit") {
        if (stationID == 0) throw std::runtime_error("must be docked to unfit a module");
        const bool isRig = (flag >= 92 && flag <= 99);
        if (isRig) throw std::runtime_error("rigs cannot be unfitted (would be destroyed)");
        DBerror err;
        // Move the module AND any loaded charge at the same slot flag to the
        // hangar (EVE unloads the weapon — no orphan charge left in the slot).
        if (!sDatabase.RunQuery(err,
            "UPDATE entity SET locationID = %u, flag = 4 WHERE locationID = %u AND flag = %d",
            stationID, shipID, flag))
        {
            throw std::runtime_error(std::string("DB error (unfit): ") + err.c_str());
        }
    } else if (action == "unload") {
        // VEV_AMMO_DOCKED: stage 1 of EVE-real drag-off — move ONLY the loaded
        // charge (category 8 rows sharing the module's slot flag) to cargo.
        if (stationID == 0) throw std::runtime_error("must be docked to unload here");
        DBerror err;
        if (!sDatabase.RunQuery(err,
            "UPDATE entity e "
            "JOIN invTypes t ON t.typeID = e.typeID "
            "JOIN invGroups g ON g.groupID = t.groupID "
            "SET e.flag = 5 "
            "WHERE e.locationID = %u AND e.flag = %d AND g.categoryID = 8",
            shipID, flag))
        {
            throw std::runtime_error(std::string("DB error (unload): ") + err.c_str());
        }
    } else if (action == "load") {
        // VEV_AMMO_DOCKED: drag ammo onto a fitted weapon while docked. The
        // in-space twin is the reload_ammo queue verb (EntityList.cpp).
        if (stationID == 0) throw std::runtime_error("must be docked to load ammo here");
        if (!payload.contains("chargeTypeID")) throw std::runtime_error("load requires { chargeTypeID }");
        uint32_t chargeTypeID = payload.at("chargeTypeID").get<uint32_t>();

        // Module type (for compatibility + magazine attrs).
        uint32_t modTypeID = 0;
        {
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT typeID FROM entity WHERE itemID = %u", itemID))
                throw std::runtime_error(std::string("DB error (load-mod): ") + res.error.c_str());
            DBResultRow row;
            if (!res.GetRow(row)) throw std::runtime_error("module not found");
            modTypeID = row.GetUInt(0);
        }
        // Charge meta: group, unit volume, size class (128).
        uint32_t chGroup = 0; double chVol = 0.0; double chSize = 0.0;
        std::string chName = "charge";
        {
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT t.groupID, t.volume, t.typeName, "
                "       COALESCE((SELECT COALESCE(valueFloat, valueInt) FROM dgmTypeAttributes "
                "                 WHERE typeID = t.typeID AND attributeID = 128), 0) "
                "FROM invTypes t WHERE t.typeID = %u", chargeTypeID))
                throw std::runtime_error(std::string("DB error (load-charge): ") + res.error.c_str());
            DBResultRow row;
            if (!res.GetRow(row)) throw std::runtime_error("unknown charge type");
            chGroup = row.GetUInt(0);
            chVol   = row.GetDouble(1);
            chName  = row.GetText(2);
            chSize  = row.GetDouble(3);
        }
        // Module side: chargeGroup1 (604), chargeSize (128), capacity (38).
        double modChargeGrp = 0, modChargeSize = 0, modCap = 0;
        {
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT attributeID, COALESCE(valueFloat, valueInt) FROM dgmTypeAttributes "
                "WHERE typeID = %u AND attributeID IN (604, 128, 38)", modTypeID))
                throw std::runtime_error(std::string("DB error (load-attrs): ") + res.error.c_str());
            DBResultRow row;
            while (res.GetRow(row)) {
                switch (row.GetInt(0)) {
                    case 604: modChargeGrp  = row.GetDouble(1); break;
                    case 128: modChargeSize = row.GetDouble(1); break;
                    case 38:  modCap        = row.GetDouble(1); break;
                }
            }
        }
        if (modCap <= 0) {
            // Magazine size lives in invTypes.capacity for most modules —
            // attr 38 only overrides it (the Vulcan has NO attr-38 row).
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT capacity FROM invTypes WHERE typeID = %u", modTypeID))
                throw std::runtime_error(std::string("DB error (load-cap): ") + res.error.c_str());
            DBResultRow row;
            if (res.GetRow(row)) modCap = row.GetDouble(0);
        }
        if (modChargeGrp <= 0) throw std::runtime_error("that module does not take charges");
        if ((uint32_t)modChargeGrp != chGroup) throw std::runtime_error("incompatible charge type for this module");
        if (modChargeSize > 0 && chSize > 0 && modChargeSize != chSize)
            throw std::runtime_error("wrong charge size for this module");
        int magazine = (chVol > 0) ? (int)(modCap / chVol) : 0;
        if (magazine <= 0) throw std::runtime_error("that module does not take charges");

        // Existing charge rows in the slot: same type keeps + tops off,
        // a different type is swapped out to cargo first.
        DBerror err;
        uint32_t haveRowID = 0; int have = 0;
        {
            DBQueryResult res;
            if (!sDatabase.RunQuery(res,
                "SELECT e.itemID, e.typeID, e.quantity FROM entity e "
                "JOIN invTypes t ON t.typeID = e.typeID "
                "JOIN invGroups g ON g.groupID = t.groupID "
                "WHERE e.locationID = %u AND e.flag = %d AND g.categoryID = 8",
                shipID, flag))
                throw std::runtime_error(std::string("DB error (load-slot): ") + res.error.c_str());
            DBResultRow row;
            while (res.GetRow(row)) {
                if (row.GetUInt(1) == chargeTypeID && haveRowID == 0) {
                    haveRowID = row.GetUInt(0);
                    have = row.IsNull(2) ? 1 : row.GetInt(2);
                } else {
                    if (!sDatabase.RunQuery(err,
                        "UPDATE entity SET flag = 5 WHERE itemID = %u", row.GetUInt(0)))
                        throw std::runtime_error(std::string("DB error (load-swap): ") + err.c_str());
                }
            }
        }
        int need = magazine - have;
        if (need <= 0) throw std::runtime_error("magazine is already full");

        // Pull from ship cargo (flag 5) first, then the station hangar (flag 4).
        int taken = 0;
        const char* srcQueries[2] = {
            "SELECT itemID, quantity FROM entity WHERE locationID = %u AND flag = 5 AND typeID = %u",
            "SELECT itemID, quantity FROM entity WHERE locationID = %u AND flag = 4 AND ownerID = %u AND typeID = %u",
        };
        for (int s = 0; s < 2 && need > 0; s++) {
            DBQueryResult res;
            bool ok = (s == 0)
                ? sDatabase.RunQuery(res, srcQueries[0], shipID, chargeTypeID)
                : sDatabase.RunQuery(res, srcQueries[1], stationID, characterID, chargeTypeID);
            if (!ok) throw std::runtime_error(std::string("DB error (load-src): ") + res.error.c_str());
            DBResultRow row;
            while (res.GetRow(row) && need > 0) {
                uint32_t srcID = row.GetUInt(0);
                int srcQty = row.IsNull(1) ? 1 : row.GetInt(1);
                int take = (srcQty < need) ? srcQty : need;
                if (!sDatabase.RunQuery(err,
                    "UPDATE entity SET quantity = quantity - %d WHERE itemID = %u", take, srcID))
                    throw std::runtime_error(std::string("DB error (load-take): ") + err.c_str());
                sDatabase.RunQuery(err, "DELETE FROM entity WHERE itemID = %u AND quantity <= 0", srcID);
                need -= take; taken += take;
            }
        }
        if (taken == 0) throw std::runtime_error(std::string("no ") + chName + " in cargo or hangar");

        if (haveRowID != 0) {
            if (!sDatabase.RunQuery(err,
                "UPDATE entity SET quantity = quantity + %d WHERE itemID = %u", taken, haveRowID))
                throw std::runtime_error(std::string("DB error (load-top): ") + err.c_str());
        } else {
            if (!sDatabase.RunQuery(err,
                "INSERT INTO entity (itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
                "VALUES ('', %u, %u, %u, %d, 0, 0, %d, 0, 0, 0, '')",
                chargeTypeID, characterID, shipID, flag, taken))
                throw std::runtime_error(std::string("DB error (load-spawn): ") + err.c_str());
        }
    } else {
        int value = (action == "online") ? 1 : 0;
        DBerror err;
        if (!sDatabase.RunQuery(err,
            "INSERT INTO entity_attributes (itemID, attributeID, valueInt, valueFloat) "
            "VALUES (%u, 2, %d, NULL) "
            "ON DUPLICATE KEY UPDATE valueInt = %d, valueFloat = NULL", itemID, value, value))
        {
            throw std::runtime_error(std::string("DB error (online): ") + err.c_str());
        }
    }

    return handleGetShipFitting(json{{"characterID", characterID}});
}

static json handleGetShipType(const json& payload) {
    if (!payload.is_object() || !payload.contains("typeID")) {
        throw std::runtime_error("payload requires { typeID: number }");
    }
    uint32_t typeID = payload.at("typeID").get<uint32_t>();

    std::string typeName, groupName; double mass = 0, cargo = 0;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT t.typeName, t.mass, t.capacity, g.groupName "
            "FROM invTypes t LEFT JOIN invGroups g ON t.groupID = g.groupID "
            "WHERE t.typeID = %u", typeID))
        {
            throw std::runtime_error(std::string("DB error (type): ") + res.error.c_str());
        }
        DBResultRow row;
        if (!res.GetRow(row)) throw std::runtime_error("ship type not found");
        typeName  = row.GetText(0) ? std::string(row.GetText(0)) : "";
        mass      = row.IsNull(1) ? 0 : row.GetDouble(1);
        cargo     = row.IsNull(2) ? 0 : row.GetDouble(2);
        groupName = row.GetText(3) ? std::string(row.GetText(3)) : "";
    }

    // Pivot the hull's dogma attributes (resonances default 1 = 0% resist,
    // warp multiplier/base default 1, everything else 0).
    double cap=0, recharge=0, cpu=0, pg=0, calib=0;
    double shieldHp=0, armorHp=0, structHp=0, shieldRecharge=0;
    double sEm=1, sTh=1, sKin=1, sExp=1, aEm=1, aTh=1, aKin=1, aExp=1;
    double vel=0, agility=0, warpBase=1, warpMult=1;
    double sig=0, scanRes=0, maxTargets=0, maxRange=0, droneCap=0, droneBw=0;
    double hi=0, med=0, low=0, rig=0, turret=0, launcher=0;
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT attributeID, COALESCE(valueFloat, valueInt) v FROM dgmTypeAttributes "
            "WHERE typeID = %u AND attributeID IN "
            "(482,55,48,11,1132, 263,265,9,479, 37,70, 1281,600, 552,564,192,76, 283,1271, "
            " 14,13,12,1137,101,102, 271,272,273,274, 267,268,269,270)", typeID))
        {
            DBResultRow row;
            while (res.GetRow(row)) {
                int id = row.GetInt(0); double v = row.GetDouble(1);
                switch (id) {
                    case 482: cap = v; break;          case 55: recharge = v; break;
                    case 48: cpu = v; break;           case 11: pg = v; break;
                    case 1132: calib = v; break;
                    case 263: shieldHp = v; break;     case 265: armorHp = v; break;
                    case 9: structHp = v; break;       case 479: shieldRecharge = v; break;
                    case 271: sEm = v; break;          case 274: sTh = v; break;
                    case 273: sKin = v; break;         case 272: sExp = v; break;
                    case 267: aEm = v; break;          case 270: aTh = v; break;
                    case 269: aKin = v; break;         case 268: aExp = v; break;
                    case 37: vel = v; break;           case 70: agility = v; break;
                    case 1281: warpBase = v; break;    case 600: warpMult = v; break;
                    case 552: sig = v; break;          case 564: scanRes = v; break;
                    case 192: maxTargets = v; break;   case 76: maxRange = v; break;
                    case 283: droneCap = v; break;     case 1271: droneBw = v; break;
                    case 14: hi = v; break;            case 13: med = v; break;
                    case 12: low = v; break;           case 1137: rig = v; break;
                    case 102: turret = v; break;       case 101: launcher = v; break;
                }
            }
        }
    }

    return json{
        {"typeID", typeID}, {"typeName", typeName}, {"groupName", groupName},
        {"mass", mass}, {"cargoCapacity", cargo},
        {"cpuOutput", cpu}, {"powerOutput", pg}, {"calibration", calib},
        {"capacitorGJ", cap}, {"rechargeMs", recharge},
        {"shieldHp", shieldHp}, {"armorHp", armorHp}, {"structureHp", structHp},
        {"shieldRechargeMs", shieldRecharge},
        {"shieldResist", json::array({ 1.0-sEm, 1.0-sTh, 1.0-sKin, 1.0-sExp })},
        {"armorResist",  json::array({ 1.0-aEm, 1.0-aTh, 1.0-aKin, 1.0-aExp })},
        {"maxVelocity", vel}, {"agility", agility}, {"warpSpeed", warpBase * warpMult},
        {"signatureRadius", sig}, {"scanResolution", scanRes},
        {"maxLockedTargets", (int)maxTargets}, {"maxTargetRange", maxRange},
        {"droneCapacity", droneCap}, {"droneBandwidth", droneBw},
        {"highSlots", (int)hi}, {"midSlots", (int)med}, {"lowSlots", (int)low},
        {"rigSlots", (int)rig}, {"turretSlots", (int)turret}, {"launcherSlots", (int)launcher},
    };
}

/** getCharacterSheet — the EVE Character Sheet for one character: identity
 *  header + the 5 hero attributes + trained skills (grouped) + skill queue.
 *  Schema verified on Catalyst:
 *    - identity   : chrCharacters + chrRaces/Bloodlines/Ancestries/Schools,
 *                   corp name via eveStaticOwners.ownerName, DoB = createDateTime
 *                   (Win64 filetime), securityStatus = securityRating.
 *    - attributes : chrCharacterAttributes (charID), attr 164-168
 *                   (Cha/Int/Mem/Per/Wil).
 *    - skills     : entity flag 7 (flagSkill) + entity_attributes 280 (level) /
 *                   276 (SP), grouped via invTypes.groupID → invGroups.
 *    - queue      : chrSkillQueue (ordered) joined to invTypes for names. */
static json handleGetCharacterSheet(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();

    // ── Identity ──
    json identity;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT c.characterName, c.skillPoints, c.securityRating, c.createDateTime, "
            "       r.raceName, b.bloodlineName, a.ancestryName, s.schoolName, "
            "       c.corporationID, o.ownerName "
            "FROM chrCharacters c "
            "LEFT JOIN chrRaces r ON c.raceID = r.raceID "
            "LEFT JOIN chrBloodlines b ON c.bloodlineID = b.bloodlineID "
            "LEFT JOIN chrAncestries a ON c.ancestryID = a.ancestryID "
            "LEFT JOIN chrSchools s ON c.schoolID = s.schoolID "
            "LEFT JOIN eveStaticOwners o ON c.corporationID = o.ownerID "
            "WHERE c.characterID = %u", characterID))
        {
            throw std::runtime_error(std::string("DB error (identity): ") + res.error.c_str());
        }
        if (res.GetRowCount() == 0) throw std::runtime_error("character not found");
        DBResultRow row; res.GetRow(row);
        auto textOr = [&](uint32_t i) { return row.GetText(i) ? std::string(row.GetText(i)) : std::string(); };
        identity = json{
            {"name",           textOr(0)},
            {"skillPoints",    row.GetInt64(1)},
            {"securityStatus", row.GetDouble(2)},
            {"dob",            row.GetInt64(3)},      // Win64 filetime
            {"raceName",       textOr(4)},
            {"bloodlineName",  textOr(5)},
            {"ancestryName",   textOr(6)},
            {"schoolName",     textOr(7)},
            {"corporationID",  row.GetUInt(8)},
            {"corpName",       textOr(9)},
        };
    }

    // ── Hero attributes (Cha/Int/Mem/Per/Wil = 164/165/166/167/168) ──
    json attributes = json::object();
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT attributeID, valueInt, valueFloat FROM chrCharacterAttributes "
            "WHERE charID = %u AND attributeID IN (164,165,166,167,168)", characterID))
        {
            DBResultRow row;
            while (res.GetRow(row)) {
                double v = row.IsNull(1) ? row.GetDouble(2) : static_cast<double>(row.GetInt(1));
                switch (row.GetUInt(0)) {
                    case 164: attributes["charisma"] = v; break;
                    case 165: attributes["intelligence"] = v; break;
                    case 166: attributes["memory"] = v; break;
                    case 167: attributes["perception"] = v; break;
                    case 168: attributes["willpower"] = v; break;
                }
            }
        }
    }

    // ── Trained skills (entity flag 7 + attrs 280 level / 276 SP) ──
    json skills = json::array();
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT g.groupName, t.typeName, t.typeID, "
            "       COALESCE(al.valueInt, al.valueFloat) AS lvl, "
            "       COALESCE(ap.valueInt, ap.valueFloat) AS sp "
            "FROM entity e "
            "JOIN invTypes t ON e.typeID = t.typeID "
            "LEFT JOIN invGroups g ON t.groupID = g.groupID "
            "LEFT JOIN entity_attributes al ON al.itemID = e.itemID AND al.attributeID = 280 "
            "LEFT JOIN entity_attributes ap ON ap.itemID = e.itemID AND ap.attributeID = 276 "
            "WHERE e.ownerID = %u AND e.flag = 7 "
            "ORDER BY g.groupName, t.typeName", characterID))
        {
            throw std::runtime_error(std::string("DB error (skills): ") + res.error.c_str());
        }
        DBResultRow row;
        while (res.GetRow(row)) {
            skills.push_back(json{
                {"groupName", row.GetText(0) ? std::string(row.GetText(0)) : "Other"},
                {"typeName",  row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"typeID",    row.GetUInt(2)},
                {"level",     row.GetInt(3)},
                {"sp",        row.GetInt(4)},
            });
        }
    }

    // ── Skill queue (ordered) ──
    json queue = json::array();
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT q.orderIndex, q.typeID, t.typeName, q.level, q.startTime, q.endTime "
            "FROM chrSkillQueue q "
            "LEFT JOIN invTypes t ON q.typeID = t.typeID "
            "WHERE q.characterID = %u ORDER BY q.orderIndex", characterID))
        {
            DBResultRow row;
            while (res.GetRow(row)) {
                queue.push_back(json{
                    {"orderIndex", row.GetInt(0)},
                    {"typeID",     row.GetUInt(1)},
                    {"typeName",   row.GetText(2) ? std::string(row.GetText(2)) : ""},
                    {"level",      row.GetInt(3)},
                    {"startTime",  row.GetInt64(4)},
                    {"endTime",    row.GetInt64(5)},
                });
            }
        }
    }

    return json{
        {"identity",   identity},
        {"attributes", attributes},
        {"skills",     skills},
        {"queue",      queue},
    };
}

// ─── Dispatcher ───────────────────────────────────────────────────────────

/** getWalletJournal — wallet transaction journal (jnlCharacters), newest first.
 *  Balance lives on the character (getCharacter); this is the history. */
static json handleGetWalletJournal(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT j.entryTypeID, et.entryTypeName, j.amount, j.balance, j.transactionDate, j.description "
        "FROM jnlCharacters j "
        "LEFT JOIN jnlEntryTypeIDs et ON j.entryTypeID = et.entryTypeID "
        "WHERE j.ownerID1 = %u OR j.ownerID2 = %u "
        "ORDER BY j.transactionDate DESC LIMIT 100", characterID, characterID))
    {
        throw std::runtime_error(std::string("DB error (wallet journal): ") + res.error.c_str());
    }
    json entries = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        entries.push_back(json{
            {"entryTypeID",   row.GetUInt(0)},
            {"entryTypeName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"amount",        row.GetDouble(2)},
            {"balance",       row.GetDouble(3)},
            {"date",          row.GetInt64(4)},
            {"description",   row.GetText(5) ? std::string(row.GetText(5)) : ""},
        });
    }
    return json{{"entries", entries}};
}

/** getWalletTransactions — market transactions (mktTransactions): items the
 *  character bought/sold, newest first. transactionType 1 = buy, 0 = sell.
 *  transactionDate is stored via %f so it is read as a double. */
static json handleGetWalletTransactions(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT t.transactionID, t.transactionDate, t.typeID, it.typeName, "
        "       t.quantity, t.price, t.transactionType, t.stationID, ss.stationName, t.clientID "
        "FROM mktTransactions t "
        "LEFT JOIN invTypes it ON t.typeID = it.typeID "
        "LEFT JOIN staStations ss ON t.stationID = ss.stationID "
        "WHERE t.characterID = %u "
        "ORDER BY t.transactionDate DESC LIMIT 100", characterID))
    {
        throw std::runtime_error(std::string("DB error (wallet transactions): ") + res.error.c_str());
    }
    json transactions = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        transactions.push_back(json{
            {"transactionID", row.GetUInt(0)},
            {"date",          row.GetDouble(1)},
            {"typeID",        row.GetUInt(2)},
            {"typeName",      row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"quantity",      row.GetUInt(4)},
            {"price",         row.GetDouble(5)},
            {"isBuy",         row.GetUInt(6) != 0},
            {"stationID",     row.GetUInt(7)},
            {"stationName",   row.GetText(8) ? std::string(row.GetText(8)) : ""},
            {"clientID",      row.GetUInt(9)},
        });
    }
    return json{{"transactions", transactions}};
}

/** getMyOrders — the character's OWN open market orders (mktOrders, ownerID =
 *  char). Distinct from the market convo's getMarketOrders (regional order book
 *  for a type). bid 1 = buy order, 0 = sell order. issued is a Win64 filetime;
 *  duration is the order lifetime in days. */
static json handleGetMyOrders(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT o.orderID, o.typeID, it.typeName, o.bid, o.price, o.volEntered, "
        "       o.volRemaining, o.issued, o.duration, o.stationID, ss.stationName, o.solarSystemID "
        "FROM mktOrders o "
        "LEFT JOIN invTypes it ON o.typeID = it.typeID "
        "LEFT JOIN staStations ss ON o.stationID = ss.stationID "
        "WHERE o.ownerID = %u "
        "ORDER BY o.issued DESC", characterID))
    {
        throw std::runtime_error(std::string("DB error (market orders): ") + res.error.c_str());
    }
    json orders = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        orders.push_back(json{
            {"orderID",       row.GetUInt(0)},
            {"typeID",        row.GetUInt(1)},
            {"typeName",      row.GetText(2) ? std::string(row.GetText(2)) : ""},
            {"isBuy",         row.GetUInt(3) != 0},
            {"price",         row.GetDouble(4)},
            {"volEntered",    row.GetUInt(5)},
            {"volRemaining",  row.GetUInt(6)},
            {"issued",        row.GetInt64(7)},
            {"duration",      row.GetUInt(8)},
            {"stationID",     row.GetUInt(9)},
            {"stationName",   row.GetText(10) ? std::string(row.GetText(10)) : ""},
            {"solarSystemID", row.GetUInt(11)},
        });
    }
    return json{{"orders", orders}};
}

/** getAssets — every asset the character owns across ALL stations, each item
 *  and ship carrying its location (station + solar system) so the client can
 *  build the EVE location tree. Distinct from getCharacterAssets, which SUMS
 *  stacks across stations (dropping location) and is scoped to one station for
 *  the in-station Items window. cargo (flag 5) rides with the active ship -> it
 *  is returned unlocated, grouped by type. */
static json handleGetAssets(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();

    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT e.locationID, COALESCE(ss.stationName,'') AS stationName, "
        "       COALESCE(ss.solarSystemID,0) AS solarSystemID, COALESCE(sys.solarSystemName,'') AS systemName, "
        "       t.typeID, t.typeName, COALESCE(g.groupName,'') AS groupName, "
        "       SUM(e.quantity) AS qty, COALESCE(mt.metaGroupID,1) AS metaGroup "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "LEFT JOIN invMetaTypes mt ON mt.typeID = e.typeID "
        "LEFT JOIN staStations ss ON ss.stationID = e.locationID "
        "LEFT JOIN mapSolarSystems sys ON sys.solarSystemID = ss.solarSystemID "
        "WHERE e.ownerID = %u AND e.flag = 4 "
        "  AND (g.categoryID IS NULL OR g.categoryID <> 6) "
        "GROUP BY e.locationID, stationName, solarSystemID, systemName, "
        "         t.typeID, t.typeName, groupName, metaGroup "
        "ORDER BY systemName, stationName, groupName, t.typeName",
        characterID))
    {
        throw std::runtime_error(std::string("DB error (assets items): ") + res.error.c_str());
    }
    json items = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        items.push_back(json{
            {"stationID",     row.GetUInt(0)},
            {"stationName",   row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"solarSystemID", row.GetUInt(2)},
            {"systemName",    row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"typeID",        row.GetUInt(4)},
            {"name",          row.GetText(5) ? std::string(row.GetText(5)) : ""},
            {"groupName",     row.GetText(6) ? std::string(row.GetText(6)) : ""},
            {"qty",           row.GetInt64(7)},
            {"metaGroup",     row.GetUInt(8)},
        });
    }

    DBQueryResult sres;
    if (!sDatabase.RunQuery(sres,
        "SELECT e.itemID, e.typeID, t.typeName, "
        "       COALESCE(NULLIF(e.itemName,''), t.typeName) AS dispName, "
        "       COALESCE(g.groupName,'') AS groupName, COALESCE(mt.metaGroupID,1) AS metaGroup, "
        "       e.locationID, COALESCE(ss.stationName,'') AS stationName, "
        "       COALESCE(ss.solarSystemID,0) AS solarSystemID, COALESCE(sys.solarSystemName,'') AS systemName "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "LEFT JOIN invMetaTypes mt ON mt.typeID = e.typeID "
        "LEFT JOIN staStations ss ON ss.stationID = e.locationID "
        "LEFT JOIN mapSolarSystems sys ON sys.solarSystemID = ss.solarSystemID "
        "WHERE e.ownerID = %u AND e.flag = 4 AND g.categoryID = 6 "
        "ORDER BY systemName, stationName, dispName",
        characterID))
    {
        throw std::runtime_error(std::string("DB error (assets ships): ") + sres.error.c_str());
    }
    json ships = json::array();
    DBResultRow srow;
    while (sres.GetRow(srow)) {
        ships.push_back(json{
            {"itemID",        static_cast<int64_t>(srow.GetInt64(0))},
            {"typeID",        srow.GetUInt(1)},
            {"typeName",      srow.GetText(2) ? std::string(srow.GetText(2)) : ""},
            {"name",          srow.GetText(3) ? std::string(srow.GetText(3)) : ""},
            {"groupName",     srow.GetText(4) ? std::string(srow.GetText(4)) : ""},
            {"metaGroup",     srow.GetUInt(5)},
            {"stationID",     srow.GetUInt(6)},
            {"stationName",   srow.GetText(7) ? std::string(srow.GetText(7)) : ""},
            {"solarSystemID", srow.GetUInt(8)},
            {"systemName",    srow.GetText(9) ? std::string(srow.GetText(9)) : ""},
        });
    }

    DBQueryResult cres;
    if (!sDatabase.RunQuery(cres,
        "SELECT t.typeID, t.typeName, COALESCE(g.groupName,'') AS groupName, "
        "       SUM(e.quantity) AS qty, COALESCE(mt.metaGroupID,1) AS metaGroup "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "LEFT JOIN invMetaTypes mt ON mt.typeID = e.typeID "
        "WHERE e.ownerID = %u AND e.flag = 5 "
        /* VEV_ASSETS_ACTIVE_SHIP */ "AND e.locationID = (SELECT shipID FROM chrCharacters WHERE characterID = e.ownerID) "
        "  AND (g.categoryID IS NULL OR g.categoryID <> 6) "
        "GROUP BY t.typeID, t.typeName, groupName, metaGroup "
        "ORDER BY groupName, t.typeName",
        characterID))
    {
        throw std::runtime_error(std::string("DB error (assets cargo): ") + cres.error.c_str());
    }
    json cargo = json::array();
    DBResultRow crow;
    while (cres.GetRow(crow)) {
        cargo.push_back(json{
            {"typeID",    crow.GetUInt(0)},
            {"name",      crow.GetText(1) ? std::string(crow.GetText(1)) : ""},
            {"groupName", crow.GetText(2) ? std::string(crow.GetText(2)) : ""},
            {"qty",       crow.GetInt64(3)},
            {"metaGroup", crow.GetUInt(4)},
        });
    }

    return json{{"items", items}, {"ships", ships}, {"cargo", cargo}};
}

/** getTypeInfo — EVE "Show Info" for a TYPE (item / module / ship): the
 *  description header, dogma attributes (with units), fitting requirements
 *  (cpu / powergrid / slot), skill prerequisites, and variations. Pure
 *  static-data reads (invTypes / invGroups / invCategories / dgm* / eveUnits /
 *  invMetaTypes). Read-only - no character context. */
static json handleGetTypeInfo(const json& payload) {
    if (!payload.is_object() || !payload.contains("typeID")) {
        throw std::runtime_error("payload requires { typeID: number }");
    }
    uint32_t typeID = payload.at("typeID").get<uint32_t>();

    DBQueryResult hres;
    if (!sDatabase.RunQuery(hres,
        "SELECT t.typeName, COALESCE(t.description,''), COALESCE(g.groupName,''), "
        "       COALESCE(c.categoryName,''), COALESCE(mt.metaGroupID,1), "
        "       t.volume, t.mass, t.capacity, t.basePrice "
        "FROM invTypes t "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "LEFT JOIN invCategories c ON c.categoryID = g.categoryID "
        "LEFT JOIN invMetaTypes mt ON mt.typeID = t.typeID "
        "WHERE t.typeID = %u", typeID))
    {
        throw std::runtime_error(std::string("DB error (typeinfo header): ") + hres.error.c_str());
    }
    DBResultRow hrow;
    if (!hres.GetRow(hrow)) throw std::runtime_error("unknown typeID");
    json out = json{
        {"typeID",       typeID},
        {"typeName",     hrow.GetText(0) ? std::string(hrow.GetText(0)) : ""},
        {"description",  hrow.GetText(1) ? std::string(hrow.GetText(1)) : ""},
        {"groupName",    hrow.GetText(2) ? std::string(hrow.GetText(2)) : ""},
        {"categoryName", hrow.GetText(3) ? std::string(hrow.GetText(3)) : ""},
        {"metaGroup",    hrow.GetUInt(4)},
        {"volume",       hrow.GetDouble(5)},
        {"mass",         hrow.GetDouble(6)},
        {"capacity",     hrow.GetDouble(7)},
        {"basePrice",    hrow.GetDouble(8)},
    };

    DBQueryResult ares;
    if (!sDatabase.RunQuery(ares,
        "SELECT da.attributeID, COALESCE(NULLIF(da.displayName,''), da.attributeName), "
        "       IF(ta.valueInt IS NULL, ta.valueFloat, ta.valueInt) AS value, "
        "       COALESCE(u.displayName,''), COALESCE(da.unitID,0), COALESCE(da.iconID,0) "
        "FROM dgmTypeAttributes ta "
        "JOIN dgmAttributeTypes da ON da.attributeID = ta.attributeID "
        "LEFT JOIN eveUnits u ON u.unitID = da.unitID "
        "WHERE ta.typeID = %u AND da.published = 1 "
        "  AND da.displayName IS NOT NULL AND da.displayName <> '' "
        "ORDER BY da.attributeID", typeID))
    {
        throw std::runtime_error(std::string("DB error (typeinfo attrs): ") + ares.error.c_str());
    }
    json attributes = json::array();
    DBResultRow arow;
    while (ares.GetRow(arow)) {
        attributes.push_back(json{
            {"attributeID", arow.GetUInt(0)},
            {"name",        arow.GetText(1) ? std::string(arow.GetText(1)) : ""},
            {"value",       arow.GetDouble(2)},
            {"unit",        arow.GetText(3) ? std::string(arow.GetText(3)) : ""},
            {"unitID",      arow.GetUInt(4)},
            {"iconID",      arow.GetUInt(5)},
        });
    }
    out["attributes"] = attributes;

    double cpu = 0, power = 0;
    DBQueryResult fres;
    if (sDatabase.RunQuery(fres,
        "SELECT attributeID, IF(valueInt IS NULL, valueFloat, valueInt) AS value "
        "FROM dgmTypeAttributes WHERE typeID = %u AND attributeID IN (30,50)", typeID))
    {
        DBResultRow frow;
        while (fres.GetRow(frow)) {
            uint32_t a = frow.GetUInt(0);
            if (a == 50) cpu = frow.GetDouble(1);
            else if (a == 30) power = frow.GetDouble(1);
        }
    }
    std::string slot;
    DBQueryResult eres;
    if (sDatabase.RunQuery(eres,
        "SELECT effectID FROM dgmTypeEffects WHERE typeID = %u AND effectID IN (11,12,13,2663,3772)", typeID))
    {
        DBResultRow erow;
        while (eres.GetRow(erow)) {
            switch (erow.GetUInt(0)) {
                case 12:   slot = "High power";   break;
                case 13:   slot = "Medium power"; break;
                case 11:   slot = "Low power";    break;
                case 2663: slot = "Rig slot";     break;
                case 3772: slot = "Subsystem";    break;
            }
        }
    }
    out["fitting"] = json{{"cpu", cpu}, {"powergrid", power}, {"slot", slot}};

    int sk[6] = {0,0,0,0,0,0};
    int lv[6] = {0,0,0,0,0,0};
    DBQueryResult pres;
    if (sDatabase.RunQuery(pres,
        "SELECT attributeID, IF(valueInt IS NULL, valueFloat, valueInt) AS value "
        "FROM dgmTypeAttributes WHERE typeID = %u "
        "AND attributeID IN (182,183,184,1285,1289,1290,277,278,279,1286,1287,1288)", typeID))
    {
        DBResultRow prow;
        while (pres.GetRow(prow)) {
            int v = static_cast<int>(prow.GetDouble(1));
            switch (prow.GetUInt(0)) {
                case 182:  sk[0]=v; break;  case 277:  lv[0]=v; break;
                case 183:  sk[1]=v; break;  case 278:  lv[1]=v; break;
                case 184:  sk[2]=v; break;  case 279:  lv[2]=v; break;
                case 1285: sk[3]=v; break;  case 1286: lv[3]=v; break;
                case 1289: sk[4]=v; break;  case 1287: lv[4]=v; break;
                case 1290: sk[5]=v; break;  case 1288: lv[5]=v; break;
            }
        }
    }
    json prerequisites = json::array();
    for (int i = 0; i < 6; ++i) {
        if (sk[i] <= 0) continue;
        std::string skillName;
        DBQueryResult nres;
        if (sDatabase.RunQuery(nres, "SELECT typeName FROM invTypes WHERE typeID = %u", static_cast<uint32_t>(sk[i]))) {
            DBResultRow nrow;
            if (nres.GetRow(nrow) && nrow.GetText(0)) skillName = nrow.GetText(0);
        }
        prerequisites.push_back(json{
            {"skillTypeID", static_cast<uint32_t>(sk[i])},
            {"skillName",   skillName},
            {"level",       lv[i]},
        });
    }
    out["prerequisites"] = prerequisites;

    uint32_t parent = typeID;
    DBQueryResult ppres;
    if (sDatabase.RunQuery(ppres, "SELECT parentTypeID FROM invMetaTypes WHERE typeID = %u", typeID)) {
        DBResultRow pprow;
        if (ppres.GetRow(pprow)) { uint32_t p = pprow.GetUInt(0); if (p) parent = p; }
    }
    json variations = json::array();
    DBQueryResult vres;
    if (sDatabase.RunQuery(vres,
        "SELECT t.typeID, t.typeName, COALESCE(mt.metaGroupID,1) AS metaGroupID "
        "FROM invTypes t LEFT JOIN invMetaTypes mt ON mt.typeID = t.typeID "
        "WHERE t.typeID = %u OR mt.parentTypeID = %u "
        "ORDER BY metaGroupID, t.typeName", parent, parent))
    {
        DBResultRow vrow;
        while (vres.GetRow(vrow)) {
            variations.push_back(json{
                {"typeID",    vrow.GetUInt(0)},
                {"typeName",  vrow.GetText(1) ? std::string(vrow.GetText(1)) : ""},
                {"metaGroup", vrow.GetUInt(2)},
            });
        }
    }
    out["variations"] = variations;

    return out;
}

/** getCharacterDetails - the Character Sheet's non-skill sub-tabs: Bio
 *  (chrCharacters.description), Employment History (chrEmployment + corp names),
 *  Standings (repStandings, this char as fromID), and Augmentations (implants =
 *  entity flag 89). Names resolve via eveStaticOwners / invTypes. Read-only.
 *  Jump clones are returned empty (evemu has no clone store we read here). */
static json handleGetCharacterDetails(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();

    std::string bio;
    DBQueryResult bres;
    if (sDatabase.RunQuery(bres, "SELECT COALESCE(description,'') FROM chrCharacters WHERE characterID = %u", characterID)) {
        DBResultRow brow;
        if (bres.GetRow(brow) && brow.GetText(0)) bio = brow.GetText(0);
    }

    json employment = json::array();
    DBQueryResult eres;
    if (sDatabase.RunQuery(eres,
        "SELECT em.corporationID, COALESCE(o.ownerName,''), em.startDate "
        "FROM chrEmployment em "
        "LEFT JOIN eveStaticOwners o ON o.ownerID = em.corporationID "
        "WHERE em.characterID = %u AND (em.deleted = 0 OR em.deleted IS NULL) "
        "ORDER BY em.startDate DESC", characterID))
    {
        DBResultRow row;
        while (eres.GetRow(row)) {
            employment.push_back(json{
                {"corporationID", row.GetUInt(0)},
                {"corpName",      row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"startDate",     row.GetDouble(2)},
            });
        }
    }

    json standings = json::array();
    DBQueryResult sres;
    if (sDatabase.RunQuery(sres,
        "SELECT rs.toID, COALESCE(o.ownerName,''), rs.standing "
        "FROM repStandings rs "
        "LEFT JOIN eveStaticOwners o ON o.ownerID = rs.toID "
        "WHERE rs.fromID = %u "
        "ORDER BY rs.standing DESC", characterID))
    {
        DBResultRow row;
        while (sres.GetRow(row)) {
            standings.push_back(json{
                {"toID",     row.GetUInt(0)},
                {"toName",   row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"standing", row.GetDouble(2)},
            });
        }
    }

    json implants = json::array();
    DBQueryResult ires;
    if (sDatabase.RunQuery(ires,
        "SELECT e.typeID, t.typeName, COALESCE(g.groupName,'') "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE e.ownerID = %u AND e.flag = 89 "
        "ORDER BY t.typeName", characterID))
    {
        DBResultRow row;
        while (ires.GetRow(row)) {
            implants.push_back(json{
                {"typeID",    row.GetUInt(0)},
                {"typeName",  row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"groupName", row.GetText(2) ? std::string(row.GetText(2)) : ""},
            });
        }
    }

    return json{
        {"bio",        bio},
        {"employment", employment},
        {"standings",  standings},
        {"implants",   implants},
        {"jumpClones", json::array()},
    };
}

/** getContacts - the character's personal address book (chrContacts), strongest
 *  relationship first. Names resolve via eveStaticOwners (NPC corps/factions)
 *  with a chrCharacters fallback (player chars). relationshipID = the standing
 *  the owner assigned. Powers People & Places -> Contacts. Read-only.
 *  (`+ 0` coerces the bit columns to ints for the DB layer.) */
static json handleGetContacts(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT cc.contactID, COALESCE(NULLIF(o.ownerName,''), cch.characterName, '') AS name, "
        "       cc.relationshipID, (cc.inWatchlist + 0) AS inWatchlist "
        "FROM chrContacts cc "
        "LEFT JOIN eveStaticOwners o ON o.ownerID = cc.contactID "
        "LEFT JOIN chrCharacters cch ON cch.characterID = cc.contactID "
        "WHERE cc.ownerID = %u AND (cc.blocked + 0) = 0 "
        "ORDER BY cc.relationshipID DESC", characterID))
    {
        throw std::runtime_error(std::string("DB error (contacts): ") + res.error.c_str());
    }
    json contacts = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        contacts.push_back(json{
            {"contactID",   row.GetUInt(0)},
            {"name",        row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"standing",    row.GetFloat(2)},
            {"inWatchlist", row.GetUInt(3) != 0},
        });
    }
    return json{{"contacts", contacts}};
}

/** getMail - the character's EVE Mail. A UNION of two halves, mirroring how
 *  evemu's MailDB actually stores mail: (1) RECEIVED rows in mailStatus WHERE
 *  characterID = me (per-recipient read/label state), received=1; (2) SENT-to-
 *  others = mailMessage WHERE senderID = me with no mailStatus row of mine
 *  (SendMail never writes a sender-side status row, so Sent derives from
 *  senderID), received=0 with synthesized Read/Sent masks. A self-mail surfaces
 *  in BOTH. Returns toCorpOrAllianceID + toListID for Corp/Alliance/list folders.
 *  sentDate is a Win32 filetime. Read-only. */
static json handleGetMail(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT m.messageID, m.senderID, "
        "       COALESCE(NULLIF(o.ownerName,''), cch.characterName, '') AS senderName, "
        "       m.title, COALESCE(m.sentDate,0) AS sentDate,COALESCE(s.statusMask,0), COALESCE(s.labelMask,0), "
        "       COALESCE(m.toCorpOrAllianceID,0), COALESCE(m.toListID,0), 1 AS received "
        "FROM mailStatus s "
        "LEFT JOIN mailMessage m USING (messageID) "
        "LEFT JOIN eveStaticOwners o ON o.ownerID = m.senderID "
        "LEFT JOIN chrCharacters cch ON cch.characterID = m.senderID "
        "WHERE s.characterID = %u "
        "UNION "
        "SELECT m.messageID, m.senderID, "
        "       COALESCE(NULLIF(o.ownerName,''), cch.characterName, '') AS senderName, "
        "       m.title, COALESCE(m.sentDate,0) AS sentDate,1 AS statusMask, 2 AS labelMask, "
        "       COALESCE(m.toCorpOrAllianceID,0), COALESCE(m.toListID,0), 0 AS received "
        "FROM mailMessage m "
        "LEFT JOIN eveStaticOwners o ON o.ownerID = m.senderID "
        "LEFT JOIN chrCharacters cch ON cch.characterID = m.senderID "
        "WHERE m.senderID = %u "
        "  AND m.messageID NOT IN (SELECT messageID FROM mailStatus WHERE characterID = %u) "
        "ORDER BY sentDate DESC", characterID, characterID, characterID))
    {
        throw std::runtime_error(std::string("DB error (mail): ") + res.error.c_str());
    }
    json messages = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        messages.push_back(json{
            {"messageID", row.GetUInt(0)},
            {"senderID",  row.GetUInt(1)},
            {"senderName", row.GetText(2) ? std::string(row.GetText(2)) : ""},
            {"title",     row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"sentDate",  row.GetInt64(4)},
            {"statusMask", row.GetInt64(5)},
            {"labelMask", row.GetInt64(6)},
            {"toCorpOrAllianceID", row.GetUInt(7)},
            {"toListID",  row.GetUInt(8)},
            {"received",  row.GetUInt(9) != 0},
        });
    }
    return json{{"messages", messages}};
}

/** getMailBody - inflated text of one message. mailMessage.body is a raw zlib
 *  stream (0x78 0x9C, no length prefix); read the BLOB binary-safe via
 *  GetText+ColumnLength (embedded NULs) and inflate in place with InflateData. */
static json handleGetMailBody(const json& payload) {
    if (!payload.is_object() || !payload.contains("messageID")) {
        throw std::runtime_error("payload requires { messageID: number }");
    }
    uint32_t messageID = payload.at("messageID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT body FROM mailMessage WHERE messageID = %u", messageID))
    {
        throw std::runtime_error(std::string("DB error (mail body): ") + res.error.c_str());
    }
    DBResultRow row;
    if (!res.GetRow(row) || row.IsNull(0)) {
        return json{{"body", ""}};
    }
    const char* raw = row.GetText(0);
    uint32_t len = row.ColumnLength(0);
    if (raw == nullptr || len == 0) {
        return json{{"body", ""}};
    }
    Buffer buf(raw, raw + len);
    if (IsDeflated(buf) && !InflateData(buf)) {
        throw std::runtime_error("mail body inflate failed");
    }
    std::string body = buf.size()
        ? std::string(reinterpret_cast<const char*>(&buf[0]), buf.size())
        : std::string();
    return json{{"body", body}};
}

/** setMailStatus - toggle read/trash on the viewer's OWN mailStatus row. Targets
 *  mailStatus (not mailMessage) and filters by messageID AND characterID. */
static json handleSetMailStatus(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")
        || !payload.contains("messageID") || !payload.contains("action")) {
        throw std::runtime_error("payload requires { characterID, messageID, action }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t messageID = payload.at("messageID").get<uint32_t>();
    std::string action = payload.at("action").get<std::string>();
    const char* sql = nullptr;
    if (action == "read")
        sql = "UPDATE mailStatus SET statusMask = statusMask | 1 WHERE messageID = %u AND characterID = %u";
    else if (action == "unread")
        sql = "UPDATE mailStatus SET statusMask = statusMask & ~1 WHERE messageID = %u AND characterID = %u";
    else if (action == "trash")
        sql = "UPDATE mailStatus SET statusMask = statusMask | 8 WHERE messageID = %u AND characterID = %u";
    else if (action == "untrash")
        sql = "UPDATE mailStatus SET statusMask = statusMask & ~8 WHERE messageID = %u AND characterID = %u";
    else
        throw std::runtime_error("setMailStatus: action must be read|unread|trash|untrash");
    DBerror err;
    if (!sDatabase.RunQuery(err, sql, messageID, characterID)) {
        throw std::runtime_error(std::string("DB error (set mail status): ") + err.c_str());
    }
    return json{{"ok", true}, {"messageID", messageID}, {"action", action}};
}

using HandlerFn = std::function<json(const json&)>;

static json handleGetCharacterAssets(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number, stationID?: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t stationID   = payload.value("stationID", 0u);  // 0 = across all stations

    // Query A - stackable contents: station item hangar (flag 4, ships
    // excluded) + active-ship cargo hold (flag 5), collapsed by type so
    // multiple stacks merge. When stationID is given the hangar is scoped to
    // it (flag-4 rows carry locationID = stationID); cargo (flag 5) rides with
    // the ship and is never station-scoped.
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT e.flag, t.typeID, t.typeName, COALESCE(g.groupName,'') AS groupName, "
        "       SUM(e.quantity) AS qty, COALESCE(mt.metaGroupID,1) AS metaGroup "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "LEFT JOIN invMetaTypes mt ON mt.typeID = e.typeID "
        "WHERE e.ownerID = %u AND e.flag IN (4,5) "
        "  AND (g.categoryID IS NULL OR g.categoryID <> 6) "
        "  AND (e.flag <> 4 OR %u = 0 OR e.locationID = %u) "
        /* VEV_ASSETS_ACTIVE_SHIP */ "  AND (e.flag <> 5 OR e.locationID = (SELECT shipID FROM chrCharacters WHERE characterID = e.ownerID)) "
        "GROUP BY e.flag, t.typeID, t.typeName, groupName, metaGroup "
        "ORDER BY groupName, t.typeName",
        characterID, stationID, stationID))
    {
        throw std::runtime_error(std::string("DB error (assets): ") + res.error.c_str());
    }
    json items = json::array();
    json cargo = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        json item = json{
            {"typeID",    row.GetUInt(1)},
            {"name",      row.GetText(2) ? std::string(row.GetText(2)) : ""},
            {"groupName", row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"qty",       row.GetInt64(4)},
            {"metaGroup", row.GetUInt(5)},
        };
        if (row.GetUInt(0) == 5) cargo.push_back(item);
        else                     items.push_back(item);
    }

    // Query B - ships in the station ship hangar (flag 4, category 6). Listed
    // individually (each hull is a distinct entity with its own itemID and an
    // optional player-assigned name), not stacked.
    DBQueryResult sres;
    if (!sDatabase.RunQuery(sres,
        "SELECT e.itemID, e.typeID, t.typeName, "
        "       COALESCE(NULLIF(e.itemName,''), t.typeName) AS dispName, "
        "       COALESCE(g.groupName,'') AS groupName, COALESCE(mt.metaGroupID,1) AS metaGroup "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "LEFT JOIN invMetaTypes mt ON mt.typeID = e.typeID "
        "WHERE e.ownerID = %u AND e.flag = 4 AND g.categoryID = 6 "
        "  AND (%u = 0 OR e.locationID = %u) "
        "ORDER BY dispName",
        characterID, stationID, stationID))
    {
        throw std::runtime_error(std::string("DB error (ships): ") + sres.error.c_str());
    }
    json ships = json::array();
    DBResultRow srow;
    while (sres.GetRow(srow)) {
        ships.push_back(json{
            {"itemID",    static_cast<int64_t>(srow.GetInt64(0))},
            {"typeID",    srow.GetUInt(1)},
            {"typeName",  srow.GetText(2) ? std::string(srow.GetText(2)) : ""},
            {"name",      srow.GetText(3) ? std::string(srow.GetText(3)) : ""},
            {"groupName", srow.GetText(4) ? std::string(srow.GetText(4)) : ""},
            {"metaGroup", srow.GetUInt(5)},
        });
    }

    return json{{"items", items}, {"ships", ships}, {"cargo", cargo}};
}

// Market group tree (invMarketGroups) + the k-space region list for the
// regional-market selector. Static reference data; the client builds the tree
// from parentID and caches it. Icons reuse the iconID webps.
static json handleGetMarketTree(const json& payload) {
    (void)payload;
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT marketGroupID, COALESCE(parentGroupID,0) AS parentID, marketGroupName, "
        "       COALESCE(iconID,0) AS iconID, CAST(hasTypes AS UNSIGNED) AS hasTypes "
        "FROM invMarketGroups ORDER BY marketGroupName"))
    {
        throw std::runtime_error(std::string("DB error (marketTree): ") + res.error.c_str());
    }
    json groups = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        groups.push_back(json{
            {"id",       row.GetUInt(0)},
            {"parentID", row.GetUInt(1)},
            {"name",     row.GetText(2) ? std::string(row.GetText(2)) : ""},
            {"iconID",   row.GetUInt(3)},
            {"hasTypes", row.GetInt64(4) != 0},
        });
    }
    DBQueryResult rres;
    if (!sDatabase.RunQuery(rres,
        "SELECT regionID, regionName FROM mapRegions WHERE regionID < 11000000 ORDER BY regionName"))
    {
        throw std::runtime_error(std::string("DB error (marketRegions): ") + rres.error.c_str());
    }
    json regions = json::array();
    DBResultRow rrow;
    while (rres.GetRow(rrow)) {
        regions.push_back(json{
            {"id",   rrow.GetUInt(0)},
            {"name", rrow.GetText(1) ? std::string(rrow.GetText(1)) : ""},
        });
    }
    return json{{"groups", groups}, {"regions", regions}};
}

// Types in a market group, or a name search across all market types when a
// non-empty `query` is given. metaGroup → the client's tech/faction badge.
static json handleGetMarketTypes(const json& payload) {
    if (!payload.is_object()) {
        throw std::runtime_error("payload must be an object");
    }
    const std::string query = payload.value("query", "");
    uint32_t marketGroupID = payload.value("marketGroupID", 0u);

    DBQueryResult res;
    bool ok;
    if (!query.empty()) {
        std::string esc;
        sDatabase.DoEscapeString(esc, query);
        ok = sDatabase.RunQuery(res,
            "SELECT t.typeID, t.typeName, COALESCE(mt.metaGroupID,1) AS metaGroup, "
            "       COALESCE(t.marketGroupID,0) AS mg "
            "FROM invTypes t LEFT JOIN invMetaTypes mt ON mt.typeID=t.typeID "
            "WHERE t.typeName LIKE '%%%s%%' AND t.marketGroupID IS NOT NULL AND t.published=1 "
            "ORDER BY t.typeName LIMIT 100", esc.c_str());
    } else {
        ok = sDatabase.RunQuery(res,
            "SELECT t.typeID, t.typeName, COALESCE(mt.metaGroupID,1) AS metaGroup, "
            "       COALESCE(t.marketGroupID,0) AS mg "
            "FROM invTypes t LEFT JOIN invMetaTypes mt ON mt.typeID=t.typeID "
            "WHERE t.marketGroupID=%u AND t.published=1 ORDER BY t.typeName", marketGroupID);
    }
    if (!ok) {
        throw std::runtime_error(std::string("DB error (marketTypes): ") + res.error.c_str());
    }
    json types = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        types.push_back(json{
            {"typeID",        row.GetUInt(0)},
            {"name",          row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"metaGroup",     row.GetInt64(2)},
            {"marketGroupID", row.GetUInt(3)},
        });
    }
    return json{{"types", types}};
}

// Live sell + buy orders for a type within a region (mktOrders). Sell ordered
// cheapest-first, buy highest-first; capped at 100 each. bid is a bit(1) so we
// CAST it. Station names join staStations.
static json handleGetMarketOrders(const json& payload) {
    if (!payload.is_object() || !payload.contains("typeID") || !payload.contains("regionID")) {
        throw std::runtime_error("payload requires { typeID: number, regionID: number }");
    }
    uint32_t typeID = payload.at("typeID").get<uint32_t>();
    uint32_t regionID = payload.at("regionID").get<uint32_t>();

    json sell = json::array();
    json buy  = json::array();

    DBQueryResult sres;
    if (!sDatabase.RunQuery(sres,
        "SELECT o.price, o.volRemaining, o.minVolume, o.jumps, o.duration, o.stationID, "
        "       COALESCE(s.stationName, CONCAT('Station ', o.stationID)) AS stationName, o.solarSystemID, "
        "       o.ownerID, CAST(o.isCorp AS UNSIGNED) AS isCorp "
        "FROM mktOrders o LEFT JOIN staStations s ON s.stationID=o.stationID "
        "WHERE o.typeID=%u AND o.regionID=%u AND CAST(o.bid AS UNSIGNED)=0 "
        "ORDER BY o.price ASC LIMIT 100", typeID, regionID))
    {
        throw std::runtime_error(std::string("DB error (sellOrders): ") + sres.error.c_str());
    }
    DBResultRow srow;
    while (sres.GetRow(srow)) {
        sell.push_back(json{
            {"price",         srow.GetDouble(0)},
            {"volRemaining",  srow.GetInt64(1)},
            {"minVolume",     srow.GetInt64(2)},
            {"jumps",         srow.GetInt64(3)},
            {"duration",      srow.GetInt64(4)},
            {"stationID",     srow.GetUInt(5)},
            {"stationName",   srow.GetText(6) ? std::string(srow.GetText(6)) : ""},
            {"solarSystemID", srow.GetUInt(7)},
            {"ownerID",       srow.GetUInt(8)},                 // VEV_CORP_FILTER
            {"isCorp",        srow.GetUInt(9) != 0},            // VEV_CORP_FILTER
        });
    }

    DBQueryResult bres;
    if (!sDatabase.RunQuery(bres,
        "SELECT o.price, o.volRemaining, o.minVolume, o.jumps, o.duration, o.stationID, "
        "       COALESCE(s.stationName, CONCAT('Station ', o.stationID)) AS stationName, o.solarSystemID, "
        "       o.ownerID, CAST(o.isCorp AS UNSIGNED) AS isCorp "
        "FROM mktOrders o LEFT JOIN staStations s ON s.stationID=o.stationID "
        "WHERE o.typeID=%u AND o.regionID=%u AND CAST(o.bid AS UNSIGNED)=1 "
        "ORDER BY o.price DESC LIMIT 100", typeID, regionID))
    {
        throw std::runtime_error(std::string("DB error (buyOrders): ") + bres.error.c_str());
    }
    DBResultRow brow;
    while (bres.GetRow(brow)) {
        buy.push_back(json{
            {"price",         brow.GetDouble(0)},
            {"volRemaining",  brow.GetInt64(1)},
            {"minVolume",     brow.GetInt64(2)},
            {"jumps",         brow.GetInt64(3)},
            {"duration",      brow.GetInt64(4)},
            {"stationID",     brow.GetUInt(5)},
            {"stationName",   brow.GetText(6) ? std::string(brow.GetText(6)) : ""},
            {"solarSystemID", brow.GetUInt(7)},
            {"ownerID",       brow.GetUInt(8)},                 // VEV_CORP_FILTER
            {"isCorp",        brow.GetUInt(9) != 0},            // VEV_CORP_FILTER
        });
    }

    return json{{"sell", sell}, {"buy", buy}};
}

// marketBuy — immediate purchase from a station's cheapest sell order, for the
// LOGGED-IN 2D pilot. The 2D client market was view-only (Place Buy Order / Sell
// were stubs), so a human couldn't transact even though the AI could. This reuses
// the PROVEN EntityList buy_from_sell_order (balance check, AccountService transfer,
// item spawn to hangar, order decrement) by enqueueing it -- no logic duplication.
// login_docked first (idempotent) so the pilot is instantiated as a phantom the
// buy handler accepts. Async: the item lands in the hangar when the loop drains the
// queue. params: { characterID, typeID, quantity? }.
static json handleMarketBuy(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("typeID")) {
        throw std::runtime_error("payload requires { characterID, typeID, quantity? }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t typeID      = payload.at("typeID").get<uint32_t>();
    uint32_t quantity    = payload.contains("quantity") ? payload.at("quantity").get<uint32_t>() : 1u;
    if (quantity == 0) quantity = 1;
    enqueueAICommand(characterID, "login_docked", "{}");
    const bool ok = enqueueAICommand(characterID, "buy_from_sell_order",
        json{{"typeID", typeID}, {"quantity", quantity}}.dump());
    return json{{"ok", ok}, {"queued", ok}};
}

// VEV_PLACE_SELL — client-driven CORP sell order (de-fakes the 2D 'Sell' button). Clones
// handleMarketBuy: login_docked (idempotent) so the pilot is a docked phantom the place_sell_order
// verb accepts, then enqueue the PROVEN EntityList place_sell_order (corp-hangar listing, Crucible
// broker fee from the corp wallet). Args snake_case to match the verb's parser. params:
// { characterID, typeID, quantity, price, stationID?, durationDays?, minVolume?, range? }.
static json handlePlaceSellOrder(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("typeID") ||
        !payload.contains("quantity") || !payload.contains("price")) {
        throw std::runtime_error("payload requires { characterID, typeID, quantity, price, stationID?, durationDays? }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t typeID      = payload.at("typeID").get<uint32_t>();
    uint32_t quantity    = payload.at("quantity").get<uint32_t>();
    double   price       = payload.at("price").get<double>();
    if (quantity == 0) quantity = 1;
    json verbArgs = {{"type_id", typeID}, {"quantity", quantity}, {"price", price}};
    if (payload.contains("stationID"))    verbArgs["station_id"]    = payload.at("stationID").get<uint32_t>();
    if (payload.contains("durationDays")) verbArgs["duration_days"] = payload.at("durationDays").get<uint32_t>();
    if (payload.contains("minVolume"))    verbArgs["min_volume"]    = payload.at("minVolume").get<uint32_t>();
    if (payload.contains("range"))        verbArgs["range"]         = payload.at("range").get<int>();
    enqueueAICommand(characterID, "login_docked", "{}");
    const bool ok = enqueueAICommand(characterID, "place_sell_order", verbArgs.dump());
    return json{{"ok", ok}, {"queued", ok}};
}

// Bookmarks ("Places") — saved warpable locations. One list row, joined to
// mapDenormalize (item name) + mapSolarSystems (system name).
static json bookmarkRowJson(DBResultRow& row) {
    return json{
        {"bookmarkID",   row.GetUInt(0)},
        {"itemID",       row.GetUInt(1)},
        {"typeID",       row.GetUInt(2)},
        {"solarSystemID",row.GetUInt(3)},
        {"systemName",   row.GetText(4) ? std::string(row.GetText(4)) : ""},
        {"label",        row.GetText(5) ? std::string(row.GetText(5)) : ""},
        {"x",            row.GetDouble(6)},
        {"y",            row.GetDouble(7)},
        {"z",            row.GetDouble(8)},
        {"created",      row.GetInt64(9)},
    };
}

static const char* BOOKMARK_SELECT =
    "SELECT b.bookmarkID, b.itemID, b.typeID, b.locationID, "
    "       COALESCE(s.solarSystemName,'') AS systemName, "
    "       COALESCE(NULLIF(b.memo,''), d.itemName, '') AS label, "
    "       b.x, b.y, b.z, b.created "
    "FROM bookmarks b "
    "LEFT JOIN mapDenormalize d ON d.itemID = b.itemID "
    "LEFT JOIN mapSolarSystems s ON s.solarSystemID = b.locationID ";

static json handleListBookmarks(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID: number }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    DBQueryResult res;
    if (!sDatabase.RunQuery(res, "%s WHERE b.ownerID = %u ORDER BY b.created DESC",
        BOOKMARK_SELECT, characterID))
    {
        throw std::runtime_error(std::string("DB error (listBookmarks): ") + res.error.c_str());
    }
    json bookmarks = json::array();
    DBResultRow row;
    while (res.GetRow(row)) bookmarks.push_back(bookmarkRowJson(row));
    return json{{"bookmarks", bookmarks}};
}

// Save a location. `itemID` explicit, else the docked station, else the
// current system's sun. Coords/type/system/name resolved from mapDenormalize
// so the bookmark is always a real, warpable celestial. Returns the new row.
static json handleAddBookmark(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID, itemID?, memo? }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t itemID = payload.value("itemID", 0u);
    std::string memo = payload.value("memo", "");

    if (itemID == 0) {
        DBQueryResult cres;
        if (!sDatabase.RunQuery(cres,
            "SELECT stationID, solarSystemID FROM chrCharacters WHERE characterID = %u", characterID))
        {
            throw std::runtime_error(std::string("DB error (char): ") + cres.error.c_str());
        }
        DBResultRow crow;
        if (!cres.GetRow(crow)) throw std::runtime_error("character not found");
        uint32_t stationID = crow.GetUInt(0);
        uint32_t solarSystemID = crow.GetUInt(1);
        if (stationID > 0) {
            itemID = stationID;
        } else {
            DBQueryResult sres;
            if (!sDatabase.RunQuery(sres,
                "SELECT itemID FROM mapDenormalize WHERE solarSystemID = %u AND groupID = 6 LIMIT 1", solarSystemID))
            {
                throw std::runtime_error(std::string("DB error (sun): ") + sres.error.c_str());
            }
            DBResultRow srow;
            if (sres.GetRow(srow)) itemID = srow.GetUInt(0);
            else throw std::runtime_error("no sun for current system");
        }
    }

    std::string memoEsc;
    sDatabase.DoEscapeString(memoEsc, memo);

    DBerror err;
    uint32_t bookmarkID = 0;
    if (!sDatabase.RunQueryLID(err, bookmarkID,
        "INSERT INTO bookmarks (ownerID, itemID, typeID, memo, created, x, y, z, locationID, note, creatorID) "
        "SELECT %u, d.itemID, d.typeID, COALESCE(NULLIF('%s',''), d.itemName), "
        "       UNIX_TIMESTAMP()*10000000+116444736000000000, d.x, d.y, d.z, d.solarSystemID, '', %u "
        "FROM mapDenormalize d WHERE d.itemID = %u",
        characterID, memoEsc.c_str(), characterID, itemID))
    {
        throw std::runtime_error(std::string("DB error (addBookmark): ") + err.c_str());
    }
    if (bookmarkID == 0) throw std::runtime_error("itemID not found in mapDenormalize");

    DBQueryResult res;
    if (!sDatabase.RunQuery(res, "%s WHERE b.bookmarkID = %u", BOOKMARK_SELECT, bookmarkID)) {
        throw std::runtime_error(std::string("DB error (read bookmark): ") + res.error.c_str());
    }
    DBResultRow row;
    if (!res.GetRow(row)) throw std::runtime_error("inserted bookmark not found");
    return bookmarkRowJson(row);
}

static json handleDeleteBookmark(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("bookmarkID")) {
        throw std::runtime_error("payload requires { characterID, bookmarkID }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t bookmarkID = payload.at("bookmarkID").get<uint32_t>();
    DBerror err;
    if (!sDatabase.RunQuery(err,
        "DELETE FROM bookmarks WHERE bookmarkID = %u AND ownerID = %u", bookmarkID, characterID))
    {
        throw std::runtime_error(std::string("DB error (deleteBookmark): ") + err.c_str());
    }
    return json{{"deleted", true}};
}

static json handleRenameBookmark(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("bookmarkID")) {
        throw std::runtime_error("payload requires { characterID, bookmarkID, memo }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t bookmarkID = payload.at("bookmarkID").get<uint32_t>();
    std::string memo = payload.value("memo", "");
    std::string memoEsc;
    sDatabase.DoEscapeString(memoEsc, memo);
    DBerror err;
    if (!sDatabase.RunQuery(err,
        "UPDATE bookmarks SET memo = '%s' WHERE bookmarkID = %u AND ownerID = %u",
        memoEsc.c_str(), bookmarkID, characterID))
    {
        throw std::runtime_error(std::string("DB error (renameBookmark): ") + err.c_str());
    }
    return json{{"renamed", true}};
}

// Eject into the Capsule. Every capsuleer has a pod (typeID 670); spawn one
// (flagCapsule 56, at the pilot's solar system) if missing, then point shipID
// at it. Returns the refreshed character.
static json handleEject(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) {
        throw std::runtime_error("payload requires { characterID }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();

    uint32_t capsuleID = 0;
    DBQueryResult cres;
    if (!sDatabase.RunQuery(cres,
        "SELECT itemID FROM entity WHERE ownerID = %u AND typeID = 670 LIMIT 1", characterID))
    {
        throw std::runtime_error(std::string("DB error (capsule lookup): ") + cres.error.c_str());
    }
    DBResultRow crow;
    if (cres.GetRow(crow)) {
        capsuleID = crow.GetUInt(0);
    } else {
        DBQueryResult sres;
        if (!sDatabase.RunQuery(sres,
            "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", characterID))
        {
            throw std::runtime_error(std::string("DB error (sys): ") + sres.error.c_str());
        }
        DBResultRow srow;
        if (!sres.GetRow(srow)) throw std::runtime_error("character not found");
        uint32_t solarSystemID = srow.GetUInt(0);
        std::string nameEsc;
        sDatabase.DoEscapeString(nameEsc, "Capsule");
        DBerror serr;
        if (!sDatabase.RunQueryLID(serr, capsuleID,
            "INSERT INTO entity (itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
            "VALUES ('%s', 670, %u, %u, 56, 0, 1, 1, 0, 0, 0, '')",
            nameEsc.c_str(), characterID, solarSystemID))
        {
            throw std::runtime_error(std::string("DB error (spawn capsule): ") + serr.c_str());
        }
        if (capsuleID == 0) throw std::runtime_error("failed to spawn capsule");
    }

    DBerror uerr;
    if (!sDatabase.RunQuery(uerr,
        "UPDATE chrCharacters SET shipID = %u WHERE characterID = %u", capsuleID, characterID))
    {
        throw std::runtime_error(std::string("DB error (eject): ") + uerr.c_str());
    }
    return handleGetCharacter(json{{"characterID", characterID}});
}

// Board an owned ship (make it the active ship). Verifies the item belongs to
// the character and is a ship (category 6) before swapping shipID.
static json handleBoardShip(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("shipItemID")) {
        throw std::runtime_error("payload requires { characterID, shipItemID }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t shipItemID = payload.at("shipItemID").get<uint32_t>();

    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT e.itemID FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE e.itemID = %u AND e.ownerID = %u AND g.categoryID = 6", shipItemID, characterID))
    {
        throw std::runtime_error(std::string("DB error (board verify): ") + res.error.c_str());
    }
    DBResultRow row;
    if (!res.GetRow(row)) throw std::runtime_error("ship not found or not owned by character");

    DBerror uerr;
    if (!sDatabase.RunQuery(uerr,
        "UPDATE chrCharacters SET shipID = %u WHERE characterID = %u", shipItemID, characterID))
    {
        throw std::runtime_error(std::string("DB error (board): ") + uerr.c_str());
    }
    return handleGetCharacter(json{{"characterID", characterID}});
}

static json handleGetShipContents(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("shipItemID")) {
        throw std::runtime_error("payload requires { characterID, shipItemID }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t shipItemID = payload.at("shipItemID").get<uint32_t>();

    // Verify the hull is a category-6 ship owned by the character, and read its
    // cargo capacity (invTypes.capacity) + drone-bay capacity (dgma 283) so the
    // panel can show used/max m^3. A 0 capacity means the hull has no such bay.
    DBQueryResult vres;
    if (!sDatabase.RunQuery(vres,
        "SELECT t.capacity, "
        "       COALESCE((SELECT COALESCE(valueFloat, valueInt) FROM dgmTypeAttributes "
        "                 WHERE typeID = e.typeID AND attributeID = 283), 0) AS droneCap "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE e.itemID = %u AND e.ownerID = %u AND g.categoryID = 6",
        shipItemID, characterID))
    {
        throw std::runtime_error(std::string("DB error (contents verify): ") + vres.error.c_str());
    }
    DBResultRow vrow;
    if (!vres.GetRow(vrow)) throw std::runtime_error("ship not found or not owned by character");
    double cargoCapacity = vrow.GetDouble(0);
    double droneCapacity = vrow.GetDouble(1);

    // Everything physically inside the hull (locationID = shipItemID): split by
    // flag into cargo (5), drones (87), and fitted modules (every other slot).
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT e.flag, t.typeID, t.typeName, COALESCE(g.groupName,'') AS groupName, "
        "       e.quantity AS qty, COALESCE(mt.metaGroupID,1) AS metaGroup, t.volume AS vol, "
        "       e.itemID AS itemID, "  /* VEV_STACK_OP: per-row id for split/merge */
        "       t.groupID AS grpID, "  /* VEV_CHARGE_MATCH */
        "       COALESCE((SELECT COALESCE(valueFloat, valueInt) FROM dgmTypeAttributes "
        "                 WHERE typeID = e.typeID AND attributeID = 128), 0) AS chSize "
        "FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN invGroups g ON g.groupID = t.groupID "
        "LEFT JOIN invMetaTypes mt ON mt.typeID = e.typeID "
        "WHERE e.locationID = %u AND e.ownerID = %u "
        "ORDER BY e.flag, groupName, t.typeName",
        shipItemID, characterID))
    {
        throw std::runtime_error(std::string("DB error (contents): ") + res.error.c_str());
    }
    json cargo = json::array();
    json drones = json::array();
    json modules = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        uint32_t flag = row.GetUInt(0);
        json item = json{
            {"typeID",    row.GetUInt(1)},
            {"name",      row.GetText(2) ? std::string(row.GetText(2)) : ""},
            {"groupName", row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"qty",       row.GetInt64(4)},
            {"metaGroup", row.GetUInt(5)},
            {"flag",      flag},
            {"itemID",    row.GetUInt(7)},
            {"volume",    row.GetDouble(6)},
            {"groupID",   row.GetUInt(8)},                       // VEV_CHARGE_MATCH
            {"chargeSize", (uint32)row.GetDouble(9)},            // 0 = sizeless
        };
        if (flag == 5) cargo.push_back(item);
        else if (flag == 87) drones.push_back(item);
        else modules.push_back(item);
    }
    return json{
        {"cargo", cargo}, {"drones", drones}, {"modules", modules},
        {"cargoCapacity", cargoCapacity}, {"droneCapacity", droneCapacity},
    };
}

static json handleRenameShip(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")
        || !payload.contains("shipItemID") || !payload.contains("name")) {
        throw std::runtime_error("payload requires { characterID, shipItemID, name }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t shipItemID = payload.at("shipItemID").get<uint32_t>();
    std::string name = payload.value("name", "");
    if (name.empty() || name.size() > 80) {
        throw std::runtime_error("name must be 1-80 characters");
    }

    // Verify the item is a ship (category 6) owned by the character before rename.
    DBQueryResult vres;
    if (!sDatabase.RunQuery(vres,
        "SELECT e.itemID FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE e.itemID = %u AND e.ownerID = %u AND g.categoryID = 6",
        shipItemID, characterID))
    {
        throw std::runtime_error(std::string("DB error (rename verify): ") + vres.error.c_str());
    }
    DBResultRow vrow;
    if (!vres.GetRow(vrow)) throw std::runtime_error("ship not found or not owned by character");

    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    DBerror err;
    if (!sDatabase.RunQuery(err,
        "UPDATE entity SET itemName = '%s' WHERE itemID = %u", nameEsc.c_str(), shipItemID))
    {
        throw std::runtime_error(std::string("DB error (rename): ") + err.c_str());
    }
    return json{{"renamed", true}, {"itemID", shipItemID}, {"name", name}};
}

static json handleTrashItem(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("typeID")) {
        throw std::runtime_error("payload requires { characterID, typeID, stationID }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t typeID = payload.at("typeID").get<uint32_t>();
    uint32_t stationID = payload.value("stationID", 0u);
    // REQUIRE a specific station — the previous "0 = any station" sentinel
    // would wipe every station's stack of this type when the Items window
    // stayed open across an undock (chr.stationID becomes 0 in space).
    // Account-wide trash is never the right behavior.
    if (stationID == 0) throw std::runtime_error("must be docked at a specific station to trash items");

    // Trash every flag-4 stack at the named station. INNER JOIN invGroups so a
    // type whose group reference is missing (data corruption) fails to match
    // rather than getting trashed — better to throw than to nuke an
    // unclassifiable row.
    DBerror err;
    if (!sDatabase.RunQuery(err,
        "DELETE e FROM entity e "
        "JOIN invTypes t ON t.typeID = e.typeID "
        "JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE e.ownerID = %u AND e.typeID = %u AND e.flag = 4 "
        "  AND g.categoryID <> 6 "
        "  AND e.locationID = %u",
        characterID, typeID, stationID))
    {
        throw std::runtime_error(std::string("DB error (trash): ") + err.c_str());
    }
    return json{{"trashed", true}, {"typeID", typeID}};
}

static json handleMoveItem(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")
        || !payload.contains("typeID") || !payload.contains("shipItemID") || !payload.contains("dest")) {
        throw std::runtime_error("payload requires { characterID, typeID, shipItemID, dest }");
    }
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t typeID = payload.at("typeID").get<uint32_t>();
    uint32_t shipItemID = payload.at("shipItemID").get<uint32_t>();
    std::string dest = payload.value("dest", "");
    if (dest != "cargo" && dest != "drones" && dest != "hangar") {
        throw std::runtime_error("dest must be one of: cargo, drones, hangar");
    }

    // Hangar <-> ship transfers happen docked; the source/dest hangar is the
    // station the pilot is in.
    DBQueryResult sres;
    if (!sDatabase.RunQuery(sres,
        "SELECT stationID FROM chrCharacters WHERE characterID = %u", characterID))
    {
        throw std::runtime_error(std::string("DB error (dock): ") + sres.error.c_str());
    }
    DBResultRow srow;
    if (!sres.GetRow(srow)) throw std::runtime_error("character not found");
    uint32_t stationID = srow.GetUInt(0);
    if (stationID == 0) throw std::runtime_error("must be docked to move items");

    // Verify the hull belongs to the pilot AND is at the same flag-4 station
    // hangar (so a pilot in Jita can't move cargo into a hull docked in Amarr)
    // + read its cargo / drone-bay capacity. droneCap uses
    // COALESCE(valueFloat, valueInt) — attribute 283 is stored either way in
    // static data; matches the convention in getShipType (Gateway.cpp ~L1378).
    DBQueryResult vres;
    if (!sDatabase.RunQuery(vres,
        "SELECT t.capacity, "
        "       COALESCE((SELECT COALESCE(valueFloat, valueInt) FROM dgmTypeAttributes "
        "                 WHERE typeID = e.typeID AND attributeID = 283), 0) AS droneCap "
        "FROM entity e JOIN invTypes t ON t.typeID = e.typeID "
        "JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE e.itemID = %u AND e.ownerID = %u AND g.categoryID = 6 "
        "  AND e.flag = 4 AND e.locationID = %u",
        shipItemID, characterID, stationID))
    {
        throw std::runtime_error(std::string("DB error (hull): ") + vres.error.c_str());
    }
    DBResultRow vrow;
    if (!vres.GetRow(vrow)) throw std::runtime_error("ship not found or not owned by character");
    double cargoCapacity = vrow.GetDouble(0);
    double droneCapacity = vrow.GetDouble(1);

    // Defense-in-depth: ships (category 6) are never moveable as items via this
    // path (boardShip is the only way to seat a hull). getCharacterAssets's
    // Items query already excludes category-6 from the drag source, but a
    // hand-crafted payload would otherwise let a ship be nested inside another
    // ship's cargo hold.
    {
        DBQueryResult catres;
        if (!sDatabase.RunQuery(catres,
            "SELECT g.categoryID FROM invTypes t JOIN invGroups g ON g.groupID = t.groupID WHERE t.typeID = %u", typeID))
        {
            throw std::runtime_error(std::string("DB error (item-cat): ") + catres.error.c_str());
        }
        DBResultRow catrow;
        if (!catres.GetRow(catrow)) throw std::runtime_error("unknown type");
        if (catrow.GetUInt(0) == 6) throw std::runtime_error("ships cannot be moved as items");
    }

    // Resolve destination flag/location + the source-row predicate (on alias e).
    uint32_t destFlag, destLoc;
    std::string srcPred;
    if (dest == "hangar") {
        destFlag = 4; destLoc = stationID;
        srcPred = "e.locationID = " + std::to_string(shipItemID) + " AND e.flag IN (5,87)";
    } else {
        destLoc = shipItemID;
        srcPred = "e.flag = 4 AND e.locationID = " + std::to_string(stationID);
        if (dest == "cargo") {
            destFlag = 5;
        } else {
            destFlag = 87;
            if (droneCapacity <= 0) throw std::runtime_error("this hull has no drone bay");
            DBQueryResult cres;
            if (!sDatabase.RunQuery(cres,
                "SELECT g.categoryID FROM invTypes t JOIN invGroups g ON g.groupID = t.groupID WHERE t.typeID = %u", typeID))
            {
                throw std::runtime_error(std::string("DB error (cat): ") + cres.error.c_str());
            }
            DBResultRow crow;
            if (!cres.GetRow(crow) || crow.GetUInt(0) != 18) throw std::runtime_error("only drones fit in the drone bay");
        }
    }

    // How much (volume) of this type is at the source, and is there any?
    DBQueryResult ires;
    if (!sDatabase.RunQuery(ires,
        "SELECT COALESCE(SUM(t.volume * e.quantity), 0) AS vol, COUNT(*) AS n "
        "FROM entity e JOIN invTypes t ON t.typeID = e.typeID "
        "WHERE e.ownerID = %u AND e.typeID = %u AND %s",
        characterID, typeID, srcPred.c_str()))
    {
        throw std::runtime_error(std::string("DB error (incoming): ") + ires.error.c_str());
    }
    DBResultRow irow;
    double incoming = 0; int64_t n = 0;
    if (ires.GetRow(irow)) { incoming = irow.GetDouble(0); n = irow.GetInt64(1); }
    if (n == 0) throw std::runtime_error("nothing of that type to move");

    // Volume guard on the into-ship direction.
    if (dest == "cargo" || dest == "drones") {
        double capacity = (dest == "cargo") ? cargoCapacity : droneCapacity;
        DBQueryResult ures;
        if (!sDatabase.RunQuery(ures,
            "SELECT COALESCE(SUM(t.volume * e.quantity), 0) AS vol "
            "FROM entity e JOIN invTypes t ON t.typeID = e.typeID "
            "WHERE e.ownerID = %u AND e.locationID = %u AND e.flag = %u",
            characterID, destLoc, destFlag))
        {
            throw std::runtime_error(std::string("DB error (used): ") + ures.error.c_str());
        }
        DBResultRow urow;
        double used = 0;
        if (ures.GetRow(urow)) used = urow.GetDouble(0);
        if (used + incoming > capacity + 0.001) {
            throw std::runtime_error(std::string("not enough room in the ")
                + (dest == "cargo" ? "cargo hold" : "drone bay"));
        }
    }

    DBerror uerr;
    if (!sDatabase.RunQuery(uerr,
        "UPDATE entity e SET e.flag = %u, e.locationID = %u "
        "WHERE e.ownerID = %u AND e.typeID = %u AND %s",
        destFlag, destLoc, characterID, typeID, srcPred.c_str()))
    {
        throw std::runtime_error(std::string("DB error (move): ") + uerr.c_str());
    }
    return json{{"moved", true}, {"typeID", typeID}, {"dest", dest}};
}

static json handleGetChannelMessages(const json& payload) {
    if (!payload.is_object() || !payload.contains("channelID")) {
        throw std::runtime_error("payload requires { channelID, sinceID?, limit? }");
    }
    int32_t channelID = payload.at("channelID").get<int32_t>();
    int32_t sinceID = payload.value("sinceID", 0);
    int32_t limit = payload.value("limit", 100);
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;

    // Newest first; caller can reverse for chat-log order. The (id > sinceID)
    // delta lets the client poll incrementally without re-fetching the whole
    // history. ai_pilot_messages.id is monotonic auto-increment.
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT id, senderID, senderName, message, "
        "       UNIX_TIMESTAMP(created_at) AS ts "
        "FROM ai_pilot_messages "
        "WHERE channelID = %d AND id > %d "
        "ORDER BY id DESC LIMIT %d",
        channelID, sinceID, limit))
    {
        throw std::runtime_error(std::string("DB error (channel messages): ") + res.error.c_str());
    }
    json messages = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        messages.push_back(json{
            {"id",         row.GetUInt(0)},
            {"senderID",   row.GetUInt(1)},
            {"senderName", row.GetText(2) ? std::string(row.GetText(2)) : ""},
            {"message",    row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"ts",         row.GetInt64(4)},
        });
    }
    return json{{"messages", messages}};
}

// ─── VEV_CORP_V1 BEGIN ──────────────────────────────────────────────────────
// Corporation + corp-fleet suite for the 2D client (docs/corp-2d-ui-design.md,
// 2026-06-11). The human player is the CEO; AI pilots are requisitioned into
// the corp (createCharacter pipeline + corp adoption + vevAiPilots roster) and
// grouped into PERSISTENT corp fleets (vevFleets/vevFleetMembers — a deliberate
// deviation from evemu's session-volatile Client*-keyed fleets, which phantoms
// can't join; see Plan - Fleets (architecture).md). All handlers are plain DB
// reads/writes — no Client*, no sFltSvc, so none of the session-fleet
// threading hazards apply.
//
// Founding mirrors CorporationDB::AddCorporation (CorporationDB.cpp:517) at
// the SQL level, the same way createCharacter mirrors CharUnboundMgrService.

static const uint32_t VEV_CORP_MGMT_SKILL   = 3363;      // Corporation Management
static const double   VEV_CORP_FOUND_COST   = 20000000;  // sConfig.rates.corpCost (eve-server.xml:94)
static const double   VEV_REQUISITION_COST  = 250000;    // Vev v1: recruiting + clone contract fee
// Role_Admin bitmask — EVE_Corp.h:184 (Role_Admin: 0xffffe07ffffff81).
static const unsigned long long VEV_CORP_ROLE_ADMIN = 1152919339943329665ULL;
// V2 (2026-06-11): ISK→SP training purchase — port of v0's trade_isk_for_sp
// (A321 §3.4: skill injectors don't exist in Crucible). cost = base × rank × level².
static const double VEV_SP_BASE_COST = 500000;

// Update-or-insert a skill at `level` (V2 — vevEnsureSkill only handles the
// absent case; this also RAISES an existing lower-level row, which career
// seeding needs since base/race seeding may already have granted the skill).
static void vevGrantSkill(uint32_t characterID, uint32_t skillTypeID, uint8_t level) {
    if (level > 5) level = 5;
    DBQueryResult res;
    DBResultRow row;
    DBerror err;
    double rank = 1.0;
    if (sDatabase.RunQuery(res,
        "SELECT valueFloat, valueInt FROM dgmTypeAttributes "
        "WHERE typeID = %u AND attributeID = 275", skillTypeID) && res.GetRow(row)) {
        if (!row.IsNull(0))      rank = row.GetDouble(0);
        else if (!row.IsNull(1)) rank = static_cast<double>(row.GetInt(1));
    }
    const uint32_t targetSP = skillPointsForLevel(level, rank);

    uint32_t itemID = 0, curLevel = 0, curSP = 0;
    if (sDatabase.RunQuery(res,
        "SELECT e.itemID, COALESCE(lvl.valueInt,0), COALESCE(sp.valueInt,0) "
        "FROM entity e "
        "LEFT JOIN entity_attributes lvl ON lvl.itemID = e.itemID AND lvl.attributeID = 280 "
        "LEFT JOIN entity_attributes sp  ON sp.itemID  = e.itemID AND sp.attributeID  = 276 "
        "WHERE e.ownerID = %u AND e.typeID = %u AND e.flag = 7 LIMIT 1",
        characterID, skillTypeID) && res.GetRow(row)) {
        itemID   = row.GetUInt(0);
        curLevel = row.GetUInt(1);
        curSP    = row.GetUInt(2);
    }
    if (itemID != 0) {
        if (curLevel >= level) return;
        sDatabase.RunQuery(err,
            "UPDATE entity_attributes SET valueInt = %u WHERE itemID = %u AND attributeID = 280",
            static_cast<uint32_t>(level), itemID);
        sDatabase.RunQuery(err,
            "UPDATE entity_attributes SET valueInt = %u WHERE itemID = %u AND attributeID = 276",
            targetSP, itemID);
        if (targetSP > curSP)
            sDatabase.RunQuery(err,
                "UPDATE chrCharacters SET skillPoints = skillPoints + %u WHERE characterID = %u",
                targetSP - curSP, characterID);
        return;
    }
    if (!sDatabase.RunQueryLID(err, itemID,
        "INSERT INTO entity "
        "(itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
        "VALUES ('', %u, %u, %u, 7, 0, 0, 1, 0, 0, 0, '')",
        skillTypeID, characterID, characterID))
        return;
    sDatabase.RunQuery(err,
        "INSERT INTO entity_attributes (itemID, attributeID, valueInt, valueFloat) "
        "VALUES (%u, 280, %u, NULL), (%u, 276, %u, NULL)",
        itemID, static_cast<uint32_t>(level), itemID, targetSP);
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET skillPoints = skillPoints + %u WHERE characterID = %u",
        targetSP, characterID);
}

// Seed an evemu career skill package (sklCareerSkills) — the engine-truth
// skill-skew behind the requisition form's Military / Business / Industry
// choice. careerID = raceID*10 + {military:1, business:4, industry:7}.
static void vevSeedCareerSkills(uint32_t characterID, uint32_t raceID, const std::string& career) {
    uint32_t offset = 7;  // industry default
    if (career == "military") offset = 1;
    else if (career == "business") offset = 4;
    const uint32_t careerID = raceID * 10 + offset;
    DBQueryResult res;
    DBResultRow row;
    json rows = json::array();
    if (sDatabase.RunQuery(res,
        "SELECT skillTypeID, level FROM sklCareerSkills WHERE careerID = %u", careerID)) {
        while (res.GetRow(row))
            rows.push_back(json{{"t", row.GetUInt(0)}, {"l", row.GetUInt(1)}});
    }
    for (const auto& r : rows)
        vevGrantSkill(characterID, r.value("t", 0u), static_cast<uint8_t>(r.value("l", 0u)));
}

// Level of a character's trained skill (entity flag 7 + attr 280), 0 if untrained.
static uint8_t vevSkillLevel(uint32_t characterID, uint32_t skillTypeID) {
    DBQueryResult res;
    DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT ea.valueInt FROM entity e "
        "JOIN entity_attributes ea ON ea.itemID = e.itemID AND ea.attributeID = 280 "
        "WHERE e.ownerID = %u AND e.typeID = %u AND e.flag = 7 LIMIT 1",
        characterID, skillTypeID) && res.GetRow(row) && !row.IsNull(0))
        return static_cast<uint8_t>(row.GetUInt(0));
    return 0;
}

// evemu memberLimit formula — CorporationDB.cpp:529-532.
static uint32_t vevCorpMemberLimit(uint32_t ceoID) {
    uint32_t lim = vevSkillLevel(ceoID, VEV_CORP_MGMT_SKILL) * 20;
    lim += vevSkillLevel(ceoID, 3731) * 100;   // Megacorp Management
    lim += vevSkillLevel(ceoID, 3732) * 400;   // Empire Control
    lim += vevSkillLevel(ceoID, 3839) * 2000;  // Sovereignty
    return lim;
}

// Inject a skill at `level` if the char doesn't have it (seedStarterSkillsAndShip
// idiom: entity flag 7 + attrs 280/276 + skillPoints bump). v1 bootstrap for the
// CEO's Corporation Management — the 2D client has no human training surface yet.
static void vevEnsureSkill(uint32_t characterID, uint32_t skillTypeID, uint8_t level) {
    if (vevSkillLevel(characterID, skillTypeID) >= level) return;
    DBQueryResult res;
    DBResultRow row;
    DBerror err;
    double rank = 1.0;
    if (sDatabase.RunQuery(res,
        "SELECT valueFloat, valueInt FROM dgmTypeAttributes "
        "WHERE typeID = %u AND attributeID = 275", skillTypeID) && res.GetRow(row)) {
        if (!row.IsNull(0))      rank = row.GetDouble(0);
        else if (!row.IsNull(1)) rank = static_cast<double>(row.GetInt(1));
    }
    const uint32_t sp = skillPointsForLevel(level, rank);
    uint32_t skillItemID = 0;
    if (!sDatabase.RunQueryLID(err, skillItemID,
        "INSERT INTO entity "
        "(itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
        "VALUES ('', %u, %u, %u, 7, 0, 0, 1, 0, 0, 0, '')",
        skillTypeID, characterID, characterID))
        return;
    sDatabase.RunQuery(err,
        "INSERT INTO entity_attributes (itemID, attributeID, valueInt, valueFloat) "
        "VALUES (%u, 280, %u, NULL), (%u, 276, %u, NULL)",
        skillItemID, static_cast<uint32_t>(level), skillItemID, sp);
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET skillPoints = skillPoints + %u WHERE characterID = %u",
        sp, characterID);
}

// The player corp this character is CEO of, or 0.
static uint32_t vevCorpIdForCeo(uint32_t characterID) {
    DBQueryResult res;
    DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT corporationID FROM crpCorporation "
        "WHERE ceoID = %u AND corporationID >= 98000000 AND deleted = 0 LIMIT 1",
        characterID) && res.GetRow(row))
        return row.GetUInt(0);
    return 0;
}

// Full corp object (incl. member roster) in the wire shape the 2D client's
// CorporationPanel consumes. viewerID drives the isCeo flag.
static json vevCorpJson(uint32_t corpID, uint32_t viewerID) {
    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT c.corporationID, c.corporationName, c.tickerName, COALESCE(c.description,''), "
        "       c.taxRate, c.ceoID, c.stationID, c.memberCount, c.memberLimit, "
        "       COALESCE(c.shape1,0), COALESCE(c.shape2,0), COALESCE(c.shape3,0), "
        "       COALESCE(c.color1,0), COALESCE(c.color2,0), COALESCE(c.color3,0) "
        "FROM crpCorporation c "
        "WHERE c.corporationID = %u", corpID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row))
        throw std::runtime_error("corp not found");

    const uint32_t ceoID = row.GetUInt(5);
    json corp = json{
        {"corpID",        row.GetUInt(0)},
        {"name",          row.GetText(1) ? std::string(row.GetText(1)) : ""},
        {"ticker",        row.GetText(2) ? std::string(row.GetText(2)) : ""},
        {"description",   row.GetText(3) ? std::string(row.GetText(3)) : ""},
        {"taxRate",       row.GetDouble(4)},
        {"ceoID",         ceoID},
        {"stationID",     row.GetUInt(6)},
        {"memberCount",   row.GetUInt(7)},
        {"memberLimit",   row.GetUInt(8)},
        {"shape1",        row.GetUInt(9)},
        {"shape2",        row.GetUInt(10)},
        {"shape3",        row.GetUInt(11)},
        {"color1",        row.GetUInt(12)},
        {"color2",        row.GetUInt(13)},
        {"color3",        row.GetUInt(14)},
        {"corpSkillLevel", vevSkillLevel(ceoID, VEV_CORP_MGMT_SKILL)},
        {"isCeo",         viewerID == ceoID},
    };

    // CEO name comes from chrCharacters (eveStaticOwners only holds corps/NPCs).
    DBQueryResult nres;
    DBResultRow nrow;
    std::string ceoName;
    if (sDatabase.RunQuery(nres,
        "SELECT characterName FROM chrCharacters WHERE characterID = %u", ceoID)
        && nres.GetRow(nrow) && nrow.GetText(0))
        ceoName = nrow.GetText(0);
    corp["ceoName"] = ceoName;

    json members = json::array();
    DBQueryResult mres;
    // V2: + wallet balance, skill points, ship typeID (for the client pip),
    // and net ISK/hr from the wallet journal (last 60 min, donations excluded
    // so CEO transfers don't pollute the earnings telemetry).
    if (sDatabase.RunQuery(mres,
        "SELECT c.characterID, c.characterName, c.solarSystemID, "
        "       COALESCE(sys.solarSystemName,''), c.stationID, c.online, "
        "       COALESCE(t.typeName,''), "
        "       (ai.characterID IS NOT NULL), "
        "       COALESCE(fm.fleetID,0), COALESCE(f.name,''), "
        "       c.balance, c.skillPoints, COALESCE(e.typeID,0), "
        "       (SELECT COALESCE(SUM(j.amount),0) FROM jnlCharacters j "
        "        WHERE j.ownerID = c.characterID AND j.entryTypeID <> 10 "
        "          AND j.transactionDate >= (UNIX_TIMESTAMP()-3600)*10000000+116444736000000000), "
        /* VEV_TRAIN_TELEMETRY: head of the training queue + live progress */
        "       (SELECT COALESCE(t2.typeName,'') FROM chrSkillQueue q "
        "        JOIN invTypes t2 ON t2.typeID = q.typeID "
        "        WHERE q.characterID = c.characterID ORDER BY q.orderIndex LIMIT 1), "
        "       (SELECT q.level FROM chrSkillQueue q "
        "        WHERE q.characterID = c.characterID ORDER BY q.orderIndex LIMIT 1), "
        "       (SELECT COALESCE(ea.valueInt,0) FROM chrSkillQueue q "
        "        JOIN entity sk ON sk.ownerID = c.characterID AND sk.typeID = q.typeID AND sk.flag = 7 "
        "        JOIN entity_attributes ea ON ea.itemID = sk.itemID AND ea.attributeID = 276 "
        "        WHERE q.characterID = c.characterID ORDER BY q.orderIndex LIMIT 1), "
        "       (SELECT COALESCE(r2.valueFloat, r2.valueInt, 1) FROM chrSkillQueue q "
        "        JOIN dgmTypeAttributes r2 ON r2.typeID = q.typeID AND r2.attributeID = 275 "
        "        WHERE q.characterID = c.characterID ORDER BY q.orderIndex LIMIT 1), "
        "       COALESCE(ai.unitID,0), COALESCE(u.name,'') "
        "FROM chrCharacters c "
        "LEFT JOIN mapSolarSystems sys ON sys.solarSystemID = c.solarSystemID "
        "LEFT JOIN entity e ON e.itemID = c.shipID "
        "LEFT JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN vevAiPilots ai ON ai.characterID = c.characterID "
        "LEFT JOIN vevFleetMembers fm ON fm.characterID = c.characterID "
        "LEFT JOIN vevFleets f ON f.fleetID = fm.fleetID "
        "LEFT JOIN vevUnits u ON u.unitID = ai.unitID "
        "WHERE c.corporationID = %u "
        "ORDER BY c.characterID", corpID))
    {
        DBResultRow mrow;
        while (mres.GetRow(mrow)) {
            members.push_back(json{
                {"characterID",     mrow.GetUInt(0)},
                {"name",            mrow.GetText(1) ? std::string(mrow.GetText(1)) : ""},
                {"solarSystemID",   mrow.GetUInt(2)},
                {"solarSystemName", mrow.GetText(3) ? std::string(mrow.GetText(3)) : ""},
                {"stationID",       mrow.GetUInt(4)},
                {"online",          mrow.GetUInt(5) != 0},
                {"shipTypeName",    mrow.GetText(6) ? std::string(mrow.GetText(6)) : ""},
                {"isAI",            mrow.GetUInt(7) != 0},
                {"fleetID",         mrow.GetUInt(8)},
                {"fleetName",       mrow.GetText(9) ? std::string(mrow.GetText(9)) : ""},
                {"isk",             mrow.GetDouble(10)},
                {"skillPoints",     mrow.GetInt64(11)},
                {"shipTypeID",      mrow.GetUInt(12)},
                {"iskPerHour",      mrow.GetDouble(13)},
                {"isCeo",           mrow.GetUInt(0) == ceoID},
                {"unitID",          mrow.GetUInt(18)},
                {"unitName",        mrow.GetText(19) ? std::string(mrow.GetText(19)) : ""},
            });
            // VEV_TRAIN_TELEMETRY: what's training + % toward the target level
            // (skillPointsForLevel boundaries; NULL row = empty queue).
            if (!mrow.IsNull(14) && mrow.GetText(14) && mrow.GetText(14)[0] != '\0') {
                const uint32_t tgtLevel = mrow.IsNull(15) ? 0 : mrow.GetUInt(15);
                const uint32_t spCur    = mrow.IsNull(16) ? 0 : mrow.GetUInt(16);
                const double   trank    = mrow.IsNull(17) ? 1.0 : (mrow.GetDouble(17) > 0 ? mrow.GetDouble(17) : 1.0);
                const uint32_t floorSp  = tgtLevel <= 1 ? 0 : skillPointsForLevel(static_cast<uint8_t>(tgtLevel - 1), trank);
                const uint32_t ceilSp   = skillPointsForLevel(static_cast<uint8_t>(tgtLevel ? tgtLevel : 1), trank);
                int pct = 0;
                if (ceilSp > floorSp) {
                    const uint32_t have = spCur < floorSp ? floorSp : (spCur > ceilSp ? ceilSp : spCur);
                    pct = static_cast<int>(100.0 * (have - floorSp) / (ceilSp - floorSp));
                }
                members.back()["trainingSkill"] = std::string(mrow.GetText(14));
                members.back()["trainingLevel"] = tgtLevel;
                members.back()["trainingPct"]   = pct;
            }
        }
    }
    // VEV_ORG: functional units (org chart) — each a skill-path team + lead.
    json units = json::array();
    {
        DBQueryResult ures; DBResultRow urow;
        if (sDatabase.RunQuery(ures,
            "SELECT u.unitID, u.name, u.kind, u.leaderCharacterID, "
            "       COALESCE(lc.characterName,''), "
            "       (SELECT COUNT(*) FROM vevAiPilots ap WHERE ap.unitID = u.unitID) "
            "FROM vevUnits u "
            "LEFT JOIN chrCharacters lc ON lc.characterID = u.leaderCharacterID "
            "WHERE u.corporationID = %u ORDER BY u.unitID", corpID)) {
            while (ures.GetRow(urow))
                units.push_back(json{
                    {"unitID",            urow.GetUInt(0)},
                    {"name",              urow.GetText(1) ? std::string(urow.GetText(1)) : ""},
                    {"kind",              urow.GetText(2) ? std::string(urow.GetText(2)) : "mining"},
                    {"leaderCharacterID", urow.GetUInt(3)},
                    {"leaderName",        urow.GetText(4) ? std::string(urow.GetText(4)) : ""},
                    {"memberCount",       urow.GetUInt(5)},
                });
        }
    }
    corp["units"] = units;
    corp["members"] = members;
    return corp;
}

// VEV_ORG/LOCATION: BFS jump distance between two solar systems over
// mapSolarSystemJumps, with a process-lifetime adjacency cache (topology is
// static). 0 = same system, -1 = unreachable/unknown. Depth-capped to bound cost.
static int vevJumpsBetween(uint32_t src, uint32_t dst) {
    if (src == 0 || dst == 0) return -1;
    if (src == dst) return 0;
    static std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    static bool loaded = false;
    if (!loaded) {
        DBQueryResult res; DBResultRow row;
        if (sDatabase.RunQuery(res,
            "SELECT fromSolarSystemID, toSolarSystemID FROM mapSolarSystemJumps")) {
            while (res.GetRow(row))
                adj[row.GetUInt(0)].push_back(row.GetUInt(1));
        }
        loaded = true;
    }
    std::unordered_map<uint32_t,int> dist; dist[src] = 0;
    std::deque<uint32_t> q; q.push_back(src);
    while (!q.empty()) {
        uint32_t u = q.front(); q.pop_front();
        int d = dist[u];
        if (d >= 50) continue;
        auto it = adj.find(u);
        if (it == adj.end()) continue;
        for (uint32_t v : it->second) {
            if (dist.find(v) == dist.end()) {
                if (v == dst) return d + 1;
                dist[v] = d + 1;
                q.push_back(v);
            }
        }
    }
    return -1;
}

// One corp fleet (vevFleets row + member roster) in the wire shape.
static json vevFleetJson(uint32_t fleetID) {
    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT f.fleetID, f.corporationID, f.name, f.purpose, f.motd, f.isAdvertised, "
        "       f.createdBy, f.createdAt, COALESCE(c.tickerName,''), COALESCE(c.corporationName,''), "
        "       COALESCE(f.standingOrder,''), f.isFreeMove, "
        "       COALESCE(f.doctrineID,0), COALESCE(f.doctrineName,''), "
        "       COALESCE(f.phase,'forming'), COALESCE(f.gateStatus,'unknown'), "
        "       COALESCE(f.gateLabel,''), COALESCE(f.hostileCount,0), "
        "       COALESCE(f.holdGo,'hold'), COALESCE(f.sitrep,''), "
        "       COALESCE(f.destinationID,0), COALESCE(f.destinationName,''), "
        "       COALESCE(f.fleetType,''), COALESCE(f.targetSize,0), COALESCE(f.costTier,'') "
        "FROM vevFleets f "
        "LEFT JOIN crpCorporation c ON c.corporationID = f.corporationID "
        "WHERE f.fleetID = %u", fleetID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row))
        throw std::runtime_error("fleet not found");

    json fleet = json{
        {"fleetID",       row.GetUInt(0)},
        {"corporationID", row.GetUInt(1)},
        {"name",          row.GetText(2) ? std::string(row.GetText(2)) : ""},
        {"purpose",       row.GetText(3) ? std::string(row.GetText(3)) : "custom"},
        {"motd",          row.GetText(4) ? std::string(row.GetText(4)) : ""},
        {"isAdvertised",  row.GetUInt(5) != 0},
        {"createdBy",     row.GetUInt(6)},
        {"createdAt",     row.GetInt64(7)},
        {"corpTicker",    row.GetText(8) ? std::string(row.GetText(8)) : ""},
        {"corpName",      row.GetText(9) ? std::string(row.GetText(9)) : ""},
        {"standingOrder", row.GetText(10) ? std::string(row.GetText(10)) : ""},
        {"isFreeMove",    row.GetUInt(11) != 0},
        {"doctrineID",    row.GetUInt(12)},
        {"doctrineName",  row.GetText(13) ? std::string(row.GetText(13)) : ""},
        {"phase",         row.GetText(14) ? std::string(row.GetText(14)) : "forming"},
        {"gateStatus",    row.GetText(15) ? std::string(row.GetText(15)) : "unknown"},
        {"gateLabel",     row.GetText(16) ? std::string(row.GetText(16)) : ""},
        {"hostileCount",  (int)row.GetInt(17)},
        {"holdGo",        row.GetText(18) ? std::string(row.GetText(18)) : "hold"},
        {"sitrep",        row.GetText(19) ? std::string(row.GetText(19)) : ""},
        {"destinationID",   row.GetUInt(20)},
        {"destinationName", row.GetText(21) ? std::string(row.GetText(21)) : ""},
        {"fleetType",     row.GetText(22) ? std::string(row.GetText(22)) : ""},
        {"targetSize",    (int)row.GetInt(23)},
        {"costTier",      row.GetText(24) ? std::string(row.GetText(24)) : ""},
    };

    // VEV_FLEET_V4: wings — the EVE hierarchy tier between fleet and squad
    // (evemu FleetData.h: fleet -> 5 wings -> 5 squads -> 10 pilots).
    json wings = json::array();
    DBQueryResult wres;
    if (sDatabase.RunQuery(wres,
        "SELECT wingID, name FROM vevWings WHERE fleetID = %u ORDER BY wingID", fleetID))
    {
        DBResultRow wrow;
        while (wres.GetRow(wrow)) {
            wings.push_back(json{
                {"wingID", wrow.GetUInt(0)},
                {"name",   wrow.GetText(1) ? std::string(wrow.GetText(1)) : ""},
            });
        }
    }
    fleet["wings"] = wings;

    // V2: squads — the fleet's sub-units, each with its own standing order
    // (VISION §11.5 scope ladder: fleet order < squad order < pilot order).
    json squads = json::array();
    DBQueryResult sres;
    if (sDatabase.RunQuery(sres,
        "SELECT squadID, name, COALESCE(standingOrder,''), wingID FROM vevSquads "
        "WHERE fleetID = %u ORDER BY squadID", fleetID))
    {
        DBResultRow srow;
        while (sres.GetRow(srow)) {
            squads.push_back(json{
                {"squadID",       srow.GetUInt(0)},
                {"name",          srow.GetText(1) ? std::string(srow.GetText(1)) : ""},
                {"standingOrder", srow.GetText(2) ? std::string(srow.GetText(2)) : ""},
                {"wingID",        srow.GetUInt(3)},
            });
        }
    }
    fleet["squads"] = squads;

    // FC system for the per-member jumps-from-FC readout (role 1 = FC).
    uint32_t fcSys = 0;
    {
        DBQueryResult fr; DBResultRow frow;
        if (sDatabase.RunQuery(fr,
            "SELECT c.solarSystemID FROM vevFleetMembers fm "
            "JOIN chrCharacters c ON c.characterID = fm.characterID "
            "WHERE fm.fleetID = %u AND fm.role = 1 LIMIT 1", fleetID) && fr.GetRow(frow))
            fcSys = frow.GetUInt(0);
    }
    json members = json::array();
    DBQueryResult mres;
    if (sDatabase.RunQuery(mres,
        "SELECT fm.characterID, COALESCE(c.characterName,''), fm.role, "
        "       (ai.characterID IS NOT NULL), COALESCE(t.typeName,''), "
        "       COALESCE(sys.solarSystemName,''), c.online, COALESCE(fm.squadID,0), "
        "       COALESCE(fm.wingID,0), COALESCE(fm.appointment,0), "
        "       COALESCE(c.stationID,0), COALESCE(st.stationName,''), "
        "       COALESCE(fm.ready,0), COALESCE(fm.readyNote,''), "
        "       COALESCE(fm.doctrineRole,''), COALESCE(c.solarSystemID,0), "
        "       COALESCE(c.securityRating,0), "
        "       (SELECT COUNT(*) FROM vevCriminalFlags cf WHERE cf.characterID=c.characterID "
        "        AND cf.expiresAt > UNIX_TIMESTAMP()) "
        "FROM vevFleetMembers fm "
        "LEFT JOIN chrCharacters c ON c.characterID = fm.characterID "
        "LEFT JOIN staStations st ON st.stationID = c.stationID "
        "LEFT JOIN mapSolarSystems sys ON sys.solarSystemID = c.solarSystemID "
        "LEFT JOIN entity e ON e.itemID = c.shipID "
        "LEFT JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN vevAiPilots ai ON ai.characterID = fm.characterID "
        "WHERE fm.fleetID = %u ORDER BY fm.role, fm.characterID", fleetID))
    {
        DBResultRow mrow;
        while (mres.GetRow(mrow)) {
            uint32_t memSys = mrow.GetUInt(15);
            int jumpsFromFc = (fcSys && memSys) ? vevJumpsBetween(fcSys, memSys) : -1;
            members.push_back(json{
                {"characterID",     mrow.GetUInt(0)},
                {"name",            mrow.GetText(1) ? std::string(mrow.GetText(1)) : ""},
                {"role",            mrow.GetUInt(2)},
                {"isAI",            mrow.GetUInt(3) != 0},
                {"shipTypeName",    mrow.GetText(4) ? std::string(mrow.GetText(4)) : ""},
                {"solarSystemName", mrow.GetText(5) ? std::string(mrow.GetText(5)) : ""},
                {"online",          mrow.GetUInt(6) != 0},
                {"squadID",         mrow.GetUInt(7)},
                {"wingID",          mrow.GetUInt(8)},
                {"appointment",     mrow.GetUInt(9)},
                {"stationID",       mrow.GetUInt(10)},
                {"stationName",     mrow.GetText(11) ? std::string(mrow.GetText(11)) : ""},
                {"ready",           mrow.GetUInt(12) != 0},
                {"readyNote",       mrow.GetText(13) ? std::string(mrow.GetText(13)) : ""},
                {"doctrineRole",    mrow.GetText(14) ? std::string(mrow.GetText(14)) : ""},
                {"solarSystemID",   memSys},
                {"jumpsFromFc",     jumpsFromFc},
                {"securityStatus", mrow.GetFloat(16)},
                {"criminal",       mrow.GetInt(17) > 0},
            });
        }
    }
    fleet["members"] = members;
    return fleet;
}


// ─── VEV_FLEET_V4 helpers ───────────────────────────────────────────────────
// EVE-real fleet hierarchy + member-side command authority (2026-06-12).
// Model mirrored from evemu src/eve-server/fleet/ (FleetData.h:15-66):
// roles FC/WC/SC/Member = 1/2/3/4, fleet->wing->squad, boss = creator,
// isFreeMove gates self-move. Caps 5 wings / 5 squads-per-wing / 10-per-squad
// incl. SC / 256 total: FleetData.h:148-157 documents them, we ENFORCE all
// four (evemu only checks the first two). DB deviations: persistent rows
// (phantom-compatible) and wingID/squadID 0 = fleet-level (evemu uses -1).
// Authorization is DELIBERATELY ADDED — evemu's FleetMustBeLeader checks are
// comment-only (any member can mutate there); real EVE gates on boss/FC.

struct VevFleetRole {
    uint32_t fleetID = 0;
    uint32_t role = 0;       // 0 = not a member
    uint32_t wingID = 0;
    uint32_t squadID = 0;
    bool isBoss = false;     // vevFleets.createdBy (evemu Job::Creator)
    bool isCeo = false;      // CEO of the owning corp = supreme commander
};

static VevFleetRole vevFleetRoleOf(uint32_t charID, uint32_t fleetID /*0 = their fleet*/) {
    VevFleetRole r;
    DBQueryResult res;
    DBResultRow row;
    if (fleetID == 0) {
        if (sDatabase.RunQuery(res,
            "SELECT fleetID FROM vevFleetMembers WHERE characterID = %u", charID)
            && res.GetRow(row))
            fleetID = row.GetUInt(0);
        if (fleetID == 0) return r;
    }
    r.fleetID = fleetID;
    if (sDatabase.RunQuery(res,
        "SELECT role, COALESCE(wingID,0), COALESCE(squadID,0) FROM vevFleetMembers "
        "WHERE fleetID = %u AND characterID = %u", fleetID, charID) && res.GetRow(row)) {
        r.role = row.GetUInt(0);
        r.wingID = row.GetUInt(1);
        r.squadID = row.GetUInt(2);
    }
    if (sDatabase.RunQuery(res,
        "SELECT createdBy, corporationID FROM vevFleets WHERE fleetID = %u", fleetID)
        && res.GetRow(row)) {
        r.isBoss = (row.GetUInt(0) == charID);
        const uint32_t corpID = row.GetUInt(1);
        if (corpID != 0) {
            DBQueryResult cres;
            DBResultRow crow;
            if (sDatabase.RunQuery(cres,
                "SELECT ceoID FROM crpCorporation WHERE corporationID = %u", corpID)
                && cres.GetRow(crow))
                r.isCeo = (crow.GetUInt(0) == charID);
        }
    }
    return r;
}

static bool vevFleetCommands(const VevFleetRole& r) {
    return r.isBoss || r.isCeo || r.role == 1;
}

// Acting character for fleet mutations — the CEO surface (ceoCharacterID:
// original V1/V2 corp-panel callers) or the fleet-command surface
// (actorCharacterID/characterID with boss/FC/CEO rights over the fleet).
static void vevRequireFleetCommand(const json& payload, uint32_t fleetID) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    if (ceoID != 0) {
        const uint32_t corpID = vevCorpIdForCeo(ceoID);
        if (corpID != 0) {
            DBQueryResult res;
            DBResultRow row;
            if (sDatabase.RunQuery(res,
                "SELECT corporationID FROM vevFleets WHERE fleetID = %u", fleetID)
                && res.GetRow(row) && row.GetUInt(0) == corpID)
                return;
        }
    }
    uint32_t actorID = payload.value("actorCharacterID", 0u);
    if (actorID == 0) actorID = payload.value("characterID", ceoID);
    if (actorID != 0 && vevFleetCommands(vevFleetRoleOf(actorID, fleetID)))
        return;
    throw std::runtime_error("fleet command requires the fleet Boss, the Fleet Commander, or the corp CEO");
}

static uint32_t vevFleetCount(uint32_t fleetID) {
    DBQueryResult res;
    DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT COUNT(*) FROM vevFleetMembers WHERE fleetID = %u", fleetID) && res.GetRow(row))
        return row.GetUInt(0);
    return 0;
}

// First squad with room (<10 incl. SC) — evemu places joiners at a RANDOM
// wing/squad (GetRandUnitIDs, with a known off-by-one); first-fit is our
// deterministic equivalent. 0/0 = fleet level when every squad is full.
static void vevAutoPlace(uint32_t fleetID, uint32_t& wingID, uint32_t& squadID) {
    wingID = 0;
    squadID = 0;
    DBQueryResult res;
    DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT s.squadID, s.wingID FROM vevSquads s "
        "LEFT JOIN vevFleetMembers m ON m.squadID = s.squadID "
        "WHERE s.fleetID = %u GROUP BY s.squadID, s.wingID "
        "HAVING COUNT(m.characterID) < 10 ORDER BY s.squadID LIMIT 1", fleetID)
        && res.GetRow(row)) {
        squadID = row.GetUInt(0);
        wingID = row.GetUInt(1);
    }
}

// Form a fresh ad-hoc fleet: Wing 1 + Squad 1 auto-created and the creator
// seated as FC at fleet level — exactly evemu CreateFleet (FleetService.cpp:
// 65-148: wing/squad pre-created, creator lands at wing -1/squad -1 as
// FleetLeader). Used by formFleet and the invite-creates-fleet path.
static uint32_t vevFormFleet(uint32_t creatorID) {
    DBQueryResult res;
    DBResultRow row;
    std::string creatorName = "Fleet";
    if (sDatabase.RunQuery(res,
        "SELECT characterName FROM chrCharacters WHERE characterID = %u", creatorID)
        && res.GetRow(row) && row.GetText(0))
        creatorName = row.GetText(0);
    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, creatorName + "'s fleet");
    DBerror err;
    uint32 fleetID = 0;
    if (!sDatabase.RunQueryLID(err, fleetID,
        "INSERT INTO vevFleets (corporationID, name, purpose, motd, isAdvertised, createdBy, createdAt) "
        "VALUES (0, '%s', 'custom', '', 0, %u, UNIX_TIMESTAMP())",
        nameEsc.c_str(), creatorID))
        throw std::runtime_error(std::string("fleet create failed: ") + err.c_str());
    uint32 wingID = 0;
    sDatabase.RunQueryLID(err, wingID,
        "INSERT INTO vevWings (fleetID, name) VALUES (%u, 'Wing 1')", fleetID);
    uint32 squadID = 0;
    sDatabase.RunQueryLID(err, squadID,
        "INSERT INTO vevSquads (fleetID, wingID, name, standingOrder) VALUES (%u, %u, 'Squad 1', '')",
        fleetID, wingID);
    sDatabase.RunQuery(err,
        "INSERT INTO vevFleetMembers (fleetID, characterID, role, wingID, squadID, joinedAt) "
        "VALUES (%u, %u, 1, 0, 0, UNIX_TIMESTAMP())", fleetID, creatorID);
    return fleetID;
}
// ─── VEV_FLEET_V4 helpers END ───────────────────────────────────────────────

// Adopt a character into a corp: corporationID + Admin-or-Member roles +
// employment record + memberCount. Mirrors Character::JoinCorporation effects.
static void vevJoinCorp(uint32_t characterID, uint32_t corpID, bool asCeo) {
    DBerror err;
    if (asCeo) {
        sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET corporationID = %u, "
            "  corpRole = %llu, rolesAtAll = %llu, rolesAtHQ = %llu, "
            "  rolesAtBase = %llu, rolesAtOther = %llu, "
            "  startDateTime = UNIX_TIMESTAMP()*10000000+116444736000000000 "
            "WHERE characterID = %u",
            corpID, VEV_CORP_ROLE_ADMIN, VEV_CORP_ROLE_ADMIN, VEV_CORP_ROLE_ADMIN,
            VEV_CORP_ROLE_ADMIN, VEV_CORP_ROLE_ADMIN, characterID);
    } else {
        sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET corporationID = %u, "
            "  corpRole = 0, rolesAtAll = 0, rolesAtHQ = 0, rolesAtBase = 0, rolesAtOther = 0, "
            "  startDateTime = UNIX_TIMESTAMP()*10000000+116444736000000000 "
            "WHERE characterID = %u",
            corpID, characterID);
    }
    sDatabase.RunQuery(err,
        "INSERT INTO chrEmployment (characterID, corporationID, startDate, deleted) "
        "VALUES (%u, %u, UNIX_TIMESTAMP()*10000000+116444736000000000, 0)",
        characterID, corpID);
    sDatabase.RunQuery(err,
        "UPDATE crpCorporation SET memberCount = memberCount + 1 WHERE corporationID = %u",
        corpID);
}

// getCorporation { characterID } → { corp: <corp|null> }. NPC corps return
// null — the panel shows the "Found a Corporation" state.
static json handleGetCorporation(const json& payload) {
    const uint32_t characterID = payload.value("characterID", 0u);
    if (characterID == 0) throw std::runtime_error("characterID required");
    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID FROM chrCharacters WHERE characterID = %u", characterID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("character not found");
    const uint32_t corpID = row.GetUInt(0);
    if (corpID < 98000000)  // NPC corp or none — no player corp to show
        return json{{"corp", nullptr}};
    return json{{"corp", vevCorpJson(corpID, characterID)}};
}

// createCorporation { characterID, name, ticker, description?, taxRate? }.
// SQL-level mirror of CorpRegistryBound::AddCorporation + CorporationDB::
// AddCorporation: fee, ticker uniqueness, memberLimit from CEO skills,
// crpCorporation + crpWalletDivisons + crpAutoPay + crpShares + eveStaticOwners,
// CEO JoinCorporation with Admin roles. v1 bootstrap: Corporation Management I
// is injected with the founding fee if untrained (no human training surface yet).
static json handleCreateCorporation(const json& payload) {
    const uint32_t characterID = payload.value("characterID", 0u);
    const std::string name     = payload.value("name", "");
    const std::string ticker   = payload.value("ticker", "");
    const std::string desc     = payload.value("description", "");
    const double taxRate       = payload.value("taxRate", 0.0);

    if (characterID == 0) throw std::runtime_error("characterID required");
    if (name.size() < 3 || name.size() > 50) throw std::runtime_error("corp name must be 3-50 characters");
    if (ticker.size() < 2 || ticker.size() > 5) throw std::runtime_error("ticker must be 2-5 characters");
    if (taxRate < 0.0 || taxRate > 1.0) throw std::runtime_error("taxRate must be 0..1");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID, balance, stationID, raceID FROM chrCharacters WHERE characterID = %u",
        characterID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("character not found");
    if (row.GetUInt(0) >= 98000000) throw std::runtime_error("already in a player corporation");
    const double balance     = row.GetDouble(1);
    const uint32_t stationID = row.GetUInt(2);
    const uint32_t raceID    = row.GetUInt(3);
    if (balance < VEV_CORP_FOUND_COST)
        throw std::runtime_error("insufficient ISK: founding a corporation costs 20,000,000 ISK");

    std::string tickEsc;
    sDatabase.DoEscapeString(tickEsc, ticker);
    if (sDatabase.RunQuery(res,
        "SELECT corporationID FROM crpCorporation WHERE tickerName = '%s' AND deleted = 0",
        tickEsc.c_str()) && res.GetRow(row))
        throw std::runtime_error("ticker already taken");

    // v1 bootstrap (corp-2d-ui-design.md §2.1): the 20M paperwork includes the
    // management course if the CEO never trained it.
    vevEnsureSkill(characterID, VEV_CORP_MGMT_SKILL, 1);
    const uint32_t memberLimit = vevCorpMemberLimit(characterID);

    std::string nameEsc, descEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    sDatabase.DoEscapeString(descEsc, desc);

    // V2: 3-layer emblem (EVE's real logo model — backdrop/pattern/symbol,
    // each shape+color) stored in crpCorporation's own shape/color columns.
    const uint32_t shape1 = payload.value("shape1", 0u), shape2 = payload.value("shape2", 0u), shape3 = payload.value("shape3", 0u);
    const uint32_t color1 = payload.value("color1", 0u), color2 = payload.value("color2", 0u), color3 = payload.value("color3", 0u);

    DBerror err;
    uint32 corpID = 0;
    if (!sDatabase.RunQueryLID(err, corpID,
        "INSERT INTO crpCorporation "
        "(corporationName, description, tickerName, url, taxRate, corporationType, hasPlayerPersonnelManager, "
        " creatorID, ceoID, stationID, raceID, shares, memberCount, memberLimit, allowedMemberRaceIDs, "
        " graphicID, isRecruiting, allianceMemberStartDate, "
        " shape1, shape2, shape3, color1, color2, color3) "
        "VALUES ('%s', '%s', '%s', '', %f, 2, 1, %u, %u, %u, %u, 1000, 0, %u, %u, 0, 0, 0, "
        " %u, %u, %u, %u, %u, %u)",
        nameEsc.c_str(), descEsc.c_str(), tickEsc.c_str(), taxRate,
        characterID, characterID, stationID, raceID, memberLimit, raceID,
        shape1, shape2, shape3, color1, color2, color3))
        throw std::runtime_error(std::string("corp insert failed: ") + err.c_str());

    // Satellite rows AddCorporation also writes (failures non-fatal upstream too).
    sDatabase.RunQuery(err, "INSERT INTO crpWalletDivisons (corporationID) VALUES (%u)", corpID);
    sDatabase.RunQuery(err, "INSERT INTO crpAutoPay (corporationID) VALUES (%u)", corpID);
    sDatabase.RunQuery(err,
        "INSERT INTO crpShares (corporationID, shareholderID, shares, shareholderCorporationID) "
        "VALUES (%u, %u, 1000, %u)", corpID, corpID, corpID);
    sDatabase.RunQuery(err,
        "INSERT INTO eveStaticOwners (ownerID, ownerName, typeID) VALUES (%u, '%s', 2)",
        corpID, nameEsc.c_str());

    vevJoinCorp(characterID, corpID, true);
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET balance = balance - %f WHERE characterID = %u",
        VEV_CORP_FOUND_COST, characterID);

    return json{{"corp", vevCorpJson(corpID, characterID)}};
}

// requisitionPilot { ceoCharacterID, name, race, bloodline, gender } — the
// VISION §5 "1.0 requisition form". Gates: caller is CEO, roster slot free
// (evemu's own memberLimit formula), fee covered. Reuses the full
// createCharacter pipeline, then adopts + registers in vevAiPilots.
static json handleRequisitionPilot(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    if (ceoID == 0) throw std::runtime_error("ceoCharacterID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT memberCount, memberLimit FROM crpCorporation WHERE corporationID = %u", corpID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("corp not found");
    if (row.GetUInt(0) >= row.GetUInt(1))
        throw std::runtime_error("member limit reached — train Corporation Management for more roster slots");

    if (!sDatabase.RunQuery(res,
        "SELECT balance FROM chrCharacters WHERE characterID = %u", ceoID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("CEO character not found");
    if (row.GetDouble(0) < VEV_REQUISITION_COST)
        throw std::runtime_error("insufficient ISK: requisitioning a pilot costs 250,000 ISK");

    // Full creation pipeline — race-native starter station, starter skills,
    // rookie ship. Throws on bad race/bloodline/name; nothing charged yet.
    json character = handleCreateCharacter(json{
        {"name",      payload.value("name", "")},
        {"race",      payload.value("race", "")},
        {"bloodline", payload.value("bloodline", "")},
        {"gender",    payload.value("gender", "male")},
    });
    const uint32_t newCharID = character.value("characterID", 0u);
    if (newCharID == 0) throw std::runtime_error("character creation failed");

    // V2: career skill package (Military / Business / Industry) — evemu's own
    // sklCareerSkills, layered on top of the base+race seed.
    const std::string career = payload.value("career", "industry");
    vevSeedCareerSkills(newCharID, raceIdForString(payload.value("race", "")), career);

    vevJoinCorp(newCharID, corpID, false);
    DBerror err;
    sDatabase.RunQuery(err,
        "INSERT INTO vevAiPilots (characterID, corporationID, requisitionedBy, requisitionedAt) "
        "VALUES (%u, %u, %u, UNIX_TIMESTAMP())", newCharID, corpID, ceoID);
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET balance = balance - %f WHERE characterID = %u",
        VEV_REQUISITION_COST, ceoID);

    return json{{"character", character}, {"corp", vevCorpJson(corpID, ceoID)}};
}

// listAdoptablePilots {} → corp-less characters (corporationID = 0 — the
// legacy probe/test chars predating corp assignment).
static json handleListAdoptablePilots(const json& payload) {
    (void)payload;
    DBQueryResult res;
    json pilots = json::array();
    if (sDatabase.RunQuery(res,
        "SELECT c.characterID, c.characterName, COALESCE(sys.solarSystemName,''), "
        "       (ai.characterID IS NOT NULL) "
        "FROM chrCharacters c "
        "LEFT JOIN mapSolarSystems sys ON sys.solarSystemID = c.solarSystemID "
        "LEFT JOIN vevAiPilots ai ON ai.characterID = c.characterID "
        "WHERE c.corporationID = 0 ORDER BY c.characterID"))
    {
        DBResultRow row;
        while (res.GetRow(row)) {
            pilots.push_back(json{
                {"characterID",     row.GetUInt(0)},
                {"name",            row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"solarSystemName", row.GetText(2) ? std::string(row.GetText(2)) : ""},
                {"isAI",            row.GetUInt(3) != 0},
            });
        }
    }
    return json{{"pilots", pilots}};
}

// adoptPilot { ceoCharacterID, targetCharacterID } — adopt an existing
// corp-less/NPC-corp character into the CEO's corp as an AI pilot.
static json handleAdoptPilot(const json& payload) {
    const uint32_t ceoID    = payload.value("ceoCharacterID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    if (ceoID == 0 || targetID == 0) throw std::runtime_error("ceoCharacterID and targetCharacterID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID FROM chrCharacters WHERE characterID = %u", targetID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("target character not found");
    if (row.GetUInt(0) >= 98000000) throw std::runtime_error("target is already in a player corporation");

    if (!sDatabase.RunQuery(res,
        "SELECT memberCount, memberLimit FROM crpCorporation WHERE corporationID = %u", corpID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("corp not found");
    if (row.GetUInt(0) >= row.GetUInt(1))
        throw std::runtime_error("member limit reached — train Corporation Management for more roster slots");

    vevJoinCorp(targetID, corpID, false);
    DBerror err;
    sDatabase.RunQuery(err,
        "INSERT IGNORE INTO vevAiPilots (characterID, corporationID, requisitionedBy, requisitionedAt) "
        "VALUES (%u, %u, %u, UNIX_TIMESTAMP())", targetID, corpID, ceoID);
    return json{{"corp", vevCorpJson(corpID, ceoID)}};
}

// VEV_FLEET_REDESIGN — role -> the org-unit `kind` whose pilots prefer this seat.
static std::string vevRoleCareerKind(const std::string& role) {
    if (role == "miner")    return "mining";
    if (role == "logi")     return "logistics";
    if (role == "hauler")   return "hauling";
    if (role == "salvager") return "industry";
    if (role == "scout")    return "exploration";
    if (role == "ewar")     return "recon";
    if (role == "anchor")   return "command";
    return "combat";   // dps, tackle
}

// canFly(charID, hullTypeID): does the pilot meet ALL the hull's required-skill
// prereqs? Mirrors the prereq read (dgmTypeAttributes 182/277...) + the trained-
// level read (entity flag=7 + entity_attributes attr 280). hull 0 = no constraint.
static bool vevCanFly(uint32 charID, uint32 hullTypeID) {
    if (hullTypeID == 0) return true;
    int sk[6] = {0,0,0,0,0,0};
    int lv[6] = {0,0,0,0,0,0};
    DBQueryResult pres;
    if (sDatabase.RunQuery(pres,
        "SELECT attributeID, IF(valueInt IS NULL, valueFloat, valueInt) AS value "
        "FROM dgmTypeAttributes WHERE typeID = %u "
        "AND attributeID IN (182,183,184,1285,1289,1290,277,278,279,1286,1287,1288)", hullTypeID))
    {
        DBResultRow prow;
        while (pres.GetRow(prow)) {
            int v = static_cast<int>(prow.GetDouble(1));
            switch (prow.GetUInt(0)) {
                case 182:  sk[0]=v; break;  case 277:  lv[0]=v; break;
                case 183:  sk[1]=v; break;  case 278:  lv[1]=v; break;
                case 184:  sk[2]=v; break;  case 279:  lv[2]=v; break;
                case 1285: sk[3]=v; break;  case 1286: lv[3]=v; break;
                case 1289: sk[4]=v; break;  case 1287: lv[4]=v; break;
                case 1290: sk[5]=v; break;  case 1288: lv[5]=v; break;
            }
        }
    }
    for (int i = 0; i < 6; ++i) {
        if (sk[i] == 0) continue;
        const int need = (lv[i] > 0) ? lv[i] : 1;
        int have = 0;
        DBQueryResult lres;
        DBResultRow lrow;
        if (sDatabase.RunQuery(lres,
            "SELECT COALESCE(a.valueInt, 0) FROM entity e "
            "JOIN entity_attributes a ON a.itemID = e.itemID AND a.attributeID = 280 "
            "WHERE e.ownerID = %u AND e.typeID = %u AND e.flag = 7 LIMIT 1",
            charID, (uint32)sk[i]) && lres.GetRow(lrow))
            have = (int)lrow.GetInt(0);
        if (have < need) return false;
    }
    return true;
}

// canFlyAny: the pilot can fly at least one candidate hull (a ship CLASS's racial
// variants). Empty list = no constraint. VEV_FLEET_COMP_V2.
static bool vevCanFlyAny(uint32 charID, const std::vector<uint32>& hulls) {
    if (hulls.empty()) return true;
    for (uint32 h : hulls) if (vevCanFly(charID, h)) return true;
    return false;
}

// autofillFleet { ceoCharacterID, fleetID, comp:[{role,candidates[],count}] } — seat
// the corp's unassigned AI pilots into the deterministic composition. Pass 1 prefers
// pilots whose org-unit kind matches the role's career; pass 2 takes any pilot who
// canFly the role hull. First pilot seated becomes FC (role 1). Returns
// { seated, shortfall:[{role,need}] }.
static json handleAutofillFleet(const json& payload) {
    const uint32_t ceoID   = payload.value("ceoCharacterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (ceoID == 0 || fleetID == 0) throw std::runtime_error("ceoCharacterID + fleetID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    {
        DBQueryResult fr; DBResultRow frow;
        if (!sDatabase.RunQuery(fr, "SELECT corporationID FROM vevFleets WHERE fleetID = %u", fleetID)
            || !fr.GetRow(frow) || frow.GetUInt(0) != corpID)
            throw std::runtime_error("fleet not in your corporation");
    }
    if (!payload.contains("comp") || !payload["comp"].is_array())
        throw std::runtime_error("comp array required");

    struct Cand { uint32 charID; std::string kind; bool used; };
    std::vector<Cand> pool;
    {
        DBQueryResult pr; DBResultRow prow;
        if (sDatabase.RunQuery(pr,
            "SELECT ai.characterID, COALESCE(u.kind,'') FROM vevAiPilots ai "
            "LEFT JOIN vevUnits u ON u.unitID = ai.unitID "
            "LEFT JOIN vevFleetMembers fm ON fm.characterID = ai.characterID "
            "WHERE ai.corporationID = %u AND fm.characterID IS NULL", corpID))
        {
            while (pr.GetRow(prow))
                pool.push_back(Cand{ prow.GetUInt(0), prow.GetText(1) ? std::string(prow.GetText(1)) : "", false });
        }
    }

    uint32 wingID = 0, squadID = 0;
    {
        DBQueryResult wr; DBResultRow wrow;
        if (sDatabase.RunQuery(wr, "SELECT wingID FROM vevWings WHERE fleetID = %u ORDER BY wingID LIMIT 1", fleetID) && wr.GetRow(wrow))
            wingID = wrow.GetUInt(0);
        DBQueryResult sr; DBResultRow srow;
        if (sDatabase.RunQuery(sr, "SELECT squadID FROM vevSquads WHERE fleetID = %u ORDER BY squadID LIMIT 1", fleetID) && sr.GetRow(srow))
            squadID = srow.GetUInt(0);
    }

    int seated = 0;
    bool firstSeat = true;
    json shortfall = json::array();
    for (const auto& rj : payload["comp"]) {
        const std::string role = rj.value("role", "");
        // v2: candidates = the role's ship CLASS (racial variants); a pilot can fly
        // the role if they can fly ANY of them. Legacy single hullTypeID still works.
        std::vector<uint32> cands;
        if (rj.contains("candidates") && rj["candidates"].is_array())
            for (const auto& cv : rj["candidates"]) cands.push_back(cv.get<uint32>());
        if (cands.empty() && rj.value("hullTypeID", 0u) != 0u)
            cands.push_back(rj.value("hullTypeID", 0u));
        int need = rj.value("count", 0);
        const std::string kind = vevRoleCareerKind(role);
        for (int pass = 0; pass < 4 && need > 0; ++pass) {
            for (auto& c : pool) {
                if (need <= 0) break;
                if (c.used) continue;
                // 0: kind+canFly  ·  1: kind (downgrade)  ·  2: canFly (any kind)  ·  3: anyone
                if ((pass == 0 || pass == 1) && c.kind != kind) continue;
                if ((pass == 0 || pass == 2) && !vevCanFlyAny(c.charID, cands)) continue;
                DBerror err;
                sDatabase.RunQuery(err, "DELETE FROM vevFleetMembers WHERE characterID = %u", c.charID);
                const uint32 seatRole = firstSeat ? 1u : 4u;
                sDatabase.RunQuery(err,
                    "INSERT INTO vevFleetMembers (fleetID, characterID, role, wingID, squadID, joinedAt) "
                    "VALUES (%u, %u, %u, %u, %u, UNIX_TIMESTAMP())",
                    fleetID, c.charID, seatRole, wingID, squadID);
                std::string roleEsc;
                sDatabase.DoEscapeString(roleEsc, role);
                sDatabase.RunQuery(err,
                    "UPDATE vevFleetMembers SET doctrineRole='%s' WHERE fleetID=%u AND characterID=%u",
                    roleEsc.c_str(), fleetID, c.charID);
                c.used = true;
                firstSeat = false;
                --need; ++seated;
            }
        }
        if (need > 0)
            shortfall.push_back(json{{"role", role}, {"need", need}});
    }
    return json{{"seated", seated}, {"shortfall", shortfall}};
}

// createCorpFleet { ceoCharacterID, name, purpose?, motd? } — a persistent
// corp fleet (org-chart layer; in-space coordination stays Plan-Fleets W2-W4).
static json handleCreateCorpFleet(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    const std::string name = payload.value("name", "");
    std::string purpose = payload.value("purpose", "custom");
    const std::string motd = payload.value("motd", "");
    // VEV_FLEET_REDESIGN — the three creation choices + the FC's seed order.
    const std::string fleetType = payload.value("fleetType", "");
    const int targetSize = payload.value("targetSize", 0);
    const std::string costTier = payload.value("costTier", "");
    const std::string standingOrder = payload.value("standingOrder", "");
    if (ceoID == 0) throw std::runtime_error("ceoCharacterID required");
    if (name.size() < 2 || name.size() > 60) throw std::runtime_error("fleet name must be 2-60 characters");
    if (purpose != "mining" && purpose != "combat" && purpose != "hauling" &&
        purpose != "patrol" && purpose != "custom")
        purpose = "custom";
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");

    std::string nameEsc, motdEsc, ftEsc, ctEsc, soEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    sDatabase.DoEscapeString(motdEsc, motd);
    sDatabase.DoEscapeString(ftEsc, fleetType);
    sDatabase.DoEscapeString(ctEsc, costTier);
    sDatabase.DoEscapeString(soEsc, standingOrder);
    DBerror err;
    uint32 fleetID = 0;
    if (!sDatabase.RunQueryLID(err, fleetID,
        "INSERT INTO vevFleets (corporationID, name, purpose, fleetType, targetSize, costTier, standingOrder, motd, isAdvertised, createdBy, createdAt) "
        "VALUES (%u, '%s', '%s', '%s', %d, '%s', '%s', '%s', 0, %u, UNIX_TIMESTAMP())",
        corpID, nameEsc.c_str(), purpose.c_str(), ftEsc.c_str(), targetSize, ctEsc.c_str(), soEsc.c_str(), motdEsc.c_str(), ceoID))
        throw std::runtime_error(std::string("fleet insert failed: ") + err.c_str());
    // V4: EVE auto-creates Wing 1 / Squad 1 on fleet formation (FleetService.cpp:91-114).
    uint32 wingID = 0;
    sDatabase.RunQueryLID(err, wingID,
        "INSERT INTO vevWings (fleetID, name) VALUES (%u, 'Wing 1')", fleetID);
    uint32 squadID = 0;
    sDatabase.RunQueryLID(err, squadID,
        "INSERT INTO vevSquads (fleetID, wingID, name, standingOrder) VALUES (%u, %u, 'Squad 1', '')",
        fleetID, wingID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// setFleetComposition { ceoCharacterID, fleetID, fleetType?, targetSize?, costTier? } —
// VEV_FLEET_COMPEDIT: update a fleet's persisted creation choices AFTER creation. The
// composition recomputes from these (client data/fleet-compositions.ts); autofill seats to plan.
static json handleSetFleetComposition(const json& payload) {
    const uint32_t ceoID   = payload.value("ceoCharacterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (ceoID == 0 || fleetID == 0) throw std::runtime_error("ceoCharacterID + fleetID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    {
        DBQueryResult fr; DBResultRow frow;
        if (!sDatabase.RunQuery(fr, "SELECT corporationID FROM vevFleets WHERE fleetID = %u", fleetID)
            || !fr.GetRow(frow) || frow.GetUInt(0) != corpID)
            throw std::runtime_error("fleet not in your corporation");
    }
    const std::string fleetType = payload.value("fleetType", "");
    const int targetSize = payload.value("targetSize", 0);
    const std::string costTier = payload.value("costTier", "");
    std::string ftEsc, ctEsc;
    sDatabase.DoEscapeString(ftEsc, fleetType);
    sDatabase.DoEscapeString(ctEsc, costTier);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleets SET fleetType='%s', targetSize=%d, costTier='%s' WHERE fleetID=%u",
        ftEsc.c_str(), targetSize, ctEsc.c_str(), fleetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// disbandCorpFleet { ceoCharacterID, fleetID }
static json handleDisbandCorpFleet(const json& payload) {
    const uint32_t ceoID   = payload.value("ceoCharacterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (ceoID == 0 || fleetID == 0) throw std::runtime_error("ceoCharacterID and fleetID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    DBerror err;
    sDatabase.RunQuery(err, "DELETE FROM vevFleetMembers WHERE fleetID = %u", fleetID);
    sDatabase.RunQuery(err, "DELETE FROM vevFleets WHERE fleetID = %u AND corporationID = %u", fleetID, corpID);
    return json{{"ok", true}};
}

// setFleetMembership { ceoCharacterID, fleetID, characterID, join, role? } —
// CEO assigns/removes corp members. Joining moves the pilot if already in
// another fleet (a pilot is in at most one fleet).
static json handleSetFleetMembership(const json& payload) {
    const uint32_t ceoID   = payload.value("ceoCharacterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const uint32_t charID  = payload.value("characterID", 0u);
    const bool join        = payload.value("join", false);
    uint32_t role          = payload.value("role", 4u);
    if (role != 1) role = 4;
    if (ceoID == 0 || fleetID == 0 || charID == 0)
        throw std::runtime_error("ceoCharacterID, fleetID, characterID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID FROM vevFleets WHERE fleetID = %u", fleetID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row) || row.GetUInt(0) != corpID)
        throw std::runtime_error("fleet not found in your corporation");
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID FROM chrCharacters WHERE characterID = %u", charID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row) || row.GetUInt(0) != corpID)
        throw std::runtime_error("pilot is not a member of your corporation");

    DBerror err;
    sDatabase.RunQuery(err, "DELETE FROM vevFleetMembers WHERE characterID = %u", charID);
    if (join) {
        sDatabase.RunQuery(err,
            "INSERT INTO vevFleetMembers (fleetID, characterID, role, joinedAt) "
            "VALUES (%u, %u, %u, UNIX_TIMESTAMP())", fleetID, charID, role);
    }
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// setFleetAdvertised { ceoCharacterID, fleetID, advertised } — what surfaces
// a corp fleet in the Fleet Finder.
static json handleSetFleetAdvertised(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const bool advertised  = payload.value("advertised", false);
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    vevRequireFleetCommand(payload, fleetID);   // V4b: boss/FC may also toggle
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleets SET isAdvertised = %u WHERE fleetID = %u",
        advertised ? 1 : 0, fleetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// listCorpFleets { characterID } → every fleet of the viewer's corp.
static json handleListCorpFleets(const json& payload) {
    const uint32_t characterID = payload.value("characterID", 0u);
    if (characterID == 0) throw std::runtime_error("characterID required");
    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID FROM chrCharacters WHERE characterID = %u", characterID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("character not found");
    const uint32_t corpID = row.GetUInt(0);

    json ids = json::array();
    if (corpID >= 98000000 && sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevFleets WHERE corporationID = %u ORDER BY fleetID", corpID))
    {
        while (res.GetRow(row)) ids.push_back(row.GetUInt(0));
    }
    // VEV_FLEET_INVITE: plus any fleet the viewer is a member of (ad-hoc
    // fleets live outside the corp; the FleetPanel finds MyFleet through this).
    if (sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevFleetMembers WHERE characterID = %u", characterID))
    {
        while (res.GetRow(row)) {
            const uint32_t fid = row.GetUInt(0);
            bool dup = false;
            for (const auto& v : ids) if (v.get<uint32_t>() == fid) { dup = true; break; }
            if (!dup) ids.push_back(fid);
        }
    }
    json fleets = json::array();
    for (const auto& id : ids) fleets.push_back(vevFleetJson(id.get<uint32_t>()));
    return json{{"fleets", fleets}};
}

// listFleetAdverts { characterID } → advertised fleets (the Finder read).
// v1 single-player: all advertised fleets are the player corp's; the shape
// already carries corpTicker/corpName for the multi-corp era.
static json handleListFleetAdverts(const json& payload) {
    (void)payload;
    DBQueryResult res;
    DBResultRow row;
    json ids = json::array();
    if (sDatabase.RunQuery(res, "SELECT fleetID FROM vevFleets WHERE isAdvertised = 1 ORDER BY fleetID")) {
        while (res.GetRow(row)) ids.push_back(row.GetUInt(0));
    }
    json fleets = json::array();
    for (const auto& id : ids) fleets.push_back(vevFleetJson(id.get<uint32_t>()));
    return json{{"fleets", fleets}};
}

// joinFleet { characterID, fleetID } — self-serve join from the Finder (the
// human CEO joining their corp's advertised fleet). First joiner with no FC
// present becomes FC.
static json handleJoinFleet(const json& payload) {
    const uint32_t charID  = payload.value("characterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (charID == 0 || fleetID == 0) throw std::runtime_error("characterID and fleetID required");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT f.corporationID, c.corporationID FROM vevFleets f, chrCharacters c "
        "WHERE f.fleetID = %u AND c.characterID = %u", fleetID, charID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("fleet or character not found");
    // VEV_FLEET_INVITE: ad-hoc fleets (corporationID 0) are open to anyone;
    // corp fleets stay corp-only.
    if (row.GetUInt(0) != 0 && row.GetUInt(0) != row.GetUInt(1))
        throw std::runtime_error("fleet is corp-only — you are not a member of that corporation");

    uint32_t role = 4;
    if (!(sDatabase.RunQuery(res,
        "SELECT characterID FROM vevFleetMembers WHERE fleetID = %u AND role = 1", fleetID)
        && res.GetRow(row)))
        role = 1;  // no FC yet — joiner takes command

    // V4: 256-pilot cap (FleetData.h:148-157) + auto-place into an open squad.
    if (vevFleetCount(fleetID) >= 256)
        throw std::runtime_error("that fleet is full (256 pilots, EVE cap)");
    uint32_t wingID = 0, squadID = 0;
    if (role == 4) vevAutoPlace(fleetID, wingID, squadID);
    DBerror err;
    sDatabase.RunQuery(err, "DELETE FROM vevFleetMembers WHERE characterID = %u", charID);
    sDatabase.RunQuery(err,
        "INSERT INTO vevFleetMembers (fleetID, characterID, role, wingID, squadID, joinedAt) "
        "VALUES (%u, %u, %u, %u, %u, UNIX_TIMESTAMP())", fleetID, charID, role, wingID, squadID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// leaveFleet { characterID }
static json handleLeaveFleet(const json& payload) {
    const uint32_t charID = payload.value("characterID", 0u);
    if (charID == 0) throw std::runtime_error("characterID required");
    DBerror err;
    sDatabase.RunQuery(err, "DELETE FROM vevFleetMembers WHERE characterID = %u", charID);
    return json{{"ok", true}};
}

// ─── V2 handlers (2026-06-11, second staging round) ────────────────────────

// setCorpLogo { ceoCharacterID, shape1..3, color1..3 } — edit the emblem of
// an existing corp (DXG was founded before the logo designer existed).
static json handleSetCorpLogo(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    if (ceoID == 0) throw std::runtime_error("ceoCharacterID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE crpCorporation SET shape1=%u, shape2=%u, shape3=%u, color1=%u, color2=%u, color3=%u "
        "WHERE corporationID = %u",
        payload.value("shape1", 0u), payload.value("shape2", 0u), payload.value("shape3", 0u),
        payload.value("color1", 0u), payload.value("color2", 0u), payload.value("color3", 0u),
        corpID);
    return json{{"corp", vevCorpJson(corpID, ceoID)}};
}

// transferIsk { ceoCharacterID, targetCharacterID, amount, direction, reason? }
// direction 'give' = CEO→member (EVE-real "Transfer ISK" with reason string);
// direction 'take' = member→CEO — a NAMED DEVIATION from EVE (a CEO can never
// touch a member's personal wallet there), justified because all members are
// corp-requisitioned AI pilots. Take is therefore restricted to vevAiPilots.
// Both sides get journal rows (entryTypeID 10 Player Donation — excluded from
// the ISK/hr earnings telemetry by design).
static json handleTransferIsk(const json& payload) {
    const uint32_t ceoID    = payload.value("ceoCharacterID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    const double amount     = payload.value("amount", 0.0);
    const std::string direction = payload.value("direction", "give");
    std::string reason = payload.value("reason", "");
    if (ceoID == 0 || targetID == 0) throw std::runtime_error("ceoCharacterID and targetCharacterID required");
    if (!(amount > 0)) throw std::runtime_error("amount must be > 0");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT c.corporationID, c.balance, (ai.characterID IS NOT NULL) "
        "FROM chrCharacters c LEFT JOIN vevAiPilots ai ON ai.characterID = c.characterID "
        "WHERE c.characterID = %u", targetID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("target character not found");
    if (row.GetUInt(0) != corpID) throw std::runtime_error("pilot is not a member of your corporation");
    const double targetBalance = row.GetDouble(1);
    const bool targetIsAI = row.GetUInt(2) != 0;

    if (!sDatabase.RunQuery(res,
        "SELECT balance FROM chrCharacters WHERE characterID = %u", ceoID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("CEO character not found");
    const double ceoBalance = row.GetDouble(0);

    uint32_t fromID = ceoID, toID = targetID;
    if (direction == "take") {
        if (!targetIsAI) throw std::runtime_error("can only withdraw from requisitioned AI pilots");
        if (targetBalance < amount) throw std::runtime_error("pilot has insufficient ISK");
        fromID = targetID; toID = ceoID;
    } else {
        if (ceoBalance < amount) throw std::runtime_error("insufficient ISK");
    }
    if (reason.empty()) reason = (direction == "take") ? "Corp wallet withdrawal" : "Corp payroll";
    std::string reasonEsc;
    sDatabase.DoEscapeString(reasonEsc, reason);

    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET balance = balance - %f WHERE characterID = %u", amount, fromID);
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET balance = balance + %f WHERE characterID = %u", amount, toID);
    // Journal rows, one per wallet perspective (ownerID = whose wallet).
    sDatabase.RunQuery(err,
        "INSERT INTO jnlCharacters (ownerID, entryTypeID, referenceID, ownerID1, ownerID2, "
        " transactionDate, accountKey, currency, amount, balance, description) "
        "SELECT %u, 10, 0, %u, %u, UNIX_TIMESTAMP()*10000000+116444736000000000, 1000, 1, %f, balance, 'DESC: %s' "
        "FROM chrCharacters WHERE characterID = %u",
        fromID, fromID, toID, -amount, reasonEsc.c_str(), fromID);
    sDatabase.RunQuery(err,
        "INSERT INTO jnlCharacters (ownerID, entryTypeID, referenceID, ownerID1, ownerID2, "
        " transactionDate, accountKey, currency, amount, balance, description) "
        "SELECT %u, 10, 0, %u, %u, UNIX_TIMESTAMP()*10000000+116444736000000000, 1000, 1, %f, balance, 'DESC: %s' "
        "FROM chrCharacters WHERE characterID = %u",
        toID, fromID, toID, amount, reasonEsc.c_str(), toID);

    return json{{"corp", vevCorpJson(corpID, ceoID)}};
}

// listPilotSkills { characterID } — the pilot's trained skills with the cost
// of the NEXT level (v0 trade_isk_for_sp pricing: base × rank × level²), for
// the Members-tab training-purchase modal.
static json handleListPilotSkills(const json& payload) {
    const uint32_t charID = payload.value("characterID", 0u);
    if (charID == 0) throw std::runtime_error("characterID required");
    DBQueryResult res;
    DBResultRow row;
    json skills = json::array();
    if (sDatabase.RunQuery(res,
        "SELECT e.typeID, COALESCE(t.typeName,''), COALESCE(lvl.valueInt,0), COALESCE(sp.valueInt,0), "
        "       COALESCE(r.valueFloat, r.valueInt, 1) "
        "FROM entity e "
        "LEFT JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN entity_attributes lvl ON lvl.itemID = e.itemID AND lvl.attributeID = 280 "
        "LEFT JOIN entity_attributes sp  ON sp.itemID  = e.itemID AND sp.attributeID  = 276 "
        "LEFT JOIN dgmTypeAttributes r ON r.typeID = e.typeID AND r.attributeID = 275 "
        "WHERE e.ownerID = %u AND e.flag = 7 "
        "ORDER BY t.typeName", charID))
    {
        while (res.GetRow(row)) {
            const uint32_t level = row.GetUInt(2);
            const double rank = row.GetDouble(4) > 0 ? row.GetDouble(4) : 1.0;
            const uint32_t nextLevel = level + 1;
            // VEV_SP_AWARE_PRICE: the purchase only buys the MISSING SP toward
            // the next level — a half-trained level costs half. frac in [0,1].
            const uint32_t spCur   = row.GetUInt(3);
            const uint32_t floorSp = level == 0 ? 0 : skillPointsForLevel(static_cast<uint8_t>(level), rank);
            const uint32_t ceilSp  = skillPointsForLevel(static_cast<uint8_t>(nextLevel > 5 ? 5 : nextLevel), rank);
            double frac = 1.0;
            if (ceilSp > floorSp) {
                const uint32_t have = spCur < floorSp ? floorSp : (spCur > ceilSp ? ceilSp : spCur);
                frac = static_cast<double>(ceilSp - have) / static_cast<double>(ceilSp - floorSp);
            }
            skills.push_back(json{
                {"skillTypeID",   row.GetUInt(0)},
                {"name",          row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"level",         level},
                {"sp",            spCur},
                {"rank",          rank},
                {"maxed",         level >= 5},
                {"nextLevelCost", level >= 5 ? 0.0 : VEV_SP_BASE_COST * rank * nextLevel * nextLevel * frac},
                {"pctToNext",     static_cast<int>((1.0 - frac) * 100.0)},
            });
        }
    }
    return json{{"skills", skills}};
}

// buySkillLevel { ceoCharacterID, targetCharacterID, skillTypeID } — port of
// v0's trade_isk_for_sp (A321 §3.4: skill injectors don't exist in Crucible;
// directors convert ISK→SP instead). The CEO pays. Lessons carried from v0:
// SP attribute is 276 NOT 277 (A323.1 Lesson #9); the pilot must be OFFLINE
// (an online char's in-memory sheet overwrites DB skill edits on save).
static json handleBuySkillLevel(const json& payload) {
    const uint32_t ceoID    = payload.value("ceoCharacterID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    const uint32_t skillTypeID = payload.value("skillTypeID", 0u);
    if (ceoID == 0 || targetID == 0 || skillTypeID == 0)
        throw std::runtime_error("ceoCharacterID, targetCharacterID, skillTypeID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID, online FROM chrCharacters WHERE characterID = %u", targetID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("target character not found");
    if (row.GetUInt(0) != corpID) throw std::runtime_error("pilot is not a member of your corporation");
    if (row.GetUInt(1) != 0) {
        // VEV_ONLINE_INJECT: the offline rule exists because a CLIENT-connected
        // pilot's in-memory sheet overwrites DB skill edits on save. AI pilots
        // have no Client* — their skills are DB-authoritative (the training
        // engine writes SP while they're online, production-proven) — so
        // online injection is safe for them. Non-AI keeps the offline rule.
        DBQueryResult aiq;
        DBResultRow airow;
        const bool isAI = sDatabase.RunQuery(aiq,
            "SELECT 1 FROM vevAiPilots WHERE characterID = %u", targetID) && aiq.GetRow(airow);
        if (!isAI)
            throw std::runtime_error("pilot must be offline for training injection (an online pilot's sheet would overwrite it)");
    }

    // must be a real skill (category 16)
    if (!sDatabase.RunQuery(res,
        "SELECT g.categoryID FROM invTypes t JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE t.typeID = %u", skillTypeID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row) || row.GetUInt(0) != 16)
        throw std::runtime_error("typeID is not a skill");

    double rank = 1.0;
    if (sDatabase.RunQuery(res,
        "SELECT valueFloat, valueInt FROM dgmTypeAttributes WHERE typeID = %u AND attributeID = 275",
        skillTypeID) && res.GetRow(row)) {
        if (!row.IsNull(0))      rank = row.GetDouble(0);
        else if (!row.IsNull(1)) rank = static_cast<double>(row.GetInt(1));
    }

    uint32_t curLevel = vevSkillLevel(targetID, skillTypeID);
    const uint32_t nextLevel = curLevel + 1;
    if (nextLevel > 5) throw std::runtime_error("skill is already at level V");
    // VEV_SP_AWARE_PRICE: charge only for the MISSING SP toward the next level
    // (mirrors listPilotSkills so the quoted price IS the charged price).
    uint32_t spCur = 0;
    {
        DBQueryResult spq;
        DBResultRow sprow;
        if (sDatabase.RunQuery(spq,
            "SELECT COALESCE(ea.valueInt,0) FROM entity sk "
            "JOIN entity_attributes ea ON ea.itemID = sk.itemID AND ea.attributeID = 276 "
            "WHERE sk.ownerID = %u AND sk.typeID = %u AND sk.flag = 7 LIMIT 1",
            targetID, skillTypeID) && spq.GetRow(sprow))
            spCur = sprow.GetUInt(0);
    }
    const uint32_t floorSp = curLevel == 0 ? 0 : skillPointsForLevel(static_cast<uint8_t>(curLevel), rank);
    const uint32_t ceilSp  = skillPointsForLevel(static_cast<uint8_t>(nextLevel), rank);
    double frac = 1.0;
    if (ceilSp > floorSp) {
        const uint32_t have = spCur < floorSp ? floorSp : (spCur > ceilSp ? ceilSp : spCur);
        frac = static_cast<double>(ceilSp - have) / static_cast<double>(ceilSp - floorSp);
    }
    const double cost = VEV_SP_BASE_COST * rank * nextLevel * nextLevel * frac;

    if (!sDatabase.RunQuery(res,
        "SELECT balance FROM chrCharacters WHERE characterID = %u", ceoID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row) || row.GetDouble(0) < cost)
        throw std::runtime_error("insufficient ISK for this training purchase");

    vevGrantSkill(targetID, skillTypeID, static_cast<uint8_t>(nextLevel));

    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET balance = balance - %f WHERE characterID = %u", cost, ceoID);
    sDatabase.RunQuery(err,
        "INSERT INTO jnlCharacters (ownerID, entryTypeID, referenceID, ownerID1, ownerID2, "
        " transactionDate, accountKey, currency, amount, balance, description) "
        "SELECT %u, 10, 0, %u, %u, UNIX_TIMESTAMP()*10000000+116444736000000000, 1000, 1, %f, balance, "
        " 'DESC: SP training purchase (typeID %u L%u)' "
        "FROM chrCharacters WHERE characterID = %u",
        ceoID, ceoID, targetID, -cost, skillTypeID, nextLevel, ceoID);
    // skill history (v0 idiom: eventTypeID 307 = GM/direct training)
    sDatabase.RunQuery(err,
        "INSERT INTO chrSkillHistory (eventTypeID, characterID, logDate, skillTypeID, skillLevel, absolutePoints) "
        "VALUES (307, %u, UNIX_TIMESTAMP()*10000000+116444736000000000, %u, %u, %u)",
        targetID, skillTypeID, nextLevel, skillPointsForLevel(static_cast<uint8_t>(nextLevel), rank));

    return json{
        {"skillTypeID", skillTypeID}, {"newLevel", nextLevel}, {"cost", cost},
        {"corp", vevCorpJson(corpID, ceoID)},
    };
}

// listCareers { race } — the race's three career skill packages
// (sklCareerSkills) for the requisition form's career picker.
static json handleListCareers(const json& payload) {
    const uint32_t raceID = raceIdForString(payload.value("race", ""));
    if (raceID == 0) throw std::runtime_error("race must be one of: amarr, caldari, gallente, minmatar");
    DBQueryResult res;
    DBResultRow row;
    json careers = json::array();
    static const struct { const char* key; uint32_t offset; } CAREERS[] = {
        {"military", 1}, {"business", 4}, {"industry", 7},
    };
    for (const auto& c : CAREERS) {
        json skills = json::array();
        uint64_t totalSp = 0;
        if (sDatabase.RunQuery(res,
            "SELECT COALESCE(t.typeName,''), cs.level, "
            "       COALESCE(r.valueFloat, r.valueInt, 1) "
            "FROM sklCareerSkills cs "
            "LEFT JOIN invTypes t ON t.typeID = cs.skillTypeID "
            "LEFT JOIN dgmTypeAttributes r ON r.typeID = cs.skillTypeID AND r.attributeID = 275 "
            "WHERE cs.careerID = %u ORDER BY t.typeName", raceID * 10 + c.offset))
        {
            while (res.GetRow(row)) {
                const double rank = row.GetDouble(2) > 0 ? row.GetDouble(2) : 1.0;
                const uint32_t sp = skillPointsForLevel(static_cast<uint8_t>(row.GetUInt(1)), rank);
                totalSp += sp;
                skills.push_back(json{
                    {"name",  row.GetText(0) ? std::string(row.GetText(0)) : ""},
                    {"level", row.GetUInt(1)},
                    {"sp",    sp},
                });
            }
        }
        careers.push_back(json{{"career", c.key}, {"skills", skills}, {"totalSp", totalSp}});
    }
    return json{{"careers", careers}};
}

// createSquad { ceoCharacterID|actorCharacterID, fleetID, name, wingID? } —
// V4: boss/FC may also create. Squads live under a wing (default: the wing
// with the fewest squads; Wing 1 created for pre-V4 fleets). Cap mirrors EVE:
// 5 squads per wing (evemu FleetService.cpp:185-189).
static json handleCreateSquad(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const std::string name = payload.value("name", "");
    uint32_t wingID        = payload.value("wingID", 0u);
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    if (name.size() < 2 || name.size() > 60) throw std::runtime_error("squad name must be 2-60 characters");
    vevRequireFleetCommand(payload, fleetID);
    DBQueryResult res;
    DBResultRow row;
    if (wingID == 0) {
        if (sDatabase.RunQuery(res,
            "SELECT w.wingID FROM vevWings w LEFT JOIN vevSquads s ON s.wingID = w.wingID "
            "WHERE w.fleetID = %u GROUP BY w.wingID ORDER BY COUNT(s.squadID), w.wingID LIMIT 1",
            fleetID) && res.GetRow(row))
            wingID = row.GetUInt(0);
        else {
            DBerror werr;
            uint32 newWing = 0;
            sDatabase.RunQueryLID(werr, newWing,
                "INSERT INTO vevWings (fleetID, name) VALUES (%u, 'Wing 1')", fleetID);
            wingID = newWing;
        }
    } else if (!(sDatabase.RunQuery(res,
        "SELECT wingID FROM vevWings WHERE wingID = %u AND fleetID = %u", wingID, fleetID)
        && res.GetRow(row)))
        throw std::runtime_error("wing not found in that fleet");
    if (sDatabase.RunQuery(res,
        "SELECT COUNT(*) FROM vevSquads WHERE wingID = %u", wingID)
        && res.GetRow(row) && row.GetUInt(0) >= 5)
        throw std::runtime_error("that wing already has 5 squads (EVE cap)");
    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    DBerror err;
    uint32 squadID = 0;
    if (!sDatabase.RunQueryLID(err, squadID,
        "INSERT INTO vevSquads (fleetID, wingID, name, standingOrder) VALUES (%u, %u, '%s', '')",
        fleetID, wingID, nameEsc.c_str()))
        throw std::runtime_error(std::string("squad insert failed: ") + err.c_str());
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// deleteSquad { ceoCharacterID|actorCharacterID, squadID } — members fall to
// fleet level; the SC is demoted (the slot is positional, not personal).
static json handleDeleteSquad(const json& payload) {
    const uint32_t squadID = payload.value("squadID", 0u);
    if (squadID == 0) throw std::runtime_error("squadID required");
    DBQueryResult res;
    DBResultRow row;
    if (!(sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevSquads WHERE squadID = %u", squadID) && res.GetRow(row)))
        throw std::runtime_error("squad not found");
    const uint32_t fleetID = row.GetUInt(0);
    vevRequireFleetCommand(payload, fleetID);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET squadID = 0, wingID = 0, role = IF(role = 3, 4, role) "
        "WHERE squadID = %u", squadID);
    sDatabase.RunQuery(err, "DELETE FROM vevSquads WHERE squadID = %u", squadID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// assignToSquad { ceoCharacterID, fleetID, characterID, squadID } — squadID 0
// = fleet-level (unsquadded). Drives the click-drag reorganize UI.
static json handleAssignToSquad(const json& payload) {
    const uint32_t ceoID   = payload.value("ceoCharacterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const uint32_t charID  = payload.value("characterID", 0u);
    const uint32_t squadID = payload.value("squadID", 0u);
    if (ceoID == 0 || fleetID == 0 || charID == 0)
        throw std::runtime_error("ceoCharacterID, fleetID, characterID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    DBQueryResult res;
    DBResultRow row;
    if (squadID != 0) {
        if (!sDatabase.RunQuery(res,
            "SELECT squadID FROM vevSquads WHERE squadID = %u AND fleetID = %u", squadID, fleetID))
            throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
        if (!res.GetRow(row)) throw std::runtime_error("squad not found in that fleet");
    }
    // V4: the FC sits at fleet level (EVE: wing -1/squad -1) — transfer
    // command first; WC/SC moving squads lose their positional star.
    if (sDatabase.RunQuery(res,
        "SELECT role FROM vevFleetMembers WHERE fleetID = %u AND characterID = %u", fleetID, charID)
        && res.GetRow(row) && row.GetUInt(0) == 1 && squadID != 0)
        throw std::runtime_error("the Fleet Commander sits at fleet level — transfer command first");
    if (squadID != 0 && sDatabase.RunQuery(res,
        "SELECT COUNT(*) FROM vevFleetMembers WHERE squadID = %u AND characterID <> %u",
        squadID, charID) && res.GetRow(row) && row.GetUInt(0) >= 10)
        throw std::runtime_error("that squad is full (10 pilots, EVE cap)");
    uint32_t wingID = 0;
    if (squadID != 0 && sDatabase.RunQuery(res,
        "SELECT wingID FROM vevSquads WHERE squadID = %u", squadID) && res.GetRow(row))
        wingID = row.GetUInt(0);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET squadID = %u, wingID = %u, role = IF(role IN (2,3), 4, role) "
        "WHERE fleetID = %u AND characterID = %u",
        squadID, wingID, fleetID, charID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// setFleetOrder { ceoCharacterID, scope: 'fleet'|'squad', id, text } — the
// scoped standing-order text (VISION §11.5 ladder). The canonical scoped text
// lives here; the client fans it out into per-pilot journal orders so the
// cognition engine sees it (fleet < squad < pilot priority).
static json handleSetFleetOrder(const json& payload) {
    const std::string scope = payload.value("scope", "");
    const uint32_t id = payload.value("id", 0u);
    const std::string text = payload.value("text", "");
    if (id == 0) throw std::runtime_error("id required");
    if (text.size() > 1000) throw std::runtime_error("order text too long (max 1000)");
    std::string textEsc;
    sDatabase.DoEscapeString(textEsc, text);
    DBerror err;
    uint32_t fleetID = id;
    if (scope == "fleet") {
        vevRequireFleetCommand(payload, fleetID);   // V4: boss/FC may also command
        sDatabase.RunQuery(err,
            "UPDATE vevFleets SET standingOrder = '%s' WHERE fleetID = %u",
            textEsc.c_str(), id);
    } else if (scope == "squad") {
        DBQueryResult res;
        DBResultRow row;
        if (!(sDatabase.RunQuery(res,
            "SELECT fleetID FROM vevSquads WHERE squadID = %u", id) && res.GetRow(row)))
            throw std::runtime_error("squad not found");
        fleetID = row.GetUInt(0);
        vevRequireFleetCommand(payload, fleetID);   // V4: boss/FC may also command
        sDatabase.RunQuery(err,
            "UPDATE vevSquads SET standingOrder = '%s' WHERE squadID = %u", textEsc.c_str(), id);
    } else {
        throw std::runtime_error("scope must be 'fleet' or 'squad'");
    }
    return json{{"fleet", vevFleetJson(fleetID)}};
}
// ─── VEV_CORP_V1 END ────────────────────────────────────────────────────────

// ─── VEV_CORP_V3 BEGIN ──────────────────────────────────────────────────────
// Certificate paths for the Training surface (corp-2d-ui-design.md §7,
// curator round 3, 2026-06-12). The cognition engine already auto-claims
// certificates (engine/training.py); this exposes the cert tree to the 2D
// client and lets the CEO pin one as a pilot's TRAINING GOAL. The engine
// contract: a vevTrainingGoals row = "work this certificate's skill
// requirements to completion, then resume normal queue self-selection"
// (engine integration is the wire-AI-pilot convo's lane).

// listCertificates { characterID } — the full cert tree (crtCategories →
// crtClasses → grade rows) with earned flags, per-grade skill requirements
// (crtRelationships parentTypeID/parentLevel) and the pilot's current goal.
static json handleListCertificates(const json& payload) {
    const uint32_t characterID = payload.value("characterID", 0u);
    if (characterID == 0) throw std::runtime_error("characterID required");

    DBQueryResult res;
    DBResultRow row;

    // Earned set.
    json earnedArr = json::array();
    if (sDatabase.RunQuery(res,
        "SELECT certificateID FROM chrCertificates WHERE characterID = %u", characterID)) {
        while (res.GetRow(row)) earnedArr.push_back(row.GetUInt(0));
    }

    // Requirements per cert: skill rows (parentTypeID+parentLevel) and cert
    // prerequisites (parentID). Keyed client-side by certificateID.
    json reqs = json::object();
    if (sDatabase.RunQuery(res,
        "SELECT r.childID, COALESCE(r.parentTypeID,0), COALESCE(r.parentLevel,0), "
        "       COALESCE(t.typeName,''), COALESCE(r.parentID,0) "
        "FROM crtRelationships r "
        "LEFT JOIN invTypes t ON t.typeID = r.parentTypeID "
        "WHERE r.childID IS NOT NULL"))
    {
        while (res.GetRow(row)) {
            const std::string key = std::to_string(row.GetUInt(0));
            if (!reqs.contains(key)) reqs[key] = json{{"skills", json::array()}, {"certs", json::array()}};
            if (row.GetUInt(1) != 0) {
                reqs[key]["skills"].push_back(json{
                    {"skillTypeID", row.GetUInt(1)},
                    {"level",       row.GetUInt(2)},
                    {"name",        row.GetText(3) ? std::string(row.GetText(3)) : ""},
                });
            } else if (row.GetUInt(4) != 0) {
                reqs[key]["certs"].push_back(row.GetUInt(4));
            }
        }
    }

    json categories = json::array();
    if (sDatabase.RunQuery(res,
        "SELECT categoryID, COALESCE(categoryName,'') FROM crtCategories ORDER BY categoryID")) {
        while (res.GetRow(row)) {
            categories.push_back(json{
                {"categoryID", row.GetUInt(0)},
                {"name",       row.GetText(1) ? std::string(row.GetText(1)) : ""},
            });
        }
    }

    // Classes with their grade rows (a class = one cert lineage, grades 1-5).
    json classes = json::array();
    if (sDatabase.RunQuery(res,
        "SELECT c.classID, COALESCE(cl.className,''), c.categoryID, "
        "       c.certificateID, COALESCE(c.grade,0), COALESCE(c.description,'') "
        "FROM crtCertificates c "
        "LEFT JOIN crtClasses cl ON cl.classID = c.classID "
        "ORDER BY c.categoryID, c.classID, c.grade"))
    {
        json* cur = nullptr;
        uint32_t curClass = 0;
        while (res.GetRow(row)) {
            const uint32_t classID = row.GetUInt(0);
            if (cur == nullptr || classID != curClass) {
                classes.push_back(json{
                    {"classID",    classID},
                    {"className",  row.GetText(1) ? std::string(row.GetText(1)) : ""},
                    {"categoryID", row.GetUInt(2)},
                    {"grades",     json::array()},
                });
                cur = &classes.back();
                curClass = classID;
            }
            (*cur)["grades"].push_back(json{
                {"certificateID", row.GetUInt(3)},
                {"grade",         row.GetUInt(4)},
                {"description",   row.GetText(5) ? std::string(row.GetText(5)) : ""},
            });
        }
    }

    uint32_t goal = 0;
    if (sDatabase.RunQuery(res,
        "SELECT certificateID FROM vevTrainingGoals WHERE characterID = %u", characterID)
        && res.GetRow(row))
        goal = row.GetUInt(0);

    return json{
        {"categories", categories},
        {"classes", classes},
        {"requirements", reqs},
        {"earned", earnedArr},
        {"goalCertificateID", goal},
    };
}

// setTrainingGoal { ceoCharacterID, targetCharacterID, certificateID } —
// certificateID 0 clears the goal (back to gemma's own queue selection).
static json handleSetTrainingGoal(const json& payload) {
    const uint32_t ceoID    = payload.value("ceoCharacterID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    const uint32_t certID   = payload.value("certificateID", 0u);
    if (ceoID == 0 || targetID == 0)
        throw std::runtime_error("ceoCharacterID and targetCharacterID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT corporationID FROM chrCharacters WHERE characterID = %u", targetID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row) || row.GetUInt(0) != corpID)
        throw std::runtime_error("pilot is not a member of your corporation");

    DBerror err;
    if (certID == 0) {
        sDatabase.RunQuery(err, "DELETE FROM vevTrainingGoals WHERE characterID = %u", targetID);
    } else {
        if (!sDatabase.RunQuery(res,
            "SELECT certificateID FROM crtCertificates WHERE certificateID = %u", certID))
            throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
        if (!res.GetRow(row)) throw std::runtime_error("unknown certificateID");
        sDatabase.RunQuery(err,
            "INSERT INTO vevTrainingGoals (characterID, certificateID, setBy, setAt) "
            "VALUES (%u, %u, %u, UNIX_TIMESTAMP()) "
            "ON DUPLICATE KEY UPDATE certificateID = %u, setBy = %u, setAt = UNIX_TIMESTAMP()",
            targetID, certID, ceoID, certID, ceoID);
    }
    return json{{"ok", true}, {"goalCertificateID", certID}};
}
// ─── VEV_CORP_V3 END ────────────────────────────────────────────────────────

// VEV_LOOT_WRECK: a wreck's lootable contents (entity rows inside the wreck)
// for the client's wreck-cargo window. Pure DB read — phantom-safe.
static json handleGetWreckContents(const json& payload) {
    const uint32_t wreckID = payload.value("wreckID", 0u);
    if (wreckID == 0) throw std::runtime_error("wreckID required");
    DBQueryResult res;
    DBResultRow row;
    json items = json::array();
    if (sDatabase.RunQuery(res,
        "SELECT e.itemID, e.typeID, e.quantity, COALESCE(t.typeName,''), COALESCE(t.volume,0) "
        "FROM entity e LEFT JOIN invTypes t ON t.typeID = e.typeID "
        "WHERE e.locationID = %u", wreckID))
    {
        while (res.GetRow(row)) {
            items.push_back(json{
                {"itemID",   row.GetUInt(0)},
                {"typeID",   row.GetUInt(1)},
                {"quantity", row.GetUInt(2)},
                {"typeName", row.GetText(3) ? std::string(row.GetText(3)) : ""},
                {"volume",   row.GetDouble(4)},
            });
        }
    }
    return json{{"items", items}};
}

// VEV_FLEET_INVITE: right-click Invite to Fleet. Ad-hoc, EVE-real: if the
// inviter has no fleet, one is created on demand (corporationID 0, inviter
// = FC); the target joins as Member (moved if already in another fleet).
// v1 auto-accepts (the invite dialog is a follow-up).
static json handleInviteToFleet(const json& payload) {
    const uint32_t inviterID = payload.value("characterID", 0u);
    const uint32_t targetID  = payload.value("targetCharacterID", 0u);
    if (inviterID == 0 || targetID == 0)
        throw std::runtime_error("characterID and targetCharacterID required");
    if (inviterID == targetID)
        throw std::runtime_error("cannot invite yourself");

    DBQueryResult res;
    DBResultRow row;
    if (!sDatabase.RunQuery(res,
        "SELECT characterID FROM chrCharacters WHERE characterID = %u", targetID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    if (!res.GetRow(row)) throw std::runtime_error("target character not found");

    uint32_t fleetID = 0;
    if (sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevFleetMembers WHERE characterID = %u", inviterID)
        && res.GetRow(row))
        fleetID = row.GetUInt(0);

    DBerror err;
    if (fleetID == 0)
        fleetID = vevFormFleet(inviterID);   // V4: Wing 1 + Squad 1 + inviter FC
    if (vevFleetCount(fleetID) >= 256)
        throw std::runtime_error("your fleet is full (256 pilots, EVE cap)");
    uint32_t wingID = 0, squadID = 0;
    vevAutoPlace(fleetID, wingID, squadID);
    sDatabase.RunQuery(err, "DELETE FROM vevFleetMembers WHERE characterID = %u", targetID);
    sDatabase.RunQuery(err,
        "INSERT INTO vevFleetMembers (fleetID, characterID, role, wingID, squadID, joinedAt) "
        "VALUES (%u, %u, 4, %u, %u, UNIX_TIMESTAMP())", fleetID, targetID, wingID, squadID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}


// ─── VEV_FLEET_V4 handlers ──────────────────────────────────────────────────

// formFleet { characterID, name? } — EVE "Form Fleet": you become FC/boss of a
// fresh ad-hoc fleet with Wing 1 / Squad 1 pre-created.
static json handleFormFleet(const json& payload) {
    const uint32_t charID = payload.value("characterID", 0u);
    if (charID == 0) throw std::runtime_error("characterID required");
    DBQueryResult res;
    DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevFleetMembers WHERE characterID = %u", charID) && res.GetRow(row))
        throw std::runtime_error("you are already in a fleet — leave it first");
    const uint32_t fleetID = vevFormFleet(charID);
    const std::string name = payload.value("name", "");
    if (name.size() >= 2 && name.size() <= 60) {
        std::string nameEsc;
        sDatabase.DoEscapeString(nameEsc, name);
        DBerror err;
        sDatabase.RunQuery(err,
            "UPDATE vevFleets SET name = '%s' WHERE fleetID = %u", nameEsc.c_str(), fleetID);
    }
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// createWing { characterID, fleetID } — auto-named "Wing N"; max 5 per fleet
// (evemu FleetService.cpp:150-155).
static json handleCreateWing(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    vevRequireFleetCommand(payload, fleetID);
    DBQueryResult res;
    DBResultRow row;
    uint32_t count = 0;
    if (sDatabase.RunQuery(res,
        "SELECT COUNT(*) FROM vevWings WHERE fleetID = %u", fleetID) && res.GetRow(row))
        count = row.GetUInt(0);
    if (count >= 5) throw std::runtime_error("a fleet holds at most 5 wings (EVE cap)");
    DBerror err;
    uint32 wingID = 0;
    sDatabase.RunQueryLID(err, wingID,
        "INSERT INTO vevWings (fleetID, name) VALUES (%u, 'Wing %u')", fleetID, count + 1);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// renameWing { characterID, wingID, name }
static json handleRenameWing(const json& payload) {
    const uint32_t wingID = payload.value("wingID", 0u);
    const std::string name = payload.value("name", "");
    if (wingID == 0) throw std::runtime_error("wingID required");
    if (name.size() < 2 || name.size() > 60) throw std::runtime_error("wing name must be 2-60 characters");
    DBQueryResult res;
    DBResultRow row;
    if (!(sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevWings WHERE wingID = %u", wingID) && res.GetRow(row)))
        throw std::runtime_error("wing not found");
    const uint32_t fleetID = row.GetUInt(0);
    vevRequireFleetCommand(payload, fleetID);
    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevWings SET name = '%s' WHERE wingID = %u", nameEsc.c_str(), wingID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// renameSquad { characterID, squadID, name }
static json handleRenameSquad(const json& payload) {
    const uint32_t squadID = payload.value("squadID", 0u);
    const std::string name = payload.value("name", "");
    if (squadID == 0) throw std::runtime_error("squadID required");
    if (name.size() < 2 || name.size() > 60) throw std::runtime_error("squad name must be 2-60 characters");
    DBQueryResult res;
    DBResultRow row;
    if (!(sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevSquads WHERE squadID = %u", squadID) && res.GetRow(row)))
        throw std::runtime_error("squad not found");
    const uint32_t fleetID = row.GetUInt(0);
    vevRequireFleetCommand(payload, fleetID);
    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevSquads SET name = '%s' WHERE squadID = %u", nameEsc.c_str(), squadID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// deleteWing { characterID, wingID } — child squads deleted; their members,
// the SCs and the WC fall to fleet level (evemu DeleteWing erases child
// squads too, FleetService.cpp:1015-1038 — but strands their members; ours
// reseats them).
static json handleDeleteWing(const json& payload) {
    const uint32_t wingID = payload.value("wingID", 0u);
    if (wingID == 0) throw std::runtime_error("wingID required");
    DBQueryResult res;
    DBResultRow row;
    if (!(sDatabase.RunQuery(res,
        "SELECT fleetID FROM vevWings WHERE wingID = %u", wingID) && res.GetRow(row)))
        throw std::runtime_error("wing not found");
    const uint32_t fleetID = row.GetUInt(0);
    vevRequireFleetCommand(payload, fleetID);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET squadID = 0, wingID = 0, role = IF(role IN (2,3), 4, role) "
        "WHERE wingID = %u", wingID);
    sDatabase.RunQuery(err, "DELETE FROM vevSquads WHERE wingID = %u", wingID);
    sDatabase.RunQuery(err, "DELETE FROM vevWings WHERE wingID = %u", wingID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// moveFleetMember { characterID, fleetID, targetCharacterID, wingID?, squadID?, role? }
// EVE MoveMember/UpdateMember (evemu FleetService.cpp:379-554) WITH the
// authorization evemu left comment-only (FleetMustBeLeader): boss/FC/CEO move
// anyone; a WC seats SC/members within their own wing; a member moves THEMSELF
// when free-move is on (no self-promotion). Position rules mirror AddMember
// (FleetService.cpp:285-345): each commander slot must be vacant; the FC sits
// at fleet level; squads cap at 10 incl. the SC.
static json handleMoveFleetMember(const json& payload) {
    const uint32_t actorID  = payload.value("characterID", 0u);
    const uint32_t fleetID  = payload.value("fleetID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    uint32_t wingID  = payload.value("wingID", 0u);
    uint32_t squadID = payload.value("squadID", 0u);
    uint32_t role    = payload.value("role", 4u);
    if (actorID == 0 || fleetID == 0 || targetID == 0)
        throw std::runtime_error("characterID, fleetID, targetCharacterID required");
    if (role < 1 || role > 4) role = 4;

    const VevFleetRole actor = vevFleetRoleOf(actorID, fleetID);
    const VevFleetRole tgt   = vevFleetRoleOf(targetID, fleetID);
    if (tgt.role == 0) throw std::runtime_error("that pilot is not in this fleet");

    DBQueryResult res;
    DBResultRow row;
    if (squadID != 0) {   // squad implies wing
        if (!(sDatabase.RunQuery(res,
            "SELECT wingID FROM vevSquads WHERE squadID = %u AND fleetID = %u", squadID, fleetID)
            && res.GetRow(row)))
            throw std::runtime_error("squad not found in that fleet");
        wingID = row.GetUInt(0);
    } else if (wingID != 0) {
        if (!(sDatabase.RunQuery(res,
            "SELECT wingID FROM vevWings WHERE wingID = %u AND fleetID = %u", wingID, fleetID)
            && res.GetRow(row)))
            throw std::runtime_error("wing not found in that fleet");
    }

    bool allowed = vevFleetCommands(actor);
    if (!allowed && actor.role == 2)   // WC: within their wing, SC/member seats only
        allowed = (tgt.wingID == actor.wingID && wingID == actor.wingID && role >= 3);
    if (!allowed && actorID == targetID && role == 4) {
        if (sDatabase.RunQuery(res,
            "SELECT isFreeMove FROM vevFleets WHERE fleetID = %u", fleetID) && res.GetRow(row))
            allowed = row.GetUInt(0) != 0;   // free-move self-seating
    }
    if (!allowed) throw std::runtime_error("you are not authorized to move that pilot");

    if (role == 1) {
        wingID = 0; squadID = 0;
        if (sDatabase.RunQuery(res,
            "SELECT characterID FROM vevFleetMembers WHERE fleetID = %u AND role = 1 AND characterID <> %u",
            fleetID, targetID) && res.GetRow(row))
            throw std::runtime_error("the Fleet Commander slot is filled — transfer command instead");
    } else if (role == 2) {
        if (wingID == 0) throw std::runtime_error("a Wing Commander needs a wing");
        squadID = 0;
        if (sDatabase.RunQuery(res,
            "SELECT characterID FROM vevFleetMembers WHERE wingID = %u AND squadID = 0 AND role = 2 AND characterID <> %u",
            wingID, targetID) && res.GetRow(row))
            throw std::runtime_error("that wing already has a Wing Commander");
    } else if (role == 3) {
        if (squadID == 0) throw std::runtime_error("a Squad Commander needs a squad");
        if (sDatabase.RunQuery(res,
            "SELECT characterID FROM vevFleetMembers WHERE squadID = %u AND role = 3 AND characterID <> %u",
            squadID, targetID) && res.GetRow(row))
            throw std::runtime_error("that squad already has a Squad Commander");
    }
    if (squadID != 0 && sDatabase.RunQuery(res,
        "SELECT COUNT(*) FROM vevFleetMembers WHERE squadID = %u AND characterID <> %u",
        squadID, targetID) && res.GetRow(row) && row.GetUInt(0) >= 10)
        throw std::runtime_error("that squad is full (10 pilots, EVE cap)");

    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET wingID = %u, squadID = %u, role = %u "
        "WHERE fleetID = %u AND characterID = %u",
        wingID, squadID, role, fleetID, targetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// makeFleetLeader { characterID, fleetID, targetCharacterID } — FC transfer
// (evemu MakeLeader, FleetBound.cpp:550-576). Boss/CEO or the sitting FC may
// hand over; the old FC steps down to member at an open squad (evemu sends
// them to a RANDOM unit — we first-fit).
static json handleMakeFleetLeader(const json& payload) {
    const uint32_t actorID  = payload.value("characterID", 0u);
    const uint32_t fleetID  = payload.value("fleetID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    if (actorID == 0 || fleetID == 0 || targetID == 0)
        throw std::runtime_error("characterID, fleetID, targetCharacterID required");
    const VevFleetRole actor = vevFleetRoleOf(actorID, fleetID);
    if (!vevFleetCommands(actor))
        throw std::runtime_error("only the Boss, the CEO or the sitting Fleet Commander can transfer command");
    const VevFleetRole tgt = vevFleetRoleOf(targetID, fleetID);
    if (tgt.role == 0) throw std::runtime_error("that pilot is not in this fleet");
    if (tgt.role == 1) return json{{"fleet", vevFleetJson(fleetID)}};   // already FC
    DBerror err;
    uint32_t wingID = 0, squadID = 0;
    vevAutoPlace(fleetID, wingID, squadID);
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET role = 4, wingID = %u, squadID = %u "
        "WHERE fleetID = %u AND role = 1", wingID, squadID, fleetID);
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET role = 1, wingID = 0, squadID = 0 "
        "WHERE fleetID = %u AND characterID = %u", fleetID, targetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// kickFleetMember { characterID, fleetID, targetCharacterID } — boss/FC/CEO;
// the boss is unkickable (EVE FleetCantKickBoss — message-only in evemu,
// enforced here).
// disbandFleet { characterID, fleetID } — tear the whole fleet down. Authorized
// for the Boss, the Fleet Commander, or the CEO (same authority as kick). Works by
// fleetID so it disbands orphaned fleets (corporationID 0) the corp-scoped
// disbandCorpFleet cannot. VEV_FLEET_TEARDOWN.
static json handleDisbandFleet(const json& payload) {
    const uint32_t actorID = payload.value("characterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (actorID == 0 || fleetID == 0)
        throw std::runtime_error("characterID and fleetID required");
    const VevFleetRole actor = vevFleetRoleOf(actorID, fleetID);
    if (!vevFleetCommands(actor))
        throw std::runtime_error("only the Boss, the Fleet Commander or the CEO can disband the fleet");
    DBerror err;
    sDatabase.RunQuery(err, "DELETE FROM vevFleetMembers WHERE fleetID = %u", fleetID);
    sDatabase.RunQuery(err, "DELETE FROM vevSquads WHERE fleetID = %u", fleetID);
    sDatabase.RunQuery(err, "DELETE FROM vevWings WHERE fleetID = %u", fleetID);
    sDatabase.RunQuery(err, "DELETE FROM vevFleets WHERE fleetID = %u", fleetID);
    return json{{"ok", true}};
}

static json handleKickFleetMember(const json& payload) {
    const uint32_t actorID  = payload.value("characterID", 0u);
    const uint32_t fleetID  = payload.value("fleetID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    if (actorID == 0 || fleetID == 0 || targetID == 0)
        throw std::runtime_error("characterID, fleetID, targetCharacterID required");
    const VevFleetRole actor = vevFleetRoleOf(actorID, fleetID);
    if (!vevFleetCommands(actor))
        throw std::runtime_error("only the Boss, the Fleet Commander or the CEO can kick");
    DBQueryResult res;
    DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT createdBy FROM vevFleets WHERE fleetID = %u", fleetID)
        && res.GetRow(row) && row.GetUInt(0) == targetID)
        throw std::runtime_error("the fleet boss cannot be kicked");
    DBerror err;
    sDatabase.RunQuery(err,
        "DELETE FROM vevFleetMembers WHERE fleetID = %u AND characterID = %u", fleetID, targetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// setFleetMotd { characterID, fleetID, motd } — boss/FC/CEO (evemu SetMOTD).
static json handleSetFleetMotd(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const std::string motd = payload.value("motd", "");
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    if (motd.size() > 400) throw std::runtime_error("MOTD too long (max 400)");
    vevRequireFleetCommand(payload, fleetID);
    std::string motdEsc;
    sDatabase.DoEscapeString(motdEsc, motd);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleets SET motd = '%s' WHERE fleetID = %u", motdEsc.c_str(), fleetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// setFleetFreeMove { characterID, fleetID, isFreeMove } — boss/CEO only (the
// EVE boss option; evemu SetOptions stores it, the client gates on it — ours
// is gated server-side in moveFleetMember).
static json handleSetFleetFreeMove(const json& payload) {
    const uint32_t charID  = payload.value("characterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const bool freeMove    = payload.value("isFreeMove", true);
    if (charID == 0 || fleetID == 0) throw std::runtime_error("characterID and fleetID required");
    const VevFleetRole r = vevFleetRoleOf(charID, fleetID);
    if (!r.isBoss && !r.isCeo)
        throw std::runtime_error("only the fleet boss (or the CEO) sets fleet options");
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleets SET isFreeMove = %u WHERE fleetID = %u", freeMove ? 1 : 0, fleetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// renameFleet { characterID, fleetID, name } — boss/CEO.
static json handleRenameFleet(const json& payload) {
    const uint32_t charID  = payload.value("characterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const std::string name = payload.value("name", "");
    if (charID == 0 || fleetID == 0) throw std::runtime_error("characterID and fleetID required");
    if (name.size() < 2 || name.size() > 60) throw std::runtime_error("fleet name must be 2-60 characters");
    const VevFleetRole r = vevFleetRoleOf(charID, fleetID);
    if (!r.isBoss && !r.isCeo)
        throw std::runtime_error("only the fleet boss (or the CEO) renames the fleet");
    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleets SET name = '%s' WHERE fleetID = %u", nameEsc.c_str(), fleetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// sendFleetBroadcast { characterID, name, group } — group: 1=Down 2=Up 3=All
// (evemu Fleet::BCast::Group). Sender role/wing/squad are snapshotted so the
// recipient set is computed EVE-real at read time. An FC broadcasting Up is
// refused (evemu FleetService.cpp:1424-1428).
static json handleSendFleetBroadcast(const json& payload) {
    const uint32_t charID = payload.value("characterID", 0u);
    const std::string name = payload.value("name", "");
    uint32_t group = payload.value("group", 3u);
    if (charID == 0) throw std::runtime_error("characterID required");
    if (name.empty() || name.size() > 32) throw std::runtime_error("broadcast name must be 1-32 characters");
    if (group < 1 || group > 3) group = 3;
    const VevFleetRole r = vevFleetRoleOf(charID, 0);
    if (r.fleetID == 0) throw std::runtime_error("you are not in a fleet");
    if (group == 2 && r.role == 1)
        throw std::runtime_error("you cannot broadcast to Superiors as the Fleet Commander");
    DBQueryResult res;
    DBResultRow row;
    uint32_t solarSystemID = 0;
    if (sDatabase.RunQuery(res,
        "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", charID) && res.GetRow(row))
        solarSystemID = row.GetUInt(0);
    // V4c: optional targetID — "shoot what I shoot" / "rep this pilot" carrier
    // (the entity or character the broadcast is ABOUT; consumers resolve it).
    const uint32_t targetID = payload.value("targetID", 0u);
    std::string nameEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    DBerror err;
    uint32 bcastID = 0;
    sDatabase.RunQueryLID(err, bcastID,
        "INSERT INTO vevFleetBroadcasts (fleetID, characterID, name, bcastGroup, senderRole, "
        "senderWingID, senderSquadID, solarSystemID, targetID, createdAt) "
        "VALUES (%u, %u, '%s', %u, %u, %u, %u, %u, %u, UNIX_TIMESTAMP())",
        r.fleetID, charID, nameEsc.c_str(), group, r.role, r.wingID, r.squadID, solarSystemID, targetID);
    if (bcastID > 200)   // keep the last ~200 per fleet
        sDatabase.RunQuery(err,
            "DELETE FROM vevFleetBroadcasts WHERE fleetID = %u AND broadcastID < %u",
            r.fleetID, bcastID - 200);
    return json{{"ok", true}, {"broadcastID", bcastID}};
}

// listFleetBroadcasts { characterID, sinceID? } — the broadcasts VISIBLE to
// this viewer, newest first. Recipient resolution mirrors evemu
// FleetService::FleetBroadcast (FleetService.cpp:1357-1437): All = everyone;
// Down from FC = whole fleet, from WC = their wing, from SC/member = their
// squad; Up reaches the chain of command (FC, own WC, own SC).
static json handleListFleetBroadcasts(const json& payload) {
    const uint32_t charID = payload.value("characterID", 0u);
    const uint32_t sinceID = payload.value("sinceID", 0u);
    if (charID == 0) throw std::runtime_error("characterID required");
    const VevFleetRole v = vevFleetRoleOf(charID, 0);
    json out = json::array();
    if (v.fleetID == 0) return json{{"broadcasts", out}, {"fleetID", 0}};
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT b.broadcastID, b.characterID, COALESCE(c.characterName,''), b.name, "
        "       b.bcastGroup, b.senderRole, b.senderWingID, b.senderSquadID, "
        "       COALESCE(s.solarSystemName,''), b.createdAt, COALESCE(b.targetID,0) "
        "FROM vevFleetBroadcasts b "
        "LEFT JOIN chrCharacters c ON c.characterID = b.characterID "
        "LEFT JOIN mapSolarSystems s ON s.solarSystemID = b.solarSystemID "
        "WHERE b.fleetID = %u AND b.broadcastID > %u "
        "ORDER BY b.broadcastID DESC LIMIT 80", v.fleetID, sinceID))
        throw std::runtime_error(std::string("DB error: ") + res.error.c_str());
    DBResultRow row;
    while (res.GetRow(row)) {
        const uint32_t senderID    = row.GetUInt(1);
        const uint32_t group       = row.GetUInt(4);
        const uint32_t senderRole  = row.GetUInt(5);
        const uint32_t senderWing  = row.GetUInt(6);
        const uint32_t senderSquad = row.GetUInt(7);
        bool visible = (senderID == charID) || (group == 3);
        if (!visible && group == 1) {          // Down the chain
            if (senderRole == 1) visible = true;
            else if (senderRole == 2) visible = (v.wingID == senderWing && senderWing != 0);
            else visible = (v.squadID == senderSquad && senderSquad != 0);
        } else if (!visible && group == 2) {   // Up the chain
            if (senderRole == 2) visible = (v.role == 1);
            else visible = (v.role == 1)
                || (v.role == 2 && v.wingID == senderWing)
                || (v.role == 3 && v.squadID == senderSquad);
        }
        if (!visible) continue;
        out.push_back(json{
            {"broadcastID",     row.GetUInt(0)},
            {"characterID",     senderID},
            {"senderName",      row.GetText(2) ? std::string(row.GetText(2)) : ""},
            {"name",            row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"group",           group},
            {"senderRole",      senderRole},
            {"solarSystemName", row.GetText(8) ? std::string(row.GetText(8)) : ""},
            {"createdAt",       row.GetInt64(9)},
            {"targetID",        row.GetUInt(10)},
        });
    }
    return json{{"broadcasts", out}, {"fleetID", v.fleetID}};
}

// warpToFleetMember { characterID, targetCharacterID, distance? } — EVE
// "Warp to Member". Same fleet + same system + target undocked; enqueues
// warp_to on the caller's ship via ai_command (EntityList.cpp resolves the
// target SHIP as a SystemEntity), so it works phantom-to-phantom — W2's
// BeyonceService path only serves 3D clients.
static json handleWarpToFleetMember(const json& payload) {
    const uint32_t charID   = payload.value("characterID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    const int distance      = payload.value("distance", 0);
    if (charID == 0 || targetID == 0)
        throw std::runtime_error("characterID and targetCharacterID required");
    const VevFleetRole a = vevFleetRoleOf(charID, 0);
    const VevFleetRole b = vevFleetRoleOf(targetID, 0);
    if (a.fleetID == 0 || a.fleetID != b.fleetID)
        throw std::runtime_error("that pilot is not in your fleet");
    DBQueryResult res;
    DBResultRow row;
    if (!(sDatabase.RunQuery(res,
        "SELECT c1.solarSystemID, c2.solarSystemID, c2.shipID, c2.stationID "
        "FROM chrCharacters c1, chrCharacters c2 "
        "WHERE c1.characterID = %u AND c2.characterID = %u", charID, targetID)
        && res.GetRow(row)))
        throw std::runtime_error("character not found");
    if (row.GetUInt(0) != row.GetUInt(1))
        throw std::runtime_error("that pilot is in a different system");
    if (row.GetUInt(3) != 0)
        throw std::runtime_error("that pilot is docked");
    const uint32_t shipID = row.GetUInt(2);
    if (shipID == 0) throw std::runtime_error("that pilot has no ship");
    enqueueAICommand(charID, "warp_to",
        json{{"targetID", shipID}, {"distance", distance}}.dump());
    return json{{"ok", true}};
}
// setFleetAppointment { characterID, fleetID, targetCharacterID, appointment }
// appointment: 0 = none, 1 = Anchor, 2 = Logi Anchor. Boss/FC/CEO assign;
// one holder per appointment per fleet (previous holder stripped, like evemu
// SetBooster). The cognition engine reads these as follow/guard targets.
static json handleSetFleetAppointment(const json& payload) {
    const uint32_t fleetID  = payload.value("fleetID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    const uint32_t appointment = payload.value("appointment", 0u);
    if (fleetID == 0 || targetID == 0)
        throw std::runtime_error("fleetID and targetCharacterID required");
    if (appointment > 2) throw std::runtime_error("appointment must be 0 (none), 1 (Anchor) or 2 (Logi Anchor)");
    vevRequireFleetCommand(payload, fleetID);
    DBQueryResult res;
    DBResultRow row;
    if (!(sDatabase.RunQuery(res,
        "SELECT characterID FROM vevFleetMembers WHERE fleetID = %u AND characterID = %u",
        fleetID, targetID) && res.GetRow(row)))
        throw std::runtime_error("that pilot is not in this fleet");
    DBerror err;
    if (appointment != 0)
        sDatabase.RunQuery(err,
            "UPDATE vevFleetMembers SET appointment = 0 WHERE fleetID = %u AND appointment = %u",
            fleetID, appointment);
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET appointment = %u WHERE fleetID = %u AND characterID = %u",
        appointment, fleetID, targetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}
// setFleetReady { characterID, ready, note? } — the pre-flight self-check:
// a member marks THEMSELF ready (or not) for the fleet's next departure, with
// an optional note (an AI pilot writes its preparation reasoning here — the
// FC reads WHY each pilot is/isn't ready). Self-set only.
static json handleSetFleetReady(const json& payload) {
    const uint32_t charID = payload.value("characterID", 0u);
    const bool ready = payload.value("ready", false);
    std::string note = payload.value("note", "");
    if (charID == 0) throw std::runtime_error("characterID required");
    if (note.size() > 200) note = note.substr(0, 200);
    const VevFleetRole r = vevFleetRoleOf(charID, 0);
    if (r.fleetID == 0) throw std::runtime_error("you are not in a fleet");
    std::string noteEsc;
    sDatabase.DoEscapeString(noteEsc, note);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE vevFleetMembers SET ready = %u, readyNote = '%s' "
        "WHERE fleetID = %u AND characterID = %u",
        ready ? 1 : 0, noteEsc.c_str(), r.fleetID, charID);
    return json{{"fleet", vevFleetJson(r.fleetID)}};
}
// ─── VEV_FLEET_V4 handlers END ──────────────────────────────────────────────


// ─── VEV_FLEET_DOCTRINE (2026-06-14) ────────────────────────────────────────
// The in-game fleet-doctrine layer: corp-published ship+fit lineups, bound to
// a fleet, assigned to pilots, with live compliance. Model from real EVE fleet
// ops (doctrine = mandated hull+fit per role; FC checks "are you in doctrine").

static std::string vevTypeName(uint32_t typeID) {
    if (typeID == 0) return "";
    DBQueryResult res; DBResultRow row;
    if (sDatabase.RunQuery(res, "SELECT typeName FROM invTypes WHERE typeID = %u", typeID)
        && res.GetRow(row) && row.GetText(0))
        return row.GetText(0);
    return "";
}

static const char* vevSlotKind(int flag) {
    if (flag >= 27 && flag <= 34) return "high";
    if (flag >= 19 && flag <= 26) return "mid";
    if (flag >= 11 && flag <= 18) return "low";
    if (flag >= 92 && flag <= 99) return "rig";
    return "other";
}

// Parse a stored fitJson string into resolved [{flag,typeID,typeName,slot}].
static json vevResolveFit(const std::string& fitJson) {
    json out = json::array();
    if (fitJson.empty()) return out;
    json arr;
    try { arr = json::parse(fitJson); } catch (...) { return out; }
    if (!arr.is_array()) return out;
    for (auto& m : arr) {
        const int flag = m.value("flag", 0);
        const uint32_t tid = m.value("typeID", 0u);
        out.push_back(json{
            {"flag", flag}, {"typeID", tid},
            {"typeName", vevTypeName(tid)}, {"slot", vevSlotKind(flag)},
        });
    }
    return out;
}

// A member's currently-fitted module typeIDs (high/mid/low/rig).
static std::vector<uint32_t> vevFittedModuleTypes(uint32_t charID) {
    std::vector<uint32_t> out;
    DBQueryResult res; DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT e.typeID FROM entity e JOIN chrCharacters c ON c.shipID = e.locationID "
        "WHERE c.characterID = %u AND ((e.flag BETWEEN 11 AND 34) OR (e.flag BETWEEN 92 AND 99))",
        charID))
        while (res.GetRow(row)) out.push_back(row.GetUInt(0));
    return out;
}

// Compliance of a member's CURRENT ship vs a target fit (hull + modules).
//  -> {compliance:int 0..100, note:string}
static json vevCompliance(uint32_t charID, uint32_t hullTypeID, const std::string& fitJson) {
    DBQueryResult res; DBResultRow row;
    uint32_t curHull = 0;
    if (sDatabase.RunQuery(res,
        "SELECT e.typeID FROM entity e JOIN chrCharacters c ON c.shipID = e.itemID "
        "WHERE c.characterID = %u", charID) && res.GetRow(row))
        curHull = row.GetUInt(0);
    if (curHull == 0)
        return json{{"compliance", 0}, {"note", "no ship"}};
    if (hullTypeID != 0 && curHull != hullTypeID)
        return json{{"compliance", 0}, {"note", std::string("needs ") + vevTypeName(hullTypeID)}};
    // hull OK (or unspecified) — score modules
    std::vector<uint32_t> need;
    if (!fitJson.empty()) {
        try {
            json arr = json::parse(fitJson);
            if (arr.is_array()) for (auto& m : arr) {
                uint32_t t = m.value("typeID", 0u);
                if (t) need.push_back(t);
            }
        } catch (...) {}
    }
    if (need.empty())
        return json{{"compliance", 100}, {"note", "in doctrine"}};
    std::vector<uint32_t> have = vevFittedModuleTypes(charID);
    int matched = 0;
    for (uint32_t t : need) {
        for (size_t i = 0; i < have.size(); ++i) {
            if (have[i] == t) { matched++; have[i] = 0; break; }
        }
    }
    int pct = (int)(100.0 * matched / (double)need.size() + 0.5);
    std::string note = (matched == (int)need.size())
        ? "in doctrine"
        : (std::to_string(matched) + "/" + std::to_string(need.size()) + " modules");
    return json{{"compliance", pct}, {"note", note}};
}

// getFleetDoctrine { fleetID } — the Doctrine tab payload: the fleet's roles
// (resolved hull+fit), each role's assigned pilots + compliance, and the
// unassigned roster. Heavy (per-member fit reads) — fetched only when the tab
// is open, NOT in the 5s member poll.
static json handleGetFleetDoctrine(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    DBQueryResult fres; DBResultRow frow;
    if (!(sDatabase.RunQuery(fres,
        "SELECT COALESCE(doctrineID,0), COALESCE(doctrineName,''), COALESCE(purpose,'custom') "
        "FROM vevFleets WHERE fleetID = %u", fleetID) && fres.GetRow(frow)))
        throw std::runtime_error("fleet not found");
    json out = json{
        {"fleetID", fleetID},
        {"doctrineID", frow.GetUInt(0)},
        {"doctrineName", frow.GetText(1) ? std::string(frow.GetText(1)) : ""},
        {"purpose", frow.GetText(2) ? std::string(frow.GetText(2)) : "custom"},
    };
    // members with their assigned role + ship
    struct Mem { uint32_t id; std::string name, ship; bool online, isAI; std::string role; };
    std::vector<Mem> mems;
    DBQueryResult mres; DBResultRow mrow;
    if (sDatabase.RunQuery(mres,
        "SELECT fm.characterID, COALESCE(c.characterName,''), COALESCE(t.typeName,''), "
        "       c.online, (ai.characterID IS NOT NULL), COALESCE(fm.doctrineRole,'') "
        "FROM vevFleetMembers fm "
        "LEFT JOIN chrCharacters c ON c.characterID = fm.characterID "
        "LEFT JOIN entity e ON e.itemID = c.shipID "
        "LEFT JOIN invTypes t ON t.typeID = e.typeID "
        "LEFT JOIN vevAiPilots ai ON ai.characterID = fm.characterID "
        "WHERE fm.fleetID = %u", fleetID))
        while (mres.GetRow(mrow))
            mems.push_back({mrow.GetUInt(0), mrow.GetText(1)?mrow.GetText(1):"",
                            mrow.GetText(2)?mrow.GetText(2):"", mrow.GetUInt(3)!=0,
                            mrow.GetUInt(4)!=0, mrow.GetText(5)?mrow.GetText(5):""});

    json roles = json::array();
    int inDoctrine = 0, assignedTotal = 0;
    DBQueryResult rres; DBResultRow rrow;
    if (sDatabase.RunQuery(rres,
        "SELECT fdRoleID, roleKey, label, hullTypeID, fitJson, desiredCount, notes, sortOrder "
        "FROM vevFleetDoctrineRoles WHERE fleetID = %u ORDER BY sortOrder, fdRoleID", fleetID))
    {
        while (rres.GetRow(rrow)) {
            const std::string roleKey = rrow.GetText(1) ? rrow.GetText(1) : "";
            const uint32_t hull = rrow.GetUInt(3);
            const std::string fitJson = rrow.GetText(4) ? rrow.GetText(4) : "";
            json assigned = json::array();
            for (auto& m : mems) {
                if (m.role != roleKey) continue;
                assignedTotal++;
                // per-pilot override fit?
                std::string useFit = fitJson; bool override_ = false;
                DBQueryResult ores; DBResultRow orow;
                if (sDatabase.RunQuery(ores,
                    "SELECT assignedFitJson FROM vevFleetMembers WHERE fleetID = %u AND characterID = %u",
                    fleetID, m.id) && ores.GetRow(orow) && orow.GetText(0) && orow.GetText(0)[0]) {
                    useFit = orow.GetText(0); override_ = true;
                }
                json comp = vevCompliance(m.id, hull, useFit);
                if (comp.value("compliance", 0) >= 80) inDoctrine++;
                assigned.push_back(json{
                    {"characterID", m.id}, {"name", m.name}, {"shipTypeName", m.ship},
                    {"online", m.online}, {"isAI", m.isAI},
                    {"compliance", comp["compliance"]}, {"complianceNote", comp["note"]},
                    {"override", override_},
                });
            }
            roles.push_back(json{
                {"fdRoleID", rrow.GetUInt(0)},
                {"roleKey", roleKey},
                {"label", rrow.GetText(2) ? std::string(rrow.GetText(2)) : ""},
                {"hullTypeID", hull},
                {"hullName", vevTypeName(hull)},
                {"fit", vevResolveFit(fitJson)},
                {"desiredCount", rrow.GetUInt(5)},
                {"notes", rrow.GetText(6) ? std::string(rrow.GetText(6)) : ""},
                {"sortOrder", (int)rrow.GetInt(7)},
                {"assigned", assigned},
            });
        }
    }
    out["roles"] = roles;
    json unassigned = json::array();
    for (auto& m : mems) if (m.role.empty())
        unassigned.push_back(json{{"characterID", m.id}, {"name", m.name},
                                  {"shipTypeName", m.ship}, {"online", m.online}, {"isAI", m.isAI}});
    out["unassigned"] = unassigned;
    out["inDoctrine"] = inDoctrine;
    out["assignedTotal"] = assignedTotal;
    return json{{"doctrine", out}};
}

// listDoctrineTemplates { characterID } — the viewer's corp templates + the
// built-in library (corp 0).
static json handleListDoctrineTemplates(const json& payload) {
    const uint32_t charID = payload.value("characterID", 0u);
    uint32_t corpID = 0;
    DBQueryResult cres; DBResultRow crow;
    if (sDatabase.RunQuery(cres, "SELECT corporationID FROM chrCharacters WHERE characterID = %u", charID)
        && cres.GetRow(crow)) corpID = crow.GetUInt(0);
    json out = json::array();
    DBQueryResult res; DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT d.doctrineID, d.corporationID, d.name, d.purpose, d.description, "
        "       (SELECT COUNT(*) FROM vevDoctrineRoles r WHERE r.doctrineID = d.doctrineID) "
        "FROM vevDoctrines d WHERE d.corporationID = 0 OR d.corporationID = %u "
        "ORDER BY d.corporationID, d.name", corpID))
        while (res.GetRow(row))
            out.push_back(json{
                {"doctrineID", row.GetUInt(0)},
                {"corporationID", row.GetUInt(1)},
                {"isBuiltIn", row.GetUInt(1) == 0},
                {"name", row.GetText(2) ? std::string(row.GetText(2)) : ""},
                {"purpose", row.GetText(3) ? std::string(row.GetText(3)) : "custom"},
                {"description", row.GetText(4) ? std::string(row.GetText(4)) : ""},
                {"roleCount", row.GetUInt(5)},
            });
    return json{{"templates", out}};
}

// bindFleetDoctrine { characterID, fleetID, doctrineID } — adopt a template:
// copy its roles into the fleet's editable role set; clears stale assignments.
static json handleBindFleetDoctrine(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const uint32_t doctrineID = payload.value("doctrineID", 0u);
    if (fleetID == 0 || doctrineID == 0) throw std::runtime_error("fleetID and doctrineID required");
    vevRequireFleetCommand(payload, fleetID);
    DBQueryResult dres; DBResultRow drow;
    if (!(sDatabase.RunQuery(dres, "SELECT name, purpose FROM vevDoctrines WHERE doctrineID = %u", doctrineID)
        && dres.GetRow(drow)))
        throw std::runtime_error("doctrine template not found");
    std::string nameEsc, purpEsc;
    sDatabase.DoEscapeString(nameEsc, drow.GetText(0) ? drow.GetText(0) : "");
    sDatabase.DoEscapeString(purpEsc, drow.GetText(1) ? drow.GetText(1) : "custom");
    DBerror err;
    sDatabase.RunQuery(err, "DELETE FROM vevFleetDoctrineRoles WHERE fleetID = %u", fleetID);
    sDatabase.RunQuery(err,
        "INSERT INTO vevFleetDoctrineRoles (fleetID, roleKey, label, hullTypeID, fitJson, desiredCount, notes, sortOrder) "
        "SELECT %u, roleKey, label, hullTypeID, fitJson, desiredCount, notes, sortOrder "
        "FROM vevDoctrineRoles WHERE doctrineID = %u", fleetID, doctrineID);
    sDatabase.RunQuery(err,
        "UPDATE vevFleets SET doctrineID = %u, doctrineName = '%s', purpose = '%s' WHERE fleetID = %u",
        doctrineID, nameEsc.c_str(), purpEsc.c_str(), fleetID);
    sDatabase.RunQuery(err, "UPDATE vevFleetMembers SET doctrineRole = '', assignedFitJson = NULL WHERE fleetID = %u", fleetID);
    return handleGetFleetDoctrine(json{{"fleetID", fleetID}});
}

// setFleetDoctrineRole { characterID, fleetID, fdRoleID?, roleKey, label,
//   hullTypeID, fit:[{flag,typeID}], desiredCount, notes, sortOrder } — upsert.
static json handleSetFleetDoctrineRole(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const uint32_t fdRoleID = payload.value("fdRoleID", 0u);
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    vevRequireFleetCommand(payload, fleetID);
    const std::string roleKey = payload.value("roleKey", "custom");
    const std::string label = payload.value("label", "");
    const uint32_t hull = payload.value("hullTypeID", 0u);
    const uint32_t desired = payload.value("desiredCount", 1u);
    const std::string notes = payload.value("notes", "");
    const int sortOrder = payload.value("sortOrder", 0);
    std::string fitStr = "[]";
    if (payload.contains("fit") && payload["fit"].is_array()) fitStr = payload["fit"].dump();
    else if (payload.contains("fitJson") && payload["fitJson"].is_string()) fitStr = payload["fitJson"].get<std::string>();
    std::string rkEsc, lblEsc, ntEsc, fitEsc;
    sDatabase.DoEscapeString(rkEsc, roleKey);
    sDatabase.DoEscapeString(lblEsc, label);
    sDatabase.DoEscapeString(ntEsc, notes.substr(0, 200));
    sDatabase.DoEscapeString(fitEsc, fitStr);
    DBerror err;
    if (fdRoleID != 0) {
        sDatabase.RunQuery(err,
            "UPDATE vevFleetDoctrineRoles SET roleKey='%s', label='%s', hullTypeID=%u, fitJson='%s', "
            "desiredCount=%u, notes='%s', sortOrder=%d WHERE fdRoleID=%u AND fleetID=%u",
            rkEsc.c_str(), lblEsc.c_str(), hull, fitEsc.c_str(), desired, ntEsc.c_str(), sortOrder, fdRoleID, fleetID);
    } else {
        sDatabase.RunQuery(err,
            "INSERT INTO vevFleetDoctrineRoles (fleetID, roleKey, label, hullTypeID, fitJson, desiredCount, notes, sortOrder) "
            "VALUES (%u, '%s', '%s', %u, '%s', %u, '%s', %d)",
            fleetID, rkEsc.c_str(), lblEsc.c_str(), hull, fitEsc.c_str(), desired, ntEsc.c_str(), sortOrder);
    }
    return handleGetFleetDoctrine(json{{"fleetID", fleetID}});
}

// deleteFleetDoctrineRole { characterID, fdRoleID }
static json handleDeleteFleetDoctrineRole(const json& payload) {
    const uint32_t fdRoleID = payload.value("fdRoleID", 0u);
    if (fdRoleID == 0) throw std::runtime_error("fdRoleID required");
    DBQueryResult res; DBResultRow row;
    if (!(sDatabase.RunQuery(res, "SELECT fleetID FROM vevFleetDoctrineRoles WHERE fdRoleID = %u", fdRoleID)
        && res.GetRow(row))) throw std::runtime_error("role not found");
    const uint32_t fleetID = row.GetUInt(0);
    vevRequireFleetCommand(payload, fleetID);
    DBerror err;
    sDatabase.RunQuery(err, "DELETE FROM vevFleetDoctrineRoles WHERE fdRoleID = %u", fdRoleID);
    return handleGetFleetDoctrine(json{{"fleetID", fleetID}});
}

// assignDoctrineRole { characterID, fleetID, targetCharacterID, roleKey, fit? }
// Seat a pilot into a role; optional fit = a per-pilot fit override.
static json handleAssignDoctrineRole(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    const std::string roleKey = payload.value("roleKey", "");
    if (fleetID == 0 || targetID == 0) throw std::runtime_error("fleetID and targetCharacterID required");
    vevRequireFleetCommand(payload, fleetID);
    std::string rkEsc;
    sDatabase.DoEscapeString(rkEsc, roleKey);
    DBerror err;
    if (payload.contains("fit") && payload["fit"].is_array()) {
        std::string fitEsc;
        sDatabase.DoEscapeString(fitEsc, payload["fit"].dump());
        sDatabase.RunQuery(err,
            "UPDATE vevFleetMembers SET doctrineRole='%s', assignedFitJson='%s' WHERE fleetID=%u AND characterID=%u",
            rkEsc.c_str(), fitEsc.c_str(), fleetID, targetID);
    } else {
        sDatabase.RunQuery(err,
            "UPDATE vevFleetMembers SET doctrineRole='%s', assignedFitJson=NULL WHERE fleetID=%u AND characterID=%u",
            rkEsc.c_str(), fleetID, targetID);
    }
    return handleGetFleetDoctrine(json{{"fleetID", fleetID}});
}

// captureFitFromShip { targetCharacterID } — read a pilot's CURRENT fit into a
// doctrine-role fit (the "copy from current ship" authoring trick). Read-only.
static json handleCaptureFitFromShip(const json& payload) {
    const uint32_t targetID = payload.value("targetCharacterID", 0u);
    if (targetID == 0) throw std::runtime_error("targetCharacterID required");
    DBQueryResult hres; DBResultRow hrow;
    uint32_t hull = 0;
    if (sDatabase.RunQuery(hres,
        "SELECT e.typeID FROM entity e JOIN chrCharacters c ON c.shipID = e.itemID WHERE c.characterID = %u",
        targetID) && hres.GetRow(hrow)) hull = hrow.GetUInt(0);
    json fit = json::array();
    DBQueryResult res; DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT e.flag, e.typeID, COALESCE(t.typeName,'') FROM entity e "
        "JOIN chrCharacters c ON c.shipID = e.locationID JOIN invTypes t ON t.typeID = e.typeID "
        "WHERE c.characterID = %u AND ((e.flag BETWEEN 11 AND 34) OR (e.flag BETWEEN 92 AND 99)) "
        "ORDER BY e.flag", targetID))
        while (res.GetRow(row)) {
            const int flag = (int)row.GetInt(0);
            fit.push_back(json{{"flag", flag}, {"typeID", row.GetUInt(1)},
                               {"typeName", row.GetText(2) ? std::string(row.GetText(2)) : ""},
                               {"slot", vevSlotKind(flag)}});
        }
    return json{{"hullTypeID", hull}, {"hullName", vevTypeName(hull)}, {"fit", fit}};
}

// saveFleetDoctrineAsTemplate { characterID, fleetID, name } — snapshot the
// fleet's roles into a new corp template (CEO).
static json handleSaveFleetDoctrineAsTemplate(const json& payload) {
    const uint32_t ceoID = payload.value("characterID", 0u);
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const std::string name = payload.value("name", "");
    if (ceoID == 0 || fleetID == 0) throw std::runtime_error("characterID and fleetID required");
    if (name.size() < 2 || name.size() > 60) throw std::runtime_error("template name must be 2-60 characters");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you must be a CEO to save a corp doctrine template");
    DBQueryResult pres; DBResultRow prow;
    std::string purpose = "custom";
    if (sDatabase.RunQuery(pres, "SELECT COALESCE(purpose,'custom') FROM vevFleets WHERE fleetID = %u", fleetID)
        && pres.GetRow(prow) && prow.GetText(0)) purpose = prow.GetText(0);
    std::string nameEsc, purpEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    sDatabase.DoEscapeString(purpEsc, purpose);
    DBerror err; uint32 docID = 0;
    if (!sDatabase.RunQueryLID(err, docID,
        "INSERT INTO vevDoctrines (corporationID, name, purpose, description, createdBy, createdAt) "
        "VALUES (%u, '%s', '%s', '', %u, UNIX_TIMESTAMP())", corpID, nameEsc.c_str(), purpEsc.c_str(), ceoID))
        throw std::runtime_error("template insert failed");
    sDatabase.RunQuery(err,
        "INSERT INTO vevDoctrineRoles (doctrineID, roleKey, label, hullTypeID, fitJson, desiredCount, notes, sortOrder) "
        "SELECT %u, roleKey, label, hullTypeID, fitJson, desiredCount, notes, sortOrder "
        "FROM vevFleetDoctrineRoles WHERE fleetID = %u", docID, fleetID);
    return json{{"doctrineID", docID}, {"ok", true}};
}

// setFleetPhase { characterID, fleetID, phase } — the fleet posture badge.
static json handleSetFleetPhase(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    const std::string phase = payload.value("phase", "");
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    static const std::set<std::string> ok = {
        "forming","staging","ready","underway","hold","engaged","extracting","stood_down"};
    if (!ok.count(phase)) throw std::runtime_error("invalid phase");
    vevRequireFleetCommand(payload, fleetID);
    std::string pEsc; sDatabase.DoEscapeString(pEsc, phase);
    DBerror err;
    sDatabase.RunQuery(err, "UPDATE vevFleets SET phase = '%s' WHERE fleetID = %u", pEsc.c_str(), fleetID);
    return json{{"fleet", vevFleetJson(fleetID)}};
}

// setFleetOps { characterID, fleetID, gateStatus?, gateLabel?, hostileCount?,
//   holdGo?, sitrep? } — the "gate is red/green" + GO/HOLD + sitrep intel.
static json handleSetFleetOps(const json& payload) {
    const uint32_t fleetID = payload.value("fleetID", 0u);
    if (fleetID == 0) throw std::runtime_error("fleetID required");
    vevRequireFleetCommand(payload, fleetID);
    DBerror err;
    if (payload.contains("gateStatus")) {
        const std::string gs = payload.value("gateStatus", "unknown");
        static const std::set<std::string> ok = {"unknown","green","red","clear"};
        if (!ok.count(gs)) throw std::runtime_error("invalid gateStatus");
        std::string e; sDatabase.DoEscapeString(e, gs);
        sDatabase.RunQuery(err, "UPDATE vevFleets SET gateStatus='%s' WHERE fleetID=%u", e.c_str(), fleetID);
    }
    if (payload.contains("gateLabel")) {
        std::string e; sDatabase.DoEscapeString(e, payload.value("gateLabel", "").substr(0, 60));
        sDatabase.RunQuery(err, "UPDATE vevFleets SET gateLabel='%s' WHERE fleetID=%u", e.c_str(), fleetID);
    }
    if (payload.contains("hostileCount"))
        sDatabase.RunQuery(err, "UPDATE vevFleets SET hostileCount=%d WHERE fleetID=%u",
                           payload.value("hostileCount", 0), fleetID);
    if (payload.contains("holdGo")) {
        const std::string hg = payload.value("holdGo", "hold");
        if (hg != "hold" && hg != "go") throw std::runtime_error("holdGo must be hold|go");
        std::string e; sDatabase.DoEscapeString(e, hg);
        sDatabase.RunQuery(err, "UPDATE vevFleets SET holdGo='%s' WHERE fleetID=%u", e.c_str(), fleetID);
    }
    if (payload.contains("sitrep")) {
        std::string e; sDatabase.DoEscapeString(e, payload.value("sitrep", "").substr(0, 200));
        sDatabase.RunQuery(err, "UPDATE vevFleets SET sitrep='%s' WHERE fleetID=%u", e.c_str(), fleetID);
    }
    return json{{"fleet", vevFleetJson(fleetID)}};
}
// searchFitTypes { query, kind, limit? } — name search filtered to a fitting
// context: kind='ship' (hulls, cat 6), 'high'|'mid'|'low'|'rig' (modules whose
// slot effect matches — 12/13/11/2663), 'charge' (cat 8). One query, no N+1
// getTypeInfo round-trips. Powers the EFT-style doctrine fit editor.
static json handleSearchFitTypes(const json& payload) {
    const std::string query = payload.value("query", "");
    const std::string kind = payload.value("kind", "ship");
    int limit = payload.value("limit", 60);
    if (limit < 1 || limit > 200) limit = 60;
    std::string qEsc;
    sDatabase.DoEscapeString(qEsc, query);
    // Idiomatic evemu RunQuery: printf format + args. The LIKE wildcards are
    // %%...%% (escaped %), the query is a %s arg. (Building a full SQL string
    // and passing it as the format mangles its literal % — the bug this fixes.)
    json out = json::array();
    DBQueryResult res; DBResultRow row;
    bool okq = false;
    if (kind == "ship") {
        okq = sDatabase.RunQuery(res,
            "SELECT t.typeID, t.typeName, COALESCE(g.groupName,'') "
            "FROM invTypes t JOIN invGroups g ON g.groupID = t.groupID "
            "WHERE g.categoryID = 6 AND t.typeName LIKE '%%%s%%' "
            "ORDER BY t.typeName LIMIT %d", qEsc.c_str(), limit);
    } else if (kind == "charge") {
        okq = sDatabase.RunQuery(res,
            "SELECT t.typeID, t.typeName, COALESCE(g.groupName,'') "
            "FROM invTypes t JOIN invGroups g ON g.groupID = t.groupID "
            "WHERE g.categoryID = 8 AND t.typeName LIKE '%%%s%%' "
            "ORDER BY t.typeName LIMIT %d", qEsc.c_str(), limit);
    } else {
        int eff = 0;
        if (kind == "high") eff = 12; else if (kind == "mid") eff = 13;
        else if (kind == "low") eff = 11; else if (kind == "rig") eff = 2663;
        else throw std::runtime_error("kind must be ship|high|mid|low|rig|charge");
        okq = sDatabase.RunQuery(res,
            "SELECT t.typeID, t.typeName, COALESCE(g.groupName,'') "
            "FROM invTypes t JOIN invGroups g ON g.groupID = t.groupID "
            "JOIN dgmTypeEffects e ON e.typeID = t.typeID AND e.effectID = %d "
            "WHERE g.categoryID = 7 AND t.typeName LIKE '%%%s%%' "
            "ORDER BY t.typeName LIMIT %d", eff, qEsc.c_str(), limit);
    }
    if (okq)
        while (res.GetRow(row))
            out.push_back(json{
                {"typeID", row.GetUInt(0)},
                {"typeName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"groupName", row.GetText(2) ? std::string(row.GetText(2)) : ""},
            });
    return json{{"types", out}};
}
// ─── VEV_FLEET_DOCTRINE END ─────────────────────────────────────────────────


// ─── VEV_PI_SCAN (PI-1): planetary interaction read endpoints ───────────────
// Deterministic resource fields. The heatmap SHAPE is a pure function of
// (planetID, resourceTypeID), so a scan is byte-identical across server reboots
// by construction. Richness (the abundance scalar, mutable later for depletion)
// is persisted in vevPlanetResources, seeded once from system security
// (lower sec = richer). The engine replicates this model for ECU yield in PI-3.
static inline uint32_t vevPiHash(uint32_t a, uint32_t b) {
    uint32_t h = a * 2654435761u;
    h ^= (b + 0x9e3779b9u + (h << 6) + (h >> 2));
    h *= 2246822519u;
    return h ? h : 1u;
}
static inline uint32_t vevPiRand(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }
static inline double   vevPiRandF(uint32_t& s) { return (vevPiRand(s) >> 8) / 16777216.0; }

static json handleGetSystemPlanets(const json& payload) {
    if (!payload.is_object() || !payload.contains("solarSystemID"))
        throw std::runtime_error("payload requires { solarSystemID }");
    uint32_t systemID = payload.at("solarSystemID").get<uint32_t>();

    float security = 0.0f;
    std::string sysName;
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT solarSystemName, security FROM mapSolarSystems WHERE solarSystemID = %u", systemID)) {
            DBResultRow row;
            if (res.GetRow(row)) { sysName = row.GetText(0) ? row.GetText(0) : ""; security = row.GetFloat(1); }
        }
    }
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT d.itemID, d.itemName, d.typeID, COALESCE(t.typeName,''), d.celestialIndex, "
        "       (SELECT COUNT(*) FROM piPlanets p WHERE p.planetID = d.itemID) AS colonies "
        "FROM mapDenormalize d LEFT JOIN invTypes t ON t.typeID = d.typeID "
        "WHERE d.solarSystemID = %u AND d.groupID = 7 ORDER BY d.celestialIndex", systemID))
        throw std::runtime_error(std::string("DB error (planets): ") + res.error.c_str());
    json planets = json::array();
    DBResultRow row;
    while (res.GetRow(row)) {
        planets.push_back(json{
            {"planetID",       row.GetUInt(0)},
            {"name",           row.GetText(1) ? std::string(row.GetText(1)) : ""},
            {"typeID",         row.GetUInt(2)},
            {"typeName",       row.GetText(3) ? std::string(row.GetText(3)) : ""},
            {"celestialIndex", row.GetUInt(4)},
            {"colonyCount",    row.GetUInt(5)},
        });
    }
    return json{{"solarSystemID", systemID}, {"solarSystemName", sysName},
                {"security", security}, {"planets", planets}};
}

static json handleScanPlanet(const json& payload) {
    if (!payload.is_object() || !payload.contains("planetID"))
        throw std::runtime_error("payload requires { planetID }");
    uint32_t planetID = payload.at("planetID").get<uint32_t>();
    uint32_t charID   = payload.value("characterID", 0u);

    uint32_t planetTypeID = 0; float security = 0.0f;
    std::string planetName, planetTypeName;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT d.typeID, COALESCE(pt.typeName,''), d.itemName, COALESCE(s.security,0) "
            "FROM mapDenormalize d "
            "LEFT JOIN invTypes pt ON pt.typeID = d.typeID "
            "LEFT JOIN mapSolarSystems s ON s.solarSystemID = d.solarSystemID "
            "WHERE d.itemID = %u AND d.groupID = 7", planetID))
            throw std::runtime_error(std::string("DB error (planet): ") + res.error.c_str());
        DBResultRow row;
        if (!res.GetRow(row)) throw std::runtime_error("planet not found");
        planetTypeID   = row.GetUInt(0);
        planetTypeName = row.GetText(1) ? row.GetText(1) : "";
        planetName     = row.GetText(2) ? row.GetText(2) : "";
        security       = row.GetFloat(3);
    }

    json resources = json::array();
    DBQueryResult rres;
    if (!sDatabase.RunQuery(rres,
        "SELECT rt.typeID, rt.typeName FROM dgmTypeAttributes d1 "
        "JOIN dgmTypeAttributes d2 ON d2.typeID = d1.typeID "
        "JOIN invTypes rt ON rt.typeID = d2.valueFloat "
        "WHERE d1.attributeID = 1632 AND d1.valueFloat = %u AND d2.attributeID = 709 "
        "ORDER BY rt.typeName", planetTypeID))
        throw std::runtime_error(std::string("DB error (resources): ") + rres.error.c_str());
    DBResultRow rrow;
    while (rres.GetRow(rrow)) {
        uint32_t resTypeID = rrow.GetUInt(0);
        std::string resName = rrow.GetText(1) ? rrow.GetText(1) : "";
        uint32_t seed = vevPiHash(planetID, resTypeID);

        double richness = 0.0;
        {
            // Ensure a row exists (seeded from sec on first encounter), then ALWAYS
            // read richness back so the value is the persisted FLOAT — identical on
            // every scan and after reboot (the determinism gate). Returning the
            // freshly-computed double would mismatch the FLOAT-rounded rescan.
            DBQueryResult pr;
            bool have = sDatabase.RunQuery(pr,
                "SELECT richness FROM vevPlanetResources WHERE planetID = %u AND resourceTypeID = %u",
                planetID, resTypeID) && pr.GetRowCount() > 0;
            if (!have) {
                uint32_t rs = seed;
                double jitter = vevPiRandF(rs);
                double secTerm = (1.1 - (double)security);
                double init = 0.25 + secTerm * 0.35 + jitter * 0.30;
                if (init < 0.10) init = 0.10;
                if (init > 1.00) init = 1.00;
                DBerror err;
                sDatabase.RunQuery(err,
                    "INSERT IGNORE INTO vevPlanetResources (planetID, resourceTypeID, seed, richness) "
                    "VALUES (%u, %u, %u, %f)", planetID, resTypeID, seed, init);
            }
            DBQueryResult pr2;
            if (sDatabase.RunQuery(pr2,
                "SELECT richness FROM vevPlanetResources WHERE planetID = %u AND resourceTypeID = %u",
                planetID, resTypeID) && pr2.GetRowCount() > 0) {
                DBResultRow prow; pr2.GetRow(prow); richness = prow.GetDouble(0);
            }
        }

        uint32_t hs = seed ^ 0x5bd1e995u;
        int nBlobs = 3 + (int)(vevPiRand(hs) % 4);
        json hotspots = json::array();
        for (int i = 0; i < nBlobs; ++i) {
            double lat   = (vevPiRandF(hs) * 180.0) - 90.0;
            double lon   = (vevPiRandF(hs) * 360.0) - 180.0;
            double sigma = 12.0 + vevPiRandF(hs) * 28.0;
            double peak  = richness * (0.55 + vevPiRandF(hs) * 0.45);
            hotspots.push_back(json{{"lat", lat}, {"lon", lon}, {"sigma", sigma}, {"peak", peak}});
        }

        resources.push_back(json{
            {"typeID", resTypeID}, {"name", resName},
            {"richness", richness}, {"seed", seed}, {"hotspots", hotspots},
        });
    }

    int remoteSensing = 0, planetology = 0, advPlanetology = 0;
    if (charID) {
        DBQueryResult sk;
        if (sDatabase.RunQuery(sk,
            "SELECT e.typeID, COALESCE(ea.valueInt, 0) FROM entity e "
            "JOIN entity_attributes ea ON ea.itemID = e.itemID AND ea.attributeID = 280 "
            "WHERE e.ownerID = %u AND e.typeID IN (13279, 2406, 2403)", charID)) {
            DBResultRow srow;
            while (sk.GetRow(srow)) {
                switch (srow.GetUInt(0)) {
                    case 13279: remoteSensing = srow.GetInt(1); break;
                    case 2406:  planetology = srow.GetInt(1); break;
                    case 2403:  advPlanetology = srow.GetInt(1); break;
                }
            }
        }
    }

    return json{
        {"planetID", planetID}, {"name", planetName},
        {"typeID", planetTypeID}, {"typeName", planetTypeName},
        {"security", security}, {"resources", resources},
        {"skills", json{{"remoteSensing", remoteSensing},
                        {"planetology", planetology},
                        {"advancedPlanetology", advPlanetology}}},
    };
}

// ─── VEV_PI_COLONY (PI-2c): read a colony + bridge build commands ───────────
static json handleGetColony(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("planetID"))
        throw std::runtime_error("payload requires { characterID, planetID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t planetID = payload.at("planetID").get<uint32_t>();

    json out = {{"planetID", planetID}, {"established", false}};
    uint32_t ccPinID = 0;
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res, "SELECT ccPinID FROM piPlanets WHERE charID=%u AND planetID=%u", charID, planetID))
            throw std::runtime_error(std::string("DB error (colony): ") + res.error.c_str());
        DBResultRow row;
        if (!res.GetRow(row)) return out;   // not established here
        ccPinID = row.GetUInt(0);
    }
    out["established"] = true;
    out["ccPinID"] = ccPinID;

    int ccLevel = 0;
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res, "SELECT level FROM piCCPin WHERE pinID=%u", ccPinID)) {
            DBResultRow row; if (res.GetRow(row)) ccLevel = row.GetInt(0);
        }
    }
    static const int CC_CPU[6] = {1675, 7057, 12136, 17215, 21315, 25415};
    static const int CC_PG[6]  = {6000, 9000, 12000, 15000, 17000, 19000};
    int lv = ccLevel < 0 ? 0 : (ccLevel > 5 ? 5 : ccLevel);
    out["ccLevel"] = ccLevel;
    out["cpuMax"] = CC_CPU[lv];
    out["pgMax"]  = CC_PG[lv];

    std::map<uint32_t, json> contents;
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res, "SELECT pinID, typeID, itemQty FROM piPinContents WHERE ccPinID=%u", ccPinID)) {
            DBResultRow row;
            while (res.GetRow(row))
                contents[row.GetUInt(0)].push_back(json{{"typeID", row.GetUInt(1)}, {"qty", row.GetUInt(2)}});
        }
    }
    json pins = json::array();
    {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT pinID, typeID, state, latitude, longitude, isCommandCenter+0, isProcess+0, "
            "isStorage+0, isECU+0, isLaunchable+0, schematicID, COALESCE(qtyPerCycle,0), "
            "(SELECT groupID FROM invTypes WHERE typeID=piPins.typeID) "
            "FROM piPins WHERE ccPinID=%u", ccPinID))
            throw std::runtime_error(std::string("DB error (pins): ") + res.error.c_str());
        DBResultRow row;
        while (res.GetRow(row)) {
            uint32_t pinID = row.GetUInt(0);
            uint32_t gid = row.GetUInt(12);   // VEV_PI_KINDFIX: groupID = static truth
            std::string kind = (pinID == ccPinID) ? "cc"
                             : gid == 1063 ? "ecu" : gid == 1028 ? "processor"
                             : gid == 1029 ? "storage" : gid == 1030 ? "launchpad" : "other";
            pins.push_back(json{
                {"pinID", pinID}, {"typeID", row.GetUInt(1)}, {"state", row.GetInt(2)},
                {"lat", row.GetDouble(3)}, {"lon", row.GetDouble(4)}, {"kind", kind},
                {"schematicID", row.GetUInt(10)}, {"qtyPerCycle", row.GetUInt(11)},
                {"contents", contents.count(pinID) ? contents[pinID] : json::array()},
            });
        }
    }
    out["pins"] = pins;

    json links = json::array();
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res, "SELECT linkID, endpoint1, endpoint2, level FROM piLinks WHERE ccPinID=%u", ccPinID)) {
            DBResultRow row;
            while (res.GetRow(row))
                links.push_back(json{{"linkID", row.GetUInt(0)}, {"src", row.GetUInt(1)}, {"dest", row.GetUInt(2)}, {"level", row.GetInt(3)}});
        }
    }
    out["links"] = links;

    json routes = json::array();
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res, "SELECT routeID, srcPinID, destPinID, typeID, itemQty FROM piRoutes WHERE ccPinID=%u", ccPinID)) {
            DBResultRow row;
            while (res.GetRow(row))
                routes.push_back(json{{"routeID", row.GetUInt(0)}, {"src", row.GetUInt(1)}, {"dest", row.GetUInt(2)}, {"typeID", row.GetUInt(3)}, {"qty", row.GetUInt(4)}});
        }
    }
    out["routes"] = routes;
    return out;
}

// Bridge a build command to the proven pi_* queue verbs. Fire-and-enqueue; the
// client refetches getColony to observe the result (real pin ids, etc.).
static uint32_t vevAllocateColonyOwner(uint32_t corpID, uint32_t requesterID);  // VEV_PI_CORP fwd decl

static bool vevEstablishViaCustoms(uint32_t owner, uint32_t systemID, uint32_t planetID, const std::string& specJson) {
    // VEV_POCO2 (de-fake R2): establish a corp colony the real EVE way — through the
    // planet's customs office, not a surface spawn-teleport. Ensure a POCO, deliver the
    // Command Center into it, then commit consuming it (fromCustoms). R2b replaces the
    // deposit with a real Primae flight depositing the CC from cargo.
    uint32_t ccTypeID = 0;
    std::string spec = specJson;
    try { json s = json::parse(specJson); if (s.contains("cc")) ccTypeID = s["cc"].value("typeID", 0u); s["fromCustoms"] = 1; spec = s.dump(); }
    catch (...) {}
    char j[192];
    snprintf(j, sizeof(j), "{\"systemID\":%u,\"planetID\":%u}", systemID, planetID);
    enqueueAICommand(owner, "pi_spawn_customs", j);
    if (ccTypeID) { snprintf(j, sizeof(j), "{\"systemID\":%u,\"planetID\":%u,\"typeID\":%u,\"qty\":1}", systemID, planetID, ccTypeID);
                    enqueueAICommand(owner, "pi_deposit_customs", j); }
    return enqueueAICommand(owner, "pi_commit_spec", spec);
}

static json handleColonyCmd(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("planetID") || !payload.contains("op"))
        throw std::runtime_error("payload requires { characterID, planetID, op }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t planetID = payload.at("planetID").get<uint32_t>();
    std::string op = payload.at("op").get<std::string>();

    uint32_t systemID = 0;
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res, "SELECT solarSystemID FROM mapDenormalize WHERE itemID=%u", planetID)) {
            DBResultRow row; if (res.GetRow(row)) systemID = row.GetUInt(0);
        }
    }
    if (systemID == 0) throw std::runtime_error("planet not found");
    auto u = [&](const char* k) -> uint32_t { return payload.contains(k) ? (uint32_t)payload.at(k).get<double>() : 0u; };
    auto f = [&](const char* k) -> double  { return payload.contains(k) ? payload.at(k).get<double>() : 0.0; };

    std::string cmd, params;
    char buf[256];
    if (op == "establish") {
        // VEV_PI_CORP: assign the colony to a corp member with free capacity
        // (requester preferred). corpID 0 (no player corp) → solo, the requester.
        uint32_t corpID = vevCorpIdForCeo(charID);
        if (corpID == 0) {
            DBQueryResult cr; DBResultRow crow;
            if (sDatabase.RunQuery(cr, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && cr.GetRow(crow))
                corpID = crow.GetUInt(0);
        }
        uint32_t owner = (corpID >= 98000000) ? vevAllocateColonyOwner(corpID, charID) : charID;
        if (owner == 0) throw std::runtime_error("no free colony capacity — train Interplanetary Consolidation or requisition pilots");
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"ccTypeID\":%u,\"lat\":%f,\"lon\":%f}", systemID, planetID, u("ccTypeID"), f("lat"), f("lon"));
        bool ok = enqueueAICommand(owner, "pi_establish", buf);
        return json{{"enqueued", ok}, {"op", op}, {"assignedTo", owner}};
    } else if (op == "build_pin") {
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"typeID\":%u,\"lat\":%f,\"lon\":%f}", systemID, planetID, u("typeID"), f("lat"), f("lon"));
        cmd = "pi_build_pin"; params = buf;
    } else if (op == "schematic") {
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"pinID\":%u,\"schematicID\":%u}", systemID, planetID, u("pinID"), u("schematicID"));
        cmd = "pi_schematic"; params = buf;
    } else if (op == "link") {
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"src\":%u,\"dest\":%u}", systemID, planetID, u("src"), u("dest"));
        cmd = "pi_link"; params = buf;
    } else if (op == "route") {
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"src\":%u,\"dest\":%u,\"typeID\":%u,\"qty\":%u}", systemID, planetID, u("src"), u("dest"), u("typeID"), u("qty"));
        cmd = "pi_route"; params = buf;
    } else if (op == "program") {
        // VEV_PI_PROGRAM: install an extraction program on an ECU pin (live colony).
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"ecuPinID\":%u,\"resourceTypeID\":%u,\"qtyPerCycle\":%u,\"cycleHours\":%f,\"numCycles\":%u,\"lat\":%f,\"lon\":%f}",
                 systemID, planetID, u("pinID"), u("resourceTypeID"), u("qtyPerCycle"), f("cycleHours"), u("numCycles"), f("lat"), f("lon"));
        cmd = "pi_program"; params = buf;
    } else if (op == "remove_pin") {
        uint32_t reqPin = u("pinID");
        { DBQueryResult cr; DBResultRow crow;
          if (sDatabase.RunQuery(cr, "SELECT ccPinID FROM piPlanets WHERE charID=%u AND planetID=%u", charID, planetID) && cr.GetRow(crow) && crow.GetUInt(0) == reqPin)
              throw std::runtime_error("can't remove the command center (it anchors the colony)"); }
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"pinID\":%u}", systemID, planetID, reqPin);
        cmd = "pi_remove_pin"; params = buf;
    } else if (op == "remove_link") {
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"src\":%u,\"dest\":%u}", systemID, planetID, u("src"), u("dest"));
        cmd = "pi_remove_link"; params = buf;
    } else if (op == "remove_route") {
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"planetID\":%u,\"routeID\":%u}", systemID, planetID, u("routeID"));
        cmd = "pi_remove_route"; params = buf;
    } else if (op == "establish_spec") {
        // design-then-commit. Load the saved design, allocate an owner from the
        // corp pool (requester preferred), then either commit now (self-serve)
        // or raise a provision job for the logi squad to deliver the CC.
        std::string mode = payload.contains("mode") ? payload.at("mode").get<std::string>() : "self";
        DBQueryResult sres; DBResultRow srow;
        if (!(sDatabase.RunQuery(sres, "SELECT specID,specJson,ccTypeID FROM vevColonySpec WHERE designerID=%u AND planetID=%u", charID, planetID) && sres.GetRow(srow)))
            throw std::runtime_error("no saved design for this planet \xe2\x80\x94 design the colony first");
        uint32_t specID = srow.GetUInt(0);
        std::string specJson = srow.GetText(1) ? srow.GetText(1) : "";
        uint32_t ccTypeID = srow.GetUInt(2);
        uint32_t corpID = vevCorpIdForCeo(charID);
        if (corpID == 0) { DBQueryResult cr; DBResultRow crow; if (sDatabase.RunQuery(cr, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && cr.GetRow(crow)) corpID = crow.GetUInt(0); }
        uint32_t owner = (corpID >= 98000000) ? vevAllocateColonyOwner(corpID, charID) : charID;
        if (owner == 0) throw std::runtime_error("no free colony capacity \xe2\x80\x94 train Interplanetary Consolidation or requisition pilots");
        if (mode == "dispatch") {
            std::string ccName; { DBQueryResult tr; DBResultRow trow; if (sDatabase.RunQuery(tr, "SELECT typeName FROM invTypes WHERE typeID=%u", ccTypeID) && tr.GetRow(trow)) ccName = trow.GetText(0) ? trow.GetText(0) : ""; }
            json items = json::array(); items.push_back(json{{"typeID", ccTypeID}, {"qty", 1}, {"name", ccName}});
            json payloadObj = json{{"specID", specID}, {"owner", owner}};
            std::string itemsEsc, payEsc; sDatabase.DoEscapeString(itemsEsc, items.dump()); sDatabase.DoEscapeString(payEsc, payloadObj.dump());
            DBerror err;
            sDatabase.RunQuery(err,
                "INSERT INTO vevLogiJobs (corpID,kind,planetID,solarSystemID,itemsJson,payloadJson,status,createdAt) "
                "VALUES (%u,'provision',%u,%u,'%s','%s','open',UNIX_TIMESTAMP()) "
                "ON DUPLICATE KEY UPDATE itemsJson='%s',payloadJson='%s',status='open'",
                corpID, planetID, systemID, itemsEsc.c_str(), payEsc.c_str(), itemsEsc.c_str(), payEsc.c_str());
            DBerror e2; sDatabase.RunQuery(e2, "UPDATE vevColonySpec SET status='dispatched',ownerID=%u WHERE specID=%u", owner, specID);
            return json{{"dispatched", true}, {"op", op}, {"assignedTo", owner}, {"specID", specID}};
        }
        bool ok = vevEstablishViaCustoms(owner, systemID, planetID, specJson);   // VEV_POCO2
        DBerror e2; sDatabase.RunQuery(e2, "UPDATE vevColonySpec SET status='committed',ownerID=%u WHERE specID=%u", owner, specID);
        return json{{"committed", ok}, {"op", op}, {"assignedTo", owner}, {"specID", specID}};
    } else if (op == "fulfill_provision") {
        // CC delivered (by a logi pilot, or the CEO manually) → commit the spec.
        uint32_t jobID = u("jobID");
        if (jobID == 0) throw std::runtime_error("fulfill_provision requires jobID");
        DBQueryResult jr; DBResultRow jrow;
        if (!(sDatabase.RunQuery(jr, "SELECT payloadJson FROM vevLogiJobs WHERE jobID=%u AND kind='provision' AND status='open'", jobID) && jr.GetRow(jrow)))
            throw std::runtime_error("no open provision job with that id");
        json pay = json::object(); try { const char* s = jrow.GetText(0); if (s) pay = json::parse(s); } catch (...) {}
        uint32_t specID = pay.value("specID", 0u), owner = pay.value("owner", 0u);
        if (specID == 0 || owner == 0) throw std::runtime_error("provision job missing spec/owner payload");
        DBQueryResult sr; DBResultRow srow2;
        if (!(sDatabase.RunQuery(sr, "SELECT specJson FROM vevColonySpec WHERE specID=%u", specID) && sr.GetRow(srow2)))
            throw std::runtime_error("spec not found");
        std::string specJson = srow2.GetText(0) ? srow2.GetText(0) : "";
        bool ok = vevEstablishViaCustoms(owner, systemID, planetID, specJson);   // VEV_POCO2
        DBerror e1; sDatabase.RunQuery(e1, "UPDATE vevLogiJobs SET status='done' WHERE jobID=%u", jobID);
        DBerror e2; sDatabase.RunQuery(e2, "UPDATE vevColonySpec SET status='committed' WHERE specID=%u", specID);
        return json{{"committed", ok}, {"op", op}, {"jobID", jobID}, {"assignedTo", owner}};
    } else if (op == "fulfill_extract") {
        // VEV_PI_LOGI2: haul a colony's produced goods to a corp hub + close the job.
        uint32_t jobID = u("jobID"), hub = u("hubStationID");
        DBQueryResult jr; DBResultRow jrow;
        if (!(sDatabase.RunQuery(jr, "SELECT planetID, solarSystemID FROM vevLogiJobs WHERE jobID=%u AND kind='extract' AND status='open'", jobID) && jr.GetRow(jrow)))
            throw std::runtime_error("no open extract job with that id");
        uint32_t jPlanet = jrow.GetUInt(0), jSys = jrow.GetUInt(1);
        uint32_t owner = 0;
        { DBQueryResult cr; DBResultRow crow; if (sDatabase.RunQuery(cr, "SELECT charID FROM piPlanets WHERE planetID=%u LIMIT 1", jPlanet) && cr.GetRow(crow)) owner = crow.GetUInt(0); }
        if (owner == 0) throw std::runtime_error("colony owner not found for that planet");
        if (hub == 0) { DBQueryResult hr; DBResultRow hrow; if (sDatabase.RunQuery(hr, "SELECT itemID FROM mapDenormalize WHERE solarSystemID=%u AND groupID=15 LIMIT 1", jSys) && hr.GetRow(hrow)) hub = hrow.GetUInt(0); }
        if (hub == 0) throw std::runtime_error("no hub station found in that system");
        char hb[128]; snprintf(hb, sizeof(hb), "{\"systemID\":%u,\"planetID\":%u,\"hubStationID\":%u}", jSys, jPlanet, hub);
        bool ok = enqueueAICommand(owner, "pi_haul_export", hb);
        DBerror e1; sDatabase.RunQuery(e1, "UPDATE vevLogiJobs SET status='done', assignedCharID=%u WHERE jobID=%u", owner, jobID);
        return json{{"hauled", ok}, {"op", op}, {"jobID", jobID}, {"assignedTo", owner}, {"hub", hub}};
    } else {
        throw std::runtime_error("unknown op (establish|build_pin|schematic|link|route)");
    }
    bool ok = enqueueAICommand(charID, cmd, params);
    return json{{"enqueued", ok}, {"op", op}};
}

// VEV_PI_SCHEM (PI-3): the P0->P4 recipe list for the schematic picker.
static json handleGetSchematics(const json& payload) {
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT pt.schematicID, COALESCE(s.schematicName,''), COALESCE(s.cycleTime,3600), pt.typeID, "
        "COALESCE(t.typeName,''), pt.quantity, pt.isInput+0 FROM piTypeMap pt "
        "LEFT JOIN schematics s ON s.schematicID = pt.schematicID "
        "LEFT JOIN invTypes t ON t.typeID = pt.typeID "
        "ORDER BY pt.schematicID, pt.isInput"))
        throw std::runtime_error(std::string("DB error (schematics): ") + res.error.c_str());
    json out = json::array();
    json cur; uint32_t curID = 0;
    DBResultRow row;
    while (res.GetRow(row)) {
        uint32_t sid = row.GetUInt(0);
        if (sid != curID) {
            if (curID != 0) out.push_back(cur);
            curID = sid;
            cur = json{{"schematicID", sid}, {"name", row.GetText(1) ? std::string(row.GetText(1)) : ""},
                       {"cycleTime", row.GetInt(2)}, {"inputs", json::array()}, {"output", json(nullptr)}};
        }
        json item = {{"typeID", row.GetUInt(3)}, {"name", row.GetText(4) ? std::string(row.GetText(4)) : ""}, {"qty", row.GetUInt(5)}};
        if (row.GetInt(6) != 0) cur["inputs"].push_back(item);
        else cur["output"] = item;
    }
    if (curID != 0) out.push_back(cur);
    return json{{"schematics", out}};
}

// ─── VEV_PI_CORP: colony capacity pool + slot allocator + network view ──────
// Per-member capacity = 1 + Interplanetary Consolidation (skill 2495, <=6).
// Pick a member with a free slot for a new colony (requester preferred).
static uint32_t vevAllocateColonyOwner(uint32_t corpID, uint32_t requesterID) {
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT c.characterID, "
        "  COALESCE((SELECT ea.valueInt FROM entity e JOIN entity_attributes ea "
        "    ON ea.itemID=e.itemID AND ea.attributeID=280 WHERE e.ownerID=c.characterID AND e.typeID=2495 LIMIT 1),0) AS ic, "
        "  (SELECT COUNT(*) FROM piPlanets p WHERE p.charID=c.characterID) AS used "
        "FROM chrCharacters c WHERE c.corporationID=%u", corpID))
        return 0;
    uint32_t best = 0; int bestFree = 0;
    DBResultRow row;
    while (res.GetRow(row)) {
        uint32_t cid = row.GetUInt(0);
        int cap = 1 + (int)row.GetInt(1); if (cap > 6) cap = 6;
        int free = cap - (int)row.GetInt(2);
        if (free <= 0) continue;
        if (cid == requesterID) return requesterID;   // requester has room → use them
        if (free > bestFree) { bestFree = free; best = cid; }
    }
    return best;
}

static json handleGetColonyNetwork(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID"))
        throw std::runtime_error("payload requires { characterID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t corpID = 0;
    {
        DBQueryResult res; DBResultRow row;
        if (sDatabase.RunQuery(res, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && res.GetRow(row))
            corpID = row.GetUInt(0);
    }
    int total = 0, used = 0;
    json members = json::array();
    if (corpID) {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT c.characterID, c.characterName, "
            "  COALESCE((SELECT ea.valueInt FROM entity e JOIN entity_attributes ea "
            "    ON ea.itemID=e.itemID AND ea.attributeID=280 WHERE e.ownerID=c.characterID AND e.typeID=2495 LIMIT 1),0), "
            "  (SELECT COUNT(*) FROM piPlanets p WHERE p.charID=c.characterID) "
            "FROM chrCharacters c WHERE c.corporationID=%u ORDER BY c.characterName", corpID)) {
            DBResultRow row;
            while (res.GetRow(row)) {
                int cap = 1 + (int)row.GetInt(2); if (cap > 6) cap = 6;
                int u = (int)row.GetInt(3);
                total += cap; used += u;
                members.push_back(json{{"characterID", row.GetUInt(0)},
                    {"name", row.GetText(1) ? std::string(row.GetText(1)) : ""}, {"capacity", cap}, {"used", u}});
            }
        }
    }
    // every colony across the corp, with planet name + owner
    json colonies = json::array();
    if (corpID) {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT p.planetID, COALESCE(d.itemName,''), COALESCE(pt.typeName,''), p.charID, "
            "  COALESCE(c.characterName,''), p.numberOfPins, p.solarSystemID "
            "FROM piPlanets p "
            "JOIN chrCharacters c ON c.characterID=p.charID AND c.corporationID=%u "
            "LEFT JOIN mapDenormalize d ON d.itemID=p.planetID "
            "LEFT JOIN invTypes pt ON pt.typeID=d.typeID "
            "ORDER BY d.itemName", corpID)) {
            DBResultRow row;
            while (res.GetRow(row))
                colonies.push_back(json{{"planetID", row.GetUInt(0)}, {"planetName", row.GetText(1) ? std::string(row.GetText(1)) : ""},
                    {"typeName", row.GetText(2) ? std::string(row.GetText(2)) : ""}, {"ownerID", row.GetUInt(3)},
                    {"ownerName", row.GetText(4) ? std::string(row.GetText(4)) : ""}, {"pins", row.GetUInt(5)}, {"solarSystemID", row.GetUInt(6)}});
        }
    }
    return json{{"corpID", corpID}, {"pool", json{{"total", total}, {"used", used}}}, {"members", members}, {"colonies", colonies}};
}

// ─── VEV_ORG: corp functional units (org chart) ──────────────────────────────
// EVE corps organize into functional divisions (mining/combat/logi/exploration/
// industry/hauling/recon), each led by a Director. A pilot's unit steers its
// skills training (the engine reads unitID -> kind -> career ladder). Unlike
// fleet squads (transient, fleet-scoped), units are CORP-scoped and persistent.
static const std::set<std::string>& vevUnitKinds() {
    static const std::set<std::string> k = {
        "mining","combat","logistics","exploration","industry","hauling","recon","command"};
    return k;
}

// createUnit { ceoCharacterID, name, kind }
static json handleCreateUnit(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    const std::string name = payload.value("name", "");
    std::string kind = payload.value("kind", "mining");
    if (ceoID == 0) throw std::runtime_error("ceoCharacterID required");
    if (name.size() < 2 || name.size() > 60) throw std::runtime_error("unit name must be 2-60 characters");
    if (!vevUnitKinds().count(kind)) kind = "mining";
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    std::string nameEsc, kindEsc;
    sDatabase.DoEscapeString(nameEsc, name);
    sDatabase.DoEscapeString(kindEsc, kind);
    DBerror err; uint32 unitID = 0;
    if (!sDatabase.RunQueryLID(err, unitID,
        "INSERT INTO vevUnits (corporationID, name, kind, leaderCharacterID, createdAt) "
        "VALUES (%u, '%s', '%s', 0, UNIX_TIMESTAMP())", corpID, nameEsc.c_str(), kindEsc.c_str()))
        throw std::runtime_error(std::string("unit insert failed: ") + err.c_str());
    return json{{"corporation", vevCorpJson(corpID, ceoID)}};
}

// renameUnit { ceoCharacterID, unitID, name?, kind? }
static json handleRenameUnit(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    const uint32_t unitID = payload.value("unitID", 0u);
    if (ceoID == 0 || unitID == 0) throw std::runtime_error("ceoCharacterID, unitID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    DBQueryResult res; DBResultRow row;
    if (!(sDatabase.RunQuery(res, "SELECT corporationID FROM vevUnits WHERE unitID = %u", unitID)
          && res.GetRow(row) && row.GetUInt(0) == corpID))
        throw std::runtime_error("unit not in your corp");
    if (payload.contains("name")) {
        const std::string name = payload.value("name", "");
        if (name.size() < 2 || name.size() > 60) throw std::runtime_error("unit name must be 2-60 characters");
        std::string nameEsc; sDatabase.DoEscapeString(nameEsc, name);
        DBerror err; sDatabase.RunQuery(err, "UPDATE vevUnits SET name = '%s' WHERE unitID = %u", nameEsc.c_str(), unitID);
    }
    if (payload.contains("kind")) {
        std::string kind = payload.value("kind", "mining");
        if (!vevUnitKinds().count(kind)) kind = "mining";
        std::string kindEsc; sDatabase.DoEscapeString(kindEsc, kind);
        DBerror err; sDatabase.RunQuery(err, "UPDATE vevUnits SET kind = '%s' WHERE unitID = %u", kindEsc.c_str(), unitID);
    }
    return json{{"corporation", vevCorpJson(corpID, ceoID)}};
}

// deleteUnit { ceoCharacterID, unitID } — members fall to unassigned (unitID 0).
static json handleDeleteUnit(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    const uint32_t unitID = payload.value("unitID", 0u);
    if (ceoID == 0 || unitID == 0) throw std::runtime_error("ceoCharacterID, unitID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    DBQueryResult res; DBResultRow row;
    if (!(sDatabase.RunQuery(res, "SELECT corporationID FROM vevUnits WHERE unitID = %u", unitID)
          && res.GetRow(row) && row.GetUInt(0) == corpID))
        throw std::runtime_error("unit not in your corp");
    DBerror err;
    sDatabase.RunQuery(err, "UPDATE vevAiPilots SET unitID = 0 WHERE unitID = %u", unitID);
    sDatabase.RunQuery(err, "DELETE FROM vevUnits WHERE unitID = %u", unitID);
    return json{{"corporation", vevCorpJson(corpID, ceoID)}};
}

// assignToUnit { ceoCharacterID, characterID, unitID } — unitID 0 = unassigned.
// Steers the pilot's skills training (the engine reads unitID -> kind -> ladder).
static json handleAssignToUnit(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    const uint32_t charID = payload.value("characterID", 0u);
    const uint32_t unitID = payload.value("unitID", 0u);
    if (ceoID == 0 || charID == 0) throw std::runtime_error("ceoCharacterID, characterID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    DBQueryResult res; DBResultRow row;
    if (!(sDatabase.RunQuery(res,
        "SELECT 1 FROM vevAiPilots WHERE characterID = %u AND corporationID = %u", charID, corpID)
        && res.GetRow(row)))
        throw std::runtime_error("that pilot is not an AI pilot in your corp");
    if (unitID != 0) {
        if (!(sDatabase.RunQuery(res, "SELECT corporationID FROM vevUnits WHERE unitID = %u", unitID)
              && res.GetRow(row) && row.GetUInt(0) == corpID))
            throw std::runtime_error("unit not in your corp");
    }
    DBerror err;
    sDatabase.RunQuery(err, "UPDATE vevAiPilots SET unitID = %u WHERE characterID = %u", unitID, charID);
    // leaving a unit clears any leadership of it
    sDatabase.RunQuery(err, "UPDATE vevUnits SET leaderCharacterID = 0 "
        "WHERE leaderCharacterID = %u AND unitID <> %u", charID, unitID);
    return json{{"corporation", vevCorpJson(corpID, ceoID)}};
}

// setUnitLeader { ceoCharacterID, unitID, characterID } — characterID 0 clears.
// The leader (Director) layers leadership skills on the unit's core path.
static json handleSetUnitLeader(const json& payload) {
    const uint32_t ceoID = payload.value("ceoCharacterID", 0u);
    const uint32_t unitID = payload.value("unitID", 0u);
    const uint32_t charID = payload.value("characterID", 0u);
    if (ceoID == 0 || unitID == 0) throw std::runtime_error("ceoCharacterID, unitID required");
    const uint32_t corpID = vevCorpIdForCeo(ceoID);
    if (corpID == 0) throw std::runtime_error("you are not the CEO of a corporation");
    DBQueryResult res; DBResultRow row;
    if (!(sDatabase.RunQuery(res, "SELECT corporationID FROM vevUnits WHERE unitID = %u", unitID)
          && res.GetRow(row) && row.GetUInt(0) == corpID))
        throw std::runtime_error("unit not in your corp");
    if (charID != 0) {
        if (!(sDatabase.RunQuery(res,
            "SELECT 1 FROM vevAiPilots WHERE characterID = %u AND unitID = %u", charID, unitID)
            && res.GetRow(row)))
            throw std::runtime_error("the leader must be a member of the unit");
    }
    DBerror err;
    sDatabase.RunQuery(err, "UPDATE vevUnits SET leaderCharacterID = %u WHERE unitID = %u", charID, unitID);
    return json{{"corporation", vevCorpJson(corpID, ceoID)}};
}

// ─── VEV_PI_LOGI1B: persist + fetch a CEO's colony design (design-then-commit) ─
static json handleSaveColonySpec(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("planetID") || !payload.contains("spec"))
        throw std::runtime_error("payload requires { characterID, planetID, spec }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t planetID = payload.at("planetID").get<uint32_t>();
    const json& spec = payload.at("spec");
    uint32_t systemID = spec.value("systemID", 0u);
    uint32_t ccTypeID = spec.contains("cc") ? spec["cc"].value("typeID", 0u) : 0u;
    std::string esc; sDatabase.DoEscapeString(esc, spec.dump());
    DBerror err;
    sDatabase.RunQuery(err,
        "INSERT INTO vevColonySpec (designerID,planetID,solarSystemID,ccTypeID,specJson,status,createdAt) "
        "VALUES (%u,%u,%u,%u,'%s','design',UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE solarSystemID=%u,ccTypeID=%u,specJson='%s',status='design'",
        charID, planetID, systemID, ccTypeID, esc.c_str(), systemID, ccTypeID, esc.c_str());
    return json{{"saved", true}, {"planetID", planetID}};
}

static json handleGetColonySpec(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("planetID"))
        throw std::runtime_error("payload requires { characterID, planetID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t planetID = payload.at("planetID").get<uint32_t>();
    DBQueryResult res; DBResultRow row;
    if (sDatabase.RunQuery(res, "SELECT specID,specJson,status,ownerID FROM vevColonySpec WHERE designerID=%u AND planetID=%u", charID, planetID) && res.GetRow(row)) {
        json spec = json::object();
        try { const char* s = row.GetText(1); if (s) spec = json::parse(s); } catch (...) {}
        return json{{"exists", true}, {"specID", row.GetUInt(0)}, {"spec", spec},
                    {"status", row.GetText(2) ? std::string(row.GetText(2)) : ""}, {"ownerID", row.GetUInt(3)}};
    }
    return json{{"exists", false}};
}

// ─── VEV_DERIVER: cascade deriver — doctrine product → full bottom-up supply bill ──
// (reuses the existing vevTypeName helper). Recursive with ancestry-based cycle breaking.
static std::map<uint32_t,int> g_vevDepth;  // VEV_AUTODRAIN: max recursion depth per node (topo order)
static void vevResolveBOM(uint32_t tid, uint64_t q, std::set<uint32_t>& path,
    std::map<uint32_t,uint64_t>& rawNeed, std::map<uint32_t,std::string>& rawSrc,
    std::map<uint32_t,uint64_t>& stepQty, std::map<uint32_t,std::string>& stepKind, int depth) {
    if (tid == 0 || q == 0) return;
    { auto it = g_vevDepth.find(tid); if (it == g_vevDepth.end() || depth > it->second) g_vevDepth[tid] = depth; }  // VEV_AUTODRAIN
    if (depth > 32 || path.count(tid)) { rawNeed[tid] += q; rawSrc[tid] = "buy"; return; }  // cycle/depth → leaf

    // 1. manufacturing blueprint? → recurse BOM
    uint32_t bpID = 0;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT blueprintTypeID FROM invBlueprintTypes WHERE productTypeID=%u LIMIT 1", tid) && r.GetRow(row)) bpID = row.GetUInt(0); }
    if (bpID != 0) {
        stepKind[tid] = "manufacture"; stepQty[tid] += q;
        std::vector<std::pair<uint32_t,uint64_t>> kids;
        { DBQueryResult r; DBResultRow row;
          if (sDatabase.RunQuery(r, "SELECT materialTypeID, quantity FROM invTypeMaterials WHERE typeID=%u UNION ALL SELECT r.requiredTypeID, r.quantity FROM ramTypeRequirements r JOIN invTypes mat ON mat.typeID=r.requiredTypeID JOIN invGroups g ON g.groupID=mat.groupID WHERE r.typeID=%u AND r.activityID=1 AND g.categoryID<>16 AND r.quantity>0", tid, bpID))
              while (r.GetRow(row)) kids.push_back({row.GetUInt(0), q * (uint64_t)row.GetUInt(1)}); }
        path.insert(tid);
        for (auto& k : kids) vevResolveBOM(k.first, k.second, path, rawNeed, rawSrc, stepQty, stepKind, depth+1);
        path.erase(tid); return;
    }
    // 2. PI product (piTypeMap output)? → recurse PI inputs  [checked BEFORE reactions]
    uint32_t schemID = 0, outQ = 1;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT schematicID, quantity FROM piTypeMap WHERE isInput=0 AND typeID=%u LIMIT 1", tid) && r.GetRow(row)) { schemID = row.GetUInt(0); outQ = row.GetUInt(1); if (!outQ) outQ = 1; } }
    if (schemID != 0) {
        stepKind[tid] = "pi"; stepQty[tid] += q;
        uint64_t runs = (q + outQ - 1) / outQ;
        std::vector<std::pair<uint32_t,uint64_t>> kids;
        { DBQueryResult r; DBResultRow row;
          if (sDatabase.RunQuery(r, "SELECT typeID, quantity FROM piTypeMap WHERE schematicID=%u AND isInput=1", schemID))
              while (r.GetRow(row)) kids.push_back({row.GetUInt(0), runs * (uint64_t)row.GetUInt(1)}); }
        path.insert(tid);
        for (auto& k : kids) vevResolveBOM(k.first, k.second, path, rawNeed, rawSrc, stepQty, stepKind, depth+1);
        path.erase(tid); return;
    }
    // 3. reaction output? → recurse reaction inputs
    uint32_t rxID = 0, rxOut = 1;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT reactionTypeID, quantity FROM invTypeReactions WHERE input=0 AND typeID=%u LIMIT 1", tid) && r.GetRow(row)) { rxID = row.GetUInt(0); rxOut = row.GetUInt(1); if (!rxOut) rxOut = 1; } }
    if (rxID != 0) {
        stepKind[tid] = "react"; stepQty[tid] += q;
        uint64_t cycles = (q + rxOut - 1) / rxOut;
        std::vector<std::pair<uint32_t,uint64_t>> kids;
        { DBQueryResult r; DBResultRow row;
          if (sDatabase.RunQuery(r, "SELECT typeID, quantity FROM invTypeReactions WHERE reactionTypeID=%u AND input=1", rxID))
              while (r.GetRow(row)) kids.push_back({row.GetUInt(0), cycles * (uint64_t)row.GetUInt(1)}); }
        path.insert(tid);
        for (auto& k : kids) vevResolveBOM(k.first, k.second, path, rawNeed, rawSrc, stepQty, stepKind, depth+1);
        path.erase(tid); return;
    }
    // 4. leaf — classify the raw source
    uint32_t cat = 0; std::string gname;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT g.categoryID, g.groupName FROM invTypes t JOIN invGroups g ON g.groupID=t.groupID WHERE t.typeID=%u", tid) && r.GetRow(row)) { cat = row.GetUInt(0); gname = row.GetText(1) ? row.GetText(1) : ""; } }
    std::string src = "buy";
    if (cat == 42) src = "pi_extract";
    else if (gname == "Moon Materials") src = "moon";
    else if (gname == "Mineral") src = "mine";
    rawNeed[tid] += q; rawSrc[tid] = src;
}
static json handleDeriveSupplyChain(const json& payload) {
    if (!payload.is_object() || !payload.contains("productTypeID"))
        throw std::runtime_error("payload requires { productTypeID }");
    uint32_t product = payload.at("productTypeID").get<uint32_t>();
    uint64_t qty = payload.contains("qty") ? (uint64_t)payload.at("qty").get<double>() : 1;
    if (qty == 0) qty = 1;
    std::map<uint32_t,uint64_t> rawNeed, stepQty;
    std::map<uint32_t,std::string> rawSrc, stepKind;
    std::set<uint32_t> path;
    vevResolveBOM(product, qty, path, rawNeed, rawSrc, stepQty, stepKind, 0);
    auto director = [](const std::string& s) -> std::string {
        if (s == "mine") return "Mining";
        if (s == "moon" || s == "react") return "POS/Reactions";
        if (s == "pi_extract" || s == "pi") return "Planetary (Logi)";
        if (s == "manufacture") return "Industry";
        return "Procurement";
    };
    json raw = json::array();
    for (auto& kv : rawNeed) raw.push_back(json{{"typeID", kv.first}, {"name", vevTypeName(kv.first)}, {"qty", kv.second}, {"source", rawSrc[kv.first]}, {"director", director(rawSrc[kv.first])}});
    json steps = json::array();
    for (auto& kv : stepQty) steps.push_back(json{{"typeID", kv.first}, {"name", vevTypeName(kv.first)}, {"qty", kv.second}, {"kind", stepKind[kv.first]}, {"director", director(stepKind[kv.first])}});
    return json{{"product", json{{"typeID", product}, {"name", vevTypeName(product)}, {"qty", qty}}}, {"raw", raw}, {"steps", steps}};
}

// ─── VEV_EXEC: explode a derived plan into a corp production backlog ─────────
static std::string vevDirectorFor(const std::string& s) {
    if (s == "mine") return "Mining";
    if (s == "moon" || s == "react") return "POS/Reactions";
    if (s == "pi_extract" || s == "pi") return "Planetary (Logi)";
    if (s == "manufacture") return "Industry";
    return "Procurement";
}
static json handleExecuteSupplyChain(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("productTypeID"))
        throw std::runtime_error("payload requires { characterID, productTypeID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t product = payload.at("productTypeID").get<uint32_t>();
    uint64_t qty = payload.contains("qty") ? (uint64_t)payload.at("qty").get<double>() : 1;
    if (qty == 0) qty = 1;
    uint32_t corpID = 0;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && r.GetRow(row)) corpID = row.GetUInt(0); }
    if (corpID == 0) throw std::runtime_error("no corporation");

    std::map<uint32_t,uint64_t> rawNeed, stepQty;
    std::map<uint32_t,std::string> rawSrc, stepKind;
    std::set<uint32_t> path;
    g_vevDepth.clear();
    vevResolveBOM(product, qty, path, rawNeed, rawSrc, stepQty, stepKind, 0);

    // re-running a plan replaces its prior OPEN jobs (done jobs are history)
    { DBerror e; sDatabase.RunQuery(e, "DELETE FROM vevProductionJobs WHERE corpID=%u AND planRoot=%u AND status='open'", corpID, product); }
    int queued = 0;
    auto add = [&](uint32_t tid, uint64_t q, const std::string& kind, const std::string& dir) {
        DBerror e;
        int d = g_vevDepth.count(tid) ? g_vevDepth[tid] : 0;
        sDatabase.RunQuery(e, "INSERT INTO vevProductionJobs (corpID,planRoot,productTypeID,qty,kind,director,depth,status,createdAt) VALUES (%u,%u,%u,%llu,'%s','%s',%d,'open',UNIX_TIMESTAMP())",
            corpID, product, tid, (unsigned long long)q, kind.c_str(), dir.c_str(), d);
        queued++;
    };
    for (auto& kv : stepQty) add(kv.first, kv.second, stepKind[kv.first], vevDirectorFor(stepKind[kv.first]));
    for (auto& kv : rawNeed) add(kv.first, kv.second, rawSrc[kv.first], vevDirectorFor(rawSrc[kv.first]));
    return json{{"queued", queued}, {"planRoot", product}, {"productName", vevTypeName(product)}, {"qty", qty}};
}
static json handleFulfillProductionJob(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("jobID"))
        throw std::runtime_error("payload requires { characterID, jobID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t jobID = (uint32_t)payload.at("jobID").get<double>();
    uint32_t hub = payload.contains("hubStationID") ? (uint32_t)payload.at("hubStationID").get<double>() : 0;
    uint32_t prodType = 0; uint64_t qty = 0; std::string kind; uint32_t corpID = 0;
    { DBQueryResult r; DBResultRow row;
      if (!(sDatabase.RunQuery(r, "SELECT productTypeID, qty, kind, corpID FROM vevProductionJobs WHERE jobID=%u AND status='open'", jobID) && r.GetRow(row)))
          throw std::runtime_error("no open production job with that id");
      prodType = row.GetUInt(0); qty = row.GetUInt(1); kind = row.GetText(2) ? row.GetText(2) : ""; corpID = row.GetUInt(3); }
    if (hub == 0) {  // default: a station in the same system as a corp colony, else any station
        DBQueryResult r; DBResultRow row;
        if (sDatabase.RunQuery(r, "SELECT itemID FROM mapDenormalize WHERE groupID=15 LIMIT 1") && r.GetRow(row)) hub = row.GetUInt(0);
    }
    if (qty == 0) qty = 1;
    char buf[200]; bool ok = false;
    if (kind == "manufacture") {
        snprintf(buf, sizeof(buf), "{\"locationID\":%u,\"productTypeID\":%u,\"runs\":%llu}", hub, prodType, (unsigned long long)qty);
        ok = enqueueAICommand(charID, "manufacture", buf);
    } else if (kind == "pi") {
        uint32_t outQ = 1; { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT quantity FROM piTypeMap WHERE isInput=0 AND typeID=%u LIMIT 1", prodType) && r.GetRow(row)) { outQ = row.GetUInt(0); if (!outQ) outQ = 1; } }
        uint64_t runs = (qty + outQ - 1) / outQ;
        snprintf(buf, sizeof(buf), "{\"locationID\":%u,\"productTypeID\":%u,\"runs\":%llu}", hub, prodType, (unsigned long long)runs);
        ok = enqueueAICommand(charID, "pi_transform", buf);
    } else if (kind == "react") {
        uint32_t rxID = 0, outQ = 1; { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT reactionTypeID, quantity FROM invTypeReactions WHERE input=0 AND typeID=%u LIMIT 1", prodType) && r.GetRow(row)) { rxID = row.GetUInt(0); outQ = row.GetUInt(1); if (!outQ) outQ = 1; } }
        uint32_t towerID = 0, towerSys = 0;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT s.itemID, e.locationID FROM posStructureData s JOIN entity e ON e.itemID=s.itemID JOIN invTypes t ON t.typeID=e.typeID WHERE t.groupID=365 AND e.ownerID IN (SELECT characterID FROM chrCharacters WHERE corporationID=%u) AND s.state>=4 LIMIT 1", corpID) && r.GetRow(row)) { towerID = row.GetUInt(0); towerSys = row.GetUInt(1); } }
        if (rxID == 0 || towerID == 0) throw std::runtime_error("react job needs a reaction + an online corp tower");
        uint64_t cyc = (qty + outQ - 1) / outQ;
        snprintf(buf, sizeof(buf), "{\"systemID\":%u,\"towerID\":%u,\"reactionTypeID\":%u,\"cycles\":%llu}", towerSys, towerID, rxID, (unsigned long long)cyc);
        ok = enqueueAICommand(charID, "pos_react", buf);
    } else {  // raw: pi_extract / mine / moon / buy → corp extraction/market delivers to the hub
        snprintf(buf, sizeof(buf), "{\"locationID\":%u,\"typeID\":%u,\"qty\":%llu}", hub, prodType, (unsigned long long)qty);
        ok = enqueueAICommand(charID, "vev_stock", buf);
    }
    DBerror e; sDatabase.RunQuery(e, "UPDATE vevProductionJobs SET status='done' WHERE jobID=%u", jobID);
    return json{{"fulfilled", ok}, {"jobID", jobID}, {"kind", kind}, {"hub", hub}};
}

static json handleRunProductionPlan(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("productTypeID"))
        throw std::runtime_error("payload requires { characterID, productTypeID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t planRoot = (uint32_t)payload.at("productTypeID").get<double>();
    uint32_t hub = payload.contains("hubStationID") ? (uint32_t)payload.at("hubStationID").get<double>() : 0;
    uint32_t corpID = 0;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && r.GetRow(row)) corpID = row.GetUInt(0); }
    if (corpID == 0) throw std::runtime_error("no corporation");
    if (hub == 0) { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT itemID FROM mapDenormalize WHERE groupID=15 LIMIT 1") && r.GetRow(row)) hub = row.GetUInt(0); }
    // fetch the plan's open jobs DEEPEST FIRST → engine FIFO runs them bottom-up (raw→pi→react→manufacture)
    struct J { uint32_t prod; uint64_t qty; std::string kind; uint32_t jobID; };
    std::vector<J> jobs;
    { DBQueryResult r; DBResultRow row;
      if (sDatabase.RunQuery(r, "SELECT jobID, productTypeID, qty, kind FROM vevProductionJobs WHERE corpID=%u AND planRoot=%u AND status='open' ORDER BY depth DESC, jobID", corpID, planRoot))
          while (r.GetRow(row)) jobs.push_back({row.GetUInt(1), row.GetUInt(2), row.GetText(3)?std::string(row.GetText(3)):"", row.GetUInt(0)}); }
    int enq = 0; char buf[200];
    for (auto& j : jobs) {
        bool ok = false;
        if (j.kind == "manufacture") {
            snprintf(buf, sizeof(buf), "{\"locationID\":%u,\"productTypeID\":%u,\"runs\":%llu}", hub, j.prod, (unsigned long long)j.qty);
            ok = enqueueAICommand(charID, "manufacture", buf);
        } else if (j.kind == "pi") {
            uint32_t outQ = 1; { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT quantity FROM piTypeMap WHERE isInput=0 AND typeID=%u LIMIT 1", j.prod) && r.GetRow(row)) { outQ = row.GetUInt(0); if (!outQ) outQ = 1; } }
            uint64_t runs = (j.qty + outQ - 1) / outQ;
            snprintf(buf, sizeof(buf), "{\"locationID\":%u,\"productTypeID\":%u,\"runs\":%llu}", hub, j.prod, (unsigned long long)runs);
            ok = enqueueAICommand(charID, "pi_transform", buf);
        } else if (j.kind == "react") {
            uint32_t outQ = 1; { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT quantity FROM invTypeReactions WHERE input=0 AND typeID=%u LIMIT 1", j.prod) && r.GetRow(row)) { outQ = row.GetUInt(0); if (!outQ) outQ = 1; } }
            uint64_t runs = (j.qty + outQ - 1) / outQ;
            snprintf(buf, sizeof(buf), "{\"locationID\":%u,\"productTypeID\":%u,\"runs\":%llu}", hub, j.prod, (unsigned long long)runs);
            ok = enqueueAICommand(charID, "react_transform", buf);
        } else {  // raw (mine/moon/pi_extract/buy) → corp extraction/market delivers to the hub
            snprintf(buf, sizeof(buf), "{\"locationID\":%u,\"typeID\":%u,\"qty\":%llu}", hub, j.prod, (unsigned long long)j.qty);
            ok = enqueueAICommand(charID, "vev_stock", buf);
        }
        if (ok) { DBerror e; sDatabase.RunQuery(e, "UPDATE vevProductionJobs SET status='done' WHERE jobID=%u", j.jobID); enq++; }
    }
    return json{{"enqueued", enq}, {"planRoot", planRoot}, {"hub", hub}, {"note", "queued bottom-up; engine runs FIFO. (react jobs need a corp tower — fulfill separately.)"}};
}

static json handleGetProductionJobs(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID")) throw std::runtime_error("payload requires { characterID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t corpID = 0;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && r.GetRow(row)) corpID = row.GetUInt(0); }
    json jobs = json::array();
    if (corpID) {
        DBQueryResult r; DBResultRow row;
        if (sDatabase.RunQuery(r, "SELECT jobID, productTypeID, qty, kind, director, status, planRoot FROM vevProductionJobs WHERE corpID=%u AND status='open' ORDER BY director, jobID", corpID))
            while (r.GetRow(row))
                jobs.push_back(json{{"jobID", row.GetUInt(0)}, {"productTypeID", row.GetUInt(1)}, {"name", vevTypeName(row.GetUInt(1))}, {"qty", row.GetUInt(2)}, {"kind", row.GetText(3)?std::string(row.GetText(3)):""}, {"director", row.GetText(4)?std::string(row.GetText(4)):""}, {"planRoot", row.GetUInt(6)}, {"planName", vevTypeName(row.GetUInt(6))}});
    }
    return json{{"jobs", jobs}};
}

// ─── VEV_PI_LOGI2: corp PI value/tax rollup for the fiscal dashboard ────────
static json handleGetPiLedger(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID"))
        throw std::runtime_error("payload requires { characterID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t corpID = 0;
    { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && r.GetRow(row)) corpID = row.GetUInt(0); }
    if (corpID == 0) return json{{"hauls", 0}, {"totalValue", 0}, {"totalTax", 0}, {"totalUnits", 0}, {"recent", json::array()}};
    json out;
    { DBQueryResult r; DBResultRow row;
      if (sDatabase.RunQuery(r, "SELECT COUNT(*), COALESCE(SUM(valueIsk),0), COALESCE(SUM(taxIsk),0), COALESCE(SUM(units),0) FROM vevPiLedger WHERE corpID=%u", corpID) && r.GetRow(row)) {
          out["hauls"] = row.GetUInt(0); out["totalValue"] = row.GetDouble(1); out["totalTax"] = row.GetDouble(2); out["totalUnits"] = row.GetUInt(3);
      } }
    json recent = json::array();
    { DBQueryResult r;
      if (sDatabase.RunQuery(r, "SELECT l.planetID, COALESCE(d.itemName,''), l.valueIsk, l.taxIsk, l.units, l.ts FROM vevPiLedger l LEFT JOIN mapDenormalize d ON d.itemID=l.planetID WHERE l.corpID=%u ORDER BY l.entryID DESC LIMIT 8", corpID)) {
          DBResultRow row;
          while (r.GetRow(row)) recent.push_back(json{{"planetID", row.GetUInt(0)}, {"planetName", row.GetText(1) ? std::string(row.GetText(1)) : ""}, {"value", row.GetDouble(2)}, {"tax", row.GetDouble(3)}, {"units", row.GetUInt(4)}, {"ts", row.GetInt64(5)}});
      } }
    out["recent"] = recent;
    return out;
}

// ─── VEV_PI_LOGI (LOGI-0): corp logistics job queue ─────────────────────────
static json handleGetLogiJobs(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID"))
        throw std::runtime_error("payload requires { characterID }");
    uint32_t charID = payload.at("characterID").get<uint32_t>();
    uint32_t corpID = 0;
    { DBQueryResult r; DBResultRow row;
      if (sDatabase.RunQuery(r, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && r.GetRow(row)) corpID = row.GetUInt(0); }
    if (corpID == 0) return json{{"jobs", json::array()}};

    // 1. gather haulable commodities per corp-colony planet (storage/launchpad pins)
    std::map<uint32_t, json> planetItems;
    std::map<uint32_t, uint32_t> planetSys;
    {
        DBQueryResult res;
        if (sDatabase.RunQuery(res,
            "SELECT p.planetID, p.solarSystemID, pc.typeID, SUM(pc.itemQty) qty, COALESCE(t.typeName,'') "
            "FROM piPlanets p "
            "JOIN chrCharacters c ON c.characterID=p.charID AND c.corporationID=%u "
            "JOIN piPins pin ON pin.ccPinID=p.ccPinID "
            "JOIN invTypes st ON st.typeID=pin.typeID AND st.groupID IN (1029,1030) "  // Storage Facilities + Spaceports = export points
            "JOIN piPinContents pc ON pc.pinID=pin.pinID "
            "LEFT JOIN invTypes t ON t.typeID=pc.typeID "
            "GROUP BY p.planetID, pc.typeID HAVING qty > 0", corpID)) {
            DBResultRow row;
            while (res.GetRow(row)) {
                uint32_t pid = row.GetUInt(0);
                planetSys[pid] = row.GetUInt(1);
                planetItems[pid].push_back(json{{"typeID", row.GetUInt(2)}, {"qty", row.GetUInt(3)},
                    {"name", row.GetText(4) ? std::string(row.GetText(4)) : ""}});
            }
        }
    }
    // create an extract job per planet w/ goods (UNIQUE KEY dedups open jobs)
    for (auto& kv : planetItems) {
        std::string items = kv.second.dump(), esc;
        sDatabase.DoEscapeString(esc, items);
        DBerror err;
        sDatabase.RunQuery(err,
            "INSERT IGNORE INTO vevLogiJobs (corpID,kind,planetID,solarSystemID,itemsJson,status,createdAt) "
            "VALUES (%u,'extract',%u,%u,'%s','open',UNIX_TIMESTAMP())", corpID, kv.first, planetSys[kv.first], esc.c_str());
    }
    // 2. return open/active jobs
    json jobs = json::array();
    DBQueryResult res;
    if (sDatabase.RunQuery(res,
        "SELECT j.jobID,j.kind,j.planetID,j.solarSystemID,j.itemsJson,j.status,j.assignedCharID,"
        "COALESCE(d.itemName,''),COALESCE(c.characterName,'') FROM vevLogiJobs j "
        "LEFT JOIN mapDenormalize d ON d.itemID=j.planetID "
        "LEFT JOIN chrCharacters c ON c.characterID=j.assignedCharID "
        "WHERE j.corpID=%u AND j.status<>'done' ORDER BY j.createdAt DESC", corpID)) {
        DBResultRow row;
        while (res.GetRow(row)) {
            json items = json::array();
            try { const char* it = row.GetText(4); if (it) items = json::parse(it); } catch (...) {}
            jobs.push_back(json{{"jobID", row.GetUInt(0)}, {"kind", row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"planetID", row.GetUInt(2)}, {"solarSystemID", row.GetUInt(3)}, {"items", items},
                {"status", row.GetText(5) ? std::string(row.GetText(5)) : ""}, {"assignedCharID", row.GetUInt(6)},
                {"planetName", row.GetText(7) ? std::string(row.GetText(7)) : ""}, {"assignedName", row.GetText(8) ? std::string(row.GetText(8)) : ""}});
        }
    }
    return json{{"jobs", jobs}};
}

// ─── VEV_BILLBOARD: the in-space content feed (surfaces + rotating items) ─────
// A billboard SURFACE is a screen placed in a system; it rotates through ITEMS
// whose targeting matches (system/region/cluster). 'data' items render live (the
// WANTED board is computed here from the CONCORD criminal flags + bounties);
// 'image' items carry an art URL. See docs/billboard-system-design.md.
static json vevWantedBoardJson() {
    json rows = json::array();
    DBQueryResult res;
    DBResultRow row;
    if (sDatabase.RunQuery(res,
        "SELECT c.characterID, COALESCE(c.characterName,''), COALESCE(b.amount,0), "
        "       COALESCE(cf.reason,'CONCORD'), COALESCE(sys.solarSystemName,''), "
        "       ROUND(COALESCE(c.securityRating,0),1), (cf.expiresAt - UNIX_TIMESTAMP()) "
        "FROM vevCriminalFlags cf "
        "JOIN chrCharacters c ON c.characterID = cf.characterID "
        "LEFT JOIN vevBounties b ON b.charID = cf.characterID "
        "LEFT JOIN mapSolarSystems sys ON sys.solarSystemID = cf.systemID "
        "WHERE cf.expiresAt > UNIX_TIMESTAMP() "
        "ORDER BY COALESCE(b.amount,0) DESC, cf.flaggedAt DESC LIMIT 16"))
    {
        while (res.GetRow(row)) {
            rows.push_back(json{
                {"characterID", row.GetUInt(0)},
                {"name",        row.GetText(1) ? std::string(row.GetText(1)) : ""},
                {"bounty",      row.GetInt64(2)},
                {"reason",      row.GetText(3) ? std::string(row.GetText(3)) : "CONCORD"},
                {"system",      row.GetText(4) ? std::string(row.GetText(4)) : ""},
                {"secStatus",   row.GetDouble(5)},
                {"expiresIn",   (int)row.GetInt(6)},
            });
        }
    }
    return rows;
}

// listBillboards { solarSystemID } -> { surfaces: [ { surfaceID, name, x, z,
//   items: [ { itemID, kind, title, body, render, artUrl, dataSpec, data? } ] } ] }
static json handleListBillboards(const json& payload) {
    const uint32_t sysID = payload.value("solarSystemID", 0u);
    if (sysID == 0) throw std::runtime_error("solarSystemID required");
    uint32_t regionID = 0;
    {
        DBQueryResult r; DBResultRow row;
        if (sDatabase.RunQuery(r, "SELECT regionID FROM mapSolarSystems WHERE solarSystemID = %u", sysID) && r.GetRow(row))
            regionID = row.GetUInt(0);
    }
    json surfaces = json::array();
    DBQueryResult sres;
    DBResultRow srow;
    if (sDatabase.RunQuery(sres,
        "SELECT surfaceID, name, x, z FROM vevBillboardSurfaces "
        "WHERE solarSystemID = %u AND enabled = 1 ORDER BY surfaceID", sysID))
    {
        while (sres.GetRow(srow)) {
            json surf = json{
                {"surfaceID", srow.GetUInt(0)},
                {"name",      srow.GetText(1) ? std::string(srow.GetText(1)) : "Billboard"},
                {"x",         srow.GetDouble(2)},
                {"z",         srow.GetDouble(3)},
            };
            json items = json::array();
            DBQueryResult ires; DBResultRow irow;
            if (sDatabase.RunQuery(ires,
                "SELECT itemID, kind, title, body, render, artUrl, dataSpec FROM vevBillboardItems "
                "WHERE (targetSystemID = 0 OR targetSystemID = %u) "
                "  AND (targetRegionID = 0 OR targetRegionID = %u) "
                "  AND (expiresAt = 0 OR expiresAt > UNIX_TIMESTAMP()) "
                "ORDER BY priority DESC, weight DESC, itemID", sysID, regionID))
            {
                while (ires.GetRow(irow)) {
                    const std::string dataSpec = irow.GetText(6) ? std::string(irow.GetText(6)) : "";
                    json item = json{
                        {"itemID",   irow.GetUInt(0)},
                        {"kind",     irow.GetText(1) ? std::string(irow.GetText(1)) : "news"},
                        {"title",    irow.GetText(2) ? std::string(irow.GetText(2)) : ""},
                        {"body",     irow.GetText(3) ? std::string(irow.GetText(3)) : ""},
                        {"render",   irow.GetText(4) ? std::string(irow.GetText(4)) : "data"},
                        {"artUrl",   irow.GetText(5) ? std::string(irow.GetText(5)) : ""},
                        {"dataSpec", dataSpec},
                    };
                    if (dataSpec == "wanted_board")
                        item["data"] = vevWantedBoardJson();
                    items.push_back(item);
                }
            }
            surf["items"] = items;
            surfaces.push_back(surf);
        }
    }
    return json{{"surfaces", surfaces}};
}


// ── VEV_CORP_HANGAR (2026-06-14): corp logistics for AI pilots. evemu's office
// rental (CorpStationMgr) + corp-hangar access are Client*-bound and unreachable by
// phantom AI pilots; these DB-direct verbs give them the same mechanic. A corp
// hangar = entity rows with ownerID=corpID, flag=4 (division 1), locationID=station
// — exactly the form the corp's own BPOs already use. rentOffice records the office
// (staOffices) so it shows in the client + is EVE-real.
static json handleRentOffice(const json& payload) {
    if (!payload.is_object() || !payload.contains("corporationID") || !payload.contains("stationID"))
        throw std::runtime_error("payload requires { corporationID, stationID }");
    uint32_t corpID = payload.at("corporationID").get<uint32_t>();
    uint32_t stationID = payload.at("stationID").get<uint32_t>();
    DBQueryResult ex;
    if (!sDatabase.RunQuery(ex, "SELECT itemID FROM staOffices WHERE corporationID = %u AND stationID = %u", corpID, stationID))
        throw std::runtime_error(std::string("DB error (office check): ") + ex.error.c_str());
    DBResultRow exr;
    if (ex.GetRow(exr))
        return json{{"rented", false}, {"officeID", exr.GetUInt(0)}, {"note", "office already exists at this station"}};
    DBQueryResult sres;
    if (!sDatabase.RunQuery(sres, "SELECT solarSystemID, stationTypeID, stationName FROM staStations WHERE stationID = %u", stationID))
        throw std::runtime_error(std::string("DB error (station): ") + sres.error.c_str());
    DBResultRow srow;
    if (!sres.GetRow(srow)) throw std::runtime_error("no such station");
    uint32_t systemID = srow.GetUInt(0);
    uint32_t stationTypeID = srow.GetUInt(1);
    std::string oname = std::string(srow.GetText(2)) + " Office";
    size_t qp = 0; while ((qp = oname.find('\'', qp)) != std::string::npos) { oname.replace(qp, 1, "''"); qp += 2; }
    uint32_t folderID = stationID + 6000000;   // STATION_OFFICE_OFFSET
    uint32_t officeID = 0;
    DBerror ierr;
    if (!sDatabase.RunQueryLID(ierr, officeID,
        "INSERT INTO entity (itemName, typeID, ownerID, locationID, flag, contraband, singleton, quantity, x, y, z, customInfo) "
        "VALUES ('%s', 27, %u, %u, 71, 0, 1, 1, 0, 0, 0, '')",
        oname.c_str(), corpID, stationID))
        throw std::runtime_error(std::string("DB error (office item): ") + ierr.c_str());
    DBerror oerr;
    if (!sDatabase.RunQuery(oerr,
        "INSERT INTO staOffices (itemID, name, officeFolderID, corporationID, stationID, solarSystemID, typeID, stationTypeID, flag, lockDown, rentalFee, expiryDateTime) "
        "VALUES (%u, '%s', %u, %u, %u, %u, 27, %u, 4, 0, 0, 0)",
        officeID, oname.c_str(), folderID, corpID, stationID, systemID, stationTypeID))
        throw std::runtime_error(std::string("DB error (office row): ") + oerr.c_str());
    return json{{"rented", true}, {"officeID", officeID}, {"officeFolderID", folderID}, {"stationID", stationID}};
}

// Withdraw a corp-hangar stack into the calling pilot's ship cargo. Corp-membership
// gated (must be in the corp that owns it). Ownership transfers to the carrier (she
// holds corp goods in her hold; depositCorpItem re-pools them at the destination).
static json handleLoadCorpItem(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("typeID") || !payload.contains("shipItemID"))
        throw std::runtime_error("payload requires { characterID, typeID, shipItemID }");
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t typeID = payload.at("typeID").get<uint32_t>();
    uint32_t shipItemID = payload.at("shipItemID").get<uint32_t>();
    DBQueryResult cres;
    if (!sDatabase.RunQuery(cres, "SELECT stationID, corporationID FROM chrCharacters WHERE characterID = %u", characterID))
        throw std::runtime_error(std::string("DB error (char): ") + cres.error.c_str());
    DBResultRow crow;
    if (!cres.GetRow(crow)) throw std::runtime_error("character not found");
    uint32_t stationID = crow.GetUInt(0);
    uint32_t corpID = crow.GetUInt(1);
    if (stationID == 0) throw std::runtime_error("must be docked to load from the corp hangar");
    DBQueryResult vres;
    if (!sDatabase.RunQuery(vres,
        "SELECT t.capacity FROM entity e JOIN invTypes t ON t.typeID = e.typeID JOIN invGroups g ON g.groupID = t.groupID "
        "WHERE e.itemID = %u AND e.ownerID = %u AND g.categoryID = 6 AND e.flag = 4 AND e.locationID = %u",
        shipItemID, characterID, stationID))
        throw std::runtime_error(std::string("DB error (hull): ") + vres.error.c_str());
    DBResultRow vrow;
    if (!vres.GetRow(vrow)) throw std::runtime_error("ship not found or not owned by character");
    double cargoCapacity = vrow.GetDouble(0);
    DBQueryResult ires;
    if (!sDatabase.RunQuery(ires,
        "SELECT COALESCE(SUM(t.volume * e.quantity), 0) AS vol, COUNT(*) AS n FROM entity e JOIN invTypes t ON t.typeID = e.typeID "
        "WHERE e.ownerID = %u AND e.typeID = %u AND e.flag = 4 AND e.locationID = %u",
        corpID, typeID, stationID))
        throw std::runtime_error(std::string("DB error (corp src): ") + ires.error.c_str());
    DBResultRow irow;
    double incoming = 0; int64_t n = 0;
    if (ires.GetRow(irow)) { incoming = irow.GetDouble(0); n = irow.GetInt64(1); }
    if (n == 0) throw std::runtime_error("no such item in the corp hangar at this station");
    DBQueryResult ures;
    if (!sDatabase.RunQuery(ures,
        "SELECT COALESCE(SUM(t.volume * e.quantity), 0) AS vol FROM entity e JOIN invTypes t ON t.typeID = e.typeID "
        "WHERE e.ownerID = %u AND e.locationID = %u AND e.flag = 5", characterID, shipItemID))
        throw std::runtime_error(std::string("DB error (used): ") + ures.error.c_str());
    DBResultRow urow;
    double used = 0;
    if (ures.GetRow(urow)) used = urow.GetDouble(0);
    if (used + incoming > cargoCapacity + 0.001)
        throw std::runtime_error("not enough room in the cargo hold for that corp stack");
    DBerror uerr;
    if (!sDatabase.RunQuery(uerr,
        "UPDATE entity SET ownerID = %u, flag = 5, locationID = %u WHERE ownerID = %u AND typeID = %u AND flag = 4 AND locationID = %u",
        characterID, shipItemID, corpID, typeID, stationID))
        throw std::runtime_error(std::string("DB error (load): ") + uerr.c_str());
    return json{{"loaded", true}, {"typeID", typeID}, {"stacks", n}};
}

// Deposit a stack from the pilot's personal hangar into the corp hangar (ownership
// to the corp, stays flag 4 at the station). Corp-membership gated.
static json handleDepositCorpItem(const json& payload) {
    if (!payload.is_object() || !payload.contains("characterID") || !payload.contains("typeID"))
        throw std::runtime_error("payload requires { characterID, typeID }");
    uint32_t characterID = payload.at("characterID").get<uint32_t>();
    uint32_t typeID = payload.at("typeID").get<uint32_t>();
    DBQueryResult cres;
    if (!sDatabase.RunQuery(cres, "SELECT stationID, corporationID FROM chrCharacters WHERE characterID = %u", characterID))
        throw std::runtime_error(std::string("DB error (char): ") + cres.error.c_str());
    DBResultRow crow;
    if (!cres.GetRow(crow)) throw std::runtime_error("character not found");
    uint32_t stationID = crow.GetUInt(0);
    uint32_t corpID = crow.GetUInt(1);
    if (stationID == 0) throw std::runtime_error("must be docked to deposit to the corp hangar");
    DBQueryResult ires;
    if (!sDatabase.RunQuery(ires,
        "SELECT COUNT(*) AS n FROM entity WHERE ownerID = %u AND typeID = %u AND flag = 4 AND locationID = %u",
        characterID, typeID, stationID))
        throw std::runtime_error(std::string("DB error (src): ") + ires.error.c_str());
    DBResultRow irow;
    int64_t n = 0;
    if (ires.GetRow(irow)) n = irow.GetInt64(0);
    if (n == 0) throw std::runtime_error("you have none of that in your hangar at this station");
    DBerror uerr;
    if (!sDatabase.RunQuery(uerr,
        "UPDATE entity SET ownerID = %u WHERE ownerID = %u AND typeID = %u AND flag = 4 AND locationID = %u",
        corpID, characterID, typeID, stationID))
        throw std::runtime_error(std::string("DB error (deposit): ") + uerr.c_str());
    return json{{"deposited", true}, {"typeID", typeID}, {"stacks", n}};
}

static const std::unordered_map<std::string, HandlerFn>& handlerTable() {
    static const std::unordered_map<std::string, HandlerFn> table = {
        {"login",                handleLogin},
        {"listBillboards",       handleListBillboards},
        {"createUnit",           handleCreateUnit},
        {"renameUnit",           handleRenameUnit},
        {"deleteUnit",           handleDeleteUnit},
        {"assignToUnit",         handleAssignToUnit},
        {"setUnitLeader",        handleSetUnitLeader},
        {"getMail",              handleGetMail},
        {"getMailBody",          handleGetMailBody},
        {"setMailStatus",        handleSetMailStatus},
        {"getContacts",          handleGetContacts},
        {"getCharacterDetails",  handleGetCharacterDetails},
        {"getTypeInfo",          handleGetTypeInfo},
        {"getAssets",            handleGetAssets},
        {"getWalletTransactions", handleGetWalletTransactions},
        {"getMyOrders",          handleGetMyOrders},
        {"getWalletJournal",     handleGetWalletJournal},
        {"listCharacters",       handleListCharacters},
        {"getCharacter",         handleGetCharacter},
        {"createCharacter",      handleCreateCharacter},
        // Vev-1.3 dock/undock control flow.
        {"undock",               handleUndock},
        {"dock",                 handleDock},
        {"listStationsInSystem", handleListStationsInSystem},
        {"getChannelMessages",   handleGetChannelMessages},
        {"moveItem",             handleMoveItem},
        {"rentOffice",            handleRentOffice},
        {"loadCorpItem",          handleLoadCorpItem},
        {"depositCorpItem",       handleDepositCorpItem},
        {"trashItem",            handleTrashItem},
        {"renameShip",           handleRenameShip},
        {"getShipContents",      handleGetShipContents},
        {"eject",                handleEject},
        {"boardShip",            handleBoardShip},
        {"listBookmarks",        handleListBookmarks},
        {"addBookmark",          handleAddBookmark},
        {"deleteBookmark",       handleDeleteBookmark},
        {"renameBookmark",       handleRenameBookmark},
        {"getMarketTree",        handleGetMarketTree},
        {"getMarketTypes",       handleGetMarketTypes},
        {"getMarketOrders",      handleGetMarketOrders},
        {"marketBuy",            handleMarketBuy},
        {"placeSellOrder",       handlePlaceSellOrder},   // VEV_PLACE_SELL
        {"getSignatures",        handleGetSignatures},    // VEV_XPL_SCAN
        {"warpToSignature",      handleWarpToSignature},  // VEV_XPL_SCAN
        {"analyze",              handleAnalyze},          // VEV_XPL_SCAN
        {"getStationGuests",     handleGetStationGuests},
        {"getLocalMembers",      handleGetLocalMembers},
        {"getStationAgents",     handleGetStationAgents},
        // Vev-1.4 navigation data: in-system view, region map, star map.
        // Read-only DB queries — actual warp / jump-gate mutations come
        // with the movement system in vev-2.x.
        {"getSystemSnapshot",    handleGetSystemSnapshot},
        {"getSystemPlanets",     handleGetSystemPlanets},   // VEV_PI_SCAN
        {"scanPlanet",           handleScanPlanet},         // VEV_PI_SCAN
        {"getColony",            handleGetColony},          // VEV_PI_COLONY
        {"colonyCmd",            handleColonyCmd},          // VEV_PI_COLONY
        {"getSchematics",        handleGetSchematics},      // VEV_PI_SCHEM
        {"getColonyNetwork",     handleGetColonyNetwork},   // VEV_PI_CORP
        {"getLogiJobs",          handleGetLogiJobs},        // VEV_PI_LOGI
        {"getPiLedger",          handleGetPiLedger},        // VEV_PI_LOGI2
        {"deriveSupplyChain",    handleDeriveSupplyChain},  // VEV_DERIVER
        {"executeSupplyChain",   handleExecuteSupplyChain}, // VEV_EXEC
        {"getProductionJobs",    handleGetProductionJobs},  // VEV_EXEC
        {"fulfillProductionJob", handleFulfillProductionJob}, // VEV_FULFILLPROD
        {"runProductionPlan",    handleRunProductionPlan},  // VEV_AUTODRAIN
        {"saveColonySpec",       handleSaveColonySpec},     // VEV_PI_LOGI1B
        {"getColonySpec",        handleGetColonySpec},      // VEV_PI_LOGI1B
        {"getRegionMap",         handleGetRegionMap},
        {"getUniverseMap",       handleGetUniverseMap},
        {"listAllRegions",       handleListAllRegions},
        // Science & Industry window (Neocom → Industry): installations +
        // active jobs + owned blueprints.
        {"getIndustry",          handleGetIndustry},
        {"getBlueprintDetail",   handleGetBlueprintDetail},
        {"getCharacterSheet",    handleGetCharacterSheet},
        // Ship fitting: the active ship's fitted modules by slot.
        {"getShipFitting",       handleGetShipFitting},
        {"getWreckContents",     handleGetWreckContents},   // VEV_LOOT_WRECK
        {"getShipType",          handleGetShipType},
        {"shipModuleCmd",        handleShipModuleCmd},
        // Items/Ships window: item hangar + ship hangar + active-ship cargo.
        {"getCharacterAssets",   handleGetCharacterAssets},
        // VEV_CORP_V1 — corp + corp-fleet suite (docs/corp-2d-ui-design.md).
        {"getCorporation",       handleGetCorporation},
        {"createCorporation",    handleCreateCorporation},
        {"requisitionPilot",     handleRequisitionPilot},
        {"adoptPilot",           handleAdoptPilot},
        {"listAdoptablePilots",  handleListAdoptablePilots},
        {"createCorpFleet",      handleCreateCorpFleet},
        {"setFleetComposition",  handleSetFleetComposition},
        {"autofillFleet",        handleAutofillFleet},
        {"disbandCorpFleet",     handleDisbandCorpFleet},
        {"disbandFleet",         handleDisbandFleet},
        {"setFleetMembership",   handleSetFleetMembership},
        {"setFleetAdvertised",   handleSetFleetAdvertised},
        {"listCorpFleets",       handleListCorpFleets},
        {"listFleetAdverts",     handleListFleetAdverts},
        {"joinFleet",            handleJoinFleet},
        {"leaveFleet",           handleLeaveFleet},
        // VEV_CORP_V2 — member management + careers + squads + scoped orders.
        {"setCorpLogo",          handleSetCorpLogo},
        {"transferIsk",          handleTransferIsk},
        {"listPilotSkills",      handleListPilotSkills},
        {"buySkillLevel",        handleBuySkillLevel},
        {"listCareers",          handleListCareers},
        {"createSquad",          handleCreateSquad},
        {"deleteSquad",          handleDeleteSquad},
        {"assignToSquad",        handleAssignToSquad},
        {"setFleetOrder",        handleSetFleetOrder},
        {"inviteToFleet",        handleInviteToFleet},   // VEV_FLEET_INVITE
        // VEV_FLEET_V4 — EVE-real fleet hierarchy (wings, FC/WC/SC, boss,
        // free-move), broadcasts, warp-to-member. Model: evemu src/eve-server/fleet/.
        {"formFleet",            handleFormFleet},
        {"createWing",           handleCreateWing},
        {"renameWing",           handleRenameWing},
        {"deleteWing",           handleDeleteWing},
        {"renameSquad",          handleRenameSquad},
        {"moveFleetMember",      handleMoveFleetMember},
        {"makeFleetLeader",      handleMakeFleetLeader},
        {"kickFleetMember",      handleKickFleetMember},
        {"setFleetMotd",         handleSetFleetMotd},
        {"setFleetFreeMove",     handleSetFleetFreeMove},
        {"renameFleet",          handleRenameFleet},
        {"sendFleetBroadcast",   handleSendFleetBroadcast},
        {"listFleetBroadcasts",  handleListFleetBroadcasts},
        {"warpToFleetMember",    handleWarpToFleetMember},
        {"setFleetAppointment",  handleSetFleetAppointment},
        {"setFleetReady",        handleSetFleetReady},
        // VEV_FLEET_DOCTRINE — the in-game doctrine + fleet-ops command layer.
        {"listDoctrineTemplates",     handleListDoctrineTemplates},
        {"getFleetDoctrine",          handleGetFleetDoctrine},
        {"bindFleetDoctrine",         handleBindFleetDoctrine},
        {"setFleetDoctrineRole",      handleSetFleetDoctrineRole},
        {"deleteFleetDoctrineRole",   handleDeleteFleetDoctrineRole},
        {"assignDoctrineRole",        handleAssignDoctrineRole},
        {"captureFitFromShip",        handleCaptureFitFromShip},
        {"saveFleetDoctrineAsTemplate", handleSaveFleetDoctrineAsTemplate},
        {"setFleetPhase",             handleSetFleetPhase},
        {"setFleetOps",               handleSetFleetOps},
        {"searchFitTypes",            handleSearchFitTypes},
        // VEV_CORP_V3 — certificate paths + training goals.
        {"listCertificates",     handleListCertificates},
        {"setTrainingGoal",      handleSetTrainingGoal},
        // Vev-2: warpTo, jumpGate, getCargo, getHangar,
        //        transferItem, repairItem, reprocess
    };
    return table;
}

static std::string dispatchFrame(const std::string& frame) {
    json req;
    try {
        req = json::parse(frame);
    } catch (const std::exception& e) {
        return makeError("?", nullptr, "BAD_JSON",
                         std::string("parse error: ") + e.what()).dump();
    }

    if (!req.is_object()) {
        return makeError("?", nullptr, "BAD_ENVELOPE",
                         "request must be a JSON object").dump();
    }

    const std::string type = req.value("type", "");
    const json id          = req.contains("id") ? req["id"] : json(nullptr);
    const json payload     = req.contains("payload") ? req["payload"] : json::object();

    if (type.empty()) {
        return makeError("?", id, "MISSING_TYPE",
                         "request envelope missing `type` field").dump();
    }

    auto it = handlerTable().find(type);
    if (it == handlerTable().end()) {
        return makeError(type, id, "UNKNOWN_TYPE",
                         "no handler registered for type=" + type).dump();
    }

    try {
        json result = it->second(payload);
        return makeResult(type, id, std::move(result)).dump();
    } catch (const std::exception& e) {
        return makeError(type, id, "BAD_PAYLOAD", e.what()).dump();
    }
}

// ─── Grid resolution helper ─────────────────────────────────────────────────
// Resolve which grid a character is on, for grid:enter. M1a: anchor on the
// character's docked station, else the system's first station. The grid-local
// origin is that anchor's 2D position (x, z from staStations). Returns false
// if the character / system / station can't be resolved.
static bool resolveGridForCharacter(uint32_t characterID,
                                    vev::grid::GridId& outGrid,
                                    vev::grid::Vec2& outAnchorPos,
                                    std::string& outAnchorName) {
    DBQueryResult cres;
    if (!sDatabase.RunQuery(cres,
        "SELECT solarSystemID, stationID FROM chrCharacters WHERE characterID = %u",
        characterID))
        return false;
    DBResultRow crow;
    if (!cres.GetRow(crow)) return false;
    const uint32_t systemID  = crow.GetUInt(0);
    const uint32_t stationID = crow.IsNull(1) ? 0 : crow.GetUInt(1);

    DBQueryResult sres;
    bool ok;
    if (stationID != 0) {
        ok = sDatabase.RunQuery(sres,
            "SELECT stationID, stationName, x, z FROM staStations WHERE stationID = %u",
            stationID);
    } else {
        // VEV_OBSERVER_LIVE_GRID: a flying phantom's TRUE position is in
        // DestinyManager memory; entity.x,z below is the stale last-SAVED
        // (station) position, so the DB anchor put the observer on the station
        // while the ship fought 270M km away. Resolve from the LIVE position
        // first (reuses nearestAnchor incl. group-885 anomaly beacons).
        {
            vev::grid::GridId lg; vev::grid::Vec2 lp; std::string ln;
            if (vev::grid::LiveGridForCharacter(characterID, lg, lp, ln)) {
                outGrid = lg; outAnchorPos = lp; outAnchorName = ln;
                return true;
            }
        }
        // Fallback (ship not in memory / mid-warp): nearest static celestial to
        // the last-saved position.
        // In space: anchor on the nearest celestial to the SHIP'S ACTUAL position
        // (belt/planet/moon/gate/station) so grid:enter lands on the grid where the
        // ship really is -- not always the station. THIS is the "logged in but see
        // nothing" fix: the ship sat at a belt while the client was put on the
        // station grid (ship 1.5 AU away, off-bubble). groupID 7=planet 8=moon
        // 9=belt 10=gate 15=station.
        double sx = 0, sz = 0;
        DBQueryResult pres;
        if (sDatabase.RunQuery(pres,
            "SELECT e.x, e.z FROM entity e JOIN chrCharacters c ON c.shipID = e.itemID "
            "WHERE c.characterID = %u", characterID)) {
            DBResultRow prow;
            if (pres.GetRow(prow)) { sx = prow.GetDouble(0); sz = prow.GetDouble(1); }
        }
        ok = sDatabase.RunQuery(sres,
            "SELECT itemID, itemName, x, z FROM mapDenormalize "
            "WHERE solarSystemID = %u AND groupID IN (7,8,9,10,15) "
            "ORDER BY (POW(x-%f,2)+POW(z-%f,2)) ASC LIMIT 1", systemID, sx, sz);
    }
    if (!ok) return false;
    DBResultRow srow;
    if (!sres.GetRow(srow)) return false;

    outGrid.solarSystemID = systemID;
    outGrid.anchorId      = srow.GetUInt(0);
    outAnchorName         = srow.GetText(1) ? std::string(srow.GetText(1)) : "";
    outAnchorPos          = { srow.GetDouble(2), srow.GetDouble(3) };
    return true;
}

// ─── Session ──────────────────────────────────────────────────────────────

// Char IDs this gateway process has logged in as a 2D phantom. Lets the
// one-session gate tell "our own phantom (reload / re-enter)" from "a real 3D
// client" — we can't call evemu's IsPhantomPlayer from the gateway. Never
// pruned: ownership is per gateway process-life. Accessed only on the single
// gateway io_context thread, so no lock needed.
static std::set<uint32_t> g_ownedChars;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket)
        : m_ws(std::move(socket)) {}

    ~Session() { leaveGrid(); }

    void Run() {
        m_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        m_ws.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(http::field::server, "vev-gateway/0.4");
            }));
        m_ws.async_accept(beast::bind_front_handler(&Session::OnAccept, shared_from_this()));
    }

    // Enqueue a text frame. Runs on the (single-threaded) gateway io_context —
    // called from OnRead AND from grid snapshot pushes (GridSession::pushSnapshot
    // runs on this thread via the projector's asio::post). One async_write in
    // flight at a time; the queue serializes so reads + pushes never overlap.
    void send(std::string msg) {
        m_writeQueue.push_back(std::move(msg));
        if (m_writeQueue.size() == 1) doWrite();
    }

private:
    void OnAccept(beast::error_code ec) {
        if (ec) {
            std::cerr << "[vev-gateway] accept: " << ec.message() << std::endl;
            return;
        }
        DoRead();
    }

    void DoRead() {
        m_ws.async_read(m_buffer,
            beast::bind_front_handler(&Session::OnRead, shared_from_this()));
    }

    void OnRead(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec == websocket::error::closed) { leaveGrid(); return; }
        if (ec) {
            std::cerr << "[vev-gateway] read: " << ec.message() << std::endl;
            leaveGrid();
            return;
        }
        const std::string frame = beast::buffers_to_string(m_buffer.data());
        m_buffer.consume(m_buffer.size());
        handleFrame(frame);
        DoRead();  // read loop decoupled from the write loop (push model)
    }

    void doWrite() {
        m_ws.text(true);
        m_ws.async_write(asio::buffer(m_writeQueue.front()),
            beast::bind_front_handler(&Session::OnWrite, shared_from_this()));
    }

    void OnWrite(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) {
            std::cerr << "[vev-gateway] write: " << ec.message() << std::endl;
            return;
        }
        m_writeQueue.pop_front();
        if (!m_writeQueue.empty()) doWrite();
    }

    // Grid lifecycle/nav are connection-aware (need `this`); everything else is
    // a stateless request→response handler via dispatchFrame.
    void handleFrame(const std::string& frame) {
        json req;
        try { req = json::parse(frame); }
        catch (...) { send(dispatchFrame(frame)); return; }  // dispatcher emits BAD_JSON
        if (!req.is_object()) { send(dispatchFrame(frame)); return; }
        const std::string type = req.value("type", "");
        if (type == vev::grid::events::enter) { onGridEnter(req); return; }
        if (type == vev::grid::events::exit)  { leaveGrid(); return; }
        if (type == vev::grid::events::nav)   { onGridNav(req); return; }
        send(dispatchFrame(frame));  // stateless path (login/getCharacter/undock/…)
    }

    void onGridEnter(const json& req) {
        if (vev::grid::g_gridManager == nullptr || vev::grid::g_gridSubs == nullptr) return;
        const json id      = req.contains("id") ? req["id"] : json(nullptr);
        const json payload = req.contains("payload") ? req["payload"] : json::object();
        const uint32_t characterID = payload.value("characterID", 0u);
        if (characterID == 0) return;
        // =================== VEV_OBSERVER_MODE ===================
        // observe:true = VIEW-ONLY attach (the CCTV/fleet-theater UI): stream
        // the grid the character is on WITHOUT logging it in, owning it, or
        // refusing because it is already online (an engine-driven AI pilot
        // ALWAYS is — that is the thing being watched).
        const bool observe = payload.value("observe", false);
        // =================== end VEV_OBSERVER_MODE ===================

        // One session per pilot: refuse if already in the game (3D client or
        // another browser). Cleared on clean logout + on server restart.
        if (!observe) {
            DBQueryResult ores;
            if (sDatabase.RunQuery(ores,
                "SELECT online FROM chrCharacters WHERE characterID = %u", characterID)) {
                DBResultRow orow;
                if (ores.GetRow(orow) && orow.GetUInt(0) != 0
                    && g_ownedChars.find(characterID) == g_ownedChars.end()) {
                    // Online and NOT our own phantom → a real 3D client. Refuse.
                    send(makeError(std::string(vev::grid::events::enter), id, "ALREADY_ONLINE",
                        "This pilot is already in the game (3D Crucible client). "
                        "Only one session per pilot.").dump());
                    return;
                }
            }
        }

        vev::grid::GridId grid;
        vev::grid::Vec2   anchorPos;
        std::string       anchorName;
        if (!resolveGridForCharacter(characterID, grid, anchorPos, anchorName)) {
            std::cerr << "[vev-gateway] grid:enter cannot resolve grid for char "
                      << characterID << std::endl;
            send(makeError(std::string(vev::grid::events::enter), id, "NO_GRID",
                "could not resolve a grid for this character").dump());
            return;
        }

        leaveGrid();  // drop any prior grid before joining the new one
        m_characterID = characterID;
        m_grid        = grid;
        m_onGrid      = true;
        m_observe     = observe;   // VEV_OBSERVER_MODE

        vev::grid::GridMember member;
        member.characterId = characterID;
        std::weak_ptr<Session> weak = weak_from_this();
        member.send = [weak](const std::string& f) {
            if (auto s = weak.lock()) s->send(f);
        };
        // VEV_REANCHOR_WIRE: keep this connection's m_grid in step with streamer
        // re-anchors (the member moves between grids as it warps/jumps), so
        // leaveGrid removes it from the grid it is ACTUALLY on, not the one it
        // entered on.
        member.setOwnerGrid = [weak](const vev::grid::GridId& g) {
            if (auto s = weak.lock()) s->m_grid = g;
        };

        auto& session = vev::grid::g_gridManager->getOrCreate(grid);
        session.addMember(std::move(member));
        vev::grid::g_gridSubs->addMember(grid, anchorPos, anchorName, characterID);  // VEV_REANCHOR_WIRE
        session.sendLatestTo(characterID);  // last produced frame, if any

        // Unified-world presence so the 3D Crucible client sees this pilot.
        // Docked -> login_docked (station guest + Local); in space ->
        // login_in_space (warp-in). Once per session; balanced by leaveGrid().
        if (!m_loggedIn && !observe) {   // VEV_OBSERVER_MODE: observers never own presence
            m_loggedIn = true;
            uint32_t st = 0;
            DBQueryResult sres;
            if (sDatabase.RunQuery(sres,
                "SELECT stationID FROM chrCharacters WHERE characterID = %u", characterID)) {
                DBResultRow srow;
                if (sres.GetRow(srow) && !srow.IsNull(0)) st = srow.GetUInt(0);
            }
            enqueueAICommand(characterID, st != 0 ? "login_docked" : "login_in_space", "{}");
            g_ownedChars.insert(characterID);
        }

        // Ack so the client's grid:enter promise resolves (snapshots stream
        // separately as id-less pushes).
        send(makeResult(std::string(vev::grid::events::enter), id, json{
            {"ok", true},
            {"grid", {{"solarSystemID", grid.solarSystemID}, {"anchorId", grid.anchorId}}},
            {"anchorName", anchorName}}).dump());
    }

    // grid:nav -> ai_command_queue. moveTo carries a SYSTEM-coordinate target
    // (x,z); goto2d keeps the ship's current Y (in-plane). stop halts.
    void onGridNav(const json& req) {
        if (m_observe) return;   // VEV_OBSERVER_MODE: view-only — observers issue NO commands
        const json payload = req.contains("payload") ? req["payload"] : json::object();
        // characterID from the PAYLOAD (not the Session's m_characterID): nav
        // must work even if the WS reconnected and this Session never re-ran
        // grid:enter (so m_characterID/m_onGrid are unset). Snapshots survive a
        // reconnect (pushed to the grid member); nav was the casualty.
        const uint32_t navChar = payload.value("characterID", 0u);
        if (navChar == 0) return;
        const std::string kind = payload.value("kind", std::string());
        if (kind == "stop") {
            enqueueAICommand(navChar, "stop", "{}");
            return;
        }
        if (kind == "moveTo" && payload.contains("target")) {
            const json t = payload["target"];
            const double x = t.value("x", 0.0);
            const double z = t.value("z", 0.0);
            enqueueAICommand(navChar, "goto2d", json{{"x", x}, {"z", z}}.dump());
            return;
        }
        if (kind == "setVector") {
            const double dx = payload.value("dirX", 0.0);
            const double dz = payload.value("dirZ", 0.0);
            const double f  = payload.value("fraction", 0.0);
            enqueueAICommand(navChar, "set_vector",
                json{{"dirX", dx}, {"dirZ", dz}, {"fraction", f}}.dump());
            return;
        }
        if (kind == "setSpeed") {
            const double f = payload.value("fraction", 0.0);
            enqueueAICommand(navChar, "set_speed_fraction", json{{"fraction", f}}.dump());
            return;
        }
        if (kind == "dock" && payload.contains("stationId")) {
            const uint32_t st = payload.value("stationId", 0u);
            if (st != 0) enqueueAICommand(navChar, "dock_at", json{{"stationID", st}}.dump());
            return;
        }
        if (kind == "warpTo" && payload.contains("targetId")) {
            // targetId = the selected entity's evemu itemID (string). The
            // server's warp_to resolves it (SystemEntity or mapDenormalize) to
            // the TRUE 3D position — not the lossy 2D x/z — then DestinyManager
            // warps. precisionM -> stop distance (default 10km). warp_to itself
            // enforces the ~150km minimum-warp rule.
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            if (targetID != 0) {
                const int distance = (int)payload.value("precisionM", 10000.0);
                enqueueAICommand(navChar, "warp_to", json{{"targetID", targetID}, {"distance", distance}}.dump());
            }
            return;
        }
        // VEV lock spine: ctrl-click on the 2D grid -> evemu lock_target.
        // Resolves the grid id (entity itemID) and initiates targeting (range +
        // max-locked enforced server-side); the 2D client draws the reticle
        // optimistically meanwhile. Same verb the mine flow uses.
        if (kind == "lockTarget" && payload.contains("targetId")) {
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            if (targetID != 0)
                enqueueAICommand(navChar, "lock_target", json{{"targetID", targetID}}.dump());
            return;
        }
        // VEV_UNLOCK_TARGET: drop a lock (the 2D target card's X).
        if (kind == "unlockTarget" && payload.contains("targetId")) {
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            if (targetID != 0)
                enqueueAICommand(navChar, "unlock_target", json{{"targetID", targetID}}.dump());
            return;
        }
        // VEV_DRONE: launch all drones currently in the drone bay (no target).
        if (kind == "launchDrones") {
            enqueueAICommand(navChar, "launch_drones", json::object().dump());
            return;
        }
        // VEV_DRONE: order launched drones to engage a grid target.
        if (kind == "droneEngage" && payload.contains("targetId")) {
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            if (targetID != 0)
                enqueueAICommand(navChar, "drone_engage", json{{"targetID", targetID}}.dump());
            return;
        }
        // VEV_DRONE: recall launched drones to the bay.
        if (kind == "droneReturn") {
            enqueueAICommand(navChar, "drone_return", json::object().dump());
            return;
        }
        // VEV player mining: lock the roid + fire the ship's mining laser so the
        // operator can test the beam themselves. Finds the mining-laser hi-slot
        // from the fit; enqueues lock_target then activate_module.
        if (kind == "mine" && payload.contains("targetId")) {
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            if (targetID != 0) {
                uint32_t slotFlag = 0;
                DBQueryResult sres;
                if (sDatabase.RunQuery(sres,
                    "SELECT e.flag FROM entity e"
                    " JOIN invTypes t ON t.typeID = e.typeID"
                    " JOIN invGroups g ON g.groupID = t.groupID"
                    " WHERE e.locationID = (SELECT shipID FROM chrCharacters WHERE characterID = %u)"
                    "   AND g.groupID IN (54,55,464,483,737) AND e.flag BETWEEN 27 AND 34 LIMIT 1", navChar)) {
                    DBResultRow srow;
                    if (sres.GetRow(srow)) slotFlag = srow.GetUInt(0);
                }
                enqueueAICommand(navChar, "lock_target", json{{"targetID", targetID}}.dump());
                if (slotFlag != 0)
                    enqueueAICommand(navChar, "activate_module", json{{"targetID", targetID}, {"slotFlag", slotFlag}}.dump());
            }
            return;
        }
        // VEV_A2_MINING_CONTROLS: real player mining = lock -> approach/orbit into
        // range -> activate the laser. lockTarget + mine already exist above; these add
        // the approach + explicit module-activate the operator drives by hand.
        if (kind == "orbit" && payload.contains("targetId")) {
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            uint32_t range = (uint32_t)payload.value("range", 5000);
            if (targetID != 0)
                enqueueAICommand(navChar, "orbit", json{{"targetID", targetID}, {"range", range}}.dump());
            return;
        }
        if (kind == "approach" && payload.contains("targetId")) {
            // VEV_NAV_FOLLOW (2026-06-12): Approach = continuous pursuit via the
            // engine's DestinyManager::Follow (50 m stop range, EVE-real) — the
            // old one-shot align_to blew straight past moving targets.
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            uint32_t range = (uint32_t)payload.value("range", 50);
            if (targetID != 0)
                enqueueAICommand(navChar, "follow", json{{"targetID", targetID}, {"range", range}}.dump());
            return;
        }
        // VEV_LOOT_WRECK: selective wreck looting from the wreck-cargo window.
        if (kind == "lootItem" && payload.contains("wreckId")) {
            const uint32_t wreckID = (uint32_t)strtoul(payload.value("wreckId", std::string()).c_str(), nullptr, 10);
            json lp = {{"wreckID", wreckID}};
            if (payload.contains("itemId")) lp["itemID"] = (uint32_t)payload.value("itemId", 0);
            else lp["all"] = true;
            if (wreckID != 0)
                enqueueAICommand(navChar, "loot_item", lp.dump());
            return;
        }
        if (kind == "activateModule" && payload.contains("targetId")) {
            const std::string tid = payload.value("targetId", std::string());
            const uint32_t targetID = (uint32_t)strtoul(tid.c_str(), nullptr, 10);
            uint32_t slotFlag = (uint32_t)payload.value("slotFlag", 0);
            if (slotFlag == 0) {  // resolve the ship's mining-laser hi-slot from the fit
                DBQueryResult sres;
                if (sDatabase.RunQuery(sres,
                    "SELECT e.flag FROM entity e"
                    " JOIN invTypes t ON t.typeID = e.typeID"
                    " JOIN invGroups g ON g.groupID = t.groupID"
                    " WHERE e.locationID = (SELECT shipID FROM chrCharacters WHERE characterID = %u)"
                    "   AND g.groupID IN (54,55,464,483,737) AND e.flag BETWEEN 27 AND 34 LIMIT 1", navChar)) {
                    DBResultRow srow;
                    if (sres.GetRow(srow)) slotFlag = srow.GetUInt(0);
                }
            }
            if (targetID != 0 && slotFlag != 0)
                { /* VEV_PARTIAL_YIELD_BRIDGE */ json ap = {{"targetID", targetID}, {"slotFlag", slotFlag}}; if (payload.contains("fraction")) ap["fraction"] = payload.value("fraction", 1.0); enqueueAICommand(navChar, "activate_module", ap.dump()); }
            return;
        }
        // VEV_STACK_OP_BRIDGE: inventory split/merge from the Ship Contents UI.
        if (kind == "stackOp") {
            json sp;
            sp["itemID"] = (uint32_t)payload.value("itemId", 0);
            if (payload.contains("withItemId")) { sp["withItemID"] = (uint32_t)payload.value("withItemId", 0); sp["merge"] = true; }
            if (payload.contains("qty")) sp["qty"] = (uint32_t)payload.value("qty", 0);
            enqueueAICommand(navChar, "stack_op", sp.dump());
            return;
        }
        if (kind == "reloadAmmo" && payload.contains("slotFlag")) {
            // VEV_RELOAD: in-space ammo swap — bridges to the reload_ammo verb.
            const uint32_t raFlag = (uint32_t)payload.value("slotFlag", 0);
            const uint32_t raType = (uint32_t)payload.value("chargeTypeID", 0);
            if (raFlag != 0)
                enqueueAICommand(navChar, "reload_ammo", json{{"slotFlag", raFlag}, {"chargeTypeID", raType}}.dump());
            return;
        }
        if (kind == "lootWreck" && payload.contains("targetId")) {
            // VEV_LOOT_WRECK: the 2D Loot window's bridge → the loot_wreck verb.
            const std::string lwTid = payload.value("targetId", std::string());
            const uint32_t lwID = (uint32_t)strtoul(lwTid.c_str(), nullptr, 10);
            if (lwID != 0)
                enqueueAICommand(navChar, "loot_wreck", json{{"targetID", lwID}}.dump());
            return;
        }
        if (kind == "jump" && payload.contains("gateId")) {
            // VEV_GRID_JUMP: gateId = the stargate's evemu itemID (string), the
            // gate the ship is sitting at. The server's `jump` enforces the
            // <2500m proximity rule, resolves the destination via mapJumps, and
            // recreates the ship in the linked system. The streamer re-anchor
            // then sees the system change and re-anchors onto the dest gate.
            const std::string gid = payload.value("gateId", std::string());
            const uint32_t gateID = (uint32_t)strtoul(gid.c_str(), nullptr, 10);
            if (gateID != 0) {
                enqueueAICommand(navChar, "jump", json{{"gateID", gateID}}.dump());
            }
            return;
        }
    }

    void leaveGrid() {
        if (m_loggedIn) {
            // Clear online IMMEDIATELY (synchronous) so a browser reload can
            // re-enter without tripping the ALREADY_ONLINE gate before the
            // queued logout_pilot (full Local/guest teardown) runs. Only the
            // 2D browser's own phantom session runs leaveGrid, so this never
            // clears a real 3D client.
            DBerror lerr;
            sDatabase.RunQuery(lerr, "UPDATE chrCharacters SET online = 0 WHERE characterID = %u", m_characterID);
            enqueueAICommand(m_characterID, "logout_pilot", "{}");
            m_loggedIn = false;
        }
        if (!m_onGrid) return;
        m_onGrid = false;
        if (vev::grid::g_gridManager == nullptr) return;
        if (auto* s = vev::grid::g_gridManager->find(m_grid)) {
            s->removeMember(m_characterID);
            if (s->empty()) vev::grid::g_gridManager->dropIfEmpty(m_grid);
        }
        // VEV_REANCHOR_WIRE: member-aware — drop THIS pilot from the projection
        // Entry's member set; the Entry self-drops when its last member leaves
        // (a shared grid survives one pilot leaving it).
        if (vev::grid::g_gridSubs != nullptr) vev::grid::g_gridSubs->removeMember(m_grid, m_characterID);
    }

    websocket::stream<tcp::socket> m_ws;
    beast::flat_buffer             m_buffer;
    std::deque<std::string>        m_writeQueue;
    uint32_t                       m_characterID = 0;
    vev::grid::GridId              m_grid{};
    bool                           m_onGrid = false;
    bool                           m_loggedIn = false;
    bool                           m_observe = false;   // VEV_OBSERVER_MODE: view-only session
};

// ─── Listener ─────────────────────────────────────────────────────────────

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& ioc, tcp::endpoint endpoint)
        : m_ioc(ioc), m_acceptor(ioc) {
        beast::error_code ec;
        m_acceptor.open(endpoint.protocol(), ec);
        if (ec) { Fail(ec, "open"); return; }
        m_acceptor.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) { Fail(ec, "set_option"); return; }
        m_acceptor.bind(endpoint, ec);
        if (ec) { Fail(ec, "bind"); return; }
        m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) { Fail(ec, "listen"); return; }
        m_ok = true;
    }

    bool Ok() const { return m_ok; }

    void Run() {
        DoAccept();
    }

private:
    void DoAccept() {
        m_acceptor.async_accept(asio::make_strand(m_ioc),
            beast::bind_front_handler(&Listener::OnAccept, shared_from_this()));
    }

    void OnAccept(beast::error_code ec, tcp::socket socket) {
        if (ec) {
            Fail(ec, "accept");
        } else {
            std::make_shared<Session>(std::move(socket))->Run();
        }
        DoAccept();
    }

    void Fail(beast::error_code ec, const char* what) {
        std::cerr << "[vev-gateway] " << what << ": " << ec.message() << std::endl;
    }

    asio::io_context&  m_ioc;
    tcp::acceptor      m_acceptor;
    bool               m_ok = false;
};

// ─── Gateway::Impl ────────────────────────────────────────────────────────

struct Gateway::Impl {
    asio::io_context           ioc{1};
    std::shared_ptr<Listener>  listener;
    std::thread                worker;
    std::atomic<bool>          running{false};
};

Gateway::Gateway() : m_impl(std::make_unique<Impl>()) {}

Gateway::~Gateway() { Stop(); }

bool Gateway::Start(uint16_t port) {
    if (m_impl->running.load()) return true;

    auto address = asio::ip::make_address("0.0.0.0");
    m_impl->listener = std::make_shared<Listener>(
        m_impl->ioc, tcp::endpoint{address, port});

    if (!m_impl->listener->Ok()) {
        m_impl->listener.reset();
        return false;
    }

    m_impl->listener->Run();
    m_impl->running.store(true);

    // Wire the tactical-grid layer. The gateway owns these globals' lifetime
    // (function-static = constructed once, no leak). The eve-server-side
    // projector (GridStreamer.cpp, main thread) reads g_gridSubs + posts
    // snapshots to g_gatewayIoc; grid:enter registers subscribers in
    // g_gridManager. All grid-side access happens on this io_context thread.
    static vev::grid::GridManager       s_gridManager;
    static vev::grid::GridSubscriptions s_gridSubs;
    vev::grid::g_gatewayIoc  = &m_impl->ioc;
    vev::grid::g_gridManager = &s_gridManager;
    vev::grid::g_gridSubs    = &s_gridSubs;

    m_impl->worker = std::thread([this]() {
        try {
            m_impl->ioc.run();
        } catch (const std::exception& ex) {
            std::cerr << "[vev-gateway] io_context::run exception: "
                      << ex.what() << std::endl;
        }
    });

    std::cout << "[vev-gateway] listening on ws://0.0.0.0:" << port
              << " (vev-1.2: real chrCharacters queries)" << std::endl;
    return true;
}

void Gateway::Stop() {
    if (!m_impl->running.exchange(false)) return;

    m_impl->ioc.stop();
    if (m_impl->worker.joinable()) m_impl->worker.join();
    m_impl->listener.reset();

    std::cout << "[vev-gateway] stopped" << std::endl;
}

bool Gateway::IsRunning() const {
    return m_impl->running.load();
}

} // namespace vev
