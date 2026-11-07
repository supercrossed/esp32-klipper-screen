#pragma once

typedef enum { SOUND_DONE = 0, SOUND_ERROR, SOUND_TEST } sound_t;

/* Plays a short chime on the built-in speaker (non-blocking). Volume comes from
 * settings (0 = muted). The codec is initialised on first use and the amplifier
 * is only powered while a sound plays. */
void sound_play(sound_t s);
