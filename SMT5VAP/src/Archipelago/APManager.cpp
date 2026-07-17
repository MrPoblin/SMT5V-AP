#include "src/Log/Log.hpp"
#include "Archipelago.h" 
#include <mutex>
#include <queue>
#include <cstdint>
#include <atomic>
#include <set>
#include <unordered_set>
#include <chrono>
#include <Windows.h>

namespace AP {
	constexpr char GameName[]{ "Shin Megami Tensei V: Vengeance" };

    static auto LastPollTime = std::chrono::steady_clock::now();
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);

    static std::mutex ItemQueueMutex;
    static std::queue<int64_t> ItemQueue;

    static std::mutex PendingCheckMutex;
    static std::set<int64_t> PendingChecks;

    static std::mutex CheckedLocationsMutex;
    static std::unordered_set<int64_t> CheckedLocations;

    static std::atomic<bool> isAPConnected{ false };


    void QueueReceivedItem(int64_t itemId) {
        std::lock_guard lock(ItemQueueMutex);
        ItemQueue.push(itemId);
    }

    bool DequeueReceivedItem(int64_t& out) {
        std::lock_guard lock(ItemQueueMutex);
        if (ItemQueue.empty()) return false;
        out = ItemQueue.front();
        ItemQueue.pop();
        return true;
    }

    // APCpp thread
	void OnItemReceived(int64_t itemId, bool notify)
	{
		if (!notify) return;
		QueueReceivedItem(itemId);
		LOG("Received and queued item: {}", itemId);
	}

    // APCpp thread
	void OnLocationChecked(int64_t locationId)
	{
        std::lock_guard lock(CheckedLocationsMutex);
        CheckedLocations.insert(locationId);
		LOG("Location in server checked: {}", locationId);
	}

    void Shutdown() {
        AP_Shutdown();
        CheckedLocations.clear();
    }

	void APInitialize(const char* IP, const char* PlayerName, const char* Password) {
        Shutdown();
		AP_Init(IP, GameName, PlayerName, Password);

		AP_SetItemClearCallback([]() {});
		AP_SetItemRecvCallback(OnItemReceived);
		AP_SetLocationCheckedCallback(OnLocationChecked);
		AP_SetDeathLinkSupported(false); //TODO: Setup Death Link

		AP_Start();
	}

    bool getAPConnected() {
        return isAPConnected.load(std::memory_order_acquire);
    }

    void setAPConnected(bool isConnected) {
        isAPConnected.store(isConnected, std::memory_order_release);
    }

    const char* getAPConnectedStatus(){
        switch (AP_GetConnectionStatus()) {
        case AP_ConnectionStatus::Authenticated:
            return "Connected";
        case AP_ConnectionStatus::Connected:
            return "Connecting...";
        case AP_ConnectionStatus::Disconnected:
            return "Disconnected";
        case AP_ConnectionStatus::ConnectionRefused:
            return "Connection Refused";
        default:
            return "Unknown";
        }
    }

    void QueueSendCheck(int64_t checkId) {
        std::lock_guard lock(CheckedLocationsMutex);
        if (CheckedLocations.contains(checkId)) {
            LOG("Location {} already checked!", checkId);
            return;
        }
        if (getAPConnected()) {
            AP_SendItem(checkId);
            LOG("Sending Check: {}", checkId);
            return;
        }
        std::lock_guard lockPending(PendingCheckMutex);
        PendingChecks.insert(checkId);
        LOG("Inserting check to queue: {}", checkId);
    }

    void SendCheckQueue() {
        std::lock_guard lock(PendingCheckMutex);
        if (PendingChecks.empty()) return;
        AP_SendItem(PendingChecks);
        LOG("Sending queue of {} checks", PendingChecks.size());
        PendingChecks.clear();
    }

    void CheckAPConnection() {
        auto now = std::chrono::steady_clock::now();
        if (now - LastPollTime < POLL_INTERVAL) return;
        LastPollTime = now;
        //If JUST connected
        if (AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated && getAPConnected() == false) {
            LOG("AP Connected!");
            setAPConnected(true);
            SendCheckQueue();
            return;
        }
        if (!(AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated) && getAPConnected() == true) {
            LOG("AP Disconnected!");
            setAPConnected(false);
            return;
        }
    }
}