#pragma once

#include <Arduino.h>
#include "protocol.h"

using BleStatusJsonProvider = String (*)();
using BleNowUnixProvider = unsigned long (*)();
using BleCommandRouter = bool (*)(
	const String& command,
	const String& payloadJson,
	String& responseType,
	String& responseDataJson,
	String& responseMessage,
	String& errorCode,
	String& errorMessage
);

void bleSetup(
	const char* deviceId,
	const char* firmwareVersion,
	BleStatusJsonProvider statusProvider,
	BleNowUnixProvider nowUnixProvider,
	BleCommandRouter commandRouter
);
void bleLoop();
bool bleIsConnected();
void blePublishTelemetryEvent(const sensor_data_message_t& sample, const uint8_t mac[6]);
