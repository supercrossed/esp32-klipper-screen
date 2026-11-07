#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "printers.h"

/*
 * Webcam viewer backend. Only one stream runs at a time; it exists only
 * while the webcam page is open (webcam_stop() frees every buffer).
 */
bool webcam_start(const printer_cfg_t *printer, const webcam_t *cam, int max_w, int max_h);
void webcam_stop(void);

/*
 * Called from the UI thread. If a newer frame is ready, returns true and
 * hands back an RGB565 buffer that stays valid until the next call that
 * returns true (or webcam_stop()).
 */
bool webcam_take_frame(const uint8_t **buf, int *w, int *h);
void webcam_status(char *out, size_t cap);   /* "640x480  12.3 fps" / error text */
