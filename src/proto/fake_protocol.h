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

/* Set by `--revert-iq`: swap the I and Q channels of the played recording,
 * which mirrors the spectrum — use when a recording's sideband is inverted
 * (the image is not suppressed / signals decode as their mirror). */
extern int fake_revert_iq;

void fake_discovery(void);
void fake_protocol_init(RADIO *r);
void fake_protocol_stop(void);
int fake_protocol_is_running(void);

#endif
