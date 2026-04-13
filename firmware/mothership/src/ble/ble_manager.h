#pragma once

#include <Arduino.h>

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
