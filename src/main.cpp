#include "wayland/wayland_app.hpp"

#include <cstdio>
#include <cstring>

namespace {

void usage(const char* program)
{
    std::fprintf(stderr, "Usage: %s [--open]\n", program);
}

}  // namespace

int main(int argc, char** argv)
{
    bool open_on_start = false;
    if (argc == 2) {
        if (std::strcmp(argv[1], "--open") != 0) {
            usage(argv[0]);
            return 64;
        }
        open_on_start = true;
    } else if (argc != 1) {
        usage(argv[0]);
        return 64;
    }
    tdvp::quick_settings::WaylandApp app;
    return app.run(open_on_start);
}

