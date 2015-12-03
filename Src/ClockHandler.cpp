/****************************************************************
 * @@@LICENSE
 *
 *  Copyright (c) 2013-2014 LG Electronics, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * LICENSE@@@
 ****************************************************************/

/**
 *  @file ClockHandler.cpp
 */

#include <luna-service2/lunaservice.h>

#include "Logging.h"
#include "JSONUtils.h"

#include "ClockHandler.h"
#include "TimePrefsHandler.h"

namespace {
	LSMethod s_methods[]  = {
		{ "getTime", &ClockHandler::cbGetTime },
		{ 0, 0 },
	};

	LSMethod s_private_methods[]  = {
		{ "setTime", &ClockHandler::cbSetTime },
		{ 0, 0 },
	};

} // anonymous namespace

const std::string ClockHandler::manual = "manual";
const std::string ClockHandler::micom = "micom";
const std::string ClockHandler::system = "system";
const time_t ClockHandler::invalidTime = (time_t)-1;
const time_t ClockHandler::invalidOffset = (time_t)LONG_MIN;

ClockHandler::ClockHandler() :
	m_manualOverride( false )
{
	// we always have manual time-source
	// assume priority 0 (the lowest non-negative)
	setup(manual, 0);
}

bool ClockHandler::setServiceHandle(LSPalmService* service)
{
	LSError lsError;
	LSErrorInit(&lsError);
	bool result = LSPalmServiceRegisterCategory( service, "/clock",
	                                             s_methods, s_private_methods,
	                                             NULL, this, &lsError );
	if (!result) {
		PmLogError( sysServiceLogContext(), "CLOCK_REGISTER_FAIL", 1,
		            PMLOGKS("MESSAGE", lsError.message),
				    "Failed to register clock handler methods" );
		LSErrorFree(&lsError);
		return false;
	}

	return true;
}

void ClockHandler::adjust(time_t offset)
{
	for (ClocksMap::iterator it = m_clocks.begin();
	     it != m_clocks.end(); ++it)
	{
		if (it->second.systemOffset == invalidOffset) continue;
		it->second.systemOffset -= offset; // maintain absolute time presented in diff from current one
		if (it->second.lastUpdate != invalidTime)
		{
			it->second.lastUpdate += offset; // maintain same distance from current time
		}
	}
}
void ClockHandler::manualOverride(bool enabled)
{
	if (m_manualOverride == enabled)
	{
		return; // nothing to change
	}

	m_manualOverride = enabled;

	if (!enabled)
	{
		// re-send clock changes again if switched to auto
		for (ClocksMap::const_iterator it = m_clocks.begin();
		     it != m_clocks.end(); ++it)
		{
			// skip those for which there was no update was called
			// even if they have initial offset set
			if (it->second.lastUpdate == invalidTime) continue;

			const Clock &clock = it->second;

			assert( clock.lastUpdate == invalidTime ||
			        clock.systemOffset != invalidOffset ); // invariant of Clock

			PmLogDebug(sysServiceLogContext(),
				"Re-sending %s with %ld offset and %ld last update mark",
				it->first.c_str(), clock.systemOffset, clock.lastUpdate
			);
			clockChanged.fire(it->first, clock.priority, clock.systemOffset, clock.lastUpdate);
		}
	}
}

void ClockHandler::setup(const std::string &clockTag, int priority, time_t offset /* = invalidOffset */)
{
	ClocksMap::iterator it = m_clocks.find(clockTag);
	if (it != m_clocks.end())
	{
		PmLogWarning( sysServiceLogContext(), "CLOCK_SETUP_OVERRIDE", 3,
		              PMLOGKS("CLOCK_TAG", clockTag.c_str()),
		              PMLOGKFV("PRIORITY", "%d", priority),
		              PMLOGKFV("OFFSET", "%ld", offset),
		              "Trying to register already existing clock (overriding old params)" );

		it->second.priority = priority;
		if (offset != invalidOffset)
		{
			it->second.systemOffset = offset;
			// That's a good question what time to set for lastUpdate.
			// Follow rule that if we specified offset than we want it to be
			// considered so set it to current time.
			it->second.lastUpdate = time(0);
		}
	}
	else
	{
		m_clocks.insert(ClocksMap::value_type(clockTag, (Clock){ priority, offset, invalidTime }));
	}

	PmLogDebug(sysServiceLogContext(), "Registered clock %s with priority %d", clockTag.c_str(), priority);
}

bool ClockHandler::update(time_t offset, const std::string &clockTag /* = manual */, time_t timeStamp /* = invalidTime */)
{
	PmLogInfo(sysServiceLogContext(), "CLOCK_UPDATE", 2,
		PMLOGKS("SOURCE", clockTag.c_str()),
		PMLOGKFV("SYSTEM_OFFSET", "%ld", offset),
		"ClockHandler::update() with time-stamp %ld",
		timeStamp
	);

	ClocksMap::iterator it = m_clocks.find(clockTag);
	if (it == m_clocks.end())
	{
		PmLogWarning( sysServiceLogContext(), "WRONG_CLOCK_UPDATE", 2,
		              PMLOGKFV("OFFSET", "%ld", offset),
		              PMLOGKS("CLOCK_TAG", clockTag.c_str()),
		              "Trying to update clock that is not registered" );
		return false;
	}

	time_t prevTimeStamp = it->second.lastUpdate;
	if (timeStamp == invalidTime)
	{
		timeStamp = time(0);
	}
	else if (prevTimeStamp != invalidTime && prevTimeStamp >= timeStamp)
	{
		PmLogInfo( sysServiceLogContext(), "CLOCK_UPDATE_OUTDATED", 2,
		           PMLOGKS("SOURCE", clockTag.c_str()),
		           PMLOGKFV("SYSTEM_OFFSET", "%ld", offset),
		           "ClockHandler::update() silently ignores updates with outdated time-stamp %ld < %ld",
		           timeStamp, it->second.lastUpdate );
		return true;
	}

	Clock &clock = it->second;
	clock.lastUpdate = timeStamp;
	clock.systemOffset = offset;

	clockChanged.fire( it->first, clock.priority, offset, clock.lastUpdate );

	return true;
}

// service handlers
bool ClockHandler::cbSetTime(LSHandle* lshandle, LSMessage *message, void *user_data)
{
	assert( user_data );

	jvalue_ref schema_v4 = convert_schema_v2_to_v4(STRICT_SCHEMA(
		PROPS_2(
			WITHDEFAULT(source, string, "manual"),
			REQUIRED(utc, integer)
		)

		REQUIRED_1( utc )));
	LSMessageJsonParser parser(message, jvalue_tostring_simple(schema_v4));
	if (!jis_null(schema_v4))
	    j_release(&schema_v4);

	if (!parser.parse(__FUNCTION__, lshandle, EValidateAndErrorAlways))
		return true;

	std::string source;
	int64_t utcInteger;
	// rely on schema validation
	(void) parser.get("source", source);
	(void) parser.get("utc", utcInteger);

	time_t systemOffset = (time_t)utcInteger - time(0);

	PmLogInfo(sysServiceLogContext(), "SET_TIME", 3,
		PMLOGKS("SENDER", LSMessageGetSenderServiceName(message)),
		PMLOGKS("SOURCE", source.c_str()),
		PMLOGKFV("UTC_OFFSET", "%ld", systemOffset),
		"/clock/setTime received with %s",
		parser.getPayload()
	);

	ClockHandler &handler = *static_cast<ClockHandler*>(user_data);

	const char *reply;
	if (handler.update(systemOffset, source))
	{
		reply = "{\"returnValue\":true}";
	}
	else
	{
		reply = "{\"returnValue\":false}";
	}

	LSError lsError;
	LSErrorInit(&lsError);
	if (!LSMessageReply(lshandle, message, reply, &lsError))
	{
		PmLogError( sysServiceLogContext(), "SETTIME_REPLY_FAIL", 1,
		            PMLOGKS("REASON", lsError.message),
		            "Failed to send reply on /clock/setTime" );
		LSErrorFree(&lsError);
		return false;
	}

	return true;
}

bool ClockHandler::cbGetTime(LSHandle* lshandle, LSMessage *message, void *user_data)
{
	assert( user_data );

	jvalue_ref schema_v4 = convert_schema_v2_to_v4(STRICT_SCHEMA(
		PROPS_3(
			WITHDEFAULT(source, string, "system"),
			WITHDEFAULT(manualOverride, boolean, false),
			OPTIONAL(fallback, string)
		)
		));
	LSMessageJsonParser parser(message, jvalue_tostring_simple(schema_v4));
	if (!jis_null(schema_v4))
	    j_release(&schema_v4);

	if (!parser.parse(__FUNCTION__, lshandle, EValidateAndErrorAlways))
		return true;

	std::string source;
	bool manualOverride;
	// rely on schema validation
	(void) parser.get("source", source);
	(void) parser.get("manualOverride", manualOverride);

	std::string fallback;
	bool haveFallback = parser.get("fallback", fallback);

	ClockHandler &handler = *static_cast<ClockHandler*>(user_data);

	pbnjson::JValue reply = pbnjson::Object();

	bool isSystem = (source == system);
	ClockHandler::ClocksMap::const_iterator it = handler.m_clocks.end();

	// override any source if manual override requested and system-wide user time selected
	if (manualOverride && handler.m_manualOverride)
	{
		it = handler.m_clocks.find(manual);
		// if manual time is registered and set to some value
		if (it != handler.m_clocks.end() && it->second.systemOffset != invalidOffset)
		{
			// override if we have user time
			source = manual;
			isSystem = false;
			haveFallback = false;
		}
		else
		{
			// override found clock for "manual"
			it = handler.m_clocks.end();
		}
	}

	if (it == handler.m_clocks.end())
	{
		// find requested clock (if not overriden)
		it = handler.m_clocks.find(source);
	}

	// fallback logic
	if ( haveFallback &&
	     (it == handler.m_clocks.end() || it->second.systemOffset == invalidOffset) &&
	     !isSystem )
	{
		// lets replace our source with fallback
		it = handler.m_clocks.find(fallback);
		source = fallback;
		isSystem = (fallback == system);
	}

	if (isSystem) // special case
	{
		reply = createJsonReply(true);
		reply.put("source", system);
		pbnjson::JValue offset = pbnjson::Object();
		offset.put("value", 0);
		offset.put("source", system);
		reply.put("offset", offset);
		reply.put("utc", (int64_t)time(0));
		reply.put("systemTimeSource", TimePrefsHandler::instance()->getSystemTimeSource());
	}
	else if (it == handler.m_clocks.end())
	{
		PmLogError( sysServiceLogContext(), "WRONG_CLOCK_GETTIME", 2,
		            PMLOGKS("CLOCK_TAG", source.c_str()),
		            PMLOGKFV("FALLBACK", "%s", haveFallback ? "true" : "false"),
		            "Trying to fetch clock that is not registered" );
		reply = createJsonReply(false, 0, "Requested clock is not registered");
		reply.put("source", source);
	}
	else
	{
		if (it->second.systemOffset == invalidOffset)
		{
			reply = createJsonReply(false, 0, "No time available for that clock");
		}
		else
		{
			reply = createJsonReply(true);
			pbnjson::JValue offset = pbnjson::Object();
			offset.put("value", (int64_t)it->second.systemOffset);
			offset.put("source", system);
			reply.put("offset", offset);
			reply.put("utc", (int64_t)(time(0) + it->second.systemOffset));
		}
		reply.put("source", it->first);
		reply.put("priority", it->second.priority);
	}

	LSError lsError;
	LSErrorInit(&lsError);
	if (!LSMessageReply(lshandle, message, jsonToString(reply,"{}").c_str(), &lsError))
	{
		PmLogError( sysServiceLogContext(), "SETTIME_REPLY_FAIL", 1,
		            PMLOGKS("REASON", lsError.message),
		            "Failed to send reply on /clock/setTime" );
		LSErrorFree(&lsError);
		return false;
	}

	return true;
}
