# Memory budget

The target display is a 1232×568 logical Wayland output. The Quick Settings
process has two modes.

## Hidden / idle

* One transparent 1232×568 top-edge layer-shell surface is retained so the
  first deliberate pull does not allocate buffers or wait for a layer-shell
  configure round trip. Its Wayland input region is only a 1232×32 strip while
  closed, so the transparent remainder cannot consume desktop, Foot, or normal
  status-panel clicks.
* No separate status surface is retained. When closed, the existing desktop
  top panel owns its normal status indicators; once the drawer crosses its
  opening threshold it becomes a modal full-output input surface and paints
  over that panel. This prevents a fast finger from leaving a narrowly revealed
  region and losing later touch-motion events.
* There is no idle timer, background screenshot, blur, image cache, GTK scene
  graph, or polling worker thread. A finger directly controls the drawer's
  `revealed_px` height; only the 90–220ms post-release settle uses one Wayland
  frame callback at a time.
* A slider interaction may create one short-lived helper process with a small
  Unix socketpair. It is not created while the edge trigger is idle, owns no
  graphics buffers, coalesces hardware writes to the latest finger position,
  and exits when the drawer closes. It is deliberately not a resident service
  or polling worker.
* The two 1232×568 `wl_shm` buffers consume 5,598,208 bytes (5.34 MiB). One
  fixed 1232×568 ARGB32 Cairo cache consumes another 2,799,104 bytes (2.67
  MiB), for a bounded graphics allocation of 8,397,312 bytes (8.01 MiB).
  Process RSS/PSS remains an image-level measured acceptance value rather than
  an unsupported static claim.

## Open drawer

The open control center intentionally covers the existing top panel, so the
drawer is a 1232×568 full-screen overlay with two 32-bit `wl_shm` buffers:

```
1232 × 568 × 4 × 2 = 5,598,208 bytes = 5.34 MiB
```

The drawer paints directly onto those buffers with Cairo. One 1232×568 ARGB32
Cairo image (2,799,104 bytes, 2.67 MiB) is created during session start and
retained as the static panel cache. Per-buffer reveal heights mean dragging
copies only the newly exposed or cleared horizontal band from that cache, while
a slider update restores and redraws only the relevant card rectangle. The
bounded persistent cache is intentional: it removes the prior first-pull
rasterisation stall and prevents full-frame SHM uploads during a direct drag.
The image-level acceptance test records RSS/PSS for the actual linked library
set and rejects persistent growth.

The image-level acceptance test must open and close the drawer thirty times,
then compare `smaps_rollup` after sixty seconds. Any persistent growth is a
failure.
