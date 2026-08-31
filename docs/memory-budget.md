# Memory budget

The target display is a 1232×568 logical Wayland output. The Quick Settings
process has two modes.

## Hidden / idle

* A transparent 1232×8 top-edge surface detects the first downward gesture.
* A small right-side status surface is retained.
* There is no timer-driven animation, background screenshot, blur, image cache,
  GTK scene graph, or polling worker thread.
* Target process PSS is at most 3 MiB and RSS at most 6 MiB.

## Open drawer

The drawer is 1232×520 and uses two 32-bit `wl_shm` buffers:

```
1232 × 520 × 4 × 2 = 5,125,120 bytes = 4.89 MiB
```

The drawer paints directly onto those buffers with Cairo. It does not allocate
an additional offscreen render target. Target PSS is at most 9 MiB and RSS at
most 12 MiB.

The image-level acceptance test must open and close the drawer thirty times,
then compare `smaps_rollup` after sixty seconds. Any persistent growth is a
failure.

