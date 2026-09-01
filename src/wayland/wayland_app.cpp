#include "wayland/wayland_app.hpp"

#include "core/controller.hpp"
#include "core/layout.hpp"
#include "core/orientation.hpp"
#include "core/state.hpp"
#include "wayland/provider_client.hpp"
#include "wayland/wifi_nmcli.hpp"

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
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace tdvp::quick_settings {
namespace {

// A physical touchscreen needs a forgiving hit target: an 8 px strip is easy
// to miss through controller jitter or a status-panel hit test.  The hidden
// layer remains transparent, but covers the upper half of the 64 px status
// bar so a normal Android-style pull-down begins in our surface reliably.
constexpr int kHiddenHeight = 32;
// Wayland does not send pointer motion after it leaves the transparent edge
// surface. Keep this deliberately below the edge strip so a genuine downward
// drag opens the full overlay before the pointer can leave it.
constexpr int kGestureOpenDistance = 4;
constexpr int kGestureCloseDistance = 24;
constexpr int kBottomDismissZoneHeight = 32;
constexpr auto kSliderUpdateInterval = std::chrono::milliseconds(45);
constexpr int kStatusHeight = 48;
constexpr int kDrawerAnimationDurationMs = 180;
constexpr uint32_t kVersionCompositor = 4;
constexpr uint32_t kVersionSeat = 5;
constexpr uint32_t kVersionLayerShell = 4;
constexpr uint32_t kVersionOutput = 2;

struct Buffer {
    wl_buffer* wl = nullptr;
    cairo_surface_t* cairo = nullptr;
    std::uint8_t* data = nullptr;
    std::size_t length = 0;
    void* owner = nullptr;
    bool busy = false;
};

struct SliderWorkerRequest {
    int slider = -1;
    int percent = 0;
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
    if (rect.height < 52) {
        draw_text(context, rect.x + 18.0, rect.y + std::min(34.0, rect.height - 14.0), 16.0,
                  title, true);
        return;
    }
    if (rect.height < 76) {
        draw_text(context, rect.x + 18.0, rect.y + 25.0, 16.0, title, true);
        if (detail[0] != '\0') {
            cairo_set_source_rgb(context, 0.31, 0.30, 0.27);
            draw_text(context, rect.x + 18.0, rect.y + 45.0, 13.0, detail);
        }
        return;
    }
    draw_text(context, rect.x + 18.0, rect.y + 37.0, 16.0, title, true);
    if (detail[0] != '\0') {
        cairo_set_source_rgb(context, 0.31, 0.30, 0.27);
        draw_text(context, rect.x + 18.0, rect.y + 64.0, 13.0, detail);
    }
}

void draw_slider(cairo_t* context, const Rect& rect, const char* title, int percent,
                 bool slider_active)
{
    draw_card(context, rect, title, "");
    percent = std::max(0, std::min(100, percent));
    const std::string percent_text = std::to_string(percent) + "%";
    cairo_set_source_rgb(context, 0.31, 0.30, 0.27);
    draw_text(context, static_cast<double>(rect.x + rect.width - 54),
              static_cast<double>(rect.y + 34), 15.0, percent_text.c_str(), true);

    const double y = static_cast<double>(rect.y + rect.height - 27);
    const double start = static_cast<double>(rect.x + 26);
    const double width = static_cast<double>(rect.width - 52);
    const double filled = width * static_cast<double>(percent) / 100.0;
    cairo_set_line_width(context, 12.0);
    cairo_set_line_cap(context, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(context, 0.82, 0.80, 0.75);
    cairo_move_to(context, start, y);
    cairo_line_to(context, start + width, y);
    cairo_stroke(context);
    cairo_set_source_rgb(context, slider_active ? 0.78 : 0.92, slider_active ? 0.20 : 0.30,
                         slider_active ? 0.06 : 0.11);
    cairo_move_to(context, start, y);
    cairo_line_to(context, start + filled, y);
    cairo_stroke(context);
    cairo_arc(context, start + filled, y, slider_active ? 15.0 : 13.0, 0.0, 2.0 * M_PI);
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

void launch_session_process(const std::vector<std::string>& arguments)
{
    if (arguments.empty())
        return;
    const pid_t child = fork();
    if (child < 0)
        return;
    if (child != 0) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return;
    }
    const pid_t grandchild = fork();
    if (grandchild != 0)
        _exit(grandchild < 0 ? 127 : 0);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
}

}  // namespace

class WaylandApp::Impl {
public:
    ~Impl()
    {
        stop_slider_worker();
        destroy_surface();
        if (output_ != nullptr)
            wl_output_destroy(output_);
        if (touch_ != nullptr)
            wl_touch_destroy(touch_);
        if (keyboard_ != nullptr)
            wl_keyboard_destroy(keyboard_);
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
        if (open_) {
            opening_animation_pending_ = true;
            refresh_state();
            refresh_wifi_networks();
        }
        create_surface();
        while (running_ && wl_display_dispatch(display_) != -1)
            reap_slider_worker();
        return 0;
    }

private:
    enum class SliderDragOwner {
        None,
        Pointer,
        Touch,
    };

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

    static const wl_output_listener& output_listener()
    {
        static wl_output_listener listener {};
        listener.geometry = output_geometry;
        listener.mode = output_mode;
        listener.done = output_done;
        listener.scale = output_scale;
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

    static const wl_touch_listener& touch_listener()
    {
        static wl_touch_listener listener {};
        listener.down = touch_down;
        listener.up = touch_up;
        listener.motion = touch_motion;
        listener.frame = touch_frame;
        listener.cancel = touch_cancel;
        listener.shape = touch_shape;
        listener.orientation = touch_orientation;
        return listener;
    }

    static const wl_keyboard_listener& keyboard_listener()
    {
        static wl_keyboard_listener listener {};
        listener.keymap = keyboard_keymap;
        listener.enter = keyboard_enter;
        listener.leave = keyboard_leave;
        listener.key = keyboard_key;
        listener.modifiers = keyboard_modifiers;
        listener.repeat_info = keyboard_repeat_info;
        return listener;
    }

    static const wl_buffer_listener& buffer_listener()
    {
        static wl_buffer_listener listener {};
        listener.release = buffer_release;
        return listener;
    }

    static const wl_callback_listener& frame_listener()
    {
        static wl_callback_listener listener {};
        listener.done = frame_done;
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
        } else if (std::strcmp(interface, wl_output_interface.name) == 0 && self->output_ == nullptr) {
            self->output_ = static_cast<wl_output*>(
                wl_registry_bind(registry, name, &wl_output_interface, std::min(version, kVersionOutput)));
            wl_output_add_listener(self->output_, &output_listener(), self);
        }
    }

    static void registry_remove(void*, wl_registry*, uint32_t)
    {
    }

    static void output_geometry(void* data, wl_output*, int32_t, int32_t, int32_t, int32_t,
                                int32_t, const char*, const char*, int32_t transform)
    {
        auto* self = static_cast<Impl*>(data);
        if (transform >= WL_OUTPUT_TRANSFORM_NORMAL && transform <= WL_OUTPUT_TRANSFORM_270)
            self->output_transform_ = static_cast<SurfaceTransform>(transform);
        else
            self->output_transform_ = SurfaceTransform::Normal;
    }

    static void output_mode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height,
                            int32_t)
    {
        auto* self = static_cast<Impl*>(data);
        if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
            return;
        self->output_mode_width_ = width;
        self->output_mode_height_ = height;
    }

    static void output_done(void*, wl_output*)
    {
    }

    static void output_scale(void*, wl_output*, int32_t)
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
        const bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
        if (has_keyboard && self->keyboard_ == nullptr) {
            self->keyboard_ = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(self->keyboard_, &keyboard_listener(), self);
        } else if (!has_keyboard && self->keyboard_ != nullptr) {
            wl_keyboard_destroy(self->keyboard_);
            self->keyboard_ = nullptr;
        }
        const bool has_touch = (capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0;
        if (has_touch && self->touch_ == nullptr) {
            self->touch_ = wl_seat_get_touch(seat);
            wl_touch_add_listener(self->touch_, &touch_listener(), self);
        } else if (!has_touch && self->touch_ != nullptr) {
            wl_touch_destroy(self->touch_);
            self->touch_ = nullptr;
            self->active_touch_id_ = -1;
        }
    }

    static void seat_name(void*, wl_seat*, const char*)
    {
    }

    static void keyboard_keymap(void*, wl_keyboard*, uint32_t, int fd, uint32_t)
    {
        if (fd >= 0)
            close(fd);
    }

    static void keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*)
    {
    }

    static void keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*)
    {
    }

    static void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                             uint32_t state)
    {
        auto* self = static_cast<Impl*>(data);
        if (key == KEY_LEFTSHIFT)
            self->left_shift_ = state == WL_KEYBOARD_KEY_STATE_PRESSED;
        else if (key == KEY_RIGHTSHIFT)
            self->right_shift_ = state == WL_KEYBOARD_KEY_STATE_PRESSED;
        if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !self->wifi_password_active())
            return;
        if (key == KEY_ESC) {
            self->cancel_wifi_password();
            return;
        }
        if (key == KEY_BACKSPACE) {
            if (!self->wifi_passphrase_.empty())
                self->wifi_passphrase_.pop_back();
            self->redraw();
            return;
        }
        if (key == KEY_ENTER || key == KEY_KPENTER) {
            self->submit_wifi_password();
            return;
        }
        const char character = self->password_character(key);
        if (character != '\0' && self->wifi_passphrase_.size() < 63U) {
            self->wifi_passphrase_ += character;
            self->redraw();
        }
    }

    static void keyboard_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t,
                                   uint32_t)
    {
    }

    static void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t)
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
        const Extent configured_surface {configured_width, configured_height};
        // Labwc exposes layer-shell configure dimensions in logical output
        // coordinates (1232x504 below the existing 64px panel on K230), even
        // though wl_output's native DRM mode is 568x1232 with transform=90.
        // Do not rotate a buffer a second time in that normal case.  Keep the
        // fallback for compositors which instead configure a narrow native
        // edge surface directly.
        const bool configured_in_native_axes =
            is_quarter_turn(self->output_transform_) &&
            ((configured_width == self->output_mode_width_ &&
              configured_height <= self->output_mode_height_) ||
             (configured_height == self->output_mode_height_ &&
              configured_width <= self->output_mode_width_));
        const SurfaceTransform buffer_transform = configured_in_native_axes
                                                      ? inverse_transform(self->output_transform_)
                                                      : SurfaceTransform::Normal;
        const Extent buffer_extent =
            buffer_extent_for_surface(configured_surface, buffer_transform);
        if (self->surface_width_ != configured_width || self->surface_height_ != configured_height ||
            self->width_ != buffer_extent.width || self->height_ != buffer_extent.height ||
            self->buffer_transform_ != buffer_transform) {
            self->destroy_buffers();
            self->surface_width_ = configured_width;
            self->surface_height_ = configured_height;
            self->width_ = buffer_extent.width;
            self->height_ = buffer_extent.height;
            self->buffer_transform_ = buffer_transform;
            wl_surface_set_buffer_transform(
                self->surface_, static_cast<int32_t>(self->buffer_transform_));
            std::fprintf(stderr,
                         "tdvp-quick-settings: layer %dx%d, output %dx%d transform %d, buffer %dx%d\n",
                         configured_width, configured_height, self->output_mode_width_,
                         self->output_mode_height_, static_cast<int>(self->output_transform_),
                         self->width_, self->height_);
            if (!self->create_buffers()) {
                std::fprintf(stderr, "tdvp-quick-settings: cannot allocate wl_shm buffers\n");
                self->running_ = false;
                return;
            }
        }
        if (self->open_ && self->opening_animation_pending_ &&
            configured_height > kHiddenHeight) {
            self->opening_animation_pending_ = false;
            self->opening_animation_active_ = true;
            self->animation_started_ = std::chrono::steady_clock::now();
            (void)self->create_animation_snapshot();
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
        const Point point = self->map_surface_point(wl_fixed_to_int(surface_x),
                                                    wl_fixed_to_int(surface_y));
        self->last_pointer_x_ = point.x;
        self->gesture_start_y_ = point.y;
        self->last_pointer_y_ = point.y;
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
        const Point point = self->map_surface_point(wl_fixed_to_int(surface_x),
                                                    wl_fixed_to_int(surface_y));
        self->last_pointer_x_ = point.x;
        self->last_pointer_y_ = point.y;
        if (self->slider_drag_owner_ == SliderDragOwner::Pointer) {
            self->update_active_slider(point.x, false);
            return;
        }
        if (self->open_ && !self->closing_animation_active_ && self->gesture_active_ &&
            self->gesture_start_y_ >= self->height_ - kBottomDismissZoneHeight &&
            self->gesture_start_y_ - point.y >= kGestureCloseDistance) {
            self->begin_close_animation();
            return;
        }
        if (!self->open_ && self->gesture_active_ &&
            point.y - self->gesture_start_y_ >= kGestureOpenDistance) {
            self->open_ = true;
            self->opening_animation_pending_ = true;
            self->refresh_state();
            self->refresh_wifi_networks();
            self->create_surface();
        }
    }

    static void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                               uint32_t state)
    {
        auto* self = static_cast<Impl*>(data);
        const auto now = std::chrono::steady_clock::now();
        if (button != BTN_LEFT)
            return;
        if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
            self->slider_drag_owner_ == SliderDragOwner::Pointer) {
            self->update_active_slider(self->last_pointer_x_, true);
            self->end_slider_drag();
            return;
        }
        if (!self->open_ || state != WL_POINTER_BUTTON_STATE_PRESSED)
            return;
        if (self->active_touch_id_ >= 0 || self->touch_event_is_recent(now))
            return;
        self->last_pointer_press_ = now;
        if (self->begin_slider_drag(self->last_pointer_x_, self->last_pointer_y_,
                                    SliderDragOwner::Pointer))
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

    static void touch_down(void* data, wl_touch*, uint32_t, uint32_t, wl_surface*, int32_t id,
                           wl_fixed_t surface_x, wl_fixed_t surface_y)
    {
        auto* self = static_cast<Impl*>(data);
        // One-finger gestures drive the control center.  Additional contacts
        // are deliberately ignored so a second finger cannot click a control
        // while the first finger is opening or dragging the drawer.
        if (self->active_touch_id_ >= 0)
            return;
        const auto now = std::chrono::steady_clock::now();
        self->last_touch_event_ = now;
        if (self->pointer_press_is_recent(now))
            return;
        const Point point = self->map_surface_point(wl_fixed_to_int(surface_x),
                                                    wl_fixed_to_int(surface_y));
        self->active_touch_id_ = id;
        self->last_pointer_x_ = point.x;
        self->last_pointer_y_ = point.y;
        self->gesture_start_y_ = point.y;
        self->gesture_active_ = true;
        self->closed_by_touch_drag_ = false;
        if (self->begin_slider_drag(point.x, point.y, SliderDragOwner::Touch))
            return;
    }

    static void touch_up(void* data, wl_touch*, uint32_t, uint32_t, int32_t id)
    {
        auto* self = static_cast<Impl*>(data);
        if (id != self->active_touch_id_)
            return;
        const bool opened_by_drag = self->opened_by_touch_drag_;
        const bool closed_by_drag = self->closed_by_touch_drag_;
        const bool slider_drag = self->slider_drag_owner_ == SliderDragOwner::Touch;
        if (slider_drag) {
            self->update_active_slider(self->last_pointer_x_, true);
            self->end_slider_drag();
        }
        self->active_touch_id_ = -1;
        self->gesture_active_ = false;
        self->opened_by_touch_drag_ = false;
        self->closed_by_touch_drag_ = false;
        self->last_touch_event_ = std::chrono::steady_clock::now();
        if (self->open_ && !opened_by_drag && !closed_by_drag && !slider_drag)
            self->handle_press(self->last_pointer_x_, self->last_pointer_y_);
    }

    static void touch_motion(void* data, wl_touch*, uint32_t, int32_t id, wl_fixed_t surface_x,
                             wl_fixed_t surface_y)
    {
        auto* self = static_cast<Impl*>(data);
        if (id != self->active_touch_id_)
            return;
        const Point point = self->map_surface_point(wl_fixed_to_int(surface_x),
                                                    wl_fixed_to_int(surface_y));
        self->last_pointer_x_ = point.x;
        self->last_pointer_y_ = point.y;
        if (self->slider_drag_owner_ == SliderDragOwner::Touch) {
            self->update_active_slider(point.x, false);
            return;
        }
        if (self->open_ && !self->closing_animation_active_ &&
            self->gesture_start_y_ >= self->height_ - kBottomDismissZoneHeight &&
            self->gesture_start_y_ - point.y >= kGestureCloseDistance) {
            self->closed_by_touch_drag_ = true;
            self->begin_close_animation();
            return;
        }
        if (!self->open_ && point.y - self->gesture_start_y_ >= kGestureOpenDistance) {
            self->opened_by_touch_drag_ = true;
            self->open_ = true;
            self->opening_animation_pending_ = true;
            self->refresh_state();
            self->refresh_wifi_networks();
            self->create_surface();
        }
    }

    static void touch_frame(void*, wl_touch*)
    {
    }

    static void touch_cancel(void* data, wl_touch*)
    {
        auto* self = static_cast<Impl*>(data);
        self->active_touch_id_ = -1;
        self->gesture_active_ = false;
        self->opened_by_touch_drag_ = false;
        self->closed_by_touch_drag_ = false;
        if (self->slider_drag_owner_ == SliderDragOwner::Touch)
            self->end_slider_drag();
        self->last_touch_event_ = std::chrono::steady_clock::now();
    }

    static void touch_shape(void*, wl_touch*, int32_t, wl_fixed_t, wl_fixed_t)
    {
    }

    static void touch_orientation(void*, wl_touch*, int32_t, wl_fixed_t)
    {
    }

    static void buffer_release(void* data, wl_buffer*)
    {
        auto* buffer = static_cast<Buffer*>(data);
        buffer->busy = false;
        auto* self = static_cast<Impl*>(buffer->owner);
        if (self != nullptr && self->surface_ != nullptr && self->redraw_pending_) {
            self->redraw_pending_ = false;
            self->redraw();
        }
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
        layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(layer_shell_, surface_, output_, layer,
                                                                "tdvp-quick-settings");
        zwlr_layer_surface_v1_add_listener(layer_surface_, &layer_listener(), this);
        if (open_) {
            zwlr_layer_surface_v1_set_anchor(
                layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            zwlr_layer_surface_v1_set_size(layer_surface_, 0, 0);
        } else {
            zwlr_layer_surface_v1_set_anchor(
                layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            zwlr_layer_surface_v1_set_size(layer_surface_, 0, kHiddenHeight);
        }
        // The trigger must cover the *physical/logical* top edge rather than
        // the work area below wf-panel-pi.  A negative exclusive zone asks the
        // compositor not to move this transparent, non-reserving edge surface
        // below another panel's reserved zone.  Once opened, use the normal
        // work area so the drawer keeps the verified 1232x504 layout below
        // the existing 64px status bar.
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, open_ ? 0 : -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_surface_, wifi_password_active() ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                                                   : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(surface_);
    }

    void destroy_surface()
    {
        if (frame_callback_ != nullptr) {
            wl_callback_destroy(frame_callback_);
            frame_callback_ = nullptr;
        }
        destroy_animation_snapshot();
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
        surface_width_ = 0;
        surface_height_ = 0;
        buffer_transform_ = SurfaceTransform::Normal;
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
            buffer.owner = this;
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
        redraw_pending_ = false;
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
            buffer.owner = nullptr;
            buffer.busy = false;
        }
        if (mapping_ != nullptr) {
            munmap(mapping_, mapping_length_);
            mapping_ = nullptr;
            mapping_length_ = 0;
        }
    }

    [[nodiscard]] Point map_surface_point(int surface_x, int surface_y) const
    {
        if (width_ <= 0 || height_ <= 0)
            return Point {surface_x, surface_y};
        return surface_to_buffer(Point {surface_x, surface_y}, Extent {width_, height_},
                                  buffer_transform_);
    }

    [[nodiscard]] bool animation_active() const
    {
        return opening_animation_active_ || closing_animation_active_;
    }

    [[nodiscard]] bool touch_event_is_recent(std::chrono::steady_clock::time_point now) const
    {
        if (last_touch_event_.time_since_epoch().count() == 0)
            return false;
        return now - last_touch_event_ < std::chrono::milliseconds(400);
    }

    [[nodiscard]] bool pointer_press_is_recent(std::chrono::steady_clock::time_point now) const
    {
        if (last_pointer_press_.time_since_epoch().count() == 0)
            return false;
        return now - last_pointer_press_ < std::chrono::milliseconds(400);
    }

    [[nodiscard]] double animation_progress() const
    {
        if (!animation_active())
            return 1.0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - animation_started_);
        return std::max(0.0, std::min(1.0,
                                     static_cast<double>(elapsed.count()) /
                                         static_cast<double>(kDrawerAnimationDurationMs)));
    }

    [[nodiscard]] double visible_drawer_fraction() const
    {
        const double progress = animation_progress();
        const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
        if (closing_animation_active_)
            return 1.0 - eased;
        if (opening_animation_active_)
            return eased;
        return 1.0;
    }

    void begin_close_animation()
    {
        if (!open_ || closing_animation_active_)
            return;
        opening_animation_pending_ = false;
        opening_animation_active_ = false;
        (void)create_animation_snapshot();
        closing_animation_active_ = true;
        animation_started_ = std::chrono::steady_clock::now();
        redraw();
    }

    static void frame_done(void* data, wl_callback* callback, uint32_t)
    {
        auto* self = static_cast<Impl*>(data);
        wl_callback_destroy(callback);
        self->frame_callback_ = nullptr;
        if (!self->animation_active())
            return;
        if (self->animation_progress() >= 1.0) {
            if (self->closing_animation_active_) {
                self->closing_animation_active_ = false;
                self->destroy_animation_snapshot();
                self->open_ = false;
                self->stop_slider_worker();
                self->create_surface();
                return;
            }
            self->opening_animation_active_ = false;
            self->destroy_animation_snapshot();
        }
        self->redraw();
    }

    bool create_animation_snapshot()
    {
        destroy_animation_snapshot();
        if (width_ <= 0 || height_ <= 0)
            return false;
        animation_snapshot_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_, height_);
        if (animation_snapshot_ == nullptr ||
            cairo_surface_status(animation_snapshot_) != CAIRO_STATUS_SUCCESS) {
            destroy_animation_snapshot();
            return false;
        }
        cairo_t* context = cairo_create(animation_snapshot_);
        if (cairo_status(context) != CAIRO_STATUS_SUCCESS) {
            cairo_destroy(context);
            destroy_animation_snapshot();
            return false;
        }
        draw_open_contents(context);
        cairo_destroy(context);
        cairo_surface_flush(animation_snapshot_);
        return true;
    }

    void destroy_animation_snapshot()
    {
        if (animation_snapshot_ != nullptr) {
            cairo_surface_destroy(animation_snapshot_);
            animation_snapshot_ = nullptr;
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
        if (buffer == nullptr || buffer->cairo == nullptr) {
            redraw_pending_ = true;
            return;
        }
        redraw_pending_ = false;

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
        if (animation_active() && frame_callback_ == nullptr) {
            frame_callback_ = wl_surface_frame(surface_);
            wl_callback_add_listener(frame_callback_, &frame_listener(), this);
        }
        wl_surface_attach(surface_, buffer->wl, 0, 0);
        wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
        wl_surface_commit(surface_);
        buffer->busy = true;
    }

    void draw_open(cairo_t* context)
    {
        cairo_save(context);
        cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(context);
        cairo_restore(context);

        const double offset = (visible_drawer_fraction() - 1.0) * static_cast<double>(height_);
        cairo_save(context);
        cairo_translate(context, 0.0, offset);
        if (animation_active() && animation_snapshot_ != nullptr) {
            cairo_set_source_surface(context, animation_snapshot_, 0.0, 0.0);
            cairo_paint(context);
        } else {
            draw_open_contents(context);
        }
        cairo_restore(context);
    }

    void draw_open_contents(cairo_t* context)
    {
        cairo_set_source_rgb(context, 0.965, 0.945, 0.90);
        cairo_rectangle(context, 0.0, 0.0, static_cast<double>(width_), static_cast<double>(height_));
        cairo_fill(context);
        cairo_set_source_rgb(context, 0.90, 0.31, 0.12);
        cairo_rectangle(context, 0.0, static_cast<double>(kStatusHeight - 3), static_cast<double>(width_), 3.0);
        cairo_fill(context);
        cairo_set_source_rgb(context, 0.15, 0.15, 0.13);
        draw_text(context, 24.0, 31.0, 18.0, "Quick Settings", true);

        const QuickSettingsModel model = derive_model(snapshot_);
        const QuickSettingsLayout layout = make_layout(Extent {width_, height_}, model);
        if (!layout.supported)
            return;
        const std::string wifi_detail = !wifi_feedback_.empty()
                                            ? wifi_feedback_
                                            : (snapshot_.wifi_connected ? "Connected" :
                                               (snapshot_.wifi_enabled ? "On" : "Off"));
        const std::string audio_detail = !audio_feedback_.empty()
                                             ? audio_feedback_
                                             : (snapshot_.audio_output == "external" ? "Speaker" :
                                                (snapshot_.audio_output == "internal" ? "Internal" :
                                                                                         "Unavailable"));
        const char* lora_detail = snapshot_.lora_enabled ? "On" : "Off";
        const char* fourth_title = model.fourth_primary_tile == AuxiliaryTile::Gps ? "GPS" : "On-screen Keyboard";
        const char* fourth_detail = model.fourth_primary_tile == AuxiliaryTile::Gps
                                         ? model.gps_detail.c_str() : "Show keyboard";
        draw_card(context, layout.primary_cards[0], "Wi-Fi", wifi_detail.c_str());
        draw_card(context, layout.primary_cards[1], "Audio Output", audio_detail.c_str());
        draw_card(context, layout.primary_cards[2], "LoRa", lora_detail);
        draw_card(context, layout.primary_cards[3], fourth_title, fourth_detail);
        draw_slider(context, layout.sliders[0], "Volume", snapshot_.volume_percent,
                    active_slider_ == 0);
        draw_slider(context, layout.sliders[1], "Screen Brightness",
                    snapshot_.display_brightness_percent,
                    active_slider_ == 1);
        draw_slider(context, layout.sliders[2], "Keyboard Backlight",
                    snapshot_.keyboard_backlight_percent,
                    active_slider_ == 2);
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
        for (std::size_t index = 0; index < layout.network_rows.size(); ++index) {
            if (index >= wifi_scan_.networks.size()) {
                if (index == 0U) {
                    const char* detail = !snapshot_.wifi_enabled ? "Turn Wi-Fi on to scan" :
                                         (wifi_scan_.error.empty() ? "No networks in cache" : wifi_scan_.error.c_str());
                    draw_card(context, layout.network_rows[index], "No Wi-Fi networks", detail);
                }
                continue;
            }
            const WifiNetwork& network = wifi_scan_.networks[index];
            std::string detail;
            if (network.active)
                detail = "Connected";
            else if (network.requires_password())
                detail = network.saved ? "Saved secure network" : "Secure network";
            else
                detail = "Open network";
            detail += "  " + std::to_string(network.signal_percent) + "%";
            draw_card(context, layout.network_rows[index], network.ssid.c_str(), detail.c_str());
        }
        draw_card(context, layout.network_settings, "Refresh networks", "Use NetworkManager scan");

        if (pending_confirmation_ != Confirmation::None)
            draw_confirmation(context);
        else if (wifi_password_active())
            draw_wifi_password(context);
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

    void draw_wifi_password(cairo_t* context)
    {
        if (selected_network_index_ < 0 ||
            static_cast<std::size_t>(selected_network_index_) >= wifi_scan_.networks.size())
            return;
        const WifiNetwork& network = wifi_scan_.networks[static_cast<std::size_t>(selected_network_index_)];
        const Rect dialog {312, 150, 608, 262};
        draw_rounded_rect(context, dialog, 12.0);
        cairo_set_source_rgba(context, 0.12, 0.12, 0.10, 0.97);
        cairo_fill(context);
        cairo_set_source_rgb(context, 1.0, 0.98, 0.94);
        draw_text(context, 344.0, 198.0, 22.0, "Connect to Wi-Fi", true);
        draw_text(context, 344.0, 230.0, 16.0, network.ssid.c_str());
        draw_text(context, 344.0, 258.0, 14.0, "Enter the network password with the physical keyboard.");
        const Rect password_field {344, 278, 544, 48};
        draw_rounded_rect(context, password_field, 7.0);
        cairo_set_source_rgb(context, 0.96, 0.95, 0.91);
        cairo_fill_preserve(context);
        cairo_set_source_rgb(context, 0.64, 0.60, 0.53);
        cairo_stroke(context);
        const std::string masked(wifi_passphrase_.size(), '*');
        cairo_set_source_rgb(context, 0.16, 0.16, 0.14);
        draw_text(context, 360.0, 309.0, 18.0, masked.empty() ? "Password" : masked.c_str());
        const Rect cancel {344, 346, 256, 48};
        const Rect connect {632, 346, 256, 48};
        draw_card(context, cancel, "Cancel", "Esc");
        draw_card(context, connect, "Connect", "Enter");
    }

    void refresh_state()
    {
        const ProviderReply reply = provider_.state();
        if (reply.ok) {
            snapshot_ = reply.snapshot;
            wifi_feedback_.clear();
            audio_feedback_.clear();
            message_.clear();
        } else {
            message_ = reply.error;
        }
    }

    void refresh_wifi_networks()
    {
        wifi_scan_ = {};
        if (!snapshot_.wifi_enabled)
            return;
        wifi_scan_ = scan_wifi_networks();
        if (wifi_scan_.ok)
            (void)request_wifi_rescan();
    }

    [[nodiscard]] int slider_at(int pointer_x, int pointer_y) const
    {
        if (pending_confirmation_ != Confirmation::None || wifi_password_active())
            return -1;
        const QuickSettingsModel model = derive_model(snapshot_);
        const QuickSettingsLayout layout = make_layout(Extent {width_, height_}, model);
        if (!layout.supported)
            return -1;
        for (std::size_t index = 0; index < layout.sliders.size(); ++index) {
            if (layout.sliders[index].contains(pointer_x, pointer_y))
                return static_cast<int>(index);
        }
        return -1;
    }

    void set_slider_snapshot_value(int slider, int percent)
    {
        switch (slider) {
        case 0:
            snapshot_.volume_percent = percent;
            break;
        case 1:
            snapshot_.display_brightness_percent = percent;
            break;
        case 2:
            snapshot_.keyboard_backlight_percent = percent;
            break;
        default:
            break;
        }
    }

    [[nodiscard]] static const char* slider_command_name(int slider)
    {
        switch (slider) {
        case 0:
            return "speaker-volume";
        case 1:
            return "display-brightness";
        case 2:
            return "keyboard-backlight";
        default:
            return nullptr;
        }
    }

    static void run_slider_worker(int descriptor)
    {
        for (;;) {
            pollfd poll_descriptor {descriptor, static_cast<short>(POLLIN | POLLHUP), 0};
            int ready = 0;
            do {
                ready = poll(&poll_descriptor, 1, -1);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0)
                break;

            SliderWorkerRequest newest;
            const ssize_t received = recv(descriptor, &newest, sizeof(newest), MSG_DONTWAIT);
            if (received == 0)
                break;
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                break;
            }
            if (received != static_cast<ssize_t>(sizeof(newest)))
                continue;

            // Keep only the most recent finger position while a hardware
            // request is in flight. This prevents a slow backlight or ALSA
            // command from turning a drag into a long, stale FIFO.
            for (;;) {
                SliderWorkerRequest newer;
                const ssize_t drained = recv(descriptor, &newer, sizeof(newer), MSG_DONTWAIT);
                if (drained == static_cast<ssize_t>(sizeof(newer))) {
                    newest = newer;
                    continue;
                }
                if (drained < 0 && errno == EINTR)
                    continue;
                break;
            }

            const char* command_name = slider_command_name(newest.slider);
            if (command_name != nullptr) {
                ProviderClient provider;
                (void)provider.request("SET " + std::string(command_name) + " " +
                                       std::to_string(newest.percent));
            }
        }
        close(descriptor);
        _exit(0);
    }

    void reap_slider_worker()
    {
        if (slider_worker_pid_ <= 0)
            return;
        int status = 0;
        const pid_t finished = waitpid(slider_worker_pid_, &status, WNOHANG);
        if (finished != slider_worker_pid_)
            return;
        slider_worker_pid_ = -1;
        if (slider_worker_descriptor_ >= 0) {
            close(slider_worker_descriptor_);
            slider_worker_descriptor_ = -1;
        }
    }

    [[nodiscard]] bool ensure_slider_worker()
    {
        reap_slider_worker();
        if (slider_worker_descriptor_ >= 0)
            return true;
        int descriptors[2] {-1, -1};
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors) != 0)
            return false;
        const int flags = fcntl(descriptors[0], F_GETFL, 0);
        if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) != 0) {
            close(descriptors[0]);
            close(descriptors[1]);
            return false;
        }
        const pid_t child = fork();
        if (child < 0) {
            close(descriptors[0]);
            close(descriptors[1]);
            return false;
        }
        if (child == 0) {
            close(descriptors[0]);
            run_slider_worker(descriptors[1]);
        }
        close(descriptors[1]);
        slider_worker_pid_ = child;
        slider_worker_descriptor_ = descriptors[0];
        return true;
    }

    void stop_slider_worker()
    {
        if (slider_worker_descriptor_ >= 0) {
            close(slider_worker_descriptor_);
            slider_worker_descriptor_ = -1;
        }
        reap_slider_worker();
    }

    [[nodiscard]] bool queue_slider_value(int slider, int percent)
    {
        if (!ensure_slider_worker())
            return false;
        const SliderWorkerRequest request {slider, percent};
        const ssize_t sent = send(slider_worker_descriptor_, &request, sizeof(request),
                                  MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == static_cast<ssize_t>(sizeof(request)))
            return true;
        if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
            close(slider_worker_descriptor_);
            slider_worker_descriptor_ = -1;
            reap_slider_worker();
        }
        return false;
    }

    bool begin_slider_drag(int pointer_x, int pointer_y, SliderDragOwner owner)
    {
        const int slider = slider_at(pointer_x, pointer_y);
        if (slider < 0)
            return false;
        active_slider_ = slider;
        slider_drag_owner_ = owner;
        last_slider_enqueued_[static_cast<std::size_t>(slider)] = -1;
        update_active_slider(pointer_x, true);
        return true;
    }

    void update_active_slider(int pointer_x, bool final_value)
    {
        if (active_slider_ < 0)
            return;
        const QuickSettingsModel model = derive_model(snapshot_);
        const QuickSettingsLayout layout = make_layout(Extent {width_, height_}, model);
        if (!layout.supported) {
            end_slider_drag();
            return;
        }
        const std::size_t index = static_cast<std::size_t>(active_slider_);
        const int percent = slider_percent(layout.sliders[index], pointer_x);
        set_slider_snapshot_value(active_slider_, percent);

        const auto now = std::chrono::steady_clock::now();
        if (last_slider_enqueued_[index] == percent ||
            (!final_value && now - last_slider_enqueue_ < kSliderUpdateInterval)) {
            redraw();
            return;
        }
        if (queue_slider_value(active_slider_, percent)) {
            last_slider_enqueued_[index] = percent;
            last_slider_enqueue_ = now;
        }
        redraw();
    }

    void end_slider_drag()
    {
        active_slider_ = -1;
        slider_drag_owner_ = SliderDragOwner::None;
        redraw();
    }

    void toggle_wifi()
    {
        const bool target_enabled = !snapshot_.wifi_enabled;
        if (!request_wifi_radio(target_enabled)) {
            wifi_feedback_ = "Unavailable";
            redraw();
            return;
        }
        // nmcli completed successfully. Reflect that authoritative result in
        // the card immediately instead of waiting for a later provider poll.
        // The next drawer opening refreshes the full board snapshot again.
        snapshot_.wifi_enabled = target_enabled;
        snapshot_.wifi_connected = false;
        wifi_feedback_ = target_enabled ? "On" : "Off";
        wifi_scan_ = {};
        if (target_enabled)
            (void)request_wifi_rescan();
        redraw();
    }

    void toggle_audio_output()
    {
        const std::string target = snapshot_.audio_output == "external" ? "internal" : "external";
        const ProviderReply reply = provider_.request("SET speaker-route " + target);
        if (!reply.ok) {
            audio_feedback_ = "Unavailable";
            redraw();
            return;
        }
        snapshot_ = reply.snapshot;
        // The provider reads the ALSA route control after tdvp-audio-route
        // returns. Do not claim a switch until that readback agrees.
        if (snapshot_.audio_output != target) {
            audio_feedback_ = "Unavailable";
            redraw();
            return;
        }
        audio_feedback_ = target == "external" ? "Speaker" : "Internal";
        message_.clear();
        redraw();
    }

    [[nodiscard]] bool wifi_password_active() const
    {
        return selected_network_index_ >= 0;
    }

    void cancel_wifi_password()
    {
        selected_network_index_ = -1;
        wifi_passphrase_.clear();
        message_ = "Wi-Fi connection cancelled";
        create_surface();
    }

    void submit_wifi_password()
    {
        if (selected_network_index_ < 0 ||
            static_cast<std::size_t>(selected_network_index_) >= wifi_scan_.networks.size()) {
            cancel_wifi_password();
            return;
        }
        if (wifi_passphrase_.empty()) {
            message_ = "Enter the Wi-Fi password first";
            redraw();
            return;
        }
        const WifiNetwork& network = wifi_scan_.networks[static_cast<std::size_t>(selected_network_index_)];
        const bool started = request_wifi_connect(network, wifi_passphrase_);
        const std::string ssid = network.ssid;
        selected_network_index_ = -1;
        std::fill(wifi_passphrase_.begin(), wifi_passphrase_.end(), '\0');
        wifi_passphrase_.clear();
        message_ = started ? "Connecting to " + ssid : "Could not start Wi-Fi connection";
        create_surface();
    }

    [[nodiscard]] char password_character(uint32_t key) const
    {
        const bool shifted = left_shift_ || right_shift_;
        if (key >= KEY_A && key <= KEY_Z) {
            const char base = static_cast<char>('a' + (key - KEY_A));
            return shifted ? static_cast<char>(base - 'a' + 'A') : base;
        }
        if (key >= KEY_1 && key <= KEY_9) {
            static constexpr char kShiftedDigits[] = "!@#$%^&*(";
            return shifted ? kShiftedDigits[key - KEY_1] : static_cast<char>('1' + (key - KEY_1));
        }
        if (key == KEY_0)
            return shifted ? ')' : '0';
        switch (key) {
        case KEY_SPACE: return ' ';
        case KEY_MINUS: return shifted ? '_' : '-';
        case KEY_EQUAL: return shifted ? '+' : '=';
        case KEY_LEFTBRACE: return shifted ? '{' : '[';
        case KEY_RIGHTBRACE: return shifted ? '}' : ']';
        case KEY_BACKSLASH: return shifted ? '|' : '\\';
        case KEY_SEMICOLON: return shifted ? ':' : ';';
        case KEY_APOSTROPHE: return shifted ? '\"' : '\'';
        case KEY_GRAVE: return shifted ? '~' : '`';
        case KEY_COMMA: return shifted ? '<' : ',';
        case KEY_DOT: return shifted ? '>' : '.';
        case KEY_SLASH: return shifted ? '?' : '/';
        default: return '\0';
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
        if (pointer_y < kStatusHeight)
            return;
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
        if (wifi_password_active()) {
            const Rect cancel {344, 346, 256, 48};
            const Rect connect {632, 346, 256, 48};
            if (cancel.contains(pointer_x, pointer_y))
                cancel_wifi_password();
            else if (connect.contains(pointer_x, pointer_y))
                submit_wifi_password();
            return;
        }
        const QuickSettingsModel model = derive_model(snapshot_);
        const QuickSettingsLayout layout = make_layout(Extent {width_, height_}, model);
        if (!layout.supported)
            return;
        if (layout.primary_cards[0].contains(pointer_x, pointer_y)) {
            toggle_wifi();
            return;
        } else if (layout.primary_cards[1].contains(pointer_x, pointer_y)) {
            toggle_audio_output();
            return;
        } else if (layout.primary_cards[2].contains(pointer_x, pointer_y)) {
            execute_radio_action(snapshot_.lora_enabled ? RequestedAction::DisableLora : RequestedAction::EnableLora, false);
            return;
        } else if (layout.primary_cards[3].contains(pointer_x, pointer_y)) {
            if (model.fourth_primary_tile == AuxiliaryTile::Gps) {
                execute_radio_action(snapshot_.gps_enabled ? RequestedAction::DisableGps : RequestedAction::EnableGps, false);
                return;
            }
            launch_session_process({"/usr/local/bin/vpl-osk"});
            message_ = "On-screen keyboard toggled";
        } else if (layout.sliders[0].contains(pointer_x, pointer_y)) {
            const int percent = slider_percent(layout.sliders[0], pointer_x);
            execute_provider("SET speaker-volume " + std::to_string(percent));
            return;
        } else if (layout.sliders[1].contains(pointer_x, pointer_y)) {
            execute_provider("SET display-brightness " + std::to_string(slider_percent(layout.sliders[1], pointer_x)));
            return;
        } else if (layout.sliders[2].contains(pointer_x, pointer_y)) {
            execute_provider("SET keyboard-backlight " + std::to_string(slider_percent(layout.sliders[2], pointer_x)));
            return;
        } else if (layout.secondary_actions[0].contains(pointer_x, pointer_y)) {
            execute_provider("SET speaker-mute toggle");
            return;
        } else if (layout.secondary_actions[1].contains(pointer_x, pointer_y)) {
            launch_session_process({"/usr/bin/pcmanfm", "--desktop-pref"});
        } else if (layout.system_actions[0].contains(pointer_x, pointer_y)) {
            if (access("/usr/bin/swaylock", X_OK) == 0) {
                launch_session_process({"/usr/bin/swaylock", "-f", "-c", "1f1e1b"});
                open_ = false;
                pending_confirmation_ = Confirmation::None;
                create_surface();
                return;
            }
            message_ = "Authenticated screen locking is not installed";
        } else if (layout.system_actions[1].contains(pointer_x, pointer_y)) {
            execute_provider("SYSTEM reboot");
            return;
        } else if (layout.system_actions[2].contains(pointer_x, pointer_y)) {
            execute_provider("SYSTEM poweroff");
            return;
        } else if (layout.network_toggle.contains(pointer_x, pointer_y)) {
            toggle_wifi();
            return;
        } else {
            bool network_selected = false;
            for (std::size_t index = 0; index < layout.network_rows.size(); ++index) {
                if (!layout.network_rows[index].contains(pointer_x, pointer_y))
                    continue;
                network_selected = true;
                if (index >= wifi_scan_.networks.size()) {
                    message_ = snapshot_.wifi_enabled ? "No network is available here" : "Turn Wi-Fi on first";
                    break;
                }
                const WifiNetwork& network = wifi_scan_.networks[index];
                if (network.active) {
                    message_ = "Already connected to " + network.ssid;
                } else if (network.requires_password() && !network.saved) {
                    selected_network_index_ = static_cast<int>(index);
                    std::fill(wifi_passphrase_.begin(), wifi_passphrase_.end(), '\0');
                    wifi_passphrase_.clear();
                    message_.clear();
                    create_surface();
                    return;
                } else if (request_wifi_connect(network)) {
                    message_ = "Connecting to " + network.ssid;
                } else {
                    message_ = "Could not start Wi-Fi connection";
                }
                break;
            }
            if (network_selected) {
                redraw();
                return;
            }
            if (layout.network_settings.contains(pointer_x, pointer_y)) {
                if (!snapshot_.wifi_enabled) {
                    message_ = "Turn Wi-Fi on before scanning";
                } else if (request_wifi_rescan()) {
                    refresh_wifi_networks();
                    message_ = "NetworkManager scan requested";
                } else {
                    message_ = "Could not request a Wi-Fi scan";
                }
            }
        }
        redraw();
    }

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_output* output_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_touch* touch_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_surface_ = nullptr;
    wl_callback* frame_callback_ = nullptr;
    cairo_surface_t* animation_snapshot_ = nullptr;
    std::array<Buffer, 2> buffers_ {};
    void* mapping_ = nullptr;
    std::size_t mapping_length_ = 0;
    int width_ = 0;
    int height_ = 0;
    int surface_width_ = 0;
    int surface_height_ = 0;
    int output_mode_width_ = 0;
    int output_mode_height_ = 0;
    SurfaceTransform output_transform_ = SurfaceTransform::Normal;
    SurfaceTransform buffer_transform_ = SurfaceTransform::Normal;
    int gesture_start_y_ = 0;
    int last_pointer_x_ = 0;
    int last_pointer_y_ = 0;
    bool gesture_active_ = false;
    int active_touch_id_ = -1;
    bool opened_by_touch_drag_ = false;
    bool opening_animation_pending_ = false;
    bool opening_animation_active_ = false;
    bool closing_animation_active_ = false;
    std::chrono::steady_clock::time_point animation_started_ {};
    std::chrono::steady_clock::time_point last_touch_event_ {};
    std::chrono::steady_clock::time_point last_pointer_press_ {};
    std::chrono::steady_clock::time_point last_slider_enqueue_ {};
    std::array<int, 3> last_slider_enqueued_ {{-1, -1, -1}};
    pid_t slider_worker_pid_ = -1;
    int slider_worker_descriptor_ = -1;
    SliderDragOwner slider_drag_owner_ = SliderDragOwner::None;
    int active_slider_ = -1;
    bool closed_by_touch_drag_ = false;
    bool left_shift_ = false;
    bool right_shift_ = false;
    bool redraw_pending_ = false;
    bool open_ = false;
    bool running_ = true;
    ProviderClient provider_;
    HardwareSnapshot snapshot_;
    WifiScanResult wifi_scan_;
    std::string message_;
    std::string wifi_feedback_;
    std::string audio_feedback_;
    int selected_network_index_ = -1;
    std::string wifi_passphrase_;
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
