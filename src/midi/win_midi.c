/*  win_midi.c

    MIDI input for Windows, over winmm.  The counterpart of mac_midi.c
    (CoreMIDI) and alsa_midi.c (ALSA rawmidi).

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

/*
 * Only the transport differs from the other two backends; the decode is the
 * same three-message subset (Note On/Off, Controller, Pitch Bend) fed to
 * NewMidiEvent()/NewMidiConfigureEvent().
 *
 * Simpler than CoreMIDI in one respect and more delicate in another:
 *
 *   - winmm hands over a SHORT MESSAGE ALREADY PARSED into one DWORD (status,
 *     data1, data2), so the byte-at-a-time state machine mac_midi.c needs for
 *     the raw packet stream has nothing to do here.
 *
 *   - but the callback runs at interrupt time, where Microsoft documents that
 *     you may call almost nothing — certainly not back into winmm, and nothing
 *     that might block or allocate.  NewMidiEvent() walks the mapping table and
 *     dispatches actions, which is far more than that budget allows.  So the
 *     callback does one thing: drop the DWORD into a ring.  A thread of our own
 *     drains it and does the work, which is also the shape alsa_midi.c has.
 */

#include <gtk/gtk.h>
#include "log.h"
#include "discovered.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "midi.h"
#include "midi_dialog.h"   // NewMidiConfigureEvent()

#ifdef _WIN32

#include <windows.h>
#include <mmsystem.h>
#include <string.h>

typedef struct _midi_device {
  char *name;
  char *port;
} MIDI_DEVICE;

#define MAX_MIDI_DEVICES 10

MIDI_DEVICE midi_devices[MAX_MIDI_DEVICES];
int n_midi_devices;

static HMIDIIN midi_handle = NULL;
static gboolean configure = FALSE;

// Ring between the interrupt-time callback and our thread.  A power of two so
// the wrap is a mask, and deliberately generous: a knob swept fast produces a
// burst of controller messages, and dropping those makes a control feel broken
// in a way that is very hard to attribute.
#define MIDI_RING 256
static volatile DWORD ring[MIDI_RING];
static volatile guint ring_head = 0;   // written by the callback
static volatile guint ring_tail = 0;   // written by the drain thread
static GThread *midi_thread_id = NULL;
static volatile gboolean running = FALSE;

static void dispatch(DWORD msg) {
  int status = msg & 0xFF;
  int chan   = status & 0x0F;
  int arg1   = (msg >> 8)  & 0x7F;
  int arg2   = (msg >> 16) & 0x7F;

  switch (status & 0xF0) {
    case 0x80:   // Note-OFF
      if (configure) NewMidiConfigureEvent(MIDI_NOTE, chan, arg1, 0);
      else           NewMidiEvent(MIDI_NOTE, chan, arg1, 0);
      break;
    case 0x90:   // Note-ON.  Velocity 0 is how several controllers (Hercules
                 // among them) report a push-button being RELEASED, so it is a
                 // note-off — see the same case in mac_midi.c.
      if (configure) NewMidiConfigureEvent(MIDI_NOTE, chan, arg1, arg2 ? 1 : 0);
      else           NewMidiEvent(MIDI_NOTE, chan, arg1, arg2 ? 1 : 0);
      break;
    case 0xB0:   // Controller change
      if (configure) NewMidiConfigureEvent(MIDI_CTRL, chan, arg1, arg2);
      else           NewMidiEvent(MIDI_CTRL, chan, arg1, arg2);
      break;
    case 0xE0:   // Pitch bend: 14 bits, LSB first
      if (configure) NewMidiConfigureEvent(MIDI_PITCH, chan, 0, arg1 + 128 * arg2);
      else           NewMidiEvent(MIDI_PITCH, chan, 0, arg1 + 128 * arg2);
      break;
    default:
      // Polyphonic pressure, program change, channel pressure, system: the
      // other backends drop these too.
      break;
  }
}

static gpointer midi_thread(gpointer data) {
  while (running) {
    guint tail = ring_tail;
    if (tail == ring_head) {
      // Idle. A 20 ms nap is far below the reaction time of a hand on a knob
      // and costs nothing; polling avoids a condition variable that the
      // interrupt-time callback is not allowed to signal.
      g_usleep(20000);
      continue;
    }
    while (tail != ring_head && running) {
      DWORD msg = ring[tail & (MIDI_RING - 1)];
      tail++;
      ring_tail = tail;
      dispatch(msg);
    }
  }
  return NULL;
}

static void CALLBACK midi_in_proc(HMIDIIN h, UINT msg, DWORD_PTR instance,
                                  DWORD_PTR p1, DWORD_PTR p2) {
  guint head, next;
  if (msg != MIM_DATA) return;   // MIM_LONGDATA is sysex; nothing here wants it

  head = ring_head;
  next = head + 1;
  // Full: drop the newest rather than overwrite the oldest, so a burst loses
  // its tail instead of scrambling the order the drain thread sees.
  if ((next - ring_tail) > MIDI_RING) return;
  ring[head & (MIDI_RING - 1)] = (DWORD)p1;
  ring_head = next;
}

// The winmm device id each list entry came from.  NOT the list index: the loop
// below skips a device whose caps cannot be read, and every skip shifts the
// index of the ones after it -- so midiInOpen() would have opened a different
// device than the operator picked.
static UINT midi_dev_id[MAX_MIDI_DEVICES];

void get_midi_devices(void) {
  UINT n = midiInGetNumDevs();
  UINT i;

  // The dialog re-enumerates on every open and these names are allocated.
  for (i = 0; i < (UINT)n_midi_devices; i++) {
    g_free(midi_devices[i].name);
    midi_devices[i].name = NULL;
  }
  n_midi_devices = 0;
  for (i = 0; i < n && n_midi_devices < MAX_MIDI_DEVICES; i++) {
    MIDIINCAPSA caps;
    if (midiInGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
    log_info("%s: %s\n", __FUNCTION__, caps.szPname);
    midi_devices[n_midi_devices].name = g_strdup(caps.szPname);
    midi_devices[n_midi_devices].port = NULL;
    midi_dev_id[n_midi_devices] = i;
    n_midi_devices++;
  }
  log_info("%s: devices=%d\n", __FUNCTION__, n_midi_devices);
}

int register_midi_device(char *myname) {
  int found = -1;
  int i;
  int mylen = (int)strlen(myname);
  MMRESULT rc;

  configure = FALSE;
  log_info("%s: %s\n", __FUNCTION__, myname);

  // midi_dev_id[] carries the winmm id this entry was enumerated from; the list
  // index is not it (get_midi_devices() skips devices whose caps fail).
  for (i = 0; i < n_midi_devices; i++) {
    if (!strncmp(midi_devices[i].name, myname, mylen)) {
      found = i;
      log_info("MIDI device found and selected: >>>%s<<<\n", midi_devices[i].name);
    }
  }
  if (found < 0) return -1;

  close_midi_device();   // idempotent; a re-register must not leak the old handle

  rc = midiInOpen(&midi_handle, midi_dev_id[found], (DWORD_PTR)midi_in_proc,
                  0, CALLBACK_FUNCTION);
  if (rc != MMSYSERR_NOERROR) {
    log_error("%s: midiInOpen failed: %u\n", __FUNCTION__, (unsigned)rc);
    midi_handle = NULL;
    return -1;
  }

  ring_head = ring_tail = 0;
  running = TRUE;
  midi_thread_id = g_thread_new("MIDI in", midi_thread, NULL);

  rc = midiInStart(midi_handle);
  if (rc != MMSYSERR_NOERROR) {
    log_error("%s: midiInStart failed: %u\n", __FUNCTION__, (unsigned)rc);
    close_midi_device();
    return -1;
  }
  return 0;
}

void close_midi_device(void) {
  log_info("%s\n", __FUNCTION__);

  if (midi_handle != NULL) {
    // Stop and reset BEFORE close: midiInClose refuses with MIDIERR_STILLPLAYING
    // while the device is running, and a handle left open holds the port against
    // every other application on the machine.
    midiInStop(midi_handle);
    midiInReset(midi_handle);
    midiInClose(midi_handle);
    midi_handle = NULL;
  }

  if (midi_thread_id != NULL) {
    running = FALSE;
    g_thread_join(midi_thread_id);
    midi_thread_id = NULL;
  }
}

void configure_midi_device(gboolean state) {
  configure = state;
}

#endif /* _WIN32 */
