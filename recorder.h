/* recorder.h -- record the active receiver's raw I/Q and demodulated audio.
 *
 * Two 16-bit stereo WAV files are written to ~/.local/share/machpsdr/:
 *   rec_<UTC>_iq.wav  -- off-air I/Q at rx->sample_rate (replayable via --faker)
 *   rec_<UTC>_af.wav  -- demodulated audio at 48 kHz
 *
 * recorder_toggle() starts/stops (GTK thread). recorder_iq()/recorder_audio()
 * are the capture taps, called from the RX audio thread; they no-op unless the
 * buffer belongs to the receiver that recording was started on. All state is a
 * single global guarded by an internal mutex, so the two threads are safe.
 */
#ifndef RECORDER_H
#define RECORDER_H

#include <glib.h>
#include "receiver.h"

/* Start recording rx (if idle) or stop the current recording (if active).
 * Returns TRUE if recording is active after the call. */
gboolean recorder_toggle(RECEIVER *rx);

/* TRUE while a recording is in progress. */
gboolean recorder_active(void);

/* Capture taps. nsamples = number of complex I/Q samples (interleaved I,Q);
 * nstereo = number of stereo audio frames (interleaved L,R). No-op unless rx is
 * the recording receiver. */
void recorder_iq(RECEIVER *rx, double *iq, int nsamples);
void recorder_audio(RECEIVER *rx, double *audio, int nstereo);

/* Build the "Recording" page for the Configure dialog (output directory + the
 * I/Q / AF stream check boxes). Defined in recorder.c. */
struct _radio;
GtkWidget *create_recording_dialog(struct _radio *radio);

#endif
