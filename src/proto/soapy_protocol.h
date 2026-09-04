/* Copyright (C)
* 2015 - John Melton, G0ORX/N6LYT
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

#ifndef _LIME_PROTOCOL_H
#define _LIME_PROTOCOL_H

#define BUFFER_SIZE 1024

SoapySDRDevice *get_soapy_device(void);

/* TRUE for a device whose radio->sample_rate is the widest span OFFERED rather
   than a fixed ADC rate (see soapy_hw_rate_for in soapy_protocol.c). */
gboolean soapy_span_driven(void);

/* The rate the TX DAC should run at.  Not radio->sample_rate: see the comment
   on the definition. */
int soapy_tx_dac_rate(void);

void soapy_protocol_create_receiver(RECEIVER *rx);
void soapy_protocol_start_receiver(RECEIVER *rx);
// Stop one receiver's receive thread and close its RX stream.  Call with
// radio->delete_rx_mutex NOT held -- see the note at the definition.
void soapy_protocol_stop_receiver(RECEIVER *rx);
// TRUE when this receiver is the one its hardware RX channel is tuned to.  More
// than one receiver can listen to one channel (a Pluto has a single RX with a
// single LO); the others follow, with an NCO of their own inside the owner's
// window.
gboolean soapy_protocol_rx_owns_hardware(RECEIVER *rx);
// Hz a following receiver's centre must move to fit inside that window; 0 when
// it already does, and 0 for a receiver that owns its channel.
long long soapy_protocol_rx_window_error(RECEIVER *rx);

void soapy_protocol_init(RADIO *r,int rx);
void soapy_protocol_stop(void);
gboolean soapy_protocol_reconnect(RECEIVER *rx);
void soapy_protocol_set_rx_frequency(RECEIVER *rx);
void soapy_protocol_set_rx_antenna(RECEIVER *rx,int ant);
void soapy_protocol_set_lna_gain(RECEIVER *rx,int gain);
void soapy_protocol_set_gain(ADC *adc);
void soapy_protocol_set_attenuation(RECEIVER *rx,int attenuation);
void soapy_protocol_change_sample_rate(RECEIVER *rx,int rate);
/* Same, for a caller that already holds radio->delete_rx_mutex (it must be
   taken before rx->mutex -- see the note on the definition). */
void soapy_protocol_change_sample_rate_locked(RECEIVER *rx,int rate);
gboolean soapy_protocol_get_automatic_gain(ADC *adc);
void soapy_protocol_set_automatic_gain(RECEIVER *rx,gboolean mode);
void soapy_protocol_create_transmitter(TRANSMITTER *tx);
void soapy_protocol_start_transmitter(TRANSMITTER *tx);
void soapy_protocol_stop_transmitter(TRANSMITTER *tx);
void soapy_protocol_rx_pause(void);
void soapy_protocol_rx_resume(void);
void soapy_protocol_activate_tx(TRANSMITTER *tx);
void soapy_protocol_deactivate_tx(TRANSMITTER *tx);
void soapy_protocol_set_tx_drive(double drive);
void soapy_protocol_set_tx_backoff(double db);
void soapy_protocol_set_tx_frequency(TRANSMITTER *tx);
void soapy_protocol_set_tx_antenna(TRANSMITTER *tx,int ant);
void soapy_protocol_set_tx_gain(DAC *dac);
void soapy_protocol_process_local_mic(RADIO *r);
void soapy_protocol_iq_samples(float isample,float qsample);
void soapy_protocol_set_mic_sample_rate(int rate);
char *soapy_protocol_read_sensor(char *name);
gboolean soapy_protocol_is_running(void);
#endif
