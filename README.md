# TDVP Quick Settings

`tdvp-quick-settings` is a small native C++17 Wayland layer-shell control
center for touch-first Linux handhelds. It is intentionally an optional desktop
enhancement: it does not own the window manager, desktop, NetworkManager,
PulseAudio, or board GPIO policy.

The first supported profile is the 1232×568 TDVP K230 desktop. The client keeps
one full-output, transparent layer-shell surface from session start, but limits
its Wayland input region to a 32px strip at the true display top edge while the
drawer is closed. That gives a physical touchscreen a forgiving trigger without
letting the transparent part intercept normal desktop input. Once the pull
crosses its threshold, the drawer becomes a modal full-output input surface and
paints over the existing status panel, so panel widgets cannot appear through or receive clicks
beneath the control center. It uses `wl_shm` plus Cairo directly and does not
link GTK, Qt, WebKit, Electron, or an application shell.

The drawer is a continuous, direct-manipulation control rather than an
open/closed animation. While a finger is down, a single `revealed_px` value is
set directly from its vertical displacement; the top portion of the drawer is
clipped to exactly that height. Moving the finger back up immediately reduces
the visible height. On release, velocity chooses a decisive fling direction;
otherwise the drawer settles to the nearest stable state. A new touch cancels
that short settle at its current rendered height, so the user can grab and
reverse it midway. The only autonomous animation is this 90–220ms post-release
settle, driven by Wayland frame callbacks. To keep the K230 compositor smooth,
the complete drawer is rasterized once during session start into a fixed 2.67
MiB Cairo image. Each alternating `wl_shm` buffer records its own revealed
height, so a drag uploads only the newly exposed or cleared horizontal band;
slider motion redraws only its own card rectangle. This bounded cache removes
the first-pull allocation and rasterisation stall without adding a timer, blur
pass, screenshot cache, or resident polling worker.

## Design boundaries

* **UI repository:** gesture handling, layer-shell surface, drawing, layout,
  semantic state model, and the documented backend client protocol.
* **Firmware provider:** authoritative board capability detection, privileged
  hardware actions, radio pin ownership, NetworkManager policy, and audio
  route policy.
* **Existing desktop:** Labwc, PCManFM, and wf-panel-pi remain independent.
  Removing this package removes only the extra Quick Settings interaction.

There is deliberately no fallback-mode controller, watchdog, panel restart
loop, or special recovery desktop in this project. The established Linux
desktop components already provide those independent interaction paths; Quick
Settings only adds the top-edge touch gesture and its overlay.

Wi-Fi actions are executed synchronously through NetworkManager: a card only
changes its local `On`/`Off` detail after `nmcli` confirms the requested radio
state. The UI deliberately has no global toast area; status feedback stays in
the control that initiated it.

Audio Output is an active board-route control, not a status label. Its state
changes only after the hardware provider reads back the K230 ALSA `External
I2S Output Switch`: `Speaker` selects the external MAX98357A-compatible I2S
route and `Internal` selects the internal codec path.

The three continuous controls use Android-style touch capture rather than a
desktop click: a finger pressed on a slider stays bound to that slider until
release, movement updates its percentage and board setting in place, and it
cannot accidentally dismiss the drawer. Volume and screen brightness occupy
two broad controls; keyboard backlight has its own full-width control below
them.

Slider drawing is deliberately decoupled from board I/O. The Wayland input
callback updates and redraws the thumb immediately; while the drawer is open,
the first slider write creates one short-lived helper which coalesces queued
positions and applies only the newest position after a slow hardware request.
It is stopped when the drawer closes. This keeps a lagging I2C, ALSA, or GPIO
operation from blocking touch motion, and does not add a resident service,
thread, or polling loop.

The K230 profile has an important radio constraint: LR2021 LoRa and the
optional keyboard-mounted nRF9151 LTE/GNSS module are electrically mutually
exclusive. A GPS control is visible only after the firmware provider has
verified that the attached keyboard has nRF9151 hardware.

## Build and test on a host

The portable state/layout core has no GUI dependency and is tested on every
host:

```powershell
cmake -S . -B build -DTDVP_QS_BUILD_WAYLAND=OFF
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

For a target image or normal Linux development host with `wayland-client`,
`cairo`, and `wayland-scanner`, retain the default
`-DTDVP_QS_BUILD_WAYLAND=ON`.

## Memory contract

The UI maintains no animation loop while idle. It deliberately retains the
full-output SHM pair and one static Cairo panel while closed, so a pull gesture
never allocates a full scene or waits for a layer-shell resize under the finger.

| State | Maximum dedicated graphics allocation |
|---|---:|
| Closed transparent full-output layer: two 1232×568 32-bit wl_shm buffers | 5.34 MiB |
| Persistent static ARGB32 Cairo panel cache | 2.67 MiB |
| UI-owned persistent graphics allocation, closed or open | 8.01 MiB |

See [docs/memory-budget.md](docs/memory-budget.md) and
[docs/backend-protocol.md](docs/backend-protocol.md).
