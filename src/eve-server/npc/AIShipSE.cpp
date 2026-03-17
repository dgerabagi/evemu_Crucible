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
#include "system/Damage.h"
#include "system/SystemManager.h"
#include "system/SystemBubble.h"

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

void AIShipSE::Process() {
    if (m_killed)
        return;

    /* Enable base call to Process Targeting and Movement */
    SystemEntity::Process();

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
        head.posX = x();
        head.posY = y();
        head.posZ = z();
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
                warp.effectStamp = -1;
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

void AIShipSE::Despawn() {
    sLog.Cyan("AIShipSE::Despawn", "Removing AI ship for char %u (%s)", m_charID, m_charName.c_str());

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

    // Remove from EntityList tracking
    sEntityList.RemoveAIShip(m_charID);

    // TODO: create wreck container (follow NPC::Killed pattern) and drop loot
    m_destiny->SendJettisonPacket();
}
