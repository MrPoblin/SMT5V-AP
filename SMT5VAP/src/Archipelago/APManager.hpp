#pragma once

#include <cstdint>

namespace APManager {
	void Shutdown();
	void APInitialize(const char* IP, const char* PlayerName, const char* Password);
	bool getAPConnected();
	const char* getAPConnectedStatus();
	void QueueSendCheck(int64_t checkId);
	void CheckAPConnection();
}