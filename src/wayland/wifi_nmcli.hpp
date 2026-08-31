#pragma once

#include <string>
#include <string_view>
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

// Pure parser for NetworkManager's `nmcli -t --escape yes` listing. Keeping it
// separate from process execution lets the exact data contract be tested on a
// host without a Wi-Fi adapter or a running NetworkManager instance.
[[nodiscard]] WifiScanResult parse_nmcli_wifi_listing(
    std::string_view listing, const std::vector<std::string>& saved_ssids);

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
