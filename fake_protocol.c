/* Fake SDR protocol - synthetic noise+tones IQ generator for UI/design testing.
 *
 * No hardware is involved. fake_discovery() appends a synthetic device to the
 * global discovered[] list. fake_protocol_init() starts a background thread that
 * continuously feeds each active receiver a synthetic IQ stream (Gaussian-ish
 * white noise plus a handful of fixed baseband tones) via add_iq_samples(), so
 * the panadapter/waterfall have something to render.
 *
 * IQ amplitude levels are tunable via the #defines below. If the panadapter
 * looks flat, raise FAKE_NOISE_AMP / the tone amplitudes; if it clips, lower them.
 */

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "discovered.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "fake_protocol.h"

#define FAKE_NOISE_AMP  0.0009    // noise floor level
#define FAKE_TONE_COUNT 4

// Synthetic TX: a quiet audio test tone fed to the transmitter's mic input
// while keyed (MOX/Tune), so the tx_panadapter shows a signal without hardware.
#define FAKE_TX_TONE_AMP  0.10     // mic tone amplitude (0..1); lower if it clips
#define FAKE_TX_TONE_FREQ 1000.0   // mic tone frequency (Hz)

static const double fake_tone_offset[FAKE_TONE_COUNT] = { 10000.0, -25000.0, 40000.0, -5000.0 };
static const double fake_tone_amp[FAKE_TONE_COUNT]    = {    0.02,     0.008,    0.03,   0.015 };

static volatile int fake_running = 0;
static GThread *fake_thread_id = NULL;

void fake_discovery(void) {
  if(devices >= MAX_DEVICES) return;

  DISCOVERED *d = &discovered[devices];
  memset(d, 0, sizeof(DISCOVERED));

  d->protocol = PROTOCOL_FAKE;
  d->device = DEVICE_HERMES;
  strcpy(d->name, "Fake Noise SDR");
  strcpy(d->software_version, "1.0");
  d->status = STATE_AVAILABLE;
  d->supported_receivers = 2;
  d->supported_transmitters = 1;
  d->adcs = 1;
  d->frequency_min = 0.0;
  d->frequency_max = 61440000.0;

  unsigned char mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  memcpy(d->info.network.mac_address, mac, 6);
  d->info.network.address_length = 6;
  strcpy(d->info.network.interface_name, "fake");

  devices++;
}

static gpointer fake_thread_fn(gpointer data) {
  RADIO *r = (RADIO *)data;
  static double phase[FAKE_TONE_COUNT] = {0.0};
  static double mic_phase = 0.0;

  while(fake_running) {
    int any = 0;

    for(int ch = 0; ch < r->discovered->supported_receivers; ch++) {
      g_mutex_lock(&r->delete_rx_mutex);
      RECEIVER *rx = r->receiver[ch];
      if(rx == NULL || !rx->show_rx) {
        g_mutex_unlock(&r->delete_rx_mutex);
        continue;
      }
      any = 1;

      int n = rx->buffer_size;
      double sr = (double)rx->sample_rate;

      for(int s = 0; s < n; s++) {
        // Gaussian-ish white noise via sum of 12 uniforms (mean 0)
        double ni = 0.0;
        double nq = 0.0;
        for(int k = 0; k < 12; k++) {
          ni += (double)rand() / (double)RAND_MAX;
          nq += (double)rand() / (double)RAND_MAX;
        }
        ni = (ni - 6.0) * FAKE_NOISE_AMP;
        nq = (nq - 6.0) * FAKE_NOISE_AMP;

        double i_sample = ni;
        double q_sample = nq;

        // Fixed baseband tones -> distinct peaks on the panadapter
        for(int t = 0; t < FAKE_TONE_COUNT; t++) {
          i_sample += fake_tone_amp[t] * cos(phase[t]);
          q_sample += fake_tone_amp[t] * sin(phase[t]);
          phase[t] += 2.0 * M_PI * fake_tone_offset[t] / sr;
          if(phase[t] > 2.0 * M_PI) phase[t] -= 2.0 * M_PI;
          if(phase[t] < -2.0 * M_PI) phase[t] += 2.0 * M_PI;
        }

        add_iq_samples(rx, i_sample, q_sample);
      }

      // While keyed, feed the TX DSP a synthetic mic tone so tx_panadapter
      // shows a signal. Only for the receiver bound to the transmitter, paced
      // with this receiver's real-time block (num_mic scales rx rate -> mic rate).
      if(r->can_transmit && r->transmitter != NULL &&
         r->transmitter->rx == rx && isTransmitting(r)) {
        TRANSMITTER *tx = r->transmitter;
        int num_mic = (int)((double)tx->mic_sample_rate * (double)n / sr);
        for(int m = 0; m < num_mic; m++) {
          float mic = (float)(FAKE_TX_TONE_AMP * sin(mic_phase));
          mic_phase += 2.0 * M_PI * FAKE_TX_TONE_FREQ / (double)tx->mic_sample_rate;
          if(mic_phase > 2.0 * M_PI) mic_phase -= 2.0 * M_PI;
          add_mic_sample(tx, mic);
        }
      }

      g_mutex_unlock(&r->delete_rx_mutex);

      // Pace to roughly real time for this receiver's block (lock released)
      g_usleep((gulong)(1000000.0 * (double)n / sr));
    }

    if(!any) g_usleep(10000);
  }

  return NULL;
}

void fake_protocol_init(RADIO *r) {
  fake_running = 1;
  fake_thread_id = g_thread_new("fake_iq", fake_thread_fn, r);
}

void fake_protocol_stop(void) {
  fake_running = 0;
}
