# Memory budget

The target display is a 1232×568 logical Wayland output. The Quick Settings
process has two modes.

## Hidden / idle

* A transparent 1232×32 top-edge surface gives a physical touchscreen a
  forgiving first downward-gesture hit target.
* No separate status surface is retained. When closed, the existing desktop
  top panel owns its normal status indicators; when the drawer opens, the
  full-screen Quick Settings overlay paints over it and receives all input.
* There is no idle timer, background screenshot, blur, image cache, GTK scene
  graph, or polling worker thread. The 180ms opening / 220ms closing transition
  exists only while a drawer is moving and is driven by one Wayland frame callback at a
  time.
* A slider interaction may create one short-lived helper process with a small
  Unix socketpair. It is not created while the edge trigger is idle, owns no
  graphics buffers, coalesces hardware writes to the latest finger position,
  and exits when the drawer closes. It is deliberately not a resident service
  or polling worker.
* The two `wl_shm` buffers consume 315,392 bytes (0.30 MiB). Process RSS/PSS
  remains an image-level measured acceptance value rather than an unsupported
  static claim.

## Open drawer

The open control center intentionally covers the existing top panel, so the
drawer is a 1232×568 full-screen overlay with two 32-bit `wl_shm` buffers:

```
1232 × 568 × 4 × 2 = 5,598,208 bytes = 5.34 MiB
```

The drawer paints directly onto those buffers with Cairo. It does not allocate
an additional persistent offscreen render target. During the 180ms opening or
220ms closing transition only, one 1232×568 ARGB32 Cairo image (2,799,104 bytes,
2.67 MiB) caches the fully rasterized drawer. This avoids redrawing text and
rounded paths on every frame; it is destroyed immediately when the transition
ends. The image-level acceptance test records RSS/PSS for the actual linked
library set and rejects persistent growth.

The image-level acceptance test must open and close the drawer thirty times,
then compare `smaps_rollup` after sixty seconds. Any persistent growth is a
failure.
