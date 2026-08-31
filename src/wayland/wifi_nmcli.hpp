#pragma once

#include <string>
#include <vector>

namespace tdvp::quick_settings {

struct WifiNetwork {
    std::string ssid;
    std::string security;
    int signal_percent = 0;
    bool active = false;
    bool saved = false;

    [[nodiscard]] bool requires_password() const;
};

struct WifiScanResult {
    bool ok = false;
    std::vector<WifiNetwork> networks;
    std::string error;
};

// Reads NetworkManager's already-known scan cache. It deliberately does not
// force a radio scan while the drawer is opening, so rendering never stalls on
// RF discovery. A later drawer opening receives NetworkManager's refreshed
// cache after the background scan request.
[[nodiscard]] WifiScanResult scan_wifi_networks();

// Start a NetworkManager operation without invoking a shell. Arguments such as
// SSIDs and passphrases are carried as individual execv arguments and cannot
// alter the command line syntax.
[[nodiscard]] bool request_wifi_rescan();
[[nodiscard]] bool request_wifi_radio(bool enabled);
[[nodiscard]] bool request_wifi_connect(const WifiNetwork& network,
                                        const std::string& passphrase = {});

}  // namespace tdvp::quick_settings
