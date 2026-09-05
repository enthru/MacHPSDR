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

/* Stop a recording if one is running; no-op otherwise. Drains what the writer
 * thread still has queued, so everything captured reaches the disk. GTK thread
 * only -- the writer's own disk-error path asks for it through an idle. */
void recorder_stop(void);

/* Frames written and frames DROPPED (the queue was full because the disk could
 * not keep up), for the current or most recent recording. Any pointer may be
 * NULL. Frames, not bytes: dropped frames are missing TIME, in the middle of
 * the recording rather than off the end of it. */
void recorder_stats(guint64 *iq_frames, guint64 *af_frames,
                    guint64 *iq_drop, guint64 *af_drop);

/* Stop a recording that was started on rx, closing both files with a correctly
 * patched header; no-op if rx is not the receiver being recorded. Returns TRUE
 * if a recording was stopped (the caller repaints the Record button).
 * delete_receiver must call this: the taps key on the RECEIVER pointer. */
gboolean recorder_stop_for_receiver(RECEIVER *rx);

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
