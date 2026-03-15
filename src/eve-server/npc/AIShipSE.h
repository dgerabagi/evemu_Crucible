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
// AIShipSE is a server-controlled ship that represents an AI character in space.
// Appears to other players as a real piloted ship (with charID, name, corp, etc.).
// Modeled after DroneSE/NPC pattern: DynamicSystemEntity + embedded data, no Client* required.

#ifndef __AISHIP_SE__H__INCL__
#define __AISHIP_SE__H__INCL__

#include "system/SystemEntity.h"

class EVEServiceManager;
class SystemManager;

class AIShipSE
: public DynamicSystemEntity
{
public:
    AIShipSE(InventoryItemRef ship, EVEServiceManager& services, SystemManager* system,
             const FactionData& data, uint32 charID, const char* charName,
             float securityStatus, float bounty);
    virtual ~AIShipSE();

    /* class type pointer querys. */
    virtual AIShipSE*           GetAIShipSE()           { return this; }
    /* class type tests. */
    virtual bool                IsAIShipSE()            { return true; }

    /* SystemEntity interface */
    virtual void Process();
    virtual void EncodeDestiny(Buffer& into);
    virtual void MakeDamageState(DoDestinyDamageState& into);
    virtual PyDict* MakeSlimItem();

    /* virtual functions */
    virtual void Killed(Damage& damage);
    virtual void TargetLost(SystemEntity* who);
    virtual void TargetedAdd(SystemEntity* who);

    /* AI ship specific */
    uint32 GetCharID() const                            { return m_charID; }
    const char* GetCharName() const                     { return m_charName.c_str(); }
    float GetSecurityStatus() const                     { return m_securityStatus; }
    float GetBountyVal() const                          { return m_bounty; }

    void SetResists();
    void Despawn();  // remove from system and clean up item

private:
    uint32 m_charID;
    std::string m_charName;
    float m_securityStatus;
    float m_bounty;

    Timer m_processTimer;
    int32 m_processTimerTick;

    float m_shieldCharge;
    float m_shieldCapacity;
    float m_armorDamage;
    float m_hullDamage;
};

#endif /* !__AISHIP_SE__H__INCL__ */
