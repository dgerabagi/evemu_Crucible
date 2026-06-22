/*
    ------------------------------------------------------------------------------------
    LICENSE:
    ------------------------------------------------------------------------------------
    This file is part of EVEmu: EVE Online Server Emulator
    Copyright 2006 - 2021 The EVEmu Team
    For the latest information visit https://evemu.dev
    ------------------------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by the Free Software
    Foundation; either version 2 of the License, or (at your option) any later
    version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License along with
    this program; if not, write to the Free Software Foundation, Inc., 59 Temple
    Place - Suite 330, Boston, MA 02111-1307, USA, or go to
    http://www.gnu.org/copyleft/lesser.txt.
    ------------------------------------------------------------------------------------
    Author:     AI Agent System
*/

// See A321 §4.2 + §4.6 (AI Command Queue — headless player ship entity)
// Modeled after NPC.cpp and Drone.cpp: server-controlled DynamicSystemEntity.

#include "eve-server.h"

#include "EntityList.h"
#include "npc/AIShipSE.h"
#include "npc/Drone.h"               // VEV_DRONE: DroneSE
#include "inventory/AttributeEnum.h" // VEV_DRONE: AttrDroneBandwidth*
#include "system/Damage.h"
#include "system/SystemManager.h"
#include "system/SystemBubble.h"
#include "Client.h"
#include "map/MapDB.h"
#include "system/Container.h"
#include "inventory/ItemFactory.h"
#include "inventory/Inventory.h"
#include "account/AccountService.h"  // VEV_SIM_AISHIPTAIL_INC: TransferFunds (insurance payout)
#include "ship/Ship.h"               // VEV_SIM_AISHIPTAIL_INC: ShipSE recharge reference + invGroups::Rookieship
#include "ship/ShipDB.h"             // VEV_SIM_AISHIPTAIL_INC: Get/DeleteShipInsurance
#include "EVE_Corp.h"                // VEV_SIM_AISHIPTAIL_INC: corpSCC
#include "EVE_Wallet.h"              // VEV_SIM_AISHIPTAIL_INC: Journal::EntryType::Insurance

#define AISHIP_PROCESS_TICK_MS 5000

AIShipSE::AIShipSE(InventoryItemRef ship, EVEServiceManager& services, SystemManager* system,
                     const FactionData& data, uint32 charID, const char* charName,
                     float securityStatus, float bounty)
: DynamicSystemEntity(ship, services, system),
  m_charID(charID),
  m_charName(charName),
  m_securityStatus(securityStatus),
  m_bounty(bounty),
  m_processTimerTick(AISHIP_PROCESS_TICK_MS),
  m_processTimer(AISHIP_PROCESS_TICK_MS)
{
    m_warID = data.factionID;
    m_allyID = data.allianceID;
    m_corpID = data.corporationID;
    m_ownerID = data.ownerID;

    // Initialize attributes (same pattern as NPC/DroneSE constructors)
    m_self->SetAttribute(AttrInertia,             EvilOne, false);
    m_self->SetAttribute(AttrDamage,              EvilZero, false);
    m_self->SetAttribute(AttrArmorDamage,         EvilZero, false);
    m_self->SetAttribute(AttrWarpCapacitorNeed,   0.00001, false);
    m_self->SetAttribute(AttrMass,                m_self->type().mass(), false);
    m_self->SetAttribute(AttrRadius,              m_self->type().radius(), false);
    m_self->SetAttribute(AttrVolume,              m_self->type().volume(), false);
    m_self->SetAttribute(AttrCapacity,            m_self->type().capacity(), false);
    m_self->SetAttribute(AttrShieldCharge,        m_self->GetAttribute(AttrShieldCapacity), false);
    m_self->SetAttribute(AttrCapacitorCharge,     m_self->GetAttribute(AttrCapacitorCapacity), false);

    m_destiny->UpdateShipVariables();

    SetResists();

    m_shieldCharge = m_self->GetAttribute(AttrShieldCharge).get_float();
    m_shieldCapacity = m_self->GetAttribute(AttrShieldCapacity).get_float();
    m_armorDamage = 0.0f;
    m_hullDamage = 0.0f;

    m_processTimer.Start(m_processTimerTick);

    _log(NPC__TRACE, "Created AIShipSE for char %u (%s) - ship type %u (%s), corpID=%u",
         m_charID, m_charName.c_str(), m_self->typeID(), m_self->name(), m_corpID);
}

AIShipSE::~AIShipSE() {
    // Ensure EntityList's m_aiShips map doesn't retain a dangling pointer
    sEntityList.RemoveAIShip(m_charID);
}

// VEV_JUMP_GRACE: arm the post-jump grace suite — mirror of Client.cpp:912-920.
void AIShipSE::StartJumpGrace() {
    if (m_destiny != nullptr) m_destiny->Cloak();              // gate cloak (also breaks on warp)
    m_jumpCloakTimer.Start(Player::Timer::JumpCloak);          // 30s expiry
    SetInvul(true);                                            // jump invuln (attacker AI skips IsInvul)
    m_jumpInvulTimer.Start(Player::Timer::JumpInvul);          // 15s expiry
}

void AIShipSE::Process() {
    if (m_killed)
        return;

    /* Enable base call to Process Targeting and Movement */
    SystemEntity::Process();

    // VEV_JUMP_GRACE: expire the post-jump cloak + invuln (AI ships have no Client::ProcessClient).
    if (m_jumpCloakTimer.Enabled() && m_jumpCloakTimer.Check(false)) {
        m_jumpCloakTimer.Disable();
        if (m_destiny != nullptr && m_destiny->IsCloaked()) m_destiny->UnCloak();
    }
    if (m_jumpInvulTimer.Enabled() && m_jumpInvulTimer.Check(false)) {
        m_jumpInvulTimer.Disable();
        SetInvul(false);
    }

    // Shield recharge on 5s tick (simplified — no pilot required)
    if (m_processTimer.Check()) {
        float charge = m_self->GetAttribute(AttrShieldCharge).get_float();
        float capacity = m_self->GetAttribute(AttrShieldCapacity).get_float();
        if (charge < capacity) {
            float rechargeAmt = capacity * 0.01f;  // 1% per tick
            charge += rechargeAmt;
            if (charge > capacity)
                charge = capacity;
            m_self->SetAttribute(AttrShieldCharge, charge);
            m_shieldCharge = charge;
        }

        // VEV_SIM_CAPRECHARGE: cap recharge leg for the phantom path, mirroring
        // ShipSE::Process (Ship.cpp:2472-2483). AIShipSE is a sibling of ShipSE, so the
        // base ShipSE::CalculateRechargeRate is unreachable here -> its pure-math curve
        // (Ship.cpp:2418-2439, byte-equal arithmetic) is inlined below; NOT a new model.
        {
            float capCharge   = m_self->GetAttribute(AttrCapacitorCharge).get_float();
            float capCapacity = m_self->GetAttribute(AttrCapacitorCapacity).get_float();
            if (capCharge < capCapacity) {
                // ---- inlined ShipSE::CalculateRechargeRate(Capacity, Current, RechargeTimeMS) ----
                float RechargeTimeMS = m_self->GetAttribute(AttrRechargeRate).get_float();
                RechargeTimeMS = (RechargeTimeMS < 1 ? 1 : RechargeTimeMS);
                float Current = (capCharge < 1 ? 1 : capCharge);
                float Cmax = (capCapacity < 1 ? 1 : capCapacity);
                float tau = (RechargeTimeMS / 5000.0);
                float Cmax2_tau = ((Cmax * 2) / tau);
                float C_Cmax = (Current / Cmax);
                float sC_Cmax = sqrt(C_Cmax);
                float rechargeRate = (Cmax2_tau * (sC_Cmax - C_Cmax));  // Gj/sec
                // ---- end inlined curve ----
                float newCapCharge = capCharge + ((m_processTimerTick / 1000) * rechargeRate);
                if (newCapCharge > capCapacity) {
                    newCapCharge = capCapacity;
                } else if ((capCapacity - newCapCharge) < 0.3) {
                    newCapCharge = capCapacity;
                }
                m_self->SetAttribute(AttrCapacitorCharge, newCapCharge);
            }
        }
    }

    // dock_at procedure: once a pending dock target is within docking range,
    // dock. The dock command despawns THIS entity, so return immediately after.
    if (m_pendingDockStationID != 0 && m_system != nullptr) {
        SystemEntity* pStation = m_system->GetSE(m_pendingDockStationID);
        if (pStation != nullptr) {
            // VEV_DOCKGATE_RADIUS (2026-06-13): mirror dock_at EXACTLY so an AI
            // pilot docks at ANY station, not just the one it spawned at.
            // DistanceTo2 returns PLAIN distance (SystemEntity.cpp: GetPosition().
            // distance) — the sqrt() here re-broke it (A371 Bug22.3), and the flat
            // 9500m gate ignored station radius: on a big station (Myyhera III hull
            // 48km, Deepari 29km) 9500m-from-CENTER is ~40km INSIDE the hull,
            // unreachable, so the pilot crawled forever and never docked. Raw
            // distance + (2500 + radius) + warp-when-far = first-class docking.
            double dist = DistanceTo2(pStation);          // plain meters, NOT sqrt(d)
            const double DOCK_GATE = 2500.0 + pStation->GetRadius();
            if (dist <= DOCK_GATE) {
                uint32 stID = m_pendingDockStationID;
                m_pendingDockStationID = 0;   // cleared only when the dock will pass the gate
                char dp[64];
                snprintf(dp, sizeof(dp), "{\"stationID\": %u}", stID);
                std::string rmsg;
                sEntityList.ExecuteAICommand(m_charID, "dock", dp, rmsg);
                return;  // dock despawned us — do not touch `this`
            } else if (dist <= 150000.0) {
                // within grid but outside the dock gate: close the gap sublight,
                // pending stays set so we re-check + dock once inside.
                // VEV_DOCK_GOTO_ALIGN: guard like the warp branch -- re-issuing
                // GotoPoint EVERY tick resets the GOTO accel ramp (m_stateStamp) so
                // activeSpeedFraction never climbs -> the ship crawls at ~96 m/s and
                // never reaches the dock gate (XPL-Pathfinder, 2026-06-15). Issue it
                // once; the station is stationary so the target never needs updating.
                if (DestinyMgr()->GetState() != Destiny::Ball::Mode::GOTO)
                    DestinyMgr()->GotoPoint(pStation->GetPosition());
            } else if (DestinyMgr()->GetState() != Destiny::Ball::Mode::WARP) {
                // VEV_DOCK_WARP_ALIGN: guard on the WARP ball-mode, NOT IsWarping().
                // IsWarping() only flips true at InitWarp (active warp); during the
                // multi-second align-to-warp phase m_warpState is still null, so the
                // old !IsWarping() guard re-issued WarpTo EVERY tick -> reset the align
                // (m_stateStamp) -> m_timeFraction never reached 0.749 -> InitWarp never
                // fired -> the ship aligned forever at ~96 m/s and never docked
                // (XPL-Pathfinder Cheetah, 2026-06-15). WarpTo sets m_ballMode=WARP up
                // front, so GetState()!=WARP correctly means "not yet aligning/warping".
                // far AND not already warping (warp finished short, or drifted):
                // warp to just OUTSIDE the hull. GUARD on IsWarping() — re-issuing
                // WarpTo every tick RE-INITS the in-flight warp so it never moves
                // (the bug that left a hauler crawling 15 AU at ~1 m/s, 2026-06-13).
                // VEV_DOCK_WARP_TO_ZERO (2026-06-16): EVE "warp to 0" lands 0-2500m from the
                // station perimeter -- a TWO-STEP arrival (warp decel -> settle -> dock on a
                // later tick), never an instant dock-from-warp, and only RARELY a minor
                // sublight gap to close. Land mostly inside the radius+2500 dock gate; ~1 in 6
                // just outside so the GOTO approach above closes a small gap (curator-corrected
                // EVE behavior, replacing the old flat 15km approach).
                double vevLandGap = (double)MakeRandomInt(0, 2500);
                if (MakeRandomInt(0, 5) == 0) vevLandGap = (double)MakeRandomInt(2500, 3500);
                DestinyMgr()->WarpTo(pStation->GetPosition(), pStation->GetRadius() + vevLandGap);
            }
            // else far but warping: let the in-flight warp complete — do nothing.
        }
    }
}

void AIShipSE::SetResists() {
    /* Same pattern as NPC::SetResists() and DroneSE::SetResists() */
    if (!m_self->HasAttribute(AttrShieldEmDamageResonance)) m_self->SetAttribute(AttrShieldEmDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrShieldExplosiveDamageResonance)) m_self->SetAttribute(AttrShieldExplosiveDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrShieldKineticDamageResonance)) m_self->SetAttribute(AttrShieldKineticDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrShieldThermalDamageResonance)) m_self->SetAttribute(AttrShieldThermalDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrArmorEmDamageResonance)) m_self->SetAttribute(AttrArmorEmDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrArmorExplosiveDamageResonance)) m_self->SetAttribute(AttrArmorExplosiveDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrArmorKineticDamageResonance)) m_self->SetAttribute(AttrArmorKineticDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrArmorThermalDamageResonance)) m_self->SetAttribute(AttrArmorThermalDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrEmDamageResonance)) m_self->SetAttribute(AttrEmDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrExplosiveDamageResonance)) m_self->SetAttribute(AttrExplosiveDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrKineticDamageResonance)) m_self->SetAttribute(AttrKineticDamageResonance, EvilOne, false);
    if (!m_self->HasAttribute(AttrThermalDamageResonance)) m_self->SetAttribute(AttrThermalDamageResonance, EvilOne, false);
}

void AIShipSE::TargetLost(SystemEntity* who) {
    // placeholder — AI targeting not yet implemented
}

void AIShipSE::TargetedAdd(SystemEntity* who) {
    // placeholder — AI threat response not yet implemented
}

// See A321 §4.2 — MakeSlimItem is what other players' clients see.
// Emulates ShipSE::MakeSlimItem() but reads from stored member vars instead of Client*.
PyDict* AIShipSE::MakeSlimItem() {
    _log(SE__SLIMITEM, "MakeSlimItem for AIShip %s (char=%u %s)", m_self->name(), m_charID, m_charName.c_str());

    PyDict *slim = new PyDict();
        slim->SetItemString("itemID",           new PyLong(m_self->itemID()));
        slim->SetItemString("typeID",           new PyInt(m_self->typeID()));
        slim->SetItemString("name",             new PyString(m_charName));
        slim->SetItemString("ownerID",          new PyInt(m_charID));
        slim->SetItemString("charID",           new PyInt(m_charID));
        slim->SetItemString("corpID",           IsCorp(m_corpID) ? new PyInt(m_corpID) : PyStatic.NewNone());
        slim->SetItemString("allianceID",       IsAlliance(m_allyID) ? new PyInt(m_allyID) : PyStatic.NewNone());
        slim->SetItemString("warFactionID",     IsFaction(m_warID) ? new PyInt(m_warID) : PyStatic.NewNone());
        slim->SetItemString("bounty",           new PyFloat(m_bounty));
        slim->SetItemString("securityStatus",   new PyFloat(m_securityStatus));
        slim->SetItemString("categoryID",       new PyInt(m_self->categoryID()));
        slim->SetItemString("groupID",          new PyInt(m_self->groupID()));

    // Encode hi-slot modules so the client renders turrets (same as ShipSE::MakeSlimItem)
    uint32 shipID = m_self->itemID();
    DBQueryResult modRes;
    if (sDatabase.RunQuery(modRes,
        "SELECT itemID, typeID FROM entity"
        " WHERE locationID = %u AND flag BETWEEN 27 AND 34"
        " ORDER BY flag",
        shipID))
    {
        DBResultRow row;
        PyList *list = new PyList();
        while (modRes.GetRow(row)) {
            sLog.Cyan("AIShipSE::MakeSlimItem", "  module: itemID=%u typeID=%u on ship %u",
                      row.GetUInt(0), row.GetUInt(1), shipID);
            list->AddItem(new_tuple(row.GetUInt(0), row.GetUInt(1)));
        }
        sLog.Cyan("AIShipSE::MakeSlimItem", "Ship %u (%s) has %lu hi-slot modules for turret rendering",
                  shipID, m_charName.c_str(), list->size());
        if (list->size() > 0)
            slim->SetItemString("modules", list);
        else
            PyDecRef(list);
    } else {
        sLog.Error("AIShipSE::MakeSlimItem", "DB query FAILED for modules on ship %u", shipID);
    }

    return slim;
}

void AIShipSE::EncodeDestiny(Buffer& into) {
    using namespace Destiny;

    uint8 mode = m_destiny->GetState();

    BallHeader head = BallHeader();
        head.entityID = GetID();
        head.mode = mode;
        head.radius = GetRadius();
        GPoint _dpos = m_destiny->GetPosition();   // VEV: x()=m_self->position() is STALE for phantoms (destiny never syncs it back to the item) -> client renders the ship at its SPAWN point -> '1.$ AU' / wrong distance. Send the LIVE destiny position.
        head.posX = _dpos.x;
        head.posY = _dpos.y;
        head.posZ = _dpos.z;
        // IsInteractive: client treats this as a piloted ship (right-click, show info, etc.)
        head.flags = Ball::Flag::IsInteractive | Ball::Flag::IsFree;
    into.Append(head);
    MassSector mass = MassSector();
        mass.mass = m_destiny->GetMass();
        mass.cloak = (m_destiny->IsCloaked() ? 1 : 0);
        mass.harmonic = m_harmonic;
        mass.corporationID = m_corpID;
        mass.allianceID = (IsAlliance(m_allyID) ? m_allyID : -1);
    into.Append(mass);
    DataSector data = DataSector();
        data.maxSpeed = m_destiny->GetMaxVelocity();
        data.velX = m_destiny->GetVelocity().x;
        data.velY = m_destiny->GetVelocity().y;
        data.velZ = m_destiny->GetVelocity().z;
        data.inertia = m_destiny->GetInertia();
        data.speedfraction = m_destiny->GetSpeedFraction();
    into.Append(data);
    switch (mode) {
        case Ball::Mode::WARP: {
            GPoint target = m_destiny->GetTargetPoint();
            WARP_Struct warp;
                warp.formationID = 0xFF;
                warp.targX = target.x;
                warp.targY = target.y;
                warp.targZ = target.z;
                warp.speed = m_destiny->GetWarpSpeed();
                warp.effectStamp = m_destiny->GetStateStamp(); // VEV_DESYNC_AISHIP_WARP: real warp-start stamp on AI-ship path (agents fly these)
                warp.followRange = 0;
                warp.followID = 0;
            into.Append(warp);
        } break;
        case Ball::Mode::FOLLOW: {
            FOLLOW_Struct follow;
                follow.followID = m_destiny->GetTargetID();
                follow.followRange = m_destiny->GetFollowDistance();
                follow.formationID = 0xFF;
            into.Append(follow);
        } break;
        case Ball::Mode::ORBIT: {
            ORBIT_Struct orbit;
                orbit.targetID = m_destiny->GetTargetID();
                orbit.followRange = m_destiny->GetFollowDistance();
                orbit.formationID = 0xFF;
            into.Append(orbit);
        } break;
        case Ball::Mode::GOTO: {
            GPoint target = m_destiny->GetTargetPoint();
            GOTO_Struct go;
                go.formationID = 0xFF;
                go.x = target.x;
                go.y = target.y;
                go.z = target.z;
            into.Append(go);
        } break;
        default: {
            STOP_Struct main;
                main.formationID = 0xFF;
            into.Append(main);
        } break;
    }

    _log(SE__DESTINY, "AIShipSE::EncodeDestiny(): %s (char=%u) - id:%lli, mode:%u, flags:0x%X",
         m_charName.c_str(), m_charID, head.entityID, head.mode, head.flags);
}

void AIShipSE::MakeDamageState(DoDestinyDamageState& into) {
    // See A371 §11 (Bug 20) — protect against div-by-zero on HP attributes.
    float shieldCap = m_self->GetAttribute(AttrShieldCapacity).get_float();
    float armorHP = m_self->GetAttribute(AttrArmorHP).get_float();
    float hullHP = m_self->GetAttribute(AttrHP).get_float();
    into.shield = (shieldCap > 0.0f) ? (m_self->GetAttribute(AttrShieldCharge).get_float() / shieldCap) : 1.0;
    into.recharge = std::max(10000.0f, m_self->GetAttribute(AttrShieldRechargeRate).get_float());
    into.timestamp = GetFileTimeNow();
    into.armor = (armorHP > 0.0f) ? (1.0 - (m_self->GetAttribute(AttrArmorDamage).get_float() / armorHP)) : 1.0;
    into.structure = (hullHP > 0.0f) ? (1.0 - (m_self->GetAttribute(AttrDamage).get_float() / hullHP)) : 1.0;
}

// VEV_DRONE: AI-pilot drone bridge. Phantom AIShipSE has no Client*, so the launch
// path (normally ShipSE::LaunchDrone via the EVE client) is reimplemented here,
// Client*-free, and driven by the ai_command_queue drone verbs in EntityList.
bool AIShipSE::LaunchDrone(InventoryItemRef dRef, const FactionData& data) {
    dRef->Move(GetLocationID(), flagNone, true);
    dRef->ChangeSingleton(true);
    GPoint position(GetPosition());
    position.MakeRandomPointOnSphere(500.0);
    dRef->SetPosition(position);
    DroneSE* pDrone = new DroneSE(dRef, GetServices(), SystemMgr(), data);
    pDrone->Launch(this);   // 'this' is a SystemEntity* controller (VEV_DRONE decouple)
    m_drones.emplace(dRef->itemID(), dRef.get());
    EvilNumber load = GetSelf()->GetAttribute(AttrDroneBandwidthLoad);
    load += dRef->GetAttribute(AttrDroneBandwidthUsed);
    if (load <= GetSelf()->GetAttribute(AttrDroneBandwidth)) {
        pDrone->Online();
        pDrone->IdleOrbit();  // VEV_DRONE: SetIdle() early-returns on an already-Idle fresh drone; orbit the carrier explicitly
        GetSelf()->SetAttribute(AttrDroneBandwidthLoad, load, false);
        return true;
    }
    return false;  // launched but inert (no bandwidth)
}

uint8 AIShipSE::LaunchAllDrones(const FactionData& data) {
    Inventory* inv = GetSelf()->GetMyInventory();
    if (inv == nullptr)
        return 0;
    if (!inv->ContentsLoaded())
        inv->LoadContents();
    std::vector<InventoryItemRef> bay;
    inv->GetItemsByFlag(flagDroneBay, bay);
    // VEV_DRONE_RELOAD (2026-06-17): the in-memory bay can read EMPTY while the DB
    // bay holds drones (a scoop recovery or fresh fit while the Inventory was
    // cached) -- ContentsLoaded() short-circuits LoadContents(), so the drones stay
    // invisible and launch fails "empty bay" (the recurring combat-pilot blocker).
    // Pull them straight from the DB via ItemFactory so a restocked bay launches.
    if (bay.empty()) {
        DBQueryResult dres;
        if (sDatabase.RunQuery(dres,
            "SELECT itemID FROM entity WHERE locationID = %u AND flag = %u",
            GetSelf()->itemID(), (uint32)flagDroneBay)) {
            DBResultRow drow;
            while (dres.GetRow(drow)) {
                InventoryItemRef dRef = sItemFactory.GetItemRef(drow.GetUInt(0));
                if (dRef.get() != nullptr) bay.push_back(dRef);
            }
        }
    }
    uint8 launched = 0;
    for (InventoryItemRef dRef : bay) {
        if (dRef.get() == nullptr)
            continue;
        if (dRef->categoryID() != EVEDB::invCategories::Drone)
            continue;
        if (m_drones.find(dRef->itemID()) != m_drones.end())
            continue;  // already in space
        if (LaunchDrone(dRef, data))
            ++launched;
    }
    return launched;
}

void AIShipSE::EngageDrones(SystemEntity* pTarget) {
    if (pTarget == nullptr)
        return;
    for (auto& cur : m_drones) {
        SystemEntity* pSE = SystemMgr()->GetSE(cur.first);
        if ((pSE != nullptr) and pSE->IsDroneSE()) {
            DroneSE* pDrone = pSE->GetDroneSE();
            if (pDrone->IsEnabled())
                pDrone->GetAI()->Target(pTarget);  // StartTargeting -> CheckDistance -> Attack
        }
    }
}

void AIShipSE::ReturnAllDrones() {
    std::vector<uint32> ids;
    for (auto& cur : m_drones)
        ids.push_back(cur.first);
    for (uint32 id : ids) {
        SystemEntity* pSE = SystemMgr()->GetSE(id);
        if ((pSE == nullptr) or !pSE->IsDroneSE())
            continue;
        InventoryItemRef iRef = pSE->GetSelf();
        if (iRef.get() != nullptr) {
            iRef->ChangeOwner(m_charID, true);
            iRef->Move(GetSelf()->itemID(), flagDroneBay, true);
            EvilNumber load = GetSelf()->GetAttribute(AttrDroneBandwidthLoad);
            load -= iRef->GetAttribute(AttrDroneBandwidthUsed);
            GetSelf()->SetAttribute(AttrDroneBandwidthLoad, load, false);
        }
        m_drones.erase(id);
        SystemMgr()->RemoveEntity(pSE);
        SafeDelete(pSE);
    }
}

// VEV_DRONE: remove launched drone SEs so a dying/despawning ship can't leave drones
// pointing m_assignedShip at freed memory.
void AIShipSE::CleanupDrones() {
    std::vector<uint32> ids;
    for (auto& cur : m_drones)
        ids.push_back(cur.first);
    for (uint32 id : ids) {
        SystemEntity* pSE = (m_system != nullptr) ? m_system->GetSE(id) : nullptr;
        if (pSE != nullptr) {
            if (m_system != nullptr)
                m_system->RemoveEntity(pSE);
            SafeDelete(pSE);
        }
    }
    m_drones.clear();
}

// VEV_JUMP_CLEANUP: the ONE pre-delete teardown seam for the cross-system jump. The jump
// SafeDeletes this SE and builds a new one; anything holding in-memory relationships to OTHER
// entities (drones in space, EWAR applied to victims) MUST be undone here, while this SE is
// still valid and still in its OLD system. Mirrors the Despawn()/Killed() cleanup the jump bypassed.
void AIShipSE::OnPreJumpDestroy() {
    ReturnAllDrones();    // scoop drones to bay (+decrement bandwidth) -- else orphaned DroneSEs in
                          //   the old system dangle their owner ptr -> use-after-free crash.
    ClearAppliedEwar();   // reverse webs/scrams so victims aren't left perma-slowed/scrambled.
}

// VEV_JUMP_CLEANUP: reverse every EWAR effect this ship applied to others (resolved in the
// OLD system, so must run before RemoveEntity). Null-guarded; a dead/departed target is skipped.
void AIShipSE::ClearAppliedEwar() {
    for (auto& cur : m_appliedWebs) {
        SystemEntity* pSE = (m_system != nullptr) ? m_system->GetSE(cur.first) : nullptr;
        if (pSE != nullptr && pSE->DestinyMgr() != nullptr && cur.second.get() != nullptr)
            pSE->DestinyMgr()->WebbedMe(cur.second, false);   // undo the m_maxShipSpeed multiply
    }
    for (auto& cur : m_appliedScrams) {
        SystemEntity* pSE = (m_system != nullptr) ? m_system->GetSE(cur.first) : nullptr;
        if (pSE != nullptr && pSE->GetSelf().get() != nullptr)
            pSE->GetSelf()->SetAttribute(AttrWarpScrambleStatus, 0);
    }
    m_appliedWebs.clear();
    m_appliedScrams.clear();
}

void AIShipSE::RecordAppliedWeb(uint32 targetID, InventoryItemRef modRef) { m_appliedWebs[targetID] = modRef; }
void AIShipSE::RecordAppliedScram(uint32 targetID) { m_appliedScrams[targetID] = true; }

// VEV_JUMP_CLEANUP: snapshot/restore per-SE state that must SURVIVE a jump (the recreate
// would otherwise reset it). Add a field to JumpCarry + a line in each when it must persist.
AIShipSE::JumpCarry AIShipSE::ExportJumpState() const {
    JumpCarry c;
    c.pendingDockStationID = m_pendingDockStationID;
    return c;
}
void AIShipSE::ImportJumpState(const JumpCarry& carry) {
    m_pendingDockStationID = carry.pendingDockStationID;
}

void AIShipSE::Despawn() {
    sLog.Cyan("AIShipSE::Despawn", "Removing AI ship for char %u (%s)", m_charID, m_charName.c_str());

    CleanupDrones();  // VEV_DRONE

    // Remove from EntityList AI ship tracking
    sEntityList.RemoveAIShip(m_charID);

    // Remove from system (handles bubble, anomaly, entity maps)
    if (m_system != nullptr)
        m_system->RemoveEntity(this);

    // Delete the spawned ship item from DB
    m_self->Delete();
}

void AIShipSE::Killed(Damage& damage) {
    if ((m_bubble == nullptr) or (m_destiny == nullptr) or (m_system == nullptr))
        return;

    sLog.Cyan("AIShipSE::Killed", "AI ship for char %u (%s) was destroyed", m_charID, m_charName.c_str());

    CleanupDrones();  // VEV_DRONE

    // Remove from EntityList tracking
    sEntityList.RemoveAIShip(m_charID);

    // VEV_HARVEST_WRECKLOOT: ported from NPC::Killed (NPC.cpp:299) — killer resolution,
    // bounty + security status, wreck container + loot drop. Helpers inherited via DynamicSystemEntity.
    {
        uint32 killerID = 0;
        Client* pClient = nullptr;
        SystemEntity* killer = damage.srcSE;
        uint32 allyID = 0;
        if (killer != nullptr) {
            allyID = killer->GetAllianceID();
            if (killer->HasPilot()) {
                pClient = killer->GetPilot();
                if (pClient != nullptr) killerID = pClient->GetCharacterID();
            } else if (killer->IsDroneSE()) {
                pClient = sEntityList.FindClientByCharID(killer->GetSelf()->ownerID());
                if (pClient != nullptr) killerID = pClient->GetCharacterID();
            } else {
                killerID = killer->GetID();
            }
        }

        uint32 locationID = GetLocationID();
        MapDB::AddKill(locationID);
        MapDB::AddFactionKill(locationID);

        if (pClient != nullptr) {
            AwardBounty(pClient);
            if (m_system->GetSystemSecurityRating() > 0)
                AwardSecurityStatus(m_self, pClient->GetChar().get());
        }

        GPoint wreckPosition = m_destiny->GetPosition();
        if (!wreckPosition.isNaN()) {
            uint32 wreckTypeID = sDataMgr.GetWreckID(m_self->typeID());
            if (!IsWreckTypeID(wreckTypeID))
                wreckTypeID = 26557; // generic frigate wreck fallback (per NPC::Killed)

            std::string wreck_name = m_self->itemName();
            wreck_name += " Wreck";
            // VEV_SALVAGE_FACTION: key the wreck's salvage table by the victim
            // hull's race faction (customInfo feeds StaticDataMgr::GetSalvage;
            // it was 0 -> AI-pilot wrecks salvaged to NOTHING).
            uint32 svFaction = 0;
            switch (m_self->type().race()) {
                case 1: svFaction = 500001; break;  // Caldari State
                case 2: svFaction = 500002; break;  // Minmatar Republic
                case 4: svFaction = 500003; break;  // Amarr Empire
                case 8: svFaction = 500004; break;  // Gallente Federation
                default: svFaction = 500001; break; // unraced hull -> Caldari T1 table
            }
            ItemData wreckItemData(wreckTypeID, m_charID, locationID, flagNone, wreck_name.c_str(), wreckPosition, itoa(svFaction)); // VEV_V6 + VEV_SALVAGE_FACTION
            WreckContainerRef wreckItemRef = sItemFactory.SpawnWreckContainer(wreckItemData);
            if (wreckItemRef.get() != nullptr) {
                if (MakeRandomFloat() < sConfig.npc.LootDropChance)
                    DropLoot(wreckItemRef, m_self->groupID(), killerID);

                // VEV_HARVEST_CARGODROP: drop ~50% of the victim's modules + rigs + cargo into the wreck (real EVE).
                if (m_self->GetMyInventory() != nullptr) {
                    m_self->GetMyInventory()->LoadContents();  // VEV: cargo hold is lazy-loaded; force in-memory before enumerating
                    std::vector<InventoryItemRef> drop;
                    m_self->GetMyInventory()->GetItemsByFlagRange(flagLowSlot0, flagHiSlot7, drop);
                    m_self->GetMyInventory()->GetItemsByFlagRange(flagRigSlot0, flagRigSlot7, drop);
                    m_self->GetMyInventory()->GetItemsByFlagRange(flagCargoHold, flagCargoHold, drop);
                    for (auto& dItem : drop) {
                        if (dItem.get() == nullptr) continue;
                        if (MakeRandomFloat() < 0.5f) {
                            // VEV v3: spawn a COPY into the wreck (mirrors DropLoot) so the wreck in-memory
                            // inventory holds it and the CLIENT sees it on open; Move() only updated the DB row.
                            ItemData vevLoot(dItem->typeID(), m_charID, wreckItemRef->itemID(), flagNone, dItem->quantity());
                            wreckItemRef->AddItem(sItemFactory.SpawnItem(vevLoot));
                        }
                    }
                }

                DBSystemDynamicEntity wreckEntity = DBSystemDynamicEntity();
                wreckEntity.allianceID = 0; // VEV_V6: zero (AI killer has no valid alliance -> AddBall deref)
                wreckEntity.categoryID = EVEDB::invCategories::Celestial;
                wreckEntity.corporationID = 0; // VEV_V6
                wreckEntity.factionID = 0; // VEV_V6
                wreckEntity.groupID = EVEDB::invGroups::Wreck;
                wreckEntity.itemID = wreckItemRef->itemID();
                wreckEntity.itemName = wreck_name;
                wreckEntity.ownerID = m_charID; // VEV_V5 wreck owner = victim char (killerID=ship-id for AI kills poisons the grid)
                wreckEntity.typeID = wreckTypeID;
                wreckEntity.position = wreckPosition;
                if (!m_system->BuildDynamicEntity(wreckEntity, m_self->itemID())) {
                    sLog.Error("AIShipSE::Killed()", "Spawning wreck failed for typeID %u", wreckTypeID);
                    wreckItemRef->Delete();
                }
            } else {
                sLog.Error("AIShipSE::Killed()", "Creating wreck item failed for type %u", wreckTypeID);
            }
        } else {
            sLog.Error("AIShipSE::Killed()", "Wreck position is NaN; no wreck spawned");
        }
    }
    // VEV_GUARD_AISHIPTAIL_PHANTOM: gate the Client*-touching death-tail
    // (insurance payout + jettison packet) on a live owner Client*. Phantom
    // AIShipSE instances (no Client* for m_ownerID) skip both operations;
    // wreck spawn + aggro resolution above are unaffected. Predicate per
    // prior crash agent's recommendation.
    if (sEntityList.FindClientByCharID(m_ownerID) != nullptr) {
        // VEV_SIM_INSURANCE: phantom death-tail insurance payout, ported from
        // ShipSE::PayInsurance (Ship.cpp:2503-2521). Skip rookie ships; pay corpSCC -> owner the
        // GetShipInsurancePayout amount (flat 15000 ISK fallback when no shipInsurance row -- the
        // payout read is Client*-free, SIMLAYER verify-on-implement #1), then delete the row.
        if (m_self->groupID() != EVEDB::invGroups::Rookieship) {
            ShipDB insDb;  // ShipDB is default-constructible (: ServiceDB); GetShipInsurancePayout is pure SQL
            std::string insReason = "Insurance payment for loss of the ship ";
            insReason += m_self->itemName();
            AccountService::TransferFunds(
                corpSCC,
                m_ownerID,
                insDb.GetShipInsurancePayout(m_self->itemID()),
                insReason,
                Journal::EntryType::Insurance,
                m_self->typeID()
            );
            ShipDB::DeleteInsuranceByShipID(m_self->itemID());
        }
        m_destiny->SendJettisonPacket();
    }
}
