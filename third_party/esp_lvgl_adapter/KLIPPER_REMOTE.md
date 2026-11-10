# esp_lvgl_adapter: project copy

This is Espressif's `espressif/esp_lvgl_adapter` **0.6.4** (from the component
registry, the version the Waveshare BSPs pull in), with one change. It is used
instead of the registry copy through `override_path` in
[`main/idf_component.yml`](../../main/idf_component.yml).

`test_apps/`, `benchmark/` and `CHECKSUMS.json` from the registry package were
left out. Everything else is unchanged except the files listed below.

## The change: async partial rotate

Switch: `CONFIG_ESP_LVGL_ADAPTER_ASYNC_PARTIAL_ROTATE` (Kconfig, default off).
The project turns it on for the 5" in [`boards/lcd5.defaults`](../../boards/lcd5.defaults);
the 7/8/10.1" keep the upstream blocking path until tested on hardware.

The screen is a portrait panel used in landscape, so every frame is rotated by
the PPA into the panel's frame buffer, one strip at a time (`TRIPLE_PARTIAL`).
Upstream does this with a blocking PPA call: the CPU waits while each strip is
rotated, then draws the next one.

With the switch on:

- `src/display/display_manager.c`: `TRIPLE_PARTIAL` with rotation gets a second
  LVGL draw buffer (in PSRAM).
- `src/display/bridge/v9/lvgl_bridge_v9.c`:
  - Every strip except the last of a frame is rotated with a **non-blocking**
    PPA job, and the flush callback returns at once, so LVGL draws the next
    strip into its other buffer meanwhile.
  - The PPA done callback (`async_ppa_done`, ISR) calls
    `lv_display_flush_ready()` and gives a semaphore.
  - `async_flush_wait` is LVGL's flush-wait callback. It loops on
    `disp->flushing`, so a leftover give can never end a wait early, and gives
    up after 1 s with an error log instead of freezing.
  - The **last strip stays blocking**, so the frame-buffer swap and the
    triple-buffer bookkeeping after it run exactly as upstream, after every
    strip has landed.
  - `rotate_copy_strided_region()` gained an `async_user` argument. `NULL`
    behaves as upstream.
  - Counters `g_lv_async_rot_queued/done/fallback/timeout` are printed by the
    console's `bench` command.

## Measured (5" board, landscape, 240-line strips)

| Full-screen scroll | Blocking | Async |
|---|---|---|
| Settings → Updates | 22.9 fps | 24.2 fps |
| Settings → Display | 18.5 fps | 19.9 fps |

Drawing and rotating share PSRAM bandwidth with the panel's refresh, so they
slow each other down when overlapped; the gain is smaller than the time saved
on paper.

## Strip size matters (don't change `DRAW_BUF_LINES` lightly)

On IDF 5.5.6 the PPA can stop responding for some strip sizes. That is not
related to this change: blocking mode does the same.

- **120 lines** (67-row strips): hangs at boot.
- **160 lines** (90-row strips): hangs within a minute with the webcam open
  (the webcam also uses the PPA).
- **240 lines** (135 rows, what `main/main.c` uses): no hang in any test.

Espressif's `0001-bugfix-ppa-Temporary-fix-for-the-PPA-hang-issue.patch` (here)
did not fix the 120-line hang.

The console's `fbcheck` compares the panel's frame buffers with LVGL's own
render, pixel for pixel.

## Updating

To move to a newer adapter: replace this folder with the new registry
package, re-apply the `ASYNC_PARTIAL_ROTATE` blocks (search for that name), and
re-run `bench`, `fbcheck` and a webcam soak.
