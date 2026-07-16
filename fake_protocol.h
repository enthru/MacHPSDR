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

void fake_discovery(void);
void fake_protocol_init(RADIO *r);
void fake_protocol_stop(void);

#endif
