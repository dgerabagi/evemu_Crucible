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
*/

#include "eve-server.h"

#include "EntityList.h"
#include "Client.h"
#include "station/Station.h"
#include "station/StationService.h"

StationService::StationService() :
    Service("station", eAccessLevel_Station)
{
    this->Add("GetSolarSystem", &StationService::GetSolarSystem);
    this->Add("GetGuests", &StationService::GetGuests);
}

PyResult StationService::GetSolarSystem(PyCallArgs &call, PyInt* solarSystemID) {
    // this needs to return some cache status?
    return new PyObject("util.CachedObject", solarSystemID);
}

PyResult StationService::GetGuests(PyCallArgs &call) {
    std::vector<Client*> clients;
    clients.clear();
    sEntityList.GetStationGuestList(call.client->GetStationID(), clients);
    PyList* res = new PyList();
    for (auto cur : clients) {
        PyTuple* t = new PyTuple(4);
			t->items[0] = new PyInt(cur->GetCharacterID());
			t->items[1] = new PyInt(cur->GetCorporationID());
			t->items[2] = new PyInt(cur->GetAllianceID());
			t->items[3] = new PyInt(cur->GetWarFactionID());
        res->AddItem(t);
    }

    // See A321 §4.7 — Include phantom AI characters in station guest list
    // Query DB directly since phantom players may have logged in before system/station was loaded
    {
        uint32 stationID = call.client->GetStationID();
        const std::set<uint32>& phantoms = sEntityList.GetPhantomPlayers();
        for (uint32 phantomID : phantoms) {
            DBQueryResult charRes;
            if (!sDatabase.RunQuery(charRes,
                "SELECT c.stationID, c.corporationID, IFNULL(corp.allianceID,0), IFNULL(corp.warFactionID,0)"
                " FROM chrCharacters c"
                " LEFT JOIN crpCorporation corp ON corp.corporationID = c.corporationID"
                " WHERE c.characterID = %u", phantomID))
                continue;
            DBResultRow row;
            if (!charRes.GetRow(row))
                continue;
            if (row.GetUInt(0) != stationID)
                continue;  // phantom is at a different station
            PyTuple* t = new PyTuple(4);
                t->items[0] = new PyInt(phantomID);
                t->items[1] = new PyInt(row.GetUInt(1));
                t->items[2] = new PyInt(row.GetUInt(2));
                t->items[3] = new PyInt(row.GetUInt(3));
            res->AddItem(t);
        }
    }

	return res;
}
