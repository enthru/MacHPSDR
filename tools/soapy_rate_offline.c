/* Headless live-rate integration test. Uses only machpsdrnull, never hardware.
 * RX's GTK/decoder endpoint is a sample counter; TX uses the real WDSP channel
 * and transmitter rate/buffer rebuild. See run-soapy-rate-test.sh.
 */
#include <assert.h>
#include <stdarg.h>
#include "../src/proto/soapy_protocol.c"
RADIO *radio;
static gint delivered[MAX_RECEIVERS];
gboolean isTransmitting(RADIO *r) { return r->mox; }
gboolean receiver_is_live(RECEIVER *rx) { return rx && radio->receiver[rx->channel]==rx; }
long long radio_ppm_correction(long long f) { return 0; }
void receiver_change_sample_rate(RECEIVER *rx,int rate) {
  for(int c=0;c<MAX_CHANNELS;c++) assert(receive_thread_id[c]==NULL && dsp_thread_id[c]==NULL);
  rx->sample_rate=rate;
  soapy_protocol_change_sample_rate(rx,rate);
}
void add_iq_samples(RECEIVER *rx,double i,double q) { g_atomic_int_inc(&delivered[rx->channel]); }
void frequency_changed(RECEIVER *rx) { soapy_protocol_set_rx_frequency(rx); }
void reconnect_note_data(void) {}
int audio_open_input(RADIO *r) { return 0; }
void audio_close_input(RADIO *r) {}
int main(void) {
  RADIO r={0}; DISCOVERED d={0}; TRANSMITTER tx={0}; RECEIVER a={0},b={0},c={0};
  radio=&r; r.discovered=&d; r.sample_rate=2304000; r.soapy_adc_rate_default=r.sample_rate;
  strcpy(d.name,"machpsdrnull"); d.device=DEVICE_SOAPYSDR; d.protocol=PROTOCOL_SOAPYSDR;
  d.supported_receivers=4; d.info.soapy.rx_rate_max=61440000;
  r.receiver[1]=&a; r.receiver[2]=&b; r.receiver[3]=&c;
  a.channel=1; b.channel=2; c.channel=3; c.adc=1;
  a.sample_rate=b.sample_rate=c.sample_rate=1920000;
  a.fft_size=b.fft_size=c.fft_size=2048;
  a.frequency_a=b.frequency_a=c.frequency_a=100000000;
  char *rxants[]={"RX"}; char *txants[]={"TX"};
  d.info.soapy.rx_antenna=rxants; d.info.soapy.tx_antenna=txants;
  r.can_transmit=TRUE; r.transmitter=&tx; tx.channel=7; tx.rx=&a;
  tx.mic_sample_rate=48000; tx.mic_dsp_rate=96000; tx.iq_output_rate=2304000;
  tx.buffer_size=1024; tx.output_samples=49152; tx.fft_size=2048;
  tx.iq_output_buffer=g_new0(double,2*tx.output_samples);
  SetDSPMult(2); OpenChannel(7,1024,2048,48000,96000,2304000,1,0,.010,.025,0,.010,0);
  soapy_device=SoapySDRDevice_makeStrArgs("driver=machpsdrnull,rx=2,tx=1,shared_rate=1,fail_rx_setup_rate=5760000"); assert(soapy_device);
  assert(slot_add(&a)); assert(slot_add(&b)); assert(slot_add(&c));
  assert(soapy_rate_start_streams());
  int rates[]={4800000,9600000,2304000,3072000,0};
  for(int pass=0;pass<2;pass++) for(int i=0;i<5;i++) {
    GError *err=NULL; assert(soapy_protocol_set_device_rate(rates[i],&err)); assert(!err);
    assert(r.sample_rate==(rates[i]?rates[i]:2304000)); assert(tx.iq_output_rate==r.sample_rate);
    // Exercise the resized output buffer with real TX DSP. No TX stream is
    // activated: the tone is measured in memory and cannot leave the process.
    double mic[2048]={0};
    SetTXAPostGenRun(7,1); SetTXAPostGenMode(7,0);
    SetTXAPostGenToneMag(7,0.25); SetTXAPostGenToneFreq(7,1000.0);
    SetChannelState(7,1,0);
    double energy=0.0;
    for(int block=0;block<8;block++) {
      int error=0;
      g_usleep(21333);
      fexchange0(7,mic,tx.iq_output_buffer,&error);
      for(int n=0;n<2*tx.output_samples;n++) {
        assert(isfinite(tx.iq_output_buffer[n]));
        energy+=tx.iq_output_buffer[n]*tx.iq_output_buffer[n];
      }
    }
    assert(energy>1.0);
    SetChannelState(7,0,1);
    assert(adc_slot[0][0].rx==&a && adc_slot[0][1].rx==&b && adc_slot[1][0].rx==&c);
    gint before[4]; for(int n=1;n<4;n++) before[n]=g_atomic_int_get(&delivered[n]);
    g_usleep(150000);
    for(int n=1;n<4;n++) assert(g_atomic_int_get(&delivered[n])>before[n]);
    if(r.sample_rate>=9600000) {
      // Exercise an actual front-end span increase, then its automatic clamp.
      g_mutex_lock(&r.delete_rx_mutex);
      a.sample_rate=9600000;
      soapy_protocol_change_sample_rate_locked(&a,a.sample_rate);
      g_mutex_unlock(&r.delete_rx_mutex);
    }
    assert(a.sample_rate<=r.sample_rate);
  }
  GError *err=NULL; r.mox=TRUE;
  assert(!soapy_protocol_set_device_rate(4800000,&err)); g_clear_error(&err); r.mox=FALSE;
  assert(r.sample_rate==2304000);
  // Null's hard limit is 20 MHz: it silently substitutes, so this must roll back.
  assert(!soapy_protocol_set_device_rate(30720000,&err)); assert(err); g_clear_error(&err);
  assert(r.sample_rate==2304000 && tx.iq_output_rate==2304000);
  assert(rx_stream[0] && rx_stream[1] && tx_stream);
  // Failure AFTER TX was rebuilt, with a span that had to shrink: rollback
  // must restore both geometries and resume all RX threads, without exiting.
  assert(soapy_protocol_set_device_rate(9600000,&err));
  g_mutex_lock(&r.delete_rx_mutex);
  a.sample_rate=9600000;
  soapy_protocol_change_sample_rate_locked(&a,a.sample_rate);
  g_mutex_unlock(&r.delete_rx_mutex);
  assert(!soapy_protocol_set_device_rate(5760000,&err)); assert(err); g_clear_error(&err);
  assert(r.sample_rate==9600000 && r.soapy_adc_rate==9600000);
  assert(a.sample_rate==9600000 && tx.iq_output_rate==9600000);
  gint before=g_atomic_int_get(&delivered[1]);
  g_usleep(150000);
  assert(g_atomic_int_get(&delivered[1])>before);
  soapy_rate_stop_streams(); SoapySDRDevice_unmake(soapy_device); soapy_device=NULL;
  for(int ch=0;ch<MAX_CHANNELS;ch++) for(int i=0;i<MAX_ADC_RECEIVERS;i++) {
    RXSLOT *sl=&adc_slot[ch][i];
    if(sl->rx==NULL) continue;
    if(sl->rx->resampler!=NULL) destroy_resample(sl->rx->resampler);
    g_free(sl->rx->buffer); g_free(sl->rx->resampled_buffer); slot_dsp_free(sl);
  }
  CloseChannel(7);
  g_free(tx.iq_output_buffer); g_free(tx.inI); g_free(tx.inQ); g_free(tx.outMI); g_free(tx.outMQ);
  puts("live device-rate test: PASS (shared RX, two ADCs, TX DSP, decrease, TX veto, rollback)");
  return 0;
}
