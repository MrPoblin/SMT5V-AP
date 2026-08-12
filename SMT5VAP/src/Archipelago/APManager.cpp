#include "src/Log/Log.hpp"
#include "Archipelago.h" 
#include "APManager.hpp"
#include "APState.hpp"
#include "ItemSync.hpp"
#include <mutex>
#include <cstdint>
#include <atomic>
#include <set>
#include <vector>
#include <functional>
#include <chrono>
#include <Windows.h>

namespace APManager {
	constexpr char GameName[]{ "Shin Megami Tensei V: Vengeance" };

    static auto LastPollTime = std::chrono::steady_clock::now();
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);

    static std::mutex PendingCheckMutex;
    static std::set<int64_t> PendingChecks;

    static std::atomic<bool> isAPConnected{ false };
    static AP_RoomInfo RoomInfo{};

    static std::mutex SlotNameMutex;
    static std::string SlotName{};
    static std::string SeedName{};

    static std::string PrevSlotName{};
    static std::string PrevSeedName{};

    static std::mutex ConnectionCallbackMutex;
    static std::vector<ConnectionCallback> ConnectedCallbacks;
    static std::vector<ConnectionCallback> DisconnectedCallbacks;
    static std::vector<ConnectionCallback> NewSeedCallbacks;

    static void FireConnectionCallbacks(const std::vector<ConnectionCallback>& callbacks) {
        for (auto& cb : callbacks) cb();
    }

    void OnConnected(ConnectionCallback cb) {
        std::lock_guard lock(ConnectionCallbackMutex);
        ConnectedCallbacks.push_back(std::move(cb));
    }

    void OnDisconnected(ConnectionCallback cb) {
        std::lock_guard lock(ConnectionCallbackMutex);
        DisconnectedCallbacks.push_back(std::move(cb));
    }

    void OnNewSeed(ConnectionCallback cb) {
        std::lock_guard lock(ConnectionCallbackMutex);
        NewSeedCallbacks.push_back(std::move(cb));
    }

    // APCpp thread
	void OnItemReceived(int64_t itemId, bool notify){
		ItemSync::OnItemReceived(itemId, notify);
	}

    void Shutdown() {
        AP_Shutdown();
        APState::Locations::Clear();
    }

	void APInitialize(const char* IP, const char* PlayerName, const char* Password) {
        Shutdown();

		AP_Init(IP, GameName, PlayerName, Password);

		AP_SetItemClearCallback(APState::ClearState);
		AP_SetItemRecvCallback(OnItemReceived);
		AP_SetLocationCheckedCallback(APState::Locations::OnLocationChecked);
		AP_SetDeathLinkSupported(false); //TODO: Setup Death Link

        {
            std::lock_guard lock(SlotNameMutex);
            SlotName = PlayerName;
        }

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
        if (APState::Locations::Contains(checkId)) {
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
            AP_GetRoomInfo(&RoomInfo);

            bool isNewSeed = false;
            {
                std::lock_guard lock(SlotNameMutex);
                if ((SlotName == PrevSlotName && RoomInfo.seed_name == PrevSeedName) || PrevSeedName == "") {
                    //Same or first Seed
                    SendCheckQueue();
                }
                else {
                    //New Seed
                    //Need to reset a bunch of things
                    isNewSeed = true;
                }
                PrevSlotName = SlotName;
                PrevSeedName = RoomInfo.seed_name;
            }

            {
                std::lock_guard lock(ConnectionCallbackMutex);
                FireConnectionCallbacks(ConnectedCallbacks);
                if (isNewSeed) FireConnectionCallbacks(NewSeedCallbacks);
            }
            return;
        }
        if (!(AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated) && getAPConnected() == true) {
            LOG("AP Disconnected!");
            setAPConnected(false);

            {
                std::lock_guard lock(ConnectionCallbackMutex);
                FireConnectionCallbacks(DisconnectedCallbacks);
            }
            return;
        }
    }
}
