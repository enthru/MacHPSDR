# find what system we are running on
UNAME_S := $(shell uname -s)

# Get git commit version and date
GIT_DATE := $(firstword $(shell git --no-pager show --date=short --format="%ai" --name-only))
GIT_VERSION := $(shell git describe --abbrev=0 --tags 2>/dev/null || echo "unknown")

CC=gcc
LINK=gcc

# GTK4 migration (branch gtk4-migration): this fork targets GTK4 only.
# The stock upstream builds against gtk+-3.0; here we link gtk4.
GTKINCLUDES=`pkg-config --cflags gtk4`
GTKLIBS=`pkg-config --libs gtk4`

#OPENGL_OPTIONS=-D OPENGL
#OPENGL_INCLUDES=`pkg-config --cflags epoxy`
#OPENGL_LIBS=`pkg-config --libs epoxy`

ifeq ($(UNAME_S), Linux)
AUDIO_LIBS=-lasound -lpulse-simple -lpulse -lpulse-mainloop-glib -lsoundio
AUDIO_SOURCES=audio.c
AUDIO_HEADERS=audio.h
endif
ifeq ($(UNAME_S), Darwin)
AUDIO_LIBS=-lsoundio -framework CoreAudio
AUDIO_SOURCES=portaudio.c
AUDIO_HEADERS=portaudio.h
# Homebrew prefix: /opt/homebrew on Apple Silicon, /usr/local on Intel.
# Used by the `app` target to locate GTK resources for bundling.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
endif

# uncomment the line below to include SoapySDR support
#
# Note: SoapySDR support has only been tested with the RTL-SDR and LimeSDR
#       No TX support yet.
#
#       If you want to build with SoapySDR support you will need to install:
#
#       sudo apt-get install libsoapysdr-dev
#	sudo apt-get install soapysdr-module-rtlsdr
#	sudo apt-get install soapysdr-module-lms7
#
SOAPYSDR_INCLUDE=SOAPYSDR

ifeq ($(SOAPYSDR_INCLUDE),SOAPYSDR)
SOAPYSDR_OPTIONS=-D SOAPYSDR
SOAPYSDR_LIBS=-lSoapySDR
SOAPYSDR_SOURCES= \
soapy_discovery.c \
soapy_protocol.c
SOAPYSDR_HEADERS= \
soapy_discovery.h \
soapy_protocol.h
SOAPYSDR_OBJS= \
soapy_discovery.o \
soapy_protocol.o
endif

# PureSignal adaptive distortion for HPSDR radios
# (currently only protocol1)
PURESIGNAL_INCLUDE=PURESIGNAL

ifeq ($(PURESIGNAL_INCLUDE),PURESIGNAL)
PURESIGNAL_OPTIONS=-D PURESIGNAL
PURESIGNAL_SOURCES= \
puresignal.c
PURESIGNAL_HEADERS= \
puresignal.h
PURESIGNAL_OBJS= \
puresignal.o
endif

# PureSignal for Protocol 2 (feedback-DDC path).  EXPERIMENTAL / UNVERIFIED:
# it compiles and is wired end-to-end (two synced feedback DDCs -> pscc()), but
# the closed predistortion loop has NEVER been run against a real Protocol-2
# radio with a feedback ADC, so convergence is unproven (see the big comment in
# protocol2.c).  Requires PURESIGNAL above (shared machinery lives there).
# Enabled so the P2 controls are live; the runtime path stays inert unless the
# operator turns PureSignal on with a P2 radio connected. Comment out to drop it.
PURESIGNAL_P2_INCLUDE=PURESIGNAL_P2
ifeq ($(PURESIGNAL_P2_INCLUDE),PURESIGNAL_P2)
PURESIGNAL_OPTIONS+=-D PURESIGNAL_P2
endif

# FT8 receive decoder (and, later, TX).  Uses the vendored ft8_lib (MIT) under
# ft8_lib/.  Comment out FT8_INCLUDE to build without FT8 support.
FT8_INCLUDE=FT8

ifeq ($(FT8_INCLUDE),FT8)
FT8_OPTIONS=-D FT8
FT8_INCLUDES=-Ift8_lib
FT8_SOURCES= \
ft8_decoder.c ft8_encoder.c ft8_qso.c ft8_panel.c ft8_dialog.c ft8_udp.c ft8_pskreporter.c ft8_waterfall.c ft8_dxcc.c \
ft8_lib/ft8/constants.c ft8_lib/ft8/crc.c ft8_lib/ft8/decode.c \
ft8_lib/ft8/encode.c ft8_lib/ft8/ldpc.c ft8_lib/ft8/message.c ft8_lib/ft8/text.c \
ft8_lib/fft/kiss_fft.c ft8_lib/fft/kiss_fftr.c \
ft8_lib/common/monitor.c
FT8_HEADERS= \
ft8_decoder.h ft8_encoder.h ft8_qso.h ft8_panel.h ft8_dialog.h ft8_udp.h ft8_pskreporter.h ft8_waterfall.h ft8_dxcc.h
FT8_OBJS= \
ft8_decoder.o ft8_encoder.o ft8_qso.o ft8_panel.o ft8_dialog.o ft8_udp.o ft8_pskreporter.o ft8_waterfall.o ft8_dxcc.o \
ft8_lib/ft8/constants.o ft8_lib/ft8/crc.o ft8_lib/ft8/decode.o \
ft8_lib/ft8/encode.o ft8_lib/ft8/ldpc.o ft8_lib/ft8/message.o ft8_lib/ft8/text.o \
ft8_lib/fft/kiss_fft.o ft8_lib/fft/kiss_fftr.o \
ft8_lib/common/monitor.o
endif

# SSTV receive decoder (analogue image, Scottie/Martin) + WEFAX / HF radiofax
# receive decoder.  Self-contained (their own Hilbert-transform FM discriminator;
# no external DSP dependency).  Comment out SSTV_INCLUDE to build without SSTV /
# WEFAX support.
SSTV_INCLUDE=SSTV

ifeq ($(SSTV_INCLUDE),SSTV)
SSTV_OPTIONS=-D SSTV
SSTV_SOURCES= sstv_decoder.c sstv_encoder.c sstv_panel.c wefax_decoder.c wefax_panel.c cw_decoder.c cw_panel.c cw_encoder.c cw_keyer.c apt_decoder.c apt_geo.c apt_coast.c apt_map.c apt_panel.c
SSTV_HEADERS= sstv_decoder.h sstv_encoder.h sstv_panel.h wefax_decoder.h wefax_panel.h cw_decoder.h cw_panel.h cw_encoder.h cw_keyer.h apt_decoder.h apt_geo.h apt_coast.h apt_map.h apt_panel.h
SSTV_OBJS= sstv_decoder.o sstv_encoder.o sstv_panel.o wefax_decoder.o wefax_panel.o cw_decoder.o cw_panel.o cw_encoder.o cw_keyer.o apt_decoder.o apt_geo.o apt_coast.o apt_map.o apt_panel.o image_save.o

# APT georeferencing needs an orbit propagator: the vendored SGP4/SDP4 tree
# builds into its own archive through its own Makefile (like wdsp/ and
# hfdl_lib/asn1/) — upstream code that must not be rebuilt with our warning
# flags nor carried in the flat OBJS list.  See sgp4sdp4/README-MACHPSDR.
SGP4_INCLUDES=-Isgp4sdp4
SGP4_LIB=sgp4sdp4/libsgp4sdp4.a
endif

# HFDL (aviation HF Data Link, ARINC 635) receive decoder — parity 4.5.
# Needs liquid-dsp (MIT; `brew install liquid-dsp` on macOS, `libliquid-dev` or
# a source build on Linux) and requires SSTV+FT8 (it reuses the shared
# decode-block machinery). Because the demodulator/framing is a port of dumphfdl
# (GPL-3.0), a build with HFDL is effectively GPLv3 — which this fork's "GPLv2 or
# later" permits. Comment the line out to build without it (drops the liquid-dsp
# dependency). On Linux, override HFDL_INCLUDES / HFDL_LIBS if liquid-dsp is not
# on the default include/lib path.
HFDL_INCLUDE=HFDL

ifeq ($(HFDL_INCLUDE),HFDL)
HFDL_OPTIONS=-D HFDL
# Vendored libfec (Phil Karn KA9Q, LGPL) under hfdl_lib/libfec — the r=1/2 K=7
# Viterbi decoder the HFDL FEC needs (built straight from its raw .c like
# ft8_lib/, no autotools).
HFDL_VENDOR_INCLUDES=-Ihfdl_lib/libfec -Ihfdl_lib -Ihfdl_lib/asn1
ifeq ($(UNAME_S), Darwin)
HFDL_INCLUDES=-I$(shell brew --prefix liquid-dsp)/include $(HFDL_VENDOR_INCLUDES)
HFDL_LIBS=-L$(shell brew --prefix liquid-dsp)/lib -lliquid -lz $(HFDL_ASN1_LIB)
endif
ifeq ($(UNAME_S), Linux)
HFDL_INCLUDES=$(HFDL_VENDOR_INCLUDES)
HFDL_LIBS=-lliquid -lz $(HFDL_ASN1_LIB)
endif
HFDL_SOURCES= hfdl_decoder.c hfdl_demod.c hfdl_fec.c hfdl_frame.c hfdl_msg.c hfdl_arinc.c hfdl_asn1.c hfdl_cpdlc.c hfdl_miam.c hfdl_ohma.c hfdl_util.c hfdl_pdu.c hfdl_panel.c hfdl_lib/libfec/viterbi27_port.c hfdl_lib/hfdl_crc.c hfdl_lib/vstring.c
HFDL_HEADERS= hfdl_decoder.h hfdl_demod.h hfdl_fec.h hfdl_frame.h hfdl_msg.h hfdl_arinc.h hfdl_asn1.h hfdl_cpdlc.h hfdl_miam.h hfdl_ohma.h hfdl_util.h hfdl_pdu.h hfdl_panel.h
HFDL_OBJS= hfdl_decoder.o hfdl_demod.o hfdl_fec.o hfdl_frame.o hfdl_msg.o hfdl_arinc.o hfdl_asn1.o hfdl_cpdlc.o hfdl_miam.o hfdl_ohma.o hfdl_util.o hfdl_pdu.o hfdl_panel.o hfdl_lib/libfec/viterbi27_port.o hfdl_lib/hfdl_crc.o hfdl_lib/vstring.o
# The FANS-1/A ASN.1 tree (asn1c output + runtime, ~240 sources) is built into
# its own archive by its own Makefile, like wdsp/ — it has no business in the
# flat OBJS list, and it must never be rebuilt with our warning flags.
HFDL_ASN1_LIB=hfdl_lib/asn1/libfansasn1.a
endif


ifeq ($(UNAME_S), Linux)
# cwdaemon support. Allows linux based logging software to key an Hermes/HermesLite2
# OPTIONAL and OFF by default: it needs the unixcw dev library (libcw.h / -lcw),
# which is not installed on a stock system, so enabling it by default would break
# a plain `make`. To use CW keying, install unixcw (see the "CW support" section
# in README.md) and uncomment the line below.
#			https://github.com/m5evt/unixcw-3.5.1.git

#CWDAEMON_INCLUDE=CWDAEMON

ifeq ($(CWDAEMON_INCLUDE),CWDAEMON)
CWDAEMON_OPTIONS=-D CWDAEMON
CWDAEMON_LIBS=-lcw
CWDAEMON_SOURCES= \
cwdaemon.c
CWDAEMON_HEADERS= \
cwdaemon.h
CWDAEMON_OBJS= \
cwdaemon.o
endif
endif

# MIDI code from piHPSDR written by Christoph van Wullen, DL1YCF.
MIDI_INCLUDE=MIDI

ifeq ($(MIDI_INCLUDE),MIDI)
MIDI_OPTIONS=-D MIDI
MIDI_HEADERS= midi.h midi_dialog.h
ifeq ($(UNAME_S), Darwin)
MIDI_SOURCES= mac_midi.c midi2.c midi3.c midi_dialog.c
MIDI_OBJS= mac_midi.o midi2.o midi3.o midi_dialog.o
MIDI_LIBS= -framework CoreMIDI -framework Foundation
endif
ifeq ($(UNAME_S), Linux)
MIDI_SOURCES= alsa_midi.c midi2.c midi3.c midi_dialog.c
MIDI_OBJS= alsa_midi.o midi2.o midi3.o midi_dialog.o
MIDI_LIBS= -lasound
endif
endif

# -std=gnu23: the codebase now uses explicit `void f(void)` prototypes
# everywhere (the K&R empty `()` prototypes it inherited from LinHPSDR were all
# converted), so C23's stricter reading of `()` as `(void)` — which is why the
# build was previously pinned to gnu11 — no longer breaks anything. The only
# remaining `()` are the two in vendored wdsp/wdsp.h (GetWDSPVersion,
# wisdom_get_status), both genuinely no-arg and only ever called with no args,
# so C23's ()->(void) is harmless there too.
#
# Warnings: -Wall -Wextra are on. The pervasive, idiomatic-noise categories are
# suppressed so the build stays clean and real warnings aren't buried:
#   -Wno-unused-parameter  GTK callbacks must match a fixed signature; hundreds
#                          legitimately ignore `data`/`widget`.
#   -Wno-unused-variable   many are config-gated (used only under a disabled
#                          #ifdef like PURESIGNAL_P2/CWDAEMON), so blind removal
#                          would break another build we can't verify here.
#   -Wno-sign-compare / -Wno-missing-field-initializers  benign int/size_t loop
#                          counters and partial `{0}` struct inits.
# Everything -Wall/-Wextra flags outside those (uninitialised use, bad function-
# pointer casts, dead functions, ...) is treated as a real finding and fixed.
#
# All deprecated GTK widget APIs (ComboBox/TreeView/Dialog families, etc.) have
# been migrated off, including the last holdout gdk_cairo_set_source_pixbuf: the
# waterfall / SSTV / WEFAX images now composite through the GtkSnapshot/GdkTexture
# GPU pipeline via the GpuImage widget (gpu_image.c). The build is therefore
# deprecation-clean and -Wno-deprecated-declarations is no longer needed.
CFLAGS= -g -O3 -std=gnu23 -Wall -Wextra \
        -Wno-unused-parameter -Wno-unused-variable \
        -Wno-sign-compare -Wno-missing-field-initializers
OPTIONS=  $(MIDI_OPTIONS) $(AUDIO_OPTIONS) $(PURESIGNAL_OPTIONS) $(SOAPYSDR_OPTIONS) \
          $(CWDAEMON_OPTIONS) $(OPENGL_OPTIONS) $(FT8_OPTIONS) $(SSTV_OPTIONS) $(HFDL_OPTIONS) \
          -D USE_VFO_B_MODE_AND_FILTER="USE_VFO_B_MODE_AND_FILTER" \
          -D GIT_DATE='"$(GIT_DATE)"' -D GIT_VERSION='"$(GIT_VERSION)"'

# WDSP: use the in-tree copy (./wdsp) rather than the system-installed library.
WDSP_DIR=wdsp
WDSP_LIB=$(WDSP_DIR)/libwdsp.dylib

ifeq ($(UNAME_S), Linux)
# Link against the in-tree ./wdsp (libwdsp.so, built by the wdsp-local target)
# and use its in-tree headers — NOT a system-wide WDSP. This fork patches WDSP
# (adds a WFM demodulator and other tweaks); a stock system libwdsp would build
# but break those features. So we do NOT `-lwdsp` from /usr/local and we do NOT
# require `sudo make install` of an upstream WDSP.
LIBS=-lrt -lm -lpthread -L$(WDSP_DIR) -lwdsp $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) $(CWDAEMON_LIBS) $(OPENGL_LIBS) $(MIDI_LIBS) $(HFDL_LIBS) $(SGP4_LIB)
WDSP_INCLUDE=-I$(WDSP_DIR)
# $ORIGIN lets the binary find ./wdsp/libwdsp.so relative to itself at run time,
# so `./machpsdr` runs straight from the repo with no WDSP install. ($$ -> $ for
# make; single-quoted so the shell passes $ORIGIN through to the linker literally.)
RPATH_FLAGS=-Wl,-rpath,'$$ORIGIN/$(WDSP_DIR)'
endif
ifeq ($(UNAME_S), Darwin)
# Link against ./wdsp/libwdsp.dylib (not /usr/local/lib) and use the in-tree header.
LIBS=-lm -lpthread -L$(WDSP_DIR) -lwdsp $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) $(MIDI_LIBS) $(HFDL_LIBS) $(SGP4_LIB)
WDSP_INCLUDE=-I$(WDSP_DIR)
# rpaths so the dylib (id @rpath/libwdsp.dylib) resolves both when running
# ./machpsdr from the repo (@loader_path/wdsp) and inside the .app (Frameworks).
RPATH_FLAGS=-Wl,-rpath,@loader_path/$(WDSP_DIR) -Wl,-rpath,@executable_path/../Frameworks
endif

# Source tree layout: the ~90 first-party .c/.h live under src/<subsystem>/ to
# keep the repo root uncluttered. The build stays path-agnostic: VPATH lets make
# resolve the bare foo.c names in OBJS/SOURCES from any of these dirs, and each
# dir is on the -I path so the flat `#include "radio.h"` style (all header
# basenames are unique) keeps working with no per-file edits. Objects still land
# in the root (gitignored *.o/*.d), so OBJS entries stay bare `foo.o` names.
SRCDIRS= src/core src/proto src/dsp src/audio src/midi src/ui src/decode
VPATH= $(SRCDIRS)
SRC_INCLUDES= $(addprefix -I,$(SRCDIRS))

INCLUDES=$(SRC_INCLUDES) $(GTKINCLUDES) $(PULSEINCLUDES) $(OPGL_INCLUDES) $(WDSP_INCLUDE) $(FT8_INCLUDES) $(HFDL_INCLUDES) $(SGP4_INCLUDES)

COMPILE=$(CC) $(CFLAGS) $(OPTIONS) $(INCLUDES)

# Feature flags are not tracked by make: switching one (e.g. commenting
# HFDL_INCLUDE in or out) leaves every existing .o looking up to date, so the
# feature links in but the code compiled without its -D stays absent — an HFDL
# build whose Decode menu has no HFDL entry, and no error anywhere. .build-flags
# records the current -D set and is rewritten only when that set actually
# changes, so a flag switch forces a rebuild and nothing else does.
# (A pattern rule, not the old `.c.o:` suffix rule — suffix rules cannot take
# extra prerequisites; adding one silently turns them into an ordinary target
# and nothing compiles at all.)
%.o: %.c .build-flags
	$(COMPILE) -MMD -MP -c -o $@ $<

PROGRAM=machpsdr

SOURCES=\
main.c\
log.c\
css.c\
audio.c\
version.c\
discovered.c\
discovery.c\
protocol1_discovery.c\
protocol2_discovery.c\
property.c\
mode.c\
filter.c\
band.c\
radio.c\
radio_state.c\
receiver.c\
receiver_state.c\
transmitter.c\
vfo.c\
meter.c\
rx_panadapter.c\
tx_panadapter.c\
mic_level.c\
mic_gain.c\
drive_level.c\
waterfall.c\
gpu_image.c\
wideband_panadapter.c\
wideband_waterfall.c\
protocol1.c\
fake_protocol.c\
protocol2.c\
reconnect.c\
radio_dialog.c\
cw_dialog.c\
receiver_dialog.c\
transmitter_dialog.c\
pa_dialog.c\
eer_dialog.c\
wideband_dialog.c\
about_dialog.c\
button_text.c\
wideband.c\
vox.c\
ext.c\
configure_dialog.c\
labels_dialog.c\
ppm_cal.c\
bookmark_dialog.c\
puresignal_dialog.c\
oc_dialog.c\
xvtr_dialog.c\
frequency.c\
error_handler.c\
radio_info.c\
diversity_mixer.c\
diversity_dialog.c\
rigctl.c \
rigctl_parse.c \
bpsk.c \
ringbuffer.c \
hl2.c \
level_meter.c \
tx_info.c \
tx_info_meter.c \
peak_detect.c \
subrx.c \
actions.c\
dxcluster.c\
cluster_dialog.c

HEADERS=\
main.h\
log.h\
css.h\
audio.h\
version.h\
discovered.h\
discovery.h\
protocol1_discovery.h\
protocol2_discovery.h\
property.h\
agc.h\
mode.h\
filter.h\
band.h\
radio.h\
receiver.h\
transmitter.h\
vfo.h\
meter.h\
rx_panadapter.h\
tx_panadapter.h\
mic_level.h\
mic_gain.h\
drive_level.h\
wideband_panadapter.h\
wideband_waterfall.h\
waterfall.h\
protocol1.h\
protocol2.h\
radio_dialog.h\
cw_dialog.h\
receiver_dialog.h\
transmitter_dialog.h\
pa_dialog.h\
eer_dialog.h\
wideband_dialog.h\
about_dialog.h\
button_text.h\
wideband.h\
vox.h\
ext.h\
configure_dialog.h\
labels_dialog.h\
ppm_cal.h\
bookmark_dialog.h\
puresignal_dialog.h\
oc_dialog.h\
xvtr_dialog.h\
frequency.h\
error_handler.h\
radio_info.h\
diversity_mixer.h\
diversity_dialog.h\
rigctl.h \
rigctl_internal.h \
bpsk.h \
ringbuffer.h \
hl2.h \
level_meter.h \
tx_info.h \
tx_info_meter.h \
peak_detect.h \
subrx.h \
actions.h\
dxcluster.h\
cluster_dialog.h

OBJS=\
main.o\
log.o\
css.o\
settings_ui.o\
audio.o\
version.o\
discovered.o\
discovery.o\
protocol1_discovery.o\
protocol2_discovery.o\
property.o\
mode.o\
filter.o\
band.o\
radio.o\
radio_state.o\
receiver.o\
receiver_state.o\
transmitter.o\
vfo.o\
meter.o\
rx_panadapter.o\
tx_panadapter.o\
mic_level.o\
mic_gain.o\
drive_level.o\
wideband_panadapter.o\
wideband_waterfall.o\
waterfall.o\
gpu_image.o\
pana_view.o\
protocol1.o\
fake_protocol.o\
protocol2.o\
reconnect.o\
radio_dialog.o\
cw_dialog.o\
receiver_dialog.o\
transmitter_dialog.o\
pa_dialog.o\
eer_dialog.o\
wideband_dialog.o\
about_dialog.o\
button_text.o\
wideband.o\
vox.o\
ext.o\
configure_dialog.o\
labels_dialog.o\
ppm_cal.o\
bookmark_dialog.o\
puresignal_dialog.o\
oc_dialog.o\
xvtr_dialog.o\
frequency.o\
error_handler.o\
radio_info.o\
diversity_mixer.o\
diversity_dialog.o\
rigctl.o \
rigctl_parse.o \
bpsk.o \
ringbuffer.o \
hl2.o \
level_meter.o \
tx_info.o \
tx_info_meter.o \
peak_detect.o \
subrx.o \
actions.o \
recorder.o \
waterfall_theme.o \
dxcluster.o \
cluster_dialog.o \
tci.o \
tci_dialog.o


$(PROGRAM): $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS) $(SSTV_OBJS) $(HFDL_OBJS)
	$(LINK) -o $(PROGRAM) $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS) $(SSTV_OBJS) $(HFDL_OBJS) $(LIBS) $(RPATH_FLAGS)

# Header dependencies: the .c.o rule emits a .d per object (-MMD -MP). Pulling
# them in here makes a plain `make` recompile every object that includes a
# changed header (e.g. a struct field added to radio.h) — without this, stale
# objects keep the old struct layout and corrupt memory at run time.
ALL_OBJS=$(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS) $(SSTV_OBJS) $(HFDL_OBJS)
-include $(ALL_OBJS:.o=.d)

# Build the in-tree WDSP (patched: WFM demod + tweaks) on BOTH platforms, so the
# app links the vendored copy and never a system-wide WDSP. Order-only prereq of
# $(PROGRAM): it must exist before linking but a rebuild here does not force a
# relink. On macOS the install-id is stamped to @rpath so it resolves via rpath
# (repo run) or when bundled into the .app; on Linux the $ORIGIN rpath (above)
# handles resolution, so no post-build fix-up is needed.
# The vendored FANS-1/A ASN.1 tree (hfdl_lib/asn1/) builds into its own archive
# through its own Makefile — 240-odd asn1c-generated sources that must not be
# compiled with our warning flags and have no place in the flat OBJS list.
# Order-only prereq, exactly like wdsp-local.
.PHONY: hfdl-asn1
ifeq ($(HFDL_INCLUDE),HFDL)
$(PROGRAM): | hfdl-asn1
hfdl_offline: | hfdl-asn1
hfdl-asn1:
	$(MAKE) -C hfdl_lib/asn1
endif

# The vendored SGP4/SDP4 propagator (sgp4sdp4/), used by the APT georeferencing.
# Same shape again: its own Makefile, order-only prereq.
.PHONY: sgp4-local
ifeq ($(SSTV_INCLUDE),SSTV)
$(PROGRAM): | sgp4-local
apt_offline: | sgp4-local
sgp4-local:
	$(MAKE) -C sgp4sdp4
endif

.PHONY: wdsp-local
$(PROGRAM): | wdsp-local

ifeq ($(UNAME_S), Darwin)
wdsp-local:
	$(MAKE) -C $(WDSP_DIR)
	install_name_tool -id @rpath/libwdsp.dylib $(WDSP_LIB)
else
wdsp-local:
	$(MAKE) -C $(WDSP_DIR)
endif


all: prebuild $(PROGRAM) $(HEADERS) $(MIDI_HEADERS) $(SOURCES) $(SOAPYSDR_SOURCES) \
                         $(CWDAEMON_SOURCES) $(MIDI_SOURCES) $(PURESIGNAL_SOURCES)

# Headless HFDL harness: feeds an I/Q WAV straight into the decoder with an
# explicit receiver centre and tuned-channel frequency and prints every message.
# Not part of `all` — it exists so the decoder can be tested WITHOUT starting the
# app (which would raise a window over whatever the operator is doing and rewrite
# the saved settings on exit).
#   make hfdl-offline && ./hfdl_offline rec.wav <centre_hz> <cursor_hz>
.PHONY: hfdl-offline
hfdl-offline: hfdl_offline
# Links only what the decode chain needs (no GTK, no WDSP, no audio): the panel
# object is the one HFDL file that pulls the UI in, so it is filtered out.
hfdl_offline: tools/hfdl_offline.c $(HFDL_OBJS) log.o
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(HFDL_INCLUDES) \
	  $(shell pkg-config --cflags glib-2.0) -o $@ tools/hfdl_offline.c \
	  $(filter-out hfdl_panel.o,$(HFDL_OBJS)) log.o \
	  $(shell pkg-config --libs glib-2.0) $(HFDL_LIBS) -lm

# Headless APT harness: feeds a demodulated-audio WAV (the format APT recordings
# circulate in) or one of our own I/Q recordings into the decoder and writes the
# decoded picture out as a PNG; `--selftest` needs no recording at all.  Same
# reason as hfdl-offline: verify the decoder without starting the app.
#   make apt-offline && ./apt_offline --selftest
#   ./apt_offline noaa.wav -o pass.png
#   ./apt_offline rec_iq.wav --iq 137100000 137100000 -o pass.png
.PHONY: apt-offline
apt-offline: apt_offline
# Links only the decoder (no GTK, no WDSP, no audio): apt_decoder.c needs
# gdk-pixbuf for the image it hands back, and nothing else.
apt_offline: tools/apt_offline.c apt_decoder.o apt_geo.o apt_coast.o apt_map.o image_save.o log.o
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(SGP4_INCLUDES) \
	  $(shell pkg-config --cflags glib-2.0 gdk-pixbuf-2.0 cairo) -o $@ tools/apt_offline.c \
	  apt_decoder.o apt_geo.o apt_coast.o apt_map.o image_save.o log.o $(SGP4_LIB) \
	  $(shell pkg-config --libs glib-2.0 gdk-pixbuf-2.0 cairo) -lm

prebuild:
	rm -f version.o


.build-flags: .FORCE
	@printf '%s\n' '$(OPTIONS)' | cmp -s - $@ || printf '%s\n' '$(OPTIONS)' > $@
.FORCE:
.PHONY: .FORCE

# `make clean` clears EVERY tree the build writes to, not just the repo root.
# Objects also land in ft8_lib/, hfdl_lib/ and wdsp/ (which builds through its
# own Makefile), and the offline HFDL harness links a binary next to the app.
# A partial clean is worse than no clean: a leftover object built under a
# different set of feature flags links fine and then misbehaves at run time —
# that is exactly the trap that cost a debugging session during TCI Phase A
# (a stale .o compiled before a field was added to struct RADIO), and the reason
# .build-flags exists.  The dependency (.d) files are removed alongside their
# objects for the same reason: a stale .d referring to a header that has since
# moved makes the next build fail with "No rule to make target".
#
# NB: this also cleans the vendored WDSP, so the next `make` rebuilds it (a
# minute or so) rather than just relinking.
.PHONY: clean
clean:
	-rm -f *.o *.d .build-flags
	-rm -f ft8_lib/ft8/*.o ft8_lib/ft8/*.d \
	       ft8_lib/fft/*.o ft8_lib/fft/*.d \
	       ft8_lib/common/*.o ft8_lib/common/*.d
	-rm -f hfdl_lib/*.o hfdl_lib/*.d \
	       hfdl_lib/libfec/*.o hfdl_lib/libfec/*.d
	-$(MAKE) -C hfdl_lib/asn1 clean
	-$(MAKE) -C sgp4sdp4 clean
	-$(MAKE) -C $(WDSP_DIR) clean
	-rm -f $(PROGRAM) hfdl_offline apt_offline
	-rm -rf $(PROGRAM).dSYM hfdl_offline.dSYM apt_offline.dSYM
	-rm -rf $(APP_NAME).app

APP_NAME=MacHPSDR
APP_BUNDLE=$(APP_NAME).app

app: $(PROGRAM)
	@echo "Building fully self-contained macOS .app bundle..."
	@if [ ! -f "$(PROGRAM)" ]; then \
		echo "Error: $(PROGRAM) not found."; exit 1; \
	fi
	@if ! command -v dylibbundler >/dev/null 2>&1; then \
		echo "Error: dylibbundler not found! Install: brew install dylibbundler"; \
		exit 1; \
	fi

	@# Clean and create bundle structure
	rm -rf $(APP_BUNDLE)
	mkdir -p $(APP_BUNDLE)/Contents/MacOS
	mkdir -p $(APP_BUNDLE)/Contents/Resources
	mkdir -p $(APP_BUNDLE)/Contents/Frameworks
	mkdir -p $(APP_BUNDLE)/Contents/Resources/lib
	mkdir -p $(APP_BUNDLE)/Contents/Resources/share
	mkdir -p $(APP_BUNDLE)/Contents/Resources/etc

	@# Copy main executable
	cp $(PROGRAM) $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin
	chmod +x $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin

	@# Bundle all dynamic libraries
	@echo "Bundling dynamic libraries..."
	@dylibbundler -of -b \
		-x $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin \
		-d $(APP_BUNDLE)/Contents/Frameworks/ \
		-s $(WDSP_DIR) \
		-p @executable_path/../Frameworks/ 2>&1 | grep -v "^Warning" || true

	@# Remove duplicate LC_RPATH entries.
	@# The binary is linked with two rpaths (@loader_path/wdsp and
	@# @executable_path/../Frameworks); dylibbundler rewrites BOTH to its
	@# -p prefix, collapsing them into two identical LC_RPATH entries.
	@# Modern dyld refuses to load a binary with duplicate LC_RPATH, so the
	@# bundled app crashes on launch. Delete the extra copies and re-sign.
	@echo "De-duplicating LC_RPATH entries..."
	@BIN=$(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin; \
		while [ $$(otool -l "$$BIN" | grep -c "path @executable_path/../Frameworks/ (offset") -gt 1 ]; do \
			install_name_tool -delete_rpath "@executable_path/../Frameworks/" "$$BIN" 2>/dev/null || break; \
		done; \
		codesign --force --sign - "$$BIN" 2>/dev/null || true

	@# Copy and fix gdk-pixbuf loaders
	@echo "Copying gdk-pixbuf loaders..."
	@if [ -d "$(BREW_PREFIX)/lib/gdk-pixbuf-2.0" ]; then \
		cp -r $(BREW_PREFIX)/lib/gdk-pixbuf-2.0 $(APP_BUNDLE)/Contents/Resources/lib/; \
		find $(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0 \( -name "*.dylib" -o -name "*.so" \) | while read lib; do \
			dylibbundler -of -b -x "$$lib" -d $(APP_BUNDLE)/Contents/Frameworks/ \
				-s $(BREW_PREFIX)/lib -p @executable_path/../Frameworks/ </dev/null 2>/dev/null || true; \
			while [ $$(otool -l "$$lib" | grep -c "path @executable_path/../Frameworks/ (offset") -gt 1 ]; do \
				install_name_tool -delete_rpath "@executable_path/../Frameworks/" "$$lib" 2>/dev/null || break; \
			done; \
			codesign --force --sign - "$$lib" 2>/dev/null || true; \
		done; \
		if [ -f "$(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ]; then \
			sed -i '' 's|$(BREW_PREFIX)/lib|@executable_path/../Resources/lib|g' \
				$(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache; \
		fi; \
	fi

	@# Copy GLib schemas
	@echo "Copying GLib schemas..."
	@if [ -d "$(BREW_PREFIX)/share/glib-2.0/schemas" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/share/glib-2.0; \
		cp -r $(BREW_PREFIX)/share/glib-2.0/schemas $(APP_BUNDLE)/Contents/Resources/share/glib-2.0/; \
		glib-compile-schemas $(APP_BUNDLE)/Contents/Resources/share/glib-2.0/schemas 2>/dev/null || true; \
	fi

	@# Copy GTK themes
	@echo "Copying GTK themes..."
	@if [ -d "$(BREW_PREFIX)/share/themes" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/share/themes; \
		for theme in Adwaita Default; do \
			if [ -d "$(BREW_PREFIX)/share/themes/$$theme" ]; then \
				cp -r $(BREW_PREFIX)/share/themes/$$theme $(APP_BUNDLE)/Contents/Resources/share/themes/ 2>/dev/null || true; \
			fi; \
		done; \
	fi

	@# Copy icon themes
	@echo "Copying icon themes..."
	@if [ -d "$(BREW_PREFIX)/share/icons" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/share/icons; \
		for icon_theme in gnome Adwaita hicolor; do \
			if [ -d "$(BREW_PREFIX)/share/icons/$$icon_theme" ]; then \
				echo "  Copying $$icon_theme icon theme..."; \
				cp -r $(BREW_PREFIX)/share/icons/$$icon_theme $(APP_BUNDLE)/Contents/Resources/share/icons/ 2>/dev/null || true; \
			fi; \
		done; \
		if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
			for icon_dir in $(APP_BUNDLE)/Contents/Resources/share/icons/*; do \
				gtk-update-icon-cache -f "$$icon_dir" 2>/dev/null || true; \
			done; \
		fi; \
	fi

	@# Copy GTK loadable modules.  GTK4 statically links its input-method and
	@# print backends, so lib/gtk-4.0 normally ships NO loadable modules (unlike
	@# GTK3's lib/gtk-3.0 immodules) and this step is a harmless no-op — kept so
	@# any future dlopen'd module under lib/gtk-4.0/4.0.0 is bundled + relinked.
	@echo "Copying GTK modules..."
	@if [ -d "$(BREW_PREFIX)/lib/gtk-4.0" ]; then \
		cp -r $(BREW_PREFIX)/lib/gtk-4.0 $(APP_BUNDLE)/Contents/Resources/lib/ 2>/dev/null || true; \
		find $(APP_BUNDLE)/Contents/Resources/lib/gtk-4.0 \( -name "*.so" -o -name "*.dylib" \) 2>/dev/null | while read lib; do \
			dylibbundler -of -b -x "$$lib" -d $(APP_BUNDLE)/Contents/Frameworks/ \
				-s $(BREW_PREFIX)/lib -p @executable_path/../Frameworks/ </dev/null 2>/dev/null || true; \
		done; \
	fi

	@# Copy SoapySDR device-driver modules (dlopen'd at runtime, so dylibbundler
	@# on the main binary does not pull them in). They in turn drag in libhackrf /
	@# librtlsdr / libusb, which dylibbundler then bundles + relinks into
	@# Frameworks. SOAPY_SDR_PLUGIN_PATH (set in the launcher) points Soapy here,
	@# so HackRF/RTL-SDR work without a Homebrew SoapySDR install on the target.
	@echo "Copying SoapySDR modules..."
	@for moddir in $(BREW_PREFIX)/lib/SoapySDR/modules*; do \
		if [ -d "$$moddir" ]; then \
			dest=$(APP_BUNDLE)/Contents/Resources/lib/SoapySDR/`basename $$moddir`; \
			mkdir -p "$$dest"; \
			cp $$moddir/*.so "$$dest"/ 2>/dev/null || true; \
			for lib in "$$dest"/*.so; do \
				[ -f "$$lib" ] || continue; \
				dylibbundler -of -b -x "$$lib" -d $(APP_BUNDLE)/Contents/Frameworks/ \
					-s $(BREW_PREFIX)/lib -p @executable_path/../Frameworks/ </dev/null 2>/dev/null || true; \
			done; \
		fi; \
	done

	@# Copy GTK settings (GTK4 reads etc/gtk-4.0/settings.ini; Homebrew's gtk4
	@# ships none, so this is normally a no-op — kept for a user-provided file).
	@if [ -f "$(BREW_PREFIX)/etc/gtk-4.0/settings.ini" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/etc/gtk-4.0; \
		cp $(BREW_PREFIX)/etc/gtk-4.0/settings.ini $(APP_BUNDLE)/Contents/Resources/etc/gtk-4.0/ 2>/dev/null || true; \
	fi

	@# Copy application PNG resources
	@echo "Copying application resources..."
	@mkdir -p $(APP_BUNDLE)/Contents/Resources/share/machpsdr
	@for png in machpsdr.png machpsdr_icon.png machpsdr_small.png; do \
		if [ -f "assets/$$png" ]; then \
			cp "assets/$$png" $(APP_BUNDLE)/Contents/Resources/; \
			cp "assets/$$png" $(APP_BUNDLE)/Contents/Resources/share/machpsdr/; \
			cp "assets/$$png" $(APP_BUNDLE)/Contents/MacOS/; \
		fi; \
	done
	@# Bundle the FT8 DXCC country file (loaded from ../Resources/cty.dat).
	@if [ -f assets/cty.dat ]; then \
		cp assets/cty.dat $(APP_BUNDLE)/Contents/Resources/; \
		cp assets/cty.dat $(APP_BUNDLE)/Contents/Resources/share/machpsdr/; \
	fi

	@# Bundle the coastline the APT map overlay draws (same search path as cty.dat).
	@if [ -f assets/coastline.bin ]; then \
		cp assets/coastline.bin $(APP_BUNDLE)/Contents/Resources/; \
		cp assets/coastline.bin $(APP_BUNDLE)/Contents/Resources/share/machpsdr/; \
	fi

	@# Create app icon
	@echo "Creating app icon..."
	@if [ -f "assets/machpsdr_icon.png" ]; then \
		mkdir -p $(APP_NAME).iconset; \
		sips -z 16 16 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_16x16.png >/dev/null 2>&1; \
		sips -z 32 32 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_16x16@2x.png >/dev/null 2>&1; \
		sips -z 32 32 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_32x32.png >/dev/null 2>&1; \
		sips -z 64 64 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_32x32@2x.png >/dev/null 2>&1; \
		sips -z 128 128 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_128x128.png >/dev/null 2>&1; \
		sips -z 256 256 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_128x128@2x.png >/dev/null 2>&1; \
		sips -z 256 256 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_256x256.png >/dev/null 2>&1; \
		sips -z 512 512 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_256x256@2x.png >/dev/null 2>&1; \
		sips -z 512 512 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_512x512.png >/dev/null 2>&1; \
		sips -z 1024 1024 machpsdr_icon.png --out $(APP_NAME).iconset/icon_512x512@2x.png >/dev/null 2>&1; \
		iconutil -c icns $(APP_NAME).iconset -o $(APP_BUNDLE)/Contents/Resources/$(APP_NAME).icns 2>/dev/null || true; \
		rm -rf $(APP_NAME).iconset; \
	fi

	@# Create launcher script
	@echo "Creating launcher script..."
	@echo '#!/bin/bash' > $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'DIR="$$(cd "$$(dirname "$$0")" && pwd)"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'RES="$$DIR/../Resources"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '# Library paths' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export DYLD_LIBRARY_PATH="$$DIR/../Frameworks:$$RES/lib"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export DYLD_FALLBACK_LIBRARY_PATH="$$DIR/../Frameworks"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '# GTK and GLib paths' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export XDG_DATA_DIRS="$$RES/share:$(BREW_PREFIX)/share:/usr/share"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export XDG_DATA_HOME="$$RES/share"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export XDG_CONFIG_HOME="$$RES/etc"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GTK_DATA_PREFIX="$$RES"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GTK_EXE_PREFIX="$$RES"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GTK_PATH="$$RES"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '# GTK theme (Adwaita is built into GTK4; no immodules.cache in GTK4)' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GTK_THEME="Adwaita"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GTK_EXE_PREFIX="$$RES"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '# GDK Pixbuf' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GDK_PIXBUF_MODULE_FILE="$$RES/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GDK_PIXBUF_MODULEDIR="$$RES/lib/gdk-pixbuf-2.0/2.10.0/loaders"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '# Icon theme' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GSETTINGS_SCHEMA_DIR="$$RES/share/glib-2.0/schemas"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '# SoapySDR device-driver modules (HackRF / RTL-SDR)' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export SOAPY_SDR_PLUGIN_PATH="$$(echo "$$RES"/lib/SoapySDR/modules* | tr " " ":")"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo '# Launch application' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'cd "$$RES"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'exec "$$DIR/$(APP_NAME)-bin" "$$@"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@chmod +x $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)

	@# Create Info.plist
	@echo "Creating Info.plist..."
	@echo '<?xml version="1.0" encoding="UTF-8"?>' > $(APP_BUNDLE)/Contents/Info.plist
	@echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '<plist version="1.0">' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '<dict>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleExecutable</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>$(APP_NAME)</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleName</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>$(APP_NAME)</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleDisplayName</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>MacHPSDR</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleIdentifier</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>com.machpsdr.app</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleVersion</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>$(GIT_VERSION)</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleShortVersionString</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>$(GIT_VERSION)</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundlePackageType</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>APPL</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleIconFile</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>$(APP_NAME)</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleSignature</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>????</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>NSHighResolutionCapable</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <true/>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>LSMinimumSystemVersion</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>10.13</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>NSPrincipalClass</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>NSApplication</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>LSApplicationCategoryType</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>public.app-category.utilities</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '</dict>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '</plist>' >> $(APP_BUNDLE)/Contents/Info.plist

	@# Fix permissions and extended attributes
	@echo "Fixing permissions and clearing extended attributes..."
	@find $(APP_BUNDLE) -type f -name "*.dylib" -exec chmod 755 {} \;
	@find $(APP_BUNDLE) -type f -name "*.so" -exec chmod 755 {} \;
	@xattr -cr $(APP_BUNDLE)
	@touch $(APP_BUNDLE)

	@# Final plain ad-hoc signature of the main binary (after xattr -cr, so it is
	@# the binary's last state). NOTE: do NOT sign with the com.apple.vm.device-access
	@# entitlement here — it is a *restricted* entitlement and an ad-hoc signature
	@# carrying it makes launchd reject the app on stricter Macs ("Launchd job spawn
	@# failed", RBSRequestErrorDomain code 5 / POSIX 111). The USB device-capture
	@# problem (needed to claim a HackRF/RTL-SDR without root) is instead solved by
	@# bundling an older libusb that predates the capture requirement — see the
	@# libusb handling in the SoapySDR-module bundling step.
	@echo "Re-signing binary (plain ad-hoc)..."
	@codesign --force --sign - $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin 2>/dev/null || true

	@# Summary
	@echo ""
	@echo "=========================================="
	@echo "  Bundle created: $(APP_BUNDLE)"
	@echo "=========================================="
	@du -sh $(APP_BUNDLE)
	@echo ""
	@echo "Contents:"
	@echo "  - Frameworks: $$(ls $(APP_BUNDLE)/Contents/Frameworks | wc -l | xargs) libraries"
	@echo "  - Icon themes: $$(ls -d $(APP_BUNDLE)/Contents/Resources/share/icons/* 2>/dev/null | wc -l | xargs)"
	@echo "  - GTK themes: $$(ls -d $(APP_BUNDLE)/Contents/Resources/share/themes/* 2>/dev/null | wc -l | xargs)"
	@echo "  - GLib schemas: $$(ls $(APP_BUNDLE)/Contents/Resources/share/glib-2.0/schemas/*.compiled 2>/dev/null | wc -l | xargs)"
	@echo ""
	@echo "Test with: open $(APP_BUNDLE)"
	@echo "Install with: make install"
	@echo "=========================================="
