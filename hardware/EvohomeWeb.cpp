/**
 * Json client for UK/EMEA Evohome API
 *
 *  Adapted for integration with Domoticz
 *
 *  Copyright 2017 - gordonb3 https://github.com/gordonb3/evohomeclient
 *
 *  Licensed under GNU General Public License 3.0 or later. 
 *  Some rights reserved. See COPYING, AUTHORS.
 *
 *  @license GPL-3.0+ <https://github.com/gordonb3/evohomeclient/blob/master/LICENSE>
 */
#include "stdafx.h"
#include "EvohomeWeb.h"
#include "../main/Logger.h"
#include "hardwaretypes.h"
#include "../main/RFXtrx.h"
#include "../main/Helper.h"
#include "../main/SQLHelper.h"
#include "../main/localtime_r.h"
#include "../main/mainworker.h"

#include "../httpclient/HTTPClient.h"
#include "../json/json.h"
#include <string>

extern std::string szUserDataFolder;

const uint8_t CEvohomeWeb::m_dczToEvoWebAPIMode[7]={0,2,3,4,6,1,5};



CEvohomeWeb::CEvohomeWeb(const int ID, const std::string &Username, const std::string &Password, const unsigned int refreshrate, const bool updatedev, const bool showschedule):
	m_username(Username),
	m_password(Password),
	m_refreshrate(refreshrate),
	m_updatedev(!updatedev),
	m_showschedule(showschedule)

{
	m_HwdID=ID;
	m_bSkipReceiveCheck = true;

	m_loggedon=false;
	m_tzoffset=-1;
	m_lastDST=-1;
	
	Init();
}


CEvohomeWeb::~CEvohomeWeb(void)
{
	m_bIsStarted=false;
}


void CEvohomeWeb::Init()
{
	LoginHeaders.clear();
	LoginHeaders.push_back("Authorization: Basic YjAxM2FhMjYtOTcyNC00ZGJkLTg4OTctMDQ4YjlhYWRhMjQ5OnRlc3Q=");
	LoginHeaders.push_back("Accept: application/json, application/xml, text/json, text/x-json, text/javascript, text/xml");
	LoginHeaders.push_back("charsets: utf-8");
}



bool CEvohomeWeb::StartSession()
{
	m_loggedon=false;
	if (!login(m_username,m_password))
		return false;
	full_installation();
	m_tcs = NULL;
	if (is_single_heating_system())
		m_tcs = &locations[0].gateways[0].temperatureControlSystems[0];

	m_zones[0] = 0;
	m_loggedon=true;
	return true;
}


bool CEvohomeWeb::StartHardware()
{
	Init();
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&CEvohomeWeb::Do_Work, this)));
	if (!m_thread)
		return false;
	m_stoprequested = false;
	m_bIsStarted=true;
	sOnConnected(this);
	return true;
}


bool CEvohomeWeb::StopHardware()
{
	if (m_thread!=NULL)
	{
		assert(m_thread);
		m_stoprequested = true;
		m_thread->join();
	}
	m_bIsStarted=false;
	return true;
}


void CEvohomeWeb::Do_Work()
{
	int sec_counter = m_refreshrate - 10;
	_log.Log(LOG_STATUS, "EvohomeWeb: Worker started...");
	while (!m_stoprequested)
	{
		sleep_seconds(1);
		sec_counter++;
		if (sec_counter % 10 == 0) {
			m_LastHeartbeat=mytime(NULL);
		}
		if (sec_counter % m_refreshrate == 0)
		{
			GetStatus();
		}
	}
	_log.Log(LOG_STATUS,"EvohomeWeb: Worker stopped...");
}


bool CEvohomeWeb::WriteToHardware(const char *pdata, const unsigned char length)
{
	if(!pdata)
		return false;
	if (!m_loggedon && !StartSession())
		return false;

	REVOBUF *tsen=(REVOBUF*)pdata;
	switch (pdata[1])
	{
		case pTypeEvohome:
			if (length<sizeof(REVOBUF::_tEVOHOME1))
				return false;
			return SetSystemMode(tsen->EVOHOME1.status);
			break;
		case pTypeEvohomeZone:
			if (length<sizeof(REVOBUF::_tEVOHOME2))
				return false;
			return SetSetpoint(pdata);
			break;
		case pTypeEvohomeWater:
			if (length<sizeof(REVOBUF::_tEVOHOME2))
				return false;
			return SetDHWState(pdata);
			break;
	}
	return false; // bad command
}


void CEvohomeWeb::GetStatus()
{
	if (!m_loggedon && !StartSession())
		return;
	if (!get_status(m_tcs->locationId))
	{
		//FIXME: should I distinguish between simple HTTP errors and session related errors?
		_log.Log(LOG_ERROR,"Evohome: failed to retrieve status");
		m_loggedon = false;
		return;
	}

	// system status
	DecodeControllerMode(m_tcs);

	// cycle all zones for status
	for (std::map<int, zone>::iterator it=m_tcs->zones.begin(); it!=m_tcs->zones.end(); ++it)
		DecodeZone(&m_tcs->zones[it->first]);

	// hot water status
	if (has_dhw(m_tcs))
		DecodeDHWState(m_tcs);
}


bool CEvohomeWeb::SetSystemMode(uint8_t sysmode)
{
	if (set_system_mode(m_tcs->systemId, (int)(m_dczToEvoWebAPIMode[sysmode])))
	{
		_log.Log(LOG_STATUS,"Evohome: changed system status to %s",GetControllerModeName(sysmode));
		return true;
	}	
	_log.Log(LOG_ERROR,"Evohome: error changing system status");
	m_loggedon = false;
	return false;
}


bool CEvohomeWeb::SetSetpoint(const char *pdata)
{
	REVOBUF *pEvo=(REVOBUF*)pdata;

	std::stringstream ssID;
	ssID << std::dec << (int)RFX_GETID3(pEvo->EVOHOME2.id1,pEvo->EVOHOME2.id2,pEvo->EVOHOME2.id3);
	std::string zoneId(ssID.str());

	zone* hz=get_zone_by_ID(zoneId);
	if (hz == NULL) // zone number not known by installation (manually added?)
	{
		_log.Log(LOG_ERROR,"Evohome: attempt to change setpoint on unknown zone");
		return false;
	}

	if ((pEvo->EVOHOME2.mode) == 0) // cancel override
	{
		if (!cancel_temperature_override(zoneId))
			return false;
		
		std::string szsetpoint, szuntil;
		if ( (hz->schedule != NULL) || get_schedule(hz->zoneId) )
		{
			szuntil=local_to_utc(get_next_switchpoint_ex(hz->schedule,szsetpoint));
			pEvo->EVOHOME2.temperature = (int16_t)(strtod(szsetpoint.c_str(),NULL)*100);
		}

		if ( (m_showschedule) && (hz->schedule != NULL) )
		{
			pEvo->EVOHOME2.year=(uint16_t)(atoi(szuntil.substr(0,4).c_str()));
			pEvo->EVOHOME2.month=(uint8_t)(atoi(szuntil.substr(5,2).c_str()));
			pEvo->EVOHOME2.day=(uint8_t)(atoi(szuntil.substr(8,2).c_str()));
			pEvo->EVOHOME2.hrs=(uint8_t)(atoi(szuntil.substr(11,2).c_str()));
			pEvo->EVOHOME2.mins=(uint8_t)(atoi(szuntil.substr(14,2).c_str()));
		}
		else
			pEvo->EVOHOME2.year=0;
		return true;
	}

	int temperature_int = (int)pEvo->EVOHOME2.temperature/100;
	int temperature_frac = (int)pEvo->EVOHOME2.temperature%100;
	std::stringstream s_setpoint;
	s_setpoint << temperature_int << "." << temperature_frac;

	if ((pEvo->EVOHOME2.mode) == 1) // permanent override
	{
		return set_temperature(zoneId, s_setpoint.str(), "");
	}
	if ((pEvo->EVOHOME2.mode) == 2) // temporary override
	{
		std::string szISODate(CEvohomeDateTime::GetISODate(pEvo->EVOHOME2));
		return set_temperature(zoneId, s_setpoint.str(), szISODate);
	}
	return false;
}


bool CEvohomeWeb::SetDHWState(const char *pdata)
{
	if (!has_dhw(m_tcs)) // Installation has no Hot Water device
	{
		_log.Log(LOG_ERROR,"Evohome: attempt to set state on non existing Hot Water device");
		return false;
	}

	REVOBUF *pEvo=(REVOBUF*)pdata;

	std::stringstream ssID;
	ssID << std::dec << (int)RFX_GETID3(pEvo->EVOHOME2.id1,pEvo->EVOHOME2.id2,pEvo->EVOHOME2.id3);
	std::string dhwId(ssID.str());

	std::string DHWstate=(pEvo->EVOHOME2.temperature==0)?"off":"on";

	if ((pEvo->EVOHOME2.mode) == 0) // cancel override (web front end does not appear to support this?)
	{
		DHWstate="auto";
	}
	if ((pEvo->EVOHOME2.mode) <= 1) // permanent override
	{
		return set_dhw_mode(dhwId, DHWstate, "");
	}
	if ((pEvo->EVOHOME2.mode) == 2) // temporary override
	{
		std::string szISODate(CEvohomeDateTime::GetISODate(pEvo->EVOHOME2));
		return set_dhw_mode(dhwId, DHWstate, szISODate);
	}
	return false;
}


void CEvohomeWeb::DecodeControllerMode(temperatureControlSystem* tcs)
{
	unsigned long ID = (unsigned long)(strtod(tcs->systemId.c_str(),NULL));
	std::map<std::string, std::string> ret;
	uint8_t sysmode=0;

	ret["systemMode"] = json_get_val(tcs->status, "systemModeStatus", "mode");

	while (sysmode<7 && strcmp(ret["systemMode"].c_str(),m_szWebAPIMode[sysmode]) != 0)
		sysmode++;

	REVOBUF tsen;
	memset(&tsen,0,sizeof(REVOBUF));
	tsen.EVOHOME1.len=sizeof(tsen.EVOHOME1)-1;
	tsen.EVOHOME1.type=pTypeEvohome;
	tsen.EVOHOME1.subtype=sTypeEvohome;
	RFX_SETID3(ID,tsen.EVOHOME1.id1,tsen.EVOHOME1.id2,tsen.EVOHOME1.id3);
	tsen.EVOHOME1.mode=0; // web API does not support temp override of controller mode
	tsen.EVOHOME1.status=sysmode;
	sDecodeRXMessage(this, (const unsigned char *)&tsen.EVOHOME1, "Controller mode", -1);

	if ( GetControllerName().empty() || m_updatedev )
	{
		ret["modelType"] = json_get_val(tcs->installationInfo, "modelType");
		SetControllerName(ret["modelType"]);
		if(ret["modelType"].empty())
			return;

		std::vector<std::vector<std::string> > result;
		result = m_sql.safe_query("SELECT HardwareID, DeviceID, Name FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID == '%q')", this->m_HwdID, tcs->systemId.c_str());
		if (!result.empty() && (result[0][2] != ret["modelType"]))
		{
			// also change lastupdate time to allow the web frontend to pick up the change
			time_t now = mytime(NULL);
			struct tm ltime;
			localtime_r(&now,&ltime);
			m_sql.safe_query("UPDATE DeviceStatus SET Name='%q', LastUpdate='%04d-%02d-%02d %02d:%02d:%02d' WHERE (HardwareID==%d) AND (DeviceID == '%q')", ret["modelType"].c_str(), ltime.tm_year+1900,ltime.tm_mon+1, ltime.tm_mday, ltime.tm_hour, ltime.tm_min, ltime.tm_sec, this->m_HwdID, tcs->systemId.c_str());
		}
	}
}


void CEvohomeWeb::DecodeZone(zone* hz)
{
	// no sense in using REVOBUF EVOHOME2 to send this to mainworker as this requires breaking up our data
	// only for mainworker to reassemble it.
	std::map<std::string, std::string> zonedata;
	json_object_object_foreach(hz->status, key, val)
	{
		if ( (strcmp(key, "zoneId") == 0) || (strcmp(key, "name") == 0) )
			zonedata[key] = json_object_get_string(val);
		else if ( (strcmp(key, "temperatureStatus") == 0) || (strcmp(key, "heatSetpointStatus") == 0) )
		{
			json_object_object_foreach(val, key2, val2)
				zonedata[key2] = json_object_get_string(val2);
		}
	}

	unsigned long evoID=atol(zonedata["zoneId"].c_str());
	std::stringstream ssUpdateStat;
	if (json_get_val(m_tcs->status, "systemModeStatus", "mode")=="HeatingOff")
		ssUpdateStat << zonedata["temperature"] << ";" << zonedata["targetTemperature"] << ";" << "HeatingOff";
	else
	{
		ssUpdateStat << zonedata["temperature"] << ";" << zonedata["targetTemperature"] << ";" << zonedata["setpointMode"];
		if (m_showschedule && zonedata["until"].empty())
			zonedata["until"] = local_to_utc(get_next_switchpoint(hz));
		if (!zonedata["until"].empty())
			ssUpdateStat << ";" << zonedata["until"];
	}
	std::string sdevname;
	uint64_t DevRowIdx=m_sql.UpdateValue(this->m_HwdID, zonedata["zoneId"].c_str(),GetUnit_by_ID(evoID),pTypeEvohomeZone,sTypeEvohomeZone,10,255,0,ssUpdateStat.str().c_str(), sdevname);

	if (m_updatedev && (DevRowIdx!=-1) && (sdevname!=zonedata["name"]))
	{
		m_sql.safe_query("UPDATE DeviceStatus SET Name='%q' WHERE (ID == %" PRIu64 ")", zonedata["name"].c_str(), DevRowIdx);
		if (sdevname.find("zone ")!=std::string::npos)
			_log.Log(LOG_STATUS,"Evohome: register new zone '%c'", zonedata["name"].c_str());
	}
}


void CEvohomeWeb::DecodeDHWState(temperatureControlSystem* tcs)
{
	// Hot Water is essentially just another zone
	std::map<std::string, std::string> dhwdata;
	json_object *j_dhw, *j_state;
	if (json_object_object_get_ex(tcs->status, "dhw", &j_dhw))
	{
		dhwdata["until"] = "";
		dhwdata["dhwId"] = json_get_val(j_dhw,"dhwId");
		dhwdata["temperature"] = json_get_val(j_dhw,"temperatureStatus","temperature");
		if ( json_object_object_get_ex(tcs->status, "stateStatus", &j_state))
		{
			dhwdata["state"] = json_get_val(j_state,"state");
			dhwdata["mode"] = json_get_val(j_state,"mode");
			if (dhwdata["mode"] == "TemporaryOverride")
				dhwdata["until"] = json_get_val(j_state,"until");
		}
	}

	if (m_updatedev) // create/update and return the first free unit
	{
		std::vector<std::vector<std::string> > result;
		result = m_sql.safe_query(
			"SELECT ID,DeviceID,Name FROM DeviceStatus WHERE (HardwareID==%d) AND (Type==%d) ORDER BY Unit",
			this->m_HwdID, pTypeEvohomeWater);
		if (result.size() < 1) // create device
		{
			std::string sdevname;
			uint64_t DevRowIdx=m_sql.UpdateValue(this->m_HwdID,dhwdata["dhwId"].c_str(),1,pTypeEvohomeWater,sTypeEvohomeWater,10,255,50,"0.0;Off;Auto",sdevname);
			m_sql.safe_query("UPDATE DeviceStatus SET Name='Hot Water' WHERE (ID == %" PRIu64 ")", DevRowIdx);
		}
		else if ( (result[0][1]!=dhwdata["dhwId"]) || (result[0][2]!="Hot Water") )
		{
			uint64_t DevRowIdx;
			std::stringstream s_str( result[0][0] );
			s_str >> DevRowIdx;
			m_sql.safe_query("UPDATE DeviceStatus SET DeviceID='%q',Name='Hot Water' WHERE (ID == %" PRIu64 ")", dhwdata["dhwId"].c_str(), DevRowIdx);
		}
	}

	std::stringstream ssUpdateStat;
	ssUpdateStat << dhwdata["temperature"] << ";" << dhwdata["state"] << ";" << dhwdata["mode"];
	if (m_showschedule && dhwdata["until"].empty())
		dhwdata["until"] = local_to_utc(get_next_switchpoint(tcs, atoi(dhwdata["dhwId"].c_str())));
	if (!dhwdata["until"].empty())
		ssUpdateStat << ";" << dhwdata["until"];

	std::string sdevname;
	uint64_t DevRowIdx=m_sql.UpdateValue(this->m_HwdID, dhwdata["dhwId"].c_str(),1,pTypeEvohomeWater,sTypeEvohomeWater,10,255,50,ssUpdateStat.str().c_str(), sdevname);
}


/*
 * Code for serial and python scripts appear to assume that zones are always returned in the same order
 * I'm not sure that is really true, so I'll use a map to match the evohome ID and our zone number.
 */
uint8_t CEvohomeWeb::GetUnit_by_ID(unsigned long evoID)
{
	uint8_t row;
	if (m_zones[0] == 0) // first run - construct 
	{
		std::vector<std::vector<std::string> > result;
		result = m_sql.safe_query(
			"SELECT Unit,DeviceID FROM DeviceStatus WHERE (HardwareID==%d) AND (Type==%d) ORDER BY Unit",
			this->m_HwdID, pTypeEvohomeZone);
		for (row=1; row <= m_nMaxZones; row++)
			m_zones[row] = 0;
		for (row=0; row < result.size(); row++)
		{
			int unit = atoi(result[row][0].c_str());	
			m_zones[unit] = atol(result[row][1].c_str());
			if (m_zones[unit] == (unsigned long)(unit + 92000)) // mark manually added, unlinked zone as free
				m_zones[unit] = 0;
		}
		m_zones[0] = 1;
	}
	for (row=1; row <= m_nMaxZones; row++)
	{
		if (m_zones[row] == evoID)
			return row;
	}
	if (m_updatedev) // create/update and return the first free unit
	{
		for (row=1; row <= m_nMaxZones; row++)
		{
			if (m_zones[row] == 0)
			{
				std::string sdevname;
				unsigned long nid=92000+row;
				char ID[40];
				sprintf(ID, "%lu", nid);
				uint64_t DevRowIdx=m_sql.UpdateValue(this->m_HwdID,ID,row,pTypeEvohomeZone,sTypeEvohomeZone,10,255,0,"0.0;0.0;Auto",sdevname);
				if (DevRowIdx == -1)
					return -1;
				char devname[8];
				sprintf(devname, "zone %u", row);
				sprintf(ID, "%lu", evoID);
				m_sql.safe_query("UPDATE DeviceStatus SET Name='%q',DeviceID='%q' WHERE (ID == %" PRIu64 ")", devname, ID, DevRowIdx);
				m_zones[row] = evoID;
				return row;
			}
		}
		_log.Log(LOG_ERROR,"Evohome: cannot add new zone because you have no free zones left");
	}
	return -1;
}


/*
 * Helper to convert local time strings to UTC time strings
 */
std::string CEvohomeWeb::local_to_utc(std::string local_time)
{
	if (m_tzoffset == -1)
	{
		// calculate timezone offset once
		time_t now = mytime(NULL);
		struct tm utime;
		gmtime_r(&now, &utime);
		utime.tm_isdst = -1;
		m_tzoffset = (int)difftime(mktime(&utime), now);
	}
	struct tm ltime;
	ltime.tm_isdst = -1;
	ltime.tm_year = atoi(local_time.substr(0, 4).c_str()) - 1900;
	ltime.tm_mon = atoi(local_time.substr(5, 2).c_str()) - 1;
	ltime.tm_mday = atoi(local_time.substr(8, 2).c_str());
	ltime.tm_hour = atoi(local_time.substr(11, 2).c_str());
	ltime.tm_min = atoi(local_time.substr(14, 2).c_str());
	ltime.tm_sec = atoi(local_time.substr(17, 2).c_str()) + m_tzoffset;
	mktime(&ltime);
	if ((m_lastDST!=ltime.tm_isdst) && (m_lastDST!=-1)) // DST changed - must recalculate timezone offset
	{
		ltime.tm_hour -= (ltime.tm_isdst - m_lastDST);
		m_lastDST=ltime.tm_isdst;
		m_tzoffset=-1;
	}
	char until[22];
	sprintf(until,"%04d-%02d-%02dT%02d:%02d:%02dZ",ltime.tm_year+1900,ltime.tm_mon+1,ltime.tm_mday,ltime.tm_hour,ltime.tm_min,ltime.tm_sec);
	return std::string(until);
}


/*
 * Evohome client API
 */

/*
 * Copyright (c) 2016 Gordon Bos <gordon@bosvangennip.nl> All rights reserved.
 *
 * Json client for UK/EMEA Evohome API
 *
 *
 * Source code subject to GNU GENERAL PUBLIC LICENSE version 3
 */


#define EVOHOME_HOST "https://tccna.honeywell.com"


const std::string weekdays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
//const std::string evo_modes[7] = {"Auto", "HeatingOff", "AutoWithEco", "Away", "DayOff", "", "Custom"};


/************************************************************************
 *									*
 *	Evohome authentication						*
 *									*
 ************************************************************************/

bool CEvohomeWeb::login(std::string user, std::string password)
{
	auth_info.clear();

	std::stringstream pdata;
	pdata << "installationInfo-Type=application%2Fx-www-form-urlencoded;charset%3Dutf-8";
	pdata << "&Host=rs.alarmnet.com%2F";
	pdata << "&Cache-Control=no-store%20no-cache";
	pdata << "&Pragma=no-cache";
	pdata << "&grant_type=password";
	pdata << "&scope=EMEA-V1-Basic%20EMEA-V1-Anonymous%20EMEA-V1-Get-Current-User-Account";
	pdata << "&Username=" << CURLEncode::URLEncode(user);
	pdata << "&Password=" << CURLEncode::URLEncode(password);
	pdata << "&Connection=Keep-Alive";

	std::string s_res;
	if (!HTTPClient::POST(EVOHOME_HOST"/Auth/OAuth/Token", pdata.str(), LoginHeaders, s_res))
	{
		_log.Log(LOG_ERROR,"Evohome: HTTP client error at login!");
		return false;
	}

	if (s_res.find("<title>") != std::string::npos) // got an HTML page
	{
		int i = s_res.find("<title>");
		char* html = &s_res[i];
		i = 7;
		char c = html[i];
		std::stringstream edata;
		while (c != '<')
		{
			edata << c;
			i++;
			c = html[i];
		}
		_log.Log(LOG_ERROR,"Evohome: login failed with message: %s", edata.str().c_str());
		return false;
	}

	json_object *j_ret = json_tokener_parse(s_res.c_str());
	json_object *j_msg;
	if ( (json_object_object_get_ex(j_ret, "error", &j_msg)) || (json_object_object_get_ex(j_ret, "message", &j_msg)) )
	{
		_log.Log(LOG_ERROR,"Evohome: login failed with message: %s", json_object_get_string(j_msg));
		return false;
	}

	json_object_object_foreach(j_ret, key, val)
	{
		auth_info[key] = json_object_get_string(val);
	}

	std::stringstream atoken;
	atoken << "Authorization: bearer " << auth_info["access_token"];
	SessionHeaders.clear();
	SessionHeaders.push_back(atoken.str());
	SessionHeaders.push_back("applicationId: b013aa26-9724-4dbd-8897-048b9aada249");
	SessionHeaders.push_back("accept: application/json, application/xml, text/json, text/x-json, text/javascript, text/xml");
	SessionHeaders.push_back("content-type: application/json");
	SessionHeaders.push_back("charsets: utf-8");

	return user_account();
}


/* 
 * Retrieve evohome user info
 */
bool CEvohomeWeb::user_account()
{
	account_info.clear();
	std::string s_res;
	if (!HTTPClient::GET(EVOHOME_HOST"/WebAPI/emea/api/v1/userAccount", SessionHeaders, s_res))
	{
		_log.Log(LOG_ERROR,"Evohome: HTTP client error at retrieve user account info!");
		return false;
	}
	json_object *j_ret = json_tokener_parse(s_res.c_str());
	json_object_object_foreach(j_ret, key, val)
	{
		account_info[key] = json_object_get_string(val);
	}
	return true;
}


/************************************************************************
 *									*
 *	Evohome heating installations retrieval				*
 *									*
 ************************************************************************/

void CEvohomeWeb::get_zones(int location, int gateway, int temperatureControlSystem)
{
	locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].zones.clear();
	json_object *j_tcs = locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].installationInfo;
	json_object *j_list;
	if ( json_object_object_get_ex(j_tcs, "zones", &j_list) )
	{
		int l = json_object_array_length(j_list);
		int i;
		for (i=0; i<l; i++)
		{
			locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].zones[i].installationInfo = json_object_array_get_idx(j_list, i);

			json_object *j_zoneId;
			json_object_object_get_ex(locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].zones[i].installationInfo, "zoneId", &j_zoneId);
			locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].zones[i].zoneId = json_object_get_string(j_zoneId);
			locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].zones[i].systemId = locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].systemId;
			locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].zones[i].gatewayId = locations[location].gateways[gateway].gatewayId;
			locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem].zones[i].locationId = locations[location].locationId;
		}
	}
}

void CEvohomeWeb::get_temperatureControlSystems(int location, int gateway)
{
	locations[location].gateways[gateway].temperatureControlSystems.clear();
	json_object *j_gw = locations[location].gateways[gateway].installationInfo;

	json_object *j_list;
	if ( json_object_object_get_ex(j_gw, "temperatureControlSystems", &j_list) )
	{
		int l = json_object_array_length(j_list);
		int i;
		for (i = 0; i < l; i++)
		{
			locations[location].gateways[gateway].temperatureControlSystems[i].installationInfo = json_object_array_get_idx(j_list, i);

			json_object *j_tcsId;
			json_object_object_get_ex(locations[location].gateways[gateway].temperatureControlSystems[i].installationInfo, "systemId", &j_tcsId);
			locations[location].gateways[gateway].temperatureControlSystems[i].systemId = json_object_get_string(j_tcsId);
			locations[location].gateways[gateway].temperatureControlSystems[i].gatewayId = locations[location].gateways[gateway].gatewayId;
			locations[location].gateways[gateway].temperatureControlSystems[i].locationId = locations[location].locationId;

			get_zones(location, gateway, i);
		}
	}
}


void CEvohomeWeb::get_gateways(int location)
{
	locations[location].gateways.clear();
	json_object *j_loc = locations[location].installationInfo;
	json_object *j_list;
	if ( json_object_object_get_ex(j_loc, "gateways", &j_list) )
	{
		int l = json_object_array_length(j_list);
		int i;
		for (i = 0; i < l; i++)
		{
			locations[location].gateways[i].installationInfo = json_object_array_get_idx(j_list, i);

			json_object *j_gwInfo, *j_gwId;
			json_object_object_get_ex(locations[location].gateways[i].installationInfo, "gatewayInfo", &j_gwInfo);
			json_object_object_get_ex(j_gwInfo, "gatewayId", &j_gwId);
			locations[location].gateways[i].gatewayId = json_object_get_string(j_gwId);
			locations[location].gateways[i].locationId = locations[location].locationId;

			get_temperatureControlSystems(location,i);
		}
	}
}


bool CEvohomeWeb::full_installation()
{
	locations.clear();
	std::stringstream url;
	url << EVOHOME_HOST << "/WebAPI/emea/api/v1/location/installationInfo?userId=" << account_info["userId"] << "&includeTemperatureControlSystems=True";

	std::string s_res;
	if (!HTTPClient::GET(url.str(), SessionHeaders, s_res))
	{
		_log.Log(LOG_ERROR,"Evohome: HTTP client error at retrieve installation!");
		return false;
	}

	// evohome v1 API does not correctly format the json output
	std::stringstream ss_jdata;
	ss_jdata << "{\"locations\": " << s_res << "}";
	json_object *j_fi = json_tokener_parse(ss_jdata.str().c_str());
	json_object *j_list;
	json_object_object_get_ex(j_fi, "locations", &j_list);
	if ( ! json_object_is_type(j_list, json_type_array))
		return false;
	int l = json_object_array_length(j_list);
	int i;
	for (i=0; i<l; i++)
	{
		locations[i].installationInfo = json_object_array_get_idx(j_list, i);

		json_object *j_locInfo, *j_locId;
		json_object_object_get_ex(locations[i].installationInfo, "locationInfo", &j_locInfo);
		json_object_object_get_ex(j_locInfo, "locationId", &j_locId);
		locations[i].locationId = json_object_get_string(j_locId);

		get_gateways(i);
	}
	return true;
}


/************************************************************************
 *									*
 *	Evohome system status retrieval					*
 *									*
 ************************************************************************/

bool CEvohomeWeb::get_status(std::string locationId)
{
	if (locations.size() == 0)
		return false;
	int i;
	for (i = 0; i < (int)locations.size(); i++)
	{
		if (locations[i].locationId == locationId)
			return get_status(i);
	}
	return false;
}
bool CEvohomeWeb::get_status(int location)
{
	if ( (locations.size() == 0) || ( json_object_is_type(locations[location].installationInfo,json_type_null) ) )
	{
		return false;
	}

	bool valid_json = true;
	std::stringstream url;
	url << EVOHOME_HOST << "/WebAPI/emea/api/v1/location/" << locations[location].locationId << "/status?includeTemperatureControlSystems=True";
	std::string s_res;
	if (!HTTPClient::GET(url.str(), SessionHeaders, s_res))
	{
		_log.Log(LOG_ERROR,"Evohome: HTTP client error at get status!");
		return false;
	}
	locations[location].status = json_tokener_parse(s_res.c_str());
	// get gateway status
	json_object *j_gwlist;
	if ( json_object_object_get_ex(locations[location].status, "gateways", &j_gwlist) )
	{
		int lgw = json_object_array_length(j_gwlist);
		int igw;
		for (igw = 0; igw < lgw; igw++)
		{
			locations[location].gateways[igw].status = json_object_array_get_idx(j_gwlist, igw);
			// get temperatureControlSystem status
			json_object *j_tcslist;
			if ( json_object_object_get_ex(locations[location].gateways[igw].status, "temperatureControlSystems", &j_tcslist) )
			{
				int ltcs = json_object_array_length(j_tcslist);
				int itcs;
				for (itcs = 0; itcs < ltcs; itcs++)
				{
					locations[location].gateways[igw].temperatureControlSystems[itcs].status = json_object_array_get_idx(j_tcslist, itcs);
					// get zone status
					json_object *j_zlist;
					if ( json_object_object_get_ex(locations[location].gateways[igw].temperatureControlSystems[itcs].status, "zones", &j_zlist) )
					{
						int lz = json_object_array_length(j_zlist);
						int iz;
						for (iz = 0; iz < lz; iz++)
						{
							locations[location].gateways[igw].temperatureControlSystems[itcs].zones[iz].status = json_object_array_get_idx(j_zlist, iz);
						}
					}
					else
						valid_json = false;
				}
			}
			else
				valid_json = false;
		}
	}
	else
		valid_json = false;

	return valid_json;
}


/************************************************************************
 *									*
 *	Locate Evohome objects by ID					*
 *									*
 ************************************************************************/

CEvohomeWeb::location* CEvohomeWeb::get_location_by_ID(std::string locationId)
{
	if (locations.size() == 0)
		full_installation();
	unsigned int l;
	for (l = 0; l < locations.size(); l++)
	{
		if (locations[l].locationId == locationId)
			return &locations[l];
	}
	return NULL;
}


CEvohomeWeb::gateway* CEvohomeWeb::get_gateway_by_ID(std::string gatewayId)
{
	if (locations.size() == 0)
		full_installation();
	unsigned int l,g;
	for (l = 0; l < locations.size(); l++)
	{
		for (g = 0; g < locations[l].gateways.size(); g++)
		{
			if (locations[l].gateways[g].gatewayId == gatewayId)
				return &locations[l].gateways[g];
		}
	}
	return NULL;
}


CEvohomeWeb::temperatureControlSystem* CEvohomeWeb::get_temperatureControlSystem_by_ID(std::string systemId)
{
	if (locations.size() == 0)
		full_installation();
	unsigned int l,g,t;
	for (l = 0; l < locations.size(); l++)
	{
		for (g = 0; g < locations[l].gateways.size(); g++)
		{
			for (t = 0; t < locations[l].gateways[g].temperatureControlSystems.size(); t++)
			{
				if (locations[l].gateways[g].temperatureControlSystems[t].systemId == systemId)
					return &locations[l].gateways[g].temperatureControlSystems[t];
			}
		}
	}
	return NULL;
}


CEvohomeWeb::zone* CEvohomeWeb::get_zone_by_ID(std::string zoneId)
{
	if (locations.size() == 0)
		full_installation();
	unsigned int l,g,t,z;
	for (l = 0; l < locations.size(); l++)
	{
		for (g = 0; g < locations[l].gateways.size(); g++)
		{
			for (t = 0; t < locations[l].gateways[g].temperatureControlSystems.size(); t++)
			{
				for (z = 0; z < locations[l].gateways[g].temperatureControlSystems[t].zones.size(); z++)
				{
					if (locations[l].gateways[g].temperatureControlSystems[t].zones[z].zoneId == zoneId)
						return &locations[l].gateways[g].temperatureControlSystems[t].zones[z];
				}
			}
		}
	}
	return NULL;
}


CEvohomeWeb::temperatureControlSystem* CEvohomeWeb::get_zone_temperatureControlSystem(CEvohomeWeb::zone* zone)
{
	unsigned int l,g,t,z;
	for (l = 0; l < locations.size(); l++)
	{
		for (g = 0; g < locations[l].gateways.size(); g++)
		{
			for (t = 0; t < locations[l].gateways[g].temperatureControlSystems.size(); t++)
			{
				for (z = 0; z < locations[l].gateways[g].temperatureControlSystems[t].zones.size(); z++)
				{
					if (locations[l].gateways[g].temperatureControlSystems[t].zones[z].zoneId == zone->zoneId)
						return &locations[l].gateways[g].temperatureControlSystems[t];
				}
			}
		}
	}
	return NULL;
}


/************************************************************************
 *									*
 *	Schedule handlers						*
 *									*
 ************************************************************************/

bool CEvohomeWeb::get_schedule(std::string zoneId)
{
	std::stringstream url;
	url << EVOHOME_HOST << "/WebAPI/emea/api/v1/temperatureZone/" << zoneId << "/schedule";
	std::string s_res;
	if ((!HTTPClient::GET(url.str(), SessionHeaders, s_res))||(!s_res.find("\"id\"")))
		return false;
	get_zone_by_ID(zoneId)->schedule = json_tokener_parse(s_res.c_str());
	return true;
}


std::string CEvohomeWeb::get_next_switchpoint(CEvohomeWeb::temperatureControlSystem* tcs, int zone)
{
	if ( (tcs->zones[zone].schedule == NULL) && !get_schedule(tcs->zones[zone].zoneId) )
			return "";
	return get_next_switchpoint(tcs->zones[zone].schedule);
}

std::string CEvohomeWeb::get_next_switchpoint(std::string zoneId)
{
	if ( (get_zone_by_ID(zoneId)->schedule == NULL) && !get_schedule(zoneId) )
			return "";
	return get_next_switchpoint(get_zone_by_ID(zoneId)->schedule);
}
std::string CEvohomeWeb::get_next_switchpoint(zone* hz)
{
	if ( (hz->schedule == NULL) && !get_schedule(hz->zoneId) )
			return "";
	return get_next_switchpoint(hz->schedule);
}
std::string CEvohomeWeb::get_next_switchpoint(json_object *schedule)
{
	std::string current_temperature;
	return get_next_switchpoint_ex(schedule, current_temperature);
}
std::string CEvohomeWeb::get_next_switchpoint_ex(json_object *schedule, std::string &current_temperature)
{
	if (schedule == NULL)
		return "";

	struct tm ltime;
	time_t now = mytime(NULL);
	localtime_r(&now,&ltime);
	int year = ltime.tm_year;
	int month = ltime.tm_mon;
	int day = ltime.tm_mday;
	std::string s_time;
	json_object *j_week;
	json_object_object_get_ex(schedule, "dailySchedules", &j_week);
	int sdays = json_object_array_length(j_week);
	int d, i;

	for (d = 0; d < 7; d++)
	{
		int wday = (ltime.tm_wday + d) % 7;
		std::string s_wday = (std::string)weekdays[wday];
		json_object *j_day, *j_dayname;
// find day
		for (i = 0; i < sdays; i++)
		{
			j_day = json_object_array_get_idx(j_week, i);
			if ( (json_object_object_get_ex(j_day, "dayOfWeek", &j_dayname)) && (strcmp(json_object_get_string(j_dayname), s_wday.c_str()) == 0) )
				i = sdays;
		}

		json_object *j_list, *j_sp, *j_tim, *j_temp;
		json_object_object_get_ex( j_day, "switchpoints", &j_list);

		int l = json_object_array_length(j_list);
		for (i = 0; i < l; i++)
		{
			j_sp = json_object_array_get_idx(j_list, i);
			json_object_object_get_ex(j_sp, "timeOfDay", &j_tim);
			s_time = json_object_get_string(j_tim);
			ltime.tm_isdst = -1;
			ltime.tm_year = year;
			ltime.tm_mon = month;
			ltime.tm_mday = day + d;
			ltime.tm_hour = atoi(s_time.substr(0, 2).c_str());
			ltime.tm_min = atoi(s_time.substr(3, 2).c_str());
			ltime.tm_sec = atoi(s_time.substr(6, 2).c_str());
			time_t ntime = mktime(&ltime);
			if (ntime > now)
			{
				i = l;
				d = 7;
			}
			else
			{
				json_object_object_get_ex(j_sp, "temperature", &j_temp);
				current_temperature = json_object_get_string(j_temp);
			}
		}
	}
	char rdata[30];
	sprintf(rdata,"%04d-%02d-%02dT%sZ",ltime.tm_year+1900,ltime.tm_mon+1,ltime.tm_mday,s_time.c_str());

	return std::string(rdata);
}


/************************************************************************
 *									*
 *	json helpers							*
 *									*
 ************************************************************************/

std::string CEvohomeWeb::json_get_val(std::string s_json, const char* key)
{
	return json_get_val(json_tokener_parse(s_json.c_str()), key);
}


std::string CEvohomeWeb::json_get_val(json_object *j_json, const char* key)
{
	json_object *j_res;

	if (json_object_object_get_ex(j_json, key, &j_res))
		return json_object_get_string(j_res);
	return "";
}


std::string CEvohomeWeb::json_get_val(std::string s_json, const char* key1, const char* key2)
{
	return json_get_val(json_tokener_parse(s_json.c_str()), key1, key2);
}


std::string CEvohomeWeb::json_get_val(json_object *j_json, const char* key1, const char* key2)
{
	json_object *j_tmp, *j_res;

	if ( ( json_object_object_get_ex( j_json, key1, &j_tmp )) && ( json_object_object_get_ex( j_tmp, key2, &j_res )) )
		return json_object_get_string(j_res);
	return "";
}


/************************************************************************
 *									*
 *	Evohome overrides						*
 *									*
 ************************************************************************/

bool CEvohomeWeb::verify_date(std::string date)
{
	if (date.length() < 10)
		return false;
	std:: string s_date = date.substr(0,10);
	struct tm mtime;
	mtime.tm_isdst = -1;
	mtime.tm_year = atoi(date.substr(0, 4).c_str()) - 1900;
	mtime.tm_mon = atoi(date.substr(5, 2).c_str()) - 1;
	mtime.tm_mday = atoi(date.substr(8, 2).c_str());
	mtime.tm_hour = 12; // midday time - prevent date shift because of DST
	mtime.tm_min = 0;
	mtime.tm_sec = 0;
	time_t ntime = mktime(&mtime);
	if ( ntime == -1)
		return false;
	char rdata[12];
	sprintf(rdata,"%04d-%02d-%02d",mtime.tm_year+1900,mtime.tm_mon+1,mtime.tm_mday);
	return (s_date == std::string(rdata));
}

	
bool CEvohomeWeb::verify_datetime(std::string datetime)
{
	if (datetime.length() < 19)
		return false;
	std:: string s_date = datetime.substr(0,10);
	std:: string s_time = datetime.substr(11,8);
	struct tm mtime;
	mtime.tm_isdst = -1;
	mtime.tm_year = atoi(datetime.substr(0, 4).c_str()) - 1900;
	mtime.tm_mon = atoi(datetime.substr(5, 2).c_str()) - 1;
	mtime.tm_mday = atoi(datetime.substr(8, 2).c_str());
	mtime.tm_hour = atoi(datetime.substr(11, 2).c_str());
	mtime.tm_min = atoi(datetime.substr(14, 2).c_str());
	mtime.tm_sec = atoi(datetime.substr(17, 2).c_str());
	time_t ntime = mktime(&mtime);
	if ( ntime == -1)
		return false;
	char c_date[12];
	sprintf(c_date,"%04d-%02d-%02d",mtime.tm_year+1900,mtime.tm_mon+1,mtime.tm_mday);
	char c_time[12];
	sprintf(c_time,"%02d:%02d:%02d",mtime.tm_hour,mtime.tm_min,mtime.tm_sec);
	return ( (s_date == std::string(c_date)) && (s_time == std::string(c_time)) );
}


bool CEvohomeWeb::set_system_mode(std::string systemId, int mode, std::string date_until)
{
	std::stringstream url;
	url << EVOHOME_HOST << "/WebAPI/emea/api/v1/temperatureControlSystem/" << systemId << "/mode";
	std::stringstream data;
	data << "{\"SystemMode\":" << mode;
	if (date_until == "")
		data << ",\"TimeUntil\":null,\"Permanent\":true}";
	else
	{
		if ( ! verify_date(date_until) )
			return false;
		data << ",\"TimeUntil\":\"" << date_until.substr(0,10) << "T00:00:00Z\",\"Permanent\":false}";
	}
	std::string s_res;
	if ((HTTPClient::PUT(url.str(), data.str(), SessionHeaders, s_res))&&(s_res.find("\"id\"")))
		return true;
	return false;
}
bool CEvohomeWeb::set_system_mode(std::string systemId, int mode)
{
	return set_system_mode(systemId, mode, "");
}

/*
bool CEvohomeWeb::set_system_mode(std::string systemId, std::string mode, std::string date_until)
{
	int i = 0;
	int s = sizeof(evo_modes);
	while (s > 0)
	{
		if (evo_modes[i] == mode)
			return set_system_mode(systemId, i, date_until);
		s -= sizeof(evo_modes[i]);
		i++;
	}
	return false;
}
bool CEvohomeWeb::set_system_mode(std::string systemId, std::string mode)
{
	return set_system_mode(systemId, mode, "");
}
*/


bool CEvohomeWeb::set_temperature(std::string zoneId, std::string temperature, std::string time_until)
{
	std::stringstream url;
	url << EVOHOME_HOST << "/WebAPI/emea/api/v1/temperatureZone/" << zoneId << "/heatSetpoint";
	std::stringstream data;
	data << "{\"HeatSetpointValue\":" << temperature;
	if (time_until == "")
		data << ",\"SetpointMode\":1,\"TimeUntil\":null}";
	else
	{
		if ( ! verify_datetime(time_until) )
			return false;
		data << ",\"SetpointMode\":2,\"TimeUntil\":\"" << time_until.substr(0,10) << "T" << time_until.substr(11,8) << "Z\"}";
	}
	std::string s_res;
	if ((HTTPClient::PUT(url.str(), data.str(), SessionHeaders, s_res))&&(s_res.find("\"id\"")))
		return true;
	return false;
}
bool CEvohomeWeb::set_temperature(std::string zoneId, std::string temperature)
{
	return set_temperature(zoneId, temperature, "");
}


bool CEvohomeWeb::cancel_temperature_override(std::string zoneId)
{
	std::stringstream url;
	url << EVOHOME_HOST << "/WebAPI/emea/api/v1/temperatureZone/" << zoneId << "/heatSetpoint";
	std::string s_data = "{\"HeatSetpointValue\":0.0,\"SetpointMode\":0,\"TimeUntil\":null}";
	std::string s_res;
	if ((HTTPClient::PUT(url.str(), s_data, SessionHeaders, s_res))&&(s_res.find("\"id\"")))
		return true;
	return false;
}


bool CEvohomeWeb::has_dhw(int location, int gateway, int temperatureControlSystem)
{
	CEvohomeWeb::temperatureControlSystem *tcs = &locations[location].gateways[gateway].temperatureControlSystems[temperatureControlSystem];
	return has_dhw(tcs);
}
bool CEvohomeWeb::has_dhw(CEvohomeWeb::temperatureControlSystem *tcs)
{
	json_object *j_dhw;
	return json_object_object_get_ex(tcs->status, "dhw", &j_dhw);
}


bool CEvohomeWeb::is_single_heating_system()
{
	if (locations.size() == 0)
		full_installation();
	return ( (locations.size() == 1) &&
		 (locations[0].gateways.size() == 1) &&
		 (locations[0].gateways[0].temperatureControlSystems.size() == 1) );
}


bool CEvohomeWeb::set_dhw_mode(std::string dhwId, std::string mode, std::string time_until)
{
	std::stringstream data;
	if (mode == "auto")
		data << "{\"State\":0,\"Mode\":0,\"UntilTime\":null}";
	else
	{
		data << "{\"State\":";
		if (mode == "on")
			data << 1;
		else
			data << 0;
		if (time_until == "")
			data << ",\"Mode\":1,\"UntilTime\":null}";
		else
		{
			if ( ! verify_datetime(time_until) )
				return false;
			data << ",\"Mode\":2,\"UntilTime\":\"" << time_until.substr(0,10) << "T" << time_until.substr(11,8) << "Z\"}";
		}
	}
	std::stringstream url;
	url << EVOHOME_HOST << "/WebAPI/emea/api/v1/domesticHotWater/" << dhwId << "/state";
	std::string s_res;
	if ((HTTPClient::PUT(url.str(), data.str(), SessionHeaders, s_res))&&(s_res.find("\"id\"")))
		return true;
	return false;
}
bool CEvohomeWeb::set_dhw_mode(std::string systemId, std::string mode)
{
	return set_dhw_mode(systemId, mode, "");
}

