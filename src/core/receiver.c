/* Copyright (C)
*
* 2018 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <gtk/gtk.h>
#include "log.h"
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "serial_compat.h"
#include <wdsp.h>

#include "agc.h"
#include "mode.h"
#include "filter.h"
#include "bandstack.h"
#include "band.h"
#include "discovered.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "recorder.h"
#include "ppm_cal.h"
#include "qo100.h"
#include "main.h"
#include "vfo.h"
#include "meter.h"
#include "radio_info.h"
#include "rx_panadapter.h"
#include "tx_panadapter.h"
#include "waterfall.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#include "fake_protocol.h"
#endif
#include "audio.h"
#include "receiver_dialog.h"
#include "configure_dialog.h"
#include "property.h"
#include "rigctl.h"
#include "subrx.h"
#include "dxcluster.h"
#include "tci.h"
#ifdef FT8
#include "ft8_decoder.h"
#include "ft8_waterfall.h"
#endif
#ifdef SSTV
#include "sstv_decoder.h"
#include "wefax_decoder.h"
#include "apt_decoder.h"
#include "cw_decoder.h"
#include "cw_keyer.h"
#endif
#ifdef HFDL
#include "hfdl_decoder.h"
#include "acars_decoder.h"
#endif
// Shared "a decoder is tapping this RX's audio" machinery (unity WDSP panel
// gain + software listen-volume) is used by both the FT8/FT4 and the SSTV
// decoders, so it is compiled whenever either is enabled.
#if defined(FT8) || defined(SSTV)
#define DECODERS 1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Minimum on-screen height (px) for the panadapter (the "spectroscope") and the
// waterfall in the vertical split, so neither can open collapsed to a sliver.
// The panadapter's size-request + shrink=FALSE enforce this against the pane's
// own apportioning; restore_paned_position_cb also clamps the restored split so
// both children keep at least this much when the RX area is short.
#define MIN_PANADAPTER_HEIGHT 90
#define MIN_WATERFALL_HEIGHT  60


void receiver_xvtr_changed(RECEIVER *rx) {
}

// WDSP iobuff ring depth as a function of the receive span (sample rate).
// A deeper ring keeps wide reception (WFM at high sample rates) glitch-free but
// adds a fixed audio latency, so scale it with the span instead of using one
// blanket value: narrow spans (<=384k, which covers every HPSDR rate) keep the
// original low-latency depth of 2, wider spans step up 4/8/16.  Must be applied
// via SetDSPMult() BEFORE any OpenChannel/SetAllRates so create_iobuffs captures
// it for the (re)built ring.
int rx_ring_depth(int sample_rate) {
  if(sample_rate <= 384000)  return 2;
  if(sample_rate <= 768000)  return 4;
  if(sample_rate <= 1536000) return 8;
  return 16;
}

// Position the zoomed panadapter/waterfall pan window so the cursor (the
// freetune/ctun listening frequency, i.e. frequency_a + ctun_offset) sits in
// the middle of the visible area instead of the span centre. At zoom==1 there
// is no pan headroom, so recentring there needs the span centre itself to move
// (done on a bandwidth change); this only slides the window when zoomed in.
// For plain tuning ctun_offset==0, so this reduces to centring on the span.
static void center_pan_on_cursor(RECEIVER *rx) {
  if(rx->zoom<=1) {
    rx->pan=0;
    return;
  }
  double hz_per_pixel=(double)rx->sample_rate/(double)rx->pixels;
  int cursor_px=(rx->pixels/2)+(int)((double)(rx->ctun_frequency-rx->frequency_a)/hz_per_pixel);
  rx->pan=cursor_px-(rx->panadapter_width/2);
  if(rx->pan<0) rx->pan=0;
  if(rx->pan>(rx->pixels-rx->panadapter_width)) rx->pan=rx->pixels-rx->panadapter_width;
}

void receiver_change_sample_rate(RECEIVER *rx,int sample_rate) {
log_info("receiver_change_sample_rate: from %d to %d radio=%d\n",rx->sample_rate,sample_rate,radio->sample_rate);
  // delete_rx_mutex FIRST, then rx->mutex: the SoapySDR receive thread takes
  // them in that order (the lock around each block, then rx->mutex inside
  // full_rx_buffer), and the resampler rebuilt below is one of the things it
  // is using.  The other order is an ABBA deadlock.
  g_mutex_lock(&radio->delete_rx_mutex);
  g_mutex_lock(&rx->mutex);
  SetChannelState(rx->channel,0,1);
  g_free(rx->audio_output_buffer);
  rx->audio_output_buffer=NULL;
  rx->sample_rate=sample_rate;
  rx->output_samples=rx->buffer_size/(rx->sample_rate/48000);
  rx->audio_output_buffer=g_new0(gdouble,2*rx->output_samples);
  rx->hz_per_pixel=(double)rx->sample_rate/(double)rx->samples;
  //SetInputSamplerate(rx->channel, sample_rate);
  // Keep the wide DSP rate while in WFM (see set_mode); 48 kHz otherwise.
  rx->dsp_rate=(rx->mode_a==WFM)?rx->sample_rate:48000;
  // Re-match the iobuff ring depth to the new span before the rebuild below.
  SetDSPMult(rx_ring_depth(rx->sample_rate));
  SetAllRates(rx->channel,rx->sample_rate,rx->dsp_rate,48000);

  receiver_init_analyzer(rx);
  SetEXTANBSamplerate (rx->channel, sample_rate);
  SetEXTNOBSamplerate (rx->channel, sample_rate);
log_info("receiver_change_sample_rate: channel=%d rate=%d buffer_size=%d output_samples=%d\n",rx->channel, rx->sample_rate, rx->buffer_size, rx->output_samples);

  /* Recentre the freetune span on the current listening frequency (the cursor)
     so it stays in the middle when the bandwidth changes. Moving the span centre
     retunes the hardware LO to the cursor; frequency_changed() (called after the
     mutex is released, below) re-derives the now-zero digital shift and issues
     the retune. */
  if(rx->freetune) {
    rx->frequency_a = rx->ctun_frequency;
    rx->ctun_min = rx->frequency_a - (rx->sample_rate / 2);
    rx->ctun_max = rx->frequency_a + (rx->sample_rate / 2);
  }

#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    soapy_protocol_change_sample_rate_locked(rx,sample_rate);   // lock held above
/*
    rx->resample_step=radio->sample_rate/rx->sample_rate;
log_info("receiver_change_sample_rate: resample_step=%d\n",rx->resample_step);
*/
  }
#endif

  SetChannelState(rx->channel,1,0);
  g_mutex_unlock(&rx->mutex);
  g_mutex_unlock(&radio->delete_rx_mutex);

  /* Freetune moved the span centre onto the cursor above: retune the LO and
     re-zero the digital shift (frequency_changed), then keep the cursor centred
     in the zoomed view. */
  if(rx->freetune) {
    frequency_changed(rx);
    center_pan_on_cursor(rx);
    update_vfo(rx);
  }

  receiver_update_title(rx);
}

#ifdef DECODERS
static gboolean decoder_taps_audio(RECEIVER *rx);   // defined below
#endif

// TRUE when this RX needs the CLEANEST possible signal, so every WDSP block that
// alters the waveform for the human ear is bypassed: the noise blankers (NB/NB2),
// all four noise-reduction algorithms (NR/NR2/NR3/NR4), the auto-notch (ANF), the
// spectral noise blanker (SNB) and the manual notches (MNF, see
// receiver_notch_sync). Only what decoding actually needs is left running —
// demodulation, the passband filter and AGC. Two cases:
//   1. A DATA mode (DIGU/DIGL) — these carry digital signals fed to a modem/decoder
//      or external software (VAC/TCI); the processing only corrupts the stream and
//      is never wanted, whether or not a built-in decoder is running.
//   2. A decoder is actively tapping the demod audio (FT8/FT4/SSTV/WEFAX/CW) — this
//      also covers the CW and FM modes, where the processing is otherwise fine for
//      listening but must step aside while decoding.
// The operator's rx->nr*/anf/nb/snb/notch selections are left untouched (the VFO
// still shows them) — only the WDSP Run flags are suppressed, so everything returns
// the instant the mode leaves DIGU/DIGL and no decoder is running.
static inline gboolean bypass_stream_dsp(RECEIVER *rx) {
  if(rx->mode_a==DIGU || rx->mode_a==DIGL) return TRUE;
#ifdef DECODERS
  return decoder_taps_audio(rx);
#else
  return FALSE;
#endif
}

void update_noise(RECEIVER *rx) {
  gboolean bypass = bypass_stream_dsp(rx);
  SetEXTANBRun(rx->channel, bypass ? 0 : rx->nb);
  SetEXTNOBRun(rx->channel, bypass ? 0 : rx->nb2);
  SetRXAANRRun(rx->channel, bypass ? 0 : rx->nr);
  SetRXAEMNRRun(rx->channel, bypass ? 0 : rx->nr2);
  SetRXARNNRRun(rx->channel, bypass ? 0 : rx->nr3);
  SetRXASBNRRun(rx->channel, bypass ? 0 : rx->nr4);
  SetRXASBNRreductionAmount(rx->channel, (float)rx->nr4_reduction);
  SetRXASBNRsmoothingFactor(rx->channel, (float)rx->nr4_smoothing);
  SetRXASBNRwhiteningFactor(rx->channel, (float)rx->nr4_whitening);
  SetRXASBNRnoiseRescale(rx->channel, (float)rx->nr4_rescale);
  SetRXASBNRpostFilterThreshold(rx->channel, (float)rx->nr4_postfilter);
  SetRXAANFRun(rx->channel, bypass ? 0 : rx->anf);
  SetRXASNBARun(rx->channel, bypass ? 0 : rx->snb);
  update_vfo(rx);
}

// returns 0=off,1=NR,2=NR2,3=NR3,4=NR4
int receiver_nr_mode(RECEIVER *rx) {
  if(rx->nr)  return 1;
  if(rx->nr2) return 2;
  if(rx->nr3) return 3;
  if(rx->nr4) return 4;
  return 0;
}

// sets exactly one (or none) of the four NR flags, then applies via update_noise
void receiver_set_nr_mode(RECEIVER *rx, int mode) {
  rx->nr  = (mode==1);
  rx->nr2 = (mode==2);
  rx->nr3 = (mode==3);
  rx->nr4 = (mode==4);
  update_noise(rx);
}

// Frequency an AF notch keeps its offset from: the demodulated centre, which
// is the ctun/freetune cursor when one is in use and the dial otherwise.
gdouble receiver_notch_anchor(RECEIVER *rx) {
  if(rx->ctun || rx->freetune) return (gdouble)rx->ctun_frequency;
  return (gdouble)rx->frequency_a;
}

gboolean receiver_has_af_notch(RECEIVER *rx) {
  for(int i=0;i<rx->notches;i++) if(rx->notch[i].af) return TRUE;
  return FALSE;
}

// Move a notch between the two anchoring modes without moving the notch: going
// AF records where it currently sits relative to the demod centre, going RF
// just freezes the absolute frequency it already has.
void receiver_set_notch_af(RECEIVER *rx, int idx, gboolean af) {
  if(idx<0 || idx>=rx->notches) return;
  if(rx->notch[idx].af==af) return;
  if(af) rx->notch[idx].af_offset=rx->notch[idx].fcenter-receiver_notch_anchor(rx);
  rx->notch[idx].af=af;
  receiver_notch_sync(rx);
}

// Push the whole MacHPSDR notch list into WDSP for this rx's channel, set the
// tune frequency so notches anchor to absolute RF, and enable the notched
// bandpass only when at least one active notch exists.
void receiver_notch_sync(RECEIVER *rx) {
  int existing=0, i;
  RXANBPGetNumNotches(rx->channel, &existing);
  // clear all WDSP notches (always delete index 0 as the list collapses)
  for(i=0;i<existing;i++) RXANBPDeleteNotch(rx->channel, 0);
  int any_active=0;
  for(i=0;i<rx->notches;i++) {
    // WDSP always wants an absolute RF frequency; an AF notch derives its one
    // from the current demod centre, which is what makes it ride the dial.
    if(rx->notch[i].af) rx->notch[i].fcenter=receiver_notch_anchor(rx)+rx->notch[i].af_offset;
    RXANBPAddNotch(rx->channel, i, rx->notch[i].fcenter, rx->notch[i].fwidth,
                   rx->notch[i].active?1:0);
    if(rx->notch[i].active) any_active=1;
  }
  RXANBPSetTuneFrequency(rx->channel, (double)rx->frequency_a);
  // Manual notches also alter the stream, so hold them off in the data modes /
  // while decoding (bypass_stream_dsp) — the notch list is kept (still persisted
  // and shown), only the WDSP run flag is cleared, restored on leaving.
  RXANBPSetNotchesRun(rx->channel, (any_active && !bypass_stream_dsp(rx)) ? 1 : 0);
}

// Append a notch at absolute RF fcenter with given width; returns index or -1 if full.
int receiver_add_notch(RECEIVER *rx, gdouble fcenter, gdouble fwidth) {
  if(rx->notches>=MAX_NOTCHES) return -1;
  int idx=rx->notches;
  rx->notch[idx].fcenter=fcenter;
  rx->notch[idx].fwidth=fwidth;
  rx->notch[idx].active=TRUE;
  rx->notch[idx].af=FALSE;
  rx->notch[idx].af_offset=0.0;
  rx->notches++;
  receiver_notch_sync(rx);
  return idx;
}

void receiver_delete_notch(RECEIVER *rx, int idx) {
  int i;
  if(idx<0 || idx>=rx->notches) return;
  for(i=idx;i<rx->notches-1;i++) rx->notch[i]=rx->notch[i+1];
  rx->notches--;
  receiver_notch_sync(rx);
}

// Return index of the notch whose band contains absolute freq f_hz, else -1.
int receiver_notch_at(RECEIVER *rx, gdouble f_hz) {
  int i;
  for(i=0;i<rx->notches;i++) {
    gdouble lo=rx->notch[i].fcenter-0.5*rx->notch[i].fwidth;
    gdouble hi=rx->notch[i].fcenter+0.5*rx->notch[i].fwidth;
    if(f_hz>=lo && f_hz<=hi) return i;
  }
  return -1;
}

gboolean receiver_is_live(RECEIVER *rx) {
  if(rx==NULL || radio==NULL || radio->discovered==NULL) return FALSE;
  for(int i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]==rx) return TRUE;
  }
  return FALSE;
}

// Cancel a g_timeout/g_idle id if it is set, and clear it.  Written out here
// because every one of these has to be cancelled BEFORE the widgets and buffers
// the callback reads are gone, and a missed one is a timer firing on freed
// memory -- which is exactly the class of bug the hidden-RX invariant in
// create_receiver already exists to prevent.
//
// Both spellings of "no timer" are accepted.  The two resize timers use -1
// (0xFFFFFFFF in the guint they are declared as) throughout rx_panadapter.c and
// waterfall.c; everything else uses glib's own 0.  Removing 0xFFFFFFFF is not a
// no-op, it is a GLib-CRITICAL "Source ID 4294967295 was not found".
static void drop_source(guint *id) {
  if(*id!=0 && *id!=(guint)-1) g_source_remove(*id);
  *id=0;
}

void receiver_destroy(RECEIVER *rx) {
  if(rx==NULL) return;

  // 1. Stop everything that can still reach this receiver.  Timers and threads
  //    first: from here on nothing new runs against it.
  if(rx->update_timer_id!=0) { g_source_remove((guint)rx->update_timer_id); rx->update_timer_id=0; }
  drop_source(&rx->panadapter_resize_timer);
  drop_source(&rx->waterfall_resize_timer);
  drop_source(&rx->paned_restore_timer);
  // The CAT listeners parse against this receiver from their own threads.
  // rigctl_close(), not disable_rigctl(): the latter closes the sockets and
  // returns, leaving the server thread to notice in its own time.
  rigctl_close(rx);

  // 2. Forget the pointer everywhere it is cached outside radio->receiver[].
  //    Each of these is a file-static in another module that would otherwise
  //    dangle; the recorder is stopped by delete_receiver before we get here,
  //    because it has files to close while the receiver is still whole.
  ppm_cal_forget_receiver(rx);
  qo100_beacon_forget_receiver(rx);
  vfo_forget_receiver(rx);
  if(radio->active_receiver==rx) {
    radio->active_receiver=NULL;
    for(int i=0;i<radio->discovered->supported_receivers;i++) {
      if(radio->receiver[i]!=NULL) { radio->active_receiver=radio->receiver[i]; break; }
    }
  }
  if(radio->transmitter!=NULL) {
    if(radio->transmitter->rx==rx) radio->transmitter->rx=NULL;
#ifdef PURESIGNAL
    if(radio->transmitter->rx_puresignal_txfbk==rx) radio->transmitter->rx_puresignal_txfbk=NULL;
    if(radio->transmitter->rx_puresignal_rxfbk==rx) radio->transmitter->rx_puresignal_rxfbk=NULL;
#endif
  }

  // 3. Sub-receiver and BPSK own WDSP state of their own, keyed off this
  //    receiver's channel, so they go before the channel does.
  if(rx->subrx!=NULL) {
    rx->subrx_enable=FALSE;
    destroy_subrx(rx);
  }
  if(rx->bpsk!=NULL) {
    destroy_bpsk(rx->bpsk);
    rx->bpsk=NULL;
  }

  // 4. The local audio stream: soundio's write callback runs on its own thread
  //    with rx as its userdata, and audio_close_output() is what waits for it.
  if(rx->local_audio || rx->output_stream!=NULL) {
    audio_close_output(rx);
    rx->local_audio=FALSE;
  }

  // 5. WDSP.  All of this is keyed by the integer channel, not by rx, so it is
  //    reclaimed even though the channel number will be reused by the next
  //    receiver opened in this slot -- which is precisely why it has to be
  //    released here: OpenChannel/XCreateAnalyzer on a channel that was never
  //    closed leaks the whole DSP chain and analyzer every time a receiver is
  //    closed and re-added.  DestroyAnalyzer joins the analyzer's dispatcher
  //    thread; CloseChannel stops and tears down the RXA chain.
  DestroyAnalyzer(rx->channel);
  destroy_anbEXT(rx->channel);
  destroy_nobEXT(rx->channel);
  CloseChannel(rx->channel);

  // 6. The widget tree.  Everything visual hangs off rx->table (see
  //    create_visual), including the waterfall's GpuImage and its pixbuf, so
  //    unparenting and dropping the last reference destroys the lot.  Any
  //    dialog that carries this receiver has to go with it.
  if(rx->bookmark_dialog!=NULL) {
    gtk_window_destroy(GTK_WINDOW(rx->bookmark_dialog));
    rx->bookmark_dialog=NULL;
  }
  if(rx->window!=NULL) {
    gtk_window_destroy(GTK_WINDOW(rx->window));
    rx->window=NULL;
  }
  if(rx->table!=NULL) {
    GtkWidget *t=rx->table;
    rx->table=NULL;
    GtkWidget *parent=gtk_widget_get_parent(t);
    if(parent!=NULL) child_remove_from_parent(t);
    else g_object_unref(g_object_ref_sink(t));
  }
  rx->panadapter=NULL; rx->waterfall=NULL; rx->vpaned=NULL; rx->vfo=NULL;
  rx->meter=NULL; rx->radio_info=NULL; rx->ft8_waterfall=NULL;
  rx->wf_hpaned=NULL; rx->iq_seek=NULL;
  g_clear_object(&rx->waterfall_pixbuf);
  if(rx->panadapter_histogram_surface!=NULL) {
    cairo_surface_destroy(rx->panadapter_histogram_surface);
    rx->panadapter_histogram_surface=NULL;
  }

  // 7. Buffers.  g_clear_pointer so a double call cannot double-free.
  g_clear_pointer(&rx->iq_input_buffer,g_free);
  g_clear_pointer(&rx->diviq_input_buffer,g_free);
  g_clear_pointer(&rx->audio_output_buffer,g_free);
  g_clear_pointer(&rx->audio_buffer,g_free);
  g_clear_pointer(&rx->pixel_samples,g_free);
  g_clear_pointer(&rx->panadapter_peaks,g_free);
  g_clear_pointer(&rx->panadapter_histogram_bins,g_free);
  g_clear_pointer(&rx->scope_iq,g_free);
  g_clear_pointer(&rx->scope_fir_taps,g_free);
  g_clear_pointer(&rx->scope_fir_hist,g_free);
  g_clear_pointer(&rx->scope_tuned_ext,g_free);
  g_clear_pointer(&rx->scope_tuned_out,g_free);
  g_clear_pointer(&rx->resampled_buffer,g_free);
  // rx->buffer and rx->resampler are SoapySDR's: allocated in
  // soapy_protocol_create_receiver(), sized from the stream MTU.  Only its own
  // idempotent re-entry guard ever freed them, at the TOP of that function, and
  // no teardown reaches it -- so closing a SoapySDR receiver leaked 32 kB of
  // buffer per cycle, which is what LeakSanitizer measured against
  // tools/soapy_null.cpp.  The resampler exists only when the device's ADC rate
  // differs from the receiver's (the null driver's does not, so that half was
  // invisible to the same run).  Freed here rather than in a Soapy-specific
  // teardown because this is where every other buffer of the receiver goes, and
  // both fields are NULL on the protocols that never set them.
  g_clear_pointer(&rx->buffer,g_free);
  if(rx->resampler!=NULL) {
    destroy_resample(rx->resampler);
    rx->resampler=NULL;
  }
  g_clear_pointer(&rx->audio_name,g_free);

  g_mutex_clear(&rx->mutex);
  g_mutex_clear(&rx->scope_mutex);
  g_mutex_clear(&rx->local_audio_mutex);

  log_info("receiver_destroy: channel=%d released\n",rx->channel);
  g_free(rx);
}

void receiver_close(RECEIVER *rx) {
  // Keep at least one receiver — never leave the radio headless
  if(radio->receivers<=1) return;

  configure_dialog_close(radio);
  // Persist this receiver's current settings so they survive the close, then
  // mark the slot inactive: the settings are kept but the receiver is not
  // auto-recreated on the next start-up (and are restored if it is re-added).
  {
    char name[80];
    receiver_save_state(rx);
    sprintf(name,"receiver[%d].active",rx->channel);
    setProperty(name,"0");
  }
  // delete_receiver frees rx (via receiver_destroy), including its update timer,
  // its bookmark dialog and its whole widget tree -- all of which used to be torn
  // down piecemeal here.  It also hands active_receiver on to a survivor, so
  // nothing below may touch rx again.
  delete_receiver(rx);
  // Rebuild the stack: re-lays out the surviving panels (with GtkPaned dividers).
  radio_rebuild_rx_stack(radio);
}

#ifdef FT8
// TRUE when a panadapter gesture should place the FT8 TX offset rather than
// tune the RX: Shift held while the active receiver is in DIGU.
static gboolean ft8_tx_offset_gesture(RECEIVER *rx, guint state) {
  return rx->mode_a==DIGU && (state & GDK_SHIFT_MASK);
}

// Set the FT8 TX audio offset from a panadapter x-coordinate, clamped to the
// FT8 passband.  Purely sets dial+offset inside the *static* display window — it
// never tunes/pans the RX, so the operator can drop TX on a clear frequency.
static void ft8_set_tx_offset_from_x(RECEIVER *rx, double ex) {
  long long half=(long long)rx->sample_rate/2LL;
  long long min_display=(rx->frequency_a - half)+(long long)((double)rx->pan*rx->hz_per_pixel);
  double clicked=(double)min_display + ex*rx->hz_per_pixel;
  int off=(int)(clicked - (double)rx->frequency_a);
  if(off<200) off=200;
  if(off>2800) off=2800;
  radio->ft8_tx_offset=off;
  if(rx->panadapter!=NULL) gtk_widget_queue_draw(rx->panadapter);
}
#endif

// Absolute RF Hz under panadapter x-pixel `x`, folding in pan/zoom. The one
// canonical inverse of the freq->x mapping the panadapter draws with; used by
// the Ctrl+click notch hit-test and the DX-cluster spot hit-test so a click
// always lands on the same RF the marker is drawn at.
static long long receiver_x_to_freq(RECEIVER *rx, double x) {
  long long half=(long long)rx->sample_rate/2LL;
  long long min_display=(rx->frequency_a - half)+(long long)((double)rx->pan*rx->hz_per_pixel);
  return (long long)((double)min_display + x*rx->hz_per_pixel);
}

// GTK4: GtkGestureClick "pressed" handler (button/state from the gesture).
#ifdef __APPLE__
// TRUE when a panadapter/waterfall click is really a leaked click from the
// Configure dialog's native title bar: on macOS the gdkmacos backend delivers
// the press to the main window even though the Configure dialog is the active
// (key) toplevel. A genuine spectrum click activates the main window first, so
// the dialog is no longer active by the time the gesture fires.
static gboolean receiver_click_leaked_from_dialog(void) {
  return radio!=NULL && radio->dialog!=NULL &&
         gtk_window_is_active(GTK_WINDOW(radio->dialog));
}
#endif

void receiver_pressed_cb(GtkGestureClick *gesture, int n_press, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
#ifdef __APPLE__
  // macOS: the gdkmacos backend delivers pointer clicks to the main window even
  // when it is not the active (key) toplevel, so a click on the Configure
  // dialog's native title bar (e.g. to drag/move the window) leaks through to the
  // panadapter/waterfall gesture underneath and retunes the RX. Drop the click
  // while the Configure dialog is itself the active window — that is exactly the
  // leak condition, and it never fires for a genuine spectrum click (which makes
  // the main window active and the dialog inactive first).
  if(receiver_click_leaked_from_dialog()) return;
#endif
  radio->active_receiver=rx;
  guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  switch(button) {
    case 1: // left button
      // Remember that THIS widget owns the drag now: receiver_motion_cb only
      // tunes/pans while this is set, so a button held down elsewhere (another
      // process' drag, e.g. a screenshot area selection) can't be mistaken for
      // a drag on the spectrum.  See RECEIVER.pointer_pressed.
      rx->pointer_pressed=TRUE;
      // Ctrl+click manages manual notch filters: click empty area to drop a
      // notch of the configured default width at the clicked RF; click on an
      // existing notch to remove it. (Ctrl+scroll resizes one, see below.)
      if(state & GDK_CONTROL_MASK) {
        double clicked=(double)receiver_x_to_freq(rx,ex);
        int hit=receiver_notch_at(rx,clicked);
        if(hit>=0) receiver_delete_notch(rx,hit);
        else       receiver_add_notch(rx,clicked,rx->notch_default_width);
        if(rx->panadapter!=NULL) gtk_widget_queue_draw(rx->panadapter);
        rx->has_moved=FALSE;
        rx->is_panning=FALSE;
        return;
      }
#ifdef FT8
      // Shift+click in DIGU sets the FT8 TX audio offset (dial+offset) without
      // tuning the dial.  Mirrors WSJT-X double-click-to-set-Tx.  The matching
      // guards in the release/motion handlers keep the RX from retuning.
      if(ft8_tx_offset_gesture(rx,state)) {
        ft8_set_tx_offset_from_x(rx,ex);
        rx->last_x=(int)ex;
        rx->has_moved=FALSE;
        rx->is_panning=FALSE;
        return;
      }
#endif
      // Plain left-click tunes on *release* (receiver_released_cb), which is
      // also where the DX-cluster spot snap lives — doing it here as well would
      // double-tune. Just stash the press position for the drag/pan test.
      //if(!rx->locked) {
        rx->last_x=(int)ex;
        rx->press_x=(int)ex;
        rx->has_moved=FALSE;
        if(rx->zoom>1 && ey>=rx->panadapter_height-20) {
          rx->is_panning=TRUE;
        }
      //}
      break;
    case 3: // right button
      if(radio->dialog==NULL) {
        // Open Configure focused on this RX's page (by name — page merges no
        // longer shift the integer index this used to compute from rx_base).
        char page[16];
        snprintf(page,sizeof(page),"RX-%d",rx->channel);
        configure_dialog_open(radio,page);
        update_receiver_dialog(rx);
      }
      break;
  }
}

/* VFO B carries its own band, LO and LO error, exactly as VFO A does — the TX
   frequency computation reads them (transmitter_get_frequency(), and protocol2's
   own copy of the same sum).  Nothing used to maintain them: lo_b was seeded from
   lo_a when the receiver was created and afterwards only the XVTR dialog ever
   wrote it, and then only if VFO B happened to sit in the edited band at that
   moment.  A cross-band split — the normal way to work a satellite transponder,
   where receive goes through one converter (a QO-100 LNB, LO 9750 MHz) and
   transmit through a completely different one (a 2.4 GHz transverter) — was
   therefore impossible: VFO B kept VFO A's LO and the radio was commanded to a
   nonsense IF.  Deriving them from frequency_b is what set_band() already does
   for VFO A, so the two VFOs now behave the same way.

   Called from both frequency_changed() (VFO A moves, and the SAT/RSAT modes that
   drag B along) and update_frequency() (every receiver_move_b() branch ends in
   one), which between them cover every path that can move VFO B. */
void receiver_sync_vfo_b_lo(RECEIVER *rx) {
  if(rx==NULL) return;
  int b=get_band_from_frequency(rx->frequency_b);
  BAND *band=band_get_band(b);
  if(band==NULL) return;
  rx->band_b=b;
  rx->lo_b=band->frequencyLO;
  rx->error_b=band->errorLO;
}

void update_frequency(RECEIVER *rx) {
  if(!rx->locked) {
    receiver_sync_vfo_b_lo(rx);
    if(rx->vfo!=NULL) {
      update_vfo(rx);
    }
    if(radio->transmitter!=NULL) {
      if(radio->transmitter->rx==rx) {
        update_tx_panadapter(radio);
      }
    }
  }
}

/* Upper tuning limit: a hard RECEIVER_FREQ_CEILING_HZ (20 GHz) ceiling — a
   sanity cap that keeps a runaway edit from producing a garbage frequency, set
   above the microwave bands an operator can reach through a converter rather
   than at any device's own top end — lowered to the discovered device's own
   maximum when that is narrower (e.g. ~61 MHz for the classic HPSDR radios, or
   6 GHz for a HackRF).

   ...and RAISED to cover any configured transverter. With a transverter the dial
   shows the frequency at the antenna while the hardware only ever sees
   frequency - lo, so a transverter band legitimately sits far above the device's
   own ceiling: a HackRF stops at 6 GHz and QO-100's downlink is at 10.49 GHz.
   Without this the dial simply cannot be tuned there — and because the guard is
   directional (it rejects only moves pushing FURTHER out of range), a frequency
   restored from a saved bandstack could be tuned downwards but never back up,
   which is a worse failure than being refused outright.

   The transverter's ceiling gets a MARGIN on top, because a transverter's
   min/max are a band plan, not the range of the converter — an LNB does not stop
   working one hertz above the frequency the operator typed in. Stopping exactly
   on the declared edge is what an operator actually runs into: with ctun or
   freetune the cursor is what the guard clamps, so a signal at the top of the
   band can be seen at the right-hand edge of the display but never brought to
   the middle of it, since the centre only follows the cursor and the cursor is
   already against the stop. The margin is the receiver's own span (floored at
   1 MHz), which is exactly the amount needed to centre either edge of the band —
   and this is a sanity cap against a runaway edit, not a safety interlock, so
   being generous with it costs nothing. */
long long receiver_max_frequency(RECEIVER *rx) {
  long long cap = RECEIVER_FREQ_CEILING_HZ;
  if(radio != NULL && radio->discovered != NULL) {
    long long dev = (long long)radio->discovered->frequency_max;
    if(dev > 0 && dev < cap) cap = dev;
  }
  long long xvtr_top = 0;
  for(int b=BANDS;b<BANDS+XVTRS;b++) {
    BAND *xb=band_get_band(b);
    if(xb!=NULL && strlen(xb->title)>0 && xb->frequencyMax>xvtr_top) xvtr_top=xb->frequencyMax;
  }
  if(xvtr_top > 0) {
    long long margin = 1000000LL;
    if(rx != NULL && (long long)rx->sample_rate > margin) margin = (long long)rx->sample_rate;
    if(xvtr_top + margin > cap) cap = xvtr_top + margin;
  }
  return cap;
}

long long receiver_move_a(RECEIVER *rx, long long hz, gboolean round) {
  long long delta = 0LL;
  if(!rx->locked) {
    /* freetune forces ctun-like behaviour but clamps to span */
    gboolean use_ctun = rx->ctun || rx->freetune;
    long long fmax = receiver_max_frequency(rx);

    /* Keep the frequency inside [0, fmax]. The value that actually moves is
       ctun_frequency (+hz) in ctun/freetune, and frequency_a (-hz) otherwise —
       guard whichever one is about to change so a ctun move can't silently push
       the tune frequency out of range (and so a normal move isn't wrongly
       blocked by the ctun offset). Reject only moves that push *further* out of
       range: if the current value is already outside [0, fmax] (e.g. a saved
       config or xvtr frequency above the device max), a move back toward the
       valid range must still be allowed, or tuning gets stuck forever. */
    long long cur = use_ctun ? rx->ctun_frequency : rx->frequency_a;
    long long nf  = use_ctun ? (cur + hz) : (cur - hz);
    if((nf < 0    && nf < cur) ||
       (nf > fmax && nf > cur)) return 0;

    if(use_ctun) {
      delta = rx->ctun_frequency;
      rx->ctun_frequency = rx->ctun_frequency + hz;

      if(rx->freetune) {
        long long span_half = (long long)(rx->sample_rate / 2);
        long long span_min  = rx->frequency_a - span_half;
        long long span_max  = rx->frequency_a + span_half;

        long long threshold = span_half * 15 / 100;

        if(rx->ctun_frequency > span_max - threshold) {
          long long shift = rx->ctun_frequency - (span_max - threshold);
          rx->frequency_a += shift;
          rx->ctun_min = rx->frequency_a - span_half;
          rx->ctun_max = rx->frequency_a + span_half;
        } else if(rx->ctun_frequency < span_min + threshold) {
          long long shift = (span_min + threshold) - rx->ctun_frequency;
          rx->frequency_a -= shift;
          rx->ctun_min = rx->frequency_a - span_half;
          rx->ctun_max = rx->frequency_a + span_half;
        } else {
          if(rx->ctun_frequency < span_min) rx->ctun_frequency = span_min;
          if(rx->ctun_frequency > span_max) rx->ctun_frequency = span_max;
        }
      }

      if(round && (rx->mode_a != CWL && rx->mode_a != CWU)) {
        rx->ctun_frequency = (rx->ctun_frequency / rx->step) * rx->step;
      }
      delta = rx->ctun_frequency - delta;
    } else {
      delta = rx->frequency_a;
      rx->frequency_a = rx->frequency_a - hz;
      if(round && (rx->mode_a != CWL && rx->mode_a != CWU)) {
        rx->frequency_a = (rx->frequency_a / rx->step) * rx->step;
      }
      delta = rx->frequency_a - delta;
    }
  }
  return delta;
}

void receiver_move_b(RECEIVER *rx,long long hz,gboolean b_only,gboolean round) {
  if(!rx->locked) {
    // Keep VFO B inside [0, fmax] (6 GHz / device max). As with VFO A, reject
    // only moves that push further out of range so a frequency already above the
    // device max can still be tuned back down.
    long long fmax = receiver_max_frequency(rx);
    long long nf = rx->frequency_b + hz;
    if ((nf <= 0   && nf < rx->frequency_b) ||
        (nf > fmax && nf > rx->frequency_b)) return;
    long long f=rx->frequency_b;
    switch(rx->split) {
      case SPLIT_OFF:
        if(round) {
          rx->frequency_b=(rx->frequency_b+hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b+hz;
        }
        update_frequency(rx);
        break;
      case SPLIT_ON:
        if(round) {
          rx->frequency_b=(rx->frequency_b+hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b+hz;
        }
        update_frequency(rx);
        break;
      case SPLIT_SAT:
        if(round) {
          rx->frequency_b=(rx->frequency_b+hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b+hz;
        }
	if(rx->subrx!=NULL) {
  	  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
          rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);
          if(rx->frequency_b<rx->ctun_min || rx->frequency_b>rx->ctun_max) {
            rx->frequency_b=f;
          }
	}
        // vfo_linked: the LINK toggle suspends the tie so the operator can set
        // the TX/RX pair up in the first place.  Four sites move the other VFO
        // (here, the RSAT branch below, receiver_move and receiver_move_to);
        // all four ask this one field.
        if(!b_only && rx->vfo_linked) {
          receiver_move_a(rx,hz,round);
          frequency_changed(rx);
        }
        update_frequency(rx);
        break;
      case SPLIT_RSAT:
        if(round) {
          rx->frequency_b=(rx->frequency_b-hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b-hz;
        }
	if(rx->subrx!=NULL) {
  	  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
          rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);
          if(rx->frequency_b<rx->ctun_min || rx->frequency_b>rx->ctun_max) {
            rx->frequency_b=f;
          }
	}
        if(!b_only && rx->vfo_linked) {
          receiver_move_a(rx,-hz,round);
          frequency_changed(rx);
          update_frequency(rx);
        }
        break;
    }

    if(rx->subrx_enable) {
      // dont allow frequency outside of passband
    }
  }
}

void receiver_move(RECEIVER *rx,long long hz,gboolean round) {
  if(!rx->locked) {
    long long delta=receiver_move_a(rx,hz,round);
    switch(rx->split) {
      case SPLIT_OFF:
        break;
      case SPLIT_ON:
        break;
      case SPLIT_SAT:
      case SPLIT_RSAT:
        if(rx->vfo_linked) receiver_move_b(rx,delta,TRUE,round);
        break;
    }

    frequency_changed(rx);
    update_frequency(rx);
  }
}

void receiver_move_to(RECEIVER *rx,long long hz) {
  long long delta;
  long long start=(long long)rx->frequency_a-(long long)(rx->sample_rate/2);
  long long offset=hz;
  long long f;

  if(!rx->locked) {
    offset=hz;

    f=start+offset+(long long)((double)rx->pan*rx->hz_per_pixel);
    f=f/rx->step*rx->step;

    double cw_offset = 0;
    if(rx->mode_a==CWL || rx->mode_a==CWU) {
      if(rx->mode_a==CWU) {
        cw_offset=-radio->cw_keyer_sidetone_frequency;
      } else {
        cw_offset=+radio->cw_keyer_sidetone_frequency;
      }
    }

    if(rx->ctun || rx->freetune) {
      delta=rx->ctun_frequency;
      rx->ctun_frequency=f + cw_offset;

      /* freetune: clamp to visible span, never move the waterfall centre */
      if(rx->freetune) {
        long long span_min = rx->frequency_a - (long long)(rx->sample_rate / 2);
        long long span_max = rx->frequency_a + (long long)(rx->sample_rate / 2);
        if(rx->ctun_frequency < span_min) rx->ctun_frequency = span_min;
        if(rx->ctun_frequency > span_max) rx->ctun_frequency = span_max;
      }

      delta=rx->ctun_frequency-delta;
    } else {
      if((rx->split==SPLIT_ON) &&
         (rx->mode_a==CWL || rx->mode_a==CWU ||
          rx->mode_a==USB || rx->mode_a==LSB)) {
        rx->frequency_b=f + cw_offset;
      } else {
        delta=rx->frequency_a;
        rx->frequency_a=f + cw_offset;
        delta=rx->frequency_a-delta;
      }
    }

    switch(rx->split) {
      case SPLIT_OFF:
        break;
      case SPLIT_ON:
        break;
      case SPLIT_SAT:
        if(rx->vfo_linked) receiver_move_b(rx,delta,TRUE,TRUE);
        break;
      case SPLIT_RSAT:
        // NOTE the sign disagrees with receiver_move above, which passes +delta
        // for RSAT and lets receiver_move_b's own branch do the inverting.  Left
        // alone deliberately: it is untestable without an inverting transponder
        // and is not what this change is about.  See .claude/notes/backlog.md.
        if(rx->vfo_linked) receiver_move_b(rx,-delta,TRUE,TRUE);
        break;
    }

    frequency_changed(rx);
    update_frequency(rx);
  }
}

/*
 * receiver_set_freetune:
 *   Enable or disable freetune mode.
 *   When enabled the waterfall/panadapter centre frequency is frozen;
 *   only ctun_frequency (the VFO cursor) moves within the visible span.
 *   When disabled the radio returns to normal tuning behaviour.
 */

void receiver_set_freetune(RECEIVER *rx, gboolean enable) {
  rx->freetune = enable;
  log_info("receiver_set_freetune: channel=%d enable=%d\n", rx->channel, enable);

  if(enable) {
    if(!rx->ctun) {
      rx->ctun_frequency = rx->frequency_a;
    }
    rx->ctun_min = rx->frequency_a - (long long)(rx->sample_rate / 2);
    rx->ctun_max = rx->frequency_a + (long long)(rx->sample_rate / 2);
    if(rx->ctun_frequency < rx->ctun_min) rx->ctun_frequency = rx->ctun_min;
    if(rx->ctun_frequency > rx->ctun_max) rx->ctun_frequency = rx->ctun_max;
    /* The hardware LO is already on the current centre; remember it so we only
       retune when the span centre later moves (see frequency_changed). */
    rx->freetune_hw_frequency = rx->frequency_a;
    SetRXAShiftRun(rx->channel, 1);
  } else {
    if(!rx->ctun) {
      /* Leaving freetune: keep listening to whatever the cursor was on by
         folding the freetune offset into the real VFO, recentre the display
         (ctun_offset -> 0 so the cursor returns to the middle) and drop the
         WDSP shift. The retune to the new centre happens in frequency_changed. */
      rx->frequency_a = rx->ctun_frequency;
      rx->ctun_offset = 0;
      SetRXAShiftFreq(rx->channel, 0.0);
      SetRXAShiftRun(rx->channel, 0);
    }
  }

  /* Только если визуальная часть уже создана */
  if(rx->vfo != NULL) {
    frequency_changed(rx);
    update_frequency(rx);
  }
}

// Layout-independent test for the physical "Q" key. GTK reports event->keyval
// after the active keyboard layout, so on a Russian layout Cmd-Q arrives as the
// Cyrillic "й" and a plain keyval==GDK_KEY_q test misses it. Instead we look up
// every keyval the pressed hardware keycode produces across all layout groups
// (there is always a Latin group where the physical Q key is q) so Cmd-Q quits
// on any keyboard layout.
// GTK4: GdkKeymap is gone — map a hardware keycode to keyvals via the display.
static gboolean key_is_q(guint keyval, guint keycode) {
  if(keyval==GDK_KEY_q || keyval==GDK_KEY_Q) return TRUE;
  GdkDisplay *display=gdk_display_get_default();
  if(!display || keycode==0) return FALSE;
  GdkKeymapKey *keys=NULL;
  guint *keyvals=NULL;
  int n=0;
  gboolean found=FALSE;
  if(gdk_display_map_keycode(display,keycode,&keys,&keyvals,&n)) {
    for(int i=0;i<n;i++) {
      if(keyvals[i]==GDK_KEY_q || keyvals[i]==GDK_KEY_Q) { found=TRUE; break; }
    }
  }
  g_free(keys);
  g_free(keyvals);
  return found;
}

// GTK4: GtkEventControllerKey "key-pressed" handler (returns TRUE if handled).
gboolean receiver_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
  log_debug("Pressed: %s\n", gdk_keyval_name(keyval));
  // Cmd-Q (macOS) / Ctrl-Q: clean shutdown. On the quartz backend the Command
  // key may show up as either GDK_META_MASK or GDK_MOD2_MASK, so accept both.
  // key_is_q() matches the physical Q key on any keyboard layout (e.g. Cmd-й on
  // a Russian layout).
  if(key_is_q(keyval,keycode) &&
     (state & (GDK_META_MASK|GDK_ALT_MASK|GDK_CONTROL_MASK))) {
    main_delete(NULL);
    return TRUE;
  }
  // SDR#-style digit entry: a number key while hovering a VFO digit overwrites
  // that digit in place. Only consumes the key when the cursor is over a digit.
  if(vfo_type_digit(keyval)) {
    return TRUE;
  }
  switch(keyval) {
    case GDK_KEY_space:
        set_mox(radio,TRUE);
      return TRUE;
#ifdef SSTV
    // Software iambic keyer paddles (Phase 4.4b): `[`/`]` are unused and
    // adjacent, and only act in CWL/CWU so they stay free for everything else.
    case GDK_KEY_bracketleft:
      if(radio->active_receiver!=NULL &&
         (radio->active_receiver->mode_a==CWL || radio->active_receiver->mode_a==CWU)) {
        cw_keyer_paddle(CW_PADDLE_DOT,TRUE);
        return TRUE;
      }
      break;
    case GDK_KEY_bracketright:
      if(radio->active_receiver!=NULL &&
         (radio->active_receiver->mode_a==CWL || radio->active_receiver->mode_a==CWU)) {
        cw_keyer_paddle(CW_PADDLE_DASH,TRUE);
        return TRUE;
      }
      break;
#endif
  }
  return FALSE;
}

// GTK4: GtkEventControllerKey "key-released" handler (void).
void receiver_key_released(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
  log_debug("Released: %s\n", gdk_keyval_name(keyval));
  switch(keyval) {
    case GDK_KEY_space:
      set_mox(radio,FALSE);
      break;
#ifdef SSTV
    // Always release on key-up (no mode gate): if the mode changed away from
    // CWL/CWU while the key was held, this is the only chance to clear the
    // paddle -- gating it the same as the press would leave it stuck down.
    // A release with nothing pressed is a harmless no-op in cw_keyer.c.
    case GDK_KEY_bracketleft:
      cw_keyer_paddle(CW_PADDLE_DOT,FALSE);
      break;
    case GDK_KEY_bracketright:
      cw_keyer_paddle(CW_PADDLE_DASH,FALSE);
      break;
#endif
  }
}

// GTK4: GtkGestureClick "released" handler.
void receiver_released_cb(GtkGestureClick *gesture, int n_press, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
#ifdef __APPLE__
  // See receiver_pressed_cb: drop clicks that leak from the Configure dialog's
  // native title bar while that dialog is the active window.
  if(receiver_click_leaked_from_dialog()) return;
#endif
  guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  int x=(int)ex;
  int moved=x-rx->last_x;
  if(button==1) {
      rx->pointer_pressed=FALSE;
#ifdef FT8
      // FT8 TX-offset gesture (Shift+click/drag in DIGU): update the offset from
      // the release point and return — never tune/pan the RX.
      if(ft8_tx_offset_gesture(rx,state)) {
        ft8_set_tx_offset_from_x(rx,(double)x);
        rx->last_x=x;
        rx->has_moved=FALSE;
        return;
      }
#endif
      if(rx->is_panning) {
        int pan=rx->pan+(moved*rx->zoom);
        if(pan<0) {
          pan=0;
        } else if(pan>(rx->pixels-rx->panadapter_width)) {
          pan=rx->pixels-rx->panadapter_width;
        }
        rx->pan=pan;
        rx->last_x=x;
        rx->has_moved=FALSE;
        rx->is_panning=FALSE;
      } else if(!rx->locked) {
        if(rx->has_moved) {
          // drag
          /* In freetune mode, dragging only moves the VFO cursor,
             not the waterfall centre */
          receiver_move(rx,(long long)((double)(moved*rx->hz_per_pixel)),TRUE);
        } else {
          // Plain click. If a DX-cluster spot marker sits within ~6px of the
          // click, tune exactly onto its frequency (round=FALSE, no step snap)
          // via receiver_move() so the move is committed and any SAT/RSAT split
          // tracks; otherwise fall back to the normal move-to-clicked-frequency.
          long long spot_f;
          long long tol=(long long)(6.0*rx->hz_per_pixel);
          if(radio->cluster_enable && radio->cluster_spots_show &&
             dxcluster_nearest(receiver_x_to_freq(rx,(double)x),tol,&spot_f)) {
            long long hz=(rx->ctun || rx->freetune) ? (spot_f - rx->ctun_frequency)
                                                    : (rx->frequency_a - spot_f);
            receiver_move(rx,hz,FALSE);
          } else {
            // move to this frequency
            receiver_move_to(rx,(long long)((float)x*rx->hz_per_pixel));
          }
        }
        rx->last_x=x;
        rx->has_moved=FALSE;
    }
  }
}

// GTK4: GtkEventControllerMotion "motion" handler (state incl. button masks
// comes from the controller; cursor coords are stashed for the scroll handler).
void receiver_motion_cb(GtkEventControllerMotion *controller, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
#ifdef __APPLE__
  // See receiver_pressed_cb: the gdkmacos backend leaks the button-1 drag motion
  // from the Configure dialog's title bar to the panadapter underneath, which
  // re-tuned the waterfall while the dialog was being dragged around. Drop the
  // motion while that dialog is the active window — same leak condition. (The
  // rx->pointer_pressed gate below now catches this case as well; this stays as
  // the fail-safe, and it also keeps the stashed cursor position honest.)
  if(receiver_click_leaked_from_dialog()) return;
#endif
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  int x=(int)ex;
  int y=(int)ey;
  rx->cursor_x=x;
  rx->cursor_y=y;
  int moved=x-rx->last_x;
  // A drag is button 1 down *in a press we received ourselves*.  Testing the
  // event's GDK_BUTTON1_MASK alone is wrong on macOS: gdk fills that mask from
  // [NSEvent pressedMouseButtons], i.e. system-wide button state, so while any
  // other process holds the button down — a Cmd-Shift-4 screenshot area
  // selection being the everyday case — merely sweeping the pointer across the
  // panadapter/waterfall was read as a drag and retuned the receiver.
  // Self-healing: a motion with no button down clears a press whose release we
  // never saw (grab broken, released off-window), so neither a stranded drag nor
  // a stranded pan can hijack the next pointer sweep.
  gboolean button1=(state & GDK_BUTTON1_MASK)==GDK_BUTTON1_MASK;
  if(!button1) { rx->pointer_pressed=FALSE; rx->is_panning=FALSE; }
  gboolean dragging=button1 && rx->pointer_pressed;
#ifdef FT8
  // Shift+drag in DIGU slides the FT8 TX offset live, without tuning the RX.
  if(ft8_tx_offset_gesture(rx,state) && dragging) {
    ft8_set_tx_offset_from_x(rx,(double)x);
    rx->last_x=x;
    return;
  }
#endif
  if(rx->is_panning && dragging) {
    int pan=rx->pan+(moved*rx->zoom);
    if(pan<0) {
      pan=0;
    } else if(pan>(rx->pixels-rx->panadapter_width)) {
      pan=rx->pixels-rx->panadapter_width;
    }
    rx->pan=pan;
    rx->last_x=x;
  } else if(!rx->locked) {
    if(dragging) {
      //receiver_move(rx,(long long)((double)(moved*rx->hz_per_pixel)),FALSE);
      receiver_move(rx,(long long)((double)(moved*rx->hz_per_pixel)),TRUE);
      rx->last_x=x;
      // Decide drag-vs-click from the cumulative distance since the press, not
      // this event's delta: on a slow drag the per-event delta stays within
      // ±1px and would never trip this, so release would wrongly re-centre.
      if (x-rx->press_x > 2 || x-rx->press_x < -2) {
        rx->has_moved=TRUE;
      }
    } else {
      if(x>4 && x<35) {
        gtk_widget_set_cursor_from_name(widget,"ew-resize");
      } else {
        gtk_widget_set_cursor_from_name(widget,"crosshair");
      }
    }
  }
}

// GTK4: GtkEventControllerScroll "scroll" handler. The signal carries no pointer
// position, so use the coords the motion handler stashed; dy<0 == scroll up.
// Accumulated surface-delta (macOS trackpad scrollingDeltaY ~= points) that
// makes one tuning notch.  ~16 points is a bit more than one classic "scroll
// line", so a trackpad notch feels close to one mouse-wheel detent instead of
// the dozens of raw precise events a single swipe fires.  Raise for a coarser
// (less sensitive) trackpad, lower for finer.
#define SCROLL_SURFACE_STEP 16.0
// Max tuning notches a single trackpad event may emit.  Without this cap a fast
// flick crosses the threshold many times in one event and the callers multiply
// the tuning step by |n|, so the dial "accelerates" and overshoots the target
// frequency.  Capping to 1 gives a steady one-notch-per-event feel (raise for a
// little flick-acceleration back).
#define SCROLL_SURFACE_MAX_NOTCH 1

int scroll_notches(GtkEventControllerScroll *controller, double dy) {
  if(dy==0.0) return 0;
  // Mechanical mouse wheel arrives as a discrete event (unit WHEEL, dy=+-1 per
  // detent): keep the classic one-notch-per-click feel untouched.
  if(gtk_event_controller_scroll_get_unit(controller)==GDK_SCROLL_UNIT_WHEEL) {
    return dy<0.0 ? -1 : 1;
  }
  // Trackpad / Magic Mouse: a precise (SURFACE) stream of many small deltas per
  // swipe.  Accumulate per-controller and only emit a notch each time the sum
  // crosses SCROLL_SURFACE_STEP, so a smooth swipe no longer over-scrolls.
  gdouble *acc=g_object_get_data((GObject *)controller,"scroll_acc");
  if(acc==NULL) {
    acc=g_new0(gdouble,1);
    g_object_set_data_full((GObject *)controller,"scroll_acc",acc,g_free);
  }
  // Reversing direction responds immediately rather than first unwinding the
  // leftover accumulation from the previous direction.
  if(((*acc)>0.0 && dy<0.0) || ((*acc)<0.0 && dy>0.0)) *acc=0.0;
  *acc+=dy;
  int notches=(int)((*acc)/SCROLL_SURFACE_STEP);
  if(notches!=0) {
    // Keep only the sub-threshold remainder: discard any whole notches beyond
    // the cap instead of banking them, so a fast flick neither accelerates the
    // dial nor leaves a backlog that lags the next scroll.
    *acc=fmod(*acc,SCROLL_SURFACE_STEP);
    if(notches>SCROLL_SURFACE_MAX_NOTCH) notches=SCROLL_SURFACE_MAX_NOTCH;
    else if(notches<-SCROLL_SURFACE_MAX_NOTCH) notches=-SCROLL_SURFACE_MAX_NOTCH;
  }
  return notches;
}

gboolean receiver_scroll_cb(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
#ifdef __APPLE__
  // See receiver_pressed_cb: the gdkmacos backend also leaks scroll events from
  // the Configure dialog to the panadapter/waterfall underneath, so two-finger
  // dragging the dialog around scrolls (pans/tunes) the waterfall. Drop the
  // scroll while that dialog is the active window — same leak condition.
  if(receiver_click_leaked_from_dialog()) return TRUE;
#endif
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  int x=rx->cursor_x;
  int y=rx->cursor_y;
  int half=rx->panadapter_height/2;
  int n=scroll_notches(controller,dy);
  if(n==0) return TRUE;   // trackpad delta below the notch threshold
  gboolean up=n<0;
  int mag=n<0?-n:n;       // notches this event (>1 on a fast flick)

  // Ctrl+scroll over a manual notch resizes it (Ctrl+click having placed it),
  // so its width can be dialled in against the interference on the spectrum
  // itself instead of via the Configure dialog. Only fires with the pointer on
  // a notch, so plain Ctrl+scroll elsewhere still tunes as before.
  {
    GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    if(state & GDK_CONTROL_MASK) {
      int hit=receiver_notch_at(rx,(double)receiver_x_to_freq(rx,x));
      if(hit>=0) {
        gdouble w=rx->notch[hit].fwidth*pow(up?1.1:1.0/1.1,(double)mag);
        if(w<NOTCH_MIN_WIDTH) w=NOTCH_MIN_WIDTH;
        if(w>NOTCH_MAX_WIDTH) w=NOTCH_MAX_WIDTH;
        rx->notch[hit].fwidth=w;
        receiver_notch_sync(rx);
        if(rx->panadapter!=NULL) gtk_widget_queue_draw(rx->panadapter);
        return TRUE;
      }
    }
  }

  if(rx->zoom>1 && y>=rx->panadapter_height-20) {
    int pan;
    if(up) {
      pan=rx->pan+rx->zoom*mag;
    } else {
      pan=rx->pan-rx->zoom*mag;
    }

    if(pan<0) {
      pan=0;
    } else if(pan>(rx->pixels-rx->panadapter_width)) {
      pan=rx->pixels-rx->panadapter_width;
    }
    rx->pan=pan;
  } else if(!rx->locked) {
    // The dB-scale strip only responds while the scale is under manual control;
    // with Panadapter Automatic on, the next frame would undo the change anyway.
    if((x>4 && x<35) && (widget==rx->panadapter) && rx->panadapter_automatic) {
      /* nothing: the automatic scale owns high/low */
    } else if((x>4 && x<35) && (widget==rx->panadapter)) {
      if(up) {
        if(y<half) {
          rx->panadapter_high=rx->panadapter_high-5*mag;
        } else {
          rx->panadapter_low=rx->panadapter_low-5*mag;
        }
      } else {
        if(y<half) {
          rx->panadapter_high=rx->panadapter_high+5*mag;
        } else {
          rx->panadapter_low=rx->panadapter_low+5*mag;
        }
      }
    } else if(up) {
      if(rx->ctun || rx->freetune) {
        receiver_move(rx,rx->step*mag,TRUE);
      } else {
        receiver_move(rx,-rx->step*mag,TRUE);
      }
    } else {
      if(rx->ctun || rx->freetune) {
        receiver_move(rx,-rx->step*mag,TRUE);
      } else {
        receiver_move(rx,+rx->step*mag,TRUE);
      }
    }
  }
  return TRUE;
}

static gboolean update_timer_cb(void *data) {
  int rc=0;
  int rc2;
  RECEIVER *rx=(RECEIVER *)data;
  gboolean running=FALSE;
  gboolean have_pixels=FALSE;   // panadapter/waterfall data fetched this tick?
  gboolean draw_display;        // not TX (or duplex) -> update pan/wf/meter
  gboolean tx_needs_update=FALSE;

  // -------------------------------------------------------------------------
  // Phase 1: read the WDSP display data under rx->mutex. This runs on the GTK
  // main thread (g_timeout at rx->fps). The mutex also guards the RX/audio
  // thread's fexchange0()/Spectrum0() in full_rx_buffer(), so we must hold it
  // for as SHORT a time as possible: only the WDSP reads, never the cairo
  // rendering. Holding it across the (pixel-area-scaled) panadapter draw used
  // to block the RX thread and stutter the audio on a large window.
  // -------------------------------------------------------------------------
  g_mutex_lock(&rx->mutex);
  switch(radio->discovered->protocol) {
    case PROTOCOL_1:
      running=protocol1_is_running();
      break;
    case PROTOCOL_2:
      running=protocol2_is_running();
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      running=soapy_protocol_is_running();
      break;
#endif
    case PROTOCOL_FAKE:
      running=fake_protocol_is_running();
      break;
  }
  draw_display = (!isTransmitting(radio) || (rx->duplex));
  if(draw_display) {
    if(rx->panadapter_resize_timer==-1 && rx->pixel_samples!=NULL) {
      GetPixels(rx->channel,0,rx->pixel_samples,&rc);

      if (radio->divmixer[rx->dmix_id] != NULL) {
        if (radio->divmixer[rx->dmix_id]->calibrate_gain) {
          GetPixels(radio->divmixer[rx->dmix_id]->rx_hidden->channel, 0,
                    radio->divmixer[rx->dmix_id]->rx_hidden->pixel_samples, &rc2);
        }
      }
      have_pixels=TRUE;
    }
    rx->meter_db=GetRXAMeter(rx->channel,rx->smeter) + radio->meter_calibration;
    // Cache the AGC hang/threshold here (under the lock) so the unlocked
    // panadapter render below never calls WDSP while the RX thread runs.
    if(rx->agc!=AGC_OFF) {
      GetRXAAGCHangLevel(rx->channel, &rx->agc_hang_level);
      GetRXAAGCThresh(rx->channel, &rx->agc_thresh_level, 4096.0, (double)rx->sample_rate);
    }
  }

#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    if(radio->transmitter!=NULL && radio->discovered->info.soapy.has_temp) {
      radio->transmitter->updated=FALSE;
    }
  }
#endif
  tx_needs_update = (radio->transmitter!=NULL && !radio->transmitter->updated);
  g_mutex_unlock(&rx->mutex);

  // -------------------------------------------------------------------------
  // Phase 2: render WITHOUT the lock. rx->pixel_samples is only written above
  // (under the lock) and only from this main-thread timer, so reading it here
  // is safe, and the RX/audio thread is never blocked waiting on the draw.
  // -------------------------------------------------------------------------
  if(draw_display) {
    if(have_pixels) {
      if(rc) {
        update_rx_panadapter(rx,running);
        update_waterfall(rx);
      } else if(!running) {
        update_rx_panadapter(rx,running);
      }
    }
    update_meter(rx);
  }
  update_radio_info(rx);

  if(tx_needs_update) {
    update_tx_panadapter(radio);
  }

  // I/Q Player scrub bar: show it only while a file is playing and follow the
  // playback position. set_value emits "value-changed" (unconnected), NOT
  // "change-value", so this never triggers a seek. Skipped during the post-drag
  // guard window so the timer doesn't yank the thumb out from under the user.
  if(rx->iq_seek!=NULL) {
    double el, tot, bw;
    if(fake_protocol_playback(&el, &tot, &bw) && tot>0.0) {
      if(!gtk_widget_get_visible(rx->iq_seek)) gtk_widget_set_visible(rx->iq_seek, TRUE);
      if(g_get_monotonic_time() > rx->iq_seek_guard_us)
        gtk_range_set_value(GTK_RANGE(rx->iq_seek), el/tot);
    } else if(gtk_widget_get_visible(rx->iq_seek)) {
      gtk_widget_set_visible(rx->iq_seek, FALSE);
    }
  }

  return TRUE;
}

static void set_mode(RECEIVER *rx,int m) {
  rx->mode_a=m;
  // Wideband (broadcast) FM must be demodulated at the full sample rate so the
  // ~180 kHz signal fits; the stereo pilot (19/38 kHz) and RDS (57 kHz)
  // subcarriers also need this wide baseband.  All other modes run the DSP at
  // 48 kHz.  The audio output stays at 48 kHz either way.
  //
  // Drive the DSP rate off the TARGET mode, not the transition: when WFM is
  // restored from props at startup there is no other-mode->WFM transition, yet
  // the channel is opened at 48 kHz (see OpenChannel) and must still be raised.
  // rx->dsp_rate tracks the channel's real DSP rate so we only rebuild when it
  // actually changes.
  int desired_dsp = (m==WFM) ? rx->sample_rate : 48000;
  if(rx->dsp_rate != desired_dsp) {
    SetChannelState(rx->channel,0,1);
    // WFM runs the DSP at the full span, so match the ring depth to that span
    // before rebuilding the iobuffs (create_iobuffs captures it).
    SetDSPMult(rx_ring_depth(rx->sample_rate));
    SetAllRates(rx->channel, rx->sample_rate, desired_dsp, 48000);
    SetChannelState(rx->channel,1,0);
    rx->dsp_rate = desired_dsp;
  }
  SetRXAMode(rx->channel, m);
}

void set_filter(RECEIVER *rx,int low,int high) {
//fprintf(stderr,"set_filter: %d %d\n",low,high);
  if(rx->mode_a==CWL) {
    rx->filter_low_a=-radio->cw_keyer_sidetone_frequency-low;
    rx->filter_high_a=-radio->cw_keyer_sidetone_frequency+high;
  } else if(rx->mode_a==CWU) {
    rx->filter_low_a=radio->cw_keyer_sidetone_frequency-low;
    rx->filter_high_a=radio->cw_keyer_sidetone_frequency+high;
  } else {
    rx->filter_low_a=low;
    rx->filter_high_a=high;
  }
  RXASetPassband(rx->channel,(double)rx->filter_low_a,(double)rx->filter_high_a);
  if(radio->transmitter!=NULL && radio->transmitter->rx==rx && radio->transmitter->use_rx_filter) {
    transmitter_set_filter(radio->transmitter,rx->filter_low_a,rx->filter_high_a);
    update_tx_panadapter(radio);
  }
}

void set_deviation(RECEIVER *rx) {
log_info("set_deviation: %d\n",rx->deviation);
  SetRXAFMDeviation(rx->channel, (double)rx->deviation);
  set_squelch(rx);
}

// Mode-aware "variable squelch". FM modes (FMN/WFM) use WDSP's FM squelch
// (keys off the demodulated noise level); every other mode uses the amplitude
// (voice) squelch AMSQ, which gates on the pre-AGC signal magnitude. Exactly
// one of the two WDSP squelches runs at a time; the other is forced off so a
// mode change can't leave a stale squelch armed on the wrong path.
//
// The single 0..1 SQL bar drives both: bar at minimum = squelch fully OFF
// (audio always passes) for every mode -- see the FM note below for why the
// bar-at-minimum-is-off rule matters.
void set_squelch(RECEIVER *rx) {
  if(rx->channel < 0) return;
  int mode=rx->mode_a;
  gboolean is_fm=(mode==FMN || mode==WFM);

  // Bar fully down = squelch OFF. For FM: fm_sq = 10^(-2*squelch) is at most 1.0
  // and the old "fm_sq > 1" disable test could never fire -- the FM squelch
  // stayed armed even with the bar fully down (fm_sq=1.0 leaves unmute_thresh=0.9
  // in SetRXAFMSQThreshold still gating audio), so on a weak/fake signal avnoise
  // never dropped below it and the channel sat muted with no way to fully open
  // it ("no sound with squelch off"). Treating the bar at minimum as OFF fixes
  // that; the same convention now applies to AMSQ for the other modes.
  rx->squelch_enable=(rx->squelch > 0.0);

  // Every path that moves the bar (VFO drag/scroll, CAT, actions) ends here, so
  // this is the one place that has to remember the setting for the current mode
  // — receiver_mode_changed restores it from the same array.
  if(mode>=0 && mode<MODES) rx->mode_squelch[mode]=rx->squelch;

  // Squelch gates the audio stream, so it must step aside while a decoder is
  // running (or in the data modes) exactly like NR/NB/ANF/notch -- otherwise a
  // closing squelch feeds the decoder SILENCE. For SSTV that silence decodes to
  // solid GREEN (a Y=Cr=Cb=0 sample maps to RGB(0,135,0) in the BT.601 full-range
  // conversion, since chroma neutral is 128 not 0), i.e. green patches wherever
  // the squelch chattered. The bar value/enable is kept (still shown/persisted);
  // only the WDSP Run flag is suppressed, and returns the moment decoding stops.
  gboolean run = rx->squelch_enable && !bypass_stream_dsp(rx);

  if(is_fm) {
    double fm_sq=pow(10.0, -2.0*rx->squelch);
    SetRXAFMSQThreshold(rx->channel, fm_sq);
    SetRXAAMSQRun(rx->channel, 0);
    SetRXAFMSQRun(rx->channel, run);
    log_info("Set FM squelch %f %f\n", rx->squelch, fm_sq);
  } else {
    // Voice/amplitude squelch. AMSQ's unmute threshold is pow(10, thresh_db/20)
    // on the pre-AGC signal magnitude, so map the 0..1 bar linearly in dB:
    // higher bar = tighter gate. The endpoints are operator-settable (Configure
    // -> RX-N) because the useful range depends on the station's noise floor
    // and can only be found against a live band, not here.
    double thresh_db=rx->amsq_min_db + (rx->amsq_max_db-rx->amsq_min_db)*rx->squelch;
    SetRXAAMSQThreshold(rx->channel, thresh_db);
    SetRXAAMSQMaxTail(rx->channel, rx->amsq_tail);
    SetRXAFMSQRun(rx->channel, 0);
    SetRXAAMSQRun(rx->channel, run);
    log_info("Set AM/voice squelch %f %f dB\n", rx->squelch, thresh_db);
  }
}

// APF -- CW audio peak filter (WDSP "speak"/SPCW). Peaks a narrow band at the
// CW sidetone (beat-note) frequency to pull weak CW out of the noise. Runs only
// in CWL/CWU so an enabled APF can't ring on SSB/AM audio; leaving CW disables
// it automatically (this is called from receiver_mode_changed). Guarded on a
// live WDSP channel like set_agc/set_squelch.
void set_apf(RECEIVER *rx) {
  if(rx->channel < 0) return;
  int mode=rx->mode_a;
  gboolean cw=(mode==CWL || mode==CWU);
  double freq=(double)radio->cw_keyer_sidetone_frequency;
  if(freq < 200.0) freq=200.0; // SPCW design 1 clamps below 200 Hz anyway
  SetRXASPCWFreq(rx->channel, freq);
  SetRXASPCWBandwidth(rx->channel, rx->apf_bw);
  SetRXASPCWGain(rx->channel, rx->apf_gain);
  SetRXASPCWRun(rx->channel, (rx->apf_enable && cw) ? 1 : 0);
  // Mirror onto the sub-receiver's channel (it gates on the SUB's own mode), so
  // every caller of set_apf — dialog, mode change, MIDI — reaches both.
  if(rx->subrx_enable && rx->subrx!=NULL) subrx_set_apf(rx);
}

void calculate_display_average(RECEIVER *rx) {
  double display_avb;
  int display_average;

  double t=0.001 * rx->display_average_time;

  display_avb = exp(-1.0 / ((double)rx->fps * t));
  display_average = max(2, (int)fmin(60, (double)rx->fps * t));
  SetDisplayAvBackmult(rx->channel, 0, display_avb);
  SetDisplayNumAverage(rx->channel, 0, display_average);
}

void receiver_fps_changed(RECEIVER *rx) {
  g_source_remove(rx->update_timer_id);
  int poll_ms=1000/(3*rx->fps); if(poll_ms<8) poll_ms=8;   // oversample vs frame production
  rx->update_timer_id=g_timeout_add(poll_ms,update_timer_cb,(gpointer)rx);
  // The waterfall scroll rate is the WDSP frame-production rate
  // (sample_rate/(fft_size-overlap)), and `overlap` is derived from rx->fps
  // ONLY inside receiver_init_analyzer(). Re-run it here so production actually
  // tracks the new fps - otherwise the slider changed only the poll interval,
  // which is visible only when it falls below the (frozen) production rate at
  // low fps. Guard with rx->mutex like the resize path (it frees/reallocs the
  // pixel buffers the audio thread reads).
  g_mutex_lock(&rx->mutex);
  receiver_init_analyzer(rx);
  g_mutex_unlock(&rx->mutex);
  calculate_display_average(rx);
}

void receiver_filter_changed(RECEIVER *rx,int filter) {
//fprintf(stderr,"receiver_filter_changed: %d\n",filter);
  // Same reasoning as receiver_mode_changed: filter_a subscripts filters[mode][]
  // and band_filters[].
  if(filter<0 || filter>=FILTERS) {
    log_error("receiver_filter_changed: filter %d out of range, ignored\n",filter);
    return;
  }
  rx->filter_a=filter;
  // Keep this mode's remembered filter in sync so the selection is restored
  // when the operator returns to the mode later.
  if(rx->mode_a>=0 && rx->mode_a<MODES) rx->mode_filter[rx->mode_a]=filter;
  if(rx->mode_a==FMN) {
    if(filter == 0) {
      rx->deviation = 2500;
    }
    else {
      rx->deviation = 5000;
    }
    switch(rx->deviation) {
      case 2500:
        set_filter(rx,-4000,4000);
        break;
      case 5000:
        set_filter(rx,-8000,8000);
        break;
    }
    set_deviation(rx);
  } else {
    FILTER *mode_filters=filters[rx->mode_a];
    FILTER *f=&mode_filters[rx->filter_a];
    set_filter(rx,f->low,f->high);
  }
  update_vfo(rx);
}

#ifdef FT8
// Add/remove the dedicated FT8 band waterfall to the RIGHT of this receiver's RF
// spectrum (~1/3 width) while it is in DIGU *and* the FT8 panel is open.  The
// spectrum lives in a GtkBox (rx->wf_hpaned).  GTK thread only.
// GTK4 note: the box no longer re-proportions the 1/3 split live on window
// resize (the "size-allocate" signal is gone); the ft8 waterfall is sized to
// 1/3 when opened and rescales its own content to whatever width it gets.
void receiver_ft8_waterfall_sync(RECEIVER *rx) {
  if(rx==NULL || rx->wf_hpaned==NULL) return;
  gboolean want = (rx->mode_a==DIGU) && radio!=NULL && radio->ft8_panel_open;
  gboolean have = (rx->ft8_waterfall!=NULL);
  if(want==have) return;
  if(want) {
    rx->ft8_waterfall=ft8_waterfall_create();
    int w=gtk_widget_get_width(rx->wf_hpaned);
    if(w>1) gtk_widget_set_size_request(rx->ft8_waterfall,w/3,-1);
    gtk_widget_set_hexpand(rx->ft8_waterfall,FALSE);
    gtk_box_append(GTK_BOX(rx->wf_hpaned),rx->ft8_waterfall);
  } else {
    // Removing the last ref disposes it, emitting "destroy" (stops its timer).
    gtk_box_remove(GTK_BOX(rx->wf_hpaned),rx->ft8_waterfall);
    rx->ft8_waterfall=NULL;
  }
}
#endif

void receiver_mode_changed(RECEIVER *rx,int mode) {
  // Refuse an out-of-range mode HERE rather than at each call site: it ends up
  // in rx->mode_a, which is a bare subscript into filters[MODES] (whose entry is
  // then dereferenced), into mode_string[MODES] and into the per-mode arrays.
  // A CAT client can send one -- `ZZMD99;` reaches this directly -- and so can a
  // hand-edited props file.
  if(mode<0 || mode>=MODES) {
    log_error("receiver_mode_changed: mode %d out of range, ignored\n",mode);
    return;
  }
  // Remember the filter selected for the mode we are leaving, then restore the
  // one this mode used last time, so each mode keeps its own bandwidth
  // independently (changing the AM filter must not move the SSB filter).
  if(rx->mode_a>=0 && rx->mode_a<MODES) {
    rx->mode_filter[rx->mode_a]=rx->filter_a;
    rx->mode_agc[rx->mode_a]=rx->agc;
    rx->mode_squelch[rx->mode_a]=rx->squelch;
  }
  set_mode(rx,mode);
  if(mode>=0 && mode<MODES) {
    rx->filter_a=rx->mode_filter[mode];
    // Restore this mode's remembered AGC speed and push it to WDSP.
    rx->agc=rx->mode_agc[mode];
    // ...and its squelch setting, so opening the gate wide on AM does not
    // leave FM wide open on the next mode change. set_squelch below pushes it.
    rx->squelch=rx->mode_squelch[mode];
    if(rx->channel>=0) set_agc(rx);
  }
  log_info("mode_changed: %d\n",mode);
  // Re-apply the mode-aware squelch (FMSQ for FM, AMSQ otherwise) and the CW
  // APF, which both key off the new mode. set_squelch/set_apf pick the right
  // WDSP block and force the others off, so switching modes can't strand a
  // squelch on the wrong path or leave the CW peaking filter ringing on SSB.
  if(rx->channel>=0) {
    set_squelch(rx);
    set_apf(rx);
  }
  receiver_filter_changed(rx,rx->filter_a);
#ifdef DECODERS
  // Re-apply the WDSP panel gain: entering DIGU/DIGL switches the channel to
  // unity (so the decoder taps full level regardless of volume/mute), leaving it
  // restores the listen gain. rx_panel_gain() depends on the just-set mode.
  // Guard against early calls before the WDSP channel exists (create_receiver
  // sets the gain itself).
  if(rx->channel>=0) receiver_set_volume(rx);
#endif
  // Re-evaluate stream-DSP bypass: a mode change can take this RX into or out of a
  // data mode (DIGU/DIGL) or a decoder-tapped mode (bypass_stream_dsp), so re-push
  // the NB/NR/ANF/SNB run flags (update_noise) and the manual-notch run flag
  // (receiver_notch_sync) — held off there, restored otherwise. Not gated on
  // DECODERS: the DIGU/DIGL rule applies regardless of decoder builds.
  if(rx->channel>=0) {
    update_noise(rx);
    receiver_notch_sync(rx);
  }
#ifdef FT8
  // Show/hide the embedded FT8 QSO panel (and gate a second receiver) when the
  // active receiver enters/leaves DIGU.  GTK-thread context here, unlike the
  // audio-thread decoder tap in process_rx_buffer().
  if(radio!=NULL && rx==radio->active_receiver) radio_ft8_panel_sync(radio);
  receiver_ft8_waterfall_sync(rx);
#endif
#ifdef SSTV
  // Same for the SSTV image, WEFAX image and CW text panels: an active-RX mode
  // change out of their mode must tear the panel down (and free the RX2 slot /
  // re-enable Add Receiver), or it strands open with no control to dismiss it.
  if(radio!=NULL && rx==radio->active_receiver) {
    radio_sstv_panel_sync(radio);
    radio_wefax_panel_sync(radio);
    radio_cw_panel_sync(radio);
    radio_apt_panel_sync(radio);
  }
#endif
#ifdef HFDL
  if(radio!=NULL && rx==radio->active_receiver) {
    radio_hfdl_panel_sync(radio);
    radio_acars_panel_sync(radio);
  }
#endif
  // TCI (Phase A): mirror the mode change to connected clients (no-op unless
  // the server runs and rx is the active receiver; see tci.c).
  tci_notify_mode(rx);
}

void receiver_band_changed(RECEIVER *rx,int band) {
#ifdef OLD_BAND
  BANDSTACK *bandstack=bandstack_get_bandstack(band);
  if(rx->band_a==band) {
    // same band selected - step to the next band stack
    rx->bandstack++;
    if(rx->bandstack>=bandstack->entries) {
      rx->bandstack=0;
    }
  } else {
    // new band - get band stack entry
    rx->band_a=band;
    rx->bandstack=bandstack->current_entry;
  }

  BANDSTACK_ENTRY *entry=&bandstack->entry[rx->bandstack];
  rx->frequency_a=entry->frequency;
  receiver_mode_changed(rx,entry->mode);
  receiver_filter_changed(rx,entry->filter);
#else
  rx->band_a=band;
  receiver_mode_changed(rx,rx->mode_a);
  receiver_filter_changed(rx,rx->filter_a);
#endif

  /* When band changes, freetune span must be re-anchored */
  if(rx->freetune) {
    rx->ctun_frequency = rx->frequency_a;
    rx->ctun_min = rx->frequency_a - (long long)(rx->sample_rate / 2);
    rx->ctun_max = rx->frequency_a + (long long)(rx->sample_rate / 2);
  }

  frequency_changed(rx);
}

#ifdef DECODERS
// TRUE when a decoder (FT8/FT4/SSTV) is selected and this receiver is in a
// digital mode (DIGU/DIGL) — i.e. its demodulated audio is being tapped by a
// decoder rather than (only) listened to. The decoder is off by default, so
// plain DIGU/DIGL listening is unaffected; the operator opts in from the
// bottom-bar Decode block (radio->decode_mode). Used to force unity WDSP panel
// gain (so the decoder always sees full level) and to apply the listen
// volume/mute in software instead — see rx_panel_gain / process_rx_buffer.
static gboolean decoder_taps_audio(RECEIVER *rx) {
  if(radio==NULL || radio->decode_mode==DECODE_OFF) return FALSE;
  // SSTV also decodes narrowband FM (VHF, e.g. the ISS on 145.800); FT8/FT4 are
  // SSB-only (DIGU/DIGL).
  if(radio->decode_mode==DECODE_SSTV)
    return rx->mode_a==DIGU || rx->mode_a==DIGL || rx->mode_a==FMN;
  // CW is only valid in the CW modes.
  if(radio->decode_mode==DECODE_CW)
    return rx->mode_a==CWL || rx->mode_a==CWU;
  // WEFAX/FT8/FT4 are HF SSB-only (DIGU/DIGL).
  return rx->mode_a==DIGU || rx->mode_a==DIGL;
}
#endif

// WDSP audio-panel gain for a receiver's channel. Normally this is the listen
// gain (volume, or 0 when muted). When a decoder is active on this RX we run the
// channel at unity instead: the decoder taps rx->audio_output_buffer, which WDSP
// scales by this gain, and the operator does not listen to the decoded signal —
// so it must decode regardless of the volume slider or mute. The listen
// volume/mute is applied to the audible output in software in
// process_rx_buffer() for that case. See receiver_set_volume().
static gdouble rx_panel_gain(RECEIVER *rx) {
#ifdef DECODERS
  if(decoder_taps_audio(rx)) return 1.0;
#endif
  return rx->mute ? 0.0 : rx->volume;
}

static void process_rx_buffer(RECEIVER *rx) {
  gdouble left_sample,right_sample;
  short left_audio_sample, right_audio_sample;
  SUBRX *subrx=(SUBRX *)rx->subrx;

  for (int i=0;i<rx->output_samples;i++) {
    // Sub-RX: main on the left channel, sub on the right, with an adjustable
    // crossfeed (rx->subrx_mix 0..100). mix=0 keeps the hard L/R split; mix=100
    // (m=0.5) collapses both to an equal mono blend audible in both ears.
    if(rx->subrx_enable) {
      gdouble main_sample=rx->audio_output_buffer[i*2];
      gdouble sub_sample=subrx->audio_output_buffer[i*2];
      gdouble m=rx->subrx_mix*0.005; // 0 .. 0.5
      left_sample = main_sample*(1.0-m) + sub_sample*m;
      right_sample = sub_sample*(1.0-m) + main_sample*m;
    } else {
      // Rx option for left channel only, right only, or both channels
      switch (rx->audio_channels) {
        case AUDIO_STEREO: {
          left_sample = rx->audio_output_buffer[i*2];
          right_sample = rx->audio_output_buffer[(i*2)+1];
          break;
        }
        case AUDIO_LEFT_ONLY: {
          left_sample = rx->audio_output_buffer[i*2];
          right_sample = 0;
          break;
        }
        case AUDIO_RIGHT_ONLY: {
          left_sample = 0;
          right_sample = rx->audio_output_buffer[(i*2)+1];
          break;
        }
      }
    }
#ifdef DECODERS
    // When a decoder is tapping this RX the WDSP channel runs at unity (see
    // rx_panel_gain) so the decoder always sees a full-level signal; apply the
    // listen volume/mute to the audible output here instead, keeping the speaker
    // behaviour unchanged.
    if(decoder_taps_audio(rx)) {
      gdouble lg = rx->mute ? 0.0 : rx->volume;
      left_sample  *= lg;
      right_sample *= lg;
    }
#endif
    // Clamp to full scale before the 16-bit conversion below.  FM has no audio
    // AGC and WDSP's NBFM de-emphasis boosts the low end hard (a 300 Hz tone at
    // rated deviation demodulates to ~3x full scale), so the demod output swings
    // well past +/-1.0 on loud/low modulation peaks.  An out-of-range
    // (short)(x*32767) is undefined and in practice wraps a positive peak to a
    // large negative value -> a loud click/pop on the HPSDR audio path; the raw
    // float fed to audio_write() likewise hard-clips into buzzy distortion on
    // the local sound card.  Bound both here so peaks flat-top cleanly instead.
    if(left_sample  >  1.0) left_sample  =  1.0; else if(left_sample  < -1.0) left_sample  = -1.0;
    if(right_sample >  1.0) right_sample =  1.0; else if(right_sample < -1.0) right_sample = -1.0;

    // Mute-while-transmitting must zero BOTH the local (float) path and the
    // radio (short) path. This used to sit after the short conversion below, so
    // it only silenced local audio while the shorts sent to the radio kept
    // playing — audible in the radio's headphones in duplex TX. Zero first.
    if (isTransmitting(radio) && (rx->mute_while_transmitting)) {
      left_sample=0;
      right_sample=0;
    }

    left_audio_sample=(short)(left_sample*32767.0);
    right_audio_sample=(short)(right_sample*32767.0);

    if(rx->local_audio) {
      audio_write(rx,(float)left_sample,(float)right_sample);
    }

    if(radio->active_receiver==rx) {

      if ((isTransmitting(radio) || rx->remote_audio==FALSE) &&  (!rx->duplex)) {
        left_audio_sample=0;
        right_audio_sample=0;
      }
      switch(radio->discovered->protocol) {
        case PROTOCOL_1:
          if(radio->discovered->device!=DEVICE_HERMES_LITE2) {
            protocol1_audio_samples(rx,left_audio_sample,right_audio_sample);
          }
          break;
        case PROTOCOL_2:
          protocol2_audio_samples(rx,left_audio_sample,right_audio_sample);
          break;
      }
    }
  }
  if(rx->local_audio && !rx->output_started) {
    audio_start_output(rx);
  }

  // Tap the clean demodulated audio (pre listen-volume) for recording.
  recorder_audio(rx, rx->audio_output_buffer, rx->output_samples);
  // TCI (Phase C): stream the same 48 kHz demod audio to any audio_start client.
  // No-op with no audio subscribers (single atomic read).
  tci_audio_feed(rx, rx->audio_output_buffer, rx->output_samples, 48000);

#ifdef FT8
  // Decoder tap: feed the active receiver's demodulated audio to the selected
  // decoder while it is in a digital mode (DIGU/DIGL).  Which decoder — if any —
  // runs is radio->decode_mode (off by default; the operator picks FT8/FT4/SSTV
  // from the bottom-bar Decode block).  Driving enable/disable from the active
  // RX's own buffer covers mode changes, active-receiver switches and start-up
  // in one place (only the active receiver ever toggles the flag, so there is no
  // cross-RX thrash).
  if(radio->active_receiver==rx) {
    gboolean digimode = (rx->mode_a==DIGU || rx->mode_a==DIGL);
    gboolean ft8_on = digimode &&
                      (radio->decode_mode==DECODE_FT8 || radio->decode_mode==DECODE_FT4);
    ft8_decoder_set_enabled(ft8_on);
    if(ft8_on) {
      ft8_decoder_set_protocol(radio->decode_mode==DECODE_FT4 ? 1 : 0);  // FT8 vs FT4
      ft8_decoder_add_audio(rx->audio_output_buffer, rx->output_samples);
    }
  }
#endif
#ifdef SSTV
  // SSTV image decoder tap: same one-place enable/disable as FT8, but its own
  // mode selector (DECODE_SSTV).  Feeds the demodulated audio to the Hilbert
  // discriminator + line decoder in sstv_decoder.c.
  if(radio->active_receiver==rx) {
    gboolean sstv_on = (rx->mode_a==DIGU || rx->mode_a==DIGL || rx->mode_a==FMN) &&
                       radio->decode_mode==DECODE_SSTV;
    sstv_decoder_set_enabled(sstv_on);
    if(sstv_on) sstv_decoder_add_audio(rx->audio_output_buffer, rx->output_samples);

    // WEFAX / HF radiofax decoder tap (same one-place enable/disable). HF USB
    // only (DIGU/DIGL).
    gboolean wefax_on = (rx->mode_a==DIGU || rx->mode_a==DIGL) &&
                        radio->decode_mode==DECODE_WEFAX;
    wefax_decoder_set_enabled(wefax_on);
    if(wefax_on) wefax_decoder_add_audio(rx->audio_output_buffer, rx->output_samples);

    // CW (Morse) decoder tap (same one-place enable/disable). CW modes only.
    gboolean cw_on = (rx->mode_a==CWL || rx->mode_a==CWU) &&
                     radio->decode_mode==DECODE_CW;
    cw_decoder_set_enabled(cw_on);
    if(cw_on) cw_decoder_add_audio(rx->audio_output_buffer, rx->output_samples);
  }
#endif
}

// Feed the spectrum analyzer in ANALYZER_FEED_BLOCK-sized chunks so the write
// never overruns WDSP's input ring (see the contract documented in receiver.h).
// SetAnalyzer is configured with bf_sz == ANALYZER_FEED_BLOCK, so each Spectrum0
// call consumes exactly one block from `iq`.
void analyzer_feed(int channel, double *iq, int nsamples) {
  int off = 0;
  for(; off + ANALYZER_FEED_BLOCK <= nsamples; off += ANALYZER_FEED_BLOCK) {
    Spectrum0(1, channel, 0, 0, iq + 2 * off);
  }
  if(off != nsamples) {
    static gboolean warned = FALSE;
    if(!warned) {
      warned = TRUE;
      g_warning("analyzer_feed: nsamples=%d not a multiple of %d; %d samples "
                "dropped from the spectrum feed",
                nsamples, ANALYZER_FEED_BLOCK, nsamples - off);
    }
  }
}

#define SCOPE_FIR_NTAPS 63

// Tuned-path vectorscope: downmix the raw wideband I/Q by the tuned carrier's
// baseband offset (off_hz = ctun_frequency - frequency_a, 0 for plain
// non-ctun tuning) so the listening signal lands on DC, then lowpass+decimate
// to the channel bandwidth. This isolates just the tuned signal, giving a
// real constellation instead of the whole-span blob of the Wideband tap.
// Runs on the audio thread; only the final rx->scope_iq/scope_iq_n publish
// is mutex-guarded (mirrors scope_iq_feed's Wideband path) -- the FIR/NCO
// state below is touched only from here.
static void scope_iq_feed_tuned(RECEIVER *rx, double *iq, int nsamples) {
  int N = SCOPE_FIR_NTAPS;

  double fc = fabs((double)rx->filter_low_a) > fabs((double)rx->filter_high_a) ?
              fabs((double)rx->filter_low_a) : fabs((double)rx->filter_high_a);
  fc += 300.0;
  if(fc < 500.0) fc = 500.0;
  if(fc > 0.45*(double)rx->sample_rate) fc = 0.45*(double)rx->sample_rate;

  if(rx->scope_fir_ntaps==0 || fc != rx->scope_fir_fc_cached || rx->sample_rate != rx->scope_fir_rate_cached) {
    if(rx->scope_fir_taps==NULL) rx->scope_fir_taps=g_new(gfloat,N);
    if(rx->scope_fir_hist==NULL) rx->scope_fir_hist=g_new0(gfloat,2*(N-1));
    double fcn = fc / (double)rx->sample_rate;
    double h[SCOPE_FIR_NTAPS];
    double sum=0.0;
    for(int k=0; k<N; k++) {
      int m = k - (N-1)/2;
      double sinc = (m==0) ? 2.0*fcn : sin(2.0*M_PI*fcn*m)/(M_PI*m);
      double w = 0.54 - 0.46*cos(2.0*M_PI*k/(N-1));
      h[k] = sinc*w;
      sum += h[k];
    }
    if(sum==0.0) sum=1.0;
    for(int k=0; k<N; k++) rx->scope_fir_taps[k]=(gfloat)(h[k]/sum);
    rx->scope_fir_fc_cached=fc;
    rx->scope_fir_rate_cached=rx->sample_rate;
    rx->scope_fir_ntaps=N;
    rx->scope_fir_hist_n=0;
  }

  int D = rx->sample_rate/12000;
  if(D<1) D=1;

  double off_hz = (double)(rx->ctun_frequency - rx->frequency_a);
  double dph = -2.0*M_PI*off_hz/(double)rx->sample_rate;

  int n = nsamples;
  int hist_n = N-1;

  // ext = carried-over history (hist_n complex) ++ this block's downmixed
  // samples (n complex), so the FIR window at output position p can always
  // reach back hist_n samples even right at the start of the block.
  // Persistent grow-on-demand scratch (never freed, matching scope_iq's
  // lifecycle) so the audio thread doesn't g_new/g_free every block.
  int cap_ext = hist_n + n;
  if(rx->scope_tuned_ext_cap < cap_ext) {
    g_free(rx->scope_tuned_ext);
    rx->scope_tuned_ext = g_new(gfloat, 2*cap_ext);
    rx->scope_tuned_ext_cap = cap_ext;
  }
  gfloat *ext = rx->scope_tuned_ext;
  memcpy(ext, rx->scope_fir_hist, sizeof(gfloat)*2*hist_n);

  double ph = rx->scope_nco_ph;
  for(int i=0; i<n; i++) {
    // (Q, I): the receiver buffer's order — WDSP reads it that way
    // (analyzer.c Spectrum0), so this matches the panadapter. Reading it as
    // (I, Q) mirrors the spectrum, and this path downmixes by a signed offset,
    // so it would isolate the wrong side of the centre.
    double I = iq[i*2+1];
    double Q = iq[i*2];
    double c = cos(ph), s = sin(ph);
    ext[(hist_n+i)*2]   = (gfloat)(I*c - Q*s);
    ext[(hist_n+i)*2+1] = (gfloat)(I*s + Q*c);
    ph += dph;
    if(ph > M_PI) ph -= 2.0*M_PI;
    else if(ph < -M_PI) ph += 2.0*M_PI;
  }
  rx->scope_nco_ph = ph;

  int cap = rx->scope_iq_cap;
  if(rx->scope_tuned_out_cap < n) {
    g_free(rx->scope_tuned_out);
    rx->scope_tuned_out = g_new(gfloat, 2*n);
    rx->scope_tuned_out_cap = n;
  }
  gfloat *out = rx->scope_tuned_out;
  int nout = 0;
  for(int p=0; p<n && nout<cap; p+=D) {
    double y_re=0.0, y_im=0.0;
    for(int t=0; t<N; t++) {
      gfloat tap = rx->scope_fir_taps[t];
      y_re += tap * ext[(p+t)*2];
      y_im += tap * ext[(p+t)*2+1];
    }
    out[nout*2]   = (gfloat)y_re;
    out[nout*2+1] = (gfloat)y_im;
    nout++;
  }

  // Carry the last hist_n downmixed complex samples of this block forward.
  // ext's tail hist_n samples are exactly this (whether n>=hist_n or not,
  // since ext = history++block and the tail always reflects the most
  // recent hist_n samples of the stream).
  memcpy(rx->scope_fir_hist, &ext[n*2], sizeof(gfloat)*2*hist_n);
  rx->scope_fir_hist_n = hist_n;

  g_mutex_lock(&rx->scope_mutex);
  memcpy(rx->scope_iq, out, sizeof(gfloat)*2*nout);
  rx->scope_iq_n = nout;
  g_mutex_unlock(&rx->scope_mutex);
}

// Vectorscope tap: snapshot the genuine off-air I/Q (same tap point as the
// recorder/PPM cal) into rx->scope_iq for the panadapter render (GTK main
// thread) to pick up under scope_mutex. Guarded on panadapter_phase so it
// costs nothing when the scope display isn't in use. Wideband (source==0)
// is the original raw copy; Tuned (source==1) downmixes+filters onto the
// tuned carrier so the scope shows a real constellation (scope_iq_feed_tuned).
static void scope_iq_feed(RECEIVER *rx, double *iq, int nsamples) {
  if(!rx->panadapter_phase || rx->scope_iq==NULL) return;
  // Diversity source: scope_iq is filled from the mixer's main-vs-hidden tap
  // (diversity_mix_full_buffers), not from this single-RX feed.
  if(rx->panadapter_phase_source==2) return;
  if(rx->panadapter_phase_source==1) {
    scope_iq_feed_tuned(rx, iq, nsamples);
    return;
  }
  int n = nsamples < rx->scope_iq_cap ? nsamples : rx->scope_iq_cap;
  g_mutex_lock(&rx->scope_mutex);
  for(int i=0; i<n; i++) {
    // (Q, I) -> (I, Q) so the vectorscope's X/Y axes are the real I and Q.
    rx->scope_iq[i*2]   = (gfloat)iq[i*2+1];
    rx->scope_iq[i*2+1] = (gfloat)iq[i*2];
  }
  rx->scope_iq_n = n;
  g_mutex_unlock(&rx->scope_mutex);
}

static void full_rx_buffer(RECEIVER *rx) {
  int error;

  if(isTransmitting(radio) && (!rx->duplex)) return;

  // Tap the genuine off-air I/Q before the noise blanker mutates it in place.
  recorder_iq(rx, rx->iq_input_buffer, rx->buffer_size);
  ppm_cal_iq_feed(rx, rx->iq_input_buffer, rx->buffer_size);
  // QO-100 beacon lock: measures the LNB's LO drift off the raw spectrum, so it
  // wants the same untouched buffer. No-op unless the operator enabled it.
  qo100_beacon_iq_feed(rx, rx->iq_input_buffer, rx->buffer_size);
  scope_iq_feed(rx, rx->iq_input_buffer, rx->buffer_size);
  // TCI (Phase B): stream this off-air I/Q block to any iq_start client. No-op
  // with no IQ subscribers (single atomic read).
  tci_iq_feed(rx, rx->iq_input_buffer, rx->buffer_size, rx->sample_rate);
#ifdef HFDL
  // HFDL (aviation HF data link) decoder tap: raw off-air complex I/Q — NOT the
  // listen-audio path, and independent of decoder_taps_audio() (like the
  // recorder/vectorscope). Active RX in DIGU only; the enable/disable gate lives
  // here in one place (only the active RX toggles it). The decoder conditions +
  // demodulates this I/Q (front-end + symbol recovery); framing is a later phase.
  if(radio->active_receiver==rx) {
    gboolean hfdl_on = (rx->mode_a==DIGU) && radio->decode_mode==DECODE_HFDL;
    hfdl_decoder_set_enabled(hfdl_on);
    // Band scanning is a persisted setting, so apply it here rather than only
    // from the panel — the decoder runs whether or not the panel is open.
    if(hfdl_on) hfdl_decoder_set_scan(radio->hfdl_scan);
    // frequency_a is the receiver centre this I/Q is taken around (CTUN moves
    // only the demodulator), which is what lets the decoder place the other
    // HFDL channels in the passband when band scanning is on. The channel to
    // DECODE, though, is wherever the operator is pointing: with CTUN/freetune
    // that is the cursor, tens of kHz off centre — decoding the centre instead
    // would mean the cursor does nothing and the only way to hear a channel is
    // to move the whole receiver onto it.
    if(hfdl_on) {
      long long cursor = (rx->ctun || rx->freetune) ? (long long)rx->ctun_frequency
                                                    : (long long)rx->frequency_a;
      hfdl_decoder_add_iq_at(rx->iq_input_buffer, rx->buffer_size,
                             rx->sample_rate, (long long)rx->frequency_a, cursor);
    }
    // VHF ACARS: the same shape, on AM.  Also raw I/Q — the decoder runs its own
    // AM detector because the channel is 25 kHz wide and, unlike the audio-fed
    // decoders, it must be able to sit on a channel other than the one being
    // listened to (that is what Scan band does).
    gboolean acars_on = (rx->mode_a==AM || rx->mode_a==SAM) &&
                        radio->decode_mode==DECODE_ACARS;
    acars_decoder_set_enabled(acars_on);
    if(acars_on) {
      acars_decoder_set_scan(radio->acars_scan);
      long long cursor = (rx->ctun || rx->freetune) ? (long long)rx->ctun_frequency
                                                    : (long long)rx->frequency_a;
      acars_decoder_add_iq(rx->iq_input_buffer, rx->buffer_size,
                           rx->sample_rate, (long long)rx->frequency_a, cursor);
    }
  }
#endif
#ifdef SSTV
  // APT (NOAA weather satellites) decoder tap: raw off-air complex I/Q, for the
  // same reason as HFDL — the signal is ~34 kHz of FM, which is wider than our
  // widest FMN filter and far narrower than WFM, so the decoder brings its own
  // front-end rather than tapping the demodulated audio the way SSTV/WEFAX do.
  // The demod mode therefore only affects what the operator hears.
  if(radio->active_receiver==rx) {
    gboolean apt_on = (rx->mode_a==FMN) && radio->decode_mode==DECODE_APT;
    apt_decoder_set_enabled(apt_on);
    if(apt_on) {
      // As for HFDL: decode where the operator is pointing (the CTUN/freetune
      // cursor), which without CTUN is the receiver centre.
      long long cursor = (rx->ctun || rx->freetune) ? (long long)rx->ctun_frequency
                                                    : (long long)rx->frequency_a;
      apt_decoder_add_iq(rx->iq_input_buffer, rx->buffer_size,
                         (double)rx->sample_rate, (long long)rx->frequency_a, cursor);
    }
  }
#endif

  // noise blanker works on origianl IQ samples
  if(rx->nb) {
     xanbEXT (rx->channel, rx->iq_input_buffer, rx->iq_input_buffer);
  }
  if(rx->nb2) {
     xnobEXT (rx->channel, rx->iq_input_buffer, rx->iq_input_buffer);
  }

  g_mutex_lock(&rx->mutex);
  fexchange0(rx->channel, rx->iq_input_buffer, rx->audio_output_buffer, &error);
  //if(error!=0 && error!=-2) {
  if(error!=0) {
    log_error("full_rx_buffer: channel=%d fexchange0: error=%d\n",rx->channel,error);
  }

  if(rx->subrx_enable) {
    subrx_iq_buffer(rx);
  }

  analyzer_feed(rx->channel, rx->iq_input_buffer, rx->buffer_size);

  process_rx_buffer(rx);
  g_mutex_unlock(&rx->mutex);

}

void full_diviqrx_buffer(RECEIVER *rx) {
  int error;

  if(isTransmitting(radio) && (!rx->duplex)) return;

  recorder_iq(rx, rx->diviq_input_buffer, rx->buffer_size);
  ppm_cal_iq_feed(rx, rx->diviq_input_buffer, rx->buffer_size);

  // noise blanker works on origianl IQ samples
  if(rx->nb) {
     xanbEXT (rx->channel, rx->diviq_input_buffer, rx->diviq_input_buffer);
  }
  if(rx->nb2) {
     xnobEXT (rx->channel, rx->diviq_input_buffer, rx->diviq_input_buffer);
  }

  g_mutex_lock(&rx->mutex);
  fexchange0(rx->channel, rx->diviq_input_buffer, rx->audio_output_buffer, &error);
  //if(error!=0 && error!=-2) {
  if(error!=0) {
    log_error("full_rx_buffer: channel=%d fexchange0: error=%d\n",rx->channel,error);
  }

  if(rx->subrx_enable) {
    subrx_iq_buffer(rx);
  }

  analyzer_feed(rx->channel, rx->diviq_input_buffer, rx->buffer_size);
  process_rx_buffer(rx);
  g_mutex_unlock(&rx->mutex);

}

void add_iq_samples(RECEIVER *rx,double i_sample,double q_sample) {
  rx->iq_input_buffer[rx->samples*2]=i_sample;
  rx->iq_input_buffer[(rx->samples*2)+1]=q_sample;
  rx->samples=rx->samples+1;
  if(rx->samples>=rx->buffer_size) {
    // If diversity mixer active, WDSP diversity mixer works on a
    // seperate channel to the rx and must all be done pre normal RX dsp
    if (radio->divmixer[rx->dmix_id] != NULL) {
      if (radio->divmixer[rx->dmix_id]->calibrate_gain) {
        full_rx_buffer(rx);
      }
      else {
        //g_print("Mixer %d\n", mixer);
        // Diversity uses 2 rx channels, if the order of these is processed incorrectly, WDSP
        // will be working with 1 packet ahead/behind.
        // Main RX # is less than hidden rx
        int mixer = rx->dmix_id;
        if (radio->divmixer[mixer]->rx_visual->channel < radio->divmixer[mixer]->rx_hidden->channel) {
          if (rx == radio->divmixer[mixer]->rx_hidden) {
            diversity_mix_full_buffers(radio->divmixer[mixer]);
          } else {
            diversity_add_buffer(radio->divmixer[mixer]);
          }
        } else {
          // Main rx number is greater than the hidden rx
          if (rx == radio->divmixer[mixer]->rx_visual) {
            diversity_mix_full_buffers(radio->divmixer[mixer]);
          } else {
            diversity_add_buffer(radio->divmixer[mixer]);
          }
        }
      }
    } else {
      // Non diversity, normal iq packet processing
      full_rx_buffer(rx);
    }
    rx->samples=0;
  }

  if(rx->bpsk_enable && rx->bpsk!=NULL) {
    bpsk_add_iq_samples(rx->bpsk,i_sample,q_sample);
  }
}

/* How many floats the phosphor/histogram buffer holds for a panadapter of these
   dimensions.  It accumulates at HALF resolution while panadapter_histogram_w/h
   keep the FULL widget dims (the resize guard compares against those), so the
   two are easy to confuse -- and were: the Histogram checkbox memset the buffer
   with the full dims, i.e. wrote four times its size, 1.8 MB past the end on a
   1200x500 panadapter, on one click. */
int receiver_histogram_cells(int width,int height) {
  if(width<=0 || height<=0) return 0;
  return ((width+1)/2) * ((height+1)/2);
}

void set_agc(RECEIVER *rx) {

  SetRXAAGCMode(rx->channel, rx->agc);
  SetRXAAGCSlope(rx->channel,rx->agc_slope);
  SetRXAAGCTop(rx->channel,rx->agc_gain);
  switch(rx->agc) {
    case AGC_OFF:
      break;
    case AGC_LONG:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,2000);
      SetRXAAGCDecay(rx->channel,2000);
      SetRXAAGCHangThreshold(rx->channel,(int)rx->agc_hang_threshold);
      break;
    case AGC_SLOW:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,1000);
      SetRXAAGCDecay(rx->channel,500);
      SetRXAAGCHangThreshold(rx->channel,(int)rx->agc_hang_threshold);
      break;
    case AGC_MEDIUM:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,0);
      SetRXAAGCDecay(rx->channel,250);
      SetRXAAGCHangThreshold(rx->channel,100);
      break;
    case AGC_FAST:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,0);
      SetRXAAGCDecay(rx->channel,50);
      SetRXAAGCHangThreshold(rx->channel,100);
      break;
  }
}

void receiver_update_title(RECEIVER *rx) {
  gchar title[128];
  g_snprintf((gchar *)&title,sizeof(title),"MacHPSDR: %s Rx-%d ADC-%d %d",radio->discovered->name,rx->channel,rx->adc,rx->sample_rate);
log_info("receiver_update_title: %s\n",title);
  if(rx->window!=NULL) {
    gtk_window_set_title(GTK_WINDOW(rx->window),title);
  }
}

// GTK4: GtkEventControllerMotion "enter" (x,y,data) / "leave" (data) handlers.
static void enter (GtkEventControllerMotion *controller, double ex, double ey, gpointer user_data) {
   RECEIVER *rx=(RECEIVER *)user_data;
   GtkWidget *ebox=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
   if((ex>4 && ex<35) && (ebox==rx->panadapter)) {
     gtk_widget_set_cursor_from_name(ebox,"ew-resize");
   } else {
     gtk_widget_set_cursor_from_name(ebox,"crosshair");
   }
}

static void leave (GtkEventControllerMotion *controller, gpointer user_data) {
   GtkWidget *ebox=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
   gtk_widget_set_cursor_from_name(ebox,"default");
}

static void close_button_cb(GtkWidget *widget, gpointer data) {
  receiver_close((RECEIVER *)data);
}

// I/Q Player scrub bar — seek helper. Move the thumb to `frac` (0..1) and seek
// the recording there. gtk_range_set_value() emits only "value-changed" (which
// is unconnected), NOT "change-value", so this never loops back into another
// seek. A short guard window keeps the periodic timer (which also calls
// set_value to reflect playback) from fighting the user's interaction.
static void iq_seek_to(RECEIVER *rx, double frac) {
  if(frac<0.0) frac=0.0;
  if(frac>1.0) frac=1.0;
  gtk_range_set_value(GTK_RANGE(rx->iq_seek), frac);
  fake_protocol_seek(radio, frac);
  rx->iq_seek_guard_us = g_get_monotonic_time() + 300000;   // 300 ms
}

// Absolute-position press/drag on the scrub bar: a click anywhere JUMPS to that
// point (not the default GtkScale trough-click, which pages a step toward the
// pointer and so rewinds on a click behind the thumb). Runs in the CAPTURE
// phase and claims the sequence so GtkRange's own paging/drag never fires; a
// single click is a begin with no updates, a drag streams updates.
static void iq_seek_frac_at(GtkGesture *g, double x, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  int w=gtk_widget_get_width(rx->iq_seek);
  if(w>0) iq_seek_to(rx, x/(double)w);
}
static void iq_seek_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
  gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
  iq_seek_frac_at(GTK_GESTURE(g), x, data);
}
static void iq_seek_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer data) {
  double sx=0.0, sy=0.0;
  gtk_gesture_drag_get_start_point(g, &sx, &sy);
  iq_seek_frac_at(GTK_GESTURE(g), sx+ox, data);
}
// Keyboard (arrow keys) / scroll-wheel still reach the range and emit
// change-value — route those through the seek too.
static gboolean iq_seek_change_cb(GtkRange *range, GtkScrollType scroll, double value, gpointer data) {
  iq_seek_to((RECEIVER *)data, value);
  return TRUE;   // handled — we already moved the thumb + sought
}

static void create_visual(RECEIVER *rx) {

  rx->dialog=NULL;

  rx->window=NULL;

  receiver_update_title(rx);

  rx->table=gtk_grid_new();

  rx->vfo=create_vfo(rx);
  gtk_widget_set_hexpand(rx->vfo,TRUE);
  gtk_grid_attach(GTK_GRID(rx->table), rx->vfo, 0, 0, 3, 1);

  rx->radio_info=create_radio_info_visual(rx);
  gtk_widget_set_size_request(rx->radio_info, 170, 60);
  gtk_grid_attach(GTK_GRID(rx->table), rx->radio_info, 3, 0, 1, 1);

  rx->meter=create_meter_visual(rx);
  gtk_widget_set_size_request(rx->meter,250,60);        // resize from 154 to 300 for custom s-meter
  gtk_grid_attach(GTK_GRID(rx->table), rx->meter, 4, 0, 2, 1);

  // Additional receivers (not the primary channel 0) get a close button in the
  // top-right corner of the panel. Placed as a real grid cell rather than a
  // GtkOverlay, because the panadapter's native window would hide an overlay.
  if(rx->channel!=0) {
    GtkWidget *close_b=gtk_button_new_with_label("✕");
    gtk_widget_set_name(close_b,"vfo-close");
    gtk_widget_set_tooltip_text(close_b,"Close this receiver");
    gtk_widget_set_halign(close_b,GTK_ALIGN_END);
    gtk_widget_set_valign(close_b,GTK_ALIGN_START);
    g_signal_connect(close_b,"clicked",G_CALLBACK(close_button_cb),rx);
    gtk_grid_attach(GTK_GRID(rx->table), close_b, 6, 0, 1, 1);
  }

  rx->vpaned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_name(rx->vpaned,"rx-spectrum");   // hairline inset frame (CSS)
  gtk_widget_set_hexpand(rx->vpaned, TRUE);
  gtk_widget_set_vexpand(rx->vpaned, TRUE);
  // Breathing room between the VFO mode/filter row and the spectrum below it.
  gtk_widget_set_margin_top(rx->vpaned, 8);

  rx->panadapter=create_rx_panadapter(rx);
  // Guarantee the spectroscope a minimum height and forbid the pane from
  // shrinking it below that (shrink=FALSE) so it can never open as a sliver.
  gtk_widget_set_size_request(rx->panadapter, -1, MIN_PANADAPTER_HEIGHT);
  gtk_paned_set_start_child (GTK_PANED(rx->vpaned), rx->panadapter);
  gtk_paned_set_resize_start_child (GTK_PANED(rx->vpaned), TRUE);
  gtk_paned_set_shrink_start_child (GTK_PANED(rx->vpaned), FALSE);
  // GTK4: enter/leave come from a motion controller (crossing events are gone).
  GtkEventController *pan_cross=gtk_event_controller_motion_new();
  g_signal_connect(pan_cross,"enter",G_CALLBACK(enter),rx);
  g_signal_connect(pan_cross,"leave",G_CALLBACK(leave),rx);
  gtk_widget_add_controller(rx->panadapter,pan_cross);

  rx->waterfall=create_waterfall(rx);
  gtk_paned_set_end_child (GTK_PANED(rx->vpaned), rx->waterfall);
  gtk_paned_set_resize_end_child (GTK_PANED(rx->vpaned), TRUE);
  gtk_paned_set_shrink_end_child (GTK_PANED(rx->vpaned), TRUE);
  GtkEventController *wf_cross=gtk_event_controller_motion_new();
  g_signal_connect(wf_cross,"enter",G_CALLBACK(enter),rx);
  g_signal_connect(wf_cross,"leave",G_CALLBACK(leave),rx);
  gtk_widget_add_controller(rx->waterfall,wf_cross);

  // Wrap the RF spectrum in a horizontal GtkBox so the FT8 band waterfall can be
  // added to its right (~1/3).  (The GTK3 "windowless box" GdkWindow workaround
  // is moot in GTK4 — widgets have no child GdkWindows — but the box layout is
  // kept.)  ft8_waterfall is added/removed by receiver_ft8_waterfall_sync().
  rx->wf_hpaned=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  rx->ft8_waterfall=NULL;
  gtk_box_append(GTK_BOX(rx->wf_hpaned), rx->vpaned);
  gtk_widget_set_hexpand(rx->wf_hpaned, TRUE);
  gtk_widget_set_vexpand(rx->wf_hpaned, TRUE);
  // The spectrum goes in through an overlay so the I/Q Player's scrub bar can
  // float over the bottom of the waterfall instead of taking a row of its own
  // below it.  A row of its own put the slider between the waterfall and the
  // bottom of the RX block — and therefore between the waterfall and the pane
  // handle, which belongs on the boundary between the spectrum and whatever is
  // under it.  Overlaid, the waterfall really is the last thing in the block,
  // and the handle lands where the eye expects it.  (It is also where every
  // media player puts a transport bar.)
  GtkWidget *spectrum_overlay=gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(spectrum_overlay), rx->wf_hpaned);
  gtk_grid_attach(GTK_GRID(rx->table), spectrum_overlay, 0, 1, 7, 2);
  gtk_widget_set_hexpand(spectrum_overlay, TRUE);
  gtk_widget_set_vexpand(spectrum_overlay, TRUE);

  // I/Q Player scrub bar: a transport slider over the foot of the waterfall to
  // seek within the looped recording. Only for the fake ("I/Q Player") device's
  // primary RX; hidden until a file is actually playing (see update_timer_cb),
  // and an invisible overlay child is not allocated, so on a real radio the
  // waterfall is untouched.
  rx->iq_seek=NULL;
  rx->iq_seek_guard_us=0;
  if(radio->discovered->protocol==PROTOCOL_FAKE && rx->channel==0) {
    rx->iq_seek=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.0005);
    gtk_scale_set_draw_value(GTK_SCALE(rx->iq_seek), FALSE);
    gtk_widget_set_name(rx->iq_seek, "iq-seek");
    gtk_widget_set_hexpand(rx->iq_seek, TRUE);
    gtk_widget_set_tooltip_text(rx->iq_seek, "Seek the I/Q recording");
    gtk_widget_set_visible(rx->iq_seek, FALSE);
    g_signal_connect(rx->iq_seek, "change-value", G_CALLBACK(iq_seek_change_cb), rx);
    // Absolute-position click/drag (jump to the pointer, not page-step).
    GtkGesture *iq_drag=gtk_gesture_drag_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(iq_drag), GTK_PHASE_CAPTURE);
    g_signal_connect(iq_drag, "drag-begin",  G_CALLBACK(iq_seek_drag_begin),  rx);
    g_signal_connect(iq_drag, "drag-update", G_CALLBACK(iq_seek_drag_update), rx);
    gtk_widget_add_controller(rx->iq_seek, GTK_EVENT_CONTROLLER(iq_drag));
    gtk_widget_set_valign(rx->iq_seek, GTK_ALIGN_END);
    gtk_widget_set_halign(rx->iq_seek, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(spectrum_overlay), rx->iq_seek);
  }

  gtk_widget_set_size_request(rx->table, -1, 180);
  // Make sure the panel grows to fill the container on window resize (otherwise
  // the vpaned can stay at its minimum and leave a gap below the waterfall).
  gtk_widget_set_hexpand(rx->table, TRUE);
  gtk_widget_set_vexpand(rx->table, TRUE);
  gtk_widget_set_valign(rx->table, GTK_ALIGN_FILL);
}

void receiver_init_analyzer(RECEIVER *rx) {
    int flp[] = {0};
    double keep_time = 0.1;
    int n_pixout=1;
    int spur_elimination_ffts = 1;
    int data_type = 1;
    int fft_size = 8192;
    int window_type = 4;
    double kaiser_pi = 14.0;
    // WDSP advances (fft_size - overlap) samples per spectrum frame, so the
    // display frame rate is sample_rate/(fft_size-overlap). A hardcoded overlap
    // (was 2048) made that rate depend on the sample rate — e.g. only ~15 fps at
    // 96 kHz — which the fps-rate GetPixels poll then aliased into a jerky
    // waterfall. Tie overlap to sample_rate/fps (the canonical linhpsdr formula)
    // so a new frame lands every 1/fps s and the waterfall scrolls smoothly.
    int overlap = (int)fmax(0.0, ceil((double)fft_size - (double)rx->sample_rate / (double)rx->fps));
    if(overlap >= fft_size) overlap = fft_size - 1;
    int clip = 0;
    int span_clip_l = 0;
    int span_clip_h = 0;
    int pixels=rx->pixels;
    int stitches = 1;
    int calibration_data_set = 0;
    double span_min_freq = 0.0;
    double span_max_freq = 0.0;

  if(rx->pixel_samples!=NULL) {
    g_free(rx->pixel_samples);
    rx->pixel_samples=NULL;
  }
  if(rx->panadapter_peaks!=NULL) {
    g_free(rx->panadapter_peaks);
    rx->panadapter_peaks=NULL;
  }
  if(rx->panadapter_histogram_bins!=NULL) {
    g_free(rx->panadapter_histogram_bins);
    rx->panadapter_histogram_bins=NULL;
  }
  // The cached heatmap blit surface is sized to the bins; drop it here so the
  // panadapter re-creates it once at the new dimensions (was created/destroyed
  // every frame before it was cached).
  if(rx->panadapter_histogram_surface!=NULL) {
    cairo_surface_destroy(rx->panadapter_histogram_surface);
    rx->panadapter_histogram_surface=NULL;
  }
  rx->panadapter_histogram_w=0;
  rx->panadapter_histogram_h=0;
  // Histogram bins accumulate in SCREEN coords (not analyzer pixels), so they
  // are sized to the widget rather than rx->pixels - this keeps memory bounded
  // regardless of zoom. Only allocate once the widget is actually sized; a
  // later resize_timeout re-runs receiver_init_analyzer() and reallocs.
  if(rx->panadapter_width>0 && rx->panadapter_height>0) {
    // Phosphor accumulates at HALF resolution; the GPU upscales the texture with
    // a linear filter at draw time. The occupancy cloud is soft, so half-res is
    // visually ~indistinguishable while the two O(area) CPU loops (decay +
    // colour-map, in update_rx_panadapter) do 1/4 the work — matters maximised.
    // _w/_h stay the FULL widget dims (the resize guard compares against them);
    // the half dims are derived as (_w+1)/2 x (_h+1)/2 everywhere.
    rx->panadapter_histogram_bins=g_new0(float,receiver_histogram_cells(rx->panadapter_width,
                                                                          rx->panadapter_height));
    rx->panadapter_histogram_w=rx->panadapter_width;
    rx->panadapter_histogram_h=rx->panadapter_height;
  }
  if(rx->pixels>0) {
    rx->pixel_samples=g_new0(float,rx->pixels);
    // Peak-hold buffer mirrors pixel_samples 1:1 but seeded to a low floor
    // (g_new0's 0.0 dBm reads as a huge signal, which would draw the overlay
    // as a flat line pinned at the top until real data pushes it down).
    rx->panadapter_peaks=g_new(float,rx->pixels);
    for(int _p=0;_p<rx->pixels;_p++) rx->panadapter_peaks[_p]=-220.0f;
    rx->hz_per_pixel=(gdouble)rx->sample_rate/(gdouble)rx->pixels;

    int max_w = fft_size + (int) fmin(keep_time * (double) rx->fps, keep_time * (double) fft_size * (double) rx->fps);

    SetAnalyzer(rx->channel,
            n_pixout,
            spur_elimination_ffts,
            data_type,
            flp,
            fft_size,
            ANALYZER_FEED_BLOCK, // transfer block: must divide the input ring; fed via analyzer_feed()
            window_type,
            kaiser_pi,
            overlap,
            clip,
            span_clip_l,
            span_clip_h,
            pixels,
            stitches,
            calibration_data_set,
            span_min_freq,
            span_max_freq,
            max_w
    );
  }

}

// WDSP's analyzer keeps fixed-size internal buffers of dMAX_PIXELS (16384)
// entries; asking SetAnalyzer for more pixels overruns them and corrupts the
// heap (blank display + broken RX/decode).  rx->pixels == panadapter_width*zoom,
// so cap the zoom to whatever keeps pixels within that ceiling.
#define WDSP_MAX_PIXELS 16384

void receiver_change_zoom(RECEIVER *rx,int zoom) {
log_info("%s: %d\n",__FUNCTION__,zoom);
  if(rx->panadapter_width>0) {
    int max_zoom=WDSP_MAX_PIXELS/rx->panadapter_width;
    if(max_zoom<1) max_zoom=1;
    if(zoom>max_zoom) zoom=max_zoom;
  }
  if(zoom<1) zoom=1;
  rx->zoom=zoom;
  rx->pixels=rx->panadapter_width*rx->zoom;
  // Centre the zoomed view on the cursor (freetune/ctun listening frequency);
  // for plain tuning ctun_offset==0 so this centres on the span, as before.
  center_pan_on_cursor(rx);
  receiver_init_analyzer(rx);
}

// Restore the saved panadapter/waterfall split. Must run only once the vpaned
// has a real allocated height - at create_receiver time the panel is not yet in
// a sized window (it gets re-parented into the RX stack and the window resized
// afterwards), so gtk_widget_get_height() would return ~1 and the
// saved proportion would be lost. Poll on a timeout until allocated, then apply.
static gboolean restore_paned_position_cb(gpointer data);

// Re-fit the panadapter/waterfall split to the vpaned's CURRENT height. The
// position is absolute pixels, so when the RX area is resized (e.g. a decode
// panel opens/closes and the outer paned re-apportions the height) the old
// position no longer matches — leaving the panadapter collapsed. Re-run the
// restore (retries until allocated) so the split re-fits the new height.
// Show or hide the panadapter (spectroscope) per rx->show_panadapter. When
// hidden the widget is set invisible so the GtkPaned gives the waterfall the
// whole height; the waterfall's own resize then drives rx->pixels/analyzer (see
// waterfall.c resize_timeout) so the spectrum feed keeps working without the
// panadapter. When shown, re-fit the saved split.
void receiver_apply_panadapter_visibility(RECEIVER *rx) {
  if(rx==NULL || rx->panadapter==NULL) return;
  if(rx->show_panadapter) {
    gtk_widget_set_visible(rx->panadapter,TRUE);
    receiver_refit_vpaned(rx);
  } else {
    gtk_widget_set_visible(rx->panadapter,FALSE);
  }
}

void receiver_refit_vpaned(RECEIVER *rx) {
  if(rx==NULL || rx->vpaned==NULL) return;
  if(!rx->show_panadapter) return;   // waterfall owns the whole pane
  rx->paned_restore_tries=0;
  // 150 ms so this lands AFTER the outer rx_stack_balance (100 ms) has set the
  // RX area height; restore_paned_position_cb then retries until the vpaned is
  // allocated at its final size.
  if(rx->paned_restore_timer!=0) g_source_remove(rx->paned_restore_timer);
  rx->paned_restore_timer=g_timeout_add(150, restore_paned_position_cb, (gpointer)rx);
}

static gboolean restore_paned_position_cb(gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(rx->vpaned==NULL) { rx->paned_restore_timer=0; return FALSE; }
  gint paned_height=gtk_widget_get_height(rx->vpaned);
  // Keep waiting until the pane is allocated a usable height. A tiny but >1
  // height (mid-allocation) would otherwise compute a sliver position and stop
  // for good, opening the spectroscope collapsed — the bug this guards against.
  // Once it is at least the two minimums tall we commit; a cramped pane past
  // ~20 retries (~3 s) is committed anyway (clamped) so we never spin forever.
  gint min_total=MIN_PANADAPTER_HEIGHT+MIN_WATERFALL_HEIGHT;
  if(paned_height<=1) return TRUE;  // not allocated yet, keep waiting
  if(paned_height<min_total && ++rx->paned_restore_tries<20) return TRUE;
  rx->paned_restore_tries=0;
  // Guard a degenerate saved split: 0 collapses the panadapter (spectroscope
  // invisible), 1 collapses the waterfall, and NaN fails every comparison —
  // fall back to an even split so the spectrum is always visible.
  double pct=rx->paned_percent;
  if(!(pct>0.05 && pct<0.95)) pct=0.5;
  gint position=(gint)((double)paned_height*pct);
  // Clamp so both the spectroscope and the waterfall keep their minimum height
  // (skip when the pane is too short to satisfy both — clamp order would flip).
  if(paned_height>=min_total) {
    if(position < MIN_PANADAPTER_HEIGHT) position=MIN_PANADAPTER_HEIGHT;
    if(position > paned_height - MIN_WATERFALL_HEIGHT) position=paned_height - MIN_WATERFALL_HEIGHT;
  }
  gtk_paned_set_position(GTK_PANED(rx->vpaned),position);
  rx->paned_restore_timer=0;
  return FALSE;  // done, stop the timeout
}


RECEIVER *create_receiver(int channel,int sample_rate, gboolean show_rx) {



  RECEIVER *rx=g_new0(RECEIVER,1);
  char name [80];
  char *value;
  gint x=-1;
  gint y=-1;
  gint width;
  gint height;

log_info("create_receiver: channel=%d sample_rate=%d\n", channel, sample_rate);
  g_mutex_init(&rx->mutex);
  rx->channel=channel;
  rx->adc=0;

  rx->frequency_min=(gint64)radio->discovered->frequency_min;
  rx->frequency_max=(gint64)radio->discovered->frequency_max;
log_info("create_receiver: channel=%d frequency_min=%lld frequency_max=%lld\n", channel, rx->frequency_min, rx->frequency_max);

// DEBUG
  if(channel==0 ) {
    rx->adc=0;
  } else {
    switch(radio->discovered->protocol) {
      case PROTOCOL_1:
        switch(radio->discovered->device) {
          case DEVICE_ANGELIA:
          case DEVICE_ORION:
          case DEVICE_ORION2:
            rx->adc=1;
            break;
          default:
            rx->adc=0;
            break;
        }
        break;
      case PROTOCOL_2:
        switch(radio->discovered->device) {
          case NEW_DEVICE_ANGELIA:
          case NEW_DEVICE_ORION:
          case NEW_DEVICE_ORION2:
            rx->adc=1;
            break;
          default:
            rx->adc=0;
            break;

        }
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        if(radio->discovered->supported_receivers>1 &&
// fv - need to figure how to deal with the Lime Suite
           strcmp(radio->discovered->name,"lms7")==0) {
          rx->adc=2;
        } else {
          rx->adc=1;
        }
        break;
#endif
    }
  }

  rx->sample_rate=sample_rate;
  rx->dsp_rate=48000;
  rx->output_rate=48000;

  switch(radio->discovered->protocol) {
    default:
      rx->frequency_a=14200000;
      rx->band_a=band20;
      rx->mode_a=USB;
      rx->bandstack=1;
      break;
    case PROTOCOL_FAKE:
      // The faker's frequency is cosmetic; start centred at 100 MHz on the
      // general-coverage band so the dial can roam its full 0..200 MHz range
      // (VHF content included) instead of being pinned to an HF ham band.
      rx->frequency_a=100000000;
      rx->band_a=bandGen;
      rx->mode_a=USB;
      rx->bandstack=1;
      break;
#ifdef IIO
    case PROTOCOL_IIO:
      rx->frequency_a=2400000000;
      rx->band_a=band2300;
      rx->mode_a=USB;
      rx->bandstack=1;
      break;
#endif
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      rx->frequency_a=145000000;
      rx->band_a=band144;
      rx->mode_a=USB;
      rx->bandstack=1;
      break;
#endif
  }

  rx->deviation=2500;
  rx->squelch_enable = FALSE;
  rx->squelch = 0.1;
  for(int i=0;i<MODES;i++) rx->mode_squelch[i]=rx->squelch;
  // AMSQ calibration defaults: the endpoints set_squelch used before they were
  // made settable, and WDSP's own amsq create-time tail.
  rx->amsq_min_db = -160.0;
  rx->amsq_max_db = -40.0;
  rx->amsq_tail = 1.5;   // WDSP's create_amsq max_tail, so the default is a no-op

  // APF (CW audio peak filter) defaults mirror the WDSP "speak" create values
  // (bw 100 Hz, gain 2.0); off by default. Freq tracks the CW sidetone in set_apf.
  rx->apf_enable = FALSE;
  rx->apf_bw = 100.0;
  rx->apf_gain = 2.0;

  rx->filter_a=F5;
  for(int i=0;i<MODES;i++) rx->mode_filter[i]=F5;
  rx->lo_a=0;
  rx->error_a=0;
  rx->offset=0;

  rx->frequency_b=rx->frequency_a;
#ifdef USE_VFO_B_MODE_AND_FILTER
  rx->band_b=rx->band_a;
  rx->mode_b=rx->mode_a;
  rx->filter_b=rx->filter_a;
#endif
  rx->lo_b=rx->lo_a;
  rx->error_b=rx->error_a;


  rx->lo_tx=0;
  rx->tx_track_rx=FALSE;

  rx->ctun=FALSE;
  rx->ctun_offset=0;
  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
  rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);

  /* freetune defaults to off */
  rx->freetune=FALSE;

  rx->bpsk=NULL;
  rx->bpsk_enable=FALSE;

  rx->frequency_b=14300000;
#ifdef USE_VFO_B_MODE_AND_FILTER
  rx->band_b=band20;
  rx->mode_b=USB;
  rx->filter_b=F5;
#endif
  rx->lo_b=0;
  rx->error_b=0;

  rx->split=FALSE;
  rx->vfo_linked=TRUE;

  rx->rit_enabled=FALSE;
  rx->rit=0;
  rx->rit_step=10;

  rx->step=100;
  rx->locked=FALSE;

  rx->volume=0.05;
  rx->mute=FALSE;
  rx->agc=AGC_OFF;
  for(int i=0;i<MODES;i++) rx->mode_agc[i]=AGC_OFF;
  rx->agc_gain=80.0;
  rx->agc_slope=35.0;
  rx->agc_hang_threshold=0.0;

  rx->smeter=RXA_S_AV;

  rx->pixels=0;
  rx->pixel_samples=NULL;
  rx->panadapter_peaks=NULL;
  rx->panadapter_histogram_bins=NULL;
  rx->panadapter_histogram_w=0;
  rx->panadapter_histogram_h=0;
  rx->scope_iq=NULL;
  rx->scope_iq_cap=0;
  rx->scope_iq_n=0;
  rx->scope_nco_ph=0.0;
  rx->scope_fir_taps=NULL;
  rx->scope_fir_ntaps=0;
  rx->scope_fir_fc_cached=0.0;
  rx->scope_fir_rate_cached=0;
  rx->scope_fir_hist=NULL;
  rx->scope_fir_hist_n=0;
  rx->waterfall_pixbuf=NULL;
  rx->iq_sequence=0;
  // Wideband receivers use the large 5120-sample I/O block.  WFM runs the whole
  // DSP chain at the (wide) sample_rate with a large output resampler
  // (sample_rate->48k).  Two things must hold to avoid glitches in WDSP's async
  // double-buffered I/O ring:
  //   (1) in_size == dsp_insize, else the DSP thread fills the output ring only
  //       every N-th fexchange while fexchange drains it every call -> periodic
  //       boundary click.  For WFM dsp_rate==sample_rate so dsp_insize==fft_size,
  //       hence buffer_size must equal fft_size.
  //   (2) the output block out_size = buffer_size/(sample_rate/48000) must be an
  //       exact integer AND reasonably large; tiny blocks (=64 at 1536k with
  //       buffer_size=2048) still glitch.
  // 5120 = 2^10 * 5 satisfies both for every offered span, because it is an exact
  // multiple of (sample_rate/48000) for 192/384/768/1536/1920 k (ratios
  // 4/8/16/32/40) -> out_size = 1280/640/320/160/128, all integer and >=128.
  // (A pure power of two like 4096 cannot support the 1920k span, whose ratio 40
  // has a factor of 5.)  The fake test device shares this so it too can offer the
  // wide spans (its 1024-sample block breaks 1920k: 1024/40 is not an integer).
  gboolean wide_buffers = (radio->discovered->protocol==PROTOCOL_FAKE);
#ifdef SOAPYSDR
  if(radio->discovered->device==DEVICE_SOAPYSDR) wide_buffers=TRUE;
#endif
  rx->buffer_size = wide_buffers ? 5120 : 1024;
log_info("create_receiver: buffer_size=%d\n",rx->buffer_size);
  rx->iq_input_buffer=g_new0(gdouble,2*rx->buffer_size);
  rx->diviq_input_buffer=g_new0(gdouble,2*rx->buffer_size);
  rx->scope_iq_cap=rx->buffer_size;
  rx->scope_iq=g_new0(gfloat,2*rx->scope_iq_cap);
  rx->scope_iq_n=0;


  rx->audio_buffer_size=480;
  rx->audio_buffer=g_new0(guchar,rx->audio_buffer_size);
  rx->audio_sequence=0;

  rx->mixed_audio=0;

  rx->output_started=FALSE;

  rx->fps=25;
  rx->meter_smoothing=50;   // half-strength S-meter needle ballistics by default
  rx->display_average_time=40.0;
  rx->display_detector_mode = DETECTOR_MODE_AVERAGE;      // matches previous hard-coded default
  rx->display_average_mode  = AVERAGE_MODE_LOG_RECURSIVE; // matches previous hard-coded default

  // Must equal buffer_size for wideband receivers so in_size==dsp_insize for the
  // WFM chain (see the buffer_size comment above): avoids the WDSP output-ring
  // boundary glitch.
  rx->fft_size = wide_buffers ? 5120 : 2048;
log_info("create_receiver: fft_size=%d\n",rx->fft_size);
  rx->low_latency=FALSE;

  rx->nb=FALSE;
  rx->nb2=FALSE;
  rx->nr=FALSE;
  rx->nr2=FALSE;
  rx->nr3=FALSE;
  rx->nr4=FALSE;
  // NR4 defaults (match create_sbnr in wdsp/sbnr.c)
  rx->nr4_reduction=10.0;
  rx->nr4_smoothing=0.0;
  rx->nr4_whitening=0.0;
  rx->nr4_rescale=2.0;
  rx->nr4_postfilter=-10.0;
  rx->anf=FALSE;
  rx->snb=FALSE;

  rx->notches=0;
  rx->notch_default_width=NOTCH_DEFAULT_WIDTH;

  rx->nr_agc=0;
  rx->nr2_gain_method=2;
  rx->nr2_npe_method=0;
  rx->nr2_ae=1;

  // set default location and sizes
  rx->window_x=channel*30;
  rx->window_y=channel*30;
  rx->window_width=820;
  rx->window_height=180;
  rx->window=NULL;
  rx->panadapter_low=-140;
  rx->panadapter_high=-60;
  rx->panadapter_step=20;
  rx->panadapter_surface=NULL;
  rx->panadapter_automatic=FALSE;
  rx->pan_auto_seeded=FALSE;

  rx->panadapter_filled=TRUE;
  rx->panadapter_gradient=TRUE;
  rx->panadapter_agc_line=TRUE;

  rx->panadapter_single_color=TRUE;

  rx->panadapter_peak_hold=FALSE;
  rx->panadapter_peak_decay=10;

  rx->panadapter_histogram=FALSE;
  rx->panadapter_histogram_decay=20;

  rx->panadapter_phase=FALSE;
  rx->panadapter_phase_mode=0;
  rx->panadapter_phase_gain=100;
  rx->panadapter_phase_source=0;
  rx->scope_ref=0.0;
  rx->qo100_ref_dbm=-1000.0;   // not yet seeded (see the QO-100 reference line)
  g_mutex_init(&rx->scope_mutex);

  rx->waterfall_automatic=TRUE;
  rx->waterfall_ft8_marker=FALSE;
  rx->waterfall_color_theme=1;

  rx->vfo_surface=NULL;
  rx->meter_surface=NULL;
  rx->radio_info_surface=NULL;

#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    rx->remote_audio=FALSE;
  } else {
#endif
    rx->remote_audio=TRUE;        // on or off remote audio
#ifdef SOAPYSDR
  }
#endif
  // Default a freshly-added radio to Local Audio on the "System Default" output
  // so it makes sound out of the box.  receiver_restore_state only overrides
  // these when the property actually exists, so an existing device's saved audio
  // choice (including a deliberate "off") is preserved — only brand-new devices
  // get this default.
  rx->local_audio=TRUE;
  rx->local_audio_buffer_size=2048;
  rx->local_audio_buffer_offset=0;
  rx->local_audio_buffer=NULL;
  rx->local_audio_latency=50;
  rx->audio_channels = 0;
  g_mutex_init(&rx->local_audio_mutex);


  rx->audio_name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
  rx->mute_when_not_active=FALSE;

  rx->zoom=1;
  rx->pan=0;
  rx->is_panning=FALSE;
  rx->pointer_pressed=FALSE;
  rx->enable_equalizer=FALSE;
  for(int i=0;i<11;i++) rx->equalizer[i]=0;

  rx->bookmark_dialog=NULL;

  rx->filter_frame=NULL;
  rx->filter_grid=NULL;

  rx->rigctl_port=19090+rx->channel;
  rx->rigctl_enable=FALSE;
  rx->cat_client_connected = FALSE;

  strcpy(rx->rigctl_serial_port,"/dev/ttyACM0");
  rx->rigctl_serial_baudrate=B9600;
  rx->rigctl_serial_enable=FALSE;
  rx->rigctl_debug=FALSE;
  rx->rigctl=NULL;

  rx->vpaned=NULL;
  rx->paned_position=-1;
  rx->paned_restore_tries=0;
  rx->paned_percent=0.5;
  rx->show_panadapter=TRUE;

  rx->split=SPLIT_OFF;
  rx->vfo_linked=TRUE;   // SAT/RSAT track together unless the operator unlinks
  rx->duplex=FALSE;
  rx->mute_while_transmitting=FALSE;

  rx->vfo=NULL;
  rx->subrx_enable=FALSE;
  rx->subrx_restore_pending=FALSE;
  rx->subrx=NULL;
  rx->subrx_mix=0;

  rx->diversity = FALSE;
  rx->diversity_hidden_rx = -1;
  // "no diversity mixer" is the SENTINEL slot, radio->divmixer[MAX_DIVERSITY_MIXERS]
  // -- the reason that array is declared one longer than the number of mixers:
  // it is permanently NULL, so the ten or so `if(radio->divmixer[rx->dmix_id])`
  // tests (including one on the per-frame panadapter draw) need no bounds check.
  // MAX_DIVERSITY_MIXERS+1 indexed one PAST the end, and what lies there is
  // radio->alex_rx_antenna/alex_tx_antenna: with both antennas 0 the read
  // happens to give NULL and the guard accidentally works, but selecting any
  // non-zero ALEX antenna turns those two ints into a garbage DIVMIXER* that
  // the very next line dereferences.
  rx->dmix_id = MAX_DIVERSITY_MIXERS;

  rx->show_rx = show_rx;

  receiver_restore_state(rx);

  // A receiver the caller asked to be hidden (a diversity *hidden* RX, or a
  // PureSignal feedback RX) must stay hidden even if this slot was persisted
  // with show_rx=1 from a session where it was a normal shown receiver.
  // Otherwise it builds a full visual + update timer that delete_receiver does
  // not tear down, leaving the timer firing on freed widgets (GTK_IS_WIDGET
  // criticals / frozen UI) when diversity is switched off.
  if(!show_rx) rx->show_rx = FALSE;

  if(radio->discovered->protocol==PROTOCOL_1) {
    if(rx->sample_rate!=sample_rate) {
      rx->sample_rate=sample_rate;
    }
  }

  // Fake test device runs at a fixed wide rate (see create_radio); don't let a
  // persisted narrower rate override it, so wideband FM has room on the display.
  if(radio->discovered->protocol==PROTOCOL_FAKE) {
    rx->sample_rate=sample_rate;
  }

  rx->output_samples=rx->buffer_size/(rx->sample_rate/48000);
  rx->audio_output_buffer=g_new0(gdouble,2*rx->output_samples);

log_info("create_receiver: OpenChannel: channel=%d buffer_size=%d sample_rate=%d fft_size=%d output_samples=%d\n", rx->channel, rx->buffer_size, rx->sample_rate, rx->fft_size,rx->output_samples);

  // Size the iobuff ring to this receiver's span (2/4/8/16 by width); captured
  // by create_iobuffs inside OpenChannel below.
  SetDSPMult(rx_ring_depth(rx->sample_rate));

  OpenChannel(rx->channel,
              rx->buffer_size,
              rx->fft_size,
              rx->sample_rate,
              48000, // dsp rate
              48000, // output rate
              0, // receive
              1, // run
              0.010, 0.025, 0.0, 0.010, 0);

  // The channel is opened at 48 kHz DSP rate regardless of any rate persisted
  // in props; keep rx->dsp_rate in sync so set_mode's guard reflects reality
  // (otherwise a restored WFM rate would make it skip the needed raise).
  rx->dsp_rate=48000;

  // Modified per pihpsdr commit d9af51206087959083feddcb325443d9368dad8c
  create_anbEXT(rx->channel, 1, rx->buffer_size, rx->sample_rate, 0.00001, 0.00001, 0.00001, 0.05, 4.95);
  create_nobEXT(rx->channel,1, 0, rx->buffer_size, rx->sample_rate, 0.00001, 0.00001, 0.00001, 0.05, 4.95);
  RXASetNC(rx->channel, rx->fft_size);
  RXASetMP(rx->channel, rx->low_latency);
#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    rx->resample_step=radio->sample_rate/rx->sample_rate;
log_info("receiver_change_sample_rate: resample_step=%d\n",rx->resample_step);
  }
#endif


  frequency_changed(rx);
  receiver_mode_changed(rx,rx->mode_a);

  SetRXAPanelGain1(rx->channel, rx_panel_gain(rx));
  SetRXAPanelSelect(rx->channel, 3);
  SetRXAPanelPan(rx->channel, 0.5);
  SetRXAPanelCopy(rx->channel, 0);
  SetRXAPanelBinaural(rx->channel, 0);
  SetRXAPanelRun(rx->channel, 1);

  if(rx->enable_equalizer) {
    SetRXAGrphEQ10(rx->channel, rx->equalizer);
    SetRXAEQRun(rx->channel, 1);
  } else {
    SetRXAEQRun(rx->channel, 0);
  }

  set_agc(rx);

  // Hold every stream-altering block off at channel-init for a data mode
  // (DIGU/DIGL) or a decoder-tapped mode (e.g. a restored DIGU config) — same rule
  // as update_noise()/receiver_notch_sync().
  gboolean bypass = bypass_stream_dsp(rx);
  SetEXTANBRun(rx->channel, bypass ? 0 : rx->nb);
  SetEXTNOBRun(rx->channel, bypass ? 0 : rx->nb2);

  SetRXAEMNRPosition(rx->channel, rx->nr_agc);
  SetRXAEMNRgainMethod(rx->channel, rx->nr2_gain_method);
  SetRXAEMNRnpeMethod(rx->channel, rx->nr2_npe_method);
  SetRXAEMNRRun(rx->channel, bypass ? 0 : rx->nr2);
  SetRXAEMNRaeRun(rx->channel, rx->nr2_ae);

  SetRXAANRVals(rx->channel, 64, 16, 16e-4, 10e-7); // defaults
  SetRXAANRRun(rx->channel, bypass ? 0 : rx->nr);
  SetRXARNNRRun(rx->channel, bypass ? 0 : rx->nr3);
  SetRXASBNRRun(rx->channel, bypass ? 0 : rx->nr4);
  SetRXASBNRreductionAmount(rx->channel, (float)rx->nr4_reduction);
  SetRXASBNRsmoothingFactor(rx->channel, (float)rx->nr4_smoothing);
  SetRXASBNRwhiteningFactor(rx->channel, (float)rx->nr4_whitening);
  SetRXASBNRnoiseRescale(rx->channel, (float)rx->nr4_rescale);
  SetRXASBNRpostFilterThreshold(rx->channel, (float)rx->nr4_postfilter);
  SetRXAANFRun(rx->channel, bypass ? 0 : rx->anf);
  SetRXASNBARun(rx->channel, bypass ? 0 : rx->snb);

  // Apply any restored manual notches now that the WDSP channel exists.
  receiver_notch_sync(rx);

  /* Restore freetune WDSP shift state after channel open */
  if(rx->freetune || rx->ctun) {
    SetRXAShiftRun(rx->channel, 1);
  }
  /* The LO starts on the current centre; seed the freetune retune tracker. */
  rx->freetune_hw_frequency = rx->frequency_a;

  int result;
  XCreateAnalyzer(rx->channel, &result, WDSP_ANALYZER_MAX_SIZE, 1, 1, "");
  if(result != 0) {
    log_info("XCreateAnalyzer channel=%d failed: %d\n", rx->channel, result);
  } else {
    receiver_init_analyzer(rx);
  }

  SetDisplayDetectorMode(rx->channel, 0, rx->display_detector_mode);
  SetDisplayAverageMode(rx->channel, 0, rx->display_average_mode);
  calculate_display_average(rx);

  /* Apply the saved broadcast-FM de-emphasis (WDSP defaults to 50 us). */
  SetRXAWFMDeemphasisTau(rx->channel, radio->wfm_deemphasis ? 75.0e-6 : 50.0e-6);

  if (!rx->show_rx) return rx;

  create_visual(rx);
  // GTK4: children are visible by default; no gtk_widget_show_all.
  radio->active_receiver=rx;
#ifdef FT8
  // Mode was applied (receiver_mode_changed) before create_visual built the box,
  // so sync the FT8 waterfall now (it appears only if DIGU + panel already open).
  receiver_ft8_waterfall_sync(rx);
#endif
  if(rx->vpaned!=NULL) {
    // Always position the panadapter/waterfall split once the vpaned has a real
    // height (see restore_paned_position_cb): it uses the saved fraction, or an
    // even 0.5 fallback when that is missing/degenerate. Gating this on a
    // positive saved percent (as before) left a 0 / never-saved split at GTK4's
    // default, which collapsed the panadapter (spectroscope invisible).
    rx->paned_restore_timer=g_timeout_add(100,restore_paned_position_cb,(gpointer)rx);
  }
  // Honour a saved "spectroscope off": hide the panadapter so the waterfall
  // takes the whole pane (the restore above no-ops while hidden).
  receiver_apply_panadapter_visibility(rx);
  update_frequency(rx);

  // Poll the display faster than WDSP produces frames (production ~= fps, tied to
  // the analyzer overlap). Polling AT fps aliased against the ~fps production and
  // caught new frames at irregular 40/80 ms intervals -> the waterfall juddered
  // unevenly. Oversampling (3x) catches each frame promptly, so the scroll
  // advances at the production cadence (regular, since the RX feed is real-time).
  int poll_ms=1000/(3*rx->fps); if(poll_ms<8) poll_ms=8;
  rx->update_timer_id=g_timeout_add(poll_ms,update_timer_cb,(gpointer)rx);


  if(rx->local_audio) {
    if(audio_open_output(rx)<0) {
      rx->local_audio=FALSE;
    }
  }

  if(rx->bpsk_enable) {
    rx->bpsk=create_bpsk(BPSK_BASE_CHANNEL+rx->channel,rx->band_a);
  }

  // Sub-RX was persisted on: its WDSP sub-channel needs the main channel open,
  // so (re)create it now that OpenChannel above (and frequency_changed/
  // receiver_mode_changed) have run with subrx_enable still FALSE. Only now is
  // it safe to open the sub-channel and flip the live flag on.
  if(rx->subrx_restore_pending && rx->subrx==NULL) {
    rx->subrx_restore_pending=FALSE;
    if(rx->mode_b<0 || rx->mode_b>=MODES) rx->mode_b=rx->mode_a;
    create_subrx(rx);        // sets rx->subrx
    rx->subrx_enable=TRUE;
  }

  if(rx->rigctl_enable) {
    launch_rigctl(rx);
  }

  if(rx->rigctl_serial_enable) {
    launch_serial(rx);
  }

  return rx;
}

void receiver_set_volume(RECEIVER *rx) {
  SetRXAPanelGain1(rx->channel, rx_panel_gain(rx));
  if(rx->subrx_enable) {
    subrx_volume_changed(rx);
  }
}

void receiver_set_agc_gain(RECEIVER *rx) {
  SetRXAAGCTop(rx->channel, rx->agc_gain);
}

void receiver_set_ctun(RECEIVER *rx) {
  rx->ctun_offset=0;
  rx->ctun_frequency=rx->frequency_a;
  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
  rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);
  if(!rx->ctun && !rx->freetune) {
    SetRXAShiftRun(rx->channel, 0);
  } else {
    SetRXAShiftRun(rx->channel, 1);
  }
  frequency_changed(rx);
  update_frequency(rx);
}
