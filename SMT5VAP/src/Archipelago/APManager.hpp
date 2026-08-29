#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace APManager {
	void Shutdown();
	void APInitialize(const char* IP, const char* PlayerName, const char* Password);
	bool getAPConnected();
	const char* getAPConnectedStatus();

	// Connection info persisted to apconfig.json next to the game exe so the
	// same room can be reconnected to automatically on the next launch.
	struct APConfig {
		std::string IP;
		std::string SlotName;
		std::string Password;
	};

	// Loads apconfig.json (if present) into cfg. Returns false if missing/unreadable.
	bool LoadConfig(APConfig& cfg);
	// Saves cfg to apconfig.json.
	void SaveConfig(const APConfig& cfg);
	// Auto-connects using apconfig.json if it exists. Returns true if it started a connection.
	bool AutoConnectIfConfigured();
	void QueueSendCheck(int64_t checkId);
	void CheckAPConnection();

	// Fired on the game thread (from CheckAPConnection in on_update) when the
	// connection transitions to Authenticated / drops / changes seed.
	using ConnectionCallback = std::function<void()>;
	void OnConnected(ConnectionCallback cb);
	void OnDisconnected(ConnectionCallback cb);
	void OnNewSeed(ConnectionCallback cb);
}
