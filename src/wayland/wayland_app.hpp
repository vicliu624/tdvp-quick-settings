#pragma once

namespace tdvp::quick_settings {

class WaylandApp {
public:
    WaylandApp();
    ~WaylandApp();

    WaylandApp(const WaylandApp&) = delete;
    WaylandApp& operator=(const WaylandApp&) = delete;

    [[nodiscard]] int run(bool open_on_start);

private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace tdvp::quick_settings

