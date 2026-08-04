/* Fake SDR protocol - synthetic noise+tones IQ generator for UI/design testing.
 *
 * Provides a discovered device with no hardware behind it; a background thread
 * feeds a synthetic IQ stream into the receivers so the panadapter/waterfall
 * render something.
 */

#ifndef FAKE_PROTOCOL_H
#define FAKE_PROTOCOL_H

/* radio.h is not self-contained (it needs TRANSMITTER/ADC/DAC/WIDEBAND defined
 * first), matching the codebase convention where protocol headers do not include
 * it. Forward-declare RADIO so this header can be included standalone (e.g. from
 * discovery.c). */
typedef struct _radio RADIO;

/* Set from main() when the binary is launched with --faker. The synthetic
 * fake device is only discovered/offered when this is non-zero. */
extern int enable_fake;

/* Optional I/Q recording to loop, from `--faker <file>`. NULL falls back to
 * the MACHPSDR_FAKE_IQ env var, then the default iq.wav. */
extern const char *fake_iq_file;

void fake_discovery(void);
void fake_protocol_init(RADIO *r);
void fake_protocol_stop(void);
int fake_protocol_is_running(void);

/* Live-swap the I/Q recording the fake ("I/Q Player") device is looping while
 * it is running. `path` NULL/empty switches back to the synthetic noise+tones
 * generator. Returns 1 on success, 0 if the file could not be loaded (in which
 * case the current playback is left untouched). Safe to call from the GTK main
 * thread — the swap is serialised against the feed thread via r->delete_rx_mutex. */
int fake_protocol_set_iq_file(RADIO *r, const char *path);
// Live carrier de-rotation (Hz) for the played recording — the UI equivalent of
// MACHPSDR_FAKE_OFFSET, needed for any capture whose signal is not at DC.
void fake_protocol_set_iq_offset(RADIO *r, double hz);

/* Playback status for the panadapter overlay. Returns 1 and fills the non-NULL
 * out-params (elapsed/total seconds into the looped recording, and the file's
 * bandwidth in Hz) when an I/Q file is playing; 0 for synthetic/idle. Call from
 * the GTK thread. */
int fake_protocol_playback(double *elapsed_s, double *total_s, double *bw_hz);

/* Seek the looped I/Q recording to `fraction` (0..1) of its length. Returns 1
 * on success, 0 when no file is playing. Serialised against the feed thread via
 * r->delete_rx_mutex; call from the GTK main thread. */
int fake_protocol_seek(RADIO *r, double fraction);

#endif
