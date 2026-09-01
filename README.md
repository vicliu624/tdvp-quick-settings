# TDVP Quick Settings

`tdvp-quick-settings` is a small native C++17 Wayland layer-shell control
center for touch-first Linux handhelds. It is intentionally an optional desktop
enhancement: it does not own the window manager, desktop, NetworkManager,
PulseAudio, or board GPIO policy.

The first supported profile is the 1232×568 TDVP K230 desktop. The panel opens
from a transparent 8px surface at the true display top edge, above the
existing status panel. It is dismissed with an upward swipe begun in the
bottom edge zone, and uses `wl_shm` plus Cairo directly. On
the shipping Labwc profile, the existing 64px top panel remains visible and
the drawer receives the remaining 1232×504 logical area; the layout is
explicitly validated for both this profile and an unobstructed 1232×568
output. It does not link GTK, Qt, WebKit, Electron, or an application shell.

Opening and closing use a 180ms ease-out slide driven by Wayland frame
callbacks. The animation translates the existing drawer content inside the
same `wl_shm` buffers. To keep the K230 compositor smooth, the complete drawer
is rasterized once into a temporary 2.37 MiB Cairo image and each animation
frame only copies that image at a different vertical offset. The temporary
image is released immediately after the transition; there is no idle timer,
blur pass, screenshot cache, or persistent additional render target.

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

The three continuous controls use Android-style touch capture rather than a
desktop click: a finger pressed on a slider stays bound to that slider until
release, movement updates its percentage and board setting in place, and it
cannot accidentally dismiss the drawer. Volume and screen brightness occupy
two broad controls; keyboard backlight has its own full-width control below
them.

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

The UI maintains no animation loop while idle and does not hold the large
drawer buffers when closed.

| State | Maximum dedicated graphics allocation |
|---|---:|
| Edge trigger only, two 1232×8 32-bit wl_shm buffers | 0.08 MiB |
| 1232×504 drawer, two 32-bit wl_shm buffers | 4.74 MiB |
| UI-owned graphics allocation while open | 4.74 MiB |

See [docs/memory-budget.md](docs/memory-budget.md) and
[docs/backend-protocol.md](docs/backend-protocol.md).
