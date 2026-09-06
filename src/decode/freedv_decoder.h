/* FreeDV 2020 receive path. */
#ifndef _FREEDV_DECODER_H
#define _FREEDV_DECODER_H

#include <glib.h>
#include <stddef.h>

/* Start the (idle) modem worker.  Call once during application start-up. */
void freedv_decoder_init(void);

/* Enable only while the active receiver is in DIGU and FreeDV 2020 is selected. */
void freedv_decoder_set_enabled(gboolean enabled);

/*
 * Feed clean, demodulated 48 kHz stereo audio and take decoded 48 kHz mono
 * speech.  Both calls are non-blocking and are intended for the RX thread.
 */
void freedv_decoder_add_audio(const gdouble *samples, int nframes);
void freedv_decoder_get_audio(gdouble *speech, int nframes);

/* Short UI text: "Searching", "Sync 3.1 dB", or an error. */
void freedv_decoder_get_status(char *out, size_t size);

#endif
