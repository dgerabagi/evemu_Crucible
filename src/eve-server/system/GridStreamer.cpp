// GridStreamer.cpp — main-thread 2D projector for Vev's unified-world grid.
//
// LIVES IN eve-server (not vev-gateway) on purpose: it reads eve-server
// state — EntityList / SystemManager / SystemEntity / DestinyManager — which
// the gateway static lib cannot see (dep direction is eve-server → vev-gateway
// → eve-core). It calls INTO the gateway (GridSubscriptions, g_gridManager,
// asio::post) — the legal direction. See vev-gateway/GridStreamer.h for the
// design + threading contract, and `Game Design - Tactical Grid Protocol.md`.
//
// Route A (unified world): evemu owns the authoritative sim; this projects it
// to 2D and ships it to browser clients. Runs on the MAIN thread (hooked into
// EntityList::Process after ProcessAICommandQueue), so iterating SystemManager
// entities is race-free. Only the finished, value-copied GridSnapshot crosses
// to the gateway thread, via asio::post — no shared mutable evemu state is
// touched off-thread.

#include "eve-server.h"

#include <algorithm>        // VEV_EWAR_MULTI: std::remove_if over the per-ship beam list

#include "GridStreamer.h"   // vev-gateway (on eve-server's include path per CMake)
#include "GridSession.h"    // vev-gateway

#include "EntityList.h"
#include "system/SystemManager.h"
#include "system/SystemEntity.h"
#include "system/DestinyManager.h"
#include "ship/Ship.h"   // VEV ship/capacitor workstream: live cap read
#include "npc/AIShipSE.h"   // VEV_REANCHOR_LOGIC: FindAIShip -> AIShipSE* (complete type)
#include "ship/modules/ModuleManager.h"   // VEV mining beam: active hi-slot scan
#include "ship/modules/GenericModule.h"   // VEV mining beam: IsMiningLaser/GetTargetID
#include "EVE_Effects.h"                   // VEV mining beam: EVEEffectID::hiPower

#include <boost/asio/post.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>   // VEV_PHANTOM_FILTER: per-tick docked/offline owner set

namespace vev::grid {

namespace {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Map an evemu SystemEntity to the grid wire `kind`. Returns nullptr for
// entities we don't surface on a 2D grid (modules, missiles, fields, …) so
// the caller can skip them.
const char* kindForEntity(SystemEntity* se) {
    if (se->IsShipSE() || se->IsAIShipSE()) return "ship";
    if (se->IsStationSE())                  return "station";
    if (se->IsGateSE())                     return "stargate";
    if (se->IsPlanetSE())                   return "planet";
    if (se->IsMoonSE())                     return "moon";
    if (se->IsBeltSE())                     return "belt";
    if (se->IsAsteroidSE())                 return "asteroid"; // VEV_GRID_ASTEROID_KIND
    if (se->IsWreckSE())                    return "wreck";
    if (se->IsDroneSE())                    return "drone";
    if (se->IsNPCSE())                      return "npc";
    if (se->IsTowerSE())                    return "tower";      // VEV_POS_GRID: POS control tower
    if (se->IsReactorSE() || se->IsArraySE()) return "structure"; // VEV_POS_GRID: silo/harvester/reactor/array
    if (se->IsCOSE())                       return "customs";    // VEV_POS_GRID: customs office (POCO)
    // VEV_XPL_GRID (2026-06-16): cosmic-site objects so a pilot is VISIBLY at a
    // relic/data site instead of on an empty grid -- hackable cans + the site
    // beacon + ruined structures. ContainerSE / CelestialSE; classify the
    // celestials by item group (502 Cosmic Signature beacon, 226 Large Collidable
    // Object ruins). Reached only for otherwise-unclassified entities (cheap).
    if (se->IsContainerSE())                return "container";
    if (se->IsCelestialSE()) {
        InventoryItemRef self = se->GetSelf();
        if (self.get() != nullptr) {
            const uint16 grp = self->groupID();
            if (grp == 502) return "site";        // Cosmic Signature beacon
            if (grp == 226) return "structure";   // Large Collidable Object (ruins)
            if (grp == 306) return "container";   // Spawn Container (relic/data site cans)
        }
    }
    return nullptr;
}

// Per-grid monotonic tick counter (contract: "monotonic tick counter for this
// grid session"). Keyed by gridKey; main-thread-only, so no lock.
std::unordered_map<std::string, uint64_t>& tickCounters() {
    static std::unordered_map<std::string, uint64_t> counters;
    return counters;
}

// VEV beam: recent single-shot AI mining (ship itemID -> {roid itemID, expiryMs}).
// Main-thread-only (activate_module handler + StreamGridsTick both run there).
std::unordered_map<uint32_t, std::pair<uint32_t, int64_t>>& recentMining() {
    static std::unordered_map<uint32_t, std::pair<uint32_t, int64_t>> m;
    return m;
}
constexpr int64_t kMiningBeamWindowMs = 35000;  // > the ~25s AI mining cycle -> beam stays lit

// VEV_WEAPON_FIRE: recent weapon fire (ship itemID -> {target, typeID, groupID,
// expiry}). Main-thread-only, twin of recentMining above.
struct RecentFire { uint32_t targetID; uint32_t typeID; uint32_t groupID; int64_t expiry; };
std::unordered_map<uint32_t, RecentFire>& recentWeaponFire() {
    static std::unordered_map<uint32_t, RecentFire> m;
    return m;
}
constexpr int64_t kWeaponFireWindowMs = 8000;  // a few weapon cycles; refreshed per shot

// VEV_EWAR_FIRE: per-ship set of EWAR/utility beams (web/painter/scram/reps) so
// they never compete with weapon tracers for one field (curator 2026-06-16).
// VEV_EWAR_MULTI: a LIST per ship (one RecentFire per module group) so a ship
// running a web AND a painter shows BOTH beams — a single slot let the painter
// (last writer) clobber the web every tick ("no stasis webifier", reported 4x).
std::unordered_map<uint32_t, std::vector<RecentFire>>& recentEwarFire() {
    static std::unordered_map<uint32_t, std::vector<RecentFire>> m;
    return m;
}

// VEV_MODULE_ACTIVE: recently-activated SELF/TOGGLE modules (reps/hardeners/AB/DCU)
// that have no target beam -> the CCTV pulses the slot. typeID+groupID, ~10s window
// (refreshed per cycle / re-affirm). targetID field unused (0).
std::unordered_map<uint32_t, std::vector<RecentFire>>& recentSelfModule() {
    static std::unordered_map<uint32_t, std::vector<RecentFire>> m;
    return m;
}
constexpr int64_t kSelfModuleWindowMs = 10000;

// VEV_GATE_FIRE: recent stargate jumps, keyed by GATE itemID -> server ms of the jump,
// held briefly so every 2D client + the CCTV flash the gate. Twin of recentMining.
std::unordered_map<uint32_t, int64_t>& recentGateFire() {
    static std::unordered_map<uint32_t, int64_t> m;  // gateID -> firedAtMs
    return m;
}
constexpr int64_t kGateFireWindowMs = 3000;
constexpr int64_t kGateJumpChoreoMs = 5000;  // VEV_GATE_CHOREO: dest gate fires ~5s after the source

// VEV_REANCHOR_LOGIC: nearest anchorable celestial (station/gate/planet/moon)
// to a 2D point, scanning the system's live entity map (already main-thread-
// owned). Out-params get the winner; returns false if no celestial. Belts are
// deliberately NOT candidates: the 2D client has no belt in its LocalSystem
// model (anchorPosFor can't resolve one), and asteroids/belts are a deferred
// cycle — a belt anchor would offset the whole bubble client-side.
// VEV_RADIUS_BUBBLE: 2D distance from a celestial's CENTRE to the warp-in point
// ships drop at. The grid bubble is sized to this + 250km so everyone who warps
// to the body lands on ONE grid. For planets this matches evemu's native warp-in
// (BeyonceService): radius·(s+1)+1e6. Stations/gates/moons drop ~at the surface,
// so the radius itself is a good approximation. Client-independent.
double warpInRadius2D(SystemEntity* se) {
    const double r = se->GetRadius();
    if (se->IsPlanetSE()) {
        double s = 20.0 * std::pow(0.025 * (10.0 * std::log10(r / 1.0e6) - 39.0), 20.0) + 0.5;
        if (s < 0.5)  s = 0.5;
        if (s > 10.5) s = 10.5;
        return r * (s + 1.0) + 1.0e6;
    }
    if (se->IsBeltSE()) return (r > 100000.0 ? r : 100000.0);  // VEV: belt grid spans the ~100km roid field, not the belt-centre radius
    if (se->GetGroupID() == 885 || se->GetGroupID() == 502) return 100000.0;  // VEV_ANCHOR_ANOMALY + VEV_ANCHOR_SIG (502 relic/data): site pocket spans rooms + warp drop
    return r;
}

bool nearestAnchor(SystemManager* sm, const GPoint& from,
                   uint32_t& outId, Vec2& outPos, std::string& outName, double& outWarpR) {
    double bestSq = 0.0; bool found = false;
    for (auto& [itemID, se] : sm->GetEntities()) {
        if (se == nullptr) continue;
        // VEV_ANCHOR_ANOMALY: cosmic-anomaly sig beacons (invGroups 885) are
        // warp destinations for the explore career — without them as anchor
        // candidates, an observer/member warping to a site never re-anchored
        // (the stuck-at-station CCTV, curator-observed 2026-06-12).
        // VEV_ANCHOR_SIG (2026-06-16): relic/data Cosmic Signature beacons (group
        // 502) are warp destinations too. Group 885 made combat anomalies anchor,
        // but a relic/data explorer warping to a 502 sig never re-anchored -> the
        // pinned CCTV stayed on a distant celestial and showed an empty grid
        // ("I check Pathfinder and see nothing", curator 2026-06-16).
        const bool vevAnomaly = (se->GetGroupID() == 885 || se->GetGroupID() == 502);
        if (!(se->IsStationSE() || se->IsGateSE() || se->IsPlanetSE()
              || se->IsMoonSE() || se->IsBeltSE() || vevAnomaly)) continue;  // VEV: belts are warp anchors -> their grid carries the roids + ships parked there
        const GPoint& p = se->GetPosition();
        const double dx = p.x - from.x, dz = p.z - from.z;   // 2D (drop Y)
        const double dsq = dx * dx + dz * dz;
        if (!found || dsq < bestSq) {
            bestSq = dsq; found = true;
            outId    = se->GetID();
            outPos   = { p.x, p.z };
            outName  = se->GetName() ? se->GetName() : "";
            outWarpR = warpInRadius2D(se);   // VEV_RADIUS_BUBBLE
        }
    }
    return found;
}

// VEV_REANCHOR_LOGIC: gateway-thread re-anchor — move a member onto a new grid
// in BOTH registries (sessions keep the same connection; projection re-centers).
// Posted from StreamGridsTick. Shared-grid: if others are already on newG, the
// mover joins them and they see each other.
void reanchorMember(uint32_t cid, const GridId& oldG, const GridId& newG,
                    const Vec2& newPos, const std::string& newName) {
    if (g_gridManager != nullptr) g_gridManager->moveMember(cid, oldG, newG);
    if (g_gridSubs    != nullptr) g_gridSubs->moveMember(oldG, newG, newPos, newName, cid);
}

}  // namespace

// VEV_OBSERVER_LIVE_GRID: see GridStreamer.h. Reuses nearestAnchor (which
// knows group-885 anomaly beacons via VEV_ANCHOR_ANOMALY) from the ship's LIVE
// position. Mid-warp -> false (no grid; the tunnel), so the caller falls back.
bool LiveGridForCharacter(uint32_t charID, GridId& outGrid, Vec2& outPos, std::string& outName) {
    AIShipSE* ship = sEntityList.FindAIShip(charID);
    if (ship == nullptr) return false;
    SystemManager* sm = ship->SystemMgr();
    if (sm == nullptr) return false;
    if (ship->DestinyMgr() != nullptr && ship->DestinyMgr()->IsWarping()) return false;
    const GPoint pos = ship->GetPosition();
    uint32_t nId = 0; Vec2 nPos; std::string nName; double nWarpR = 0.0;
    if (!nearestAnchor(sm, pos, nId, nPos, nName, nWarpR)) return false;
    outGrid.solarSystemID = sm->GetID();
    outGrid.anchorId      = nId;
    outPos                = nPos;
    outName               = nName;
    return true;
}

void StreamGridsTick() {
    if (g_gridSubs == nullptr || g_gatewayIoc == nullptr) return;

    const std::vector<GridSubscriptions::Entry> subs = g_gridSubs->snapshot();
    if (subs.empty()) return;

    // VEV_PHANTOM_FILTER (2026-06-05): SQL-only zombie recovery
    //   UPDATE chrCharacters SET stationID=<sta>, online=0 WHERE characterID=...
    // bypasses the docking handler, so the SystemEntity stays in SystemManager
    // forever and we keep streaming a frozen ship with stale velocity. Build a
    // per-tick owner-skip set so one filter covers both true zombies AND
    // SQL-docked pilots. DB cost: one indexed query / 1Hz tick.
    std::unordered_set<uint32_t> phantomOwners;
    {
        DBQueryResult phRes;
        if (sDatabase.RunQuery(phRes,
                "SELECT characterID FROM chrCharacters WHERE stationID != 0 OR online = 0")) {
            DBResultRow phRow;
            while (phRes.GetRow(phRow)) phantomOwners.insert(phRow.GetUInt(0));
        }
    }

    const int64_t t = nowMs();
    for (const auto& sub : subs) {
        SystemManager* sm = sEntityList.FindOrBootSystem(sub.grid.solarSystemID);
        if (sm == nullptr) continue;
        // VEV_RADIUS_BUBBLE: bubble = the anchor's warp-in radius + 250km, so a
        // planet's whole warp-in sphere (where ships drop) is ONE grid — not a
        // flat 250km of its centre (which a planet dwarfs).
        SystemEntity* anchorSE = sm->GetSE(sub.grid.anchorId);
        const double aWarpR = (anchorSE != nullptr) ? warpInRadius2D(anchorSE) : kGridRadiusM;
        const double projRadiusSq = (aWarpR + kGridRadiusM) * (aWarpR + kGridRadiusM);

        GridSnapshot snap;
        snap.grid         = sub.grid;
        snap.serverTimeMs = t;
        snap.tick         = ++tickCounters()[gridKey(sub.grid)];
        snap.anchorName   = sub.anchorName;
        // VEV_ANCHOR_SELF: for a moving ship-anchor (the deep-void self-anchor) project
        // against the LIVE ship position so the pilot stays centred as it cruises; for a
        // static celestial this equals sub.anchorPos (it does not move).
        Vec2 originPos = sub.anchorPos;
        if (anchorSE != nullptr && (anchorSE->IsAIShipSE() || anchorSE->IsShipSE())) {
            const GPoint& vevAp = anchorSE->GetPosition();
            originPos = { vevAp.x, vevAp.z };
        }
        snap.anchorPos    = originPos;   // TRUE anchor system pos for the client

        // GetEntities() returns a COPY of the system's entity map; safe to walk
        // on the main thread (we own the world here).
        for (auto& [itemID, se] : sm->GetEntities()) {
            if (se == nullptr) continue;
            const char* kind = kindForEntity(se);
            if (kind == nullptr) continue;

            // VEV_PHANTOM_FILTER: skip ships whose pilot row is DB-docked or offline.
            if ((se->IsShipSE() || se->IsAIShipSE())
                && phantomOwners.count(se->GetOwnerID()) > 0) continue;

            // Project 3D→2D, grid-local (origin = the grid anchor): drop Y,
            // subtract the anchor's system position from x/z.
            const GPoint& p = se->GetPosition();
            const double localX = p.x - originPos.x;
            const double localZ = p.z - originPos.z;

            // Off-grid filter: skip anything beyond the 250km bubble (you'd
            // warp to it). The anchor itself sits at (0,0) → always included.
            if (localX * localX + localZ * localZ > projRadiusSq) continue;

            GridEntity e;
            e.id      = std::to_string(se->GetID());
            e.kind    = kind;
            e.typeId  = se->GetTypeID();
            e.name    = se->GetName() ? se->GetName() : "";
            e.pos     = { localX, localZ };
            e.radius  = se->GetRadius();
            // VEV_GATE_FIRE: a stargate recently jumped-through -> flash it on every client.
            if (std::string(kind) == "stargate") {
                auto itG = recentGateFire().find((uint32_t)se->GetID());
                if (itG != recentGateFire().end()) {
                    const int64_t gfAge = nowMs() - itG->second;  // VEV_GATE_CHOREO
                    if (gfAge >= 0 && gfAge < kGateFireWindowMs) e.gateFiringMs = (double)itG->second;
                    else if (gfAge >= kGateFireWindowMs) recentGateFire().erase(itG);
                }
            }

            DestinyManager* dm = se->DestinyMgr();
            if (dm != nullptr) {
                const GVector& v = dm->GetVelocity();
                e.vel = { v.x, v.z };
                // VEV_REAL_HEADING (2026-06-15): stream evemu's ACTUAL ship facing
                // (m_shipHeading, which _Turn() rotates gradually at the hull's real
                // agility rate) instead of deriving heading from velocity -- which
                // SNAPS the instant the ship accelerates out of warp-align. Fall back
                // to the velocity direction only when the facing is degenerate.
                const GVector& fh = dm->GetHeading();
                if (fh.x * fh.x + fh.z * fh.z > 1e-6) {
                    e.heading = std::atan2(fh.z, fh.x);
                } else {
                    const double speedSq = e.vel.x * e.vel.x + e.vel.z * e.vel.z;
                    if (speedSq > 1e-4) e.heading = std::atan2(e.vel.z, e.vel.x);
                }
            }

            if (se->IsShipSE() || se->IsAIShipSE()) {
                e.ownerCharacterId = se->GetOwnerID();
                e.travelMode = (dm != nullptr && dm->IsWarping()) ? "warp" : "subwarp";
                // VEV_WARP_HUD: serialize warp telemetry + gate cloak so the CCTV
                // observer can render the real warp readout (speed/distance/ETA) + cloak.
                if (dm != nullptr && dm->IsWarping()) {
                    SerializedWarpState ws;
                    GPoint wd = dm->GetWarpDest();
                    // target PROJECTED to grid-local (same frame as e.pos = p - originPos)
                    ws.target = { wd.x - originPos.x, wd.z - originPos.z };
                    const GVector& wv = dm->GetVelocity();
                    ws.speedMs = std::sqrt(wv.x*wv.x + wv.y*wv.y + wv.z*wv.z);
                    double total = dm->GetWarpTotalDist();
                    double remain = se->GetPosition().distance(wd);  // system coords -> frame-free
                    ws.distanceRemainingM = remain;
                    ws.warpSpeedMs = dm->GetWarpSpeedMs();
                    ws.etaS = dm->GetWarpRemainingS();  // VEV_WARP_ETA: real remaining warp time
                    double pf = (total > 1.0) ? (total - remain) / total : 0.0;
                    if (pf < 0.0) pf = 0.0; if (pf > 1.0) pf = 1.0;
                    ws.progressFrac = pf;
                    ws.alignProgressFrac = 1.0;
                    ws.phase = (pf < 0.12) ? "accelerate" : (pf > 0.88) ? "decelerate" : "cruise";
                    e.warp = ws;
                }
                if (dm != nullptr && dm->IsCloaked()) e.cloaked = true;
                // VEV_JUMP_GRACE: jump invuln (SE-side) + gate-cloak countdown for the CCTV
                if (se->IsInvul()) e.invuln = true;
                if (AIShipSE* aiSE = se->GetAIShipSE()) {
                    double cr = aiSE->GetCloakRemainingMs();
                    if (cr > 0.0) e.cloakRemainingMs = cr;
                }
                // VEV_MINING_WARP_CLEAR: entering warp drops all active modules
                // (EVE) -- wipe the recent-mining beam so it doesn't linger in/after warp.
                if (dm != nullptr && dm->IsWarping()) recentMining().erase((uint32_t)se->GetID());
                // VEV_WF: do NOT force-erase weapon/ewar fire on warp — IsWarping()
                // mis-reports on a just-warped phantom mid-combat and wiped every shot
                // before it streamed (curator 5x). Let the ~8s expiry clear it.
                // (was: if (IsWarping) recentWeaponFire().erase(...))
                // VEV_LOCKED_TARGETS: server-confirmed locks for the 2D target cards.
                if (se->TargetMgr() != nullptr) {
                    PyList* tl = se->TargetMgr()->GetTargets();
                    if (tl != nullptr) {
                        if (!tl->empty()) {
                            std::vector<std::string> locks;
                            for (size_t ti = 0; ti < tl->size(); ++ti) {
                                const int64 lockId = PyRep::IntegerValue(tl->GetItem(ti));
                                if (lockId > 0) locks.push_back(std::to_string((uint32)lockId));
                            }
                            if (!locks.empty()) e.lockedTargetIds = std::move(locks);
                        }
                        PyDecRef(tl);
                    }
                }
                // Live, server-authoritative capacitor (ship/capacitor workstream).
                // Ship item ref for BOTH a Client*-piloted ShipSE AND an AIShipSE
                // phantom. GetShipSE() is null for phantoms (they are a
                // DynamicSystemEntity, NOT a ShipSE), so the old GetShipSE()-gated
                // block never ran for agent miners -> the harvest beam + capacitor
                // never reached the 2D client. GetSelf() is the wrapped ship item
                // for both kinds; cast it to ShipItemRef (proven: SystemManager.cpp).
                ShipItemRef ship;
                if (ShipSE* sse = se->GetShipSE())        ship = sse->GetShipItemRef();
                else if (se->GetSelf().get() != nullptr)  ship = ShipItemRef::StaticCast(se->GetSelf());
                if (ship.get() != nullptr) {
                    e.capacitor    = ship->GetShipCapacitorLevel();
                    e.capacitorMax = ship->GetAttribute(AttrCapacitorCapacity).get_float();
                    // VEV_STREAM_SHIP_HP: live shield/armor/hull so the 2D HUD shows real
                    // tank (was 0 -- only cap was streamed). Mirrors get_self_tank_state
                    // (EntityList.cpp): armor/hull are stored as DAMAGE, current = max - dmg.
                    {
                        const float aMax = ship->GetAttribute(AttrArmorHP).get_float();
                        const float hMax = ship->GetAttribute(AttrHP).get_float();
                        e.shieldMax = ship->GetAttribute(AttrShieldCapacity).get_float();
                        e.shield    = ship->GetAttribute(AttrShieldCharge).get_float();
                        e.armorMax  = aMax;
                        e.armor     = aMax - ship->GetAttribute(AttrArmorDamage).get_float();
                        e.hullMax   = hMax;
                        e.hull      = hMax - ship->GetAttribute(AttrDamage).get_float();
                    }
                    // VEV mining beam: surface the roid this ship is actively mining
                    // so the 2D client draws the harvest beam + loops the SFX. An
                    // active hi-slot MiningLaser carries its target roid.
                    if (ship->HasModuleManager()) {
                        std::vector<GenericModule*> activeHi;
                        ship->GetModuleManager()->GetActiveModules(EVEEffectID::hiPower, activeHi);
                        for (GenericModule* mod : activeHi) {
                            if (mod != nullptr && mod->IsMiningLaser()) {
                                const uint32 tid = mod->GetTargetID();
                                if (tid != 0) { e.miningTargetId = std::to_string(tid); break; }
                            }
                        }
                    }
                }
                // VEV beam: AI mining is single-shot (no persistent ActiveModule), so
                // the scan above finds nothing for agent miners. Fall back to the
                // recent-mining window the activate_module handler records, so the 2D
                // client draws the harvest beam continuously across the ~25s cycles.
                if (e.miningTargetId.has_value() == false) {
                    const uint32_t sid = static_cast<uint32_t>(se->GetID());
                    auto itM = recentMining().find(sid);
                    if (itM != recentMining().end()) {
                        if (itM->second.second >= t) e.miningTargetId = std::to_string(itM->second.first);
                        else recentMining().erase(itM);
                    }
                }
                // VEV_WEAPON_FIRE: surface recent weapon fire (single-shot like AI
                // mining) so every 2D client draws this ship's tracers/beams.
                {
                    const uint32_t sidW = static_cast<uint32_t>(se->GetID());
                    auto itW = recentWeaponFire().find(sidW);
                    if (itW != recentWeaponFire().end()) {
                        if (itW->second.expiry >= t) {
                            e.weaponTargetId = std::to_string(itW->second.targetID);
                            e.weaponTypeId   = itW->second.typeID;
                            e.weaponGroupId  = itW->second.groupID;
                        } else recentWeaponFire().erase(itW);
                    }
                    auto itE = recentEwarFire().find(sidW);
                    if (itE != recentEwarFire().end()) {
                        auto& beams = itE->second;
                        // drop expired beams; emit every live one (web + painter…)
                        beams.erase(std::remove_if(beams.begin(), beams.end(),
                                    [t](const RecentFire& f){ return f.expiry < t; }),
                                    beams.end());
                        if (beams.empty()) {
                            recentEwarFire().erase(itE);
                        } else {
                            e.ewarFx.clear();
                            for (const auto& f : beams)
                                e.ewarFx.push_back({ std::to_string(f.targetID), f.groupID, f.typeID });
                            // PRIMARY (back-compat single field) = first live beam
                            e.ewarTargetId = std::to_string(beams.front().targetID);
                            e.ewarTypeId   = beams.front().typeID;
                            e.ewarGroupId  = beams.front().groupID;
                        }
                    }
                    // VEV_MODULE_ACTIVE: self/toggle modules (reps/hardeners/AB) —
                    // no beam, surfaced so the CCTV fitting panel pulses the slot.
                    auto itSM = recentSelfModule().find(sidW);
                    if (itSM != recentSelfModule().end()) {
                        auto& mods = itSM->second;
                        mods.erase(std::remove_if(mods.begin(), mods.end(),
                                   [t](const RecentFire& f){ return f.expiry < t; }), mods.end());
                        if (mods.empty()) recentSelfModule().erase(itSM);
                        else { e.activeModules.clear();
                               for (auto& f : mods) e.activeModules.push_back({ f.typeID, f.groupID }); }
                    }
                }
            }

            // VEV_STREAM_NPC_HP: rats stream live tank too (the block above is
            // ship-only) -> rat health shells + HP-truth impact FX on the 2D
            // client. Same attrs the ship path reads; damage attrs may be unset
            // until first hit -> guarded, default 0.
            if (se->IsNPCSE() && se->GetSelf().get() != nullptr) {
                InventoryItemRef npcSelf = se->GetSelf();
                const float aMaxN = npcSelf->HasAttribute(AttrArmorHP) ? npcSelf->GetAttribute(AttrArmorHP).get_float() : 0.0f;
                const float hMaxN = npcSelf->HasAttribute(AttrHP) ? npcSelf->GetAttribute(AttrHP).get_float() : 0.0f;
                const float aDmgN = npcSelf->HasAttribute(AttrArmorDamage) ? npcSelf->GetAttribute(AttrArmorDamage).get_float() : 0.0f;
                const float hDmgN = npcSelf->HasAttribute(AttrDamage) ? npcSelf->GetAttribute(AttrDamage).get_float() : 0.0f;
                e.shieldMax = npcSelf->HasAttribute(AttrShieldCapacity) ? npcSelf->GetAttribute(AttrShieldCapacity).get_float() : 0.0f;
                e.shield    = npcSelf->HasAttribute(AttrShieldCharge) ? npcSelf->GetAttribute(AttrShieldCharge).get_float() : 0.0f;
                e.armorMax  = aMaxN;
                e.armor     = aMaxN - aDmgN;
                e.hullMax   = hMaxN;
                e.hull      = hMaxN - hDmgN;
                // VEV_WEAPON_FIRE: rats' shots stream too (NoteWeaponFire is
                // called from NPCAIMgr::AttackTarget).
                {
                    const uint32_t sidN = static_cast<uint32_t>(se->GetID());
                    auto itN = recentWeaponFire().find(sidN);
                    if (itN != recentWeaponFire().end()) {
                        if (itN->second.expiry >= t) {
                            e.weaponTargetId = std::to_string(itN->second.targetID);
                            e.weaponTypeId   = itN->second.typeID;
                            e.weaponGroupId  = itN->second.groupID;
                        } else recentWeaponFire().erase(itN);
                    }
                }
            }

            snap.entities.push_back(std::move(e));
        }

        // Hand the finished snapshot to the gateway thread. The session lookup
        // happens THERE (g_gridManager is gateway-thread-owned), so we never
        // touch the session map off-thread.
        const GridId gid = sub.grid;
        boost::asio::post(*g_gatewayIoc, [gid, snap = std::move(snap)]() {
            if (g_gridManager == nullptr) return;
            if (GridSession* s = g_gridManager->find(gid)) {
                s->pushSnapshot(snap);
            }
        });

        // VEV_REANCHOR_LOGIC: follow each member to the celestial it is now
        // nearest to. Moves the member's subscription onto that celestial's grid
        // (shared: co-located pilots land on the same grid + see each other) and
        // catches a cross-system jump (ship's live system != the grid's). Posted
        // to the gateway thread, same handoff as the snapshot. Off-grid/mid-warp
        // (>250km from the candidate) → no move, like EVE's warp tunnel; the
        // member re-anchors on arrival. `sub` is a copy; moves take effect next tick.
        for (uint32_t cid : sub.members) {
            AIShipSE* ship = sEntityList.FindAIShip(cid);
            if (ship == nullptr) continue;
            // VEV_WARP_FOLLOW: the observed pilot warps across bubbles off the pinned
            // source grid -> the CCTV froze on the last accel tick ("488 km/s, 161 Mm,
            // 0s") while the real (fast) cruise + arrival happened off-grid. The engine
            // warp is correct + first-class; only the spectator camera couldn't follow.
            // So during warp, anchor the grid on the SHIP ITSELF (same pattern as the
            // deep-space VEV_ANCHOR_SELF below) -> the grid follows the warp tunnel and
            // the live warp block (ramping speed + counting-down ETA) streams the whole
            // way. Anchoring on the ship (not a celestial) = no mid-warp flicker; the
            // normal celestial re-anchor resumes on arrival (IsWarping() false).
            const bool vevWarpFollow = (ship->DestinyMgr() != nullptr && ship->DestinyMgr()->IsWarping());
            SystemManager* shipSm = ship->SystemMgr();
            if (shipSm == nullptr) continue;
            const uint32_t liveSys = shipSm->GetID();
            const GPoint shipPos = ship->GetPosition();
            uint32_t nId = 0; Vec2 nPos; std::string nName; double nWarpR = 0.0;
            if (vevWarpFollow) {
                nId = ship->GetID(); nPos = { shipPos.x, shipPos.z };
                nWarpR = kGridRadiusM; nName = ship->GetName() ? ship->GetName() : "";
            } else if (!nearestAnchor(shipSm, shipPos, nId, nPos, nName, nWarpR)) {
                continue;
            }
            // Is the nearest celestial/beacon within its warp-in bubble? (Checked BEFORE
            // gridChanged so a pilot drifting OFF its current anchor's bubble -- which
            // keeps the same nearest celestial, so gridChanged would be false -- still
            // re-anchors instead of stranding invisible on a stale grid.)
            const double rdx = nPos.x - shipPos.x, rdz = nPos.z - shipPos.z;
            const double arriveSq = (nWarpR + kGridRadiusM) * (nWarpR + kGridRadiusM);
            if (rdx * rdx + rdz * rdz > arriveSq) {
                // VEV_ANCHOR_SELF (2026-06-16): off-bubble from EVERY celestial/beacon =
                // the pilot is in the deep-space void (scanning between sites, drifting
                // post-warp). Don't strand the pinned CCTV on a stale distant celestial
                // (curator: "I check Pathfinder and see nothing") -- anchor the grid on
                // the SHIP ITSELF so the observed pilot is always centred + visible. The
                // projection below uses the live ship position for ship-anchored grids.
                nId = ship->GetID();
                nPos = { shipPos.x, shipPos.z };
                nWarpR = kGridRadiusM;
                nName = ship->GetName() ? ship->GetName() : "";
            }
            const bool gridChanged =
                (liveSys != sub.grid.solarSystemID) || (nId != sub.grid.anchorId);
            if (!gridChanged) continue;
            const GridId oldGrid = sub.grid;
            const GridId newGrid{ liveSys, nId };
            const Vec2 newPos = nPos;
            const std::string newName = nName;
            boost::asio::post(*g_gatewayIoc, [cid, oldGrid, newGrid, newPos, newName]() {
                reanchorMember(cid, oldGrid, newGrid, newPos, newName);
            });
        }
    }
}

void NoteMining(uint32_t shipID, uint32_t targetID) {
    recentMining()[shipID] = { targetID, nowMs() + kMiningBeamWindowMs };
}

// VEV_GATE_FIRE: a ship jumped -- flash BOTH the source gate (as it leaves) and the
// destination gate (as it arrives; that grid is the observer's next anchor).
void NoteGateJump(uint32_t sourceGateID, uint32_t destGateID) {
    const int64_t now = nowMs();
    if (sourceGateID) recentGateFire()[sourceGateID] = now;
    if (destGateID)   recentGateFire()[destGateID]   = now + kGateJumpChoreoMs;  // VEV_GATE_CHOREO
}

// VEV_MODULE_ACTIVE: record a self/toggle module activation (no target beam).
void NoteSelfModule(uint32_t shipID, uint32_t moduleTypeID, uint32_t moduleGroupID) {
    auto& list = recentSelfModule()[shipID];
    const int64_t exp = nowMs() + kSelfModuleWindowMs;
    for (auto& r : list)
        if (r.typeID == moduleTypeID && r.groupID == moduleGroupID) { r.expiry = exp; return; }
    list.push_back({ 0, moduleTypeID, moduleGroupID, exp });
}

// Module groups that are UTILITY/EWAR (web/scram/ECM/painter + remote reps/energy
// transfer). They call NoteWeaponFire to draw their own beam, but the per-ship
// fire record has ONE slot, so every cycle they CLOBBERED a real weapon's tracer
// (curator 2026-06-16: "I only see the target painter laser, no gun fire"). A
// utility note must NOT overwrite a still-live WEAPON note -- the gun tracers win.
static inline bool isUtilityFireGroup(uint32_t g) {
    switch (g) {
        case 52:   // Warp Scrambler
        case 65:   // Stasis Web
        case 201:  // ECM
        case 379:  // Target Painter
        case 41:   // Shield Transfer (remote)
        case 325:  // Remote Armor Repairer
        case 67:   // Energy Transfer
        case 68:   // Remote Hull Repairer
            return true;
        default: return false;   // turrets (53/55/74) + launchers + rat shots (0) = weapon
    }
}

void NoteWeaponFire(uint32_t shipID, uint32_t targetID, uint32_t weaponTypeID, uint32_t weaponGroupID) {
    // Route to SEPARATE per-ship slots so weapon TRACERS and EWAR/utility BEAMS
    // never compete for one field (curator 2026-06-16, reported 4x: "I only see
    // the target painter laser, no gun fire"). The 2D client reads both streams
    // and renders both -- guns AND web AND painter.
    RecentFire rec = { targetID, weaponTypeID, weaponGroupID, nowMs() + kWeaponFireWindowMs };
    if (isUtilityFireGroup(weaponGroupID)) {
        // VEV_EWAR_MULTI: upsert by module group so web (65) and painter (379)
        // co-exist instead of overwriting each other — refresh the matching
        // group's expiry, or append a new beam.
        auto& beams = recentEwarFire()[shipID];
        bool found = false;
        for (auto& f : beams) {
            if (f.groupID == weaponGroupID) { f = rec; found = true; break; }
        }
        if (!found) beams.push_back(rec);
    } else {
        recentWeaponFire()[shipID] = rec;
    }
}

// VEV_SIM_PAINTER registry (targetID -> {sigMult, expiry}).
static std::unordered_map<uint32_t, std::pair<float, int64_t>> s_vevPainted;
void NotePainted(uint32_t targetID, float sigMult, int64_t durationMs) {
    s_vevPainted[targetID] = { sigMult, nowMs() + durationMs };
}
float GetPaintMult(uint32_t targetID) {
    auto it = s_vevPainted.find(targetID);
    if (it == s_vevPainted.end()) return 1.0f;
    if (it->second.second < nowMs()) { s_vevPainted.erase(it); return 1.0f; }
    return it->second.first;
}

}  // namespace vev::grid
