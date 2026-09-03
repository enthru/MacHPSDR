/* Copyright (C)
 * 2026 - MacHPSDR fork
 *
 * Radio-independent helpers for the TCI binary stream header. Kept in a
 * header so the offline harness can test the units without linking tci.c.
 */

#ifndef TCI_STREAM_H
#define TCI_STREAM_H

#include <stddef.h>
#include <stdint.h>

// Stream.length is the total number of real samples (interleaved floats), not
// the byte count and not the number of frames per channel. Some clients pad
// their WebSocket message beyond that declared data, so never consume it.
static inline size_t tci_stream_float_count(uint32_t declared, size_t payload_bytes) {
  size_t available = payload_bytes / sizeof(float);
  return (size_t)declared < available ? (size_t)declared : available;
}

// Convert the duration represented by an interleaved stream block to samples
// on the mono microphone clock. For example, 2048 samples / 2 channels at
// 48 kHz occupy 1024 samples on a 48 kHz mono clock.
static inline int tci_stream_mic_samples(uint32_t length, uint32_t channels,
                                         uint32_t stream_rate, uint32_t mic_rate) {
  if (length == 0 || channels == 0 || stream_rate == 0 || mic_rate == 0) return 0;
  uint64_t numerator = (uint64_t)length * mic_rate;
  uint64_t denominator = (uint64_t)channels * stream_rate;
  uint64_t samples = numerator / denominator;
  return samples > (uint64_t)INT32_MAX ? INT32_MAX : (int)samples;
}

#endif
