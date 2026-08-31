#include "wayland/wifi_nmcli.hpp"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line)
{
    if (condition)
        return;
    std::cerr << "wifi_tests.cpp:" << line << ": expectation failed: " << expression << '\n';
    ++failures;
}

#define EXPECT(expression) expect((expression), #expression, __LINE__)

void test_nmcli_listing_is_deduplicated_and_ranked()
{
    const auto scan = tdvp::quick_settings::parse_nmcli_wifi_listing(
        " :Office\\:Guest:52:WPA2\n"
        "*:Studio:77:WPA2\n"
        " :Studio:94:WPA2\n"
        " :Cafe:61:--\n"
        " ::45:WPA2\n",
        {"Office:Guest", "Studio"});
    EXPECT(scan.ok);
    EXPECT(scan.networks.size() == 3U);
    EXPECT(scan.networks[0].ssid == "Studio");
    EXPECT(scan.networks[0].active);
    EXPECT(scan.networks[0].signal_percent == 77);
    EXPECT(scan.networks[0].saved);
    EXPECT(scan.networks[1].ssid == "Cafe");
    EXPECT(!scan.networks[1].requires_password());
    EXPECT(scan.networks[2].ssid == "Office:Guest");
    EXPECT(scan.networks[2].saved);
    EXPECT(scan.networks[2].requires_password());
}

}  // namespace

int main()
{
    test_nmcli_listing_is_deduplicated_and_ranked();
    if (failures == 0) {
        std::cout << "all tdvp-quick-settings Wi-Fi tests passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
