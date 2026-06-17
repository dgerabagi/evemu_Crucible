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
    Author:        Zhur
    Rewrite:    Allan
*/

#include "eve-server.h"
#include "system/Container.h"  // VEV_FIX_JETTISON_INC

#include <cmath>    // VEV_WARP_IN: std::pow/log10/asin for the planet warp-in beacon
#include <cstdlib>  // VEV_WARP_IN: srandom/random (deterministic per-planet seed, matches BeyonceService)

#include "EVE_Mail.h"

#include "Client.h"
#include "ConsoleCommands.h"
#include "EntityList.h"
#include "EVEServerConfig.h"
#include "ServiceDB.h"
#include "agents/Agent.h"
#include "exploration/Probes.h"
#include "map/MapDB.h"
#include "market/MarketMgr.h"
#include "market/MarketBotMgr.h"
#include "missions/MissionDataMgr.h"
#include "station/Station.h"
#include "system/DestinyManager.h"
#include "system/SystemManager.h"
#include "pos/Tower.h"  // VEV_POS_FUEL: TowerSE::VevReonline()
#include "planet/Planet.h"     // VEV_PI_ESTABLISH
#include "planet/Colony.h"     // VEV_PI_ESTABLISH
#include "../vev-gateway/dep/nlohmann/json.hpp"  // VEV_PI_LOGI1A: spec commit

// Vev tactical-grid projector (declared in vev-gateway; eve-server has the
// gateway include dir + links it — see eve-server/CMakeLists.txt).
#include "GridStreamer.h"
#include "system/cosmicMgrs/AnomalyMgr.h"
#include "system/cosmicMgrs/CivilianMgr.h"
#include "system/cosmicMgrs/WormholeMgr.h"
#include "system/cosmicMgrs/ManagerDB.h"
#include "corporation/CorporationDB.h"
#include "market/MarketDB.h"  // See A321 §4.6 — AI command queue market operations
#include "npc/AIShipSE.h"    // See A321 §4.2 — AI ship entity (headless player ship)
#include "chat/LSCService.h" // See A321 §4.7 — Phantom session Local chat presence
#include "station/StationDataMgr.h" // See A321 §11.1 — Station dock/undock position data
#include "inventory/Inventory.h"    // See A321 §4.5 — Inventory access for mining ore extraction
#include "station/ReprocessingDB.h"   // See A331 §3.5 — Ore reprocessing primitives
#include "account/AccountService.h"   // See A331 §3.5 — Market transaction ISK transfers
#include "system/Damage.h"            // See A371 §9 — AI weapon activation (direct damage pattern)

EntityList::EntityList()
: m_services(nullptr),
m_targTimer(0, true),
m_stampTimer(0, true),
m_minuteTimer(0, true),
m_aiCmdTimer(0, true),
m_startTime(0),
m_npcs(0),
m_stamp(1000),   /* arbitrary.  start at 1k.  in seconds.  used for destiny and client counters */
m_minutes(0),
m_connections(0),
m_clientSeedID(0)
{
    m_agents.clear();
    m_probes.clear();
    m_clients.clear();
    m_players.clear();
    m_systems.clear();
    m_stations.clear();
    m_targMgrs.clear();
    m_corpMembers.clear();

    m_shipTracking = sConfig.debug.UseShipTracking;
}

EntityList::~EntityList() {
    sLog.Green("   ServerShutdown", " Complete.");
}

void EntityList::Initialize() {
    m_startTime = GetFileTimeNow();

    /* start the timers */
    m_targTimer.Start(250);     // testing targeting and scan probes at 4/sec
    m_stampTimer.Start(1000);   // 1hz tic timer
    m_minuteTimer.Start(60000); // does this need to be accurate?
    m_aiCmdTimer.Start(5000);   // See A321 §4.6 — AI command queue poll every 5s
    sLog.Cyan("   AICommandQueue", "Timer started — polling every 5s.");

    m_clientSeedID = ServiceDB::SetClientSeed();
    sLog.Green( "       ServerInit", "ClientSeed Initialized." );

    if (is_log_enabled(SERVER__STACKTRACE))
        sConfig.debug.StackTrace = true;

    sLog.Blue("       EntityList", "Entity Manager Initialized.");
}

void EntityList::Shutdown() {
    /** @todo finish this....
     * halt server called from admin client. (gm command ingame)
     * call d'tor on all connected clients
     * server run loop will exit after control is returned from this function, which will clean up remaining items.
     */
    for (auto cur : m_clients)
        SafeDelete(cur);

    m_clients.clear();
}

void EntityList::Close()
{
    if (m_clients.size() > 0) {
        sLog.Yellow("       EntityList", "Cleaning up %lu clients, %lu systems, %lu agents, and %lu stations", \
                    m_clients.size(), m_systems.size(), m_agents.size(), m_stations.size());
    } else {
        sLog.Green("       EntityList", "Cleaning up %lu clients, %lu systems, %lu agents, and %lu stations", \
                    m_clients.size(), m_systems.size(), m_agents.size(), m_stations.size());
    }

    // Clean up phantom player sessions (set offline in DB)
    for (uint32 charID : m_phantomPlayers) {
        DBerror err;
        sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET online = 0 WHERE characterID = %u", charID);
    }
    if (!m_phantomPlayers.empty())
        sLog.Green("       EntityList", "Cleaned up %lu phantom player sessions.", m_phantomPlayers.size());
    m_phantomPlayers.clear();

    for (auto cur : m_clients)
        SafeDelete(cur);

    for (auto cur : m_agents)
        SafeDelete(cur.second);

    for (auto cur : m_systems) {
        cur.second->UnloadSystem();
        SafeDelete(cur.second);
    }

    sLog.Warning("       EntityList", "Entity List has been closed." );
}

/* m_clients is used to search for online players and numerous other things.
 *  the problem here is any searching is done thru iteration, which can get expensive.
 *  however, clients are added before their char is selected, so there is no charID for map placement.
 *    maybe use m_clients for basic Process() calls and use m_players for character/client searching
 *
 * update:  done and working very well.
 */

void EntityList::Add( Client* pClient ) {
    ++m_connections;
    if (pClient != nullptr)
        m_clients.push_back(pClient);
}

void EntityList::Remove(Client* pClient) {
    /* note:  will get expensive for many clients  */
    std::vector<Client*>::iterator itr = m_clients.begin();
    for (; itr != m_clients.end(); ++itr)
        if ((*itr) == pClient) {
            m_clients.erase(itr);
            return;
        }
}

void EntityList::AddPlayer(Client* pClient)
{
    if (pClient != nullptr)
        if (pClient->IsValidSession()) {
            m_players.emplace(pClient->GetCharacterID(), pClient);
            if (IsPlayerCorp(pClient->GetCorporationID())) {
                corpRole role;
                role.emplace(pClient, pClient->GetCorpRole());
                m_corpMembers.emplace(pClient->GetCorporationID(), role);
            }
        } else {
            m_players.emplace(pClient->GetCharID(), pClient);
            // make note about invalid session and failure to add player to corp roles.
            //   this is nbd if player is in npc corp or in player corp with no roles
        }
}

// VEV_TEARDOWN_GUARD_SWEEP (2026-06-12): a dying Client* must vanish from
// EVERY station guest list. A stale m_stationData/m_locationID at session
// teardown left freed pointers in m_guestList; the next guest broadcast
// (incl. the phantom-logout notify the 2D client fires) deref'd them — the
// intermittent double-login segfault class (crashes 23:28 + 00:32, 2026-06-11/12).
void EntityList::RemoveGuestFromAllStations(Client* pClient)
{
    if (pClient == nullptr)
        return;
    for (auto& cur : m_stations)
        if (cur.second.get() != nullptr)
            cur.second->RemoveGuest(pClient);
}

void EntityList::RemovePlayer(Client* pClient)
{
    if (pClient != nullptr)
        if (pClient->IsValidSession()) {
            m_players.erase(pClient->GetCharacterID());
            // remove player from corp map, if applicable
            std::map<uint32, corpRole>::iterator itr = m_corpMembers.find(pClient->GetCorporationID());
            if (itr != m_corpMembers.end())
                itr->second.erase(pClient);
        } else {
            m_players.erase(pClient->GetCharID());
        }
}

// See A321 §4.7 — Phantom player sessions (online presence without TCP)
void EntityList::AddPhantomPlayer(uint32 charID)
{
    m_phantomPlayers.insert(charID);
    // Set DB online status so any DB-based checks also see them as online
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET online = 1 WHERE characterID = %u", charID);
    sLog.Green("    PhantomSession", "Character %u is now online (phantom).", charID);
}

void EntityList::RemovePhantomPlayer(uint32 charID)
{
    m_phantomPlayers.erase(charID);
    DBerror err;
    sDatabase.RunQuery(err,
        "UPDATE chrCharacters SET online = 0 WHERE characterID = %u", charID);
    sLog.Green("    PhantomSession", "Character %u is now offline (phantom removed).", charID);
}


void EntityList::Process() {
    Client* pClient(nullptr);
    std::vector<Client*>::iterator citr = m_clients.begin();
    while (citr != m_clients.end()) {
        if ((*citr)->ProcessNet()) {
            ++citr;
        } else {
            pClient = *citr;
            citr = m_clients.erase(citr);
            SafeDelete(pClient);
        }
    }

    if (m_targTimer.Check()) {
        std::unordered_map<SystemEntity*, TargetManager*>::iterator titr = m_targMgrs.begin();
        while (titr != m_targMgrs.end()) {
            if (titr->second->Process()) {
                ++titr;
            } else {
                titr = m_targMgrs.erase(titr);
            }
        }
        std::map<uint32, ProbeSE*>::iterator pitr = m_probes.begin();
        while (pitr != m_probes.end()) {
            if (pitr->second->ProcessTic()) {
                ++pitr;
            } else {
                pitr = m_probes.erase(pitr);
            }
        }
    }

    /* check for 1Hz timer tic */
    if (m_stampTimer.Check()) {
        double profileStartTime(GetTimeUSeconds());

        ++m_stamp;

        for (auto cur : m_players)
            if (cur.second->IsValidSession())   // verify client is constructed before calling ProcessClient() on it
                cur.second->ProcessClient();

    /** @todo test for adding OpenMP here to enable MP per system. */
    // this wont work....possibility of removing systems, therefore invalidating the iterator.
    // bad things can happen if this is running parallel on MP
    //#pragma omp parallel  // starts a new team
        std::map<uint32, SystemManager*>::iterator itr = m_systems.begin();
        while (itr != m_systems.end()) {
            if (itr->second == nullptr) { /* this shouldnt happen.  log error to make note */
                sLog.Error(" EntityList::Proc", "Deleting System %u", itr->first);
                itr = m_systems.erase(itr);
                continue;
            } else if (!itr->second->ProcessTic()) {    /* Process each loaded system */
                itr->second->UnloadSystem();
                SafeDelete(itr->second);
                itr = m_systems.erase(itr);
                continue;
            }
            ++itr;
        }

        // these need 1Hz tics
        sCivMgr.Process();
        sBubbleMgr.Process();

        // Vev tactical-grid 2D projection + stream (Route A unified world).
        // Runs here, on the main thread, after positions + bubbles settle for
        // this 1Hz tic: reads each active grid's evemu balls, projects 3D->2D,
        // and asio::posts the GridSnapshot to the gateway thread. 1 Hz cadence
        // (curator-locked) — the browser interpolates between frames.
        vev::grid::StreamGridsTick();

        // these minute tics do not need to be precise
        if (m_minuteTimer.Check()) {
            ++m_minutes;
            sMissionDataMgr.Process();  // 1m

            if (m_minutes % 5 == 0) { // ~5m
                sWHMgr.Process();
                // write something to tic corps vote cases.
                for (auto cur : m_systems)
                    cur.second->UpdateData();   // update active system timers and dynamic data every 5m
            }
            if (m_minutes % 15 == 0) { // ~15m
                sMktBotMgr.Process();  // 15m to 30m ---marketbot update; enabled this for timer checks in process
                sConsole.UpdateStatus();
            }
            if (m_minutes % 60 == 0) { // ~1h
                MapDB::ManipulateTimeData();
                sMktMgr.Process();  // not used - does nothing at this time
            }
        }

        if (sConfig.debug.UseProfiling)
            sProfiler.AddTime(Profile::entityS, GetTimeUSeconds() - profileStartTime);
    }

    // See A321 §4.6 — AI command queue poll (independent of 1Hz stamp timer)
    if (m_aiCmdTimer.Check())
        ProcessAICommandQueue();
}

SystemManager* EntityList::FindOrBootSystem(uint32 systemID) {
    if (!sDataMgr.IsSolarSystem(systemID)) {
        _log(SERVER__INIT_ERR, "BootSystem() called with invalid systemID (%u)", systemID);
        return nullptr;
    }

    std::map<uint32, SystemManager*>::iterator itr = m_systems.find(systemID);
    if (itr != m_systems.end())
        return itr->second;

    SystemManager* pSM = new SystemManager(systemID, *m_services);
    if ((pSM == nullptr) or (!pSM->BootSystem())) {
        _log(SERVER__INIT_ERR, "BootSystem() - Booting system %u failed", systemID);
        SafeDelete(pSM);
        return nullptr;
    }

    _log(SERVER__INIT, "BootSystem() - Booted system %u", systemID);
    m_systems[systemID] = pSM;
    return pSM;
}

// cannot put add/remove station in header due to incomplete StationItemRef class
void EntityList::AddStation(uint32 stationID, StationItemRef itemRef) {
    m_stations[stationID] = itemRef;
}

void EntityList::RemoveStation(uint32 stationID) {
    m_stations.erase(stationID);
}

Agent* EntityList::GetAgent(uint32 agentID) {
    std::map<uint32, Agent*>::iterator res = m_agents.find(agentID);
    if (res != m_agents.end())
        return res->second;

    Agent* pAgent = new Agent(agentID);
    if (!pAgent->Load()) {
        delete pAgent;
        return nullptr;
    }
    m_agents[agentID] = pAgent;
    return pAgent;
}

void EntityList::GetClients(std::vector<Client*> &result) const {
    for (auto cur : m_players)
        result.push_back(cur.second);
}

void EntityList::GetCorpClients(std::vector<Client*> &result, uint32 corpID) const {
    std::map<uint32, corpRole>::const_iterator cItr = m_corpMembers.find(corpID);
    if (cItr == m_corpMembers.end())
        return;

    corpRole::const_iterator itr = cItr->second.begin(), end = cItr->second.end();
    while (itr != end) {
        if (itr->first != nullptr)
            result.push_back(itr->first);
        ++itr;
    }
}

// this method is corrected, as stations have their own guestlist now.
void EntityList::GetStationGuestList(uint32 stationID, std::vector<Client*> &result) const {
    std::map<uint32, StationItemRef>::const_iterator itr = m_stations.find(stationID);
    if (itr != m_stations.end())
        itr->second->GetGuestList(result);
}

bool EntityList::IsOnline(uint32 charID)
{
    if (m_players.find(charID) != m_players.end())
        return true;
    // See A321 §4.7 — phantom players count as online
    if (m_phantomPlayers.find(charID) != m_phantomPlayers.end())
        return true;
    return false;
}

PyRep* EntityList::PyIsOnline(uint32 charID)
{
    if (m_players.find(charID) != m_players.end())
        return PyStatic.NewTrue();
    // See A321 §4.7 — phantom players count as online
    if (m_phantomPlayers.find(charID) != m_phantomPlayers.end())
        return PyStatic.NewTrue();
    return PyStatic.NewFalse();
}

Client* EntityList::FindClientByCharID(uint32 charID) const
{
    std::map<uint32, Client*>::const_iterator itr = m_players.find(charID);
    if (itr != m_players.end())
        return itr->second;
    return nullptr;
}

StationItemRef EntityList::GetStationByID(uint32 stationID) {
    std::map<uint32, StationItemRef>::iterator res = m_stations.find(stationID);
    if (res != m_stations.end())
        return res->second;
    return StationItemRef(nullptr);
}

std::string EntityList::GetAnomalyID()
{
    // these should be totally unique.  design a way to enforce this
    std::string str1 = "", str2 = "";
    for (uint8 i = 0; i < 3; ++i) {
        str1 += alphaList[MakeRandomInt(0,25)];    //rand() % sizeof(alphaList) - 1
        str2 += std::to_string(MakeRandomInt(0,9));
    }

    std::string res = str1;
    res += "-";
    res += str2;
    // not sure if we need to keep track of these IDs...
    //m_anomIDs.push_back(res);
    return res;
}

void EntityList::GetUpTime( std::string& time )
{
    float seconds = m_stamp - 1000;
    float minutes = seconds/60;
    float hours = minutes/60;
    float days = hours/24;
    float weeks = days/7;
    float months = days/30;

    int s(fmod(seconds, 60));
    int m(fmod(minutes, 60));
    int h(fmod(hours, 24));
    int d(fmod(days, 7));
    int w(fmod(weeks, 4));
    int M(fmod(months, 12));

    std::ostringstream uptime;
    if (M) {
        uptime << M << "M" << w << "w" << d << "d" << h << "h" << m << "m" << s << "s";
    } else if (w) {
        uptime << w << "w" << d << "d" << h << "h" << m << "m" << s << "s";
    } else if (d) {
        uptime << d << "d" << h << "h" << m << "m" << s << "s";
    } else if (h) {
        uptime << h << "h" << m << "m" << s << "s";
    } else if (m) {
        uptime << m << "m" << s << "s";
    } else {
        uptime << s << "s";
    }

    //std::shared_ptr<const char*> ret = uptime.str().c_str();
    time = uptime.str();
}


// this is my answer to the crazy looping of Multicast shit...
void EntityList::CorpNotify(uint32 corpID, uint8 bCastType, const char* notifyType, const char* idType, PyTuple* payload) const
{
    // make sure this is player corp (which it really should be, but just in case....)
    if (IsNPCCorp(corpID))
        return;
    std::map<uint32, Client*> cMap;
    std::map<uint32, corpRole>::const_iterator cItr = m_corpMembers.find(corpID);
    if (cItr == m_corpMembers.end()) {
        PySafeDecRef(payload);
        return; // no corp members online now.  nothing to do here.
    }

    // determine who in corp needs to be notified
    using namespace Notify::Types;
    //using namespace Corp::Role;
    // auto doesnt work here...dunno why yet.
    corpRole::const_iterator itr = cItr->second.begin(), end = cItr->second.end();
    switch (bCastType) {
        case CorpNews:
        case CorpNewCEO:
        case CharLeftCorp: {
            // all members?
            while (itr != end) {
                cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;
        case CorpAppNew:
        case CorpAppReject:
        case CorpAppAccept: {
            // who else wants/needs this?
            // PersonnelManager is only role that can view corp applications
            while (itr != end) {
                //if ((itr->second & Corp::Role::Director) == Corp::Role::Director)
                //    cMap.insert(std::make_pair(std::make_pair(itr->first->GetCharacterID(), itr->first)));
                if ((itr->second & Corp::Role::PersonnelManager) == Corp::Role::PersonnelManager)
                    cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;
        case CorpVote: {
            // any member that can vote (has shares)
            //  damn...dunno if i wanna do this one like this....hit db every loop here??  fukin nuts!
            // this is another vote for putting "corp shares" in character.corpData
            //    well, then we'd have to hit db for offine chars.....omg
            CorporationDB mdb;
            while (itr != end) {
                // if (itr->first->GetChar()->HasShares())  // not written, no underlying code yet
                if (mdb.HasShares(itr->first->GetCharacterID(), corpID))
                    cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;

        // unused yet.  (not coded or not understood  ...mostly the latter at this point in corp code)
        case CharMedal:
        case AllMaintenanceBill:
        case AllWarDeclared:
        case AllWarSurrender:
        case AllWarRetracted:
        case AllWarInvalidated:
        case CharBill:
        case CorpAllBill:
        case BillOutOfMoney:
        case BillPaidChar:
        case BillPaidCorpAll:
        case CorpTaxChange:
        case CorpDividend:
        case CorpVoteCEORevoked:
        case CorpWarDeclared:
        case CorpWarFightingLegal:
        case CorpWarSurrender:
        case CorpWarRetracted:
        case CorpWarInvalidated:
        case ContainerPassword:
        case SovAllClaimFail:
        case SovCorpClaimFail:
        case SovAllBillLate:
        case SovCorpBillLate:
        case SovAllClaimLost:
        case SovCorpClaimLost:
        case SovAllClaimAquired:
        case SovCorpClaimAquired:
        case AllAnchoring:
        case AllStructVulnerable:
        case AllStrucInvulnerable:
        case SovDisruptor:
        case CorpStructLost:
        case CorpOfficeExpiration:
        case FWCorpJoin:
        case FWCorpLeave:
        case FWCorpKick:
        case FWCharKick:
        case FWCorpWarning:
        case FWCharWarning:
        case FWCharRankLoss:
        case FWCharRankGain:
        case FWAllianceWarning:
        case FWAllianceKick:
        case TransactionReversal:
        case Reimbursement:
        case TowerAlert:
        case TowerResourceAlert:
        case StationAggression1:
        case StationStateChange:
        case StationConquer:
        case StationAggression2:
        case FacWarCorpJoinRequest:
        case FacWarCorpLeaveRequest:
        case FacWarCorpJoinWithdraw:
        case FacWarCorpLeaveWithdraw:
        case CorpLiquidation:
        case SovereigntyTCUDamage:
        case SovereigntySBUDamage:
        case SovereigntyIHDamage:
        case ContactAdd:
        case ContactEdit:
        case CorpKicked:
        case OrbitalAttacked:
        case OrbitalReinforced:
        case OwnershipTransferred:
            break;

        // internal corp notifications
        case FactoryJob: {      // factory job completion added to calendar
            // who else wants/needs this?
            //  lets start with factory manager, and may have to add later
            while (itr != end) {
                if ((itr->second & Corp::Role::FactoryManager) == Corp::Role::FactoryManager)
                    cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;
        case MarketOrder: {
            // who else wants/needs this?
            //  lets start with traders, and may have to add later
            while (itr != end) {
                if ((itr->second & Corp::Role::Trader) == Corp::Role::Trader)
                    cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;
        case WalletChange: {
            while (itr != end) {
                if ((itr->second & Corp::Role::Accountant) == Corp::Role::Accountant)
                    cMap.emplace(itr->first->GetCharacterID(), itr->first);
                if ((itr->second & Corp::Role::Auditor) == Corp::Role::Auditor)
                    cMap.emplace(itr->first->GetCharacterID(), itr->first);
                // this may need to check if player has access to division changed - will require a LOT more code
                if ((itr->second & Corp::Role::JuniorAccountant) == Corp::Role::JuniorAccountant)
                    cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;
        case ItemUpdateStation: {
            // all members?
            while (itr != end) {
                cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;
        case ItemUpdateSystem: {
            // all members?
            while (itr != end) {
                cMap.emplace(itr->first->GetCharacterID(), itr->first);
                ++itr;
            }
        } break;
    }

    for (auto cur : cMap) {
        PyIncRef(payload);
        cur.second->SendNotification( notifyType, idType, payload, false );   // are any of these sequenced?
    }

    PyDecRef(payload);
}

void EntityList::Broadcast(const char* notifyType, const char* idType, PyTuple** payload) const {
    //build a little notification out of it.
    EVENotificationStream notify;
        notify.remoteObject = 1;
        notify.args = *payload;
    payload = nullptr;    //consumed

    //now sent it to the client
    PyAddress dest;
        dest.type = PyAddress::Broadcast;
        dest.service = notifyType;
        dest.bcast_idtype = idType;
    Broadcast(dest, notify);
}

void EntityList::Broadcast(const PyAddress &dest, EVENotificationStream &noti) const {
    for (auto cur : m_players)
        cur.second->SendNotification(dest, noti);
}

void EntityList::Multicast(const character_set &cset, const PyAddress &dest, EVENotificationStream &noti) const {
    std::map<uint32, Client*>::const_iterator itr = m_players.begin();
    for (auto cur : cset) {
        itr = m_players.find(cur);
        if (itr != m_players.end())
            itr->second->SendNotification(dest, noti);
    }
}

// updated to remove looping thru entire client list for each call....still needs work
void EntityList::Multicast( const char* notifyType, const char* idType, PyTuple** in_payload, NotificationDestination target, uint32 targID, bool seq )
{
    PyTuple* payload = *in_payload;
    in_payload = nullptr;

    std::vector<Client*> cVec;
    cVec.clear();
    switch( target ) {
        case NOTIF_DEST__LOCATION: {
            if (sDataMgr.IsStation(targID)) {
                GetStationGuestList(targID, cVec);
            } else if (sDataMgr.IsSolarSystem(targID)) {
                SystemManager* pSysMgr = FindOrBootSystem(targID);
                if (pSysMgr == nullptr)
                    break;
                pSysMgr->GetClientList(cVec);
            } else {
                sLog.Error("EntityList::Multicast 1", "DEST__LOCATION - location %u is neither station nor system", targID);
                EvE::traceStack();
            }
        } break;
        case NOTIF_DEST__CORPORATION: {
            std::map<uint32, corpRole>::const_iterator cItr = m_corpMembers.find(targID);
            if (cItr == m_corpMembers.end())
                break;
            corpRole::const_iterator itr = cItr->second.begin();
            while (itr != cItr->second.end()) {
                cVec.push_back(itr->first);
                ++itr;
            }
        } break;
    };

    for (auto cur : cVec) {
        PyIncRef(payload);
        cur->SendNotification( notifyType, idType, &payload, seq );
    }

    PyDecRef( payload );
}

// updated.  so much better this way.
void EntityList::Multicast(const char* notifyType, const char* idType, PyTuple** in_payload, const MulticastTarget &mcset, bool seq)
{
    // consume payload
    PyTuple* payload = *in_payload;
    in_payload = nullptr;

    if (!mcset.characters.empty())
        for (auto cur : mcset.characters) {
            std::map<uint32, Client*>::iterator itr = m_players.find(cur);
            if ( itr != m_players.end()) {
                PyIncRef(payload);
                itr->second->SendNotification( notifyType, idType, &payload, seq );
            }
        }

    if (!mcset.locations.empty()) {
        SystemManager* pSysMgr(nullptr);
        std::vector<Client*> cVec;
        cVec.clear();
        for (auto cur : mcset.locations) {
            if (sDataMgr.IsStation(cur)) {
                GetStationGuestList(cur, cVec);
            } else if (sDataMgr.IsSolarSystem(cur)) {
                pSysMgr = FindOrBootSystem(cur);
                if (pSysMgr == nullptr)
                    continue;
                pSysMgr->GetClientList(cVec);
            } else {
                sLog.Error("EntityList::Multicast 2", "location %u is neither station nor system", cur);
                EvE::traceStack();
            }
        }
        for (auto cur : cVec) {
            PyIncRef(payload);
            cur->SendNotification( notifyType, idType, &payload, seq );
        }
    }

    // this will need list of interested parties from corp.  update this call to use CorpNotify() where possible.
    if (!mcset.corporations.empty()) {
        sLog.Error("EntityList::Multicast 2", "Corporation MulticastTarget called.");
        EvE::traceStack();
        for (auto cur : mcset.corporations) {
            std::map<uint32, corpRole>::const_iterator cItr = m_corpMembers.find(cur);
            if (cItr == m_corpMembers.end())
                continue;
            corpRole::const_iterator itr = cItr->second.begin();
            while (itr != cItr->second.end()) {
                PyIncRef(payload);
                itr->first->SendNotification( notifyType, idType, &payload, seq );
                ++itr;
            }
        }
    }

    PyDecRef( payload );
}

void EntityList::Multicast(const character_set &cset, const char* notifyType, const char* idType, PyTuple** in_payload, bool seq) const
{
    // consume payload
    PyTuple* payload = *in_payload;
    in_payload = nullptr;

    std::map<uint32, Client*>::const_iterator itr = m_players.begin();
    for (auto cur : cset) {
        itr = m_players.find(cur);
        if (itr != m_players.end()) {
            PyIncRef(payload);
            itr->second->SendNotification(notifyType, idType, &payload, seq);
        }
    }
    PyDecRef( payload );
}

void EntityList::Unicast(uint32 charID, const char* notifyType, const char* idType, PyTuple** payload, bool seq) {
    Client* pClient = FindClientByCharID(charID);
    if (pClient != nullptr)
        pClient->SendNotification( notifyType, idType, payload, seq );
}

/** @todo @note NOTE: TODO: HACK: the Find* methods below can get very expensive for many players */

//used by gmCommands....i dont like this one either....but at least it's use will be seldom
Client* EntityList::FindClientByName(const char* name) const {
    for (auto cur : m_players) {
        CharacterRef cRef = cur.second->GetChar();
        if (cRef.get() != nullptr)
            if (strcmp(cRef->name(), name) == 0)
                return cur.second;
    }
    return nullptr;
}

/** @todo  this needs more work.  hacked for now...  */
void EntityList::RegisterSID(int64 &sessionID) {
    /*  this whole method is just made up...eventually it will return a unique long long */
    /* max for int64 = 9223372036854775807 */
    std::set<int64>::iterator itr = m_sessions.find(sessionID);
    std::pair<std::set<int64>::iterator, bool > test;
    if (itr == m_sessions.end())
        test = m_sessions.insert(sessionID);
    if (test.second)
        return;
}

void EntityList::RemoveSID ( int64 sessionID ) {
    m_sessions.erase(sessionID);
}

// VEV_ANOM_AI_GATE: does any engine-driven AI pilot fly in this system?
// AnomalyMgr's spawn gate counts only Client* logins, so AI-only systems
// never generated signals -- the explore career had nothing to find.
bool EntityList::HasAIShipInSystem(uint32 systemID) {
    for (auto& kv : m_aiShips) {
        if (kv.second != nullptr && kv.second->SystemMgr() != nullptr
            && kv.second->SystemMgr()->GetID() == systemID)
            return true;
    }
    return false;
}

AIShipSE* EntityList::FindAIShip(uint32 charID) {
    auto itr = m_aiShips.find(charID);
    if (itr != m_aiShips.end())
        return itr->second;
    return nullptr;
}

// See A321 §4.2 + §4.6 (AI Command Queue — Python→C++ bridge)
// Polls ai_command_queue for pending commands and executes them.
// Phase 1: station-based commands (market sell/buy, skill queue).
// Phase 2+: movement/combat via SystemManager dispatch.
// =================== VEV_SIM_TOGGLE_CYCLES ===================
// Traditional cycle mechanics for the VEV_SIM_TOGGLE modules: an engaged
// toggle debits capacitor (attr 6) every cycle (attr 73 duration — AB 10 s,
// DC 30 s, hardeners per dogma); a dry capacitor auto-disengages it (saved
// attrs restored), exactly the classic behavior. Registry is file-scope so
// the queue tick can cycle it; all access is on the eve-server thread.
// Engage/disengage persist the changed SHIP attrs to entity_attributes so
// the vev-gateway (asio threads — must not touch live item refs) serves
// live resonances/velocity from the DB (VEV_MODULE_LIVE in Gateway.cpp).
struct VevToggleState {
    uint32 shipItemID;
    uint32 charID;
    std::vector<std::pair<uint16, float>> saved;
    float capNeed;
    int64 durationMs;
    int64 nextCycleMs;
    std::string name;
};
static std::map<uint32, VevToggleState> s_vevToggleReg;

static void vevPersistShipAttr(uint32 itemID, uint16 attr, float val) {
    DBerror err;
    sDatabase.RunQuery(err,
        "INSERT INTO entity_attributes (itemID, attributeID, valueInt, valueFloat) "
        "VALUES (%u, %u, NULL, %f) "
        "ON DUPLICATE KEY UPDATE valueInt = NULL, valueFloat = %f",
        itemID, attr, val, val);
}

static void vevToggleRestore(VevToggleState& ts, const char* why) {
    InventoryItemRef shipRef = sItemFactory.GetItemRef(ts.shipItemID);
    for (auto& kv : ts.saved) {
        if (shipRef.get() != nullptr)
            shipRef->SetAttribute(kv.first, kv.second);
        vevPersistShipAttr(ts.shipItemID, kv.first, kv.second);
    }
    sLog.Cyan("activate_module", "TOGGLE-OFF char=%u %s (%s)", ts.charID, ts.name.c_str(), why);
}

static void vevProcessToggleCycles() {
    const int64 nowMs = GetTimeMSeconds();
    for (auto it = s_vevToggleReg.begin(); it != s_vevToggleReg.end(); ) {
        VevToggleState& ts = it->second;
        if (nowMs < ts.nextCycleMs) { ++it; continue; }
        InventoryItemRef shipRef = sItemFactory.GetItemRef(ts.shipItemID);
        if (shipRef.get() == nullptr) { it = s_vevToggleReg.erase(it); continue; }
        const float cap = shipRef->HasAttribute(AttrCapacitorCharge)
            ? shipRef->GetAttribute(AttrCapacitorCharge).get_float() : 0.0f;
        if (ts.capNeed > 0.0f && cap < ts.capNeed) {
            vevToggleRestore(ts, "capacitor dry");
            it = s_vevToggleReg.erase(it);
            continue;
        }
        if (ts.capNeed > 0.0f)
            shipRef->SetAttribute(AttrCapacitorCharge, cap - ts.capNeed);
        ts.nextCycleMs = nowMs + ts.durationMs;
        ++it;
    }
}
// =================== end VEV_SIM_TOGGLE_CYCLES ===================

// =================== VEV_SIM_PASSIVES ===================
// Passive fitted modules do NOTHING for phantoms (dogma effects need Client*).
// Recompute the ship item's derived attrs from TYPE BASE + every fitted+online
// passive at undock / login_in_space. IDEMPOTENT: always starts from the
// type's dgmTypeAttributes (+ invTypes.capacity for cargo), so repeated calls
// never compound. Active modules (toggles/reps/weapons) are EXCLUDED — their
// sims manage their own attrs (and engage AFTER spawn, so recalc-at-undock
// never clobbers an engaged toggle).
static void vevRecalcPassives(uint32 charID) {
    uint32 shipID = 0, shipTypeID = 0;
    {
        DBQueryResult r;
        if (!sDatabase.RunQuery(r,
            "SELECT c.shipID, e.typeID FROM chrCharacters c JOIN entity e ON e.itemID = c.shipID WHERE c.characterID = %u", charID))
            return;
        DBResultRow row;
        if (!r.GetRow(row)) return;
        shipID = row.GetUInt(0); shipTypeID = row.GetUInt(1);
    }
    if (shipID == 0 || shipTypeID == 0) return;
    InventoryItemRef shipRef = sItemFactory.GetItemRef(shipID);
    if (shipRef.get() == nullptr) return;

    // 1) base values for every attr passives can touch.
    //    263 shieldCap, 265 armorHP, 9 hullHP, 4 mass, 37 maxVel, 70 agility,
    //    55 rechargeRate, 482 capCapacity, 479 shieldRechargeRate, resonances.
    std::map<uint16, float> vals;
    static const uint16 kDerived[] = { 263, 265, 9, 4, 37, 70, 55, 482, 479,
                                       267,268,269,270, 271,272,273,274, 109,110,111,113 };
    for (size_t i = 0; i < sizeof(kDerived)/sizeof(kDerived[0]); ++i) vals[kDerived[i]] = 0.0f;
    vals[267]=vals[268]=vals[269]=vals[270]=1.0f;   // armor resonances default
    vals[271]=vals[272]=vals[273]=vals[274]=1.0f;   // shield
    vals[109]=vals[110]=vals[111]=vals[113]=1.0f;   // hull
    {
        DBQueryResult r;
        if (sDatabase.RunQuery(r,
            "SELECT attributeID, COALESCE(valueFloat, valueInt) FROM dgmTypeAttributes WHERE typeID = %u", shipTypeID)) {
            DBResultRow row;
            while (r.GetRow(row)) {
                uint16 a = (uint16)row.GetUInt(0);
                if (vals.find(a) != vals.end()) vals[a] = row.GetFloat(1);
            }
        }
    }
    float cargoBase = 0.0f;
    {
        DBQueryResult r;
        if (sDatabase.RunQuery(r, "SELECT capacity FROM invTypes WHERE typeID = %u", shipTypeID)) {
            DBResultRow row;
            if (r.GetRow(row)) cargoBase = row.GetFloat(0);
        }
    }
    float cargoMult = 1.0f;

    // 2) fold in every fitted + ONLINE passive module's bonus attrs.
    //    Groups: 38 shield extender, 329 armor plates, 765 expanded cargohold,
    //    764 overdrive, 763 nanofiber, 43 cap recharger, 61 cap battery,
    //    39 shield recharger, 770 shield flux coil, 98 armor coating,
    //    295 shield amplifier, 326 energized plating.
    DBQueryResult mods;
    if (!sDatabase.RunQuery(mods,
        "SELECT da.attributeID, COALESCE(da.valueFloat, da.valueInt)"
        " FROM entity e"
        " JOIN invTypes t ON t.typeID = e.typeID"
        " JOIN invGroups g ON g.groupID = t.groupID"
        " LEFT JOIN entity_attributes ea ON ea.itemID = e.itemID AND ea.attributeID = 2"
        " JOIN dgmTypeAttributes da ON da.typeID = e.typeID"
        " WHERE e.locationID = %u AND e.flag BETWEEN 11 AND 34"
        "   AND g.groupID IN (38,329,765,764,763,43,61,39,770,98,295,326)"
        "   AND COALESCE(ea.valueInt, 1) <> 0"
        "   AND da.attributeID IN (72,1159,796,149,150,144,134,147,67,1076,169,"
        "                          267,268,269,270,271,272,273,274,974,975,976,977)",
        shipID))
        return;
    DBResultRow mrow;
    while (mods.GetRow(mrow)) {
        uint16 a = (uint16)mrow.GetUInt(0);
        float v = mrow.GetFloat(1);
        switch (a) {
            case 72:   vals[263] += v; break;                     // shield extender capacity add
            case 1159: vals[265] += v; break;                     // plates armorHP add
            case 796:  vals[4]   += v; break;                     // plates mass add
            case 149:  cargoMult *= v; break;                     // cargohold + / overdrive -
            case 150:  vals[9]   *= v; break;                     // nanofiber hull malus
            case 144:  vals[55]  *= v; break;                     // cap recharger (lower = faster)
            case 134:  vals[479] *= v; break;                     // shield recharger
            case 147:  vals[482] *= v; break;                     // battery capacity mult
            case 67:   vals[482] += v; break;                     // battery flat add
            case 1076: vals[37]  *= (1.0f + v / 100.0f); break;   // overdrive/nanofiber velocity %
            case 169:  vals[70]  *= (1.0f + v / 100.0f); break;   // nanofiber agility % (negative)
            case 267: case 268: case 269: case 270:
            case 271: case 272: case 273: case 274:
                if (v > 0.0f && v < 1.0f) vals[a] *= v; break;    // passive armor/shield resists
            case 974: if (v > 0.0f && v < 1.0f) vals[113] *= v; break;   // hull em
            case 975: if (v > 0.0f && v < 1.0f) vals[111] *= v; break;   // hull explosive
            case 976: if (v > 0.0f && v < 1.0f) vals[109] *= v; break;   // hull kinetic
            case 977: if (v > 0.0f && v < 1.0f) vals[110] *= v; break;   // hull thermal
            default: break;
        }
    }

    // 3) write the recomputed values onto the live ship item
    for (std::map<uint16, float>::iterator it = vals.begin(); it != vals.end(); ++it)
        if (it->second > 0.0f) shipRef->SetAttribute(it->first, it->second);
    if (cargoBase > 0.0f) shipRef->SetAttribute(38, cargoBase * cargoMult);
    sLog.Cyan("VEV_SIM_PASSIVES", "recalc char=%u ship=%u: shield=%.0f armor=%.0f hull=%.0f vmax=%.0f cargo=%.0f",
        charID, shipID, vals[263], vals[265], vals[9], vals[37], cargoBase * cargoMult);
}
// =================== end VEV_SIM_PASSIVES ===================

void EntityList::ProcessAICommandQueue() {
    vevProcessToggleCycles();   // VEV_SIM_TOGGLE_CYCLES
    DBQueryResult res;
    if (!sDatabase.RunQuery(res,
        "SELECT id, charID, command, params FROM ai_command_queue WHERE status = 'pending' ORDER BY id LIMIT 10"))
    {
        return;  // query failed silently — don't spam logs every 5s
    }

    DBResultRow row;
    while (res.GetRow(row)) {
        uint32 cmdID   = row.GetUInt(0);
        uint32 charID  = row.GetUInt(1);
        const char* command = row.GetText(2);
        const char* params  = row.IsNull(3) ? "{}" : row.GetText(3);

        // Mark as executing
        DBerror err;
        sDatabase.RunQuery(err,
            "UPDATE ai_command_queue SET status = 'executing' WHERE id = %u", cmdID);

        // Dispatch command
        std::string resultMsg;
        bool success = ExecuteAICommand(charID, command, params, resultMsg);

        // Escape result message for safe SQL insertion (single quotes etc.)
        std::string escapedMsg;
        sDatabase.DoEscapeString(escapedMsg, resultMsg);

        // Mark done or failed
        if (success) {
            sDatabase.RunQuery(err,
                "UPDATE ai_command_queue SET status = 'done', result_msg = '%s', executed_at = NOW() WHERE id = %u",
                escapedMsg.c_str(), cmdID);
        } else {
            sDatabase.RunQuery(err,
                "UPDATE ai_command_queue SET status = 'failed', result_msg = '%s', executed_at = NOW() WHERE id = %u",
                escapedMsg.c_str(), cmdID);
        }

        sLog.Cyan("    AICommandQueue", "cmd %u: char=%u command='%s' → %s: %s",
            cmdID, charID, command, success ? "done" : "FAILED", resultMsg.c_str());
    }
}

// See A321 §4.6 — Command dispatch switch.
// Phase 1: station-based ops (market, skills).
// Phase 2: movement/combat via AIShipSE + DestinyManager/TargetManager.
bool EntityList::ExecuteAICommand(uint32 charID, const char* command, const char* params, std::string &resultMsg) {
    std::string cmd(command);

    // --- Phase 1: Station-based commands (pure DB, no in-space entity needed) ---

    if (cmd == "sell_ore" || cmd == "sell_item") {
        resultMsg = "use 'sell_to_buy_order' instead — see A331 §3.5";
        return false;
    }

    if (cmd == "ping") {
        resultMsg = "pong";
        return true;
    }

    if (cmd == "set_balance") {
        // params: {"amount": 50000.0}
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT online FROM chrCharacters WHERE characterID = %u", charID))
        {
            resultMsg = "character not found";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        if (charRow.GetUInt(0) != 0) {
            resultMsg = "cannot modify online character (SaveCharacter overwrite)";
            return false;
        }
        std::string p(params);
        size_t pos = p.find("\"amount\"");
        if (pos == std::string::npos) {
            resultMsg = "missing 'amount' in params";
            return false;
        }
        pos = p.find(":", pos);
        if (pos == std::string::npos) {
            resultMsg = "malformed params";
            return false;
        }
        double amount = atof(p.c_str() + pos + 1);
        if (amount < 0) {
            resultMsg = "amount must be >= 0";
            return false;
        }
        DBerror err;
        if (!sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET balance = %.2f WHERE characterID = %u", amount, charID))
        {
            resultMsg = "DB update failed";
            return false;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "balance set to %.2f", amount);
        resultMsg = buf;
        return true;
    }

    // --- spawn_in_space: Create AIShipSE for an AI character in a solar system ---
    // See A321 §4.2 — THE critical primitive for real in-game AI characters.
    // params: {"systemID": 30002187, "typeID": 17478}
    //   systemID = target solar system
    //   typeID = ship type to fly (optional — defaults to character's current ship type)
    if (cmd == "spawn_in_space") {
        // Check if already spawned
        if (HasAIShip(charID)) {
            resultMsg = "AI ship already in space for this character";
            return false;
        }

        // Parse systemID from params
        std::string p(params);
        size_t sysPos = p.find("\"systemID\"");
        if (sysPos == std::string::npos) {
            resultMsg = "missing 'systemID' in params";
            return false;
        }
        size_t colonPos = p.find(":", sysPos);
        if (colonPos == std::string::npos) {
            resultMsg = "malformed params (systemID)";
            return false;
        }
        uint32 systemID = (uint32)atol(p.c_str() + colonPos + 1);
        if (systemID == 0) {
            resultMsg = "invalid systemID";
            return false;
        }

        // Parse optional typeID from params (ship type)
        uint32 shipTypeID = 0;
        size_t typePos = p.find("\"typeID\"");
        if (typePos != std::string::npos) {
            colonPos = p.find(":", typePos);
            if (colonPos != std::string::npos)
                shipTypeID = (uint32)atol(p.c_str() + colonPos + 1);
        }

        // Query character data from DB
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT c.characterName, c.corporationID, c.shipID, c.stationID, c.solarSystemID,"
            " c.securityRating, c.bounty,"
            " IFNULL(co.allianceID, 0), IFNULL(co.warFactionID, 0)"
            " FROM chrCharacters c"
            " LEFT JOIN crpCorporation co ON c.corporationID = co.corporationID"
            " WHERE c.characterID = %u", charID))
        {
            resultMsg = "DB query failed";
            return false;
        }
        DBResultRow row;
        if (!charRes.GetRow(row)) {
            resultMsg = "character not found";
            return false;
        }

        std::string charName = row.GetText(0);
        uint32 corpID = row.GetUInt(1);
        uint32 shipID = row.GetUInt(2);
        uint32 stationID = row.GetUInt(3);
        // uint32 solarSystemID = row.GetUInt(4);  // character's current system (not used — we spawn in target)
        float securityRating = row.GetFloat(5);
        float bounty = row.GetFloat(6);
        int32 allianceID = row.GetInt(7);
        int32 warFactionID = row.GetInt(8);

        // If no typeID specified, get it from the character's current ship
        if (shipTypeID == 0 && shipID > 0) {
            DBQueryResult shipRes;
            if (sDatabase.RunQuery(shipRes,
                "SELECT typeID FROM entity WHERE itemID = %u", shipID))
            {
                DBResultRow shipRow;
                if (shipRes.GetRow(shipRow))
                    shipTypeID = shipRow.GetUInt(0);
            }
        }
        if (shipTypeID == 0) {
            // Fallback: Capsule (670) — every character can fly one
            shipTypeID = 670;
        }

        // Get station position for spawn point (spawn near a station in the target system)
        // NOTE: Stations are NOT in the 'entity' table — use staStations which has x,y,z.
        GPoint spawnPos(0, 0, 0);
        {
            DBQueryResult stationRes;
            if (sDatabase.RunQuery(stationRes,
                "SELECT x, y, z FROM staStations WHERE solarSystemID = %u LIMIT 1", systemID))
            {
                DBResultRow stationRow;
                if (stationRes.GetRow(stationRow)) {
                    spawnPos.x = stationRow.GetDouble(0);
                    spawnPos.y = stationRow.GetDouble(1);
                    spawnPos.z = stationRow.GetDouble(2);
                }
            }
        }
        // Offset slightly from station (5km random sphere) so we don't clip inside
        spawnPos.MakeRandomPointOnSphere(5000.0);

        // Boot the target solar system if not loaded
        SystemManager* pSystem = FindOrBootSystem(systemID);
        if (pSystem == nullptr) {
            resultMsg = "failed to load solar system";
            return false;
        }

        // Create a new ship item in the target system
        ItemData idata(shipTypeID, charID, systemID, flagNone,
                       charName.c_str(), spawnPos, "ai_agent_ship");
        InventoryItemRef shipRef = sItemFactory.SpawnItem(idata);
        if (shipRef.get() == nullptr) {
            resultMsg = "failed to create ship item";
            return false;
        }

        // Populate faction data
        FactionData fData = FactionData();
        fData.ownerID = charID;
        fData.corporationID = corpID;
        fData.allianceID = allianceID;
        fData.factionID = warFactionID;

        // Create the AIShipSE
        AIShipSE* pAIShip = new AIShipSE(shipRef, *m_services, pSystem, fData,
                                          charID, charName.c_str(), securityRating, bounty);
        if (pAIShip == nullptr) {
            resultMsg = "failed to create AIShipSE";
            return false;
        }

        // Add to solar system (registers with bubble manager, entity maps, etc.)
        pSystem->AddEntity(pAIShip, false);
        pAIShip->DestinyMgr()->SetPosition(spawnPos);

        // Track in EntityList for future command dispatch
        AddAIShip(charID, pAIShip);

        char buf[128];
        snprintf(buf, sizeof(buf), "spawned shipType=%u itemID=%u in system=%u at (%.0f,%.0f,%.0f)",
                 shipTypeID, shipRef->itemID(), systemID, spawnPos.x, spawnPos.y, spawnPos.z);
        resultMsg = buf;
        return true;
    }

    // --- despawn: Remove AIShipSE from space ---
    // params: {} (no extra params needed — charID identifies the ship)
    if (cmd == "despawn") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "no AI ship in space for this character";
            return false;
        }
        pAIShip->Despawn();
        resultMsg = "AI ship removed from space";
        return true;
    }

    // --- goto2d: in-plane move (keeps current Y) for 2D pilots ---
    // params: {"x": 123.0, "z": 789.0}  (SYSTEM coords; Y taken from the ball)
    if (cmd == "goto2d") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space - undock first";
            return false;
        }
        std::string p(params);
        double gx = 0, gz = 0;
        size_t xPos = p.find("\"x\"");
        size_t zPos = p.find("\"z\"");
        if (xPos != std::string::npos) { size_t c = p.find(":", xPos); if (c != std::string::npos) gx = atof(p.c_str() + c + 1); }
        if (zPos != std::string::npos) { size_t c = p.find(":", zPos); if (c != std::string::npos) gz = atof(p.c_str() + c + 1); }
        GPoint cur = pAIShip->GetPosition();
        GPoint target(gx, cur.y, gz);
        DestinyManager* dm = pAIShip->DestinyMgr();
        float curFrac = dm->GetUserSpeedFraction();
        if (curFrac <= 0.01f) {
            // stopped -> full speed, decelerate to a stop ON the point (no overshoot)
            dm->MoveToPointAndStop(target, 1.0f);
            resultMsg = "moving to target (full speed, auto-stop)";
        } else {
            // already at a commanded speed -> just vector toward the point at that speed
            dm->GotoDirection(GPoint(target.x - cur.x, 0.0, target.z - cur.z));
            dm->SetSpeedFraction(curFrac, true);
            resultMsg = "vectoring to target at current speed";
        }
        return true;
    }

    // --- set_vector: hold a 2D heading at a sub-warp speed fraction (the
    //     draggable velocity arrow). params: {"dirX":..,"dirZ":..,"fraction":0..1}
    if (cmd == "set_vector") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "AI ship not in space - undock first"; return false; }
        std::string p(params);
        double dx = 0, dz = 0, f = 0;
        size_t xPos = p.find("\"dirX\"");
        size_t zPos = p.find("\"dirZ\"");
        size_t fPos = p.find("\"fraction\"");
        if (xPos != std::string::npos) { size_t c = p.find(":", xPos); if (c != std::string::npos) dx = atof(p.c_str() + c + 1); }
        if (zPos != std::string::npos) { size_t c = p.find(":", zPos); if (c != std::string::npos) dz = atof(p.c_str() + c + 1); }
        if (fPos != std::string::npos) { size_t c = p.find(":", fPos); if (c != std::string::npos) f = atof(p.c_str() + c + 1); }
        if (f < 0) f = 0; if (f > 1) f = 1;
        pAIShip->DestinyMgr()->GotoDirection(GPoint(dx, 0.0, dz));
        pAIShip->DestinyMgr()->SetSpeedFraction((float)f, true);
        resultMsg = "holding commanded vector";
        return true;
    }

    // --- set_speed_fraction: sub-warp throttle 0..1, heading unchanged ---
    if (cmd == "set_speed_fraction") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "AI ship not in space - undock first"; return false; }
        std::string p(params);
        double f = 0;
        size_t fPos = p.find("\"fraction\"");
        if (fPos != std::string::npos) { size_t c = p.find(":", fPos); if (c != std::string::npos) f = atof(p.c_str() + c + 1); }
        if (f < 0) f = 0; if (f > 1) f = 1;
        pAIShip->DestinyMgr()->SetSpeedFraction((float)f, true);
        resultMsg = "speed fraction set";
        return true;
    }

    // --- goto: Move AI ship to a point in space ---
    // params: {"x": 123.0, "y": 456.0, "z": 789.0}
    if (cmd == "goto") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — spawn first";
            return false;
        }
        std::string p(params);
        // Parse x, y, z
        double gx = 0, gy = 0, gz = 0;
        size_t xPos = p.find("\"x\"");
        size_t yPos = p.find("\"y\"");
        size_t zPos = p.find("\"z\"");
        if (xPos != std::string::npos) { size_t c = p.find(":", xPos); if (c != std::string::npos) gx = atof(p.c_str() + c + 1); }
        if (yPos != std::string::npos) { size_t c = p.find(":", yPos); if (c != std::string::npos) gy = atof(p.c_str() + c + 1); }
        if (zPos != std::string::npos) { size_t c = p.find(":", zPos); if (c != std::string::npos) gz = atof(p.c_str() + c + 1); }
        GPoint target(gx, gy, gz);
        pAIShip->DestinyMgr()->GotoPoint(target);
        resultMsg = "moving to target point";
        return true;
    }

    // --- stop: Stop AI ship movement ---
    if (cmd == "stop") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — spawn first";
            return false;
        }
        pAIShip->DestinyMgr()->Stop();
        resultMsg = "stopped";
        return true;
    }

    // --- status: Report AI ship's current state (position, speed, ball mode) ---
    // See A331 §3.2 — Enables Python to poll for warp completion before issuing jump
    // params: {} (no params needed)
    if (cmd == "status") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space";
            return false;
        }
        DestinyManager* pDest = pAIShip->DestinyMgr();
        uint8 ballMode = pDest->GetState();
        const char* modeStr = "unknown";
        switch (ballMode) {
            case Destiny::Ball::Mode::STOP:    modeStr = "stopped"; break;
            case Destiny::Ball::Mode::GOTO:    modeStr = "moving"; break;
            case Destiny::Ball::Mode::FOLLOW:  modeStr = "following"; break;
            case Destiny::Ball::Mode::WARP:    modeStr = "warping"; break;
            case Destiny::Ball::Mode::ORBIT:   modeStr = "orbiting"; break;
            case Destiny::Ball::Mode::MISSILE: modeStr = "missile"; break;
            case Destiny::Ball::Mode::MUSHROOM: modeStr = "mushroom"; break;
            default: modeStr = "other"; break;
        }
        GPoint pos = pAIShip->GetPosition();
        double speed = pDest->GetVelocity().length();
        char buf[300];
        snprintf(buf, sizeof(buf),
            "mode=%s x=%.0f y=%.0f z=%.0f speed=%.1f",
            modeStr, pos.x, pos.y, pos.z, speed);
        resultMsg = buf;
        return true;
    }

    // --- set_speed: Set AI ship speed fraction (0.0 to 1.0) ---
    // See A331 §3.2 (travel skillset)
    // params: {"fraction": 0.5}
    if (cmd == "set_speed") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }
        std::string p(params);
        double fraction = 1.0;
        size_t fPos = p.find("\"fraction\"");
        if (fPos != std::string::npos) { size_t c = p.find(":", fPos); if (c != std::string::npos) fraction = atof(p.c_str() + c + 1); }
        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;
        pAIShip->DestinyMgr()->SetSpeedFraction(fraction, true);
        char buf[64];
        snprintf(buf, sizeof(buf), "speed set to %.0f%%", fraction * 100.0);
        resultMsg = buf;
        return true;
    }

    // --- align_to: Align AI ship towards a target entity ---
    // See A331 §3.2 (travel skillset)
    // params: {"targetID": 12345}
    if (cmd == "align_to") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }
        std::string p(params);
        uint32 targetID = 0;
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        if (targetID == 0) {
            resultMsg = "missing 'targetID' in params";
            return false;
        }
        SystemEntity* pTarget = pAIShip->SystemMgr()->GetSE(targetID);
        if (pTarget == nullptr) {
            resultMsg = "target entity not found in system";
            return false;
        }
        pAIShip->DestinyMgr()->AlignTo(pTarget);
        char buf[100];
        snprintf(buf, sizeof(buf), "aligning to %s (ID %u)", pTarget->GetName(), targetID);
        resultMsg = buf;
        return true;
    }

    // --- orbit: Orbit a target entity ---
    // params: {"targetID": 12345, "range": 5000.0}
    if (cmd == "orbit") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — spawn first";
            return false;
        }
        std::string p(params);
        uint32 targetID = 0;
        double range = 5000.0;
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        size_t rPos = p.find("\"range\"");
        if (rPos != std::string::npos) { size_t c = p.find(":", rPos); if (c != std::string::npos) range = atof(p.c_str() + c + 1); }
        if (targetID == 0) {
            resultMsg = "missing 'targetID' in params";
            return false;
        }
        SystemEntity* pTarget = pAIShip->SystemMgr()->GetSE(targetID);
        if (pTarget == nullptr) {
            resultMsg = "target entity not found in system";
            return false;
        }
        pAIShip->DestinyMgr()->Orbit(pTarget, range);
        char buf[64];
        snprintf(buf, sizeof(buf), "orbiting target %u at range %.0f", targetID, range);
        resultMsg = buf;
        return true;
    }

    // --- loot_item: selective wreck looting (EVE-real) ---
    // VEV_LOOT_WRECK (2026-06-12): move one stack (itemID) or everything
    // (all:true) from a wreck into the ship's cargo. Gates: pilot in space,
    // wreck present in this system, within 2500 m (EVE's loot range). Items
    // move via InventoryItem::Move — in-memory containers + DB stay coherent.
    // params: {wreckID, itemID?} or {wreckID, all:true}
    if (cmd == "loot_item") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — spawn first";
            return false;
        }
        std::string p(params);
        uint32 wreckID = 0, itemID = 0;
        bool lootAll = (p.find("\"all\"") != std::string::npos && p.find("true") != std::string::npos);
        size_t wPos = p.find("\"wreckID\"");
        if (wPos != std::string::npos) { size_t c = p.find(":", wPos); if (c != std::string::npos) wreckID = (uint32)atol(p.c_str() + c + 1); }
        size_t iPos = p.find("\"itemID\"");
        if (iPos != std::string::npos) { size_t c = p.find(":", iPos); if (c != std::string::npos) itemID = (uint32)atol(p.c_str() + c + 1); }
        if (wreckID == 0 || (itemID == 0 && !lootAll)) {
            resultMsg = "missing 'wreckID' (+ 'itemID' or all:true) in params";
            return false;
        }
        SystemEntity* pWreck = pAIShip->SystemMgr()->GetSE(wreckID);
        if (pWreck == nullptr) {
            resultMsg = "wreck not found in system";
            return false;
        }
        GPoint myPos = pAIShip->GetPosition();
        GPoint wkPos = pWreck->GetPosition();
        if (myPos.distance(wkPos) > 2500.0) {
            resultMsg = "out of loot range (2500 m) — approach the wreck";
            return false;
        }
        uint32 shipID = pAIShip->GetID();
        // Stacks to move: one itemID, or every entity row inside the wreck.
        std::vector<uint32> stacks;
        if (lootAll) {
            DBQueryResult lres;
            DBResultRow lrow;
            if (sDatabase.RunQuery(lres,
                "SELECT itemID FROM entity WHERE locationID = %u", wreckID))
                while (lres.GetRow(lrow)) stacks.push_back(lrow.GetUInt(0));
        } else {
            stacks.push_back(itemID);
        }
        int moved = 0;
        for (size_t si = 0; si < stacks.size(); ++si) {
            InventoryItemRef iRef = sItemFactory.GetItemRef(stacks[si]);
            if (iRef.get() == nullptr) continue;
            if (iRef->locationID() != wreckID) continue;   // stale/foreign row
            iRef->Move(shipID, flagCargoHold, true);
            iRef->SaveItem();   // Move() is in-memory; SaveItem persists (move_item idiom)
            moved++;
        }
        char lbuf[96];
        snprintf(lbuf, sizeof(lbuf), "looted %d stack%s from wreck %u", moved, moved == 1 ? "" : "s", wreckID);
        resultMsg = lbuf;
        return moved > 0;
    }

    // --- follow: continuous pursuit (EVE's Approach) ---
    // VEV_NAV_FOLLOW (2026-06-12): DestinyManager::Follow is the engine's own
    // "follow or approach object in space" — it keeps correcting course while
    // the target moves, unlike the one-shot align_to the 2D Approach used
    // (which blew straight past wrecks/rats). params: {targetID, range?=50}.
    if (cmd == "follow") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — spawn first";
            return false;
        }
        std::string p(params);
        uint32 targetID = 0;
        double range = 50.0;
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        size_t rPos = p.find("\"range\"");
        if (rPos != std::string::npos) { size_t c = p.find(":", rPos); if (c != std::string::npos) range = atof(p.c_str() + c + 1); }
        if (targetID == 0) {
            resultMsg = "missing 'targetID' in params";
            return false;
        }
        SystemEntity* pTarget = pAIShip->SystemMgr()->GetSE(targetID);
        if (pTarget == nullptr) {
            resultMsg = "target entity not found in system";
            return false;
        }
        // VEV_NAV_FOLLOW_GOTO (2026-06-15): DestinyManager::Follow/Orbit set the
        // destiny STATE on an AIShipSE phantom but never PROPEL it -- live-proven:
        // 72 follow cmds issued, ship stayed speed=0 at 11km while rats shot it.
        // GotoPoint DOES propel the phantom (speed 0->174 m/s observed). So
        // \"follow\" now == approach via GotoPoint toward the target CURRENT pos,
        // stopping range short. The combat FSM re-issues follow each tick while
        // beyond weapons range, so this chases a moving rat (re-aims every call)
        // and converges to brawl range, where the FSM stop()s for tracking.
        GPoint myPos = pAIShip->GetPosition();
        GPoint tgtPos = pTarget->GetPosition();
        GVector toTarget(myPos, tgtPos);     // heading from us toward the target
        double d = toTarget.length();
        GPoint dest = tgtPos;
        if (range > 0.0 && d > range) {
            toTarget.normalize();
            dest = myPos + (toTarget * (d - range));   // stop range short of it
        }
        pAIShip->DestinyMgr()->GotoPoint(dest);
        char buf[80];
        snprintf(buf, sizeof(buf), "approaching target %u to %.0f m (was %.0f m)", targetID, range, d);
        resultMsg = buf;
        return true;
    }

    // --- undock: Real undock for AI phantom characters ---
    // See A321 §11.1 — Mirrors Client::UndockFromStation() code path:
    //   1. Remove from station guests + OnCharNoLongerInStation
    //   2. Load character's actual ship from ItemFactory
    //   3. Move ship from station hangar to solar system
    //   4. Create AIShipSE at station undock point
    //   5. Set undock velocity (heading away from station)
    //   6. Update character location (stationID = 0)
    // params: {} (uses character's current shipID and stationID)
    if (cmd == "undock") {
        vevRecalcPassives(charID);   // VEV_SIM_PASSIVES: apply fitted passives before the spawn reads attrs
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "character is not a phantom player — must login_docked first";
            return false;
        }
        if (HasAIShip(charID)) {
            // Idempotent: already in space = success, not an error. Stops the 2D
            // client from getting stuck "docked" when the server already has a
            // ball (the limbo that blocked re-undock).
            // VEV_UNDOCK_CLEAR_STATIONID (2026-06-16): an in-space phantom MUST read
            // stationID=0 in the DB, or GridStreamer's phantom-filter (stationID!=0
            // -> skip) hides its ship the whole time it is in space (the curator's
            // "hacking succeeds but no Pathfinder on grid" bug). The login_in_space
            // heal re-stations a still-in-space pilot to its home; this idempotent
            // path is where the FSM re-undocks into, so clear the stale stationID.
            DBerror uerr;
            sDatabase.RunQuery(uerr,
                "UPDATE chrCharacters SET stationID = 0 WHERE characterID = %u", charID);
            resultMsg = "already in space";
            return true;
        }

        // Query character data
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT c.characterName, c.stationID, c.corporationID, c.solarSystemID,"
            " c.shipID, c.securityRating, c.bounty,"
            " IFNULL(corp.allianceID,0), IFNULL(corp.warFactionID,0)"
            " FROM chrCharacters c"
            " LEFT JOIN crpCorporation corp ON corp.corporationID = c.corporationID"
            " WHERE c.characterID = %u", charID))
        {
            resultMsg = "DB query failed";
            return false;
        }
        DBResultRow row;
        if (!charRes.GetRow(row)) {
            resultMsg = "character not found";
            return false;
        }
        std::string charName = row.GetText(0);
        uint32 stationID = row.GetUInt(1);
        uint32 corpID = row.GetUInt(2);
        uint32 solarSystemID = row.GetUInt(3);
        uint32 shipID = row.GetUInt(4);
        float securityRating = row.GetFloat(5);
        float bounty = row.GetFloat(6);
        uint32 allianceID = row.GetUInt(7);
        uint32 warFactionID = row.GetUInt(8);

        if (stationID == 0) {
            resultMsg = "character is not docked at a station";
            return false;
        }
        if (shipID == 0) {
            resultMsg = "character has no ship (shipID = 0)";
            return false;
        }

        // Get station data for undock position and orientation
        StationData stData;
        if (!stDataMgr.GetStationData(stationID, stData)) {
            resultMsg = "station data not found";
            return false;
        }

        // Boot the solar system if not already loaded
        SystemManager* pSystem = FindOrBootSystem(solarSystemID);
        if (pSystem == nullptr) {
            resultMsg = "failed to load solar system";
            return false;
        }

        // Load the character's actual ship from ItemFactory
        InventoryItemRef shipRef = sItemFactory.GetItemRef(shipID);
        if (shipRef.get() == nullptr) {
            resultMsg = "failed to load ship from ItemFactory";
            return false;
        }

        // 1. Remove from station guest list + notify
        StationItemRef sRef = GetStationByID(stationID);
        if (sRef.get() != nullptr) {
            sRef->RemovePhantomGuest(charID);

            // Send OnCharNoLongerInStation to all real clients at the station
            OnCharNoLongerInStation ocnis;
                ocnis.charID = charID;
                ocnis.corpID = corpID;
                ocnis.allianceID = allianceID;
                ocnis.factionID = warFactionID;
            PyTuple* tmp = ocnis.Encode();

            std::vector<Client*> clients;
            sRef->GetGuestList(clients);
            for (auto cur : clients) {
                PySafeIncRef(tmp);
                cur->SendNotification("OnCharNoLongerInStation", "stationid", &tmp);
            }
            PySafeDecRef(tmp);
        }

        // 2. Move ship item from station to solar system (mirrors Client::MoveToLocation)
        // Ship goes from flagHangar in station to flagNone in system
        shipRef->Move(solarSystemID, flagNone, true);
        shipRef->SetPosition(stData.dockPosition);
        shipRef->SaveItem();

        // 3. Update character location: stationID = 0 (in space)
        DBerror err;
        sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET stationID = 0, locationID = %u WHERE characterID = %u",
            solarSystemID, charID);

        // 4. Create AIShipSE at undock point with the character's real ship
        FactionData fData = FactionData();
        fData.ownerID = charID;
        fData.corporationID = corpID;
        fData.allianceID = allianceID;
        fData.factionID = warFactionID;

        AIShipSE* pAIShip = new AIShipSE(shipRef, *m_services, pSystem, fData,
                                          charID, charName.c_str(), securityRating, bounty);
        if (pAIShip == nullptr) {
            resultMsg = "failed to create AIShipSE";
            return false;
        }

        // Add to solar system (registers with bubble manager, grid, etc.)
        pSystem->AddEntity(pAIShip, false);
        pAIShip->DestinyMgr()->SetPosition(stData.dockPosition);

        // 5. Set undock velocity — fly away from station like a real undock
        // Uses dock orientation as heading direction, but FLATTENED into the x/z
        // plane: Vev is 2D (EVE's Y axis is dropped), so a station whose undock
        // vector points mostly along Y would send the ship "up/down" — invisible
        // in 2D and straight out of the grid. Project egress onto x/z + renormalize
        // so the ship drifts off the station IN-PLANE. (3D clients keep native undock.)
        GPoint egress(stData.dockOrientation.x, 0.0, stData.dockOrientation.z);
        double egressLen = sqrt(egress.x * egress.x + egress.z * egress.z);
        if (egressLen > 1.0e-6) {
            egress.x /= egressLen;
            egress.z /= egressLen;
        } else {
            egress.x = 1.0; egress.z = 0.0;  // degenerate pure-Y dock -> default +x
        }
        pAIShip->DestinyMgr()->Undock(egress);

        // Track in EntityList for future command dispatch
        AddAIShip(charID, pAIShip);

        char buf[200];
        snprintf(buf, sizeof(buf), "undocked: %s in ship %u from station %u, heading away at max speed",
                 charName.c_str(), shipID, stationID);
        resultMsg = buf;
        return true;
    }

    // --- dock_at: REAL dock procedure — approach/warp, then auto-dock in range.
    // Unlike 'dock' (requires you already be within ~10km), this GETS you there:
    // warp if >150km, else sublight-approach; AIShipSE::Process docks the moment
    // you're within 2500m of the station. params: {"stationID": 60015021}
    if (cmd == "dock_at") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space - undock first";
            return false;
        }
        std::string p(params);
        uint32 stationID = 0;
        size_t sp = p.find("\"stationID\"");
        if (sp != std::string::npos) { size_t c = p.find(":", sp); if (c != std::string::npos) stationID = (uint32)atol(p.c_str() + c + 1); }
        if (stationID == 0) { resultMsg = "missing 'stationID' in params"; return false; }
        SystemEntity* pStation = pAIShip->SystemMgr()->GetSE(stationID);
        if (pStation == nullptr) { resultMsg = "station not in current system"; return false; }
        // VEV_DOCK_DIST: DistanceTo2 returns PLAIN distance, not squared
        // (see A371 Bug22.3 note below) — the old sqrt() here shrank 5.5M km
        // to "74 km", so distant ships took the sublight branch and crawled
        // forever instead of warping to dock.
        double dist = pAIShip->DistanceTo2(pStation);
        if (dist <= 2500.0 + pStation->GetRadius()) {
            char dp[64];
            snprintf(dp, sizeof(dp), "{\"stationID\": %u}", stationID);
            return ExecuteAICommand(charID, "dock", dp, resultMsg);
        }
        pAIShip->SetPendingDock(stationID);
        if (dist > 150000.0) {
            pAIShip->DestinyMgr()->WarpTo(pStation->GetPosition(), pStation->GetRadius() + 2000.0);  // VEV_DOCKGATE_RADIUS: land just outside the hull, in dock range
            resultMsg = "warping to dock";
        } else {
            pAIShip->DestinyMgr()->GotoPoint(pStation->GetPosition());
            resultMsg = "approaching to dock";
        }
        return true;
    }

    // --- dock: Real dock for AI phantom characters ---
    // See A321 §11.1 — Mirrors Client::DockToStation() code path:
    //   1. Remove AIShipSE from space (despawn from system)
    //   2. Move ship item back to station hangar
    //   3. Update character location (stationID = station)
    //   4. Add back to station guest list + OnCharNowInStation
    // params: {"stationID": 60015021}  (optional — defaults to character's original station)
    if (cmd == "dock") {
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "character is not a phantom player";
            return false;
        }
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "no AI ship in space for this character — must undock first";
            return false;
        }

        // Parse optional stationID from params (default: nearest station or original station)
        uint32 targetStationID = 0;
        if (params != nullptr && strlen(params) > 2) {
            std::string p(params);
            size_t stPos = p.find("\"stationID\"");
            if (stPos != std::string::npos) {
                size_t colonPos = p.find(":", stPos);
                if (colonPos != std::string::npos)
                    targetStationID = (uint32)atol(p.c_str() + colonPos + 1);
            }
        }

        // If no stationID specified, query character's home base
        if (targetStationID == 0) {
            DBQueryResult charRes;
            if (sDatabase.RunQuery(charRes,
                "SELECT baseID FROM chrCharacters WHERE characterID = %u", charID))
            {
                DBResultRow row;
                if (charRes.GetRow(row))
                    targetStationID = row.GetUInt(0);
            }
        }
        if (targetStationID == 0) {
            resultMsg = "no station to dock at (specify stationID in params or set baseID)";
            return false;
        }

        // Get station data to know system + position
        StationData stData;
        if (!stDataMgr.GetStationData(targetStationID, stData)) {
            resultMsg = "station data not found for target station";
            return false;
        }

        // PROXIMITY CHECK — must be within docking range of the station
        // See A334 §2.9 — prevents teleport-dock from anywhere in system
        // Same pattern as jump gate proximity check (A334 §2.8)
        {
            SystemEntity* pStation = pAIShip->SystemMgr()->GetSE(targetStationID);
            if (pStation == nullptr) {
                resultMsg = "station is not in the current system - warp there first";
                return false;
            }
            double dist = pAIShip->DistanceTo2(pStation);
            dist = sqrt(dist);
            sLog.Cyan("   AICommandQueue", "dock proximity check: charID %u, station %u, distance %.0fm", charID, targetStationID, dist);
            // =================== VEV_DOCK_RANGE_RADIUS ===================
            // Docking range = station radius + 2500 m, MIRRORING dock_at's own
            // forward gate (EntityList.cpp dock_at branch). The old hard 10 km
            // created a dead band on big stations (Deepari II hull ~13.3 km):
            // dock_at forwarded inside radius+2.5km, dock rejected outside
            // 10 km — a phantom could NEVER dock from its natural approach
            // stop (haul FSM live-fires, takes 3-11, 2026-06-12).
            const double vevDockRange = 2500.0 + pStation->GetRadius();
            if (dist > vevDockRange) {
                char buf[256];
                snprintf(buf, sizeof(buf), "too far from station - distance: %.0fm, need < %.0fm (radius-aware). Warp to station first.", dist, vevDockRange);
            // =================== end VEV_DOCK_RANGE_RADIUS ===================
                resultMsg = buf;
                return false;
            }
        }

        // Query character data for guest list notifications
        DBQueryResult charRes;
        std::string charName;
        uint32 corpID = 0, allianceID = 0, warFactionID = 0;
        if (sDatabase.RunQuery(charRes,
            "SELECT c.characterName, c.corporationID,"
            " IFNULL(corp.allianceID,0), IFNULL(corp.warFactionID,0)"
            " FROM chrCharacters c"
            " LEFT JOIN crpCorporation corp ON corp.corporationID = c.corporationID"
            " WHERE c.characterID = %u", charID))
        {
            DBResultRow row;
            if (charRes.GetRow(row)) {
                charName = row.GetText(0);
                corpID = row.GetUInt(1);
                allianceID = row.GetUInt(2);
                warFactionID = row.GetUInt(3);
            }
        }

        // Get the ship itemID from the AIShipSE before we destroy it
        uint32 shipID = pAIShip->GetSelf()->itemID();

        // 1. Remove AIShipSE from space
        pAIShip->DestinyMgr()->Stop();
        SystemManager* pSystem = pAIShip->SystemMgr();
        if (pSystem != nullptr)
            pSystem->RemoveEntity(pAIShip);
        RemoveAIShip(charID);
        SafeDelete(pAIShip);

        // 2. Move ship back to station hangar
        InventoryItemRef shipRef = sItemFactory.GetItemRef(shipID);
        if (shipRef.get() != nullptr) {
            shipRef->Move(targetStationID, flagHangar, true);
            shipRef->SaveItem();
        }

        // 3. Update character location: back in station
        DBerror err;
        sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET stationID = %u, locationID = %u,"
            " solarSystemID = %u, constellationID = %u, regionID = %u"
            " WHERE characterID = %u",
            targetStationID, targetStationID,
            stData.systemID, stData.constellationID, stData.regionID,
            charID);

        // 4. Add back to station guest list + Local chat + notify
        StationItemRef sStRef = GetStationByID(targetStationID);
        if (sStRef.get() != nullptr) {
            sStRef->AddPhantomGuest(charID);

            // Send OnCharNowInStation to all real clients at the station
            OnCharNowInStation ocnis;
                ocnis.charID = charID;
                ocnis.corpID = corpID;
                ocnis.allianceID = allianceID;
                ocnis.warFactionID = warFactionID;
            PyTuple* tmp = ocnis.Encode();

            std::vector<Client*> clients;
            sStRef->GetGuestList(clients);
            for (auto cur : clients) {
                PySafeIncRef(tmp);
                cur->SendNotification("OnCharNowInStation", "stationid", &tmp);
            }
            PySafeDecRef(tmp);
        }

        // Re-join Local chat if at a different system
        if (m_services != nullptr) {
            LSCService* lsc = m_services->Lookup<LSCService>("LSC");
            if (lsc != nullptr) {
                lsc->CreateSystemChannel(stData.systemID);
                LSCChannel* localChan = lsc->GetChannelByID((int32)stData.systemID);
                if (localChan != nullptr && !localChan->IsJoined(charID)) {
                    localChan->JoinChannelAsPhantom(charID, charName, corpID, allianceID, warFactionID, 0);
                }
            }
        }

        char buf[200];
        snprintf(buf, sizeof(buf), "docked: %s in ship %u at station %u (%s)",
                 charName.c_str(), shipID, targetStationID, stData.name.c_str());
        resultMsg = buf;
        return true;
    }

    // --- warp_to: Warp AI ship to a celestial/entity ---
    // See A331 §3.2 (travel skillset) — Uses REAL player warp via DestinyManager::WarpTo()
    // AIShipSE is not NPCSE/DroneSE, so it goes through the full warp path:
    // alignment → warp tunnel → deceleration → bubble transfer.
    // NOTE: This command returns 'done' when warp is INITIATED, not when the ship arrives.
    // Python FSM must poll get_ship_status() to confirm arrival.
    // params: {"targetID": 40339004, "distance": 10000}
    //   targetID = belt/gate/station/entity itemID in the current system
    //   distance = stop distance in meters (default 10000 = 10km)
    // VEV_DRONE_TEST: spawn an NPC near the AI ship as a combat target (mirrors GM /spawnn,
    // via the crash-safe SystemManager::BuildDynamicEntity in-space spawn path).
    if (cmd == "spawn_npc") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space";
            return false;
        }
        std::string p(params);
        uint32 typeID = 0;
        int32 dist = 15000;
        size_t tPos = p.find("\"typeID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) typeID = (uint32)atol(p.c_str() + c + 1); }
        size_t dPos = p.find("\"distance\"");
        if (dPos != std::string::npos) { size_t c = p.find(":", dPos); if (c != std::string::npos) dist = (int32)atol(p.c_str() + c + 1); }
        if (typeID < 34) {
            resultMsg = "spawn_npc: invalid typeID";
            return false;
        }
        SystemManager* pSystem = pAIShip->SystemMgr();
        if (pSystem == nullptr) {
            resultMsg = "spawn_npc: no system";
            return false;
        }
        std::string typeName = "NPC";
        uint32 groupID = 0, categoryID = 0;
        double radius = 50.0;
        DBQueryResult tres;
        if (sDatabase.RunQuery(tres, "SELECT t.typeName, t.groupID, g.categoryID, t.radius FROM invTypes t JOIN invGroups g ON g.groupID = t.groupID WHERE t.typeID = %u", typeID)) {
            DBResultRow trow;
            if (tres.GetRow(trow)) { typeName = trow.GetText(0); groupID = trow.GetUInt(1); categoryID = trow.GetUInt(2); radius = trow.GetDouble(3); }
        }
        GPoint loc(pAIShip->GetPosition());
        loc.MakeRandomPointOnSphere((double)dist);
        ItemData idata(typeID, 1, pAIShip->GetLocationID(), flagNone, typeName.c_str(), loc);
        InventoryItemRef item = sItemFactory.SpawnItem(idata);
        if (item.get() == nullptr) {
            resultMsg = "spawn_npc: SpawnItem failed";
            return false;
        }
        DBSystemDynamicEntity ent = DBSystemDynamicEntity();
            ent.categoryID = categoryID;
            ent.groupID = groupID;
            ent.itemID = item->itemID();
            ent.itemName = typeName;
            ent.typeID = typeID;
            ent.position = loc;
            ent.allianceID = 1;
            ent.corporationID = 1;
            ent.factionID = 1;
            ent.ownerID = 1;
        if (!pSystem->BuildDynamicEntity(ent)) {
            resultMsg = "spawn_npc: BuildDynamicEntity failed";
            return false;
        }
        char buf[112];
        snprintf(buf, sizeof(buf), "spawned %s (typeID %u, item %u) at %dm", typeName.c_str(), typeID, item->itemID(), dist);
        resultMsg = buf;
        sLog.Cyan("spawn_npc", "char=%u %s", charID, buf);
        return true;
    }

    // VEV_DRONE: launch all drones currently in the drone bay (flag 87).
    if (cmd == "launch_drones") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }
        FactionData data = FactionData();
            data.ownerID = pAIShip->GetCharID();
            data.corporationID = pAIShip->GetCorporationID();
            data.allianceID = pAIShip->GetAllianceID();
            data.factionID = pAIShip->GetWarFactionID();
        uint8 n = pAIShip->LaunchAllDrones(data);
        if (n == 0) {
            resultMsg = "no drones launched (empty bay or no bandwidth)";
            return false;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "launched %u drone(s)", (uint32)n);
        resultMsg = buf;
        sLog.Cyan("launch_drones", "char=%u launched %u drone(s)", charID, (uint32)n);
        return true;
    }

    // VEV_DRONE: order all launched drones to engage a target on grid.
    if (cmd == "drone_engage") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space";
            return false;
        }
        std::string p(params);
        uint32 targetID = 0;
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        if (targetID == 0) {
            resultMsg = "missing 'targetID' in params";
            return false;
        }
        SystemManager* pSystem = pAIShip->SystemMgr();
        SystemEntity* pTarget = (pSystem != nullptr) ? pSystem->GetSE(targetID) : nullptr;
        if (pTarget == nullptr) {
            resultMsg = "target not found on grid";
            return false;
        }
        pAIShip->EngageDrones(pTarget);
        resultMsg = "drones engaging";
        sLog.Cyan("drone_engage", "char=%u drones -> target %u", charID, targetID);
        return true;
    }

    // VEV_DRONE: recall all launched drones to the bay.
    if (cmd == "drone_return") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space";
            return false;
        }
        pAIShip->ReturnAllDrones();
        resultMsg = "drones returning to bay";
        sLog.Cyan("drone_return", "char=%u drones recalled", charID);
        return true;
    }

    if (cmd == "warp_to") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }
        std::string p(params);
        uint32 targetID = 0;
        int32 distance = 10000;  // default 10km
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        size_t dPos = p.find("\"distance\"");
        if (dPos != std::string::npos) { size_t c = p.find(":", dPos); if (c != std::string::npos) distance = (int32)atol(p.c_str() + c + 1); }
        if (targetID == 0) {
            resultMsg = "missing 'targetID' in params";
            return false;
        }

        SystemManager* pSystem = pAIShip->SystemMgr();
        if (pSystem == nullptr) {
            resultMsg = "AI ship has no system";
            return false;
        }

        // Try to find the target as an existing SystemEntity (player ships, NPCs, asteroids, etc.)
        SystemEntity* pTarget = pSystem->GetSE(targetID);
        GPoint warpPoint;
        std::string targetName = "unknown";

        if (pTarget != nullptr) {
            warpPoint = pTarget->GetPosition();
            targetName = pTarget->GetName();
        } else {
            // Target might be a celestial from mapDenormalize (belt, gate, station)
            // that hasn't been loaded as a SystemEntity yet
            DBQueryResult celestialRes;
            if (sDatabase.RunQuery(celestialRes,
                "SELECT itemName, x, y, z, solarSystemID FROM mapDenormalize WHERE itemID = %u", targetID))
            {
                DBResultRow row;
                if (celestialRes.GetRow(row)) {
                    // VEV_WARP_SYSGUARD (conv1) — reject a target that is NOT in the pilot's current
                    // system. Without this, a wrong/cross-system targetID resolves to its absolute xyz
                    // and the ship attempts a galaxy-spanning warp (the "~500 AU never arrives" bug).
                    uint32 targetSysID = row.GetUInt(4);
                    if (targetSysID != pSystem->GetID()) {
                        char buf[200];
                        snprintf(buf, sizeof(buf), "target %u is in system %u, not your current system %u — warp aborted (use a gate to change systems)",
                                 targetID, targetSysID, pSystem->GetID());
                        resultMsg = buf;
                        return false;
                    }
                    warpPoint.x = row.GetDouble(1);
                    warpPoint.y = row.GetDouble(2);
                    warpPoint.z = row.GetDouble(3);
                    targetName = row.GetText(0);
                } else {
                    resultMsg = "target not found in system or mapDenormalize";
                    return false;
                }
            } else {
                resultMsg = "target not found in system or mapDenormalize";
                return false;
            }
        }

        // VEV_WARP_IN: for a planet, move warpPoint to the SAME deterministic
        // warp-in beacon the native client uses (MIRRORS BeyonceService.cpp
        // CmdWarpToStuff planet branch — keep in sync) so the 2D ship co-locates
        // with the 3D client. srandom(itemID) → identical beacon for every pilot.
        if (pTarget != nullptr && pTarget->IsPlanetSE()) {
            const double pr = pTarget->GetRadius();
            srandom(targetID);
            int rando = random();
            // INTEGER division INTENTIONAL — mirrors BeyonceService's quirk: rando/RAND_MAX
            // (both int) truncates to 0, so j is ~-1/3 for every planet. Do NOT cast RAND_MAX
            // to double; float division here lands the 2D ship ~1-2Mm off the 3D client's beacon.
            double jj = (((double)(rando / RAND_MAX) - 1.0) / 3.0);
            double ss = 20.0 * std::pow(0.025 * (10.0 * std::log10(pr / 1000000.0) - 39.0), 20.0) + 0.5;
            if (ss < 0.5)  ss = 0.5;
            if (ss > 10.5) ss = 10.5;
            const double denom = std::sqrt(warpPoint.x * warpPoint.x + warpPoint.z * warpPoint.z);
            const double sign  = (warpPoint.x != 0.0) ? (warpPoint.x / std::fabs(warpPoint.x)) : 1.0;
            double tt = std::asin(denom > 0.0 ? (sign * (warpPoint.z / denom)) : 0.0) + jj;
            const double dd = pr * (ss + 1.0) + 1000000.0;
            warpPoint.x += dd * std::sin(tt);
            warpPoint.y += 0.5 * pr * std::sin(jj);
            warpPoint.z -= dd * std::cos(tt);
        }

        // Minimum warp distance check (~150km) — don't warp to things that are close
        double currentDist = pAIShip->GetPosition().distance(warpPoint);
        if (currentDist < 150000.0) {
            char buf[200];
            snprintf(buf, sizeof(buf), "target %s is only %.0fm away — too close to warp (min 150km)",
                     targetName.c_str(), currentDist);
            resultMsg = buf;
            return false;
        }

        pAIShip->DestinyMgr()->WarpTo(warpPoint, distance);

        char buf[200];
        snprintf(buf, sizeof(buf), "warping to %s (ID %u) at %.0f AU, stop distance %dm",
                 targetName.c_str(), targetID, currentDist / 1.496e11, distance);
        resultMsg = buf;
        return true;
    }

    // --- jump: Jump through stargate to adjacent system ---
    // See A331 §3.2 (travel skillset)
    // Mirrors Client::StargateJump + ExecuteJump + MoveToLocation for headless AI ships.
    // Ship MUST be near the gate (within 2500m) — no teleporting through mid-warp.
    // params: {"gateID": 50014124}
    //   gateID = itemID of the stargate the AI ship is at (NOT the destination gate)
    if (cmd == "jump") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }
        std::string p(params);
        uint32 gateID = 0;
        size_t gPos = p.find("\"gateID\"");
        if (gPos != std::string::npos) { size_t c = p.find(":", gPos); if (c != std::string::npos) gateID = (uint32)atol(p.c_str() + c + 1); }
        if (gateID == 0) {
            resultMsg = "missing 'gateID' in params";
            return false;
        }

        // Gate proximity check — ship must be near the gate to jump (no mid-warp teleporting)
        SystemEntity* pGateSE = pAIShip->SystemMgr()->GetSE(gateID);
        if (pGateSE == nullptr) {
            resultMsg = "stargate entity not found in current system";
            return false;
        }
        double distToGate = pAIShip->DistanceTo2(pGateSE);
        if (distToGate > 2500.0) {
            char buf[200];
            snprintf(buf, sizeof(buf), "too far from gate to jump (%.0fm away, need < 2500m) — warp to gate first and wait for arrival",
                     distToGate);
            resultMsg = buf;
            return false;
        }

        // Resolve destination gate via mapJumps
        DBQueryResult jumpRes;
        if (!sDatabase.RunQuery(jumpRes,
            "SELECT celestialID FROM mapJumps WHERE stargateID = %u", gateID))
        {
            resultMsg = "DB query failed for mapJumps";
            return false;
        }
        DBResultRow jumpRow;
        if (!jumpRes.GetRow(jumpRow)) {
            resultMsg = "no jump destination found for gate";
            return false;
        }
        uint32 destGateID = jumpRow.GetUInt(0);

        // Get destination gate static data (position, radius, systemID)
        StaticData toData = StaticData();
        if (!sDataMgr.GetStaticInfo(destGateID, toData)) {
            resultMsg = "failed to get static data for destination gate";
            return false;
        }

        // Calculate landing point on sphere around dest gate (6.5–9.5km from gate center)
        GPoint landingPoint = toData.position;
        landingPoint.MakeRandomPointOnSphereLayer(toData.radius + 6500, toData.radius + 9500);

        // Send jump-out visual effects to observers in current system
        pAIShip->DestinyMgr()->SendJumpOut(gateID);
        pAIShip->DestinyMgr()->SendGateActivity(gateID);

        // Clear targets before jumping
        if (pAIShip->TargetMgr() != nullptr) {
            pAIShip->TargetMgr()->ClearAllTargets(false);
        }

        // Halt movement
        pAIShip->DestinyMgr()->Halt();

        // Save references we need before destroying old entity
        uint32 shipID = pAIShip->GetSelf()->itemID();
        SystemManager* pOldSystem = pAIShip->SystemMgr();
        uint32 oldSystemID = pOldSystem->GetID();
        uint32 destSystemID = toData.systemID;

        // Query character data for re-creation in new system
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT c.characterName, c.corporationID, c.securityRating, c.bounty,"
            " IFNULL(corp.allianceID,0), IFNULL(corp.warFactionID,0)"
            " FROM chrCharacters c"
            " LEFT JOIN crpCorporation corp ON corp.corporationID = c.corporationID"
            " WHERE c.characterID = %u", charID))
        {
            resultMsg = "DB query failed for character data";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        std::string charName = charRow.GetText(0);
        uint32 corpID = charRow.GetUInt(1);
        float securityRating = charRow.GetFloat(2);
        float bounty = charRow.GetFloat(3);
        uint32 allianceID = charRow.GetUInt(4);
        uint32 warFactionID = charRow.GetUInt(5);

        // 1. Remove old AIShipSE from current system
        pOldSystem->RemoveEntity(pAIShip);
        RemoveAIShip(charID);
        SafeDelete(pAIShip);
        pAIShip = nullptr;

        // 2. Move ship item to destination system
        InventoryItemRef shipRef = sItemFactory.GetItemRef(shipID);
        if (shipRef.get() == nullptr) {
            resultMsg = "failed to load ship from ItemFactory after system removal";
            return false;
        }
        shipRef->Move(destSystemID, flagNone, true);
        shipRef->SetPosition(landingPoint);
        shipRef->SaveItem();

        // 3. Boot destination system
        SystemManager* pDestSystem = FindOrBootSystem(destSystemID);
        if (pDestSystem == nullptr) {
            resultMsg = "failed to boot destination system";
            return false;
        }

        // 4. Create new AIShipSE in destination system
        FactionData fData = FactionData();
        fData.ownerID = charID;
        fData.corporationID = corpID;
        fData.allianceID = allianceID;
        fData.factionID = warFactionID;

        AIShipSE* pNewAIShip = new AIShipSE(shipRef, *m_services, pDestSystem, fData,
                                              charID, charName.c_str(), securityRating, bounty);
        if (pNewAIShip == nullptr) {
            resultMsg = "failed to create AIShipSE in destination system";
            return false;
        }

        // 5. Add to destination system
        pDestSystem->AddEntity(pNewAIShip, false);
        pNewAIShip->DestinyMgr()->SetPosition(landingPoint);

        // Track in EntityList
        AddAIShip(charID, pNewAIShip);

        // 6. Update character DB location
        DBerror err;
        sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET solarSystemID = %u, constellationID = %u,"
            " regionID = %u, locationID = %u"
            " WHERE characterID = %u",
            toData.systemID, toData.constellationID, toData.regionID,
            toData.systemID, charID);

        // 7. Update Local chat — leave old system, join new system
        if (m_services != nullptr) {
            LSCService* lsc = m_services->Lookup<LSCService>("LSC");
            if (lsc != nullptr) {
                // Leave old system Local
                LSCChannel* oldLocal = lsc->GetChannelByID((int32)oldSystemID);
                if (oldLocal != nullptr)
                    oldLocal->LeaveChannelAsPhantom(charID, charName, corpID, allianceID, warFactionID, 0);
                // Join new system Local
                lsc->CreateSystemChannel(destSystemID);
                LSCChannel* newLocal = lsc->GetChannelByID((int32)destSystemID);
                if (newLocal != nullptr && !newLocal->IsJoined(charID))
                    newLocal->JoinChannelAsPhantom(charID, charName, corpID, allianceID, warFactionID, 0);
            }
        }

        // Add jump to map dynamic data (for StarMap F10 display)
        MapDB::AddJump(oldSystemID);
        MapDB::AddJump(destSystemID);

        char buf[200];
        snprintf(buf, sizeof(buf), "jumped from system %u through gate %u to system %u (gate %u)",
                 oldSystemID, gateID, destSystemID, destGateID);
        resultMsg = buf;
        sLog.Cyan("AICmd:jump", "%s (charID %u): %s", charName.c_str(), charID, buf);
        return true;
    }

    // --- lock_target: Lock a target entity (asteroid, NPC, etc.) ---
    // See A331 §3.3/§3.4 — Uses NPC targeting overload (no Client* needed)
    // params: {"targetID": 12345}
    if (cmd == "lock_target") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }
        std::string p(params);
        uint32 targetID = 0;
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        if (targetID == 0) {
            resultMsg = "missing 'targetID' in params";
            return false;
        }
        SystemEntity* pTarget = pAIShip->SystemMgr()->GetSE(targetID);
        if (pTarget == nullptr) {
            resultMsg = "target entity not found in system";
            return false;
        }
        // VEV_GUARD_LOCK_TARGET_NONLOCKABLE (conv3, 2026-05-31): stations/gates/
        // planets/containers have TargetMgr=nullptr (SystemEntity.h:294); the NPC
        // overload at TargetManager.cpp:226 derefs tSE->TargetMgr() -> SIGSEGV.
        // Reproed live by Zoe-Combat q60835 (lock station 60015021). Fail-fast.
        if (pTarget->TargetMgr() == nullptr) {
            resultMsg = "target is not lockable (station/gate/planet/container)";
            return false;
        }

        // Use NPC targeting overload — no Client* required
        // lockTime in ms, maxTargets, maxRange
        float lockTime = 3000.0f;  // 3 seconds default
        uint8 maxTargets = 6;
        double maxRange = 50000.0;  // 50km default
        // Try to get ship attributes for more accurate values
        InventoryItemRef shipRef = pAIShip->GetSelf();
        if (shipRef.get() != nullptr) {
            if (shipRef->HasAttribute(AttrMaxTargetRange))
                maxRange = shipRef->GetAttribute(AttrMaxTargetRange).get_double();
            if (shipRef->HasAttribute(AttrMaxLockedTargets))
                maxTargets = (uint8)shipRef->GetAttribute(AttrMaxLockedTargets).get_uint32();
        }

        // Debug: log the actual distance and range being checked
        double dbgDist = pAIShip->GetPosition().distance(pTarget->GetPosition());
        sLog.Cyan("lock_target", "char=%u target=%u(%s) dist=%.1fm maxRange=%.1fm maxTargets=%u",
            charID, targetID, pTarget->GetName(), dbgDist, maxRange, maxTargets);

        bool chase = false;
        bool locked = pAIShip->TargetMgr()->StartTargeting(pTarget, lockTime, maxTargets, maxRange, chase);
        if (!locked) {
            sLog.Warning("lock_target", "FAILED: chase=%s dist=%.1fm maxRange=%.1fm",
                chase ? "true" : "false", dbgDist, maxRange);
            if (chase) {
                resultMsg = "target out of range — need to approach first";
            } else {
                resultMsg = "failed to lock target (already locked or at max targets)";
            }
            return false;
        }

        char buf[200];
        snprintf(buf, sizeof(buf), "locking target %s (ID %u), lock time %.1fs",
                 pTarget->GetName(), targetID, lockTime / 1000.0f);
        resultMsg = buf;
        return true;
    }

    // --- unlock_target: Release a target lock ---
    // params: {"targetID": 12345}
    if (cmd == "unlock_target") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space";
            return false;
        }
        std::string p(params);
        uint32 targetID = 0;
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        if (targetID == 0) {
            resultMsg = "missing 'targetID' in params";
            return false;
        }
        SystemEntity* pTarget = pAIShip->SystemMgr()->GetSE(targetID);
        if (pTarget == nullptr) {
            resultMsg = "target entity not found";
            return false;
        }
        // VEV_PHANTOM_GUARD_UNLOCK_TARGETMGR (conv9, 2026-06-01): mirror the live
        // guard shape at EntityList.cpp:1871 -- the only unguarded TargetMgr() deref
        // left in the ai_command_queue dispatch. m_targMgr is non-null for any
        // FindAIShip() hit today (m_aiShips = constructed in-space AIShipSE), so this
        // is defense-in-depth, not a live crash fix; removes the asymmetry vs :1871
        // and survives any future destructible-but-not-targetable / partial phantom.
        if (pAIShip->TargetMgr() == nullptr) {
            resultMsg = "ship has no target manager (nothing to unlock)";
            return false;
        }
        pAIShip->TargetMgr()->ClearTarget(pTarget);
        char buf[100];
        snprintf(buf, sizeof(buf), "unlocked target %s (ID %u)", pTarget->GetName(), targetID);
        resultMsg = buf;
        return true;
    }

    // --- activate_module: Activate a fitted module (mining laser, weapon, etc.) ---
    // See A331 §3.3 — AI mining bypasses ModuleManager (which requires Client*).
    // Instead, we directly extract ore from the asteroid like NPCs do damage directly.
    // For mining: reads module cycle time + mining yield, extracts ore, puts in cargo.
    // params: {"slotFlag": 27, "targetID": 12345}
    //   slotFlag = inventory flag of the module slot (27=HiSlot1, 28=HiSlot2, etc.)
    //   targetID = asteroid SystemEntity ID to mine
    if (cmd == "activate_module") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }
        std::string p(params);
        uint32 slotFlag = 0;
        uint32 targetID = 0;
        size_t sPos = p.find("\"slotFlag\"");
        if (sPos != std::string::npos) { size_t c = p.find(":", sPos); if (c != std::string::npos) slotFlag = (uint32)atol(p.c_str() + c + 1); }
        size_t tPos = p.find("\"targetID\"");
        if (tPos != std::string::npos) { size_t c = p.find(":", tPos); if (c != std::string::npos) targetID = (uint32)atol(p.c_str() + c + 1); }
        if (slotFlag == 0 || targetID == 0) {
            resultMsg = "missing 'slotFlag' and/or 'targetID' in params";
            return false;
        }

        SystemEntity* pTarget = pAIShip->SystemMgr()->GetSE(targetID);
        if (pTarget == nullptr) {
            resultMsg = "target entity not found in system";
            return false;
        }

        // Find the module item in the given slot on the ship
        uint32 shipID = pAIShip->GetSelf()->itemID();
        DBQueryResult modRes;
        if (!sDatabase.RunQuery(modRes,
            "SELECT e.itemID, e.typeID, t.typeName, t.groupID, g.categoryID"
            " FROM entity e"
            " JOIN invTypes t ON t.typeID = e.typeID"
            " JOIN invGroups g ON g.groupID = t.groupID"
            " WHERE e.locationID = %u AND e.flag = %u",
            shipID, slotFlag))
        {
            resultMsg = "failed to query module in slot";
            return false;
        }
        DBResultRow modRow;
        if (!modRes.GetRow(modRow)) {
            char buf[100];
            snprintf(buf, sizeof(buf), "no module found in slot flag %u", slotFlag);
            resultMsg = buf;
            return false;
        }
        uint32 moduleID = modRow.GetUInt(0);
        uint32 moduleTypeID = modRow.GetUInt(1);
        std::string moduleName = modRow.GetText(2);

        // ===================== VEV_SIM_CAPDEBIT (conv9 sim-wire) =====================
        // Charge capacitor for firing this module. The phantom path bypasses
        // ModuleManager/ActiveModule, so AttrCapacitorNeed was never debited (AI fired
        // for free). MIRROR of ActiveModule.cpp:555-575 -- same attributes + same
        // newCap = charge - need >= 0 gate. Phantom adaptation: no Client*, so abort =
        // resultMsg + return false (not GetPilot()->SendNotifyMsg), and the debit is a
        // direct SetAttribute on the ship item (not pilot-bound SetShipCapacitorLevel).
        // Runs BEFORE the mining/turret/EWAR dispatch so it gates every module kind.
        {
            InventoryItemRef capModRef = sItemFactory.GetItemRef(moduleID);
            if (capModRef.get() != nullptr && capModRef->HasAttribute(AttrCapacitorNeed)) {
                float capNeed = capModRef->GetAttribute(AttrCapacitorNeed).get_float();
                if (capNeed > 0.0f) {
                    InventoryItemRef capShipRef = pAIShip->GetSelf();
                    float capCharge = (capShipRef.get() != nullptr)
                        ? capShipRef->GetAttribute(AttrCapacitorCharge).get_float() : 0.0f;
                    float newCap = capCharge - capNeed;
                    if (newCap < 0.0f) {
                        char capbuf[160];
                        snprintf(capbuf, sizeof(capbuf),
                            "insufficient capacitor: module needs %.0f GJ, only %.0f GJ available",
                            capNeed, capCharge);
                        resultMsg = capbuf;
                        return false;
                    }
                    if (capShipRef.get() != nullptr)
                        capShipRef->SetAttribute(AttrCapacitorCharge, newCap);
                }
            }
        }
        // =================== end VEV_SIM_CAPDEBIT ===================

        uint32 moduleGroupID = modRow.GetUInt(3);

        // Determine if this is a mining module
        bool isMiningModule = false;
        // Strip Miner (464), Mining Laser (54), Frequency Mining Laser (483),
        // Gas Cloud Harvester (737), Mercoxit Mining Crystal (468)
        if (moduleGroupID == 464 || moduleGroupID == 54 || moduleGroupID == 483 || moduleGroupID == 737) {
            isMiningModule = true;
        }

        // VEV_BEAM_AFTER_EXTRACT: the beam is recorded AFTER a successful extraction
        // (end of the mining branch), not here — firing it pre-gate drew a harvest
        // beam for activations the range gate rejected (the 20km ghost beam).

        if (!isMiningModule) {
            // VEV_FIRE_REQUIRES_LOCK (EVE-real, combat test 2 2026-06-12): a
            // turret only fires on a target this ship has LOCKED — no more
            // pre-firing at nothing while the lock is still cycling. Gate =
            // the target's targeted-by registry (registered by StartTargeting).
            // VEV_LOCK_EXEMPTIONS (salvage batch, 2026-06-12): the lock rule
            // applies to COMBAT targets only. SELF-targeted modules (reps,
            // toggles, smartbombs, boosters — the client passes our own shipID)
            // and WRECKS/CONTAINERS (no TargetMgr — engine-unlockable; salvager
            // + tractor target them by selection, like EVE's loot range rules)
            // are exempt, else the gate bricks every non-combat module.
            if (pTarget == nullptr) {
                resultMsg = "target not found on grid";
                return false;
            }
            if (pTarget != (SystemEntity*)pAIShip && !pTarget->IsWreckSE() && !pTarget->IsContainerSE()
                && (pTarget->TargetMgr() == nullptr || !pTarget->TargetMgr()->IsTargetedBy(pAIShip))) {
                resultMsg = "target is not locked -- lock before firing";
                return false;
            }
            // --- Weapon activation: turrets/lasers (direct damage, same pattern as NPC turrets) ---
            // See A371 §9 (AI combat self-testing) — bypasses ModuleManager (requires Client*).
            // Uses the same Damage constructor + ApplyDamage() pipeline as NPCAIMgr::AttackTarget().

            // Look up loaded charge in same slot (categoryID=8 = Charge)
            DBQueryResult chargeRes;
            InventoryItemRef chargeRef;
            float emDmg = 0, kinDmg = 0, therDmg = 0, expDmg = 0;
            bool hasCharge = false;
            if (sDatabase.RunQuery(chargeRes,
                "SELECT e.itemID FROM entity e"
                " JOIN invTypes t ON t.typeID = e.typeID"
                " JOIN invGroups g ON g.groupID = t.groupID"
                " WHERE e.locationID = %u AND e.flag = %u AND g.categoryID = 8",
                shipID, slotFlag))
            {
                DBResultRow chargeRow;
                if (chargeRes.GetRow(chargeRow)) {
                    chargeRef = sItemFactory.GetItemRef(chargeRow.GetUInt(0));
                    if (chargeRef.get() != nullptr) {
                        hasCharge = true;
                        emDmg = chargeRef->GetAttribute(AttrEmDamage).get_float();
                        therDmg = chargeRef->GetAttribute(AttrThermalDamage).get_float();
                        kinDmg = chargeRef->GetAttribute(AttrKineticDamage).get_float();
                        expDmg = chargeRef->GetAttribute(AttrExplosiveDamage).get_float();
                    }
                }
            }
            if (!hasCharge) {
                // Civilian weapons have damage on the weapon itself
                InventoryItemRef weapRef = sItemFactory.GetItemRef(moduleID);
                if (weapRef.get() != nullptr) {
                    emDmg = weapRef->GetAttribute(AttrEmDamage).get_float();
                    therDmg = weapRef->GetAttribute(AttrThermalDamage).get_float();
                    kinDmg = weapRef->GetAttribute(AttrKineticDamage).get_float();
                    expDmg = weapRef->GetAttribute(AttrExplosiveDamage).get_float();
                }
            }
            if (emDmg + therDmg + kinDmg + expDmg <= 0) {
                // ===================== VEV_SIM_EWAR (conv9 sim-wire) =====================
                // No damage attrs -> this may be EWAR (scram/web/jam), not a broken turret.
                // Today these fall through to "cannot fire" and return false -> EWAR is INERT
                // for AI ships. Branch on the module groupID and call the already
                // phantom-safe target-side effects. groupID numerics from invGroups.h:
                //   Warp_Scrambler=52, Stasis_Web=65, ECM=201.
                // MIRRORS the real engine application: web = ActiveModule.cpp:854-857
                // (WebbedMe(m_modRef, true)); scram = ActiveModule.cpp:865-867
                // (GetSelf()->SetAttribute(AttrWarpScrambleStatus)).
                if (moduleGroupID == 65) {
                    // Stasis Web: slow the target. WebbedMe is phantom-safe (operates on
                    // the target's DestinyManager / mySE / m_maxShipSpeed -- no Client*).
                    InventoryItemRef webModRef = sItemFactory.GetItemRef(moduleID);
                    if (pTarget->DestinyMgr() == nullptr) {
                        resultMsg = "target has no destiny manager -- cannot web";
                        return false;
                    }
                    if (webModRef.get() == nullptr) {
                        resultMsg = "web module item not found";
                        return false;
                    }
                    pTarget->DestinyMgr()->WebbedMe(webModRef, true);
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        "effects.StasisWeb", 1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    sLog.Cyan("activate_module", "EWAR/WEB char=%u %s -> %s(%u)",
                        charID, moduleName.c_str(), pTarget->GetName(), targetID);
                    char ewbuf[200];
                    snprintf(ewbuf, sizeof(ewbuf), "stasis web applied to %s (slowed)", pTarget->GetName());
                    resultMsg = ewbuf;
                    return true;
                }
                if (moduleGroupID == 52) {
                    // Warp Scrambler: set AttrWarpScrambleStatus on the target's self-item.
                    // This is the exact setter the engine uses (ActiveModule.cpp:865-867);
                    // the target-side warp guard reads this attribute. SetAttribute is
                    // phantom-safe (item attr write, no Client*).
                    InventoryItemRef tgtSelf = pTarget->GetSelf();
                    if (tgtSelf.get() == nullptr) {
                        resultMsg = "target has no item -- cannot scramble";
                        return false;
                    }
                    tgtSelf->SetAttribute(AttrWarpScrambleStatus, 1);
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        "effects.WarpScramble", 1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    sLog.Cyan("activate_module", "EWAR/SCRAM char=%u %s -> %s(%u)",
                        charID, moduleName.c_str(), pTarget->GetName(), targetID);
                    char esbuf[200];
                    snprintf(esbuf, sizeof(esbuf), "warp scramble applied to %s (warp-locked)", pTarget->GetName());
                    resultMsg = esbuf;
                    return true;
                }
                if (moduleGroupID == 201) {
                    // VEV_SIM_ECM: real jam-roll — EVE: success chance = jammer
                    // strength / target sensor strength (matched by sensor type,
                    // strongest jammer axis wins). Success breaks every lock the
                    // target holds (ClearAllTargets — the same phantom-safe call
                    // warp entry uses); NPCs re-acquire on their next AI tick.
                    InventoryItemRef jamRef = sItemFactory.GetItemRef(moduleID);
                    InventoryItemRef jamTgt = pTarget->GetSelf();
                    float jamStr = 0.0f, sensorStr = 1.0f;
                    if (jamRef.get() != nullptr && jamTgt.get() != nullptr) {
                        static const uint16 kJamMap[4][2] = {
                            { AttrScanGravimetricStrengthBonus,   AttrScanGravimetricStrength },
                            { AttrScanLadarStrengthBonus,         AttrScanLadarStrength },
                            { AttrScanMagnetometricStrengthBonus, AttrScanMagnetometricStrength },
                            { AttrScanRadarStrengthBonus,         AttrScanRadarStrength },
                        };
                        for (int ji = 0; ji < 4; ++ji) {
                            const float js = jamRef->HasAttribute(kJamMap[ji][0]) ? jamRef->GetAttribute(kJamMap[ji][0]).get_float() : 0.0f;
                            const float ss = jamTgt->HasAttribute(kJamMap[ji][1]) ? jamTgt->GetAttribute(kJamMap[ji][1]).get_float() : 0.0f;
                            if (js > jamStr) { jamStr = js; sensorStr = (ss > 0.1f) ? ss : 1.0f; }
                        }
                    }
                    float jamChance = (sensorStr > 0.0f) ? (jamStr / sensorStr) : 0.0f;
                    if (jamChance > 1.0f) jamChance = 1.0f;
                    const bool jammed = (MakeRandomFloat(0.0f, 1.0f) < jamChance);
                    if (jammed && pTarget->TargetMgr() != nullptr)
                        pTarget->TargetMgr()->ClearAllTargets();
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        "effects.ECM", 1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    char jamBuf[200];
                    snprintf(jamBuf, sizeof(jamBuf), "ECM vs %s: %.0f%% -> %s", pTarget->GetName(),
                             jamChance * 100.0f, jammed ? "JAMMED (locks broken)" : "jam failed");
                    sLog.Cyan("activate_module", "EWAR/ECM char=%u %s", charID, jamBuf);
                    resultMsg = jamBuf;
                    return true;
                }
                // =================== VEV_SIM_REMOTEREP ===================
                // Remote reps (logi): Armor Repair Projector 325 / Shield
                // Transporter 41 / Remote Hull 585 / Energy Transfer Array 67 —
                // the selfrep math applied to a LOCKED friendly target. Range
                // gate = module optimal (54). Cap was debited by CAPDEBIT.
                if (moduleGroupID == 325 || moduleGroupID == 41 || moduleGroupID == 585 || moduleGroupID == 67) {
                    InventoryItemRef rrRef = sItemFactory.GetItemRef(moduleID);
                    InventoryItemRef rrTgt = pTarget->GetSelf();
                    if (rrRef.get() == nullptr || rrTgt.get() == nullptr) {
                        resultMsg = "remote rep: item refs not found";
                        return false;
                    }
                    const float rrOpt = rrRef->HasAttribute(AttrMaxRange) ? rrRef->GetAttribute(AttrMaxRange).get_float() : 4000.0f;
                    const double rrDist = pAIShip->GetPosition().distance(pTarget->GetPosition());
                    if (rrDist > rrOpt) {
                        char rrOor[140];
                        snprintf(rrOor, sizeof(rrOor), "out of rep range: %.0fm > %.0fm", rrDist, rrOpt);
                        resultMsg = rrOor;
                        return false;
                    }
                    char rrBuf[200];
                    if (moduleGroupID == 41) {
                        const float b = rrRef->HasAttribute(AttrShieldBonus) ? rrRef->GetAttribute(AttrShieldBonus).get_float() : 0.0f;
                        const float cap2 = rrTgt->GetAttribute(AttrShieldCapacity).get_float();
                        const float cur2 = rrTgt->GetAttribute(AttrShieldCharge).get_float();
                        const float new2 = (cur2 + b > cap2) ? cap2 : cur2 + b;
                        rrTgt->SetAttribute(AttrShieldCharge, new2);
                        snprintf(rrBuf, sizeof(rrBuf), "transferred %.0f shield to %s", new2 - cur2, pTarget->GetName());
                    } else if (moduleGroupID == 67) {
                        const float b = rrRef->HasAttribute(90) ? rrRef->GetAttribute(90).get_float() : 0.0f;
                        const float cap2 = rrTgt->GetAttribute(AttrCapacitorCapacity).get_float();
                        const float cur2 = rrTgt->GetAttribute(AttrCapacitorCharge).get_float();
                        const float new2 = (cur2 + b > cap2) ? cap2 : cur2 + b;
                        rrTgt->SetAttribute(AttrCapacitorCharge, new2);
                        snprintf(rrBuf, sizeof(rrBuf), "transferred %.0f GJ to %s", new2 - cur2, pTarget->GetName());
                    } else {
                        const float amt = rrRef->HasAttribute(AttrArmorDamageAmount) ? rrRef->GetAttribute(AttrArmorDamageAmount).get_float() : 0.0f;
                        const uint16 dAttr = (moduleGroupID == 325) ? AttrArmorDamage : AttrDamage;
                        const float dCur2 = rrTgt->HasAttribute(dAttr) ? rrTgt->GetAttribute(dAttr).get_float() : 0.0f;
                        const float dNew2 = (dCur2 - amt > 0.0f) ? dCur2 - amt : 0.0f;
                        rrTgt->SetAttribute(dAttr, dNew2);
                        snprintf(rrBuf, sizeof(rrBuf), "remote-repaired %.0f %s HP on %s", dCur2 - dNew2,
                                 (moduleGroupID == 325) ? "armor" : "hull", pTarget->GetName());
                    }
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        (moduleGroupID == 41) ? "effects.ShieldTransfer" : "effects.ArmorRepair",
                        1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    sLog.Cyan("activate_module", "REMOTEREP char=%u %s", charID, rrBuf);
                    resultMsg = rrBuf;
                    return true;
                }
                // =================== VEV_SIM_PAINTER ===================
                // Target Painter (379): FUNCTIONAL — blooms the target's
                // signature (attr 554, +25% T1) for the paint window; the
                // weapon to-hit reads GetPaintMult. Range = optimal + falloff.
                if (moduleGroupID == 379) {
                    InventoryItemRef tpRef = sItemFactory.GetItemRef(moduleID);
                    if (tpRef.get() == nullptr) { resultMsg = "painter item not found"; return false; }
                    const float tpOpt = tpRef->HasAttribute(AttrMaxRange) ? tpRef->GetAttribute(AttrMaxRange).get_float() : 25000.0f;
                    const float tpFall = tpRef->HasAttribute(AttrFalloff) ? tpRef->GetAttribute(AttrFalloff).get_float() : 0.0f;
                    const double tpDist = pAIShip->GetPosition().distance(pTarget->GetPosition());
                    if (tpDist > tpOpt + tpFall) {
                        char tpOor[140];
                        snprintf(tpOor, sizeof(tpOor), "out of painter range: %.0fm > %.0fm", tpDist, tpOpt + tpFall);
                        resultMsg = tpOor;
                        return false;
                    }
                    const float tpBonus = tpRef->HasAttribute(554) ? tpRef->GetAttribute(554).get_float() : 25.0f;
                    const int64 tpDur = tpRef->HasAttribute(AttrDuration) ? (int64)tpRef->GetAttribute(AttrDuration).get_float() : 10000;
                    vev::grid::NotePainted((uint32_t)targetID, 1.0f + tpBonus / 100.0f, tpDur + 2000);
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        "effects.TargetPaint", 1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    char tpBuf[180];
                    snprintf(tpBuf, sizeof(tpBuf), "painting %s: signature +%.0f%% for %llds",
                             pTarget->GetName(), tpBonus, (long long)(tpDur / 1000));
                    sLog.Cyan("activate_module", "PAINTER char=%u %s", charID, tpBuf);
                    resultMsg = tpBuf;
                    return true;
                }
                // =================== VEV_SIM_SALVAGE ===================
                // Salvager (1122) vs a WRECK: mirror of the human Prospector
                // module (Prospector.cpp:65-199). chance = wreck accessDifficulty
                // (901) + module accessDifficultyBonus (902); success rolls the
                // victim-faction salvage table (wreck customInfo) into our cargo
                // and CONSUMES the wreck (Salvaged()). Wreck must be looted EMPTY
                // first — same rule as the human path (loot_wreck does that).
                if (moduleGroupID == 1122) {
                    if (!pTarget->IsWreckSE()) {
                        resultMsg = "salvager needs a wreck target";
                        return false;
                    }
                    InventoryItemRef svmRef = sItemFactory.GetItemRef(moduleID);
                    InventoryItemRef wreckRef = pTarget->GetSelf();
                    if (svmRef.get() == nullptr || wreckRef.get() == nullptr) {
                        resultMsg = "salvager/wreck item not found";
                        return false;
                    }
                    const float svOpt = svmRef->HasAttribute(AttrMaxRange) ? svmRef->GetAttribute(AttrMaxRange).get_float() : 5000.0f;
                    const double svDist = pAIShip->GetPosition().distance(pTarget->GetPosition());
                    if (svDist > svOpt) {
                        char svOor[140];
                        snprintf(svOor, sizeof(svOor), "out of salvage range: %.0fm > %.0fm", svDist, svOpt);
                        resultMsg = svOor;
                        return false;
                    }
                    if (wreckRef->GetMyInventory() != nullptr && !wreckRef->GetMyInventory()->IsEmpty()) {
                        resultMsg = "wreck still holds loot — loot it first, then salvage";
                        return false;
                    }
                    const int svAccess = wreckRef->HasAttribute(AttrAccessDifficulty) ? wreckRef->GetAttribute(AttrAccessDifficulty).get_int() : 30;
                    const int svBonus = svmRef->HasAttribute(AttrAccessDifficultyBonus) ? svmRef->GetAttribute(AttrAccessDifficultyBonus).get_int() : 5;
                    int svChance = svAccess + svBonus;
                    if (svChance < 0) svChance = 0;
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        "effects.Salvaging", 1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    if (MakeRandomInt(0, 100) >= svChance) {
                        char svFail[140];
                        snprintf(svFail, sizeof(svFail), "salvage failed (%d%% chance) — cycle again", svChance);
                        sLog.Cyan("activate_module", "SALVAGE char=%u %s", charID, svFail);
                        resultMsg = svFail;
                        return true;   // a failed roll is still a completed cycle
                    }
                    // SUCCESS: roll the faction salvage table into our cargo
                    std::vector<uint32> svList;
                    sDataMgr.GetSalvage(atoi(wreckRef->customInfo().c_str()), svList);
                    uint8 svDrop = 1;
                    switch (svAccess) {
                        case 30: svDrop = 1; break;  case 20: svDrop = 2; break;
                        case 10: svDrop = 3; break;  case 0:  svDrop = 4; break;
                        case -10: svDrop = 5; break; case -20: svDrop = 6; break;
                        default: svDrop = 1; break;
                    }
                    ShipItemRef svShip = ShipItemRef::StaticCast(pAIShip->GetSelf());
                    Inventory* svInv = (svShip.get() != nullptr) ? svShip->GetMyInventory() : nullptr;
                    uint32 svMin = svDrop, svMax = svDrop * (uint32)sConfig.rates.DropSalvage;
                    if (svMax < svMin) svMax = svMin;
                    std::string svGot;
                    int svKinds = 0;
                    for (size_t svI = 0; svI < svList.size(); ++svI) {
                        if (IsEven(MakeRandomInt(0, 10)))
                            continue;
                        const uint32 svQty = (uint32)MakeRandomInt(svMin, svMax);
                        ItemData svLoot(svList[svI], charID, locTemp, flagNone, svQty);
                        InventoryItemRef svRef2 = sItemFactory.SpawnItem(svLoot);
                        if (svRef2.get() == nullptr) continue;
                        if (svInv != nullptr && svInv->HasAvailableSpace(flagCargoHold, svRef2)) {
                            svRef2->MergeTypesInCargo(svShip.get(), flagCargoHold);
                            char svRow[96];
                            snprintf(svRow, sizeof(svRow), " %ux %s;", svQty, svRef2->name());
                            svGot += svRow;
                            svKinds++;
                        } else {
                            svRef2->Delete();
                            break;   // cargo full — remaining salvage lost (EVE)
                        }
                    }
                    pTarget->GetWreckSE()->Salvaged();   // wreck consumed
                    char svBuf[300];
                    snprintf(svBuf, sizeof(svBuf), "salvage SUCCESS (%d%%): %d kinds —%s wreck consumed",
                             svChance, svKinds, svKinds ? svGot.c_str() : " nothing dropped;");
                    sLog.Cyan("activate_module", "SALVAGE char=%u %s", charID, svBuf);
                    resultMsg = svBuf;
                    return true;
                }
                // Tractor Beam (650): engage/disengage the engine's NATIVE
                // DestinyManager tractor on a wreck/container — it flies to us.
                if (moduleGroupID == 650) {
                    if (!pTarget->IsWreckSE() && !pTarget->IsContainerSE()) {
                        resultMsg = "tractor beam needs a wreck or container target";
                        return false;
                    }
                    if (pTarget->DestinyMgr() == nullptr) {
                        resultMsg = "target cannot be tractored (no destiny)";
                        return false;
                    }
                    if (pTarget->DestinyMgr()->IsTractored()) {
                        pTarget->DestinyMgr()->TractorBeamStop();
                        resultMsg = "tractor beam disengaged";
                        return true;
                    }
                    InventoryItemRef trRef = sItemFactory.GetItemRef(moduleID);
                    const float trOpt = (trRef.get() != nullptr && trRef->HasAttribute(AttrMaxRange))
                        ? trRef->GetAttribute(AttrMaxRange).get_float() : 20000.0f;
                    const double trDist = pAIShip->GetPosition().distance(pTarget->GetPosition());
                    if (trDist > trOpt) {
                        char trOor[140];
                        snprintf(trOor, sizeof(trOor), "out of tractor range: %.0fm > %.0fm", trDist, trOpt);
                        resultMsg = trOor;
                        return false;
                    }
                    EvilNumber trSpd = (trRef.get() != nullptr && trRef->HasAttribute(AttrMaxTractorVelocity))
                        ? trRef->GetAttribute(AttrMaxTractorVelocity) : EvilNumber(500);
                    pTarget->DestinyMgr()->TractorBeamStart(pAIShip, trSpd);
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        "effects.TractorBeam", 1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    char trBuf[160];
                    snprintf(trBuf, sizeof(trBuf), "tractor engaged on %s (%.0fm out, pulling)", pTarget->GetName(), trDist);
                    sLog.Cyan("activate_module", "TRACTOR char=%u %s", charID, trBuf);
                    resultMsg = trBuf;
                    return true;
                }
                // =================== end VEV_SIM_SALVAGE ===================
                // =================== VEV_SIM_UTILITY ===================
                // Capacitor Booster (76): consume ONE loaded booster charge
                // (group 87, capacitorBonus attr 67) -> instant cap, clamped.
                if (moduleGroupID == 76) {
                    InventoryItemRef cbShip = pAIShip->GetSelf();
                    uint32 cbChargeID = 0;
                    DBQueryResult cbq;
                    if (sDatabase.RunQuery(cbq,
                        "SELECT e.itemID FROM entity e"
                        " JOIN invTypes t ON t.typeID = e.typeID"
                        " JOIN invGroups g ON g.groupID = t.groupID"
                        " WHERE e.locationID = (SELECT shipID FROM chrCharacters WHERE characterID = %u)"
                        "   AND e.flag = %u AND g.groupID = 87 LIMIT 1", charID, slotFlag)) {
                        DBResultRow cbr;
                        if (cbq.GetRow(cbr)) cbChargeID = cbr.GetUInt(0);
                    }
                    if (cbChargeID == 0 || cbShip.get() == nullptr) {
                        resultMsg = "no cap booster charge loaded";
                        return false;
                    }
                    InventoryItemRef cbRef = sItemFactory.GetItemRef(cbChargeID);
                    if (cbRef.get() == nullptr) { resultMsg = "booster charge item not found"; return false; }
                    const float cbBonus = cbRef->HasAttribute(67) ? cbRef->GetAttribute(67).get_float() : 0.0f;
                    const float cbCap = cbShip->GetAttribute(AttrCapacitorCapacity).get_float();
                    const float cbCur = cbShip->GetAttribute(AttrCapacitorCharge).get_float();
                    const float cbNew = (cbCur + cbBonus > cbCap) ? cbCap : cbCur + cbBonus;
                    cbShip->SetAttribute(AttrCapacitorCharge, cbNew);
                    if (cbRef->quantity() > 1) cbRef->AlterQuantity(-1, true);
                    else cbRef->Delete();
                    char cbBuf[160];
                    snprintf(cbBuf, sizeof(cbBuf), "cap boosted +%.0f GJ (%.0f/%.0f)", cbNew - cbCur, cbNew, cbCap);
                    sLog.Cyan("activate_module", "CAPBOOST char=%u %s", charID, cbBuf);
                    resultMsg = cbBuf;
                    return true;
                }
                // Energy Vampire (68) / Energy Neutralizer (71): drain the
                // target's cap; a vampire transfers the drained energy to us.
                // Attrs: 90 powerTransferAmount (vamp), 97 energyDestabilization
                // (neut). Range gate: module optimal (54) + falloff (158).
                if (moduleGroupID == 68 || moduleGroupID == 71) {
                    InventoryItemRef nvRef = sItemFactory.GetItemRef(moduleID);
                    InventoryItemRef nvTgt = pTarget->GetSelf();
                    InventoryItemRef nvSelf = pAIShip->GetSelf();
                    if (nvRef.get() == nullptr || nvTgt.get() == nullptr || nvSelf.get() == nullptr) {
                        resultMsg = "vamp/neut: item refs not found";
                        return false;
                    }
                    const float nvOpt = nvRef->HasAttribute(AttrMaxRange) ? nvRef->GetAttribute(AttrMaxRange).get_float() : 5000.0f;
                    const float nvFall = nvRef->HasAttribute(AttrFalloff) ? nvRef->GetAttribute(AttrFalloff).get_float() : 0.0f;
                    const double nvDist = pAIShip->GetPosition().distance(pTarget->GetPosition());
                    if (nvDist > nvOpt + nvFall) {
                        char nvOor[160];
                        snprintf(nvOor, sizeof(nvOor), "out of range: %.0fm > %.0fm", nvDist, nvOpt + nvFall);
                        resultMsg = nvOor;
                        return false;
                    }
                    float amount = 0.0f;
                    if (nvRef->HasAttribute(90)) amount = nvRef->GetAttribute(90).get_float();
                    else if (nvRef->HasAttribute(97)) amount = nvRef->GetAttribute(97).get_float();
                    if (amount <= 0.0f) { resultMsg = "module has no drain amount"; return false; }
                    const float tCur = nvTgt->HasAttribute(AttrCapacitorCharge) ? nvTgt->GetAttribute(AttrCapacitorCharge).get_float() : 0.0f;
                    const float drained = (tCur < amount) ? tCur : amount;
                    nvTgt->SetAttribute(AttrCapacitorCharge, tCur - drained);
                    char nvBuf[200];
                    if (moduleGroupID == 68) {
                        const float sCap2 = nvSelf->GetAttribute(AttrCapacitorCapacity).get_float();
                        const float sCur2 = nvSelf->GetAttribute(AttrCapacitorCharge).get_float();
                        const float sNew2 = (sCur2 + drained > sCap2) ? sCap2 : sCur2 + drained;
                        nvSelf->SetAttribute(AttrCapacitorCharge, sNew2);
                        snprintf(nvBuf, sizeof(nvBuf), "vampire drained %.0f GJ from %s (we gained %.0f)",
                                 drained, pTarget->GetName(), sNew2 - sCur2);
                    } else {
                        snprintf(nvBuf, sizeof(nvBuf), "neutralized %.0f GJ of %s's capacitor", drained, pTarget->GetName());
                    }
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, targetID, 0,
                        (moduleGroupID == 68) ? "effects.EnergyVampire" : "effects.EnergyDestabilization",
                        1, 1, 1, 0, 0, 0);
                    vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);
                    sLog.Cyan("activate_module", "VAMP/NEUT char=%u %s", charID, nvBuf);
                    resultMsg = nvBuf;
                    return true;
                }
                // Survey Scanner (49): report every asteroid within scan range
                // (attr 197) — name + remaining m3. The AI pilots' prospecting
                // verb; result rides resultMsg into the command journal.
                if (moduleGroupID == 49) {
                    InventoryItemRef svRef = sItemFactory.GetItemRef(moduleID);
                    const float svRange = (svRef.get() != nullptr && svRef->HasAttribute(197))
                        ? svRef->GetAttribute(197).get_float() : 15000.0f;
                    std::map<uint32, SystemEntity*> svEnts;
                    if (pAIShip->SysBubble() != nullptr) pAIShip->SysBubble()->GetEntities(svEnts);
                    std::string svOut = "survey:";
                    int svCount = 0;
                    for (std::map<uint32, SystemEntity*>::iterator vit = svEnts.begin(); vit != svEnts.end(); ++vit) {
                        SystemEntity* pR = vit->second;
                        if (pR == nullptr || !pR->IsAsteroidSE()) continue;
                        const double svD = pAIShip->GetPosition().distance(pR->GetPosition());
                        if (svD > svRange) continue;
                        InventoryItemRef rRef = pR->GetSelf();
                        if (rRef.get() == nullptr) continue;
                        const float rQty = rRef->HasAttribute(AttrQuantity) ? rRef->GetAttribute(AttrQuantity).get_float() : 0.0f;
                        const float rVol = rRef->HasAttribute(AttrVolume) ? rRef->GetAttribute(AttrVolume).get_float() : 0.0f;
                        if (svCount < 12) {
                            char svRow[120];
                            snprintf(svRow, sizeof(svRow), " %s %.0fm3 @%.0fm;", pR->GetName(), rQty * rVol, svD);
                            svOut += svRow;
                        }
                        svCount++;
                    }
                    char svTail[64];
                    snprintf(svTail, sizeof(svTail), " (%d rocks in %.0fm)", svCount, svRange);
                    svOut += svTail;
                    sLog.Cyan("activate_module", "SURVEY char=%u %d rocks", charID, svCount);
                    resultMsg = svOut;
                    return true;
                }
                // =================== end VEV_SIM_UTILITY ===================
                // =================== VEV_SIM_TOGGLE ===================
                // Active TOGGLE modules for AI/phantom ships: one activate ENGAGES,
                // the next DISENGAGES (saved ship attrs restored). Mirrors the real
                // engine's attr application; SetAttribute is phantom-safe.
                //   Propulsion Module (46): AB/MWD — AttrMaxVelocity × (1 + sf% ×
                //     sbf/(mass+massAddition)), mass += massAddition, then
                //     DestinyManager::SpeedBoost() — the engine's own prop-mod
                //     recompute (DestinyManager.cpp:3062 re-reads both attrs).
                //   Damage Control (60) / Shield Hardener (77) / Armor Hardener
                //     (328): multiply the ship's damage resonances by the module's
                //     resonance attrs — exactly what Damage.cpp reads on ApplyDamage.
                // Engaged state = in-memory map (moduleID → saved attrs); a server
                // restart reverts to disengaged (client re-syncs on next click).
                if (moduleGroupID == 46 || moduleGroupID == 60 || moduleGroupID == 77 || moduleGroupID == 328) {
                    InventoryItemRef tglModRef = sItemFactory.GetItemRef(moduleID);
                    InventoryItemRef tglShipRef = pAIShip->GetSelf();
                    if (tglModRef.get() == nullptr || tglShipRef.get() == nullptr) {
                        resultMsg = "toggle: module/ship item not found";
                        return false;
                    }
                    auto itT = s_vevToggleReg.find(moduleID);
                    if (itT != s_vevToggleReg.end()) {
                        // DISENGAGE: restore + persist every saved ship attr
                        vevToggleRestore(itT->second, "player toggle");
                        s_vevToggleReg.erase(itT);
                        if (moduleGroupID == 46 && pAIShip->DestinyMgr() != nullptr)
                            pAIShip->DestinyMgr()->SpeedBoost(true);
                        char tobuf[160];
                        snprintf(tobuf, sizeof(tobuf), "%s disengaged", moduleName.c_str());
                        sLog.Cyan("activate_module", "TOGGLE-OFF char=%u %s", charID, moduleName.c_str());
                        resultMsg = tobuf;
                        return true;
                    }
                    std::vector<std::pair<uint16, float>> savedAttrs;
                    if (moduleGroupID == 46) {
                        // EVE AB/MWD math: speedFactor (20, %) × speedBoostFactor
                        // (567, thrust) / (ship mass + massAddition 796).
                        const float sf   = tglModRef->HasAttribute(AttrSpeedFactor) ? tglModRef->GetAttribute(AttrSpeedFactor).get_float() : 0.0f;
                        const float sbf  = tglModRef->HasAttribute(AttrSpeedBoostFactor) ? tglModRef->GetAttribute(AttrSpeedBoostFactor).get_float() : 0.0f;
                        const float mAdd = tglModRef->HasAttribute(AttrMassAddition) ? tglModRef->GetAttribute(AttrMassAddition).get_float() : 0.0f;
                        const float mass = tglShipRef->GetAttribute(AttrMass).get_float();
                        const float vMax = tglShipRef->GetAttribute(AttrMaxVelocity).get_float();
                        if (sf <= 0.0f || sbf <= 0.0f || mass + mAdd <= 0.0f) {
                            resultMsg = "prop mod has no speed attributes";
                            return false;
                        }
                        const float boost = (sf / 100.0f) * (sbf / (mass + mAdd));
                        savedAttrs.push_back(std::make_pair((uint16)AttrMaxVelocity, vMax));
                        savedAttrs.push_back(std::make_pair((uint16)AttrMass, mass));
                        tglShipRef->SetAttribute(AttrMaxVelocity, vMax * (1.0f + boost));
                        tglShipRef->SetAttribute(AttrMass, mass + mAdd);
                        if (pAIShip->DestinyMgr() != nullptr)
                            pAIShip->DestinyMgr()->SpeedBoost();
                    {
                        // VEV_SIM_TOGGLE_CYCLES: register for cycle processing —
                        // cap/cycle (attr 6) every duration (attr 73). The first
                        // cycle's cap was already debited by VEV_SIM_CAPDEBIT.
                        VevToggleState tst;
                        tst.shipItemID = tglShipRef->itemID();
                        tst.charID = charID;
                        tst.saved = savedAttrs;
                        tst.name = moduleName;
                        tst.capNeed = tglModRef->HasAttribute(AttrCapacitorNeed)
                            ? tglModRef->GetAttribute(AttrCapacitorNeed).get_float() : 0.0f;
                        int64 durMs = tglModRef->HasAttribute(AttrDuration)
                            ? (int64)tglModRef->GetAttribute(AttrDuration).get_float() : 30000;
                        if (durMs < 1000) durMs = 1000;
                        tst.durationMs = durMs;
                        tst.nextCycleMs = GetTimeMSeconds() + durMs;
                        // Persist the ENGAGED ship attrs for the gateway's live reads.
                        for (auto& kv : savedAttrs)
                            if (tglShipRef->HasAttribute(kv.first))
                                vevPersistShipAttr(tst.shipItemID, kv.first,
                                                   tglShipRef->GetAttribute(kv.first).get_float());
                        s_vevToggleReg[moduleID] = tst;
                    }
                        char tbuf[200];
                        snprintf(tbuf, sizeof(tbuf), "%s engaged: max velocity %.0f -> %.0f m/s",
                                 moduleName.c_str(), vMax, vMax * (1.0f + boost));
                        sLog.Cyan("activate_module", "TOGGLE-ON char=%u %s", charID, tbuf);
                        resultMsg = tbuf;
                        return true;
                    }
                    // Resist toggles: multiply each ship resonance the module
                    // carries a (damage-reducing, <1.0) multiplier for. Hull
                    // module attrs (974-977) map onto the bare ship resonances
                    // (109/110/111/113) that Damage.cpp's hull pass reads.
                    static const std::pair<uint16, uint16> kResMap[] = {
                        { (uint16)AttrArmorEmDamageResonance,         (uint16)AttrArmorEmDamageResonance },
                        { (uint16)AttrArmorExplosiveDamageResonance,  (uint16)AttrArmorExplosiveDamageResonance },
                        { (uint16)AttrArmorKineticDamageResonance,    (uint16)AttrArmorKineticDamageResonance },
                        { (uint16)AttrArmorThermalDamageResonance,    (uint16)AttrArmorThermalDamageResonance },
                        { (uint16)AttrShieldEmDamageResonance,        (uint16)AttrShieldEmDamageResonance },
                        { (uint16)AttrShieldExplosiveDamageResonance, (uint16)AttrShieldExplosiveDamageResonance },
                        { (uint16)AttrShieldKineticDamageResonance,   (uint16)AttrShieldKineticDamageResonance },
                        { (uint16)AttrShieldThermalDamageResonance,   (uint16)AttrShieldThermalDamageResonance },
                        { (uint16)AttrHullEmDamageResonance,          (uint16)AttrEmDamageResonance },
                        { (uint16)AttrHullExplosiveDamageResonance,   (uint16)AttrExplosiveDamageResonance },
                        { (uint16)AttrHullKineticDamageResonance,     (uint16)AttrKineticDamageResonance },
                        { (uint16)AttrHullThermalDamageResonance,     (uint16)AttrThermalDamageResonance },
                    };
                    int appliedRes = 0;
                    for (size_t ri = 0; ri < sizeof(kResMap)/sizeof(kResMap[0]); ++ri) {
                        if (!tglModRef->HasAttribute(kResMap[ri].first)) continue;
                        const float mult = tglModRef->GetAttribute(kResMap[ri].first).get_float();
                        if (mult <= 0.0f || mult >= 1.0f) continue;   // only damage-REDUCING multipliers
                        const float cur = tglShipRef->HasAttribute(kResMap[ri].second)
                            ? tglShipRef->GetAttribute(kResMap[ri].second).get_float() : 1.0f;
                        savedAttrs.push_back(std::make_pair(kResMap[ri].second, cur));
                        tglShipRef->SetAttribute(kResMap[ri].second, cur * mult);
                        appliedRes++;
                    }
                    if (appliedRes == 0) {
                        resultMsg = "module has no recognized resistance attributes";
                        return false;
                    }
                    {
                        // VEV_SIM_TOGGLE_CYCLES: register for cycle processing —
                        // cap/cycle (attr 6) every duration (attr 73). The first
                        // cycle's cap was already debited by VEV_SIM_CAPDEBIT.
                        VevToggleState tst;
                        tst.shipItemID = tglShipRef->itemID();
                        tst.charID = charID;
                        tst.saved = savedAttrs;
                        tst.name = moduleName;
                        tst.capNeed = tglModRef->HasAttribute(AttrCapacitorNeed)
                            ? tglModRef->GetAttribute(AttrCapacitorNeed).get_float() : 0.0f;
                        int64 durMs = tglModRef->HasAttribute(AttrDuration)
                            ? (int64)tglModRef->GetAttribute(AttrDuration).get_float() : 30000;
                        if (durMs < 1000) durMs = 1000;
                        tst.durationMs = durMs;
                        tst.nextCycleMs = GetTimeMSeconds() + durMs;
                        // Persist the ENGAGED ship attrs for the gateway's live reads.
                        for (auto& kv : savedAttrs)
                            if (tglShipRef->HasAttribute(kv.first))
                                vevPersistShipAttr(tst.shipItemID, kv.first,
                                                   tglShipRef->GetAttribute(kv.first).get_float());
                        s_vevToggleReg[moduleID] = tst;
                    }
                    char rbuf2[200];
                    snprintf(rbuf2, sizeof(rbuf2), "%s engaged: %d resistance layers hardened", moduleName.c_str(), appliedRes);
                    sLog.Cyan("activate_module", "TOGGLE-ON char=%u %s", charID, rbuf2);
                    resultMsg = rbuf2;
                    return true;
                }
                // =================== end VEV_SIM_TOGGLE ===================
                // =================== VEV_SIM_SELFREP ===================
                // Self-repair (Armor Repairer 62 / Shield Booster 40 / Hull
                // Repairer 63): the human path is ModuleManager/Client*-bound, so
                // phantoms get a direct attr-credit sim — mirroring how
                // VEV_SIM_EWAR rescued web/scram from the no-damage rejection.
                // Cap was already debited by VEV_SIM_CAPDEBIT (gates every module
                // kind). SetAttribute is phantom-safe (item attr write, no
                // Client*). The repaired values reach the 2D HUD automatically via
                // VEV_STREAM_SHIP_HP. Target is the ship ITSELF (the client passes
                // its own shipID as targetID to satisfy the mandatory-param gate).
                if (moduleGroupID == 62 || moduleGroupID == 40 || moduleGroupID == 63) {
                    InventoryItemRef repModRef = sItemFactory.GetItemRef(moduleID);
                    InventoryItemRef repShipRef = pAIShip->GetSelf();
                    if (repModRef.get() == nullptr || repShipRef.get() == nullptr) {
                        resultMsg = "selfrep: module/ship item not found";
                        return false;
                    }
                    char rbuf[200];
                    if (moduleGroupID == 40) {   // shield booster -> charge credit, clamped to capacity
                        const float bonus = repModRef->HasAttribute(AttrShieldBonus)
                            ? repModRef->GetAttribute(AttrShieldBonus).get_float() : 0.0f;
                        const float sCap = repShipRef->GetAttribute(AttrShieldCapacity).get_float();
                        const float sCur = repShipRef->GetAttribute(AttrShieldCharge).get_float();
                        const float sNew = (sCur + bonus > sCap) ? sCap : sCur + bonus;
                        repShipRef->SetAttribute(AttrShieldCharge, sNew);
                        snprintf(rbuf, sizeof(rbuf), "shield boosted %.0f HP (%.0f/%.0f)", sNew - sCur, sNew, sCap);
                    } else {                     // armor (62) / hull (63) -> damage decrement, floored at 0
                        const float amount = repModRef->HasAttribute(AttrArmorDamageAmount)
                            ? repModRef->GetAttribute(AttrArmorDamageAmount).get_float() : 0.0f;
                        const uint16 dmgAttr = (moduleGroupID == 62) ? AttrArmorDamage : AttrDamage;
                        const float dCur = repShipRef->HasAttribute(dmgAttr)
                            ? repShipRef->GetAttribute(dmgAttr).get_float() : 0.0f;
                        const float dNew = (dCur - amount > 0.0f) ? dCur - amount : 0.0f;
                        repShipRef->SetAttribute(dmgAttr, dNew);
                        snprintf(rbuf, sizeof(rbuf), "%s repaired %.0f HP (damage %.0f -> %.0f)",
                                 (moduleGroupID == 62) ? "armor" : "hull", dCur - dNew, dCur, dNew);
                    }
                    pAIShip->DestinyMgr()->SendSpecialEffect(
                        pAIShip->GetID(), moduleID, moduleTypeID, pAIShip->GetID(), 0,
                        (moduleGroupID == 40) ? "effects.ShieldBoosting" : "effects.ArmorRepair",
                        1, 1, 1, 0, 0, 0);
                    sLog.Cyan("activate_module", "SELFREP char=%u %s: %s", charID, moduleName.c_str(), rbuf);
                    resultMsg = rbuf;
                    return true;
                }
                // =================== end VEV_SIM_SELFREP ===================
                // =================== end VEV_SIM_EWAR ===================
                resultMsg = "module has no damage attributes — cannot fire";
                return false;
            }

            // =================== VEV_SIM_SMARTBOMB ===================
            // Smartbombs (72) are AoE, not turrets: damage EVERY non-static
            // entity within empFieldRange of the ship (ApplyDamage's own
            // immunity gates handle asteroids etc.). No to-hit — smartbombs
            // always hit (EVE). emDmg etc. were already resolved above.
            if (moduleGroupID == 72) {
                InventoryItemRef sbRef = sItemFactory.GetItemRef(moduleID);
                if (sbRef.get() == nullptr) { resultMsg = "smartbomb item not found"; return false; }
                const float sbRange = sbRef->HasAttribute(99) ? sbRef->GetAttribute(99).get_float() : 5000.0f;
                std::map<uint32, SystemEntity*> sbEnts;
                if (pAIShip->SysBubble() != nullptr) pAIShip->SysBubble()->GetEntities(sbEnts);
                int sbKilled = 0, sbStruck = 0;
                for (std::map<uint32, SystemEntity*>::iterator sit = sbEnts.begin(); sit != sbEnts.end(); ++sit) {
                    SystemEntity* pE = sit->second;
                    if (pE == nullptr || pE == pAIShip) continue;
                    if (pE->IsStaticEntity()) continue;
                    if (pAIShip->GetPosition().distance(pE->GetPosition()) > sbRange) continue;
                    Damage sbDmg(pAIShip, sbRef, kinDmg, therDmg, emDmg, expDmg, 1.0f,
                                 EVEEffectID::targetAttack);
                    sbStruck++;
                    if (pE->ApplyDamage(sbDmg)) sbKilled++;
                }
                pAIShip->DestinyMgr()->SendSpecialEffect(
                    pAIShip->GetID(), moduleID, moduleTypeID, pAIShip->GetID(), 0,
                    "effects.EMPWave", 1, 1, 1, 0, 0, 0);
                vev::grid::NoteWeaponFire(pAIShip->GetID(), pAIShip->GetID(), moduleTypeID, moduleGroupID);
                char sbBuf[200];
                snprintf(sbBuf, sizeof(sbBuf), "smartbomb: struck %d within %.0fm (%d destroyed)",
                         sbStruck, sbRange, sbKilled);
                sLog.Cyan("activate_module", "SMARTBOMB char=%u %s", charID, sbBuf);
                resultMsg = sbBuf;
                return true;
            }
            // =================== end VEV_SIM_SMARTBOMB ===================

            // Get weapon item for turret stats
            InventoryItemRef weapRef = sItemFactory.GetItemRef(moduleID);
            float dmgMult = 1.0f;
            float optimal = 6000.0f;
            float falloff = 2000.0f;
            float tracking = 0.25f;
            float sigRes = 40000.0f;
            if (weapRef.get() != nullptr) {
                if (weapRef->HasAttribute(AttrDamageMultiplier))
                    dmgMult = weapRef->GetAttribute(AttrDamageMultiplier).get_float();
                if (weapRef->HasAttribute(AttrMaxRange))
                    optimal = weapRef->GetAttribute(AttrMaxRange).get_float();
                if (weapRef->HasAttribute(AttrFalloff))
                    falloff = weapRef->GetAttribute(AttrFalloff).get_float();
                if (weapRef->HasAttribute(AttrTrackingSpeed))
                    tracking = weapRef->GetAttribute(AttrTrackingSpeed).get_float();
                if (weapRef->HasAttribute(AttrOptimalSigRadius))
                    sigRes = weapRef->GetAttribute(AttrOptimalSigRadius).get_float();
            }

            // Apply range multiplier from charge (e.g. Radio crystals extend range)
            if (hasCharge && chargeRef->HasAttribute(AttrWeaponRangeMultiplier)) {
                float rangeMult = chargeRef->GetAttribute(AttrWeaponRangeMultiplier).get_float();
                optimal *= rangeMult;
                falloff *= rangeMult;
            }

            // VEV_SIM_PASSIVES: fitted damage mods multiply weapon damage
            // (Heat Sink 205 → energy 53, Gyrostab 59 → projectile 55,
            // MagStab 302 → hybrid 74, BCS 367 → launchers); Tracking
            // Computers (213) add % optimal/falloff/tracking. Online only,
            // read at fire time.
            float vevDmgModMult = 1.0f;
            {
                uint32 dmgModGroup = 367;   // default: launcher groups → BCS
                if      (moduleGroupID == 53) dmgModGroup = 205;
                else if (moduleGroupID == 55) dmgModGroup = 59;
                else if (moduleGroupID == 74) dmgModGroup = 302;
                float rangePct = 0.0f, falloffPct = 0.0f, trackPct = 0.0f;
                DBQueryResult dm;
                if (sDatabase.RunQuery(dm,
                    "SELECT g.groupID, da.attributeID, COALESCE(da.valueFloat, da.valueInt)"
                    " FROM entity e"
                    " JOIN invTypes t ON t.typeID = e.typeID"
                    " JOIN invGroups g ON g.groupID = t.groupID"
                    " LEFT JOIN entity_attributes ea ON ea.itemID = e.itemID AND ea.attributeID = 2"
                    " JOIN dgmTypeAttributes da ON da.typeID = e.typeID AND da.attributeID IN (64,213,349,351,767)"
                    " WHERE e.locationID = (SELECT shipID FROM chrCharacters WHERE characterID = %u)"
                    "   AND e.flag BETWEEN 11 AND 34 AND g.groupID IN (%u, 213)"
                    "   AND COALESCE(ea.valueInt, 1) <> 0", charID, dmgModGroup)) {
                    DBResultRow drow;
                    while (dm.GetRow(drow)) {
                        uint32 gid = drow.GetUInt(0);
                        uint16 da2 = (uint16)drow.GetUInt(1);
                        float dv = drow.GetFloat(2);
                        if (gid == dmgModGroup && (da2 == 64 || da2 == 213) && dv > 0.0f) vevDmgModMult *= dv;
                        if (gid == 213 && da2 == 351) rangePct   += dv;
                        if (gid == 213 && da2 == 349) falloffPct += dv;
                        if (gid == 213 && da2 == 767) trackPct   += dv;
                    }
                }
                optimal  *= (1.0f + rangePct   / 100.0f);
                falloff  *= (1.0f + falloffPct / 100.0f);
                tracking *= (1.0f + trackPct   / 100.0f);
            }

            // Calculate to-hit using EVE turret formula (same as TurretFormulas::GetNPCToHit)
            double distance = pAIShip->GetPosition().distance(pTarget->GetPosition());
            GVector relVel = pTarget->GetVelocity() - pAIShip->GetVelocity();
            double transV = relVel.length();
            double angularVel = (distance > 1.0) ? (transV / distance) : 0;
            float targetSig = pTarget->GetSelf()->GetAttribute(AttrSignatureRadius).get_float();
            if (targetSig <= 0) targetSig = 100.0f;
            // VEV_SIM_PAINTER: a painted target's signature blooms -> easier to hit.
            targetSig *= vev::grid::GetPaintMult((uint32_t)targetID);

            float a = (tracking > 0) ? (float)(angularVel / tracking) : 0;
            float b = (targetSig > 0) ? (sigRes / targetSig) : 1.0f;
            float c = pow(a * b, 2.0f);
            float rangeExcess = (float)std::max(0.0, distance - (double)optimal);
            float e = (falloff > 0) ? pow(rangeExcess / falloff, 2.0f) : 0;
            float chanceToHit = pow(0.5f, c + e);

            float rNum = MakeRandomFloat(0.0f, 1.0f);
            float toHit = 0.0f;
            if (rNum <= 0.02f) toHit = 3.0f;                    // critical hit
            else if (rNum < chanceToHit) toHit = rNum + 0.49f;  // normal hit
            // else 0.0 = miss

            // Apply weapon damage multiplier (incl. fitted damage mods)
            emDmg *= dmgMult * vevDmgModMult;
            therDmg *= dmgMult * vevDmgModMult;
            kinDmg *= dmgMult * vevDmgModMult;
            expDmg *= dmgMult * vevDmgModMult;

            // Create Damage and apply (same pipeline as NPC AttackTarget)
            Damage dmg(pAIShip, weapRef.get() ? weapRef : pAIShip->GetSelf(),
                       kinDmg, therDmg, emDmg, expDmg, toHit,
                       EVEEffectID::targetAttack);
            bool killed = pTarget->ApplyDamage(dmg);

            // Send turret visual effect to players in bubble
            std::string effectGuid = "effects.Laser";
            int32 cycleDuration = 3500;
            if (weapRef.get() && weapRef->HasAttribute(AttrSpeed))
                cycleDuration = (int32)weapRef->GetAttribute(AttrSpeed).get_float();
            pAIShip->DestinyMgr()->SendSpecialEffect(
                pAIShip->GetID(), moduleID, moduleTypeID, targetID,
                hasCharge ? chargeRef->typeID() : 0,
                effectGuid, 1, 1, 1, cycleDuration, 0, 0);
            // VEV_WEAPON_FIRE: record for the 2D GridStreamer (twin of NoteMining)
            // so every 2D client draws this ship's weapon fire.
            vev::grid::NoteWeaponFire(pAIShip->GetID(), targetID, moduleTypeID, moduleGroupID);

            // VEV_AMMO: a real shot consumes one round (missiles too). Stack
            // empty -> the charge item is deleted; the next activation falls
            // back to "no charge" (Civilian weapons keep firing; charged guns
            // reject until re-ammoed via fitting).
            if (hasCharge && chargeRef.get() != nullptr) {
                if (chargeRef->quantity() > 1) chargeRef->AlterQuantity(-1, true);
                else chargeRef->Delete();
            }

            float totalDmg = (emDmg + therDmg + kinDmg + expDmg) * toHit;
            sLog.Cyan("activate_module", "WEAPON char=%u %s → %s(%u) dist=%.0fm chance=%.1f%% toHit=%.2f dmg=%.1f%s",
                charID, moduleName.c_str(), pTarget->GetName(), targetID, distance,
                chanceToHit * 100.0f, toHit, totalDmg, killed ? " KILLED" : "");

            char buf[300];
            snprintf(buf, sizeof(buf), "weapon: %s fired at %s — dist:%.0fm, hit:%.0f%%, %s, dmg:%.1f%s",
                     moduleName.c_str(), pTarget->GetName(), distance,
                     chanceToHit * 100.0f,
                     toHit > 2.5f ? "CRIT" : (toHit > 0 ? "HIT" : "MISS"),
                     totalDmg,
                     killed ? " — TARGET DESTROYED" : "");
            resultMsg = buf;
            return true;
        }

        // Verify target is an asteroid
        if (!pTarget->IsAsteroidSE()) {
            resultMsg = "target is not an asteroid — cannot mine";
            return false;
        }

        // VEV_MINING_RANGE (Track A, 2026-06-11): a mining laser only reaches its
        // optimal range (Miner I AttrMaxRange=54 -> 10,000m). The AI mining path
        // bypasses ModuleManager (Client*-gated) and previously extracted ore at ANY
        // distance, so pilots mined from 17km and never moved (congregating on the
        // warp-in beacon). Gate it so the FSM/human must approach within range.
        {
            float miningRange = 0.0f;
            DBQueryResult rngRes;
            if (sDatabase.RunQuery(rngRes,
                "SELECT IFNULL(valueFloat, valueInt) FROM dgmTypeAttributes"
                " WHERE typeID = %u AND attributeID = 54", moduleTypeID))  // AttrMaxRange
            {
                DBResultRow rngRow;
                if (rngRes.GetRow(rngRow)) miningRange = rngRow.GetFloat(0);
            }
            if (miningRange > 0.0f) {
                double mdist = pAIShip->GetPosition().distance(pTarget->GetPosition());
                if (mdist > miningRange) {
                    char rbuf[220];
                    snprintf(rbuf, sizeof(rbuf),
                        "target out of mining range: %.0fm away, %s reaches %.0fm -- approach first",
                        mdist, moduleName.c_str(), miningRange);
                    resultMsg = rbuf;
                    return false;
                }
            }
        }

        // Get mining yield: miningAmount attribute (m3 per cycle) on the module
        // Strip Miner I typeID 17482: miningAmount = 540.0 m3/cycle (180s cycle)
        DBQueryResult yieldRes;
        float miningAmount = 540.0f;  // default for Strip Miner I
        float cycleDuration = 180.0f; // seconds
        if (sDatabase.RunQuery(yieldRes,
            "SELECT"
            " IFNULL((SELECT IFNULL(valueFloat, valueInt) FROM dgmTypeAttributes"
            "   WHERE typeID = %u AND attributeID = 77), 540.0),"  // AttrMiningAmount = 77
            " IFNULL((SELECT IFNULL(valueFloat, valueInt) FROM dgmTypeAttributes"
            "   WHERE typeID = %u AND attributeID = 73), 180000.0)",  // AttrDuration = 73 (ms)
            moduleTypeID, moduleTypeID))
        {
            DBResultRow yieldRow;
            if (yieldRes.GetRow(yieldRow)) {
                miningAmount = yieldRow.GetFloat(0);
                cycleDuration = yieldRow.GetFloat(1) / 1000.0f;  // ms → seconds
            }
        }

        // VEV_SIM_PASSIVES: Mining Laser Upgrades (group 546) multiply yield —
        // +miningAmountBonus% (attr 434) per fitted + online module.
        {
            DBQueryResult ur;
            if (sDatabase.RunQuery(ur,
                "SELECT COALESCE(da.valueFloat, da.valueInt)"
                " FROM entity e"
                " JOIN invTypes t ON t.typeID = e.typeID"
                " JOIN invGroups g ON g.groupID = t.groupID"
                " LEFT JOIN entity_attributes ea ON ea.itemID = e.itemID AND ea.attributeID = 2"
                " JOIN dgmTypeAttributes da ON da.typeID = e.typeID AND da.attributeID = 434"
                " WHERE e.locationID = (SELECT shipID FROM chrCharacters WHERE characterID = %u)"
                "   AND e.flag BETWEEN 11 AND 34 AND g.groupID = 546"
                "   AND COALESCE(ea.valueInt, 1) <> 0", charID)) {
                DBResultRow urow;
                while (ur.GetRow(urow))
                    miningAmount *= (1.0f + urow.GetFloat(0) / 100.0f);
            }
        }

        // Get asteroid ore type and remaining quantity
        InventoryItemRef roidRef = pTarget->GetSelf();
        if (roidRef.get() == nullptr) {
            resultMsg = "asteroid has no item reference";
            return false;
        }
        uint32 oreTypeID = roidRef->typeID();
        float oreVolume = roidRef->GetAttribute(AttrVolume).get_float();
        float roidQuantity = roidRef->GetAttribute(AttrQuantity).get_float();

        if (roidQuantity <= 0 || oreVolume <= 0) {
            resultMsg = "asteroid is depleted";
            return false;
        }

        // Calculate ore amount from one cycle
        // VEV_PARTIAL_YIELD: an early-deactivated mining cycle awards prorated ore.
        // The client sends fraction = elapsed/cycleDuration on mid-cycle stop;
        // absent/1.0 = a full cycle. Clamped 0..1.
        {
            double yfrac = 1.0;
            size_t fPos = p.find("\"fraction\"");
            if (fPos != std::string::npos) {
                size_t c = p.find(":", fPos);
                if (c != std::string::npos) yfrac = atof(p.c_str() + c + 1);
            }
            if (yfrac < 0.0) yfrac = 0.0;
            if (yfrac > 1.0) yfrac = 1.0;
            miningAmount *= (float)yfrac;
            if (miningAmount <= 0.01f) {
                resultMsg = "cycle stopped (no yield)";
                return true;
            }
        }
        float oreAmount = miningAmount / oreVolume;
        if (oreAmount > roidQuantity)
            oreAmount = roidQuantity;

        // Check cargo space on the ship
        InventoryItemRef shipRef = pAIShip->GetSelf();
        // Procurer has ore hold (flagOreHold=133), check that first, fallback to cargo (flagCargoHold=5)
        EVEItemFlags holdFlag = flagCargoHold;
        if (shipRef->HasAttribute(AttrOreHoldCapacity))
            holdFlag = flagOreHold;

        float remainingVolume = shipRef->GetMyInventory()->GetRemainingCapacity(holdFlag);
        float neededVolume = oreAmount * oreVolume;
        if (remainingVolume < oreVolume) {
            resultMsg = "cargo/ore hold is full — cannot mine";
            return false;
        }
        if (neededVolume > remainingVolume) {
            // Partial fill — only mine what fits
            oreAmount = remainingVolume / oreVolume;
        }

        // Extract ore from asteroid
        roidQuantity -= oreAmount;
        if (roidQuantity > 0.0f) {
            roidRef->SetAttribute(AttrQuantity, roidQuantity);
            // Update asteroid radius (visual size)
            double radius = exp((roidQuantity + 112404.8) / 25000);
            roidRef->SetAttribute(AttrRadius, radius);
        } else {
            // Asteroid depleted — remove it
            pTarget->Delete();
        }

        // Create ore item in ship cargo/ore hold
        ItemData idata(oreTypeID, shipRef->ownerID(), shipRef->itemID(), holdFlag, (uint32)oreAmount);  // VEV: spawn ore DIRECTLY in cargo (locTemp/flagNone + Move into phantom AIShipSE hold is a silent no-op -> ore stranded at loc=5/flag=0)
        InventoryItemRef oRef = sItemFactory.SpawnItem(idata);
        if (oRef.get() != nullptr) {
            // Move ore directly into ship's hold (simpler than MergeTypesInCargo which needs ShipItem*)
            oRef->Move(shipRef->itemID(), holdFlag, true);
        }

        // Send mining visual effect to players in bubble
        // effectID 67 = miningLaser, guid = "effects.Mining" (from dgmEffects table)
        std::string effectGuid = "effects.Mining";
        pAIShip->DestinyMgr()->SendSpecialEffect(
            pAIShip->GetID(),  // entityID
            moduleID,          // moduleID (for turret visual)
            moduleTypeID,      // moduleTypeID
            targetID,          // targetID  
            0,                 // chargeTypeID
            effectGuid,        // guid
            0,                 // isOffensive
            1,                 // start
            1,                 // active
            (int32)(cycleDuration * 1000),  // duration ms
            0,                 // repeat
            0                  // graphicID (0 = use default)
        );

        char buf[256];
        snprintf(buf, sizeof(buf), "mining: %s activated on %s — extracted %.0f units of ore (typeID %u), %.1f m3, %.0f remaining in asteroid",
                 moduleName.c_str(), pTarget->GetName(), oreAmount, oreTypeID, oreAmount * oreVolume, roidQuantity > 0 ? roidQuantity : 0.0f);
        resultMsg = buf;
        // VEV_BEAM_AFTER_EXTRACT: extraction succeeded -> light the harvest beam.
        vev::grid::NoteMining(shipID, targetID);
        return true;
    }

    // --- reload_ammo: in-space ammo swap for a fitted weapon ---
    // VEV_RELOAD: unload the slot's current charge to cargo, then load the
    // requested type from cargo (qty = module capacity / charge volume — the
    // per-gun magazine, EVE-real). chargeTypeID 0 = unload only.
    // params: {"slotFlag": N, "chargeTypeID": N}
    if (cmd == "reload_ammo") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "AI ship not in space"; return false; }
        uint32 raFlag = 0, raType = 0;
        {
            std::string p(params);
            size_t fp = p.find("\"slotFlag\"");
            if (fp != std::string::npos) raFlag = (uint32)strtoul(p.c_str() + p.find_first_of("0123456789", fp), nullptr, 10);
            size_t tp = p.find("\"chargeTypeID\"");
            if (tp != std::string::npos) raType = (uint32)strtoul(p.c_str() + p.find_first_of("0123456789", tp), nullptr, 10);
        }
        if (raFlag < 11 || raFlag > 34) { resultMsg = "reload_ammo requires a fit slotFlag (11-34)"; return false; }
        ShipItemRef raShip = ShipItemRef::StaticCast(pAIShip->GetSelf());
        if (raShip.get() == nullptr || raShip->GetMyInventory() == nullptr) { resultMsg = "ship inventory unavailable"; return false; }
        Inventory* raInv = raShip->GetMyInventory();
        // the module in this slot (singleton) + any current charge (non-singleton)
        std::vector<InventoryItemRef> slotItems;
        raInv->GetItemsByFlag((EVEItemFlags)raFlag, slotItems);
        InventoryItemRef raMod(nullptr), raCur(nullptr);
        for (size_t ri = 0; ri < slotItems.size(); ++ri) {
            if (slotItems[ri].get() == nullptr) continue;
            if (slotItems[ri]->isSingleton()) raMod = slotItems[ri];
            else raCur = slotItems[ri];
        }
        if (raMod.get() == nullptr) { resultMsg = "no module fitted in that slot"; return false; }
        std::string raOut;
        // 1) unload the current charge into cargo
        if (raCur.get() != nullptr) {
            char ub[96];
            snprintf(ub, sizeof(ub), "unloaded %ux %s; ", raCur->quantity(), raCur->name());
            raOut += ub;
            raCur->MergeTypesInCargo(raShip.get(), flagCargoHold);
        }
        // 2) load the requested type from cargo
        if (raType != 0) {
            std::vector<InventoryItemRef> cargo;
            raInv->GetItemsByFlag(flagCargoHold, cargo);
            InventoryItemRef raSrc(nullptr);
            for (size_t ri = 0; ri < cargo.size(); ++ri) {
                if (cargo[ri].get() != nullptr && cargo[ri]->typeID() == raType) { raSrc = cargo[ri]; break; }
            }
            if (raSrc.get() == nullptr) {
                resultMsg = (raOut + "no such ammo in cargo").c_str();
                return false;
            }
            // magazine size = module capacity / charge volume (floor), min 1
            float raCap = raMod->HasAttribute(AttrCapacity) ? raMod->GetAttribute(AttrCapacity).get_float() : 0.0f;
            float raVol = raSrc->type().volume();
            int32 raFit = (raCap > 0.0f && raVol > 0.0f) ? (int32)(raCap / raVol) : raSrc->quantity();
            if (raFit < 1) raFit = 1;
            if (raFit >= raSrc->quantity()) {
                raSrc->Move(raShip->itemID(), (EVEItemFlags)raFlag, true);
                char lb[96];
                snprintf(lb, sizeof(lb), "loaded %ux %s", raSrc->quantity(), raSrc->name());
                raOut += lb;
            } else {
                raSrc->AlterQuantity(-raFit, true);
                ItemData raNew(raType, charID, locTemp, flagNone, (uint32)raFit);
                InventoryItemRef raRef = sItemFactory.SpawnItem(raNew);
                if (raRef.get() == nullptr) { resultMsg = "reload: charge spawn failed"; return false; }
                raRef->Move(raShip->itemID(), (EVEItemFlags)raFlag, true);
                char lb[96];
                snprintf(lb, sizeof(lb), "loaded %dx %s", raFit, raRef->name());
                raOut += lb;
            }
        }
        if (raOut.empty()) raOut = "slot had no charge and none requested";
        sLog.Cyan("reload_ammo", "char=%u flag=%u type=%u: %s", charID, raFlag, raType, raOut.c_str());
        resultMsg = raOut;
        return true;
    }

    // --- loot_wreck: move EVERYTHING from a wreck/container into our cargo ---
    // VEV_LOOT_WRECK: the 2D Loot window's verb. Range-gated at 2500m (EVE's
    // loot range). Space-checked per stack; an emptied wreck stays on grid for
    // the SALVAGER (loot-first rule). params: {"targetID": <wreckID>}
    if (cmd == "loot_wreck") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "AI ship not in space"; return false; }
        uint32 lwTargetID = 0;
        {
            std::string p(params);
            size_t tpos = p.find("\"targetID\"");
            if (tpos != std::string::npos) lwTargetID = (uint32)strtoul(p.c_str() + p.find_first_of("0123456789", tpos), nullptr, 10);
        }
        if (lwTargetID == 0) { resultMsg = "loot_wreck requires targetID"; return false; }
        SystemEntity* pWreck = pAIShip->SystemMgr()->GetSE(lwTargetID);
        if (pWreck == nullptr) { resultMsg = "wreck not found on grid"; return false; }
        if (!pWreck->IsWreckSE() && !pWreck->IsContainerSE()) { resultMsg = "target is not a wreck/container"; return false; }
        const double lwDist = pAIShip->GetPosition().distance(pWreck->GetPosition());
        if (lwDist > 2500.0) {
            char lwOor[120];
            snprintf(lwOor, sizeof(lwOor), "out of loot range: %.0fm > 2500m", lwDist);
            resultMsg = lwOor;
            return false;
        }
        InventoryItemRef lwRef = pWreck->GetSelf();
        if (lwRef.get() == nullptr || lwRef->GetMyInventory() == nullptr) { resultMsg = "wreck has no inventory"; return false; }
        ShipItemRef lwShip = ShipItemRef::StaticCast(pAIShip->GetSelf());
        Inventory* lwInv = (lwShip.get() != nullptr) ? lwShip->GetMyInventory() : nullptr;
        if (lwInv == nullptr) { resultMsg = "our ship has no inventory"; return false; }
        std::map<uint32, InventoryItemRef> lwMap;
        lwRef->GetMyInventory()->GetInventoryMap(lwMap);
        int lwMoved = 0, lwSkipped = 0;
        std::string lwGot;
        for (std::map<uint32, InventoryItemRef>::iterator lit = lwMap.begin(); lit != lwMap.end(); ++lit) {
            InventoryItemRef li = lit->second;
            if (li.get() == nullptr) continue;
            if (lwInv->HasAvailableSpace(flagCargoHold, li)) {
                char lwRow[96];
                snprintf(lwRow, sizeof(lwRow), " %ux %s;", li->quantity(), li->name());
                li->MergeTypesInCargo(lwShip.get(), flagCargoHold);
                lwGot += lwRow;
                lwMoved++;
            } else {
                lwSkipped++;
            }
        }
        char lwBuf[300];
        snprintf(lwBuf, sizeof(lwBuf), "looted %d stacks%s%s", lwMoved, lwGot.c_str(),
                 lwSkipped ? " (some left — cargo full)" : (lwMoved ? " wreck is empty — salvageable" : " — wreck was empty"));
        sLog.Cyan("loot_wreck", "char=%u wreck=%u: %s", charID, lwTargetID, lwBuf);
        resultMsg = lwBuf;
        return true;
    }

    // --- deactivate_module: Stop a module (mining laser, weapon) ---
    // For AI ships, this is a no-op since we don't have persistent module cycling.
    // Each activate_module is a single-shot extraction. This command exists for API completeness.
    if (cmd == "deactivate_module") {
        resultMsg = "module deactivated (AI modules are single-cycle)";
        return true;
    }

    // --- fit_module: Equip a module from hangar/cargo onto the ship ---
    // See A344 §7.3 (Ship Fitting Awareness) + A348 (Ship Fit Engineering Feat)
    // params: {"itemID": 140001013, "slotFlag": 28}
    //   itemID = the module item to fit (must be in station hangar, flag=4)
    //   slotFlag = target slot (27-34=high, 19-26=mid, 11-18=low)
    // If the target slot is occupied, the existing module is moved to station hangar first.
    // Character MUST be docked (phantom player).
    if (cmd == "fit_module") {
        // Parse params first
        std::string p(params);
        uint32 itemID = 0;
        uint32 slotFlag = 0;
        size_t iPos = p.find("\"itemID\"");
        if (iPos != std::string::npos) { size_t c = p.find(":", iPos); if (c != std::string::npos) itemID = (uint32)atol(p.c_str() + c + 1); }
        size_t sPos = p.find("\"slotFlag\"");
        if (sPos != std::string::npos) { size_t c = p.find(":", sPos); if (c != std::string::npos) slotFlag = (uint32)atol(p.c_str() + c + 1); }

        if (itemID == 0 || slotFlag == 0) {
            resultMsg = "missing 'itemID' or 'slotFlag' in params";
            return false;
        }

        // Validate slot range (high=27-34, mid=19-26, low=11-18)
        if (!((slotFlag >= 11 && slotFlag <= 18) || (slotFlag >= 19 && slotFlag <= 26) || (slotFlag >= 27 && slotFlag <= 34))) {
            resultMsg = "invalid slotFlag — must be 11-18 (low), 19-26 (mid), or 27-34 (high)";
            return false;
        }

        // Get character's station and ship — docked check via stationID > 0
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID, shipID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 stationID = charRow.GetUInt(0);
        uint32 shipID = charRow.GetUInt(1);
        if (stationID == 0) {
            resultMsg = "must be docked to fit modules";
            return false;
        }
        if (shipID == 0) {
            resultMsg = "no ship assigned";
            return false;
        }

        // Verify the module exists, belongs to this character, and is in hangar (flag=4)
        DBQueryResult modRes;
        if (!sDatabase.RunQuery(modRes,
            "SELECT e.itemID, e.typeID, e.flag, e.locationID, t.typeName, g.categoryID, g.groupName"
            " FROM entity e"
            " JOIN invTypes t ON t.typeID = e.typeID"
            " JOIN invGroups g ON g.groupID = t.groupID"
            " WHERE e.itemID = %u AND e.ownerID = %u",
            itemID, charID)) {
            resultMsg = "failed to query module";
            return false;
        }
        DBResultRow modRow;
        if (!modRes.GetRow(modRow)) {
            resultMsg = "module not found or not owned by this character";
            return false;
        }
        uint32 modFlag = modRow.GetUInt(2);
        uint32 modCategory = modRow.GetUInt(5);
        std::string modName = modRow.GetText(4);
        std::string groupName = modRow.GetText(6);

        // Must be a Module category item
        if (modCategory != 7) {  // 7 = Module category
            char buf[128];
            snprintf(buf, sizeof(buf), "%s is not a module (category %u)", modName.c_str(), modCategory);
            resultMsg = buf;
            return false;
        }

        // Module must be in hangar (flag=4) or cargo (flag=5)
        if (modFlag != 4 && modFlag != 5) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s is not in hangar or cargo (flag=%u) — already fitted?", modName.c_str(), modFlag);
            resultMsg = buf;
            return false;
        }

        // Check if the target slot is occupied — if so, unfit the existing module to hangar first
        DBQueryResult slotRes;
        if (!sDatabase.RunQuery(slotRes,
            "SELECT e.itemID, t.typeName FROM entity e"
            " JOIN invTypes t ON t.typeID = e.typeID"
            " WHERE e.locationID = %u AND e.flag = %u",
            shipID, slotFlag)) {
            resultMsg = "failed to query existing slot";
            return false;
        }
        DBResultRow slotRow;
        std::string swappedOut = "";
        if (slotRes.GetRow(slotRow)) {
            // Slot occupied — move existing module to station hangar
            uint32 existingID = slotRow.GetUInt(0);
            std::string existingName = slotRow.GetText(1);
            sDatabase.RunQuery(charRes,
                "UPDATE entity SET locationID = %u, flag = 4 WHERE itemID = %u",
                stationID, existingID);
            char buf[128];
            snprintf(buf, sizeof(buf), " (swapped out %s)", existingName.c_str());
            swappedOut = buf;
        }

        // Fit the new module: move it to the ship at the target slot
        sDatabase.RunQuery(charRes,
            "UPDATE entity SET locationID = %u, flag = %u, singleton = 1 WHERE itemID = %u",
            shipID, slotFlag, itemID);

        // =================== VEV_MODULE_ONLINE ===================
        // EVE onlines a module as it is fitted (CPU/PG permitting). Nothing
        // ever set isOnline (attr 2) on the AI path, so every fitted module
        // read as a dead fit in dogma-derived views (curator-found via the
        // fitting window). Phantom sim: online unconditionally on fit.
        sDatabase.RunQuery(charRes,
            "INSERT INTO entity_attributes (itemID, attributeID, valueInt, valueFloat) "
            "VALUES (%u, 2, 1, NULL) "
            "ON DUPLICATE KEY UPDATE valueInt = 1, valueFloat = NULL",
            itemID);
        // =================== end VEV_MODULE_ONLINE ===================

        char buf[256];
        snprintf(buf, sizeof(buf), "fitted %s to slot %u on ship %u%s (online)",
                 modName.c_str(), slotFlag, shipID, swappedOut.c_str());
        resultMsg = buf;
        return true;
    }

    // =================== VEV_MODULE_ONLINE ===================
    // online_module / offline_module: toggle isOnline (attr 2) on whatever is
    // fitted in `slotFlag` of the ACTIVE ship. Works docked or in space (EVE
    // allows onlining in space, capacitor permitting — phantom sim skips the
    // cap cost). params: {"slotFlag": 27}
    if (cmd == "online_module" || cmd == "offline_module") {
        const bool vevOn = (cmd[1] == 'n');   // "online_module"
        std::string p(params);
        uint32 slotFlag = 0;
        size_t sPos = p.find("\"slotFlag\"");
        if (sPos != std::string::npos) { size_t c = p.find(":", sPos); if (c != std::string::npos) slotFlag = (uint32)atol(p.c_str() + c + 1); }
        if (slotFlag == 0) { resultMsg = "missing 'slotFlag' in params"; return false; }
        DBQueryResult cres;
        if (!sDatabase.RunQuery(cres,
            "SELECT COALESCE(shipID,0) FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow crow;
        if (!cres.GetRow(crow) || crow.GetUInt(0) == 0) {
            resultMsg = "no active ship";
            return false;
        }
        const uint32 vevShip = crow.GetUInt(0);
        DBQueryResult mres;
        DBResultRow mrow;
        if (!sDatabase.RunQuery(mres,
            "SELECT e.itemID, t.typeName FROM entity e JOIN invTypes t ON t.typeID = e.typeID "
            "WHERE e.locationID = %u AND e.flag = %u", vevShip, slotFlag)
            || !mres.GetRow(mrow)) {
            resultMsg = "no module in that slot";
            return false;
        }
        const uint32 vevMod = mrow.GetUInt(0);
        std::string vevModName = mrow.GetText(1);
        // live item first (in-space instanced fits), SQL write-through always
        InventoryItemRef vevRef = sItemFactory.GetItemRef(vevMod);
        if (vevRef.get() != nullptr)
            vevRef->SetAttribute(AttrOnline, (int)(vevOn ? 1 : 0));   // attr 2
        DBerror verr;
        sDatabase.RunQuery(verr,
            "INSERT INTO entity_attributes (itemID, attributeID, valueInt, valueFloat) "
            "VALUES (%u, 2, %u, NULL) "
            "ON DUPLICATE KEY UPDATE valueInt = %u, valueFloat = NULL",
            vevMod, vevOn ? 1 : 0, vevOn ? 1 : 0);
        char obuf[160];
        snprintf(obuf, sizeof(obuf), "%s %s (slot %u)",
                 vevModName.c_str(), vevOn ? "ONLINE" : "OFFLINE", slotFlag);
        resultMsg = obuf;
        return true;
    }
    // =================== end VEV_MODULE_ONLINE ===================

    // --- unfit_module: Remove a fitted module from ship to station hangar ---
    // See A344 §7.3 (Ship Fitting Awareness)
    // params: {"slotFlag": 28} — remove whatever is in this slot
    // Character MUST be docked.
    if (cmd == "unfit_module") {
        std::string p(params);
        uint32 slotFlag = 0;
        size_t sPos = p.find("\"slotFlag\"");
        if (sPos != std::string::npos) { size_t c = p.find(":", sPos); if (c != std::string::npos) slotFlag = (uint32)atol(p.c_str() + c + 1); }

        if (slotFlag == 0) {
            resultMsg = "missing 'slotFlag' in params";
            return false;
        }

        // Get character's station and ship — docked check via stationID > 0
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID, shipID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 stationID = charRow.GetUInt(0);
        uint32 shipID = charRow.GetUInt(1);
        if (stationID == 0) { resultMsg = "not docked"; return false; }
        if (shipID == 0) { resultMsg = "no ship"; return false; }

        // Find module in the slot
        DBQueryResult modRes;
        if (!sDatabase.RunQuery(modRes,
            "SELECT e.itemID, t.typeName FROM entity e"
            " JOIN invTypes t ON t.typeID = e.typeID"
            " WHERE e.locationID = %u AND e.flag = %u",
            shipID, slotFlag)) {
            resultMsg = "failed to query slot";
            return false;
        }
        DBResultRow modRow;
        if (!modRes.GetRow(modRow)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "no module in slot %u", slotFlag);
            resultMsg = buf;
            return false;
        }
        uint32 modItemID = modRow.GetUInt(0);
        std::string modName = modRow.GetText(1);

        // Move to station hangar
        sDatabase.RunQuery(charRes,
            "UPDATE entity SET locationID = %u, flag = 4 WHERE itemID = %u",
            stationID, modItemID);

        char buf[128];
        snprintf(buf, sizeof(buf), "unfit %s from slot %u to station hangar", modName.c_str(), slotFlag);
        resultMsg = buf;
        return true;
    }

    // --- login_in_space: warp-in cinematic for a 2D pilot entering in space ---
    // When a phantom pilot logs in while IN SPACE (stationID=0) they don't pop
    // into existence next to the station — they appear ~1.5 AU out IN THE x/z
    // PLANE (Vev drops Y) and WARP IN to the station, decelerating to ~10km off.
    // Presence: online + Local (no station guest, they're in space). Idempotent:
    // an existing ball is repositioned + re-warped, not duplicated.
    if (cmd == "login_in_space") {
        vevRecalcPassives(charID);   // VEV_SIM_PASSIVES: apply fitted passives before the spawn reads attrs
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT c.characterName, c.corporationID, c.solarSystemID, c.shipID,"
            " c.securityRating, c.bounty,"
            " IFNULL(corp.allianceID,0), IFNULL(corp.warFactionID,0)"
            " FROM chrCharacters c"
            " LEFT JOIN crpCorporation corp ON corp.corporationID = c.corporationID"
            " WHERE c.characterID = %u", charID))
        {
            resultMsg = "DB query failed";
            return false;
        }
        DBResultRow row;
        if (!charRes.GetRow(row)) {
            resultMsg = "character not found";
            return false;
        }
        std::string charName = row.GetText(0);
        uint32 corpID = row.GetUInt(1);
        uint32 solarSystemID = row.GetUInt(2);
        uint32 shipID = row.GetUInt(3);
        float securityRating = row.GetFloat(4);
        float bounty = row.GetFloat(5);
        uint32 allianceID = row.GetUInt(6);
        uint32 warFactionID = row.GetUInt(7);
        if (solarSystemID == 0) { resultMsg = "character has no solar system"; return false; }
        if (shipID == 0) { resultMsg = "character has no ship (shipID = 0)"; return false; }

        // VEV login-location: prefer the ship's last in-space position (saved on
        // logout) so a refresh resumes WHERE THEY WERE (e.g. the belt), not warped
        // in to the station.
        GPoint lastPos(0,0,0); bool haveLastPos = false;
        {
            DBQueryResult lpres;
            if (sDatabase.RunQuery(lpres, "SELECT x, y, z FROM entity WHERE itemID = %u", shipID)) {
                DBResultRow lprow;
                if (lpres.GetRow(lprow)) {
                    lastPos.x = lprow.GetDouble(0); lastPos.y = lprow.GetDouble(1); lastPos.z = lprow.GetDouble(2);
                    if (lastPos.x != 0.0 || lastPos.z != 0.0) haveLastPos = true;
                }
            }
        }

        // Anchor on the system's first station.
        GPoint stationPos(0, 0, 0);
        uint32 anchorStationID = 0;
        {
            DBQueryResult sres;
            if (sDatabase.RunQuery(sres,
                "SELECT stationID, x, y, z FROM staStations"
                " WHERE solarSystemID = %u ORDER BY stationID LIMIT 1", solarSystemID))
            {
                DBResultRow srow;
                if (sres.GetRow(srow)) {
                    anchorStationID = srow.GetUInt(0);
                    stationPos.x = srow.GetDouble(1);
                    stationPos.y = srow.GetDouble(2);
                    stationPos.z = srow.GetDouble(3);
                }
            }
        }
        if (anchorStationID == 0) { resultMsg = "no station in system to warp to"; return false; }

        // Far spawn: 1.5 AU from station, IN-PLANE (same Y; random x/z angle).
        double angle = MakeRandomInt(0, 35999) / 36000.0 * 6.283185307179586;
        double dist = 1.5 * (double)ONE_AU_IN_METERS;
        GPoint spawnPos(stationPos.x + dist * cos(angle), stationPos.y, stationPos.z + dist * sin(angle));
        if (haveLastPos) spawnPos = lastPos;   // VEV: return to last in-space position

        // Presence: online + Local (no station guest).
        if (!IsOnline(charID))
            AddPhantomPlayer(charID);
        if (m_services != nullptr) {
            LSCService* lsc = m_services->Lookup<LSCService>("LSC");
            if (lsc != nullptr) {
                lsc->CreateSystemChannel(solarSystemID);
                LSCChannel* localChan = lsc->GetChannelByID((int32)solarSystemID);
                if (localChan != nullptr)
                    localChan->JoinChannelAsPhantom(charID, charName, corpID, allianceID, warFactionID, 0);
            }
        }

        SystemManager* pSystem = FindOrBootSystem(solarSystemID);
        if (pSystem == nullptr) { resultMsg = "failed to load solar system"; return false; }

        // Reposition an existing ball, else spawn a fresh one at the far point.
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            InventoryItemRef shipRef = sItemFactory.GetItemRef(shipID);
            if (shipRef.get() == nullptr) { resultMsg = "failed to load ship from ItemFactory"; return false; }
            shipRef->Move(solarSystemID, flagNone, true);
            shipRef->SetPosition(spawnPos);
            shipRef->SaveItem();
            FactionData fData = FactionData();
            fData.ownerID = charID;
            fData.corporationID = corpID;
            fData.allianceID = allianceID;
            fData.factionID = warFactionID;
            pAIShip = new AIShipSE(shipRef, *m_services, pSystem, fData,
                                   charID, charName.c_str(), securityRating, bounty);
            if (pAIShip == nullptr) { resultMsg = "failed to create AIShipSE"; return false; }
            pSystem->AddEntity(pAIShip, false);
            AddAIShip(charID, pAIShip);
        }
        pAIShip->DestinyMgr()->SetPosition(spawnPos);

        // Char location -> in space.
        DBerror err;
        sDatabase.RunQuery(err,
            "UPDATE chrCharacters SET stationID = 0, locationID = %u WHERE characterID = %u",
            solarSystemID, charID);

        // VEV login-location: warp in near where the pilot logged off — the nearest
        // warpable celestial (belt/planet/moon/station) to the spawn position. A 2D
        // grid needs a celestial anchor to render anything, so a raw deep-space
        // safe-spot shows NOTHING; landing at the closest celestial is both visible
        // and EVE-like. groupID: 7=planet 8=moon 9=belt 15=station.
        GPoint warpTarget = stationPos;
        {
            DBQueryResult cres;
            if (sDatabase.RunQuery(cres,
                "SELECT x, y, z FROM mapDenormalize "
                "WHERE solarSystemID = %u AND groupID IN (7,8,9,15) "
                "ORDER BY (POW(x-%f,2)+POW(z-%f,2)) ASC LIMIT 1",
                solarSystemID, spawnPos.x, spawnPos.z))
            {
                DBResultRow crow;
                if (cres.GetRow(crow)) {
                    warpTarget.x = crow.GetDouble(0);
                    warpTarget.y = crow.GetDouble(1);
                    warpTarget.z = crow.GetDouble(2);
                }
            }
        }
        pAIShip->DestinyMgr()->WarpTo(warpTarget, 10000);

        char buf[200];
        snprintf(buf, sizeof(buf), "login in space: %s warping in to station %u from 1.5 AU",
                 charName.c_str(), anchorStationID);
        resultMsg = buf;
        return true;
    }

    // --- login_docked: Make an AI character appear online (phantom session) ---
    // See A321 §4.7 — Full phantom presence: online status + Local chat + station guest list
    // params: {} (no extra params needed — charID identifies the character)
    if (cmd == "login_docked") {
        if (IsOnline(charID)) {
            resultMsg = "character already online";
            return false;
        }
        // Query character data needed for full presence
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT c.characterName, c.stationID, c.corporationID, c.solarSystemID,"
            " IFNULL(corp.allianceID,0), IFNULL(corp.warFactionID,0)"
            " FROM chrCharacters c"
            " LEFT JOIN crpCorporation corp ON corp.corporationID = c.corporationID"
            " WHERE c.characterID = %u", charID))
        {
            resultMsg = "DB query failed";
            return false;
        }
        DBResultRow row;
        if (!charRes.GetRow(row)) {
            resultMsg = "character not found";
            return false;
        }
        std::string charName = row.GetText(0);
        uint32 stationID = row.GetUInt(1);
        uint32 corpID = row.GetUInt(2);
        uint32 solarSystemID = row.GetUInt(3);
        uint32 allianceID = row.GetUInt(4);
        uint32 warFactionID = row.GetUInt(5);
        if (stationID == 0) {
            resultMsg = "character is not docked at a station";
            return false;
        }

        // 1. Mark as online (phantom set + DB)
        AddPhantomPlayer(charID);

        // 2. Join system Local chat channel
        if (m_services != nullptr && solarSystemID != 0) {
            LSCService* lsc = m_services->Lookup<LSCService>("LSC");
            if (lsc != nullptr) {
                // Ensure the system channel exists
                lsc->CreateSystemChannel(solarSystemID);
                LSCChannel* localChan = lsc->GetChannelByID((int32)solarSystemID);
                if (localChan != nullptr) {
                    localChan->JoinChannelAsPhantom(charID, charName, corpID, allianceID, warFactionID, 0);
                }
            }
        }

        // 3. Add to station guest list + notify existing guests
        StationItemRef sRef = GetStationByID(stationID);
        if (sRef.get() != nullptr) {
            sRef->AddPhantomGuest(charID);

            // Send OnCharNowInStation to all real clients at the station
            OnCharNowInStation ocnis;
                ocnis.charID = charID;
                ocnis.corpID = corpID;
                ocnis.allianceID = allianceID;
                ocnis.warFactionID = warFactionID;
            PyTuple* tmp = ocnis.Encode();

            std::vector<Client*> clients;
            sRef->GetGuestList(clients);
            for (auto cur : clients) {
                PySafeIncRef(tmp);
                cur->SendNotification("OnCharNowInStation", "stationid", &tmp);
            }
            PySafeDecRef(tmp);
        }

        char buf[200];
        snprintf(buf, sizeof(buf), "phantom login: %s (station %u, system %u, local+guests)", charName.c_str(), stationID, solarSystemID);
        resultMsg = buf;
        return true;
    }

    // --- logout_pilot: Remove phantom session (full presence teardown) ---
    // See A321 §4.7 — Reverses login_docked: Local chat, station guests, online status
    // params: {} (no extra params needed)
    if (cmd == "logout_pilot") {
        if (IsPhantomPlayer(charID)) {
            // VEV login-location: persist the ship's current in-space position so a
            // re-login (login_in_space) returns the pilot here, not to the station.
            {
                AIShipSE* pLogoutShip = FindAIShip(charID);
                if (pLogoutShip != nullptr && pLogoutShip->GetSelf().get() != nullptr) {
                    GPoint lp = pLogoutShip->GetPosition();
                    DBerror lperr;
                    sDatabase.RunQuery(lperr, "UPDATE entity SET x=%f, y=%f, z=%f WHERE itemID=%u",
                                       lp.x, lp.y, lp.z, pLogoutShip->GetSelf()->itemID());
                }
            }
            // Query character data for leave notifications
            DBQueryResult charRes;
            if (!sDatabase.RunQuery(charRes,
                "SELECT c.characterName, c.stationID, c.corporationID, c.solarSystemID,"
                " IFNULL(corp.allianceID,0), IFNULL(corp.warFactionID,0)"
                " FROM chrCharacters c"
                " LEFT JOIN crpCorporation corp ON corp.corporationID = c.corporationID"
                " WHERE c.characterID = %u", charID))
            {
                // Still remove even if query fails
                RemovePhantomPlayer(charID);
                resultMsg = "phantom removed (DB query for cleanup failed)";
                return true;
            }
            DBResultRow row;
            if (!charRes.GetRow(row)) {
                RemovePhantomPlayer(charID);
                resultMsg = "phantom removed (char not found for cleanup)";
                return true;
            }
            std::string charName = row.GetText(0);
            uint32 stationID = row.GetUInt(1);
            uint32 corpID = row.GetUInt(2);
            uint32 solarSystemID = row.GetUInt(3);
            uint32 allianceID = row.GetUInt(4);
            uint32 warFactionID = row.GetUInt(5);

            // 1. Leave system Local chat channel
            if (m_services != nullptr && solarSystemID != 0) {
                LSCService* lsc = m_services->Lookup<LSCService>("LSC");
                if (lsc != nullptr) {
                    LSCChannel* localChan = lsc->GetChannelByID((int32)solarSystemID);
                    if (localChan != nullptr) {
                        localChan->LeaveChannelAsPhantom(charID, charName, corpID, allianceID, warFactionID, 0);
                    }
                }
            }

            // 2. Remove from station guest list + notify remaining guests
            StationItemRef sRef = GetStationByID(stationID);
            if (sRef.get() != nullptr) {
                sRef->RemovePhantomGuest(charID);

                OnCharNoLongerInStation ocnis;
                    ocnis.charID = charID;
                    ocnis.corpID = corpID;
                    ocnis.allianceID = allianceID;
                    ocnis.factionID = warFactionID;
                PyTuple* tmp = ocnis.Encode();

                std::vector<Client*> clients;
                sRef->GetGuestList(clients);
                for (auto cur : clients) {
                    PySafeIncRef(tmp);
                    cur->SendNotification("OnCharNoLongerInStation", "stationid", &tmp);
                }
                PySafeDecRef(tmp);
            }

            // 3. Remove online status
            RemovePhantomPlayer(charID);
            resultMsg = "phantom session removed (local+guests cleaned)";
            return true;
        }
        // Also check if it's a real player (can't force-logout real connections)
        if (m_players.find(charID) != m_players.end()) {
            resultMsg = "cannot logout real player via command queue";
            return false;
        }
        resultMsg = "character not online";
        return false;
    }

    // --- get_ship_status: Return live in-memory state of the AI ship ---
    // See A321.1 §6.1 (OODA Loop — Observe phase)
    // params: {} (none needed — reads live state from DestinyManager)
    // Returns: position, speed, movement state, bubble info, target info
    if (cmd == "get_ship_status") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            // Check if docked (phantom but not in space)
            if (IsPhantomPlayer(charID)) {
                resultMsg = "docked";
                return true;
            }
            resultMsg = "not online";
            return false;
        }

        DestinyManager* pDM = pAIShip->DestinyMgr();
        const GPoint& pos = pAIShip->GetPosition();
        // See A371 Bug22.3 — Diagnostic: log position source comparison.
        // Check if DestinyManager::m_position matches SystemEntity::GetPosition()
        if (pDM != nullptr) {
            const GPoint& dmPos = pDM->GetPosition();
            sLog.Cyan("get_ship_status", "%s(%u): SE.pos=(%.0f,%.0f,%.0f) DM.pos=(%.0f,%.0f,%.0f) match=%s",
                pAIShip->GetName(), pAIShip->GetID(),
                pos.x, pos.y, pos.z, dmPos.x, dmPos.y, dmPos.z,
                (fabs(pos.x - dmPos.x) < 1.0 && fabs(pos.y - dmPos.y) < 1.0 && fabs(pos.z - dmPos.z) < 1.0) ? "YES" : "NO");
        }
        const char* stateName = "unknown";
        uint8 mode = pDM ? pDM->GetState() : 255;
        switch (mode) {
            case 0: stateName = "goto"; break;
            case 1: stateName = "follow"; break;
            case 2: stateName = "stop"; break;
            case 3: stateName = pDM->IsWarping() ? "warping" : "aligning"; break;
            case 4: stateName = "orbit"; break;
            default: stateName = "unknown"; break;
        }

        float speed = pDM ? pDM->GetSpeed() : 0.0f;
        uint16 bubbleID = pAIShip->SysBubble() ? pAIShip->SysBubble()->GetID() : 0;
        bool isBelt = pAIShip->SysBubble() ? pAIShip->SysBubble()->IsBelt() : false;

        // =================== VEV_OWN_HP ===================
        // A phantom could not read its OWN HP anywhere: this verb had no HP
        // fields and get_overview SKIPS self -- ratting's hull bail was dead
        // since birth and defense v0 had to read its tank from rep responses
        // (found 2026-06-12). Same attr math as the overview's HP block.
        float vevSh = -1.0f, vevAr = -1.0f, vevHu = -1.0f;
        {
            InventoryItemRef vevSelf = pAIShip->GetSelf();
            if (vevSelf.get() != nullptr) {
                float sCap = vevSelf->GetAttribute(AttrShieldCapacity).get_float();
                float sChg = vevSelf->GetAttribute(AttrShieldCharge).get_float();
                float aMax = vevSelf->GetAttribute(AttrArmorHP).get_float();
                float aDmg = vevSelf->GetAttribute(AttrArmorDamage).get_float();
                float hMax = vevSelf->GetAttribute(AttrHP).get_float();
                float hDmg = vevSelf->GetAttribute(AttrDamage).get_float();
                vevSh = (sCap > 0) ? (sChg / sCap * 100.0f) : 0.0f;
                vevAr = (aMax > 0) ? ((1.0f - aDmg / aMax) * 100.0f) : 0.0f;
                vevHu = (hMax > 0) ? ((1.0f - hDmg / hMax) * 100.0f) : 0.0f;
            }
        }
        // =================== end VEV_OWN_HP ===================
        // VEV_OWN_CAP: capacitor % — 'can I run my guns/web/AB/prop?' is core
        // combat state. Same self-attr read as the activate_module cap gate.
        float vevCap = -1.0f;
        {
            InventoryItemRef vevSelf2 = pAIShip->GetSelf();
            if (vevSelf2.get() != nullptr) {
                float cCap = vevSelf2->GetAttribute(AttrCapacitorCapacity).get_float();
                float cChg = vevSelf2->HasAttribute(AttrCapacitorCharge)
                             ? vevSelf2->GetAttribute(AttrCapacitorCharge).get_float() : cCap;
                vevCap = (cCap > 0) ? (cChg / cCap * 100.0f) : 0.0f;
            }
        }
        char buf[512];
        snprintf(buf, sizeof(buf),
            "pos=(%.0f, %.0f, %.0f) speed=%.1f state=%s bubble=%u isBelt=%s sh=%.0f ar=%.0f hu=%.0f cap=%.0f",
            pos.x, pos.y, pos.z, speed, stateName, bubbleID, isBelt ? "yes" : "no",
            vevSh, vevAr, vevHu, vevCap);
        resultMsg = buf;
        return true;
    }

    // --- get_overview: Return all entities in the AI ship's bubble (like D-scan at 0) ---
    // See A321.1 §6.1 (OODA Loop — Observe phase)
    // params: {} (none needed — reads bubble contents)
    // Returns: list of nearby entities with type, distance, name, ID
    if (cmd == "get_overview") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space";
            return false;
        }

        SystemBubble* pBubble = pAIShip->SysBubble();
        if (pBubble == nullptr) {
            resultMsg = "AI ship has no bubble";
            return false;
        }

        sLog.Cyan("get_overview", "AIShip %s(%u) in bubbleID=%u pos=(%.0f,%.0f,%.0f)",
            pAIShip->GetName(), pAIShip->GetID(), pBubble->GetID(),
            pAIShip->GetPosition().x, pAIShip->GetPosition().y, pAIShip->GetPosition().z);

        std::map<uint32, SystemEntity*> entities;
        pBubble->GetEntities(entities);

        // Also fetch static entities (asteroids, stations, gates) — they live in m_entities, not m_dynamicEntities
        std::map<uint32, SystemEntity*> statics;
        pBubble->GetStaticEntities(statics);
        for (auto& kv : statics)
            entities.emplace(kv.first, kv.second);

        // VEV_XPL_OVERVIEW_CANS: relic/data site cans spawn into the system but can land
        // in a bubble the warp-in pilot never joins (orphaned site pocket). GridStreamer
        // already streams group-306 cans system-wide, so the CCTV shows the site while the
        // pilot's bubble does not -- the explorer then can't approach what it sees. Mirror
        // GridStreamer: pull nearby group-306 Spawn Containers from the system manager so
        // the AI's overview matches what is actually rendered at the site it warped to.
        SystemManager* pSM = pAIShip->SystemMgr();
        if (pSM != nullptr) {
            for (auto& kv : pSM->GetEntities()) {
                SystemEntity* cse = kv.second;
                if (cse == nullptr || cse->GetGroupID() != 306) continue;
                if (entities.find(kv.first) != entities.end()) continue;
                if (pAIShip->DistanceTo2(cse) <= 250000.0)  // site pocket ~12km; 250km slack
                    entities.emplace(kv.first, cse);
            }
        }

        sLog.Cyan("get_overview", "GetEntities returned %zu dynamic + %zu static = %zu total from bubble %u",
            entities.size() - statics.size(), statics.size(), entities.size(), pBubble->GetID());

        std::string result;
        int count = 0;
        for (auto& kv : entities) {
            SystemEntity* se = kv.second;
            if (se == nullptr || se == pAIShip)
                continue;

            const char* category = "unknown";
            if (se->IsAsteroidSE())   category = "asteroid";
            else if (se->IsNPCSE())    category = "npc";
            else if (se->IsAIShipSE()) category = "ai_ship";
            else if (se->IsShipSE())   category = "player_ship";
            else if (se->IsWreckSE())  category = "wreck";
            else if (se->IsContainerSE()) category = "container";
            else if (se->IsStationSE()) category = "station";
            else if (se->IsDroneSE())  category = "drone";  // VEV_DRONE
            else if (se->IsGateSE())   category = "gate";
            else if (se->GetGroupID() == 306) category = "container";  // VEV_XPL: relic/data site cans (any SE class, matches GridStreamer)
            else continue;  // skip celestials, belts, etc.

            // See A371 Bug22.3 — DistanceTo2 returns actual distance (not squared).
            // The "2" means "between 2 entities," NOT "squared." No sqrt needed.
            double dist = pAIShip->DistanceTo2(se);

            // See A371 Bug22.3 — Diagnostic: log per-entity position during get_overview
            // to trace server-side position discrepancy between get_ship_status and get_overview.
            if (se->IsShipSE() || se->IsAIShipSE()) {
                const GPoint& myPos = pAIShip->GetPosition();
                const GPoint& sePos = se->GetPosition();
                sLog.Cyan("get_overview", "  entity %s(%u): myPos=(%.0f,%.0f,%.0f) sePos=(%.0f,%.0f,%.0f) dist=%.0f",
                    se->GetName(), se->GetID(), myPos.x, myPos.y, myPos.z, sePos.x, sePos.y, sePos.z, dist);
            }

            // See A371 Bug19 — Battle telemetry: include HP percentages for combat entities.
            // Format: id|name|typeID|category|distance|shieldPct|armorPct|hullPct
            // Non-combat entities (asteroids, stations, etc.) get -1|-1|-1 for HP fields.
            float shieldPct = -1.0f, armorPct = -1.0f, hullPct = -1.0f;
            if (se->IsNPCSE() || se->IsShipSE() || se->IsAIShipSE()) {
                InventoryItemRef item = se->GetSelf();
                if (item.get() != nullptr) {
                    float shieldCap = item->GetAttribute(AttrShieldCapacity).get_float();
                    float shieldChg = item->GetAttribute(AttrShieldCharge).get_float();
                    float armorMax  = item->GetAttribute(AttrArmorHP).get_float();
                    float armorDmg  = item->GetAttribute(AttrArmorDamage).get_float();
                    float hullMax   = item->GetAttribute(AttrHP).get_float();
                    float hullDmg   = item->GetAttribute(AttrDamage).get_float();
                    shieldPct = (shieldCap > 0) ? (shieldChg / shieldCap * 100.0f) : 0.0f;
                    armorPct  = (armorMax > 0)  ? ((1.0f - armorDmg / armorMax) * 100.0f) : 0.0f;
                    hullPct   = (hullMax > 0)   ? ((1.0f - hullDmg / hullMax) * 100.0f) : 0.0f;
                }
            }

            char entry[512];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%s|%.0f|%.0f|%.0f|%.0f;",
                     se->GetID(), se->GetName(), se->GetTypeID(), category, dist,
                     shieldPct, armorPct, hullPct);
            result += entry;
            count++;
            if (count >= 50)  // cap at 50 entries
                break;
        }

        if (result.empty()) {
            resultMsg = "bubble empty (no interesting entities)";
        } else {
            char header[64];
            snprintf(header, sizeof(header), "%d entities: ", count);
            resultMsg = header + result;
        }
        return true;
    }

    // ========================================================================
    // INVENTORY MANAGEMENT PRIMITIVES
    // See A331 §3.5 (inventory_management skillset)
    // See A335 (migration from SQL fuckery to C++ bridge)
    // ========================================================================

    // --- get_cargo: Return items in the AI ship's cargo hold and ore hold ---
    // See A331 §3.5 (inventory_management — perception)
    // params: {} (none needed)
    // Returns: list of items with itemID|name|typeID|qty|volume per item
    // Works both in space (via AIShipSE) and docked (via sItemFactory ship lookup)
    if (cmd == "get_cargo") {
        InventoryItemRef shipRef;
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip != nullptr) {
            shipRef = pAIShip->GetSelf();
        } else if (IsPhantomPlayer(charID)) {
            // Docked — look up shipID from DB
            DBQueryResult charRes;
            if (!sDatabase.RunQuery(charRes,
                "SELECT shipID FROM chrCharacters WHERE characterID = %u", charID)) {
                resultMsg = "failed to query character ship";
                return false;
            }
            DBResultRow row;
            if (!charRes.GetRow(row)) {
                resultMsg = "character not found";
                return false;
            }
            uint32 shipID = row.GetUInt(0);
            shipRef = sItemFactory.GetItemRef(shipID);
        } else {
            resultMsg = "character not online";
            return false;
        }

        if (shipRef.get() == nullptr) {
            resultMsg = "ship not found";
            return false;
        }

        Inventory* inv = shipRef->GetMyInventory();
        if (inv == nullptr) {
            resultMsg = "ship has no inventory";
            return false;
        }

        // Check both cargo hold and ore hold
        std::vector<InventoryItemRef> items;
        inv->GetItemsByFlag(flagCargoHold, items);
        if (shipRef->HasAttribute(AttrOreHoldCapacity))
            inv->GetItemsByFlag(flagOreHold, items);

        float cargoRemaining = inv->GetRemainingCapacity(flagCargoHold);
        float oreRemaining = 0.0f;
        if (shipRef->HasAttribute(AttrOreHoldCapacity))
            oreRemaining = inv->GetRemainingCapacity(flagOreHold);

        std::string result;
        int count = 0;
        for (auto& iRef : items) {
            if (iRef.get() == nullptr) continue;
            float vol = iRef->HasAttribute(AttrVolume) ? iRef->GetAttribute(AttrVolume).get_float() : 0.0f;
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%u|%.2f|%s;",
                     iRef->itemID(), iRef->name(), iRef->typeID(),
                     (uint32)iRef->quantity(), vol,
                     (iRef->flag() == flagOreHold) ? "ore_hold" : "cargo");
            result += entry;
            count++;
        }

        char header[128];
        snprintf(header, sizeof(header), "%d items (cargo_free=%.0f ore_free=%.0f): ",
                 count, cargoRemaining, oreRemaining);
        resultMsg = header + result;
        return true;
    }

    // --- get_hangar: Return items in the character's station hangar ---
    // See A331 §3.5 (inventory_management — perception)
    // params: {} (none needed — reads current station from character data)
    // Returns: list of items in station hangar belonging to this character
    // Only works when docked
    if (cmd == "get_hangar") {
        if (!IsPhantomPlayer(charID) && FindAIShip(charID) == nullptr) {
            resultMsg = "character not online";
            return false;
        }

        // Get station ID from character
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 stationID = charRow.GetUInt(0);
        if (stationID == 0) {
            resultMsg = "character is not docked (stationID=0)";
            return false;
        }

        // Query items in station hangar belonging to this character
        DBQueryResult itemRes;
        if (!sDatabase.RunQuery(itemRes,
            "SELECT e.itemID, e.typeID, e.quantity, e.flag, IFNULL(t.typeName, 'unknown')"
            " FROM entity e"
            " LEFT JOIN invTypes t ON t.typeID = e.typeID"
            " WHERE e.locationID = %u AND e.ownerID = %u AND e.flag = %u",
            stationID, charID, (uint32)flagHangar)) {
            resultMsg = "failed to query hangar";
            return false;
        }

        std::string result;
        int count = 0;
        DBResultRow row;
        while (itemRes.GetRow(row)) {
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%u;",
                     row.GetUInt(0), row.GetText(4), row.GetUInt(1), row.GetUInt(2));
            result += entry;
            count++;
            if (count >= 100) break;
        }

        char header[64];
        snprintf(header, sizeof(header), "%d hangar items at station %u: ", count, stationID);
        resultMsg = header + result;
        return true;
    }

    // --- move_item: Transfer an item between ship hold and station hangar ---
    // See A331 §3.5 (inventory_management — action)
    // params: {"itemID": 140000999, "to": "hangar"} or {"itemID": 140000999, "to": "cargo"}
    //   itemID = the item to move
    //   to = "hangar" (ship→station) or "cargo" (station→ship) or "ore_hold" (station→ore hold)
    if (cmd == "move_item") {
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "must be docked to move items";
            return false;
        }

        // Parse params
        std::string p(params);
        size_t idPos = p.find("\"itemID\"");
        size_t toPos = p.find("\"to\"");
        if (idPos == std::string::npos || toPos == std::string::npos) {
            resultMsg = "missing 'itemID' or 'to' in params";
            return false;
        }
        size_t colonPos = p.find(":", idPos);
        uint32 itemID = (uint32)atol(p.c_str() + colonPos + 1);
        size_t toColon = p.find(":", toPos);
        size_t toQuote1 = p.find("\"", toColon + 1);
        size_t toQuote2 = p.find("\"", toQuote1 + 1);
        std::string toTarget = p.substr(toQuote1 + 1, toQuote2 - toQuote1 - 1);

        if (itemID == 0) {
            resultMsg = "invalid itemID";
            return false;
        }

        // Get character's station and ship
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID, shipID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 stationID = charRow.GetUInt(0);
        uint32 shipID = charRow.GetUInt(1);
        if (stationID == 0) {
            resultMsg = "not docked";
            return false;
        }

        // Load the item
        InventoryItemRef iRef = sItemFactory.GetItemRef(itemID);
        if (iRef.get() == nullptr) {
            resultMsg = "item not found";
            return false;
        }

        // Verify ownership
        if (iRef->ownerID() != charID) {
            resultMsg = "item does not belong to this character";
            return false;
        }

        if (toTarget == "hangar") {
            // Move to station hangar
            iRef->Move(stationID, flagHangar, true);
            iRef->SaveItem();
            char buf[128];
            snprintf(buf, sizeof(buf), "moved %s (%u) to station hangar", iRef->name(), itemID);
            resultMsg = buf;
        } else if (toTarget == "cargo" || toTarget == "ore_hold") {
            // Move to ship hold
            EVEItemFlags targetFlag = flagCargoHold;
            if (toTarget == "ore_hold")
                targetFlag = flagOreHold;

            InventoryItemRef shipRef = sItemFactory.GetItemRef(shipID);
            if (shipRef.get() == nullptr) {
                resultMsg = "ship not found";
                return false;
            }

            // Check capacity
            Inventory* shipInv = shipRef->GetMyInventory();
            if (shipInv == nullptr) {
                resultMsg = "ship has no inventory";
                return false;
            }
            float remaining = shipInv->GetRemainingCapacity(targetFlag);
            float itemVol = iRef->HasAttribute(AttrVolume) ? iRef->GetAttribute(AttrVolume).get_float() * iRef->quantity() : 0.0f;
            if (itemVol > remaining) {
                char buf[128];
                snprintf(buf, sizeof(buf), "not enough space: need %.0f, have %.0f", itemVol, remaining);
                resultMsg = buf;
                return false;
            }

            iRef->Move(shipID, targetFlag, true);
            iRef->SaveItem();
            char buf[128];
            snprintf(buf, sizeof(buf), "moved %s (%u) to ship %s", iRef->name(), itemID, toTarget.c_str());
            resultMsg = buf;
        } else {
            resultMsg = "invalid 'to' target — use 'hangar', 'cargo', or 'ore_hold'";
            return false;
        }
        return true;
    }

    // VEV_CORP_HANGAR (2026-06-14): withdraw a CORP-hangar stack into this pilot's
    // ship cargo. The item stays corp-owned (corp property in transit); the matching
    // deposit_corp_item drops it in the destination corp hangar. The phantom path for
    // evemu's Client-bound corp hangar — gives AI haulers real corp logistics.
    if (cmd == "load_corp_item") {
        if (!IsPhantomPlayer(charID)) { resultMsg = "must be docked to load corp cargo"; return false; }
        std::string p(params);
        size_t idPos = p.find("\"itemID\"");
        if (idPos == std::string::npos) { resultMsg = "missing 'itemID'"; return false; }
        size_t colon = p.find(":", idPos);
        uint32 itemID = (uint32)atol(p.c_str() + colon + 1);
        if (itemID == 0) { resultMsg = "invalid itemID"; return false; }
        DBQueryResult cr;
        if (!sDatabase.RunQuery(cr, "SELECT stationID, shipID, corporationID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character"; return false;
        }
        DBResultRow crow;
        if (!cr.GetRow(crow)) { resultMsg = "character not found"; return false; }
        uint32 stationID = crow.GetUInt(0);
        uint32 shipID = crow.GetUInt(1);
        uint32 corpID = crow.GetUInt(2);
        if (stationID == 0) { resultMsg = "not docked"; return false; }
        InventoryItemRef iRef = sItemFactory.GetItemRef(itemID);
        if (iRef.get() == nullptr) { resultMsg = "corp item not found"; return false; }
        if (iRef->ownerID() != corpID) { resultMsg = "that item is not owned by your corp"; return false; }
        if (iRef->locationID() != stationID) { resultMsg = "corp item is not at this station"; return false; }
        InventoryItemRef shipRef = sItemFactory.GetItemRef(shipID);
        if (shipRef.get() == nullptr) { resultMsg = "ship not found"; return false; }
        Inventory* shipInv = shipRef->GetMyInventory();
        if (shipInv == nullptr) { resultMsg = "ship has no inventory"; return false; }
        float remaining = shipInv->GetRemainingCapacity(flagCargoHold);
        float itemVol = iRef->HasAttribute(AttrVolume) ? iRef->GetAttribute(AttrVolume).get_float() * iRef->quantity() : 0.0f;
        if (itemVol > remaining) {
            char buf[128]; snprintf(buf, sizeof(buf), "not enough cargo space: need %.0f, have %.0f", itemVol, remaining);
            resultMsg = buf; return false;
        }
        iRef->Move(shipID, flagCargoHold, true);
        iRef->SaveItem();
        char buf[160]; snprintf(buf, sizeof(buf), "loaded corp %s (%u) into cargo", iRef->name(), itemID);
        resultMsg = buf;
        return true;
    }

    // VEV_CORP_HANGAR (2026-06-14): deposit an item into the corp hangar at the
    // CURRENT station — either a corp item carried in cargo (the delivery end of a
    // haul) or the pilot's OWN item (a member donating to the corp pool). Lands it
    // corp-owned, flagHangar, at the station.
    if (cmd == "deposit_corp_item") {
        if (!IsPhantomPlayer(charID)) { resultMsg = "must be docked to deposit"; return false; }
        std::string p(params);
        size_t idPos = p.find("\"itemID\"");
        if (idPos == std::string::npos) { resultMsg = "missing 'itemID'"; return false; }
        size_t colon = p.find(":", idPos);
        uint32 itemID = (uint32)atol(p.c_str() + colon + 1);
        if (itemID == 0) { resultMsg = "invalid itemID"; return false; }
        DBQueryResult cr;
        if (!sDatabase.RunQuery(cr, "SELECT stationID, corporationID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character"; return false;
        }
        DBResultRow crow;
        if (!cr.GetRow(crow)) { resultMsg = "character not found"; return false; }
        uint32 stationID = crow.GetUInt(0);
        uint32 corpID = crow.GetUInt(1);
        if (stationID == 0) { resultMsg = "not docked"; return false; }
        InventoryItemRef iRef = sItemFactory.GetItemRef(itemID);
        if (iRef.get() == nullptr) { resultMsg = "item not found"; return false; }
        if (iRef->ownerID() != corpID && iRef->ownerID() != charID) {
            resultMsg = "you cannot deposit that item"; return false;
        }
        if (iRef->ownerID() != corpID) { iRef->ChangeOwner(corpID, true); }
        iRef->Move(stationID, flagHangar, true);
        iRef->SaveItem();
        char buf[160]; snprintf(buf, sizeof(buf), "deposited %s (%u) to corp hangar", iRef->name(), itemID);
        resultMsg = buf;
        return true;
    }

    // --- reprocess: Refine ore into minerals at station ---
    // See A331 §3.5 (inventory_management — action)
    // params: {"itemID": 140000999}
    //   itemID = the ore item to reprocess (must be in station hangar or ship cargo)
    // Returns: list of minerals produced
    // Only works when docked
    // ── VEV_PI_ESTABLISH: establish a colony headlessly (charID-owned — the
    //    gateway/corp path; no Client* needed). Spawns the command-center item
    //    and records the colony (Colony::CreateCommandPin writes piCCPin +
    //    piPlanets + pins). Full build = follow-on pi_* verbs.
    //    params: {"systemID":N,"planetID":N,"ccTypeID":N,"lat":F,"lon":F}
    if (cmd == "pi_establish") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 {
            size_t kp = p.find(k); if (kp == std::string::npos) return 0;
            size_t c = p.find(":", kp); return (c == std::string::npos) ? 0 : (uint32)atol(p.c_str() + c + 1);
        };
        auto rdF = [&p](const char* k) -> double {
            size_t kp = p.find(k); if (kp == std::string::npos) return 0.0;
            size_t c = p.find(":", kp); return (c == std::string::npos) ? 0.0 : atof(p.c_str() + c + 1);
        };
        uint32 systemID = rdU("\"systemID\"");
        uint32 planetID = rdU("\"planetID\"");
        uint32 ccTypeID = rdU("\"ccTypeID\"");
        double lat = rdF("\"lat\""), lon = rdF("\"lon\"");
        if (systemID == 0 || planetID == 0 || ccTypeID == 0) {
            resultMsg = "pi_establish needs systemID, planetID, ccTypeID"; return false;
        }
        SystemManager* pSM = FindOrBootSystem(systemID);
        if (pSM == nullptr) { resultMsg = "pi_establish: system not found"; return false; }
        SystemEntity* pSE = pSM->GetSE(planetID);
        if (pSE == nullptr) { resultMsg = "pi_establish: planet not instantiated in system"; return false; }
        PlanetSE* pPlanet = pSE->GetPlanetSE();
        if (pPlanet == nullptr) { resultMsg = "pi_establish: that entity is not a planet"; return false; }
        Colony* colony = pPlanet->GetColony(charID);
        if (colony->HasColony()) { resultMsg = "pi_establish: you already have a colony on this planet"; return false; }
        ItemData ccData(ccTypeID, charID, planetID, flagNone, 1);
        InventoryItemRef ccRef = sItemFactory.SpawnItem(ccData);
        if (ccRef.get() == nullptr) { resultMsg = "pi_establish: failed to spawn command center item"; return false; }
        colony->CreateCommandPin(ccRef->itemID(), ccTypeID, lat, lon);
        colony->Save();
        char buf[160];
        snprintf(buf, sizeof(buf), "colony established on planet %u (cc item %u, type %u)",
                 planetID, ccRef->itemID(), ccTypeID);
        resultMsg = buf;
        return true;
    }
    // ── VEV_PI_LOGI1A: atomically build a whole colony from a designed SPEC.
    //    The CEO designs the colony (pins/links/routes/recipes) with NO live CC;
    //    on CC delivery (provision job) the spec is replayed here in one pass,
    //    mapping client temp-ids -> real engine pin ids. params = the spec JSON:
    //    {systemID,planetID,cc:{tmp,typeID,lat,lon},
    //     pins:[{tmp,typeID,lat,lon,schematicID?,ecu?{resourceTypeID,qtyPerCycle,cycleHours,numCycles}}],
    //     links:[{src,dest}], routes:[{src,dest,typeID,qty}]}
    if (cmd == "pi_commit_spec") {
        nlohmann::json spec;
        try { spec = nlohmann::json::parse(params); }
        catch (...) { resultMsg = "pi_commit_spec: bad JSON spec"; return false; }
        uint32 systemID = spec.value("systemID", 0u);
        uint32 planetID = spec.value("planetID", 0u);
        if (systemID == 0 || planetID == 0 || !spec.contains("cc")) { resultMsg = "pi_commit_spec needs systemID, planetID, cc"; return false; }
        SystemManager* pSM = FindOrBootSystem(systemID);
        if (pSM == nullptr) { resultMsg = "pi_commit_spec: system not found"; return false; }
        SystemEntity* pSE = pSM->GetSE(planetID);
        if (pSE == nullptr) { resultMsg = "pi_commit_spec: planet not instantiated"; return false; }
        PlanetSE* pPlanet = pSE->GetPlanetSE();
        if (pPlanet == nullptr) { resultMsg = "pi_commit_spec: not a planet"; return false; }
        Colony* colony = pPlanet->GetColony(charID);
        if (colony->HasColony()) { resultMsg = "pi_commit_spec: colony already exists here"; return false; }

        std::map<int, uint32> idmap;
        auto& cc = spec["cc"];
        uint32 ccTypeID = cc.value("typeID", 0u);
        double clat = cc.value("lat", 0.0), clon = cc.value("lon", 0.0);
        if (ccTypeID == 0) { resultMsg = "pi_commit_spec: cc.typeID required"; return false; }
        uint32 ccItemID = 0;
        bool fromCustoms = spec.value("fromCustoms", 0) != 0;   // VEV_POCO1: real ferry vs spawn-teleport
        if (fromCustoms) {
            // consume the command center the logi pilot delivered to the planet's customs office.
            uint32 coID = 0;
            { DBQueryResult cr; DBResultRow crow; if (sDatabase.RunQuery(cr, "SELECT e.itemID FROM entity e JOIN invTypes t ON t.typeID=e.typeID WHERE t.groupID=1025 AND e.customInfo='%u' LIMIT 1", planetID) && cr.GetRow(crow)) coID = crow.GetUInt(0); }
            if (coID == 0) { resultMsg = "pi_commit_spec: no customs office at this planet (deliver via the CO)"; return false; }
            { DBQueryResult ir; DBResultRow irow; if (sDatabase.RunQuery(ir, "SELECT itemID FROM entity WHERE locationID=%u AND ownerID=%u AND typeID=%u LIMIT 1", coID, charID, ccTypeID) && ir.GetRow(irow)) ccItemID = irow.GetUInt(0); }
            if (ccItemID == 0) { resultMsg = "pi_commit_spec: command center not yet delivered to the customs office"; return false; }
            InventoryItemRef ccRef = sItemFactory.GetItemRef(ccItemID);
            if (ccRef.get() == nullptr) { resultMsg = "pi_commit_spec: CC item missing"; return false; }
            ccRef->Move(planetID, flagNone, true);   // CO hangar -> planet surface
        } else {
            ItemData ccData(ccTypeID, charID, planetID, flagNone, 1);
            InventoryItemRef ccRef = sItemFactory.SpawnItem(ccData);
            if (ccRef.get() == nullptr) { resultMsg = "pi_commit_spec: failed to spawn CC item"; return false; }
            ccItemID = ccRef->itemID();
        }
        colony->CreateCommandPin(ccItemID, ccTypeID, clat, clon);
        idmap[cc.value("tmp", 0)] = ccItemID;

        int nPins = 0, nLinks = 0, nRoutes = 0;
        if (spec.contains("pins")) for (auto& pn : spec["pins"]) {
            uint32 typeID = pn.value("typeID", 0u);
            if (typeID == 0) continue;
            auto* itype = sItemFactory.GetType(typeID);
            if (itype == nullptr) continue;
            double lat = pn.value("lat", 0.0), lon = pn.value("lon", 0.0);
            colony->CreatePin(itype->groupID(), 0, typeID, lat, lon);
            uint32 realID = colony->GetLastCreatedPin();
            idmap[pn.value("tmp", -1)] = realID;
            nPins++;
            if (pn.contains("schematicID")) {
                uint32 schem = pn.value("schematicID", 0u);
                if (schem) colony->SetSchematic(realID, (uint8)schem);
            }
            if (pn.contains("ecu")) {
                auto& e = pn["ecu"];
                uint32 res = e.value("resourceTypeID", 0u);
                uint32 qpc = e.value("qtyPerCycle", 3000u);
                uint32 ncy = e.value("numCycles", 24u);
                double chr = e.value("cycleHours", 1.0);
                if (res) { colony->AddExtractorHead(realID, 0, lat, lon); colony->SetProgramResults(realID, (uint16)res, (uint16)ncy, 0.5f, (float)chr, qpc); }
            }
        }
        if (spec.contains("links")) for (auto& lk : spec["links"]) {
            int s = lk.value("src", -1), d = lk.value("dest", -1);
            if (idmap.count(s) && idmap.count(d)) { colony->CreateLink(idmap[s], idmap[d], 0); nLinks++; }
        }
        if (spec.contains("routes")) for (auto& rt : spec["routes"]) {
            int s = rt.value("src", -1), d = rt.value("dest", -1);
            uint32 typeID = rt.value("typeID", 0u), qty = rt.value("qty", 0u);
            if (typeID && idmap.count(s) && idmap.count(d)) {
                PyList* path = new PyList();
                path->AddItem(new PyInt(idmap[s]));
                path->AddItem(new PyInt(idmap[d]));
                colony->CreateRoute(0, typeID, qty, path);
                PyDecRef(path);
                nRoutes++;
            }
        }
        colony->Save();
        char buf[200];
        snprintf(buf, sizeof(buf), "committed spec on planet %u: cc %u, %d pins, %d links, %d routes", planetID, ccItemID, nPins, nLinks, nRoutes);
        resultMsg = buf;
        return true;
    }
    // ── VEV_PI_BUILD: build one colony structure (processor/storage/launchpad/
    //    ECU) on an established colony. Real pin id is RETURNED in result_msg so
    //    the caller can link/route/schematic it. params:
    //    {"systemID":N,"planetID":N,"typeID":N,"lat":F,"lon":F}
    if (cmd == "pi_build_pin") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 {
            size_t kp = p.find(k); if (kp == std::string::npos) return 0;
            size_t c = p.find(":", kp); return (c == std::string::npos) ? 0 : (uint32)atol(p.c_str() + c + 1); };
        auto rdF = [&p](const char* k) -> double {
            size_t kp = p.find(k); if (kp == std::string::npos) return 0.0;
            size_t c = p.find(":", kp); return (c == std::string::npos) ? 0.0 : atof(p.c_str() + c + 1); };
        uint32 systemID = rdU("\"systemID\""), planetID = rdU("\"planetID\""), typeID = rdU("\"typeID\"");
        double lat = rdF("\"lat\""), lon = rdF("\"lon\"");
        if (systemID == 0 || planetID == 0 || typeID == 0) { resultMsg = "pi_build_pin needs systemID, planetID, typeID"; return false; }
        SystemManager* pSM = FindOrBootSystem(systemID);
        if (pSM == nullptr) { resultMsg = "pi_build_pin: system not found"; return false; }
        SystemEntity* pSE = pSM->GetSE(planetID);
        if (pSE == nullptr) { resultMsg = "pi_build_pin: planet not instantiated"; return false; }
        PlanetSE* pPlanet = pSE->GetPlanetSE();
        if (pPlanet == nullptr) { resultMsg = "pi_build_pin: not a planet"; return false; }
        Colony* colony = pPlanet->GetColony(charID);
        if (!colony->HasColony()) { resultMsg = "pi_build_pin: establish a command center first"; return false; }
        auto* itype = sItemFactory.GetType(typeID);
        if (itype == nullptr) { resultMsg = "pi_build_pin: unknown typeID"; return false; }
        uint32 groupID = itype->groupID();
        colony->CreatePin(groupID, 0, typeID, lat, lon);
        uint32 realID = colony->GetLastCreatedPin();
        colony->Save();
        char buf[160]; snprintf(buf, sizeof(buf), "built pin %u (type %u, group %u)", realID, typeID, groupID);
        resultMsg = buf; return true;
    }
    // ── VEV_PI_BUILD: set a processor's schematic. params:
    //    {"systemID":N,"planetID":N,"pinID":N,"schematicID":N}
    if (cmd == "pi_schematic") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 {
            size_t kp = p.find(k); if (kp == std::string::npos) return 0;
            size_t c = p.find(":", kp); return (c == std::string::npos) ? 0 : (uint32)atol(p.c_str() + c + 1); };
        uint32 systemID = rdU("\"systemID\""), planetID = rdU("\"planetID\"");
        uint32 pinID = rdU("\"pinID\""), schematicID = rdU("\"schematicID\"");
        if (systemID == 0 || planetID == 0 || pinID == 0) { resultMsg = "pi_schematic needs systemID, planetID, pinID"; return false; }
        SystemManager* pSM = FindOrBootSystem(systemID);
        if (pSM == nullptr) { resultMsg = "pi_schematic: system not found"; return false; }
        SystemEntity* pSE = pSM->GetSE(planetID);
        if (pSE == nullptr) { resultMsg = "pi_schematic: planet not instantiated"; return false; }
        PlanetSE* pPlanet = pSE->GetPlanetSE();
        if (pPlanet == nullptr) { resultMsg = "pi_schematic: not a planet"; return false; }
        Colony* colony = pPlanet->GetColony(charID);
        if (!colony->HasColony()) { resultMsg = "pi_schematic: no colony here"; return false; }
        colony->SetSchematic(pinID, (uint8)schematicID);
        colony->Save();
        char buf[120]; snprintf(buf, sizeof(buf), "schematic %u set on pin %u", schematicID, pinID);
        resultMsg = buf; return true;
    }
    // ── VEV_PI_BUILD: link two pins. params: {"systemID","planetID","src","dest"}
    if (cmd == "pi_link") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 {
            size_t kp = p.find(k); if (kp == std::string::npos) return 0;
            size_t c = p.find(":", kp); return (c == std::string::npos) ? 0 : (uint32)atol(p.c_str() + c + 1); };
        uint32 systemID = rdU("\"systemID\""), planetID = rdU("\"planetID\"");
        uint32 srcPin = rdU("\"src\""), destPin = rdU("\"dest\"");
        if (systemID == 0 || planetID == 0 || srcPin == 0 || destPin == 0) { resultMsg = "pi_link needs systemID, planetID, src, dest"; return false; }
        SystemManager* pSM = FindOrBootSystem(systemID);
        if (pSM == nullptr) { resultMsg = "pi_link: system not found"; return false; }
        SystemEntity* pSE = pSM->GetSE(planetID);
        if (pSE == nullptr) { resultMsg = "pi_link: planet not instantiated"; return false; }
        PlanetSE* pPlanet = pSE->GetPlanetSE();
        if (pPlanet == nullptr) { resultMsg = "pi_link: not a planet"; return false; }
        Colony* colony = pPlanet->GetColony(charID);
        if (!colony->HasColony()) { resultMsg = "pi_link: no colony here"; return false; }
        colony->CreateLink(srcPin, destPin, 0);
        colony->Save();
        char buf[128]; snprintf(buf, sizeof(buf), "linked pin %u <-> %u", srcPin, destPin);
        resultMsg = buf; return true;
    }
    // ── VEV_PI_BUILD: route a commodity src->dest. CreateRoute also moves any
    //    matching material the source already holds (immediate). params:
    //    {"systemID","planetID","src","dest","typeID","qty"}
    if (cmd == "pi_route") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 {
            size_t kp = p.find(k); if (kp == std::string::npos) return 0;
            size_t c = p.find(":", kp); return (c == std::string::npos) ? 0 : (uint32)atol(p.c_str() + c + 1); };
        uint32 systemID = rdU("\"systemID\""), planetID = rdU("\"planetID\"");
        uint32 srcPin = rdU("\"src\""), destPin = rdU("\"dest\"");
        uint32 typeID = rdU("\"typeID\""), qty = rdU("\"qty\"");
        if (systemID == 0 || planetID == 0 || srcPin == 0 || destPin == 0 || typeID == 0) { resultMsg = "pi_route needs systemID, planetID, src, dest, typeID"; return false; }
        SystemManager* pSM = FindOrBootSystem(systemID);
        if (pSM == nullptr) { resultMsg = "pi_route: system not found"; return false; }
        SystemEntity* pSE = pSM->GetSE(planetID);
        if (pSE == nullptr) { resultMsg = "pi_route: planet not instantiated"; return false; }
        PlanetSE* pPlanet = pSE->GetPlanetSE();
        if (pPlanet == nullptr) { resultMsg = "pi_route: not a planet"; return false; }
        Colony* colony = pPlanet->GetColony(charID);
        if (!colony->HasColony()) { resultMsg = "pi_route: no colony here"; return false; }
        PyList* path = new PyList();
        path->AddItem(new PyInt(srcPin));
        path->AddItem(new PyInt(destPin));
        colony->CreateRoute(0, typeID, qty, path);
        PyDecRef(path);
        colony->Save();
        char buf[160]; snprintf(buf, sizeof(buf), "routed %u x type %u: pin %u -> %u", qty, typeID, srcPin, destPin);
        resultMsg = buf; return true;
    }
    // ── VEV_PI_ECU: install an extraction program on an ECU. params:
    //    {systemID,planetID,ecuPinID,resourceTypeID,qtyPerCycle,cycleHours,numCycles,lat,lon}
    if (cmd == "pi_program") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        auto rdF = [&p](const char* k) -> double { size_t kp=p.find(k); if(kp==std::string::npos) return 0.0; size_t c=p.find(":",kp); return (c==std::string::npos)?0.0:atof(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), planetID=rdU("\"planetID\""), ecuID=rdU("\"ecuPinID\"");
        uint32 resTypeID=rdU("\"resourceTypeID\""), qtyPerCycle=rdU("\"qtyPerCycle\""), numCycles=rdU("\"numCycles\"");
        double cycleHours=rdF("\"cycleHours\""), lat=rdF("\"lat\""), lon=rdF("\"lon\"");
        if (systemID==0||planetID==0||ecuID==0||resTypeID==0) { resultMsg="pi_program needs systemID, planetID, ecuPinID, resourceTypeID"; return false; }
        if (qtyPerCycle==0) qtyPerCycle=3000;
        if (numCycles==0) numCycles=24;
        if (cycleHours<=0) cycleHours=1.0;
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pi_program: system not found";return false;}
        SystemEntity* pSE=pSM->GetSE(planetID);
        if(!pSE){resultMsg="pi_program: planet not instantiated";return false;}
        PlanetSE* pPlanet=pSE->GetPlanetSE();
        if(!pPlanet){resultMsg="pi_program: not a planet";return false;}
        Colony* colony=pPlanet->GetColony(charID);
        if(!colony->HasColony()){resultMsg="pi_program: no colony here";return false;}
        colony->AddExtractorHead(ecuID, 0, lat, lon);
        colony->SetProgramResults(ecuID, (uint16)resTypeID, (uint16)numCycles, 0.5f, (float)cycleHours, qtyPerCycle);
        colony->Save();
        char buf[160]; snprintf(buf,sizeof(buf),"ECU %u program: type %u, %u/cycle, %u cycles", ecuID, resTypeID, qtyPerCycle, numCycles);
        resultMsg=buf; return true;
    }
    // ── VEV_PI_ECU: force N production cycles NOW (test). params: {systemID,planetID,cycles}
    // ── VEV_PI_REMOVE: tear down a structure / link / route (full edit-ability).
    if (cmd == "pi_remove_pin" || cmd == "pi_remove_link" || cmd == "pi_remove_route") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), planetID=rdU("\"planetID\"");
        if (systemID==0||planetID==0) { resultMsg=cmd+" needs systemID, planetID"; return false; }
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg=cmd+": system not found";return false;}
        SystemEntity* pSE=pSM->GetSE(planetID);
        if(!pSE){resultMsg=cmd+": planet not instantiated";return false;}
        PlanetSE* pPlanet=pSE->GetPlanetSE();
        if(!pPlanet){resultMsg=cmd+": not a planet";return false;}
        Colony* colony=pPlanet->GetColony(charID);
        if(!colony->HasColony()){resultMsg=cmd+": no colony here";return false;}
        char buf[96];
        if (cmd == "pi_remove_pin") {
            uint32 pinID=rdU("\"pinID\""); if(!pinID){resultMsg="pi_remove_pin needs pinID";return false;}
            colony->RemovePin(pinID); colony->Save();
            snprintf(buf,sizeof(buf),"removed pin %u",pinID);
        } else if (cmd == "pi_remove_link") {
            uint32 src=rdU("\"src\""), dest=rdU("\"dest\""); if(!src||!dest){resultMsg="pi_remove_link needs src, dest";return false;}
            colony->RemoveLink(src,dest); colony->Save();
            snprintf(buf,sizeof(buf),"removed link %u<->%u",src,dest);
        } else {
            uint32 routeID=rdU("\"routeID\""); if(!routeID){resultMsg="pi_remove_route needs routeID";return false;}
            colony->RemoveRoute((uint16)routeID); colony->Save();
            snprintf(buf,sizeof(buf),"removed route %u",routeID);
        }
        resultMsg=buf; return true;
    }
    // ── VEV_PI_LOGI2: haul a colony's produced goods to a corp hub (real items
    //    in the hangar) + charge the EVE-real customs export tax (ISK sink) + ledger.
    if (cmd == "pi_haul_export") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), planetID=rdU("\"planetID\""), hubStationID=rdU("\"hubStationID\"");
        if (systemID==0||planetID==0||hubStationID==0) { resultMsg="pi_haul_export needs systemID, planetID, hubStationID"; return false; }
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pi_haul_export: system not found";return false;}
        SystemEntity* pSE=pSM->GetSE(planetID);
        if(!pSE){resultMsg="pi_haul_export: planet not instantiated";return false;}
        PlanetSE* pPlanet=pSE->GetPlanetSE();
        if(!pPlanet){resultMsg="pi_haul_export: not a planet";return false;}
        Colony* colony=pPlanet->GetColony(charID);
        if(!colony->HasColony()){resultMsg="pi_haul_export: no colony here";return false;}
        double value=0, tax=0; int count=0; std::string manifest;
        colony->HaulExport(hubStationID, value, tax, count, manifest);
        if (count == 0) { resultMsg="pi_haul_export: nothing in storage/launchpad to haul"; return true; }
        DBerror err;
        sDatabase.RunQuery(err, "UPDATE chrCharacters SET balance = GREATEST(0, balance - %f) WHERE characterID = %u", tax, charID);
        sDatabase.RunQuery(err, "INSERT INTO vevPiLedger (corpID, charID, planetID, kind, valueIsk, taxIsk, units, ts) "
                                "SELECT COALESCE(corporationID,0), %u, %u, 'export', %f, %f, %d, UNIX_TIMESTAMP() FROM chrCharacters WHERE characterID=%u",
                           charID, planetID, value, tax, count, charID);
        char buf[200]; snprintf(buf, sizeof(buf), "hauled %d units (value %.0f ISK, export tax %.0f ISK) to station %u", count, value, tax, hubStationID);
        resultMsg=buf; return true;
    }
    // ── VEV_POCO0: spawn a customs office (POCO) for a planet if it lacks one.
    if (cmd == "pi_spawn_customs") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), planetID=rdU("\"planetID\"");
        if (systemID==0||planetID==0) { resultMsg="pi_spawn_customs needs systemID, planetID"; return false; }
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pi_spawn_customs: system not found";return false;}
        SystemEntity* pSE=pSM->GetSE(planetID);
        if(!pSE){resultMsg="pi_spawn_customs: planet not instantiated";return false;}
        PlanetSE* pPlanet=pSE->GetPlanetSE();
        if(!pPlanet){resultMsg="pi_spawn_customs: not a planet";return false;}
        if (pPlanet->HasCOSE()) { resultMsg="pi_spawn_customs: customs office already exists"; return true; }
        pPlanet->CreateCustomsOffice();
        if (!pPlanet->HasCOSE()) { resultMsg="pi_spawn_customs: spawn failed"; return false; }
        resultMsg="spawned customs office on planet"; return true;
    }
    // ── VEV_POCO1: deposit items into a planet's customs office (the delivery
    //    endpoint a logi pilot drops cargo into; import to surface happens on deploy).
    if (cmd == "pi_deposit_customs") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), planetID=rdU("\"planetID\""), typeID=rdU("\"typeID\""), qty=rdU("\"qty\"");
        if (systemID==0||planetID==0||typeID==0) { resultMsg="pi_deposit_customs needs systemID, planetID, typeID"; return false; }
        if (qty==0) qty=1;
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pi_deposit_customs: system not found";return false;}
        SystemEntity* pSE=pSM->GetSE(planetID);
        if(!pSE){resultMsg="pi_deposit_customs: planet not instantiated";return false;}
        PlanetSE* pPlanet=pSE->GetPlanetSE();
        if(!pPlanet){resultMsg="pi_deposit_customs: not a planet";return false;}
        if(!pPlanet->HasCOSE()){resultMsg="pi_deposit_customs: no customs office (spawn one first)";return false;}
        uint32 coID=0;
        { DBQueryResult cr; DBResultRow crow; if (sDatabase.RunQuery(cr, "SELECT e.itemID FROM entity e JOIN invTypes t ON t.typeID=e.typeID WHERE t.groupID=1025 AND e.customInfo='%u' LIMIT 1", planetID) && cr.GetRow(crow)) coID = crow.GetUInt(0); }
        if(coID==0){resultMsg="pi_deposit_customs: customs office id not found";return false;}
        ItemData idata(typeID, charID, coID, flagHangar, qty);
        InventoryItemRef iRef = sItemFactory.SpawnItem(idata);
        if (iRef.get()==nullptr){resultMsg="pi_deposit_customs: spawn failed";return false;}
        char buf[128]; snprintf(buf,sizeof(buf),"deposited %u x type %u into customs office %u", qty, typeID, coID);
        resultMsg=buf; return true;
    }
    // ── VEV_POS0: deploy + online a control tower at a moon (headless).
    if (cmd == "pos_deploy_tower") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), moonID=rdU("\"moonID\""), typeID=rdU("\"typeID\"");
        if (systemID==0||moonID==0) { resultMsg="pos_deploy_tower needs systemID, moonID"; return false; }
        if (typeID==0) typeID=16213;   // default: Caldari Control Tower
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pos_deploy_tower: system not found";return false;}
        if(pSM->GetSE(moonID)==nullptr){resultMsg="pos_deploy_tower: moon not instantiated";return false;}
        uint32 corpID=0;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && r.GetRow(row)) corpID = row.GetUInt(0); }
        uint32 towerID = pSM->DeployTower(moonID, typeID, charID, corpID);
        if (towerID==0) { resultMsg="pos_deploy_tower: deploy failed"; return false; }
        char buf[128]; snprintf(buf,sizeof(buf),"deployed + onlined tower %u (type %u) at moon %u", towerID, typeID, moonID);
        resultMsg=buf; return true;
    }
    // ── VEV_POS1: moon harvest — the tower pulls its moon's materials into its bay.
    // ── VEV_POS_ANCHOR: anchor a REAL POS module (silo 14343 / moon-harvesting array 16221 /
    //    reactor 16869) at a tower, inside its force-field bubble. A real, vulnerable structure in
    //    space — the eve-real infrastructure that must exist before production. params: {systemID,towerID,typeID}
    if (cmd == "pos_anchor") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), towerID=rdU("\"towerID\""), typeID=rdU("\"typeID\"");
        if (systemID==0||towerID==0||typeID==0) { resultMsg="pos_anchor needs systemID, towerID, typeID"; return false; }
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pos_anchor: system not found";return false;}
        int towerState=-1;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT state FROM posStructureData WHERE itemID=%u", towerID) && r.GetRow(row)) towerState=row.GetInt(0); }
        if (towerState < (int)EVEPOS::StructureState::Online) { resultMsg="pos_anchor: tower is not online"; return false; }
        uint32 corpID=0;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT corporationID FROM chrCharacters WHERE characterID=%u", charID) && r.GetRow(row)) corpID = row.GetUInt(0); }
        uint32 structID = pSM->AnchorPosStructure(towerID, typeID, charID, corpID);
        if (structID==0) { resultMsg="pos_anchor: anchor failed"; return false; }
        char buf[128]; snprintf(buf,sizeof(buf),"anchored structure %u (type %u) at tower %u", structID, typeID, towerID);
        resultMsg=buf; return true;
    }
    // ── VEV_POS_AUTOPROC: configure which reaction an anchored Reactor runs on its periodic tick.
    //    The reactor then auto-reacts (ReactorSE::Process) pulling from + depositing into its tower's
    //    Silo, hands-off. params: {systemID, structureID, reactionTypeID}  (reactionTypeID 0 = clear)
    if (cmd == "pos_set_reaction") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 structureID=rdU("\"structureID\""), reactionTypeID=rdU("\"reactionTypeID\"");
        if (structureID==0) { resultMsg="pos_set_reaction needs structureID"; return false; }
        if (reactionTypeID==0) {
            DBerror e; sDatabase.RunQuery(e, "DELETE FROM vevPosReactor WHERE structureID=%u", structureID);
            resultMsg="pos_set_reaction: cleared reaction"; return true;
        }
        // validate the reaction exists
        { DBQueryResult r; DBResultRow row; uint32 n=0; if (sDatabase.RunQuery(r, "SELECT COUNT(*) FROM invTypeReactions WHERE reactionTypeID=%u", reactionTypeID) && r.GetRow(row)) n=row.GetUInt(0); if (n==0) { resultMsg="pos_set_reaction: unknown reactionTypeID"; return false; } }
        DBerror e; sDatabase.RunQuery(e, "INSERT INTO vevPosReactor (structureID, reactionTypeID) VALUES (%u,%u) ON DUPLICATE KEY UPDATE reactionTypeID=%u", structureID, reactionTypeID, reactionTypeID);
        char buf[128]; snprintf(buf,sizeof(buf),"reactor %u set to reaction %u (auto-runs on tick)", structureID, reactionTypeID);
        resultMsg=buf; return true;
    }
    // ── VEV_POS_FUEL: re-online a tower that ran out of fuel (stock fuel blocks, then call this).
    if (cmd == "pos_online") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), towerID=rdU("\"towerID\"");
        if (systemID==0||towerID==0) { resultMsg="pos_online needs systemID, towerID"; return false; }
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pos_online: system not found";return false;}
        uint32 fuel=0; { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r,"SELECT COALESCE(SUM(e.quantity),0) FROM entity e JOIN invTypes t ON t.typeID=e.typeID WHERE e.locationID=%u AND t.groupID=1136", towerID) && r.GetRow(row)) fuel=row.GetUInt(0); }
        if (fuel==0) { resultMsg="pos_online: no fuel - stock fuel blocks first"; return false; }
        SystemEntity* se=pSM->GetSE(towerID);
        if (se==nullptr || se->GetTowerSE()==nullptr) { resultMsg="pos_online: tower not instantiated"; return false; }
        se->GetTowerSE()->VevReonline();
        char buf[96]; snprintf(buf,sizeof(buf),"tower %u back online (fuel %u)", towerID, fuel); resultMsg=buf; return true;
    }
    if (cmd == "pos_harvest") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), towerID=rdU("\"towerID\""), cycles=rdU("\"cycles\"");
        if (systemID==0||towerID==0) { resultMsg="pos_harvest needs systemID, towerID"; return false; }
        if (cycles==0) cycles=1;
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pos_harvest: system not found";return false;}
        // tower must exist + be online, and have an anchored moon
        uint32 moonID=0; int towerState=-1;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT anchorpointID, state FROM posStructureData WHERE itemID=%u", towerID) && r.GetRow(row)) { moonID=row.GetUInt(0); towerState=row.GetInt(1); } }
        if (moonID==0) { resultMsg="pos_harvest: no tower/moon for that id"; return false; }
        if (towerState < (int)EVEPOS::StructureState::Online) { resultMsg="pos_harvest: tower is not online"; return false; }
        // owner of the tower (deposit harvested goo under the owner)
        uint32 ownerID=charID;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT ownerID FROM entity WHERE itemID=%u", towerID) && r.GetRow(row)) ownerID=row.GetUInt(0); }
        // VEV_POS_ANCHOR: harvesting requires a REAL anchored Silo (groupID 404) at the tower —
        // moon goo flows INTO that real, vulnerable structure, not the tower bay. No silo, no harvest.
        uint32 siloID=0;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT s.itemID FROM posStructureData s JOIN entity e ON e.itemID=s.itemID JOIN invTypes t ON t.typeID=e.typeID WHERE s.towerID=%u AND t.groupID=404 AND s.state>=%d ORDER BY s.itemID DESC LIMIT 1", towerID, (int)EVEPOS::StructureState::Online) && r.GetRow(row)) siloID=row.GetUInt(0); }
        if (siloID==0) { resultMsg="pos_harvest: anchor a Silo at the tower first (pos_anchor type 14343)"; return false; }
        // read the moon composition + deposit richness-scaled qty per material into the tower bay
        DBQueryResult res;
        if (!sDatabase.RunQuery(res, "SELECT materialTypeID, richness FROM vevMoonResources WHERE moonID=%u", moonID)) {
            resultMsg="pos_harvest: moon composition query failed"; return false;
        }
        DBResultRow row; int mats=0; uint32 totalUnits=0;
        while (res.GetRow(row)) {
            uint32 matType=row.GetUInt(0); double rich=row.GetDouble(1);
            uint32 qty=(uint32)(rich * 100.0 * cycles);   // base 100 units/cycle, scaled by moon richness
            if (qty==0) continue;
            ItemData idata(matType, ownerID, siloID, flagHangar, qty);
            InventoryItemRef iRef=sItemFactory.SpawnItem(idata);
            if (iRef.get()!=nullptr) { mats++; totalUnits+=qty; }
        }
        if (mats==0) { resultMsg="pos_harvest: moon has no surveyed materials"; return true; }
        char buf[160]; snprintf(buf,sizeof(buf),"harvested %u units across %d moon materials (moon %u) into silo %u over %u cycle(s)", totalUnits, mats, moonID, siloID, cycles);
        resultMsg=buf; return true;
    }
    // ── VEV_POS2: run a moon reaction at the tower (inputs from bay → reacted output).
    if (cmd == "pos_react") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), towerID=rdU("\"towerID\""), reactionTypeID=rdU("\"reactionTypeID\""), cycles=rdU("\"cycles\"");
        if (systemID==0||towerID==0||reactionTypeID==0) { resultMsg="pos_react needs systemID, towerID, reactionTypeID"; return false; }
        if (cycles==0) cycles=1;
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pos_react: system not found";return false;}
        int towerState=-1; uint32 ownerID=charID;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT state FROM posStructureData WHERE itemID=%u", towerID) && r.GetRow(row)) towerState=row.GetInt(0); }
        if (towerState < (int)EVEPOS::StructureState::Online) { resultMsg="pos_react: tower is not online"; return false; }
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT ownerID FROM entity WHERE itemID=%u", towerID) && r.GetRow(row)) ownerID=row.GetUInt(0); }
        // VEV_POS_ANCHOR: reacting requires a REAL anchored Reactor (groupID 438) + Silo (404) at the
        // tower. Inputs are pulled from the silo and the reacted output is deposited back into it — real
        // infrastructure does the work. No reactor / no silo, no reaction.
        uint32 reactorID=0, siloID=0;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT s.itemID FROM posStructureData s JOIN entity e ON e.itemID=s.itemID JOIN invTypes t ON t.typeID=e.typeID WHERE s.towerID=%u AND t.groupID=438 AND s.state>=%d ORDER BY s.itemID DESC LIMIT 1", towerID, (int)EVEPOS::StructureState::Online) && r.GetRow(row)) reactorID=row.GetUInt(0); }
        if (reactorID==0) { resultMsg="pos_react: anchor a Reactor at the tower first (pos_anchor type 16869)"; return false; }
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT s.itemID FROM posStructureData s JOIN entity e ON e.itemID=s.itemID JOIN invTypes t ON t.typeID=e.typeID WHERE s.towerID=%u AND t.groupID=404 AND s.state>=%d ORDER BY s.itemID DESC LIMIT 1", towerID, (int)EVEPOS::StructureState::Online) && r.GetRow(row)) siloID=row.GetUInt(0); }
        if (siloID==0) { resultMsg="pos_react: anchor a Silo at the tower first (pos_anchor type 14343)"; return false; }
        // read the reaction formula
        std::vector<std::pair<uint32,uint32>> inputs, outputs;   // (typeID, qtyPerCycle)
        { DBQueryResult r; DBResultRow row;
          if (!sDatabase.RunQuery(r, "SELECT input, typeID, quantity FROM invTypeReactions WHERE reactionTypeID=%u", reactionTypeID)) { resultMsg="pos_react: formula query failed"; return false; }
          while (r.GetRow(row)) { if (row.GetInt(0)==0) outputs.push_back({row.GetUInt(1),row.GetUInt(2)}); else inputs.push_back({row.GetUInt(1),row.GetUInt(2)}); } }
        if (inputs.empty()||outputs.empty()) { resultMsg="pos_react: unknown/empty reaction"; return false; }
        // PASS 1: verify the tower bay has every input (summed across stacks)
        for (auto& in : inputs) {
            uint32 need = in.second * cycles, have = 0;
            DBQueryResult r; DBResultRow row;
            if (sDatabase.RunQuery(r, "SELECT COALESCE(SUM(quantity),0) FROM entity WHERE locationID=%u AND flag=4 AND typeID=%u", siloID, in.first) && r.GetRow(row)) have=row.GetUInt(0);
            if (have < need) { char b[128]; snprintf(b,sizeof(b),"pos_react: insufficient input type %u (need %u, have %u)", in.first, need, have); resultMsg=b; return false; }
        }
        // PASS 2: consume inputs (greedy across stacks), then produce outputs
        for (auto& in : inputs) {
            uint32 need = in.second * cycles;
            DBQueryResult r; DBResultRow row;
            sDatabase.RunQuery(r, "SELECT itemID, quantity FROM entity WHERE locationID=%u AND flag=4 AND typeID=%u ORDER BY quantity ASC", siloID, in.first);
            while (need > 0 && r.GetRow(row)) {
                uint32 itemID=row.GetUInt(0), q=row.GetUInt(1);
                InventoryItemRef iRef=sItemFactory.GetItemRef(itemID);
                if (iRef.get()==nullptr) continue;
                if (q <= need) { need -= q; iRef->Delete(); }
                else { iRef->SetQuantity(q - need); need = 0; }
            }
        }
        uint32 madeUnits=0;
        for (auto& out : outputs) {
            uint32 qty = out.second * cycles;
            ItemData idata(out.first, ownerID, siloID, flagHangar, qty);
            InventoryItemRef iRef=sItemFactory.SpawnItem(idata);
            if (iRef.get()!=nullptr) madeUnits+=qty;
        }
        char buf[160]; snprintf(buf,sizeof(buf),"reaction %u x%u: consumed %lu input type(s), produced %u units of %lu output(s)", reactionTypeID, cycles, (unsigned long)inputs.size(), madeUnits, (unsigned long)outputs.size());
        resultMsg=buf; return true;
    }
    // ── VEV_T2: stage items into a hangar (corp-supplied inputs / seed helper).
    if (cmd == "vev_stock") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 locationID=rdU("\"locationID\""), typeID=rdU("\"typeID\""), qty=rdU("\"qty\"");
        if (locationID==0||typeID==0) { resultMsg="vev_stock needs locationID, typeID"; return false; }
        if (qty==0) qty=1;
        ItemData idata(typeID, charID, locationID, flagHangar, qty);
        InventoryItemRef iRef=sItemFactory.SpawnItem(idata);
        if (iRef.get()==nullptr) { resultMsg="vev_stock: spawn failed"; return false; }
        char buf[96]; snprintf(buf,sizeof(buf),"stocked %u x type %u at %u", qty, typeID, locationID); resultMsg=buf; return true;
    }
    // ── VEV_T2: manufacture a product from its blueprint BOM (POS-3 components + T2-0 hull).
    if (cmd == "manufacture") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 locationID=rdU("\"locationID\""), productTypeID=rdU("\"productTypeID\""), runs=rdU("\"runs\"");
        if (locationID==0||productTypeID==0) { resultMsg="manufacture needs locationID, productTypeID"; return false; }
        if (runs==0) runs=1;
        uint32 bpID=0;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT blueprintTypeID FROM invBlueprintTypes WHERE productTypeID=%u", productTypeID) && r.GetRow(row)) bpID=row.GetUInt(0); }
        if (bpID==0) { resultMsg="manufacture: no blueprint for that product"; return false; }
        std::vector<std::pair<uint32,uint32>> bom;   // (materialTypeID, qtyPerRun)
        { DBQueryResult r; DBResultRow row;
          if (!sDatabase.RunQuery(r,
              "SELECT materialTypeID, quantity FROM invTypeMaterials WHERE typeID=%u "
              "UNION ALL SELECT r.requiredTypeID, r.quantity FROM ramTypeRequirements r "
              "JOIN invTypes mat ON mat.typeID=r.requiredTypeID JOIN invGroups g ON g.groupID=mat.groupID "
              "WHERE r.typeID=%u AND r.activityID=1 AND g.categoryID<>16 AND r.quantity>0", productTypeID, bpID)) { resultMsg="manufacture: BOM query failed"; return false; }
          while (r.GetRow(row)) bom.push_back({row.GetUInt(0), row.GetUInt(1)}); }
        if (bom.empty()) { resultMsg="manufacture: no material requirements"; return false; }
        // PASS 1: verify every material present (summed across the caller's stacks at the location)
        for (auto& m : bom) {
            uint32 need=m.second*runs, have=0;
            DBQueryResult r; DBResultRow row;
            if (sDatabase.RunQuery(r, "SELECT COALESCE(SUM(quantity),0) FROM entity WHERE locationID=%u AND flag=4 AND ownerID=%u AND typeID=%u", locationID, charID, m.first) && r.GetRow(row)) have=row.GetUInt(0);
            if (have < need) { char b[140]; snprintf(b,sizeof(b),"manufacture: missing material type %u (need %u, have %u)", m.first, need, have); resultMsg=b; return false; }
        }
        // PASS 2: consume materials (greedy across stacks)
        for (auto& m : bom) {
            uint32 need=m.second*runs;
            DBQueryResult r; DBResultRow row;
            sDatabase.RunQuery(r, "SELECT itemID, quantity FROM entity WHERE locationID=%u AND flag=4 AND ownerID=%u AND typeID=%u ORDER BY quantity ASC", locationID, charID, m.first);
            while (need>0 && r.GetRow(row)) {
                uint32 itemID=row.GetUInt(0), q=row.GetUInt(1);
                InventoryItemRef iRef=sItemFactory.GetItemRef(itemID);
                if (iRef.get()==nullptr) continue;
                if (q<=need) { need-=q; iRef->Delete(); } else { iRef->SetQuantity(q-need); need=0; }
            }
        }
        ItemData idata(productTypeID, charID, locationID, flagHangar, runs);
        InventoryItemRef out=sItemFactory.SpawnItem(idata);
        if (out.get()==nullptr) { resultMsg="manufacture: failed to spawn product"; return false; }
        char buf[160]; snprintf(buf,sizeof(buf),"manufactured %u x type %u (consumed %lu material type(s)) at %u", runs, productTypeID, (unsigned long)bom.size(), locationID); resultMsg=buf; return true;
    }
    // ── VEV_PITRANSFORM: run a PI schematic at a facility (inputs → PI commodity output).
    if (cmd == "pi_transform") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 locationID=rdU("\"locationID\""), productTypeID=rdU("\"productTypeID\""), runs=rdU("\"runs\"");
        if (locationID==0||productTypeID==0) { resultMsg="pi_transform needs locationID, productTypeID"; return false; }
        if (runs==0) runs=1;
        uint32 schemID=0, outQty=1;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT schematicID, quantity FROM piTypeMap WHERE isInput=0 AND typeID=%u LIMIT 1", productTypeID) && r.GetRow(row)) { schemID=row.GetUInt(0); outQty=row.GetUInt(1); if(!outQty)outQty=1; } }
        if (schemID==0) { resultMsg="pi_transform: no PI schematic for that product"; return false; }
        std::vector<std::pair<uint32,uint32>> inputs;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT typeID, quantity FROM piTypeMap WHERE schematicID=%u AND isInput=1", schemID)) while (r.GetRow(row)) inputs.push_back({row.GetUInt(0),row.GetUInt(1)}); }
        if (inputs.empty()) { resultMsg="pi_transform: schematic has no inputs"; return false; }
        // PASS 1: verify inputs (summed across the caller's stacks)
        for (auto& in : inputs) {
            uint32 need=in.second*runs, have=0;
            DBQueryResult r; DBResultRow row;
            if (sDatabase.RunQuery(r, "SELECT COALESCE(SUM(quantity),0) FROM entity WHERE locationID=%u AND flag=4 AND ownerID=%u AND typeID=%u", locationID, charID, in.first) && r.GetRow(row)) have=row.GetUInt(0);
            if (have<need) { char b[140]; snprintf(b,sizeof(b),"pi_transform: insufficient input type %u (need %u, have %u)", in.first, need, have); resultMsg=b; return false; }
        }
        // PASS 2: consume + produce
        for (auto& in : inputs) {
            uint32 need=in.second*runs;
            DBQueryResult r; DBResultRow row;
            sDatabase.RunQuery(r, "SELECT itemID, quantity FROM entity WHERE locationID=%u AND flag=4 AND ownerID=%u AND typeID=%u ORDER BY quantity ASC", locationID, charID, in.first);
            while (need>0 && r.GetRow(row)) {
                uint32 itemID=row.GetUInt(0), qv=row.GetUInt(1);
                InventoryItemRef iRef=sItemFactory.GetItemRef(itemID);
                if (iRef.get()==nullptr) continue;
                if (qv<=need) { need-=qv; iRef->Delete(); } else { iRef->SetQuantity(qv-need); need=0; }
            }
        }
        ItemData idata(productTypeID, charID, locationID, flagHangar, outQty*runs);
        InventoryItemRef out=sItemFactory.SpawnItem(idata);
        if (out.get()==nullptr) { resultMsg="pi_transform: spawn failed"; return false; }
        char buf[140]; snprintf(buf,sizeof(buf),"refined %u x type %u (%lu input type(s)) at %u", outQty*runs, productTypeID, (unsigned long)inputs.size(), locationID); resultMsg=buf; return true;
    }
    // ── VEV_REACTTR: run a reaction at a facility (inputs from location → reacted output).
    if (cmd == "react_transform") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 locationID=rdU("\"locationID\""), productTypeID=rdU("\"productTypeID\""), runs=rdU("\"runs\"");
        if (locationID==0||productTypeID==0) { resultMsg="react_transform needs locationID, productTypeID"; return false; }
        if (runs==0) runs=1;
        uint32 rxID=0, outQty=1;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT reactionTypeID, quantity FROM invTypeReactions WHERE input=0 AND typeID=%u LIMIT 1", productTypeID) && r.GetRow(row)) { rxID=row.GetUInt(0); outQty=row.GetUInt(1); if(!outQty)outQty=1; } }
        if (rxID==0) { resultMsg="react_transform: no reaction for that product"; return false; }
        std::vector<std::pair<uint32,uint32>> inputs;
        { DBQueryResult r; DBResultRow row; if (sDatabase.RunQuery(r, "SELECT typeID, quantity FROM invTypeReactions WHERE reactionTypeID=%u AND input=1", rxID)) while (r.GetRow(row)) inputs.push_back({row.GetUInt(0),row.GetUInt(1)}); }
        if (inputs.empty()) { resultMsg="react_transform: reaction has no inputs"; return false; }
        for (auto& in : inputs) {
            uint32 need=in.second*runs, have=0;
            DBQueryResult r; DBResultRow row;
            if (sDatabase.RunQuery(r, "SELECT COALESCE(SUM(quantity),0) FROM entity WHERE locationID=%u AND flag=4 AND ownerID=%u AND typeID=%u", locationID, charID, in.first) && r.GetRow(row)) have=row.GetUInt(0);
            if (have<need) { char b[140]; snprintf(b,sizeof(b),"react_transform: insufficient input type %u (need %u, have %u)", in.first, need, have); resultMsg=b; return false; }
        }
        for (auto& in : inputs) {
            uint32 need=in.second*runs;
            DBQueryResult r; DBResultRow row;
            sDatabase.RunQuery(r, "SELECT itemID, quantity FROM entity WHERE locationID=%u AND flag=4 AND ownerID=%u AND typeID=%u ORDER BY quantity ASC", locationID, charID, in.first);
            while (need>0 && r.GetRow(row)) {
                uint32 itemID=row.GetUInt(0), qv=row.GetUInt(1);
                InventoryItemRef iRef=sItemFactory.GetItemRef(itemID);
                if (iRef.get()==nullptr) continue;
                if (qv<=need) { need-=qv; iRef->Delete(); } else { iRef->SetQuantity(qv-need); need=0; }
            }
        }
        ItemData idata(productTypeID, charID, locationID, flagHangar, outQty*runs);
        InventoryItemRef out=sItemFactory.SpawnItem(idata);
        if (out.get()==nullptr) { resultMsg="react_transform: spawn failed"; return false; }
        char buf[140]; snprintf(buf,sizeof(buf),"reacted %u x type %u (%lu input(s)) at %u", outQty*runs, productTypeID, (unsigned long)inputs.size(), locationID); resultMsg=buf; return true;
    }
    if (cmd == "pi_tick") {
        std::string p(params);
        auto rdU = [&p](const char* k) -> uint32 { size_t kp=p.find(k); if(kp==std::string::npos) return 0; size_t c=p.find(":",kp); return (c==std::string::npos)?0:(uint32)atol(p.c_str()+c+1); };
        uint32 systemID=rdU("\"systemID\""), planetID=rdU("\"planetID\""), cycles=rdU("\"cycles\"");
        if (systemID==0||planetID==0) { resultMsg="pi_tick needs systemID, planetID"; return false; }
        if (cycles==0) cycles=1;
        SystemManager* pSM=FindOrBootSystem(systemID);
        if(!pSM){resultMsg="pi_tick: system not found";return false;}
        SystemEntity* pSE=pSM->GetSE(planetID);
        if(!pSE){resultMsg="pi_tick: planet not instantiated";return false;}
        PlanetSE* pPlanet=pSE->GetPlanetSE();
        if(!pPlanet){resultMsg="pi_tick: not a planet";return false;}
        Colony* colony=pPlanet->GetColony(charID);
        if(!colony->HasColony()){resultMsg="pi_tick: no colony here";return false;}
        colony->ForceProductionCycles((int)cycles);
        char buf[96]; snprintf(buf,sizeof(buf),"forced %u production cycle(s)", cycles);
        resultMsg=buf; return true;
    }
    if (cmd == "stack_op") {
        // VEV_STACK_OP: inventory stack management for the 2D client (split a
        // stack / merge two stacks of the same type). Runs on the main thread via
        // the command queue; uses the LIVE item API (AlterQuantity/SpawnItem) so
        // memory + DB stay coherent — never raw SQL.
        std::string p(params);
        auto readU32 = [&p](const char* key) -> uint32 {
            size_t k = p.find(key);
            if (k == std::string::npos) return 0;
            size_t c = p.find(":", k);
            return (c == std::string::npos) ? 0 : (uint32)atol(p.c_str() + c + 1);
        };
        const uint32 itemID = readU32("\"itemID\"");
        const uint32 withID = readU32("\"withItemID\"");
        const uint32 qty    = readU32("\"qty\"");
        const bool isMerge  = (p.find("\"merge\"") != std::string::npos);
        if (itemID == 0) { resultMsg = "missing 'itemID'"; return false; }
        InventoryItemRef iRef = sItemFactory.GetItemRef(itemID);
        if (iRef.get() == nullptr) { resultMsg = "item not found"; return false; }
        if (iRef->ownerID() != charID) { resultMsg = "not your item"; return false; }
        if (isMerge) {
            InventoryItemRef wRef = sItemFactory.GetItemRef(withID);
            if (wRef.get() == nullptr) { resultMsg = "merge target not found"; return false; }
            if (wRef->typeID() != iRef->typeID()) { resultMsg = "cannot merge different types"; return false; }
            if (wRef->locationID() != iRef->locationID() || wRef->flag() != iRef->flag()) {
                resultMsg = "stacks must be in the same hold"; return false;
            }
            const int32 addQty = (int32)iRef->quantity();
            wRef->AlterQuantity(addQty, true);
            iRef->SetQuantity(0, true, true);   // deleteOnZero -> row removed
            char buf[160];
            snprintf(buf, sizeof(buf), "merged %d units into stack %u", addQty, withID);
            resultMsg = buf;
            return true;
        }
        // split
        if (qty == 0 || qty >= (uint32)iRef->quantity()) {
            resultMsg = "split qty must be >0 and < the stack size"; return false;
        }
        ItemData sdata(iRef->typeID(), iRef->ownerID(), iRef->locationID(), iRef->flag(), qty);
        InventoryItemRef nRef = sItemFactory.SpawnItem(sdata);
        if (nRef.get() == nullptr) { resultMsg = "split failed (spawn)"; return false; }
        iRef->AlterQuantity(-(int32)qty, true);
        char buf[160];
        snprintf(buf, sizeof(buf), "split %u units into new stack %u", qty, nRef->itemID());
        resultMsg = buf;
        return true;
    }
    if (cmd == "reprocess") {
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "must be docked to reprocess";
            return false;
        }

        // Parse itemID
        std::string p(params);
        size_t idPos = p.find("\"itemID\"");
        if (idPos == std::string::npos) {
            resultMsg = "missing 'itemID' in params";
            return false;
        }
        size_t colonPos = p.find(":", idPos);
        uint32 itemID = (uint32)atol(p.c_str() + colonPos + 1);
        if (itemID == 0) {
            resultMsg = "invalid itemID";
            return false;
        }

        // Get station and ship
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID, shipID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 stationID = charRow.GetUInt(0);
        uint32 shipID = charRow.GetUInt(1);
        if (stationID == 0) {
            resultMsg = "not docked";
            return false;
        }

        // Load the ore item
        InventoryItemRef oreRef = sItemFactory.GetItemRef(itemID);
        if (oreRef.get() == nullptr) {
            resultMsg = "item not found";
            return false;
        }
        if (oreRef->ownerID() != charID) {
            resultMsg = "item does not belong to this character";
            return false;
        }

        // Get reprocessing output
        ReprocessingDB reprocDB;
        std::vector<Recoverable> recoverables;
        if (!reprocDB.GetRecoverables(oreRef->typeID(), recoverables) || recoverables.empty()) {
            resultMsg = "item cannot be reprocessed (no mineral output)";
            return false;
        }

        uint32 oreQty = (uint32)oreRef->quantity();

        // =================== VEV_REPROCESS_PORTION ===================
        // Yield is amountPerBatch per PORTION (invTypes.portionSize, ore=100),
        // not per UNIT. The old qty*batch printed 100x minerals (live-fired:
        // 266 Scordite -> 221,578 Trit). Remainder ore below a full portion
        // survives, EVE-real (the old path Delete()d it).
        uint32 vevPortion = 100;
        {
            DBQueryResult pres;
            DBResultRow prow;
            if (sDatabase.RunQuery(pres,
                "SELECT COALESCE(portionSize,1) FROM invTypes WHERE typeID = %u",
                oreRef->typeID()) && pres.GetRow(prow))
                vevPortion = prow.GetUInt(0) > 0 ? prow.GetUInt(0) : 1;
        }
        const uint32 vevBatches = oreQty / vevPortion;
        if (vevBatches == 0) {
            char pbuf[128];
            snprintf(pbuf, sizeof(pbuf),
                "not enough ore to reprocess: have %u, a batch needs %u units",
                oreQty, vevPortion);
            resultMsg = pbuf;
            return false;
        }
        // =================== end VEV_REPROCESS_PORTION ===================

        // Create output minerals in station hangar
        // Simplified: 100% efficiency (no skill/standing modifiers for AI)
        std::string result;
        int mineralCount = 0;
        for (auto& rec : recoverables) {
            uint32 mineralQty = rec.amountPerBatch * vevBatches;
            if (mineralQty == 0) continue;

            ItemData idata(rec.typeID, charID, stationID, flagHangar, mineralQty);
            InventoryItemRef mRef = sItemFactory.SpawnItem(idata);
            if (mRef.get() != nullptr) {
                char entry[128];
                snprintf(entry, sizeof(entry), "%u|%s|%u;",
                         rec.typeID, mRef->name(), mineralQty);
                result += entry;
                mineralCount++;
            }
        }

        // VEV_REPROCESS_PORTION: consume only the full portions; the
        // remainder stack survives in the hangar.
        const uint32 vevConsumed = vevBatches * vevPortion;
        if (vevConsumed >= oreQty)
            oreRef->Delete();
        else
            oreRef->AlterQuantity(-(int32)vevConsumed, true);

        char header[160];
        snprintf(header, sizeof(header), "reprocessed %u units (of %u) into %d minerals: ", vevConsumed, oreQty, mineralCount);
        resultMsg = header + result;
        return true;
    }

    // --- jettison: Drop an item from cargo into a jetcan in space ---
    // See A331 §3.5 (inventory_management — action)
    // params: {"itemID": 140000999}
    //   itemID = the item to jettison from cargo
    // Only works when in space
    if (cmd == "jettison") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space";
            return false;
        }

        // Parse itemID
        std::string p(params);
        size_t idPos = p.find("\"itemID\"");
        if (idPos == std::string::npos) {
            resultMsg = "missing 'itemID' in params";
            return false;
        }
        size_t colonPos = p.find(":", idPos);
        uint32 itemID = (uint32)atol(p.c_str() + colonPos + 1);
        if (itemID == 0) {
            resultMsg = "invalid itemID";
            return false;
        }

        // Load the item
        InventoryItemRef iRef = sItemFactory.GetItemRef(itemID);
        if (iRef.get() == nullptr) {
            resultMsg = "item not found";
            return false;
        }
        if (iRef->ownerID() != charID) {
            resultMsg = "item does not belong to this character";
            return false;
        }

        // VEV_FIX_JETTISON_SEGFAULT (conv3, 2026-05-31): the old path used
        // SpawnItem instead of SpawnCargoContainer + never allocated a ContainerSE,
        // so iRef->Move with notify=true derefed canRef->GetMySE()==nullptr ->
        // SIGSEGV. Mirrors ShipBound::Jettison (ShipService.cpp:840 jetcan branch
        // lines 985-1003). Diagnosed + fix-spec by conv-6.
        // =================== VEV_JETTISON_DEPOSIT ===================
        // Optional containerID: deposit into an EXISTING can within loot
        // range instead of spawning a new one -- the shared-can enabler
        // (a 20-frigate fleet pooling ore into ONE can; without this every
        // jettison scattered a fresh can). Mirrors loot_item's range gate.
        {
            uint32 vevCanID = 0;
            size_t vcPos = p.find("\"containerID\"");
            if (vcPos != std::string::npos) {
                size_t c = p.find(":", vcPos);
                if (c != std::string::npos) vevCanID = (uint32)atol(p.c_str() + c + 1);
            }
            if (vevCanID != 0) {
                SystemEntity* pCanSE = pAIShip->SystemMgr()->GetSE(vevCanID);
                if (pCanSE == nullptr) {
                    resultMsg = "container not found in system";
                    return false;
                }
                if (pAIShip->GetPosition().distance(pCanSE->GetPosition()) > 2500.0) {
                    resultMsg = "out of deposit range (2500 m) -- approach the container";
                    return false;
                }
                CargoContainerRef vevCanRef = sItemFactory.GetCargoRef(vevCanID);
                if (vevCanRef.get() == nullptr) {
                    resultMsg = "target is not a cargo container";
                    return false;
                }
                if (!vevCanRef->GetMyInventory()->HasAvailableSpace(flagNone, iRef)) {
                    resultMsg = "container is full";
                    return false;
                }
                iRef->Move(vevCanID, flagNone, true);
                iRef->SaveItem();
                char dbuf[200];
                snprintf(dbuf, sizeof(dbuf),
                    "deposited %s (%u) into container %u",
                    iRef->name(), itemID, vevCanID);
                resultMsg = dbuf;
                return true;
            }
        }
        // =================== end VEV_JETTISON_DEPOSIT ===================
        FactionData fdata;
        fdata.allianceID = 0; fdata.corporationID = 0;
        fdata.factionID = 0;  fdata.ownerID = charID;
        uint32 systemID = pAIShip->SystemMgr()->GetID();
        GPoint location = pAIShip->GetPosition();
        location.MakeRandomPointOnSphere(500.0f);
        ItemData cdata(23, charID, systemID, flagNone, "Jettisoned Cargo Container", location);
        CargoContainerRef canRef = sItemFactory.SpawnCargoContainer(cdata);
        if (canRef.get() == nullptr) {
            resultMsg = "failed to spawn cargo container";
            return false;
        }
        ContainerSE* cSE = new ContainerSE(canRef, *m_services /*VEV_FIX_JET_SVC*/,
                                           pAIShip->SystemMgr(), fdata);
        cSE->Init();
        canRef->SetMySE(cSE);
        canRef->SetAnchor(true);
        pAIShip->SystemMgr()->AddEntity(cSE);
        pAIShip->DestinyMgr()->SendJettisonPacket();
        if (!canRef->GetMyInventory()->HasAvailableSpace(flagNone, iRef)) {
            resultMsg = "jetcan capacity exceeded";
            return false;
        }
        // Move the item into the container (flagNone NOT flagCargoHold)
        iRef->Move(canRef->itemID(), flagNone, true);
        iRef->SaveItem();

        char buf[200];
        snprintf(buf, sizeof(buf), "jettisoned %s (%u) into cargo container %u at (%.0f, %.0f, %.0f)",
                 iRef->name(), itemID, canRef->itemID(),
                 pAIShip->GetPosition().x, pAIShip->GetPosition().y, pAIShip->GetPosition().z);
        resultMsg = buf;
        return true;
    }

    // --- sell_to_buy_order: Sell items from hangar to an existing buy order at station ---
    // See A331 §3.5 (inventory_management — action) + A335 §4 (migration priority)
    // params: {"typeID": 1230, "quantity": 100} or {"itemID": 140000999}
    //   typeID + quantity = sell this type/qty from hangar to best buy order
    //   itemID = sell this specific item stack to best buy order
    // Only works when docked. Finds the best NPC/player buy order automatically.
    if (cmd == "sell_to_buy_order") {
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "must be docked to sell";
            return false;
        }

        // Get station
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 stationID = charRow.GetUInt(0);
        if (stationID == 0) {
            resultMsg = "not docked";
            return false;
        }

        // Parse params — support both {typeID, quantity} and {itemID}
        std::string p(params);
        uint32 itemID = 0;
        uint32 typeID = 0;
        uint32 quantity = 0;

        size_t itemPos = p.find("\"itemID\"");
        if (itemPos != std::string::npos) {
            size_t colonPos = p.find(":", itemPos);
            itemID = (uint32)atol(p.c_str() + colonPos + 1);
        }

        size_t typePos = p.find("\"typeID\"");
        if (typePos != std::string::npos) {
            size_t colonPos = p.find(":", typePos);
            typeID = (uint32)atol(p.c_str() + colonPos + 1);
        }

        size_t qtyPos = p.find("\"quantity\"");
        if (qtyPos != std::string::npos) {
            size_t colonPos = p.find(":", qtyPos);
            quantity = (uint32)atol(p.c_str() + colonPos + 1);
        }

        // Resolve item reference
        InventoryItemRef iRef;
        if (itemID > 0) {
            iRef = sItemFactory.GetItemRef(itemID);
            if (iRef.get() == nullptr) {
                resultMsg = "item not found";
                return false;
            }
            if (iRef->ownerID() != charID) {
                resultMsg = "item does not belong to this character";
                return false;
            }
            typeID = iRef->typeID();
            quantity = (uint32)iRef->quantity();
        } else if (typeID > 0 && quantity > 0) {
            // Find item of this type in the hangar
            DBQueryResult findRes;
            if (!sDatabase.RunQuery(findRes,
                "SELECT itemID FROM entity WHERE locationID = %u AND ownerID = %u"
                " AND flag = %u AND typeID = %u AND quantity >= %u LIMIT 1",
                stationID, charID, (uint32)flagHangar, typeID, quantity)) {
                resultMsg = "failed to search hangar";
                return false;
            }
            DBResultRow findRow;
            if (!findRes.GetRow(findRow)) {
                resultMsg = "no matching item in hangar with sufficient quantity";
                return false;
            }
            itemID = findRow.GetUInt(0);
            iRef = sItemFactory.GetItemRef(itemID);
            if (iRef.get() == nullptr) {
                resultMsg = "item disappeared before sale";
                return false;
            }
        } else {
            resultMsg = "params: need {\"itemID\": X} or {\"typeID\": X, \"quantity\": X}";
            return false;
        }

        // Find the best buy order at this station
        uint32 orderID = MarketDB::FindBuyOrder(typeID, stationID, quantity, 0.01);
        if (orderID == 0) {
            resultMsg = "no buy order found at this station for this item";
            return false;
        }

        // Get order details
        Market::OrderInfo oInfo = Market::OrderInfo();
        if (!MarketDB::GetOrderInfo(orderID, oInfo)) {
            resultMsg = "failed to get order info";
            return false;
        }

        // Calculate sale amount
        uint32 qtySold = quantity;
        if (qtySold > oInfo.quantity)
            qtySold = oInfo.quantity;
        double money = oInfo.price * qtySold;

        // Handle item: delete or reduce quantity
        if (qtySold >= (uint32)iRef->quantity()) {
            iRef->Delete();
        } else {
            iRef->AlterQuantity(-(int32)qtySold, true);
        }

        // Update buy order: reduce or delete
        if (qtySold >= oInfo.quantity) {
            MarketDB::DeleteOrder(orderID);
        } else {
            MarketDB::AlterOrderQuantity(orderID, oInfo.quantity - qtySold);
        }

        // Credit ISK to the AI pilot. Use TransferFunds for proper journal entry.
        // Station owner pays from escrow (standard NPC buy order flow).
        uint32 stationOwner = stDataMgr.GetOwnerID(stationID);
        std::string reason = "DESC:  Selling items in ";
        reason += stDataMgr.GetStationName(stationID);
        AccountService::TransferFunds(
            stationOwner,          // from (station owner / escrow)
            charID,                // to (AI pilot)
            money,
            reason,
            Journal::EntryType::MarketTransaction,
            orderID
        );

        char buf[256];
        snprintf(buf, sizeof(buf), "sold %u x typeID %u for %.2f ISK (%.2f each) via order %u",
                 qtySold, typeID, money, oInfo.price, orderID);
        resultMsg = buf;
        return true;
    }

    // --- create_sell_order: Place a sell order on the regional market ---
    // CONV6_PATCH_create_sell_order_bridge (conv-6 stage, 2026-06-01)
    // Mirrors the Client-less sell_to_buy_order pattern; writes directly via
    // MarketDB::StoreOrder. SIMPLIFIED vs MarketProxyService::PlaceCharOrder:
    // no broker fee, no corp path, no SendOnOwnOrderChanged push. Conv-7 owns
    // the market lane and may replace this with a richer version. Must remain
    // docked (phantom-player only).
    //
    // params: {"type_id": 1230, "quantity": 100, "price": 5.50,
    //          "station_id": <opt>, "duration_days": 90, "min_volume": 1,
    //          "range": 0}
    //   type_id    (req)  invType to sell
    //   quantity   (req)  units to escrow into the order
    //   price      (req)  ISK per unit (rounds to 2dp downstream)
    //   station_id (opt)  defaults to character's currently-docked stationID
    //   duration_days (opt) 90 default. Range 1..90.
    //   min_volume (opt)  1 default. Per-trade minimum buyer quantity.
    //   range      (opt)  0 = station only (default). -1 = region.
    if (cmd == "create_sell_order") {
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "must be docked to create sell order";
            return false;
        }

        // Resolve docked station for default station_id + ownership checks
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 dockedStationID = charRow.GetUInt(0);
        if (dockedStationID == 0) {
            resultMsg = "not docked";
            return false;
        }

        std::string p(params);

        // --- parse type_id (required)
        uint32 typeID = 0;
        size_t tPos = p.find("\"type_id\"");
        if (tPos != std::string::npos) {
            size_t c = p.find(":", tPos);
            if (c != std::string::npos) typeID = (uint32)atol(p.c_str() + c + 1);
        }
        if (typeID == 0) {
            resultMsg = "params require {type_id, quantity, price}";
            return false;
        }

        // --- parse quantity (required)
        uint32 quantity = 0;
        size_t qPos = p.find("\"quantity\"");
        if (qPos != std::string::npos) {
            size_t c = p.find(":", qPos);
            if (c != std::string::npos) quantity = (uint32)atol(p.c_str() + c + 1);
        }
        if (quantity == 0) {
            resultMsg = "params require {type_id, quantity, price}";
            return false;
        }

        // --- parse price (required)
        double price = 0.0;
        size_t pPos = p.find("\"price\"");
        if (pPos != std::string::npos) {
            size_t c = p.find(":", pPos);
            if (c != std::string::npos) price = atof(p.c_str() + c + 1);
        }
        if (price <= 0.0) {
            resultMsg = "params require {type_id, quantity, price}; price>0";
            return false;
        }

        // --- parse station_id (optional, defaults to docked station)
        uint32 stationID = dockedStationID;
        size_t sPos = p.find("\"station_id\"");
        if (sPos != std::string::npos) {
            size_t c = p.find(":", sPos);
            if (c != std::string::npos) {
                uint32 sv = (uint32)atol(p.c_str() + c + 1);
                if (sv != 0) stationID = sv;
            }
        }

        // --- parse duration_days (optional, default 90)
        uint32 durationDays = 90;
        size_t dPos = p.find("\"duration_days\"");
        if (dPos != std::string::npos) {
            size_t c = p.find(":", dPos);
            if (c != std::string::npos) {
                uint32 dv = (uint32)atol(p.c_str() + c + 1);
                if (dv >= 1 && dv <= 90) durationDays = dv;
            }
        }

        // --- parse min_volume (optional, default 1)
        uint32 minVolume = 1;
        size_t mPos = p.find("\"min_volume\"");
        if (mPos != std::string::npos) {
            size_t c = p.find(":", mPos);
            if (c != std::string::npos) {
                uint32 mv = (uint32)atol(p.c_str() + c + 1);
                if (mv >= 1) minVolume = mv;
            }
        }

        // --- parse range (optional, default 0 = station)
        int16 orderRange = 0;
        size_t rPos = p.find("\"range\"");
        if (rPos != std::string::npos) {
            size_t c = p.find(":", rPos);
            if (c != std::string::npos) orderRange = (int16)atol(p.c_str() + c + 1);
        }

        // Sell orders for a phantom AI pilot must list from the docked
        // station; cross-station listing would require physically locating
        // the items there. Reject mismatches loudly.
        if (stationID != dockedStationID) {
            resultMsg = "sell order station_id must equal docked stationID for phantom pilots";
            return false;
        }

        // --- locate matching item in this character's hangar (sufficient qty)
        DBQueryResult findRes;
        if (!sDatabase.RunQuery(findRes,
            "SELECT itemID FROM entity WHERE locationID = %u AND ownerID = %u"
            " AND flag = %u AND typeID = %u AND quantity >= %u LIMIT 1",
            stationID, charID, (uint32)flagHangar, typeID, quantity)) {
            resultMsg = "failed to search hangar";
            return false;
        }
        DBResultRow findRow;
        if (!findRes.GetRow(findRow)) {
            resultMsg = "no matching item in hangar with sufficient quantity";
            return false;
        }
        uint32 itemID = findRow.GetUInt(0);
        InventoryItemRef iRef = sItemFactory.GetItemRef(itemID);
        if (iRef.get() == nullptr) {
            resultMsg = "item disappeared before listing";
            return false;
        }
        if (iRef->ownerID() != charID) {
            resultMsg = "item does not belong to this character";
            return false;
        }

        // --- escrow item: split stack if listing only part of it, else move
        // the whole stack into the order's escrow slot. Mirrors what
        // MarketProxyService::PlaceCharOrder does after validation
        // (the engine convention: the listed quantity leaves the player's
        // hangar entirely; cancellation returns it).
        if (quantity < (uint32)iRef->quantity()) {
            iRef->AlterQuantity(-(int32)quantity, true);
        } else {
            iRef->Delete();
        }

        // --- assemble SaveData. See EVE_Market.h Market::SaveData.
        Market::SaveData data = Market::SaveData();
        data.bid           = false;                                 // SELL
        data.isCorp        = false;
        data.contraband    = false;
        data.jumps         = 1;
        data.orderRange    = orderRange;
        data.typeID        = (uint16)typeID;
        data.accountKey    = 1000;                                  // Cash
        data.orderID       = 0;
        data.ownerID       = charID;
        data.regionID      = sDataMgr.GetStationRegion(stationID);
        data.stationID     = stationID;
        data.solarSystemID = sDataMgr.GetStationSystem(stationID);
        data.minVolume     = minVolume;
        data.volEntered    = quantity;
        data.volRemaining  = quantity;
        data.duration      = durationDays;
        data.memberID      = 0;
        data.issued        = GetFileTimeNow();
        data.price         = (float)price;
        data.escrow        = 0.0f;                                  // sell = no ISK escrow

        uint32 orderID = MarketDB::StoreOrder(data);
        if (orderID == 0) {
            resultMsg = "failed to record sell order in DB";
            return false;
        }

        // --- invalidate region cache so live clients see the new order
        sMktMgr.InvalidateOrdersCache(data.regionID, typeID);

        // --- journal entry (no ISK movement; this is just the order-create
        // bookkeeping line. Real ISK arrives when a buyer executes the
        // order via the existing ExecuteSellOrder flow.)
        // CONV6_PATCH_create_sell_order_bridge — see also MarketProxyService.cpp:175
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "sell order %u placed: %u x typeID %u @ %.2f ISK at station %u "
                 "(region %u, duration %u days, range %d, min_volume %u)",
                 orderID, quantity, typeID, price, stationID,
                 data.regionID, durationDays, (int)orderRange, minVolume);
        resultMsg = buf;
        return true;
    }

    // ---- CONV-6 PATCHER D: create_buy_order bridge (queue side of MarketProxyService bid=1) ----
    // // CONV6_PATCH_create_buy_order_bridge
    // See A346 (Market Operations) + A335 §4 (P1 economy-bridge staging).
    // Wraps MarketProxyService::PlaceCharOrder bid=1 path WITHOUT a Client*:
    //   * validates pilot wallet >= price*quantity + brokerage
    //   * escrows ISK to station owner via AccountService::TransferFunds
    //   * inserts row via MarketDB::StoreOrder (bid=1)
    // params: {type_id, quantity, price, station_id, duration_days?, min_volume?, range?}
    // - station_id optional: defaults to pilot's docked station; falls back fail if undocked.
    // - duration_days optional (default 14, capped at 90 per EVE rules).
    // - min_volume optional (default 1).
    // - range optional (default -1 = Station; valid: -1, 0, 4, 32767 per EVE_Market.h Market::Range).
    // Brokerage is conservative flat 1% (EVE base broker rate) since EvEMath::Market::BrokerFee
    // + StandingDB are not in EntityList.cpp's include scope. Skill/standing discounts are
    // forgone in favor of a Client-less surgical bridge; AI pilots pay slightly more, which
    // is acceptable for the staging bridge.
    if (cmd == "create_buy_order") {
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "must be docked to place buy order";
            return false;
        }

        // Parse params (snake_case primary, camelCase fallback to match get_market_prices style).
        std::string p(params);
        auto parseUInt = [&](const char* keyA, const char* keyB) -> uint32 {
            size_t k = p.find(keyA);
            if (k == std::string::npos && keyB != nullptr) k = p.find(keyB);
            if (k == std::string::npos) return 0;
            size_t c = p.find(":", k);
            if (c == std::string::npos) return 0;
            return (uint32)atol(p.c_str() + c + 1);
        };
        auto parseDouble = [&](const char* keyA, const char* keyB) -> double {
            size_t k = p.find(keyA);
            if (k == std::string::npos && keyB != nullptr) k = p.find(keyB);
            if (k == std::string::npos) return 0.0;
            size_t c = p.find(":", k);
            if (c == std::string::npos) return 0.0;
            return atof(p.c_str() + c + 1);
        };

        uint32 typeID      = parseUInt("\"type_id\"",       "\"typeID\"");
        uint32 quantity    = parseUInt("\"quantity\"",      nullptr);
        double price       = parseDouble("\"price\"",       nullptr);
        uint32 stationIDIn = parseUInt("\"station_id\"",    "\"stationID\"");
        uint32 durDays     = parseUInt("\"duration_days\"", "\"duration\"");
        uint32 minVol      = parseUInt("\"min_volume\"",    "\"minVolume\"");
        // range parsed as signed by reading raw int via atoi.
        int rangeIn = 0;
        bool rangeProvided = false;
        {
            size_t k = p.find("\"range\"");
            if (k != std::string::npos) {
                size_t c = p.find(":", k);
                if (c != std::string::npos) {
                    rangeIn = atoi(p.c_str() + c + 1);
                    rangeProvided = true;
                }
            }
        }

        if (typeID == 0 || quantity == 0 || price <= 0.0) {
            resultMsg = "params require {type_id, quantity, price, station_id?}";
            return false;
        }
        if (durDays == 0) durDays = 14;
        if (durDays > 90) durDays = 90;
        if (minVol == 0) minVol = 1;
        int16 orderRange = rangeProvided ? (int16)rangeIn : (int16)-1; // default Station
        // Whitelist range values per EVE_Market.h Market::Range enum.
        if (orderRange != -1 && orderRange != 0 && orderRange != 4 && orderRange != 32767) {
            resultMsg = "invalid range (allowed: -1 station, 0 system, 4 constellation, 32767 region)";
            return false;
        }

        // Resolve stationID: explicit param OR pilot's docked station.
        uint32 stationID = stationIDIn;
        if (stationID == 0) {
            DBQueryResult charRes;
            if (!sDatabase.RunQuery(charRes,
                "SELECT stationID FROM chrCharacters WHERE characterID = %u", charID)) {
                resultMsg = "failed to query character station";
                return false;
            }
            DBResultRow charRow;
            if (!charRes.GetRow(charRow)) { resultMsg = "character not found"; return false; }
            stationID = charRow.GetUInt(0);
            if (stationID == 0) { resultMsg = "not docked and no station_id provided"; return false; }
        }

        // Resolve region from station (mapDenormalize.regionID — same shape get_market_prices needs).
        DBQueryResult regRes;
        if (!sDatabase.RunQuery(regRes,
            "SELECT regionID, solarSystemID FROM mapDenormalize WHERE itemID = %u", stationID)) {
            resultMsg = "failed to resolve station region";
            return false;
        }
        DBResultRow regRow;
        if (!regRes.GetRow(regRow)) { resultMsg = "station not found in mapDenormalize"; return false; }
        uint32 regionID = regRow.GetUInt(0);
        uint32 solarSystemID = regRow.GetUInt(1);

        // Compute escrow + conservative brokerage (flat 1%).
        double money = price * (double)quantity;
        double fee = money * 0.01;
        double total = money + fee;

        // Wallet check against chrCharacters.balance (same column used by buy_from_sell_order
        // and the rest of the AI-pilot wallet bridge).
        DBQueryResult balRes;
        if (!sDatabase.RunQuery(balRes,
            "SELECT balance FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query wallet";
            return false;
        }
        DBResultRow balRow;
        if (!balRes.GetRow(balRow)) { resultMsg = "character not found"; return false; }
        double balance = balRow.GetDouble(0);
        if (balance < total) {
            char errbuf[200];
            snprintf(errbuf, sizeof(errbuf),
                "insufficient ISK: have %.2f, need %.2f (price*qty %.2f + 1%% brokerage %.2f)",
                balance, total, money, fee);
            resultMsg = errbuf;
            return false;
        }

        // Build SaveData mirroring MarketProxyService.cpp:295-325 (bid=1 path).
        Market::SaveData data = Market::SaveData();
        data.bid           = true;
        data.isCorp        = false;
        data.contraband    = false;
        data.jumps         = 1;
        data.orderRange    = orderRange;
        data.typeID        = (uint16)typeID;
        data.accountKey    = (uint16)Account::KeyType::Cash;
        data.orderID       = 0; // assigned by StoreOrder
        data.ownerID       = charID;
        data.regionID      = regionID;
        data.stationID     = stationID;
        data.solarSystemID = solarSystemID;
        data.minVolume     = minVol;
        data.volEntered    = quantity;
        data.volRemaining  = quantity;
        data.duration      = durDays;
        data.memberID      = 0;
        data.issued        = GetFileTimeNow();
        data.price         = (float)price;
        data.escrow        = (float)money;

        uint32 orderID = MarketDB::StoreOrder(data);
        if (orderID == 0) {
            resultMsg = "failed to record buy order in mktOrders";
            return false;
        }

        // Escrow + brokerage transfers (mirror MarketProxyService.cpp:330-340 player branch).
        uint32 stationOwner = stDataMgr.GetOwnerID(stationID);
        std::string brokerReason = "DESC:  Setting up buy order in ";
        brokerReason += stDataMgr.GetStationName(stationID);
        AccountService::TransferFunds(
            charID,
            stationOwner,
            (float)fee,
            brokerReason,
            Journal::EntryType::Brokerfee,
            orderID,
            (uint32)Account::KeyType::Cash
        );
        AccountService::TransferFunds(
            charID,
            stationOwner,
            (float)money,
            brokerReason,
            Journal::EntryType::MarketEscrow,
            orderID,
            (uint32)Account::KeyType::Escrow
        );

        char okbuf[256];
        snprintf(okbuf, sizeof(okbuf),
            "buy order %u placed: %u x typeID %u @ %.2f ISK (escrow %.2f + fee %.2f) at station %u region %u",
            orderID, quantity, typeID, price, money, fee, stationID, regionID);
        resultMsg = okbuf;
        return true;
    }

    // --- send_channel_message: Send an in-game chat message as a phantom AI character ---
    // See A321 §4.7 — Delivers a real OnLSC notification to all channel members (visible in EVE client)
    // params: {"channel_id": 100, "message": "Hello fellow pilots!"}
    if (cmd == "send_channel_message") {
        std::string p(params);

        // Parse channel_id (default 100 = AI Pilots)
        int32 channelID = 100;
        size_t chPos = p.find("\"channel_id\"");
        if (chPos != std::string::npos) {
            size_t c = p.find(":", chPos);
            if (c != std::string::npos) channelID = (int32)atol(p.c_str() + c + 1);
        }

        // Parse message string
        size_t mPos = p.find("\"message\"");
        if (mPos == std::string::npos) {
            resultMsg = "missing 'message' in params";
            return false;
        }
        size_t qStart = p.find("\"", mPos + 9);  // find opening quote of value
        if (qStart == std::string::npos) {
            resultMsg = "malformed 'message' param";
            return false;
        }
        qStart++;  // skip the opening quote
        // Find closing quote, handling escaped quotes
        std::string message;
        for (size_t i = qStart; i < p.size(); ++i) {
            if (p[i] == '\\' && i + 1 < p.size()) {
                message += p[++i];
            } else if (p[i] == '"') {
                break;
            } else {
                message += p[i];
            }
        }
        if (message.empty()) {
            resultMsg = "empty message";
            return false;
        }
        // VEV_CHAT_CAP_v1 (2026-05-31, convo-5 / COMMS ANCHOR): hard-reject
        // over-cap messages instead of silent-truncate. Cap=240 chars per the
        // orchestra soft-cap (cards/fleet-social.jsonl fleet-social-chat-cooldown
        // + cognition-system/archive/orchestra-era/trials/fleet-social-01.md Message length discipline).
        // Goal: 0% client-side visual truncation. Was: silent resize(500) which
        // let messages reach the EVE client where the chat panel truncated at
        // ~300 chars visually. Now: failed row with actionable error so the
        // sending swimmer rewrites to fit instead of shipping a clipped message.
        if (message.size() > 240) {
            char errbuf[200];
            snprintf(errbuf, sizeof(errbuf),
                "message too long (%zu chars > 240 cap); rewrite to fit.",
                message.size());
            resultMsg = errbuf;
            return false;
        }

        // Look up character info
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT characterName, corporationID FROM chrCharacters WHERE characterID = %u", charID))
        {
            resultMsg = "character not found";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        std::string charName = charRow.GetText(0);
        uint32 corpID = charRow.GetUInt(1);
        uint32 allianceID = 0;

        // Find the channel
        LSCService* lsc = m_services->Lookup<LSCService>("LSC");
        if (lsc == nullptr) {
            resultMsg = "LSC service not available";
            return false;
        }
        LSCChannel* chan = lsc->GetChannelByID(channelID);
        if (chan == nullptr) {
            resultMsg = "channel not found: " + std::to_string(channelID);
            return false;
        }

        // Ensure the phantom is joined to this channel
        if (!chan->IsJoined(charID)) {
            chan->JoinChannelAsPhantom(charID, charName, corpID, allianceID, 0, 0);
        }

        // Send the real in-game message
        chan->SendMessageAsPhantom(charID, charName, corpID, allianceID, 0, message.c_str());

        char buf[256];
        snprintf(buf, sizeof(buf), "sent to channel %d: %.80s", channelID, message.c_str());
        resultMsg = buf;
        return true;
    }

    // --- buy_from_sell_order: Buy items from an existing sell order at station ---
    // See A346 §1 (Market Operations) + A335 §4 (migration priority)
    // params: {"typeID": 3327, "quantity": 1} or {"type_name": "Miner I", "quantity": 1}
    // Only works when docked. Finds the cheapest NPC/player sell order automatically.
    // Uses TransferFunds for in-memory balance (NOT direct SQL).
    if (cmd == "buy_from_sell_order") {
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "must be docked to buy";
            return false;
        }

        // Get station
        DBQueryResult charRes;
        if (!sDatabase.RunQuery(charRes,
            "SELECT stationID, balance FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character";
            return false;
        }
        DBResultRow charRow;
        if (!charRes.GetRow(charRow)) {
            resultMsg = "character not found";
            return false;
        }
        uint32 stationID = charRow.GetUInt(0);
        double balance = charRow.GetFloat(1);
        if (stationID == 0) {
            resultMsg = "not docked";
            return false;
        }

        // Parse params
        std::string p(params);
        uint32 typeID = 0;
        uint32 quantity = 1;

        // Try typeID first
        size_t typePos = p.find("\"typeID\"");
        if (typePos != std::string::npos) {
            size_t colonPos = p.find(":", typePos);
            typeID = (uint32)atol(p.c_str() + colonPos + 1);
        }

        // Try type_name if no typeID
        if (typeID == 0) {
            size_t namePos = p.find("\"type_name\"");
            if (namePos != std::string::npos) {
                size_t colonPos = p.find(":", namePos);
                size_t quoteStart = p.find("\"", colonPos + 1);
                size_t quoteEnd = p.find("\"", quoteStart + 1);
                if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
                    std::string typeName = p.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                    // Escape and look up
                    std::string escapedName;
                    sDatabase.DoEscapeString(escapedName, typeName);
                    DBQueryResult nameRes;
                    if (sDatabase.RunQuery(nameRes,
                        "SELECT typeID FROM invTypes WHERE typeName = '%s' LIMIT 1",
                        escapedName.c_str())) {
                        DBResultRow nameRow;
                        if (nameRes.GetRow(nameRow)) {
                            typeID = nameRow.GetUInt(0);
                        }
                    }
                    if (typeID == 0) {
                        resultMsg = "unknown item type: " + typeName;
                        return false;
                    }
                }
            }
        }

        size_t qtyPos = p.find("\"quantity\"");
        if (qtyPos != std::string::npos) {
            size_t colonPos = p.find(":", qtyPos);
            quantity = (uint32)atol(p.c_str() + colonPos + 1);
        }

        if (typeID == 0 || quantity == 0) {
            resultMsg = "params: need {\"typeID\": X, \"quantity\": X} or {\"type_name\": \"...\", \"quantity\": X}";
            return false;
        }

        // Find cheapest sell order at this station
        // MarketDB::FindSellOrder(typeID, stationID, quantity, maxPrice)
        // We pass a very high max price to find ANY sell order
        uint32 orderID = MarketDB::FindSellOrder(typeID, stationID, quantity, 999999999.0);
        if (orderID == 0) {
            resultMsg = "no sell order found at this station for typeID " + std::to_string(typeID);
            return false;
        }

        // Get order details
        Market::OrderInfo oInfo = Market::OrderInfo();
        if (!MarketDB::GetOrderInfo(orderID, oInfo)) {
            resultMsg = "failed to get order info";
            return false;
        }

        // Calculate purchase
        uint32 qtyBuy = quantity;
        if (qtyBuy > oInfo.quantity)
            qtyBuy = oInfo.quantity;
        double totalCost = oInfo.price * qtyBuy;

        // Check balance — use the in-memory balance if character is online
        // (the DB balance may be stale for online characters)
        Client* pBuyer = sEntityList.FindClientByCharID(charID);
        if (pBuyer != nullptr) {
            balance = pBuyer->GetBalance();
        }

        if (totalCost > balance) {
            // Try to buy what we can afford
            uint32 affordable = (uint32)(balance / oInfo.price);
            if (affordable == 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "cannot afford typeID %u: need %.2f ISK, have %.2f ISK",
                         typeID, oInfo.price, balance);
                resultMsg = buf;
                return false;
            }
            qtyBuy = affordable;
            totalCost = oInfo.price * qtyBuy;
        }

        // Debit ISK from the AI pilot. Use TransferFunds for proper journal entry.
        uint32 stationOwner = stDataMgr.GetOwnerID(stationID);
        std::string reason = "DESC:  Buying items in ";
        reason += stDataMgr.GetStationName(stationID);
        AccountService::TransferFunds(
            charID,             // from (AI pilot)
            stationOwner,       // to (station owner / escrow)
            totalCost,
            reason,
            Journal::EntryType::MarketTransaction,
            orderID
        );

        // Create or stack item in hangar
        // Check if buyer already has this type in hangar (flag=0 = hangar)
        DBQueryResult existRes;
        bool stacked = false;
        if (sDatabase.RunQuery(existRes,
            "SELECT itemID, quantity FROM entity WHERE ownerID = %u AND typeID = %u"
            " AND locationID = %u AND flag = 0 AND singleton = 0 LIMIT 1",
            charID, typeID, stationID)) {
            DBResultRow existRow;
            if (existRes.GetRow(existRow)) {
                // Stack onto existing item
                uint32 existingID = existRow.GetUInt(0);
                InventoryItemRef existRef = sItemFactory.GetItemRef(existingID);
                if (existRef.get() != nullptr) {
                    existRef->AlterQuantity((int32)qtyBuy, true);
                    stacked = true;
                }
            }
        }

        if (!stacked) {
            // Create new item in hangar
            ItemData iData(
                typeID,
                charID,       // ownerID
                stationID,    // locationID
                flagHangar,   // flag (0 = hangar)
                qtyBuy
            );
            InventoryItemRef newItem = sItemFactory.SpawnItem(iData);
            if (newItem.get() == nullptr) {
                resultMsg = "CRITICAL: ISK debited but item creation failed!";
                return false;
            }
        }

        // Update sell order: reduce or delete
        if (qtyBuy >= oInfo.quantity) {
            MarketDB::DeleteOrder(orderID);
        } else {
            MarketDB::AlterOrderQuantity(orderID, oInfo.quantity - qtyBuy);
        }

        // Record transaction
        Market::TxData txData = Market::TxData();
        txData.isBuy = true;
        txData.accountKey = Account::KeyType::Cash;
        txData.typeID = typeID;
        txData.quantity = qtyBuy;
        txData.price = oInfo.price;
        txData.stationID = stationID;
        txData.regionID = oInfo.regionID;
        txData.clientID = charID;
        MarketDB::RecordTransaction(txData);

        char buf[256];
        snprintf(buf, sizeof(buf), "bought %u x typeID %u for %.2f ISK (%.2f each) via order %u",
                 qtyBuy, typeID, totalCost, oInfo.price, orderID);
        resultMsg = buf;
        return true;
    }

    // See A371 Bug11 — Server-side combat mission lifecycle test.
    // Runs the full Encounter mission flow without a client: offer → accept → verify bubble → check completion.
    // params: {"agentID": 3008938}
    if (cmd == "mission_test") {
        std::string p(params);
        // Parse agentID from params
        uint32 agentID = 0;
        size_t aPos = p.find("\"agentID\"");
        if (aPos != std::string::npos) {
            size_t colonPos = p.find(":", aPos);
            if (colonPos != std::string::npos)
                agentID = (uint32)atol(p.c_str() + colonPos + 1);
        }
        if (agentID == 0) {
            resultMsg = "missing or invalid 'agentID' in params";
            return false;
        }

        Agent* pAgent = sEntityList.GetAgent(agentID);
        if (pAgent == nullptr) {
            resultMsg = "agent not found: " + std::to_string(agentID);
            return false;
        }

        char buf[1024];

        // Step 1: Check if character already has an active mission with this agent
        MissionOffer existingOffer = MissionOffer();
        if (pAgent->HasMission(charID, existingOffer)) {
            // If there's an accepted encounter mission, verify its state
            if (existingOffer.typeID == Mission::Type::Encounter
                    and existingOffer.stateID == Mission::State::Accepted) {
                // Try to ensure the bubble exists (re-creates if lost after restart)
                SystemBubble* pBubble = pAgent->EnsureEncounterBubble(existingOffer);

                uint32 npcCount = 0;
                bool isMission = false;
                if (pBubble != nullptr) {
                    npcCount = pBubble->CountNPCs();
                    isMission = pBubble->IsMission();
                }

                snprintf(buf, sizeof(buf),
                    "EXISTING mission '%s' (id=%u state=%u type=%u) "
                    "bubbleID=%u bubble=%s IsMission=%s CountNPCs=%u "
                    "complete=%s",
                    existingOffer.name.c_str(), existingOffer.missionID,
                    existingOffer.stateID, existingOffer.typeID,
                    existingOffer.dungeonLocationID,
                    pBubble != nullptr ? "FOUND" : "NULL",
                    isMission ? "true" : "false",
                    npcCount,
                    (pBubble != nullptr and isMission and npcCount < 1) ? "TRUE(BUG!)" : "FALSE(correct)");
                resultMsg = buf;
                return true;
            }
            // Non-encounter or non-accepted mission — remove it so we can test fresh
            pAgent->RemoveOffer(charID);
        }

        // Step 2: Create a new encounter offer
        MissionOffer offer = MissionOffer();
        pAgent->MakeOffer(charID, offer);

        if (offer.typeID != Mission::Type::Encounter) {
            // Got a non-encounter mission — report it
            snprintf(buf, sizeof(buf),
                "OFFERED non-encounter: '%s' (type=%u) — re-run to try again (random selection)",
                offer.name.c_str(), offer.typeID);
            resultMsg = buf;
            return true;
        }

        // Step 3: Accept the mission — triggers SetupEncounterMission
        offer.stateID = Mission::State::Accepted;
        offer.dateAccepted = GetFileTimeNow();
        offer.expiryTime = GetFileTimeNow() + (30 * pAgent->GetLevel() * EvE::Time::Minute);

        pAgent->SetupEncounterMission(offer);
        pAgent->UpdateOffer(charID, offer);

        // Step 4: Verify the bubble and NPC state
        SystemBubble* pBubble = nullptr;
        if (offer.dungeonLocationID > 0)
            pBubble = sBubbleMgr.FindBubbleByID(offer.dungeonLocationID);

        uint32 npcCount = 0;
        bool isMission = false;
        if (pBubble != nullptr) {
            npcCount = pBubble->CountNPCs();
            isMission = pBubble->IsMission();
        }

        bool isComplete = (pBubble != nullptr and isMission and npcCount < 1);

        snprintf(buf, sizeof(buf),
            "ACCEPTED '%s' (id=%u) → bubbleID=%u bubble=%s IsMission=%s CountNPCs=%u complete=%s %s",
            offer.name.c_str(), offer.missionID,
            offer.dungeonLocationID,
            pBubble != nullptr ? "FOUND" : "NULL",
            isMission ? "true" : "false",
            npcCount,
            isComplete ? "TRUE(BUG!)" : "FALSE(correct)",
            npcCount > 0 ? "— NPCs spawned OK" : "— WARNING: 0 NPCs!");
        resultMsg = buf;
        return true;
    }



    // =================== VEV_GET_ANOMALIES ===================
    // God's-eye cosmic-signal list for the pilot's CURRENT system: every
    // anomaly + signature the AnomalyMgr holds (id|name|dungeonType|itemID|
    // pos). The probe minigame (Scan.cpp) is Client*-coupled and skipped --
    // consistent with the other phantom perception verbs. params: {} (in space)
    if (cmd == "get_anomalies") {
        // VEV_ANOMALIES_DOCKED: signatures are per-SYSTEM data, so a docked
        // pilot can scan too. Prefer the in-space ship's system; fall back to
        // the character's solarSystemID so the explore FSM can pre-scan from
        // dock and only undock when a real site exists (the doom-loop fix).
        SystemManager* pSM = nullptr;
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip != nullptr) {
            pSM = pAIShip->SystemMgr();
        } else {
            DBQueryResult cres;
            if (sDatabase.RunQuery(cres,
                    "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", charID)) {
                DBResultRow crow;
                if (cres.GetRow(crow) && !crow.IsNull(0))
                    pSM = FindOrBootSystem(crow.GetUInt(0));
            }
        }
        if (pSM == nullptr) {
            resultMsg = "no system for character";
            return false;
        }
        AnomalyMgr* pAM = pSM->GetAnomMgr();
        if (pAM == nullptr) {
            resultMsg = "no anomaly manager for this system";
            return false;
        }
        std::vector<CosmicSignature> vevSigs;
        // VEV_ANOMALY_HYGIENE (Phase A): return ONLY real cosmic anomalies
        // (m_anomByItemID, dungeonType==Anomaly, warp-to directly w/ no probes) --
        // the EVE "Anomaly" probe-scanner tab. Signatures (gravimetric/radar/relic/
        // data/wormhole that NEED PROBES, plus AddSignal-registered ships/drones/
        // structures) belong to the probe-scan loop (Scan.cpp) and will surface via a
        // separate get_signatures verb once that path is liberated for phantoms
        // (Phase C). Conflating both here made the explore FSM warp to a customs
        // office / a leftover drone / an unnamed wormhole and "clear" it (Vagrant).
        pAM->GetAnomalyList(vevSigs);
        std::string out;
        char ebuf[256];
        int n = 0;
        for (auto& s : vevSigs) {
            snprintf(ebuf, sizeof(ebuf), "%s|%s|type=%u|item=%u|pos=(%.0f,%.0f,%.0f);",
                     s.sigID.c_str(), s.sigName.c_str(), s.dungeonType, s.sigItemID,
                     s.position.x, s.position.y, s.position.z);
            out += ebuf;
            n++;
        }
        char hbuf[64];
        snprintf(hbuf, sizeof(hbuf), "%d signal(s): ", n);
        resultMsg = hbuf + out;
        return true;
    }
    // =================== end VEV_GET_ANOMALIES ===================

    // =================== VEV_GET_SIGNATURES ===================
    // The probe-scan SIGNATURE list (relic/data/wormhole/ore/gas, dungeonType 2-6)
    // for the pilot's CURRENT system -- the non-combat exploration content. Mirrors
    // get_anomalies but reads GetSignatureList (m_sigByItemID). NO probe minigame:
    // ShipScanResult already resolves every sig from its intrinsic sigStrength +
    // stored position (Scan.cpp:246-293), so we read AnomalyMgr directly. The explore
    // FSM filters dungeonType 3=relic(Analyzer)/4=data(Codebreaker). params: {}.
    if (cmd == "get_signatures") {
        SystemManager* pSM = nullptr;
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip != nullptr) {
            pSM = pAIShip->SystemMgr();
        } else {
            DBQueryResult cres;
            if (sDatabase.RunQuery(cres,
                    "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", charID)) {
                DBResultRow crow;
                if (cres.GetRow(crow) && !crow.IsNull(0))
                    pSM = FindOrBootSystem(crow.GetUInt(0));
            }
        }
        if (pSM == nullptr) { resultMsg = "no system for character"; return false; }
        AnomalyMgr* pAM = pSM->GetAnomMgr();
        if (pAM == nullptr) { resultMsg = "no anomaly manager for this system"; return false; }
        std::vector<CosmicSignature> vevSigs;
        pAM->GetSignatureList(vevSigs);
        std::string out;
        char ebuf[256];
        int n = 0;
        for (auto& s : vevSigs) {
            // VEV_SIG_HYGIENE: real cosmic signatures only (dungeonType 2-6:
            // grav/relic/data/ladar/wormhole). AddSignal-registered structures,
            // ships, drones + anomalies come back as type 7+ -- not exploration content.
            if (s.dungeonType < 2 || s.dungeonType > 6) continue;
            snprintf(ebuf, sizeof(ebuf), "%s|%s|type=%u|item=%u|pos=(%.0f,%.0f,%.0f);",
                     s.sigID.c_str(), s.sigName.c_str(), s.dungeonType, s.sigItemID,
                     s.position.x, s.position.y, s.position.z);
            out += ebuf;
            n++;
        }
        char hbuf[64];
        snprintf(hbuf, sizeof(hbuf), "%d signature(s): ", n);
        resultMsg = hbuf + out;
        return true;
    }
    // =================== end VEV_GET_SIGNATURES ===================

    // =================== VEV_WARP_TO_SIGNATURE ===================
    // Warp to a cosmic signature/anomaly by sigID -- the site position is already
    // stored on the CosmicSignature. Resolves sigID -> position via AnomalyMgr, then
    // warps via the AIShip warp path. params: {"sigID":"ABC-123"[,"distance":10000]}.
    if (cmd == "warp_to_signature") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "AI ship not in space - undock first"; return false; }
        std::string p(params);
        std::string sigID;
        size_t sp = p.find("\"sigID\"");
        if (sp != std::string::npos) {
            size_t colon = p.find(":", sp);
            size_t q1 = (colon != std::string::npos) ? p.find("\"", colon) : std::string::npos;
            if (q1 != std::string::npos) { size_t q2 = p.find("\"", q1 + 1); if (q2 != std::string::npos) sigID = p.substr(q1 + 1, q2 - q1 - 1); }
        }
        if (sigID.empty()) { resultMsg = "missing 'sigID' in params"; return false; }
        int32 distance = 10000;
        size_t dPos = p.find("\"distance\"");
        if (dPos != std::string::npos) { size_t c = p.find(":", dPos); if (c != std::string::npos) distance = (int32)atol(p.c_str() + c + 1); }
        SystemManager* pSystem = pAIShip->SystemMgr();
        if (pSystem == nullptr) { resultMsg = "AI ship has no system"; return false; }
        AnomalyMgr* pAM = pSystem->GetAnomMgr();
        if (pAM == nullptr) { resultMsg = "no anomaly manager"; return false; }
        std::vector<CosmicSignature> sigs;
        pAM->GetSignatureList(sigs);
        pAM->GetAnomalyList(sigs);   // accept either list by sigID
        GPoint warpPoint;
        std::string sname;
        bool found = false;
        for (auto& s : sigs) {
            if (s.sigID == sigID) { warpPoint = s.position; sname = s.sigName; found = true; break; }
        }
        if (!found) { resultMsg = "signature not found in current system"; return false; }
        pAIShip->DestinyMgr()->WarpTo(warpPoint, distance);
        char wbuf[128];
        snprintf(wbuf, sizeof(wbuf), "warping to signature %s (%s)", sigID.c_str(), sname.c_str());
        resultMsg = wbuf;
        return true;
    }
    // =================== end VEV_WARP_TO_SIGNATURE ===================

    // =================== VEV_ANALYZE ===================
    // Hack a relic (Magnetometric/3, Analyzer 22177) or data (Radar/4, Codebreaker
    // 22175) site by sigID -- the non-combat exploration payoff. The AI bypasses the
    // physical-can targeting + probe minigame (cf. the Client*-bound Scan/Prospector
    // classes): warp to the signature, run the analyzer, roll the hack, drop relic/data
    // loot to cargo. Roll mirrors Prospector::CheckSuccess (Data_Miner bonus, attr 902).
    // On success the site is consumed (RemoveSignal). params: {"sigID":"X","moduleID":N}.
    if (cmd == "analyze") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "AI ship not in space - undock first"; return false; }
        std::string p(params);
        std::string sigID;
        size_t sp = p.find("\"sigID\"");
        if (sp != std::string::npos) {
            size_t colon = p.find(":", sp);
            size_t q1 = (colon != std::string::npos) ? p.find("\"", colon) : std::string::npos;
            if (q1 != std::string::npos) { size_t q2 = p.find("\"", q1 + 1); if (q2 != std::string::npos) sigID = p.substr(q1 + 1, q2 - q1 - 1); }
        }
        uint32 moduleID = 0;
        size_t mPos = p.find("\"moduleID\"");
        if (mPos != std::string::npos) { size_t c = p.find(":", mPos); if (c != std::string::npos) moduleID = (uint32)atol(p.c_str() + c + 1); }
        if (sigID.empty()) { resultMsg = "missing 'sigID' in params"; return false; }
        SystemManager* pSystem = pAIShip->SystemMgr();
        if (pSystem == nullptr) { resultMsg = "AI ship has no system"; return false; }
        AnomalyMgr* pAM = pSystem->GetAnomMgr();
        if (pAM == nullptr) { resultMsg = "no anomaly manager"; return false; }
        std::vector<CosmicSignature> sigs;
        pAM->GetSignatureList(sigs);
        CosmicSignature sigHit;
        bool found = false;
        for (auto& s : sigs) { if (s.sigID == sigID) { sigHit = s; found = true; break; } }
        if (!found) { resultMsg = "signature not found in current system"; return false; }
        if (sigHit.dungeonType != 3 && sigHit.dungeonType != 4) {
            resultMsg = "not a relic/data site (only Magnetometric/Radar are hackable)";
            return false;
        }
        InventoryItemRef modRef = sItemFactory.GetItemRef(moduleID);
        if (modRef.get() == nullptr || modRef->groupID() != 538) {
            resultMsg = "no Codebreaker/Analyzer (Data Miner) module provided";
            return false;
        }
        const double hackDist = pAIShip->GetPosition().distance(sigHit.position);
        if (hackDist > 30000.0) {   // VEV_XPL: must be AT the site (approach the cans), not a 100km range-hack
            char hbuf2[128];
            snprintf(hbuf2, sizeof(hbuf2), "too far from site: %.0fm (warp to the signature first)", hackDist);
            resultMsg = hbuf2;
            return false;
        }
        // VEV_HACK_SKILL_MATH (2026-06-16): success scales with the pilot's relevant
        // hacking skill vs site difficulty -- not a flat roll. Data (Radar) keys off
        // Hacking (21718); relic (Magnetometric) off Archaeology (13278) -- EVE's real
        // virus-coherence skills. Read the trained level from entity/entity_attributes
        // (flag=7 skill, attr 280=level) so it works for offline phantoms (mirrors get_skills).
        const uint32 vevHackSkill = (sigHit.dungeonType == 4) ? 21718 : 13278;
        int vevSkillLvl = 0;
        {
            DBQueryResult sres;
            if (sDatabase.RunQuery(sres,
                "SELECT COALESCE(al.valueInt, al.valueFloat, 0) FROM entity e "
                "LEFT JOIN entity_attributes al ON al.itemID = e.itemID AND al.attributeID = 280 "
                "WHERE e.ownerID = %u AND e.flag = 7 AND e.typeID = %u LIMIT 1", charID, vevHackSkill)) {
                DBResultRow srow;
                if (sres.GetRow(srow) && !srow.IsNull(0)) vevSkillLvl = srow.GetInt(0);
            }
        }
        int hbonus = modRef->HasAttribute(AttrAccessDifficultyBonus) ? modRef->GetAttribute(AttrAccessDifficultyBonus).get_int() : 5;
        // base 45 + 8/skill-level (0..40) + module bonus - site difficulty (relic runs
        // a tougher virus than data). Skill-0 pilot ~38-44%; maxed + good module ~95%.
        const int vevDiff = (sigHit.dungeonType == 3) ? 12 : 6;
        int hchance = 45 + vevSkillLvl * 8 + hbonus - vevDiff;
        if (hchance > 95) hchance = 95;
        if (hchance < 15) hchance = 15;
        if (MakeRandomInt(0, 100) >= hchance) {
            char hfail[128];
            snprintf(hfail, sizeof(hfail), "hack failed (%d%% chance) - cycle again", hchance);
            resultMsg = hfail;
            return true;   // a failed roll is still a completed cycle
        }
        // SUCCESS: drop relic/data loot into cargo (data=datacores, relic=salvage mats)
        // VEV_XPL_LOOT_TABLES (2026-06-16): authentic value-weighted relic/data drops
        // grounded in evemu invTypes (relic = Salvaged Materials grp 754; data =
        // Datacores grp 333). Cheap salvage common, multi-million Intact pieces / top
        // datacores rare -> a site mostly pays modestly with an occasional jackpot, like
        // real EVE. Drop COUNT scales up in lower-sec (richer) space.
        struct VevLoot { uint32 typeID; int weight; };
        static const VevLoot relicTbl[] = {
            {25599,30},{25604,30},{25613,28},{25611,26},{25607,24},{25598,22},
            {25605,12},{25595,12},{25609,11},{25606,10},{25620,9},
            {25619,4},{25617,3},
            {25624,1},{25622,1},{25608,1},{25625,1}};
        static const VevLoot dataTbl[] = {
            {20424,20},{20414,20},{20423,18},{20418,16},{20172,14},
            {20417,10},{20415,10},{20411,9},{20412,8},
            {20171,3},{20416,3},{20413,2}};
        const VevLoot* tbl = (sigHit.dungeonType == 4) ? dataTbl : relicTbl;
        const int tblN = (sigHit.dungeonType == 4) ? (int)(sizeof(dataTbl)/sizeof(VevLoot))
                                                   : (int)(sizeof(relicTbl)/sizeof(VevLoot));
        int wsum = 0; for (int wi = 0; wi < tblN; ++wi) wsum += tbl[wi].weight;
        const float vevSec = pSystem->GetSystemSecurityRating();
        int drops = 2 + MakeRandomInt(0, 1) + (vevSec < 0.45f ? 1 : 0) + (vevSec <= 0.0f ? 1 : 0);
        ShipItemRef hkShip = ShipItemRef::StaticCast(pAIShip->GetSelf());
        Inventory* hkInv = (hkShip.get() != nullptr) ? hkShip->GetMyInventory() : nullptr;
        std::string got;
        int kinds = 0;
        for (int di = 0; di < drops; ++di) {
            int roll = MakeRandomInt(0, wsum - 1), pick = 0, acc = 0;
            for (int wi = 0; wi < tblN; ++wi) { acc += tbl[wi].weight; if (roll < acc) { pick = wi; break; } }
            const uint32 qty = (uint32)MakeRandomInt(1, 3);
            ItemData lootData(tbl[pick].typeID, charID, locTemp, flagNone, qty);
            InventoryItemRef lref = sItemFactory.SpawnItem(lootData);
            if (lref.get() == nullptr) continue;
            if (hkInv != nullptr && hkInv->HasAvailableSpace(flagCargoHold, lref)) {
                lref->MergeTypesInCargo(hkShip.get(), flagCargoHold);
                char lrow[96];
                snprintf(lrow, sizeof(lrow), " %ux %s;", qty, lref->name());
                got += lrow;
                kinds++;
            } else {
                lref->Delete();
                break;
            }
        }
        pAM->RemoveSignal(sigHit.sigItemID);   // site consumed
        char abuf[320];
        snprintf(abuf, sizeof(abuf), "%s SUCCESS (%d%%): %d kind(s) -%s site consumed",
                 (sigHit.dungeonType == 4) ? "data hack" : "relic analyze", hchance, kinds, kinds ? got.c_str() : " nothing;");
        sLog.Cyan("activate_module", "ANALYZE char=%u %s", charID, abuf);
        resultMsg = abuf;
        return true;
    }
    // =================== end VEV_ANALYZE ===================

    // =================== VEV_AGENT_MISSIONS ===================
    // The cycle-2 headline: AI pilots ACCEPT and COMPLETE real agent missions
    // for real rewardISK. Mirrors AgentBound::DoAction's Accept/Complete arms
    // (AgentBound.cpp) minus the Client* coupling; payout is byte-identical
    // TransferFunds (the VEV_SIM_BOUNTY-proven phantom path). State persists
    // in agtOffers (engine python polls objective/progress there).
    // LP + standings: marker-TODO (Client*-coupled; not ISK-load-bearing).

    // accept_mission: {"agentID": 3013180, "missionType": "mining"|"courier"|"encounter"?}
    // Docked-at-the-agent's-station only. Re-rolls up to 10x for missionType.
    if (cmd == "accept_mission") {
        std::string p(params);
        uint32 agentID = 0;
        size_t aPos = p.find("\"agentID\"");
        if (aPos != std::string::npos) {
            size_t c = p.find(":", aPos);
            if (c != std::string::npos) agentID = (uint32)atol(p.c_str() + c + 1);
        }
        if (agentID == 0) { resultMsg = "missing 'agentID' in params"; return false; }
        std::string wantType;
        size_t tPos = p.find("\"missionType\"");
        if (tPos != std::string::npos) {
            size_t q1 = p.find("\"", p.find(":", tPos) + 1);
            size_t q2 = (q1 == std::string::npos) ? std::string::npos : p.find("\"", q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                wantType = p.substr(q1 + 1, q2 - q1 - 1);
        }
        if (!IsPhantomPlayer(charID)) { resultMsg = "not a phantom player — login first"; return false; }
        Agent* pAgent = sEntityList.GetAgent(agentID);
        if (pAgent == nullptr) { resultMsg = "agent not found: " + std::to_string(agentID); return false; }
        // docked at the agent's station?
        uint32 vevStation = 0;
        {
            DBQueryResult sres; DBResultRow srow;
            if (sDatabase.RunQuery(sres,
                "SELECT COALESCE(stationID,0) FROM chrCharacters WHERE characterID = %u", charID)
                && sres.GetRow(srow))
                vevStation = srow.GetUInt(0);
        }
        if (vevStation == 0 || vevStation != pAgent->GetStationID()) {
            char wbuf[128];
            snprintf(wbuf, sizeof(wbuf), "wrong station|docked=%u|agent_at=%u",
                     vevStation, pAgent->GetStationID());
            resultMsg = wbuf;
            return false;
        }
        char buf[1024];
        MissionOffer offer = MissionOffer();
        bool have = pAgent->HasMission(charID, offer);
        if (have && offer.stateID == Mission::State::Accepted) {
            snprintf(buf, sizeof(buf),
                "already_active|%u|%s|objTypeID=%u|objQty=%u|dest=%u|reward=%.0f",
                offer.missionID, offer.name.c_str(), offer.courierTypeID,
                offer.courierAmount, offer.destinationID, (double)offer.rewardISK);
            resultMsg = buf;
            return true;
        }
        if (!have) {
            int tries = 0;
            uint8 want = 0;
            if (wantType == "mining") want = Mission::Type::Mining;
            else if (wantType == "courier") want = Mission::Type::Courier;
            else if (wantType == "encounter") want = Mission::Type::Encounter;
            do {
                if (tries++ > 0) pAgent->DeleteOffer(charID);
                offer = MissionOffer();
                pAgent->MakeOffer(charID, offer);
                if (offer.destinationID == 0 && offer.missionID == 0) {
                    resultMsg = "agent could not generate an offer (destination roll failed)";
                    return false;
                }
            } while (want != 0 && offer.typeID != want && tries < 10);
            if (want != 0 && offer.typeID != want) {
                snprintf(buf, sizeof(buf),
                    "could not roll requested type after %d tries, last=%u", tries, offer.typeID);
                resultMsg = buf;
                return false;
            }
        }
        // ACCEPT — mirror AgentBound Accept minus Client*
        offer.stateID = Mission::State::Accepted;
        offer.dateAccepted = GetFileTimeNow();
        offer.expiryTime = GetFileTimeNow() + (30 * pAgent->GetLevel() * EvE::Time::Minute);
        if (offer.typeID == Mission::Type::Courier && offer.courierTypeID) {
            // courier package straight into the origin-station hangar
            ItemData cdata(offer.courierTypeID, charID, offer.originID,
                           flagHangar, offer.courierAmount);
            sItemFactory.SpawnItem(cdata);
        } else if (offer.typeID == Mission::Type::Encounter) {
            pAgent->SetupEncounterMission(offer);   // mission_test-proven phantom path
        }
        // Mining spawns nothing (A382) — the pilot mines the objective itself.
        pAgent->UpdateOffer(charID, offer);
        snprintf(buf, sizeof(buf),
            "accepted|%u|type=%u|%s|objTypeID=%u|objQty=%u|dest=%u|reward=%.0f|bonus=%.0f/%umin",
            offer.missionID, offer.typeID, offer.name.c_str(), offer.courierTypeID,
            offer.courierAmount, offer.destinationID,
            (double)offer.rewardISK, (double)offer.bonusISK, offer.bonusTime);
        resultMsg = buf;
        return true;
    }

    // complete_mission: {"agentID": 3013180} — docked at the DESTINATION,
    // goods in station hangar or ship cargo; pays rewardISK (+time bonus).
    if (cmd == "complete_mission") {
        std::string p(params);
        uint32 agentID = 0;
        size_t aPos = p.find("\"agentID\"");
        if (aPos != std::string::npos) {
            size_t c = p.find(":", aPos);
            if (c != std::string::npos) agentID = (uint32)atol(p.c_str() + c + 1);
        }
        if (agentID == 0) { resultMsg = "missing 'agentID' in params"; return false; }
        if (!IsPhantomPlayer(charID)) { resultMsg = "not a phantom player — login first"; return false; }
        Agent* pAgent = sEntityList.GetAgent(agentID);
        if (pAgent == nullptr) { resultMsg = "agent not found: " + std::to_string(agentID); return false; }
        MissionOffer offer = MissionOffer();
        if (!pAgent->HasMission(charID, offer)) {
            resultMsg = "no mission with this agent";
            return false;
        }
        char buf[1024];
        if (offer.stateID != Mission::State::Accepted) {
            snprintf(buf, sizeof(buf), "mission not in accepted state|state=%u", offer.stateID);
            resultMsg = buf;
            return false;
        }
        // expired? mirror MissionDataMgr::Process's failure arm
        if (offer.expiryTime > 0 && offer.expiryTime < GetFileTimeNow()) {
            offer.stateID = Mission::State::Failed;
            pAgent->UpdateOffer(charID, offer);
            pAgent->RemoveOffer(charID);
            resultMsg = "expired|mission failed";
            return true;
        }
        uint32 vevStation = 0, vevShipID = 0;
        {
            DBQueryResult sres; DBResultRow srow;
            if (sDatabase.RunQuery(sres,
                "SELECT COALESCE(stationID,0), COALESCE(shipID,0) FROM chrCharacters WHERE characterID = %u",
                charID) && sres.GetRow(srow)) {
                vevStation = srow.GetUInt(0);
                vevShipID = srow.GetUInt(1);
            }
        }
        if (offer.typeID == Mission::Type::Encounter) {
            SystemBubble* pBubble = pAgent->EnsureEncounterBubble(offer);
            if (pBubble == nullptr || !pBubble->IsMission() || pBubble->CountNPCs() > 0) {
                snprintf(buf, sizeof(buf), "npcs remaining|%u",
                         pBubble != nullptr ? pBubble->CountNPCs() : 0);
                resultMsg = buf;
                return false;
            }
        } else if (offer.courierTypeID) {
            if (vevStation == 0 || vevStation != offer.destinationID) {
                snprintf(buf, sizeof(buf), "wrong station|docked=%u|dest=%u",
                         vevStation, offer.destinationID);
                resultMsg = buf;
                return false;
            }
            // count the goods: destination hangar + active-ship cargo
            DBQueryResult ires; DBResultRow irow;
            uint32 have = 0;
            std::vector<std::pair<uint32, uint32>> vevStacks;  // itemID, qty
            if (sDatabase.RunQuery(ires,
                "SELECT itemID, quantity FROM entity WHERE ownerID = %u AND typeID = %u "
                "AND ((locationID = %u AND flag = %u) OR (locationID = %u AND flag = %u))",
                charID, offer.courierTypeID,
                vevStation, (uint32)flagHangar, vevShipID, (uint32)flagCargoHold)) {
                while (ires.GetRow(irow)) {
                    vevStacks.push_back({irow.GetUInt(0), irow.GetUInt(1)});
                    have += irow.GetUInt(1);
                }
            }
            if (have < offer.courierAmount) {
                snprintf(buf, sizeof(buf), "insufficient items|have=%u|need=%u|typeID=%u",
                         have, offer.courierAmount, offer.courierTypeID);
                resultMsg = buf;
                return false;
            }
            // turn-in: remove exactly courierAmount via the live item API
            uint32 toRemove = offer.courierAmount;
            for (auto& st : vevStacks) {
                if (toRemove == 0) break;
                InventoryItemRef iRef = sItemFactory.GetItemRef(st.first);
                if (iRef.get() == nullptr) continue;
                if (st.second > toRemove) {
                    iRef->AlterQuantity(-(int32)toRemove, true);
                    toRemove = 0;
                } else {
                    toRemove -= st.second;
                    iRef->Delete();
                }
            }
        }
        // COMPLETE — mirror AgentBound Complete minus Client*
        offer.stateID = Mission::State::Completed;
        offer.dateCompleted = GetFileTimeNow();
        pAgent->UpdateOffer(charID, offer);
        if (offer.rewardItemID) {
            ItemData rdata(offer.rewardItemID, charID,
                           pAgent->GetStationID(), flagHangar,
                           offer.rewardItemQty > 0 ? offer.rewardItemQty : 1);
            sItemFactory.SpawnItem(rdata);
        }
        double vevPaidBonus = 0;
        if (offer.rewardISK)
            AccountService::TransferFunds(pAgent->GetID(), charID, offer.rewardISK,
                "Mission Reward", Journal::EntryType::AgentMissionReward, pAgent->GetID());
        if ((offer.bonusTime > 0) and (offer.dateAccepted > 0)) {
            double bonusEnd = offer.dateAccepted + (double)offer.bonusTime * EvE::Time::Minute;
            if (GetFileTimeNow() < bonusEnd) {
                AccountService::TransferFunds(pAgent->GetID(), charID, offer.bonusISK,
                    "Mission Bonus Reward", Journal::EntryType::AgentMissionTimeBonusReward,
                    pAgent->GetID());
                vevPaidBonus = (double)offer.bonusISK;
            }
        }
        // VEV_CORP_TAX: EVE-real corp tax on agent ISK rewards (mission reward +
        // time bonus). The corp takes its taxRate cut into the corp wallet — the
        // way EVE taxes bounties + mission rewards (market sales are untaxed). Funds
        // the corp treasury (ships/fits/skillbooks); mirrors the PI export-tax sink.
        {
            double vevGross = (double)offer.rewardISK + vevPaidBonus;
            if (vevGross > 0.0) {
                uint32 vevCorpID = 0; double vevTaxRate = 0.0;
                DBQueryResult tres; DBResultRow trow;
                if (sDatabase.RunQuery(tres,
                    "SELECT c.corporationID, COALESCE(co.taxRate,0) FROM chrCharacters c "
                    "JOIN crpCorporation co ON co.corporationID=c.corporationID "
                    "WHERE c.characterID=%u", charID) && tres.GetRow(trow)) {
                    vevCorpID = trow.GetUInt(0);
                    vevTaxRate = trow.GetFloat(1);
                }
                double vevTax = vevGross * vevTaxRate;
                if (vevCorpID && vevTax > 0.0) {
                    AccountService::TransferFunds(charID, vevCorpID, vevTax,
                        "Corp Tax (mission reward)", Journal::EntryType::CorporationPayment,
                        pAgent->GetID());
                    sLog.Green("VEV_CORP_TAX", "char %u paid %.0f ISK (%.0f%%) corp tax to corp %u on mission reward", charID, vevTax, vevTaxRate*100.0, vevCorpID);
                }
            }
        }
        // TODO(VEV_AGENT_MISSIONS): rewardLP via LPService + standings via
        // sStandingMgr — both Client*-adjacent, neither ISK-load-bearing.
        pAgent->RemoveOffer(charID);
        snprintf(buf, sizeof(buf),
            "completed|%u|%s|reward=%.0f|bonus=%.0f|rewardItem=%ux%u",
            offer.missionID, offer.name.c_str(), (double)offer.rewardISK,
            vevPaidBonus, offer.rewardItemID,
            offer.rewardItemID ? (offer.rewardItemQty > 0 ? offer.rewardItemQty : 1) : 0);
        resultMsg = buf;
        return true;
    }
    // =================== end VEV_AGENT_MISSIONS ===================

    // ===================== VEV_PERCEPTION_V8 (conv8 perception lane) =====================
    // 6 read verbs bridging existing evemu state into the ai_command_queue surface.
    // House style mirrored from get_overview / get_hangar: manual parse, pipe-delimited result_msg.

    if (cmd == "get_wallet") {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res, "SELECT balance FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query wallet"; return false; }
        DBResultRow row;
        if (!res.GetRow(row)) { resultMsg = "character not found"; return false; }
        char buf[64];
        snprintf(buf, sizeof(buf), "balance|%.2f", row.GetDouble(0));
        resultMsg = buf;
        return true;
    }

    if (cmd == "get_local_pilots") {
        DBQueryResult sres;
        if (!sDatabase.RunQuery(sres, "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character"; return false; }
        DBResultRow srow;
        if (!sres.GetRow(srow)) { resultMsg = "character not found"; return false; }
        uint32 sysID = srow.GetUInt(0);
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT characterID, characterName, raceID FROM chrCharacters"
            " WHERE solarSystemID = %u AND online = 1 ORDER BY characterName", sysID)) {
            resultMsg = "failed to query local roster"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%u;", row.GetUInt(0), row.GetText(1), row.GetUInt(2));
            result += entry; count++;
            if (count >= 100) break;
        }
        char header[80];
        snprintf(header, sizeof(header), "%d in local (system %u): ", count, sysID);
        resultMsg = header + result;
        return true;
    }

    if (cmd == "read_channel") {
        std::string p(params);
        uint32 channelID = 0;
        size_t k = p.find("\"channel_id\"");
        if (k != std::string::npos) {
            size_t c = p.find(":", k);
            if (c != std::string::npos) channelID = (uint32)atol(p.c_str() + c + 1);
        }
        if (channelID == 0) {
            // VEV audit-fix: default to the pilot's CURRENT system Local channel (= solarSystemID),
            // where live agent traffic is. Legacy channel 100 is STALE (traffic migrated to the
            // system channel) -- a no-param read_channel on 100 returned silence.
            DBQueryResult cres;
            if (sDatabase.RunQuery(cres, "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", charID)) {
                DBResultRow crow;
                if (cres.GetRow(crow)) channelID = crow.GetUInt(0);
            }
            if (channelID == 0) channelID = 100;  // last-resort fallback
        }
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT senderID, senderName, message, created_at FROM ai_pilot_messages"
            " WHERE channelID = %u ORDER BY id DESC LIMIT 20", channelID)) {
            resultMsg = "failed to query channel"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            char entry[600];
            // NOTE: message is free text and may contain | or ; -- pipe format is lossy here;
            // JSON-for-this-verb is the recommended retrofit (see VEV_ROADMAP open decisions).
            snprintf(entry, sizeof(entry), "%u|%s|%s|%s;",
                row.GetUInt(0), row.GetText(1), row.GetText(2), row.GetText(3));
            result += entry; count++;
        }
        char header[80];
        snprintf(header, sizeof(header), "%d msgs on channel %u (newest first): ", count, channelID);
        resultMsg = header + result;
        return true;
    }

    if (cmd == "get_self_tank_state") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "no in-space ship (docked or offline)"; return false; }
        InventoryItemRef item = pAIShip->GetSelf();
        if (item.get() == nullptr) { resultMsg = "no ship item"; return false; }
        float shieldCap = item->GetAttribute(AttrShieldCapacity).get_float();
        float shieldChg = item->GetAttribute(AttrShieldCharge).get_float();
        float armorMax  = item->GetAttribute(AttrArmorHP).get_float();
        float armorDmg  = item->GetAttribute(AttrArmorDamage).get_float();
        float hullMax   = item->GetAttribute(AttrHP).get_float();
        float hullDmg   = item->GetAttribute(AttrDamage).get_float();
        float capCap    = item->GetAttribute(AttrCapacitorCapacity).get_float();
        float capChg    = item->GetAttribute(AttrCapacitorCharge).get_float();
        float shieldPct = (shieldCap > 0) ? (shieldChg / shieldCap * 100.0f) : 0.0f;
        float armorPct  = (armorMax > 0)  ? ((1.0f - armorDmg / armorMax) * 100.0f) : 0.0f;
        float hullPct   = (hullMax > 0)   ? ((1.0f - hullDmg / hullMax) * 100.0f) : 0.0f;
        float capPct    = (capCap > 0.0f) ? (capChg / capCap * 100.0f) : -1.0f;  // VEV_CAP_UNSENTINEL: cap now wired (VEV_SIM_CAPDEBIT/CAPRECHARGE) -- report REAL cap% (AIShipSE ctor seeds AttrCapacitorCharge full @AIShipSE.cpp:75; debit/recharge maintain it). -1 only if no cap attr.
        char buf[256];
        snprintf(buf, sizeof(buf),
            "shield_pct|%.1f|armor_pct|%.1f|hull_pct|%.1f|cap_pct|%.1f|"
            "shield_max|%.0f|armor_max|%.0f|hull_max|%.0f|cap_max|%.0f",
            shieldPct, armorPct, hullPct, capPct, shieldCap, armorMax, hullMax, capCap);
        resultMsg = buf;
        return true;
    }

    if (cmd == "get_belt_survey") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) { resultMsg = "AI ship not in space"; return false; }
        SystemBubble* pBubble = pAIShip->SysBubble();
        if (pBubble == nullptr) { resultMsg = "AI ship has no bubble"; return false; }
        std::map<uint32, SystemEntity*> entities;
        pBubble->GetEntities(entities);
        std::map<uint32, SystemEntity*> statics;
        pBubble->GetStaticEntities(statics);
        for (auto& kv : statics) entities.emplace(kv.first, kv.second);
        std::string result; int count = 0;
        for (auto& kv : entities) {
            SystemEntity* se = kv.second;
            if (se == nullptr || !se->IsAsteroidSE()) continue;
            double dist = pAIShip->DistanceTo2(se);
            float qty = 0.0f;
            InventoryItemRef item = se->GetSelf();
            if (item.get() != nullptr) qty = item->GetAttribute(AttrQuantity).get_float();
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%.0f|%.0f;",
                se->GetID(), se->GetName(), se->GetTypeID(), qty, dist);
            result += entry; count++;
            if (count >= 100) break;
        }
        if (result.empty()) { resultMsg = "no asteroids in bubble (not at a belt, or belt not spawned)"; return true; }
        char header[64];
        snprintf(header, sizeof(header), "%d asteroids: ", count);
        resultMsg = header + result;
        return true;
    }

    if (cmd == "get_market_prices") {
        std::string p(params);
        uint32 typeID = 0, regionID = 0;
        size_t kt = p.find("\"type_id\""); if (kt == std::string::npos) kt = p.find("\"typeID\"");
        if (kt != std::string::npos) { size_t c = p.find(":", kt); if (c != std::string::npos) typeID = (uint32)atol(p.c_str() + c + 1); }
        size_t kr = p.find("\"region_id\""); if (kr == std::string::npos) kr = p.find("\"regionID\"");
        if (kr != std::string::npos) { size_t c = p.find(":", kr); if (c != std::string::npos) regionID = (uint32)atol(p.c_str() + c + 1); }
        // CONV6_PATCH_market_prices_region_default — when region_id is omitted but type_id is
        // present, derive the region from the pilot's current location
        // (docked station OR in-space solar system). Falls through to the
        // legacy "params require..." error on any DB failure so the agent
        // still gets a clear hint instead of a silent stall.
        if (regionID == 0 && typeID != 0) {
            DBQueryResult locRes;
            if (sDatabase.RunQuery(locRes,
                "SELECT stationID, solarSystemID FROM chrCharacters WHERE characterID = %u", charID)) {
                DBResultRow locRow;
                if (locRes.GetRow(locRow)) {
                    uint32 stID = locRow.GetUInt(0);
                    uint32 syID = locRow.GetUInt(1);
                    uint32 lookupID = (stID > 0) ? stID : syID;
                    if (lookupID > 0) {
                        DBQueryResult regRes;
                        if (sDatabase.RunQuery(regRes,
                            "SELECT regionID FROM mapDenormalize WHERE itemID = %u", lookupID)) {
                            DBResultRow regRow;
                            if (regRes.GetRow(regRow)) {
                                regionID = regRow.GetUInt(0);
                            }
                        }
                    }
                }
            }
        }
        if (typeID == 0 || regionID == 0) { resultMsg = "params require {type_id, region_id}"; return false; }
        std::string result;
        DBQueryResult sres;
        if (!sDatabase.RunQuery(sres,
            "SELECT price, volRemaining, stationID FROM mktOrders"
            " WHERE typeID=%u AND regionID=%u AND CAST(bid AS UNSIGNED)=0 ORDER BY price ASC LIMIT 20", typeID, regionID)) {
            resultMsg = "failed to query sell orders"; return false; }
        int sc = 0; DBResultRow srow;
        while (sres.GetRow(srow)) { char e[128]; snprintf(e, sizeof(e), "sell|%.2f|%u|%u;", srow.GetDouble(0), srow.GetUInt(1), srow.GetUInt(2)); result += e; sc++; }
        DBQueryResult bres;
        if (!sDatabase.RunQuery(bres,
            "SELECT price, volRemaining, stationID FROM mktOrders"
            " WHERE typeID=%u AND regionID=%u AND CAST(bid AS UNSIGNED)=1 ORDER BY price DESC LIMIT 20", typeID, regionID)) {
            resultMsg = "failed to query buy orders"; return false; }
        int bc = 0; DBResultRow brow;
        while (bres.GetRow(brow)) { char e[128]; snprintf(e, sizeof(e), "buy|%.2f|%u|%u;", brow.GetDouble(0), brow.GetUInt(1), brow.GetUInt(2)); result += e; bc++; }
        char header[96];
        snprintf(header, sizeof(header), "type %u region %u: %d sell %d buy: ", typeID, regionID, sc, bc);
        resultMsg = header + result;
        return true;
    }
    if (cmd == "get_celestials") {
        DBQueryResult sres;
        if (!sDatabase.RunQuery(sres, "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character"; return false; }
        DBResultRow srow;
        if (!sres.GetRow(srow)) { resultMsg = "character not found"; return false; }
        uint32 sysID = srow.GetUInt(0);
        // mapDenormalize groupIDs: 6=Sun 7=Planet 8=Moon 9=Belt 10=Stargate 15=Station.
        // Subsumes get_system_belts / get_system_stations / get_system_stargates (consumer filters on kind).
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT itemID, itemName, typeID, groupID, x, y, z FROM mapDenormalize "
            "WHERE solarSystemID = %u AND groupID IN (6,7,8,9,10,15) ORDER BY groupID, itemID", sysID)) {
            resultMsg = "failed to query celestials"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            uint32 gid = row.GetUInt(3);
            const char* kind = "celestial";
            switch (gid) {
                case 6:  kind = "sun";      break;
                case 7:  kind = "planet";   break;
                case 8:  kind = "moon";     break;
                case 9:  kind = "belt";     break;
                case 10: kind = "stargate"; break;
                case 15: kind = "station";  break;
            }
            char entry[320];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%s|%.0f|%.0f|%.0f;",
                row.GetUInt(0), row.GetText(1), row.GetUInt(2), kind,
                row.GetDouble(4), row.GetDouble(5), row.GetDouble(6));
            result += entry; count++;
            if (count >= 200) break;
        }
        char header[80];
        snprintf(header, sizeof(header), "%d celestials (system %u): ", count, sysID);
        resultMsg = header + result;
        return true;
    }

    if (cmd == "get_bookmarks") {
        // Lifts the verified BOOKMARK_SELECT (Gateway.cpp handleListBookmarks) onto the queue surface.
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT b.bookmarkID, COALESCE(NULLIF(b.memo,''), d.itemName, ''), b.locationID, "
            "COALESCE(s.solarSystemName,''), b.x, b.y, b.z "
            "FROM bookmarks b "
            "LEFT JOIN mapDenormalize d ON d.itemID = b.itemID "
            "LEFT JOIN mapSolarSystems s ON s.solarSystemID = b.locationID "
            "WHERE b.ownerID = %u ORDER BY b.created DESC", charID)) {
            resultMsg = "failed to query bookmarks"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            char entry[320];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%s|%.0f|%.0f|%.0f;",
                row.GetUInt(0), row.GetText(1), row.GetUInt(2), row.GetText(3),
                row.GetDouble(4), row.GetDouble(5), row.GetDouble(6));
            result += entry; count++;
        }
        char header[48];
        snprintf(header, sizeof(header), "%d bookmarks: ", count);
        resultMsg = header + result;
        return true;
    }
    // ---- v8c cheap single-file reads (exact SQL verified live by the perception sweep) ----

    if (cmd == "get_location") {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT c.stationID, c.solarSystemID, COALESCE(s.solarSystemName,'?'), c.online "
            "FROM chrCharacters c LEFT JOIN mapSolarSystems s ON s.solarSystemID = c.solarSystemID "
            "WHERE c.characterID = %u", charID)) {
            resultMsg = "failed to query location"; return false; }
        DBResultRow row;
        if (!res.GetRow(row)) { resultMsg = "character not found"; return false; }
        uint32 stationID = row.GetUInt(0);
        uint32 sysID = row.GetUInt(1);
        int online = (row.GetUInt(3) != 0) ? 1 : 0;
        const char* dockState = (stationID != 0) ? "docked" : "in_space";
        char buf[320];
        if (stationID == 0) {
            AIShipSE* pAIShip = FindAIShip(charID);
            if (pAIShip != nullptr) {
                const GPoint& pos = pAIShip->GetPosition();
                snprintf(buf, sizeof(buf), "system|%u|%s|dock_state|%s|online|%d|x|%.0f|y|%.0f|z|%.0f",
                    sysID, row.GetText(2), dockState, online, pos.x, pos.y, pos.z);
            } else {
                snprintf(buf, sizeof(buf), "system|%u|%s|dock_state|%s|online|%d",
                    sysID, row.GetText(2), dockState, online);
            }
        } else {
            snprintf(buf, sizeof(buf), "system|%u|%s|dock_state|%s|stationID|%u|online|%d",
                sysID, row.GetText(2), dockState, stationID, online);
        }
        resultMsg = buf;
        return true;
    }

    if (cmd == "get_environment") {
        DBQueryResult sres;
        if (!sDatabase.RunQuery(sres, "SELECT solarSystemID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character"; return false; }
        DBResultRow srow;
        if (!sres.GetRow(srow)) { resultMsg = "character not found"; return false; }
        uint32 sysID = srow.GetUInt(0);
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT COALESCE(s.solarSystemName,'?'), s.security, COALESCE(s.securityClass,'?'), "
            "COALESCE(f.factionName,'none'), COALESCE(r.regionName,'?') "
            "FROM mapSolarSystems s "
            "LEFT JOIN facFactions f ON f.factionID = s.factionID "
            "LEFT JOIN mapRegions r ON r.regionID = s.regionID "
            "WHERE s.solarSystemID = %u", sysID)) {
            resultMsg = "failed to query environment"; return false; }
        DBResultRow row;
        if (!res.GetRow(row)) { resultMsg = "system not found"; return false; }
        char buf[400];
        snprintf(buf, sizeof(buf), "system|%s|security|%.2f|sec_class|%s|faction|%s|region|%s",
            row.GetText(0), row.GetDouble(1), row.GetText(2), row.GetText(3), row.GetText(4));
        resultMsg = buf;
        return true;
    }

    if (cmd == "get_skill_queue") {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT q.orderIndex, q.typeID, COALESCE(t.typeName,'?'), q.level "
            "FROM chrSkillQueue q LEFT JOIN invTypes t ON t.typeID = q.typeID "
            "WHERE q.characterID = %u ORDER BY q.orderIndex", charID)) {
            resultMsg = "failed to query skill queue"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%u|%s|%u;",
                row.GetUInt(0), row.GetUInt(1), row.GetText(2), row.GetUInt(3));
            result += entry; count++;
        }
        char header[64];
        snprintf(header, sizeof(header), "%d in skill queue%s: ", count, count == 0 ? " (IDLE-training wasted)" : "");
        resultMsg = header + result;
        return true;
    }

    // CONV6_PATCH_get_skills_bridge --- read CURRENT trained skills (typeID|level|sp triples).
    // Direct DB join: entity rows where ownerID=charID + flag=7 (flagSkill),
    // LEFT JOIN entity_attributes for attribute 280 (AttrSkillLevel) and 276
    // (AttrSkillPoints). Pattern mirrors Gateway.cpp:1611-1613 (3D char-sheet
    // skill query), so the read works for offline pilots too --- no Client*
    // and no in-memory inventory dependency.
    if (cmd == "get_skills") {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT e.typeID, COALESCE(t.typeName,'?'), "
            "       COALESCE(al.valueInt, al.valueFloat, 0), "
            "       COALESCE(ap.valueInt, ap.valueFloat, 0) "
            "FROM entity e "
            "LEFT JOIN invTypes t ON t.typeID = e.typeID "
            "LEFT JOIN entity_attributes al ON al.itemID = e.itemID AND al.attributeID = 280 "
            "LEFT JOIN entity_attributes ap ON ap.itemID = e.itemID AND ap.attributeID = 276 "
            "WHERE e.ownerID = %u AND e.flag = 7 "
            "ORDER BY t.typeName", charID)) {
            resultMsg = "failed to query skills"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%lld;",
                row.GetUInt(0), row.GetText(1),
                row.GetUInt(2), (long long)row.GetInt64(3));
            result += entry; count++;
        }
        char header[80];
        snprintf(header, sizeof(header), "%d skills: ", count);
        resultMsg = header + result;
        return true;
    }

    if (cmd == "get_fitting") {
        uint32 shipID = 0;
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip != nullptr && pAIShip->GetSelf().get() != nullptr) {
            shipID = pAIShip->GetSelf()->itemID();
        } else {
            DBQueryResult sres;
            if (sDatabase.RunQuery(sres, "SELECT shipID FROM chrCharacters WHERE characterID = %u", charID)) {
                DBResultRow srow;
                if (sres.GetRow(srow)) shipID = srow.GetUInt(0);
            }
        }
        if (shipID == 0) { resultMsg = "no ship"; return false; }
        // module slot flags: med 11-18, lo 19-26, hi 27-34, rig 92-99
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT e.flag, e.typeID, COALESCE(t.typeName,'?'), COALESCE(g.categoryID,0) "
            "FROM entity e LEFT JOIN invTypes t ON t.typeID = e.typeID "
            "LEFT JOIN invGroups g ON g.groupID = t.groupID "
            "WHERE e.locationID = %u AND (e.flag BETWEEN 11 AND 34 OR e.flag BETWEEN 92 AND 99) ORDER BY e.flag", shipID)) {
            resultMsg = "failed to query fitting"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%u|%s|%u;",
                row.GetUInt(0), row.GetUInt(1), row.GetText(2), row.GetUInt(3));
            result += entry; count++;
        }
        char header[64];
        snprintf(header, sizeof(header), "%d fitted (ship %u): ", count, shipID);
        resultMsg = header + result;
        return true;
    }

    if (cmd == "resolve_affiliation") {
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT c.corporationID, COALESCE(NULLIF(o.ownerName,''), crp.corporationName, '?'), "
            "COALESCE(crp.allianceID,0), COALESCE(aln.allianceName,'none') "
            "FROM chrCharacters c "
            "LEFT JOIN eveStaticOwners o ON o.ownerID = c.corporationID "
            "LEFT JOIN crpCorporation crp ON crp.corporationID = c.corporationID "
            "LEFT JOIN alnAlliance aln ON aln.allianceID = crp.allianceID "
            "WHERE c.characterID = %u", charID)) {
            resultMsg = "failed to query affiliation"; return false; }
        DBResultRow row;
        if (!res.GetRow(row)) { resultMsg = "character not found"; return false; }
        uint32 corpID = row.GetUInt(0);
        if (corpID == 0) { resultMsg = "corpID|0|corp|(no player corp)|allianceID|0|alliance|none"; return true; }  // VEV audit-fix: corp-0 guard
        char buf[320];
        snprintf(buf, sizeof(buf), "corpID|%u|corp|%s|allianceID|%u|alliance|%s",
            corpID, row.GetText(1), row.GetUInt(2), row.GetText(3));
        resultMsg = buf;
        return true;
    }
    // ---- v8c+ saturation-pass reads (surfaced by cross-checking the live card deck) ----

    if (cmd == "get_corp_members") {
        DBQueryResult cres;
        if (!sDatabase.RunQuery(cres, "SELECT corporationID FROM chrCharacters WHERE characterID = %u", charID)) {
            resultMsg = "failed to query character"; return false; }
        DBResultRow crow;
        if (!cres.GetRow(crow)) { resultMsg = "character not found"; return false; }
        uint32 myCorp = crow.GetUInt(0);
        // VEV audit-fix: corp-0 guard -- without it the old subselect returned ALL corp-0 pilots as a bogus roster
        if (myCorp == 0) { resultMsg = "0 corp members (not in a player corp)"; return true; }
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT m.characterID, m.characterName, m.online FROM chrCharacters m "
            "WHERE m.corporationID = %u ORDER BY m.online DESC, m.characterID", myCorp)) {
            resultMsg = "failed to query corp members"; return false; }
        std::string result; int count = 0, online = 0; DBResultRow row;
        while (res.GetRow(row)) {
            int on = (row.GetUInt(2) != 0) ? 1 : 0;
            if (on) online++;
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%d;", row.GetUInt(0), row.GetText(1), on);
            result += entry; count++;
            if (count >= 200) break;
        }
        char header[80];
        snprintf(header, sizeof(header), "%d corp members (%d online): ", count, online);
        resultMsg = header + result;
        return true;
    }

    if (cmd == "get_insurance") {
        // READ is live; shipInsurance stays EMPTY until the insure_ship WRITE is verified in evemu
        // (wealth/conv6 to confirm) -- returns [] not an error in that case.
        DBQueryResult res;
        if (!sDatabase.RunQuery(res,
            "SELECT shipID, shipName, fraction, payOutAmount FROM shipInsurance WHERE ownerID = %u", charID)) {
            resultMsg = "failed to query insurance"; return false; }
        std::string result; int count = 0; DBResultRow row;
        while (res.GetRow(row)) {
            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%.3f|%u;",
                row.GetUInt(0), row.GetText(1), row.GetDouble(2), row.GetUInt(3));
            result += entry; count++;
        }
        char header[64];
        snprintf(header, sizeof(header), "%d insured ships: ", count);
        resultMsg = header + result;
        return true;
    }

    // ===================== VEV_DSCAN_V1 (conv4 awareness lane) =====================
    // Directional scan verb — mirrors exploration/Scan.cpp:100 Scan::ConeScan
    // algorithm but accepts an AIShipSE caller (no Client* needed).
    // Params: {angle_deg: 5-360, range_m: 1-14400000000, direction_xyz?: [x,y,z]}
    // 360-deg default = full dome scan (no direction filter).

    if (cmd == "get_dscan") {
        AIShipSE* pAIShip = FindAIShip(charID);
        if (pAIShip == nullptr) {
            resultMsg = "AI ship not in space — undock first";
            return false;
        }

        // Manual param parse (matches house style)
        std::string p(params);
        float angle = 360.0f;            // default: full dome
        double range = 14400000000.0;    // default: max ~14 AU
        double dx = 1.0, dy = 0.0, dz = 0.0;
        bool has_direction = false;

        size_t aPos = p.find("\"angle_deg\"");
        if (aPos != std::string::npos) {
            size_t c = p.find(":", aPos);
            if (c != std::string::npos) angle = (float)atof(p.c_str() + c + 1);
        }
        size_t rPos = p.find("\"range_m\"");
        if (rPos != std::string::npos) {
            size_t c = p.find(":", rPos);
            if (c != std::string::npos) range = atof(p.c_str() + c + 1);
        }
        size_t dPos = p.find("\"direction_xyz\"");
        if (dPos != std::string::npos) {
            size_t br = p.find("[", dPos);
            if (br != std::string::npos) {
                if (sscanf(p.c_str() + br, "[%lf,%lf,%lf]", &dx, &dy, &dz) == 3) {
                    has_direction = true;
                }
            }
        }

        // Sanity caps
        if (angle < 5.0f) angle = 5.0f;
        if (angle > 360.0f) angle = 360.0f;
        if (range < 1.0) range = 1.0;
        if (range > 14400000000.0) range = 14400000000.0;

        // Get all entities within range via SystemMgr::DScan
        std::vector<SystemEntity*> seVec;
        const GPoint vertex(pAIShip->GetPosition());
        pAIShip->SystemMgr()->DScan((int64)range, vertex, seVec);

        // Cone-filter (skip filter when angle == 360 = full dome)
        float angle_half_rad = (angle * 3.14159265f / 180.0f) / 2.0f;
        bool do_cone_filter = (angle < 360.0f);
        GPoint U(dx, dy, dz);

        std::string result;
        int count = 0;
        for (auto se : seVec) {
            if (se == nullptr || se == pAIShip) continue;

            if (do_cone_filter) {
                GVector VR(vertex, se->GetPosition());
                VR.normalize();
                float dot = (float)U.dotProduct(VR);
                // Guard acos domain to avoid NaN on edge cases
                if (dot > 1.0f) dot = 1.0f;
                if (dot < -1.0f) dot = -1.0f;
                float acDP = (float)acos(dot);
                if (acDP >= angle_half_rad) continue;
            }

            InventoryItemRef item = se->GetSelf();
            uint32 typeID = (item.get() != nullptr) ? item->typeID() : 0;
            uint32 groupID = (item.get() != nullptr) ? item->groupID() : 0;

            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%u;",
                     se->GetID(), se->GetName(), typeID, groupID);
            result += entry;
            count++;
            if (count >= 200) break;   // hard cap
        }

        char header[160];
        snprintf(header, sizeof(header), "%d contacts (angle=%.1f range=%.0f%s): ",
                 count, angle, range, has_direction ? " directed" : " dome");
        resultMsg = std::string(header) + result;
        return true;
    }
    // =================== end VEV_DSCAN_V1 ===================

    // =================== end VEV_PERCEPTION_V8 ===================

    // Unknown command
    resultMsg = "unknown command: " + cmd;
    return false;
}
