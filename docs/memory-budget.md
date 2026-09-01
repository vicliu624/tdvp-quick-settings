# Memory budget

The target display is a 1232×568 logical Wayland output. The Quick Settings
process has two modes.

## Hidden / idle

* A transparent 1232×8 top-edge surface, intentionally above the existing
  status panel's reserved work area, detects the first downward gesture.
* No separate status surface is retained; the existing desktop top panel stays
  responsible for persistent status indicators.
* There is no timer-driven animation, background screenshot, blur, image cache,
  GTK scene graph, or polling worker thread.
* The two `wl_shm` buffers consume 78,848 bytes (0.08 MiB). Process RSS/PSS
  remains an image-level measured acceptance value rather than an unsupported
  static claim.

## Open drawer

The shipping Labwc profile reserves 64px for its existing top panel, so the
drawer is 1232×504 and uses two 32-bit `wl_shm` buffers:

```
1232 × 504 × 4 × 2 = 4,967,424 bytes = 4.74 MiB
```

The drawer paints directly onto those buffers with Cairo. It does not allocate
an additional offscreen render target. The image-level acceptance test records
RSS/PSS for the actual linked library set and rejects persistent growth.

The image-level acceptance test must open and close the drawer thirty times,
then compare `smaps_rollup` after sixty seconds. Any persistent growth is a
failure.
