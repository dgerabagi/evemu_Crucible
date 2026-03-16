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
void EntityList::ProcessAICommandQueue() {
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
        if (!IsPhantomPlayer(charID)) {
            resultMsg = "character is not a phantom player — must login_docked first";
            return false;
        }
        if (HasAIShip(charID)) {
            resultMsg = "AI ship already in space for this character";
            return false;
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
        // Uses dock orientation as heading direction (same as Client::UndockFromStation)
        pAIShip->DestinyMgr()->Undock(stData.dockOrientation);

        // Track in EntityList for future command dispatch
        AddAIShip(charID, pAIShip);

        char buf[200];
        snprintf(buf, sizeof(buf), "undocked: %s in ship %u from station %u, heading away at max speed",
                 charName.c_str(), shipID, stationID);
        resultMsg = buf;
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
            // Docking range: 10km (generous — real EVE uses station model radius + ~500m)
            if (dist > 10000.0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "too far from station - distance: %.0fm, need < 10000m. Warp to station first.", dist);
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
                "SELECT itemName, x, y, z FROM mapDenormalize WHERE itemID = %u", targetID))
            {
                DBResultRow row;
                if (celestialRes.GetRow(row)) {
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
        uint32 moduleGroupID = modRow.GetUInt(3);

        // Determine if this is a mining module
        bool isMiningModule = false;
        // Strip Miner (464), Mining Laser (54), Frequency Mining Laser (483),
        // Gas Cloud Harvester (737), Mercoxit Mining Crystal (468)
        if (moduleGroupID == 464 || moduleGroupID == 54 || moduleGroupID == 483 || moduleGroupID == 737) {
            isMiningModule = true;
        }

        if (!isMiningModule) {
            resultMsg = "non-mining module activation not yet implemented (only mining supported)";
            return false;
        }

        // Verify target is an asteroid
        if (!pTarget->IsAsteroidSE()) {
            resultMsg = "target is not an asteroid — cannot mine";
            return false;
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
        ItemData idata(oreTypeID, shipRef->ownerID(), locTemp, flagNone, (uint32)oreAmount);
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

        char buf[256];
        snprintf(buf, sizeof(buf), "fitted %s to slot %u on ship %u%s",
                 modName.c_str(), slotFlag, shipID, swappedOut.c_str());
        resultMsg = buf;
        return true;
    }

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

        char buf[512];
        snprintf(buf, sizeof(buf),
            "pos=(%.0f, %.0f, %.0f) speed=%.1f state=%s bubble=%u isBelt=%s",
            pos.x, pos.y, pos.z, speed, stateName, bubbleID, isBelt ? "yes" : "no");
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
            else if (se->IsGateSE())   category = "gate";
            else continue;  // skip celestials, belts, etc.

            double distSq = pAIShip->DistanceTo2(se);
            double dist = sqrt(distSq);

            char entry[256];
            snprintf(entry, sizeof(entry), "%u|%s|%u|%s|%.0f;",
                     se->GetID(), se->GetName(), se->GetTypeID(), category, dist);
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

    // --- reprocess: Refine ore into minerals at station ---
    // See A331 §3.5 (inventory_management — action)
    // params: {"itemID": 140000999}
    //   itemID = the ore item to reprocess (must be in station hangar or ship cargo)
    // Returns: list of minerals produced
    // Only works when docked
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

        // Create output minerals in station hangar
        // Simplified: 100% efficiency (no skill/standing modifiers for AI)
        std::string result;
        int mineralCount = 0;
        for (auto& rec : recoverables) {
            uint32 mineralQty = rec.amountPerBatch * oreQty;
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

        // Delete the original ore
        oreRef->Delete();

        char header[128];
        snprintf(header, sizeof(header), "reprocessed %u units of ore into %d minerals: ", oreQty, mineralCount);
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

        // Create a cargo container (jetcan) in space at the ship's position
        // typeID 23 = Cargo Container
        uint32 systemID = pAIShip->SystemMgr()->GetID();
        ItemData cdata(23, charID, systemID, flagNone, 1);
        InventoryItemRef canRef = sItemFactory.SpawnItem(cdata);
        if (canRef.get() == nullptr) {
            resultMsg = "failed to create cargo container";
            return false;
        }

        // Move the item into the container
        iRef->Move(canRef->itemID(), flagCargoHold, true);
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
        // Truncate to 500 chars
        if (message.size() > 500) message.resize(500);

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

    // Unknown command
    resultMsg = "unknown command: " + cmd;
    return false;
}
