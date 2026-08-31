#include "wayland/wayland_app.hpp"

#include "core/controller.hpp"
#include "core/layout.hpp"
#include "core/state.hpp"
#include "wayland/provider_client.hpp"

#include <cairo/cairo.h>
#include <wayland-client.h>

extern "C" {
#include "xdg-shell-client-protocol.h"
// The upstream protocol's C argument is literally named `namespace`.  That is
// valid C, but not valid C++. Scope the compatibility macro strictly to the
// generated C header rather than carrying a patched protocol description.
#define namespace tdvp_layer_shell_namespace
#include "wlr-layer-shell-client-protocol.h"
#undef namespace
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/input-event-codes.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace tdvp::quick_settings {
namespace {

constexpr int kHiddenHeight = 8;
// Wayland does not send pointer motion after it leaves the transparent edge
// surface.  Keep this deliberately below the 8 px input strip so a genuine
// downward drag opens the full overlay before the pointer can leave it.
constexpr int kGestureOpenDistance = 4;
constexpr int kStatusHeight = 48;
constexpr uint32_t kVersionCompositor = 4;
constexpr uint32_t kVersionSeat = 5;
constexpr uint32_t kVersionLayerShell = 4;

struct Buffer {
    wl_buffer* wl = nullptr;
    cairo_surface_t* cairo = nullptr;
    std::uint8_t* data = nullptr;
    std::size_t length = 0;
    bool busy = false;
};

int create_anonymous_file(std::size_t size)
{
    char path[] = "/tmp/tdvp-quick-settings-XXXXXX";
    const int descriptor = mkstemp(path);
    if (descriptor < 0)
        return -1;
    if (unlink(path) != 0 || ftruncate(descriptor, static_cast<off_t>(size)) != 0) {
        const int saved_errno = errno;
        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    return descriptor;
}

void draw_rounded_rect(cairo_t* context, const Rect& rect, double radius)
{
    const double x = static_cast<double>(rect.x);
    const double y = static_cast<double>(rect.y);
    const double width = static_cast<double>(rect.width);
    const double height = static_cast<double>(rect.height);
    const double r = std::min(radius, std::min(width, height) / 2.0);
    cairo_new_sub_path(context);
    cairo_arc(context, x + width - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(context, x + width - r, y + height - r, r, 0.0, M_PI / 2.0);
    cairo_arc(context, x + r, y + height - r, r, M_PI / 2.0, M_PI);
    cairo_arc(context, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(context);
}

void draw_text(cairo_t* context, double x, double y, double size, const char* text, bool bold = false)
{
    cairo_select_font_face(context, "sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, size);
    cairo_move_to(context, x, y);
    cairo_show_text(context, text);
}

void draw_card(cairo_t* context, const Rect& rect, const char* title, const char* detail)
{
    draw_rounded_rect(context, rect, 10.0);
    cairo_set_source_rgb(context, 0.98, 0.97, 0.94);
    cairo_fill_preserve(context);
    cairo_set_source_rgb(context, 0.63, 0.59, 0.52);
    cairo_set_line_width(context, 1.2);
    cairo_stroke(context);

    cairo_set_source_rgb(context, 0.16, 0.16, 0.14);
    draw_text(context, rect.x + 18.0, rect.y + 37.0, 16.0, title, true);
    cairo_set_source_rgb(context, 0.31, 0.30, 0.27);
    draw_text(context, rect.x + 18.0, rect.y + 64.0, 13.0, detail);
}

void draw_slider(cairo_t* context, const Rect& rect, const char* title, int percent)
{
    draw_card(context, rect, title, "");
    const double y = static_cast<double>(rect.y + 72);
    const double start = static_cast<double>(rect.x + 22);
    const double width = static_cast<double>(rect.width - 44);
    const double active = width * static_cast<double>(percent) / 100.0;
    cairo_set_line_width(context, 6.0);
    cairo_set_line_cap(context, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(context, 0.82, 0.80, 0.75);
    cairo_move_to(context, start, y);
    cairo_line_to(context, start + width, y);
    cairo_stroke(context);
    cairo_set_source_rgb(context, 0.92, 0.30, 0.11);
    cairo_move_to(context, start, y);
    cairo_line_to(context, start + active, y);
    cairo_stroke(context);
    cairo_arc(context, start + active, y, 9.0, 0.0, 2.0 * M_PI);
    cairo_fill(context);
}

int clamp_percent(int value)
{
    return std::max(0, std::min(100, value));
}

int slider_percent(const Rect& rect, int pointer_x)
{
    const int start = rect.x + 22;
    const int width = rect.width - 44;
    if (width <= 0)
        return 0;
    return clamp_percent((pointer_x - start) * 100 / width);
}

std::string current_time_text()
{
    std::time_t now = std::time(nullptr);
    std::tm local {};
    if (localtime_r(&now, &local) == nullptr)
        return "--:--";
    char text[6] {};
    if (std::strftime(text, sizeof(text), "%H:%M", &local) == 0U)
        return "--:--";
    return text;
}

void launch_session_process(const std::vector<const char*>& arguments)
{
    if (arguments.empty())
        return;
    const pid_t child = fork();
    if (child != 0)
        return;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const char* argument : arguments)
        argv.push_back(const_cast<char*>(argument));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
}

}  // namespace

class WaylandApp::Impl {
public:
    ~Impl()
    {
        destroy_surface();
        if (pointer_ != nullptr)
            wl_pointer_destroy(pointer_);
        if (seat_ != nullptr)
            wl_seat_destroy(seat_);
        if (layer_shell_ != nullptr)
            zwlr_layer_shell_v1_destroy(layer_shell_);
        if (shm_ != nullptr)
            wl_shm_destroy(shm_);
        if (compositor_ != nullptr)
            wl_compositor_destroy(compositor_);
        if (registry_ != nullptr)
            wl_registry_destroy(registry_);
        if (display_ != nullptr)
            wl_display_disconnect(display_);
    }

    int run(bool open_on_start)
    {
        std::signal(SIGCHLD, SIG_IGN);
        display_ = wl_display_connect(nullptr);
        if (display_ == nullptr) {
            std::fprintf(stderr, "tdvp-quick-settings: cannot connect to Wayland display\n");
            return 69;
        }
        registry_ = wl_display_get_registry(display_);
        wl_registry_add_listener(registry_, &registry_listener(), this);
        if (wl_display_roundtrip(display_) < 0 || compositor_ == nullptr || shm_ == nullptr ||
            layer_shell_ == nullptr) {
            std::fprintf(stderr, "tdvp-quick-settings: compositor lacks required layer-shell interfaces\n");
            return 69;
        }
        open_ = open_on_start;
        if (open_)
            refresh_state();
        create_surface();
        while (running_ && wl_display_dispatch(display_) != -1) {
        }
        return 0;
    }

private:
    static const wl_registry_listener& registry_listener()
    {
        static wl_registry_listener listener {};
        listener.global = registry_global;
        listener.global_remove = registry_remove;
        return listener;
    }

    static const zwlr_layer_surface_v1_listener& layer_listener()
    {
        static zwlr_layer_surface_v1_listener listener {};
        listener.configure = layer_configure;
        listener.closed = layer_closed;
        return listener;
    }

    static const wl_pointer_listener& pointer_listener()
    {
        static wl_pointer_listener listener {};
        listener.enter = pointer_enter;
        listener.leave = pointer_leave;
        listener.motion = pointer_motion;
        listener.button = pointer_button;
        listener.axis = pointer_axis;
        listener.frame = pointer_frame;
        listener.axis_source = pointer_axis_source;
        listener.axis_stop = pointer_axis_stop;
        listener.axis_discrete = pointer_axis_discrete;
        return listener;
    }

    static const wl_buffer_listener& buffer_listener()
    {
        static wl_buffer_listener listener {};
        listener.release = buffer_release;
        return listener;
    }

    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t version)
    {
        auto* self = static_cast<Impl*>(data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self->compositor_ = static_cast<wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface,
                                 std::min(version, kVersionCompositor)));
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self->shm_ = static_cast<wl_shm*>(
                wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
            self->seat_ = static_cast<wl_seat*>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, kVersionSeat)));
            wl_seat_add_listener(self->seat_, &seat_listener(), self);
        } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
            self->layer_shell_ = static_cast<zwlr_layer_shell_v1*>(
                wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                                 std::min(version, kVersionLayerShell)));
        }
    }

    static void registry_remove(void*, wl_registry*, uint32_t)
    {
    }

    static const wl_seat_listener& seat_listener()
    {
        static wl_seat_listener listener {};
        listener.capabilities = seat_capabilities;
        listener.name = seat_name;
        return listener;
    }

    static void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities)
    {
        auto* self = static_cast<Impl*>(data);
        const bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
        if (has_pointer && self->pointer_ == nullptr) {
            self->pointer_ = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(self->pointer_, &pointer_listener(), self);
        } else if (!has_pointer && self->pointer_ != nullptr) {
            wl_pointer_destroy(self->pointer_);
            self->pointer_ = nullptr;
        }
    }

    static void seat_name(void*, wl_seat*, const char*)
    {
    }

    static void layer_configure(void* data, zwlr_layer_surface_v1* layer_surface,
                                uint32_t serial, uint32_t width, uint32_t height)
    {
        auto* self = static_cast<Impl*>(data);
        zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
        const int configured_width = static_cast<int>(width);
        const int configured_height = static_cast<int>(height);
        if (configured_width <= 0 || configured_height <= 0)
            return;
        if (self->width_ != configured_width || self->height_ != configured_height) {
            self->destroy_buffers();
            self->width_ = configured_width;
            self->height_ = configured_height;
            if (!self->create_buffers()) {
                std::fprintf(stderr, "tdvp-quick-settings: cannot allocate wl_shm buffers\n");
                self->running_ = false;
                return;
            }
        }
        self->redraw();
    }

    static void layer_closed(void* data, zwlr_layer_surface_v1*)
    {
        static_cast<Impl*>(data)->running_ = false;
    }

    static void pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t surface_x,
                              wl_fixed_t surface_y)
    {
        auto* self = static_cast<Impl*>(data);
        self->last_pointer_x_ = wl_fixed_to_int(surface_x);
        self->gesture_start_y_ = wl_fixed_to_int(surface_y);
        self->last_pointer_y_ = self->gesture_start_y_;
        self->gesture_active_ = true;
    }

    static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*)
    {
        static_cast<Impl*>(data)->gesture_active_ = false;
    }

    static void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t surface_x,
                               wl_fixed_t surface_y)
    {
        auto* self = static_cast<Impl*>(data);
        self->last_pointer_x_ = wl_fixed_to_int(surface_x);
        self->last_pointer_y_ = wl_fixed_to_int(surface_y);
        if (!self->open_ && self->gesture_active_ &&
            wl_fixed_to_int(surface_y) - self->gesture_start_y_ >= kGestureOpenDistance) {
            self->open_ = true;
            self->refresh_state();
            self->create_surface();
        }
    }

    static void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                               uint32_t state)
    {
        auto* self = static_cast<Impl*>(data);
        if (!self->open_ || button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
            return;
        self->handle_press(self->last_pointer_x_, self->last_pointer_y_);
    }

    static void pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t)
    {
    }

    static void pointer_frame(void*, wl_pointer*)
    {
    }

    static void pointer_axis_source(void*, wl_pointer*, uint32_t)
    {
    }

    static void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t)
    {
    }

    static void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t)
    {
    }

    static void buffer_release(void* data, wl_buffer*)
    {
        static_cast<Buffer*>(data)->busy = false;
    }

    void create_surface()
    {
        destroy_surface();
        surface_ = wl_compositor_create_surface(compositor_);
        if (surface_ == nullptr) {
            running_ = false;
            return;
        }
        const uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(layer_shell_, surface_, nullptr, layer,
                                                                "tdvp-quick-settings");
        zwlr_layer_surface_v1_add_listener(layer_surface_, &layer_listener(), this);
        if (open_) {
            zwlr_layer_surface_v1_set_anchor(
                layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            zwlr_layer_surface_v1_set_size(layer_surface_, 0, 0);
        } else {
            zwlr_layer_surface_v1_set_anchor(layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                                  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                                  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            zwlr_layer_surface_v1_set_size(layer_surface_, 0, kHiddenHeight);
        }
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_surface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(surface_);
    }

    void destroy_surface()
    {
        destroy_buffers();
        if (layer_surface_ != nullptr) {
            zwlr_layer_surface_v1_destroy(layer_surface_);
            layer_surface_ = nullptr;
        }
        if (surface_ != nullptr) {
            wl_surface_destroy(surface_);
            surface_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
    }

    bool create_buffers()
    {
        const int stride = width_ * 4;
        const std::size_t frame_length =
            static_cast<std::size_t>(stride) * static_cast<std::size_t>(height_);
        const std::size_t total_length = frame_length * buffers_.size();
        const int descriptor = create_anonymous_file(total_length);
        if (descriptor < 0)
            return false;
        void* data = mmap(nullptr, total_length, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        if (data == MAP_FAILED) {
            close(descriptor);
            return false;
        }
        mapping_ = data;
        mapping_length_ = total_length;
        wl_shm_pool* pool = wl_shm_create_pool(shm_, descriptor, static_cast<int>(total_length));
        close(descriptor);
        if (pool == nullptr) {
            munmap(mapping_, mapping_length_);
            mapping_ = nullptr;
            mapping_length_ = 0;
            return false;
        }
        for (std::size_t index = 0; index < buffers_.size(); ++index) {
            Buffer& buffer = buffers_[index];
            const std::size_t offset = index * frame_length;
            buffer.data = static_cast<std::uint8_t*>(mapping_) + offset;
            buffer.length = frame_length;
            buffer.cairo = cairo_image_surface_create_for_data(buffer.data, CAIRO_FORMAT_ARGB32, width_, height_, stride);
            buffer.wl = wl_shm_pool_create_buffer(pool, static_cast<int>(offset), width_, height_, stride,
                                                   WL_SHM_FORMAT_ARGB8888);
            if (buffer.wl == nullptr || cairo_surface_status(buffer.cairo) != CAIRO_STATUS_SUCCESS) {
                wl_shm_pool_destroy(pool);
                destroy_buffers();
                return false;
            }
            wl_buffer_add_listener(buffer.wl, &buffer_listener(), &buffer);
        }
        wl_shm_pool_destroy(pool);
        return true;
    }

    void destroy_buffers()
    {
        for (Buffer& buffer : buffers_) {
            if (buffer.wl != nullptr) {
                wl_buffer_destroy(buffer.wl);
                buffer.wl = nullptr;
            }
            if (buffer.cairo != nullptr) {
                cairo_surface_destroy(buffer.cairo);
                buffer.cairo = nullptr;
            }
            buffer.data = nullptr;
            buffer.length = 0;
            buffer.busy = false;
        }
        if (mapping_ != nullptr) {
            munmap(mapping_, mapping_length_);
            mapping_ = nullptr;
            mapping_length_ = 0;
        }
    }

    void redraw()
    {
        Buffer* buffer = nullptr;
        for (Buffer& candidate : buffers_) {
            if (!candidate.busy) {
                buffer = &candidate;
                break;
            }
        }
        if (buffer == nullptr || buffer->cairo == nullptr)
            return;

        cairo_t* context = cairo_create(buffer->cairo);
        if (open_) {
            draw_open(context);
        } else {
            cairo_save(context);
            cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.0);
            cairo_paint(context);
            cairo_restore(context);
        }
        cairo_destroy(context);
        cairo_surface_flush(buffer->cairo);
        wl_surface_attach(surface_, buffer->wl, 0, 0);
        wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
        wl_surface_commit(surface_);
        buffer->busy = true;
    }

    void draw_open(cairo_t* context)
    {
        cairo_set_source_rgb(context, 0.965, 0.945, 0.90);
        cairo_paint(context);
        cairo_set_source_rgb(context, 0.90, 0.31, 0.12);
        cairo_rectangle(context, 0.0, static_cast<double>(kStatusHeight - 3), static_cast<double>(width_), 3.0);
        cairo_fill(context);
        cairo_set_source_rgb(context, 0.15, 0.15, 0.13);
        draw_text(context, 24.0, 31.0, 18.0, "Quick Settings", true);
        const std::string clock = current_time_text();
        draw_text(context, static_cast<double>(width_ - 80), 31.0, 16.0, clock.c_str(), true);
        draw_text(context, 24.0, 88.0, 24.0, "Quick Settings", true);

        const QuickSettingsModel model = derive_model(snapshot_);
        const QuickSettingsLayout layout = make_layout(Extent {width_, height_}, model);
        if (!layout.supported)
            return;
        const char* wifi_detail = snapshot_.wifi_connected ? "Connected" :
                                  (snapshot_.wifi_enabled ? "On" : "Off");
        const char* audio_detail = snapshot_.audio_output == "external" ? "Speaker" :
                                   (snapshot_.audio_output == "internal" ? "Internal" : "Unavailable");
        const char* lora_detail = snapshot_.lora_enabled ? "On" : "Off";
        const char* fourth_title = model.fourth_primary_tile == AuxiliaryTile::Gps ? "GPS" : "On-screen Keyboard";
        const char* fourth_detail = model.fourth_primary_tile == AuxiliaryTile::Gps
                                         ? model.gps_detail.c_str() : "Show keyboard";
        draw_card(context, layout.primary_cards[0], "Wi-Fi", wifi_detail);
        draw_card(context, layout.primary_cards[1], "Audio Output", audio_detail);
        draw_card(context, layout.primary_cards[2], "LoRa", lora_detail);
        draw_card(context, layout.primary_cards[3], fourth_title, fourth_detail);
        draw_slider(context, layout.sliders[0], "Volume", snapshot_.volume_percent > 0 ? snapshot_.volume_percent : 60);
        draw_slider(context, layout.sliders[1], "Screen Brightness",
                    snapshot_.display_brightness_percent > 0 ? snapshot_.display_brightness_percent : 60);
        draw_slider(context, layout.sliders[2], "Keyboard Backlight",
                    snapshot_.keyboard_backlight_percent > 0 ? snapshot_.keyboard_backlight_percent : 45);
        draw_card(context, layout.secondary_actions[0], snapshot_.muted ? "Unmute" : "Mute", "Audio control");
        draw_card(context, layout.secondary_actions[1], "Settings", "System settings");
        draw_card(context, layout.system_actions[0], "Lock", "Lock session");
        draw_card(context, layout.system_actions[1], "Restart", "Restart system");
        draw_card(context, layout.system_actions[2], "Power Off", "Shut down");

        cairo_set_source_rgb(context, 0.72, 0.68, 0.60);
        cairo_set_line_width(context, 1.0);
        cairo_move_to(context, 908.0, 64.0);
        cairo_line_to(context, 908.0, static_cast<double>(height_ - 32));
        cairo_stroke(context);
        cairo_set_source_rgb(context, 0.15, 0.15, 0.13);
        draw_text(context, 928.0, 88.0, 22.0, "Networks", true);
        draw_card(context, layout.network_toggle, snapshot_.wifi_enabled ? "Wi-Fi On" : "Wi-Fi Off", "");
        const char* names[] = {"Saved networks", "Select in advanced settings", "NetworkManager", "Wi-Fi radio control"};
        for (std::size_t index = 0; index < layout.network_rows.size(); ++index)
            draw_card(context, layout.network_rows[index], names[index], "");
        draw_card(context, layout.network_settings, "Network settings", "Advanced options");

        if (!message_.empty()) {
            cairo_set_source_rgb(context, 0.52, 0.18, 0.08);
            draw_text(context, 24.0, 506.0, 13.0, message_.c_str());
        }
        if (pending_confirmation_ != Confirmation::None)
            draw_confirmation(context);
    }

    void draw_confirmation(cairo_t* context)
    {
        const Rect dialog {340, 188, 552, 170};
        draw_rounded_rect(context, dialog, 12.0);
        cairo_set_source_rgba(context, 0.12, 0.12, 0.10, 0.96);
        cairo_fill(context);
        cairo_set_source_rgb(context, 1.0, 0.98, 0.94);
        const char* text = pending_confirmation_ == Confirmation::StopLoraBeforeEnablingGps
                               ? "Stop LoRa and switch to GPS?"
                               : "Stop GPS and switch to LoRa?";
        draw_text(context, 368.0, 234.0, 20.0, text, true);
        draw_text(context, 368.0, 266.0, 14.0, "The K230 radio mux permits only one of these devices.");
        const Rect cancel {368, 290, 208, 48};
        const Rect confirm {656, 290, 208, 48};
        draw_card(context, cancel, "Cancel", "");
        draw_card(context, confirm, "Switch", "");
    }

    void refresh_state()
    {
        const ProviderReply reply = provider_.state();
        if (reply.ok) {
            snapshot_ = reply.snapshot;
            message_.clear();
        } else {
            message_ = reply.error;
        }
    }

    void execute_provider(const std::string& command)
    {
        const ProviderReply reply = provider_.request(command);
        if (reply.ok) {
            snapshot_ = reply.snapshot;
            message_.clear();
        } else {
            message_ = reply.error;
        }
        redraw();
    }

    void execute_radio_action(RequestedAction action, bool confirmed)
    {
        const ActionOutcome planned = plan_action(snapshot_, action, confirmed);
        if (!planned.accepted) {
            if (planned.confirmation != Confirmation::None) {
                pending_action_ = action;
                pending_confirmation_ = planned.confirmation;
                redraw();
            } else {
                message_ = "This keyboard does not provide GPS/LTE hardware.";
                redraw();
            }
            return;
        }
        for (std::size_t index = 0; index < planned.commands.size; ++index) {
            const BackendCommandKind command = planned.commands.values[index].kind;
            switch (command) {
            case BackendCommandKind::SetRadioProfileLora:
                execute_provider("SET radio-profile lora");
                break;
            case BackendCommandKind::SetRadioProfileNrf9151:
                execute_provider("SET radio-profile nrf9151");
                break;
            case BackendCommandKind::DisableRadio:
                execute_provider("SET radio-profile lora");
                break;
            case BackendCommandKind::SetLoraPowerOn:
                execute_provider("SET lora-power on");
                break;
            case BackendCommandKind::SetLoraPowerOff:
                execute_provider("SET lora-power off");
                break;
            case BackendCommandKind::SetGnssPowerOn:
                execute_provider("SET gnss-power on");
                break;
            case BackendCommandKind::SetGnssPowerOff:
                execute_provider("SET gnss-power off");
                break;
            }
        }
        pending_confirmation_ = Confirmation::None;
        refresh_state();
        redraw();
    }

    void handle_press(int pointer_x, int pointer_y)
    {
        if (pointer_y < kStatusHeight) {
            open_ = false;
            pending_confirmation_ = Confirmation::None;
            create_surface();
            return;
        }
        if (pending_confirmation_ != Confirmation::None) {
            const Rect cancel {368, 290, 208, 48};
            const Rect confirm {656, 290, 208, 48};
            if (cancel.contains(pointer_x, pointer_y)) {
                pending_confirmation_ = Confirmation::None;
                redraw();
            } else if (confirm.contains(pointer_x, pointer_y)) {
                execute_radio_action(pending_action_, true);
            }
            return;
        }
        const QuickSettingsModel model = derive_model(snapshot_);
        const QuickSettingsLayout layout = make_layout(Extent {width_, height_}, model);
        if (!layout.supported)
            return;
        if (layout.primary_cards[0].contains(pointer_x, pointer_y)) {
            launch_session_process({"/usr/bin/nmcli", "nmcli", "radio", "wifi", snapshot_.wifi_enabled ? "off" : "on"});
            message_ = snapshot_.wifi_enabled ? "Wi-Fi disabled" : "Wi-Fi enabled";
        } else if (layout.primary_cards[1].contains(pointer_x, pointer_y)) {
            execute_provider(snapshot_.audio_output == "external" ? "SET speaker-route internal" : "SET speaker-route external");
            return;
        } else if (layout.primary_cards[2].contains(pointer_x, pointer_y)) {
            execute_radio_action(snapshot_.lora_enabled ? RequestedAction::DisableLora : RequestedAction::EnableLora, false);
            return;
        } else if (layout.primary_cards[3].contains(pointer_x, pointer_y)) {
            if (model.fourth_primary_tile == AuxiliaryTile::Gps) {
                execute_radio_action(snapshot_.gps_enabled ? RequestedAction::DisableGps : RequestedAction::EnableGps, false);
                return;
            }
            launch_session_process({"/usr/local/bin/vpl-osk", "vpl-osk"});
            message_ = "On-screen keyboard toggled";
        } else if (layout.sliders[0].contains(pointer_x, pointer_y)) {
            const int percent = slider_percent(layout.sliders[0], pointer_x);
            launch_session_process({"/usr/bin/pactl", "pactl", "set-sink-volume", "@DEFAULT_SINK@", (std::to_string(percent) + "%").c_str()});
            snapshot_.volume_percent = percent;
        } else if (layout.sliders[1].contains(pointer_x, pointer_y)) {
            execute_provider("SET display-brightness " + std::to_string(slider_percent(layout.sliders[1], pointer_x)));
            return;
        } else if (layout.sliders[2].contains(pointer_x, pointer_y)) {
            execute_provider("SET keyboard-backlight " + std::to_string(slider_percent(layout.sliders[2], pointer_x)));
            return;
        } else if (layout.secondary_actions[0].contains(pointer_x, pointer_y)) {
            launch_session_process({"/usr/bin/pactl", "pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle"});
            snapshot_.muted = !snapshot_.muted;
        } else if (layout.secondary_actions[1].contains(pointer_x, pointer_y)) {
            launch_session_process({"/usr/bin/nm-connection-editor", "nm-connection-editor"});
        } else if (layout.system_actions[0].contains(pointer_x, pointer_y)) {
            execute_provider("SYSTEM lock");
            return;
        } else if (layout.system_actions[1].contains(pointer_x, pointer_y)) {
            execute_provider("SYSTEM reboot");
            return;
        } else if (layout.system_actions[2].contains(pointer_x, pointer_y)) {
            execute_provider("SYSTEM poweroff");
            return;
        } else if (layout.network_toggle.contains(pointer_x, pointer_y)) {
            launch_session_process({"/usr/bin/nmcli", "nmcli", "radio", "wifi", snapshot_.wifi_enabled ? "off" : "on"});
        } else if (layout.network_settings.contains(pointer_x, pointer_y)) {
            launch_session_process({"/usr/bin/nm-connection-editor", "nm-connection-editor"});
        }
        redraw();
    }

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_surface_ = nullptr;
    std::array<Buffer, 2> buffers_ {};
    void* mapping_ = nullptr;
    std::size_t mapping_length_ = 0;
    int width_ = 0;
    int height_ = 0;
    int gesture_start_y_ = 0;
    int last_pointer_x_ = 0;
    int last_pointer_y_ = 0;
    bool gesture_active_ = false;
    bool open_ = false;
    bool running_ = true;
    ProviderClient provider_;
    HardwareSnapshot snapshot_;
    std::string message_;
    RequestedAction pending_action_ = RequestedAction::EnableLora;
    Confirmation pending_confirmation_ = Confirmation::None;
};

WaylandApp::WaylandApp()
    : impl_(new Impl())
{
}

WaylandApp::~WaylandApp()
{
    delete impl_;
}

int WaylandApp::run(bool open_on_start)
{
    return impl_->run(open_on_start);
}

}  // namespace tdvp::quick_settings
