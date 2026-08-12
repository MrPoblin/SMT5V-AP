#pragma once

#include <cstdint>
#include <functional>

namespace APManager {
	void Shutdown();
	void APInitialize(const char* IP, const char* PlayerName, const char* Password);
	bool getAPConnected();
	const char* getAPConnectedStatus();
	void QueueSendCheck(int64_t checkId);
	void CheckAPConnection();

	// Fired on the game thread (from CheckAPConnection in on_update) when the
	// connection transitions to Authenticated / drops / changes seed.
	using ConnectionCallback = std::function<void()>;
	void OnConnected(ConnectionCallback cb);
	void OnDisconnected(ConnectionCallback cb);
	void OnNewSeed(ConnectionCallback cb);
}
