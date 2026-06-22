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
    // VEV_POSITION_DESYNC: phantoms read the LIVE destiny position, not the stale item store.
    // m_self->position() is never synced back from destiny for an AIShipSE, so after a multi-jump
    // relocate get_ship_status + the jump proximity gate saw a ~70 AU stale (old-system) position
    // and the FSM stall-detector aborted (only a server restart cleared it). Same class the
    // EncodeDestiny fix (AIShipSE.cpp) already cured for rendering. Null-guarded like GetVelocity.
    const GPoint& GetPosition() const override { return (m_destiny != nullptr ? m_destiny->GetPosition() : SystemEntity::GetPosition()); }
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
    /* dock_at: station to auto-dock at once within range (AIShipSE::Process). */
    void SetPendingDock(uint32 stationID)               { m_pendingDockStationID = stationID; }
    // VEV_JUMP_GRACE: arm the post-jump grace suite (gate cloak + jump invuln) on this
    // recreated AI ship — mirror of the human path (Client.cpp:912-920). Expired in Process().
    void StartJumpGrace();
    double GetCloakRemainingMs()                        { return m_jumpCloakTimer.Enabled() ? (double)m_jumpCloakTimer.GetRemainingTime() : 0.0; }
    double GetInvulRemainingMs()                        { return m_jumpInvulTimer.Enabled() ? (double)m_jumpInvulTimer.GetRemainingTime() : 0.0; }

    // VEV_JUMP_CLEANUP: the cross-system jump (EntityList "jump") DESTROYS + RECREATES this SE,
    // so per-SE state must be handled explicitly. Every per-SE member belongs to ONE bucket:
    //   (1) CLEAN  -> torn down in OnPreJumpDestroy() before the SafeDelete (drones, applied EWAR).
    //   (2) CARRY  -> survives via Export/ImportJumpState (add a field to JumpCarry).
    //   (3) DERIVE -> rebuilt by the ctor from the surviving InventoryItem (do nothing).
    // A new per-SE member in none of these silently vanishes on jump -- pick a bucket.
    struct JumpCarry { uint32 pendingDockStationID = 0; };
    JumpCarry ExportJumpState() const;
    void      ImportJumpState(const JumpCarry& carry);
    void      OnPreJumpDestroy();                                          // (1) drones + applied EWAR
    void      RecordAppliedWeb(uint32 targetID, InventoryItemRef modRef);  // EWAR ledger (jump-reversal)
    void      RecordAppliedScram(uint32 targetID);
    void      ClearAppliedEwar();

    /* VEV_DRONE: AI-pilot drone bridge. Phantom AIShipSE has no Client*, so the
       launch/engage/return path lives here (not on ShipSE), driven by ai_command_queue. */
    uint8 LaunchAllDrones(const FactionData& data);  // launch all bay drones; returns count
    void  EngageDrones(SystemEntity* pTarget);       // all launched drones target pTarget
    void  ReturnAllDrones();                         // scoop all launched drones to bay
    bool  LaunchDrone(InventoryItemRef dRef, const FactionData& data);  // single-drone launch

private:
    uint32 m_charID;
    std::string m_charName;
    float m_securityStatus;
    float m_bounty;
    uint32 m_pendingDockStationID = 0;  // dock_at target; Process docks in range

    Timer m_processTimer;
    int32 m_processTimerTick;
    Timer m_jumpCloakTimer;   // VEV_JUMP_GRACE: gate-cloak 30s expiry (no Client::ProcessClient for AI)
    Timer m_jumpInvulTimer;   // VEV_JUMP_GRACE: jump-invuln 15s expiry

    float m_shieldCharge;
    float m_shieldCapacity;
    float m_armorDamage;
    float m_hullDamage;

    std::map<uint32, InventoryItem*> m_drones;  // VEV_DRONE: launched drones (itemID -> item*)
    // VEV_JUMP_CLEANUP: EWAR this ship applied, reversed on jump so victims aren't left
    // perma-webbed/scrambled. web: targetID -> module ref (WebbedMe undo); scram: targetID set.
    std::map<uint32, InventoryItemRef> m_appliedWebs;
    std::map<uint32, bool> m_appliedScrams;
    void CleanupDrones();  // VEV_DRONE: remove all launched drone SEs on despawn/death
};

#endif /* !__AISHIP_SE__H__INCL__ */
