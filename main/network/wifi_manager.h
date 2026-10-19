/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One owner for the ESP-IDF Wi-Fi driver. Lifecycle methods and tick() are
// called by the main application task. Snapshot reads and scan requests are
// safe from other tasks; event/HTTP callbacks only enqueue bounded messages.
class WifiManager {
public:
    struct Network {
        std::string ssid;
        int rssi = 0;
        bool secure = false;
    };
    struct Snapshot {
        std::string state = "idle";
        std::string message = "Wi-Fi is not started";
        std::string ssid;
        std::string ap_ssid;
        std::string ap_password;
        std::uint32_t seconds_remaining = 0;
        std::uint32_t revision = 0;
        std::vector<Network> networks;
        bool scan_busy = false;
        bool provisioning = false;
        // Actual driver/validated-link state, never inferred from GOT_IP alone.
        bool radio_on = false;
        // Driver stopped, STOP observed, and asynchronous teardown drained.
        bool radio_idle = false;
        bool station_connected = false;
        bool clock_mode = false;
        bool has_saved_credentials = false;
    };

    static WifiManager& instance();
    bool begin();
    void tick();
    bool startProvisioning();
    void stopProvisioning();
    bool provisioning() const;
    Snapshot snapshot() const;
    void requestScan();

    // Main-task-only policy controls. Clock mode revokes provisioning/scans
    // and stops the driver except during an explicitly enabled sync window.
    void setClockMode(bool active);
    void setClockSyncEnabled(bool enabled);

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

private:
    WifiManager();
    struct Impl;
    Impl* _impl;
};
