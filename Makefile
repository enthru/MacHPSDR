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
AUDIO_HEADERS=portaudio.h
# Homebrew prefix: /opt/homebrew on Apple Silicon, /usr/local on Intel.
# Used by the `app` target to locate GTK resources for bundling.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
# ...and to COMPILE. SoapySDR and libsoundio are reached as <SoapySDR/Device.h>
# and -lsoundio, with no pkg-config and no -I/-L of their own, and neither Apple
# clang nor the linker searches the Homebrew prefix by default. This built for
# years only because a developer shell that has run `brew shellenv` exports
# CPATH=$(BREW_PREFIX)/include and LIBRARY_PATH=$(BREW_PREFIX)/lib; on a clean
# machine — a fresh checkout, a CI runner, a plain `make` from Finder — the very
# first file that includes discovered.h fails instead. Reproduce with
# `env -u CPATH -u LIBRARY_PATH make`.
BREW_INCLUDES=-I$(BREW_PREFIX)/include
BREW_LIBS=-L$(BREW_PREFIX)/lib
endif
# Windows / MSYS2 (MinGW-w64).  `uname -s` there is MINGW64_NT-10.0-<build>, so
# this has to be a substring test, not the equality the other two use.  MSVC is
# not a target: WDSP's linux_port.h and this tree lean on GNU C (__sync_*
# builtins, statement expressions), so the toolchain is gcc either way.
# Audio is libsoundio's WASAPI backend — the same USE_SOUNDIO path macOS runs,
# which is why no new audio backend is needed here.
ISMINGW := $(findstring MINGW,$(UNAME_S))
ifneq ($(ISMINGW),)
AUDIO_LIBS=-lsoundio
AUDIO_SOURCES=audio.c
AUDIO_HEADERS=audio.h
# WIN32_LEAN_AND_MEAN globally: <windows.h> otherwise drags in the Winsock 1.1
# <winsock.h>, which cannot coexist with the <winsock2.h> net_compat.h needs,
# and GTK's headers include <windows.h> from all over.  Defined here rather than
# per-file so no translation unit can lose the race by include order.
# -include win_compat.h: the functions mingw's CRT lacks (stpcpy) are used by the
# VENDORED ft8_lib, which is carried verbatim.  Force-including is how they are
# supplied without editing upstream code.
#
# __USE_MINGW_ANSI_STDIO makes printf mingw's own ISO-conformant one instead of
# the CRT's.  This tree has 75 uses of %lld and one of %td, none of which the
# Microsoft printf is obliged to understand — and the failure mode is not a
# diagnostic, it is a log line full of garbage.
#
# NOMINMAX stops <windef.h> defining min/max as macros, which collide with the
# ones main.h and WDSP's linux_port.h define.
WIN_OPTIONS=-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D__USE_MINGW_ANSI_STDIO=1 \
            -include win_compat.h
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
# MSYS2 keeps zlib on the default include/lib path, like Linux; liquid-dsp is not
# packaged there at all and has to be built from source (see the CI workflow).
# Without this branch both stay EMPTY and the HFDL link fails with a wall of
# undefined liquid symbols rather than anything naming the cause.
#
# -lfftw3f is here and NOT on the other platforms because a source-built
# liquid-dsp on Windows lands as a static libliquid.a: liquid's own shared-object
# rule is Linux-shaped (it emits a .so with an soname) and does not link there, so
# the archive is what gets installed — and an archive carries no dependency of its
# own, leaving liquid's FFT backend to resolve against OUR link line.  float FFTW
# is already present for WDSP's NR4, so this costs nothing.
ifneq ($(ISMINGW),)
HFDL_INCLUDES=$(HFDL_VENDOR_INCLUDES)
HFDL_LIBS=-lliquid -lfftw3f -lz $(HFDL_ASN1_LIB)
endif
HFDL_SOURCES= hfdl_decoder.c hfdl_demod.c hfdl_fec.c hfdl_frame.c hfdl_msg.c hfdl_arinc.c hfdl_asn1.c hfdl_cpdlc.c hfdl_miam.c hfdl_ohma.c hfdl_util.c hfdl_pdu.c hfdl_panel.c hfdl_lib/libfec/viterbi27_port.c hfdl_lib/hfdl_crc.c hfdl_lib/vstring.c acars_demod.c acars_decoder.c acars_panel.c
HFDL_HEADERS= hfdl_decoder.h hfdl_demod.h hfdl_fec.h hfdl_frame.h hfdl_msg.h hfdl_arinc.h hfdl_asn1.h hfdl_cpdlc.h hfdl_miam.h hfdl_ohma.h hfdl_util.h hfdl_pdu.h hfdl_panel.h acars_demod.h acars_decoder.h acars_panel.h
# VHF ACARS rides on the same flag: its whole application layer IS hfdl_msg.c
# (message header, reassembly, ARINC-622/CPDLC/MIAM/OHMA), so there is nothing
# to build without HFDL and no second GPL story to tell — the link differs, the
# messages do not.
HFDL_OBJS= hfdl_decoder.o hfdl_demod.o hfdl_fec.o hfdl_frame.o hfdl_msg.o hfdl_arinc.o hfdl_asn1.o hfdl_cpdlc.o hfdl_miam.o hfdl_ohma.o hfdl_util.o hfdl_pdu.o hfdl_panel.o hfdl_lib/libfec/viterbi27_port.o hfdl_lib/hfdl_crc.o hfdl_lib/vstring.o acars_demod.o acars_decoder.o acars_panel.o
# The objects in HFDL_OBJS that pull GTK in — every offline harness filters
# exactly these out, so a new panel is named once here rather than in each rule
# (which is how hfdl_offline came to be linked against acars_panel.o).
HFDL_PANEL_OBJS= hfdl_panel.o acars_panel.o
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
# Windows: winmm.  -lwinmm is already on the Windows link line for
# timeBeginPeriod, so the backend adds no new library.
ifneq ($(ISMINGW),)
MIDI_SOURCES= win_midi.c midi2.c midi3.c midi_dialog.c
MIDI_OBJS= win_midi.o midi2.o midi3.o midi_dialog.o
MIDI_LIBS=
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
#
# The flag is PROBED, not hardcoded: `gnu23` is the spelling gcc 14+ and clang
# 18+ accept, while gcc 13 (Ubuntu 24.04's default, and what CI runs) and older
# Apple clang know the same standard only as `gnu2x` and fail the build outright
# with "unrecognized command-line option". Both spellings select C23, so the
# ()->(void) rule this tree depends on holds either way; newer compilers still
# accept gnu2x, so the fallback is safe rather than merely tolerated.
STD_FLAG := $(shell $(CC) -std=gnu23 -E -x c /dev/null >/dev/null 2>&1 \
                    && echo -std=gnu23 || echo -std=gnu2x)
CFLAGS= -g -O3 $(STD_FLAG) -Wall -Wextra \
        -Wno-unused-parameter -Wno-unused-variable \
        -Wno-sign-compare -Wno-missing-field-initializers
OPTIONS=  $(MIDI_OPTIONS) $(AUDIO_OPTIONS) $(PURESIGNAL_OPTIONS) $(SOAPYSDR_OPTIONS) \
          $(CWDAEMON_OPTIONS) $(OPENGL_OPTIONS) $(FT8_OPTIONS) $(SSTV_OPTIONS) $(HFDL_OPTIONS) \
          $(WIN_OPTIONS) \
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
LIBS=-lm -lpthread -L$(WDSP_DIR) -lwdsp $(BREW_LIBS) $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) $(MIDI_LIBS) $(HFDL_LIBS) $(SGP4_LIB)
WDSP_INCLUDE=-I$(WDSP_DIR)
# rpaths so the dylib (id @rpath/libwdsp.dylib) resolves both when running
# ./machpsdr from the repo (@loader_path/wdsp) and inside the .app (Frameworks).
RPATH_FLAGS=-Wl,-rpath,@loader_path/$(WDSP_DIR) -Wl,-rpath,@executable_path/../Frameworks
endif
ifneq ($(ISMINGW),)
# -lws2_32 is Winsock, -liphlpapi backs the getifaddrs() shim in net_compat.c,
# -lwinmm is timeBeginPeriod (the default 15.6 ms timer tick is far too coarse
# for the protocol-1 output pacing).
LIBS=-lm -lpthread -L$(WDSP_DIR) -lwdsp $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) \
     $(MIDI_LIBS) $(HFDL_LIBS) $(SGP4_LIB) -lws2_32 -liphlpapi -lwinmm
WDSP_INCLUDE=-I$(WDSP_DIR)
WDSP_LIB=$(WDSP_DIR)/libwdsp.dll
# Windows has no rpath: the loader looks next to the .exe, so libwdsp.dll is
# copied there by the install/package step rather than found by a link flag.
RPATH_FLAGS=

# ---- subsystem -------------------------------------------------------------
# GUI subsystem, so a double-click does not also open a stray console window.
# The log output that a console-subsystem build gave for free is NOT lost:
# win_startup() in main.c calls AttachConsole(ATTACH_PARENT_PROCESS) and reopens
# stdout/stderr on it, so running the .exe from cmd or PowerShell still prints
# everything, and only a launch with no console at all has nowhere to print to
# (MACHPSDR_LOG_FILE covers that case).
#
#   make WIN_CONSOLE=1     build for the console subsystem instead
#
# Kept one variable away because that is the setting worth reaching for when the
# app dies before main() — a crash in the loader or in GTK's own startup prints
# to a console this attach has not happened yet to acquire.
ifeq ($(WIN_CONSOLE),1)
WIN_LDFLAGS=-mconsole
else
WIN_LDFLAGS=-mwindows
endif

# ---- resources -------------------------------------------------------------
# The .exe icon and its VERSIONINFO block (src/core/machpsdr.rc).  windres is
# derived from CC so the cross harness picks up its own
# x86_64-w64-mingw32-windres and MSYS2 picks up the plain one; either can be
# overridden on the command line.
WINDRES ?= $(CC:gcc=windres)
WIN_RES_OBJS=machpsdr_res.o
# VERSIONINFO's FILEVERSION is four numbers, while GIT_VERSION is a tag ("4.0",
# possibly "v4.0.1" or "unknown").  Strip a leading v and anything from the
# first non-numeric onwards, then pad to four fields; an unparseable tag falls
# back to all zeros rather than failing the build over a cosmetic field.
WIN_VER_NUM := $(shell printf '%s' '$(GIT_VERSION)' \
                 | sed -e 's/^[vV]//' -e 's/[^0-9.].*//' \
                 | awk -F. 'BEGIN{OFS=","} {print $$1+0, $$2+0, $$3+0, 0}')
ifeq ($(strip $(WIN_VER_NUM)),)
WIN_VER_NUM=0,0,0,0
endif
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

INCLUDES=$(SRC_INCLUDES) $(GTKINCLUDES) $(PULSEINCLUDES) $(OPGL_INCLUDES) $(WDSP_INCLUDE) $(FT8_INCLUDES) $(HFDL_INCLUDES) $(SGP4_INCLUDES) $(BREW_INCLUDES)

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
# mingw's gcc appends .exe to an extensionless -o, so a target named `machpsdr`
# would never look up to date and every make would relink.
ifneq ($(ISMINGW),)
PROGRAM=machpsdr.exe
endif

SOURCES=\
main.c\
log.c\
css.c\
audio.c\
version.c\
net_compat.c\
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
net_compat.h\
serial_compat.h\
time_compat.h\
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
net_compat.o\
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
qo100.o\
qo100_dialog.o\
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


$(PROGRAM): $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS) $(SSTV_OBJS) $(HFDL_OBJS) $(WIN_RES_OBJS)
	$(LINK) -o $(PROGRAM) $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS) $(SSTV_OBJS) $(HFDL_OBJS) $(WIN_RES_OBJS) $(LIBS) $(RPATH_FLAGS) $(WIN_LDFLAGS)

# Windows resources.  Both WIN_RES_OBJS and WIN_LDFLAGS above are EMPTY off
# Windows and this rule does not exist there, so the two lines above are the
# same link they always were on macOS and Linux.
# -I twice: the icon is found next to assets/, the .rc's own #includes next to
# it in src/core/.  Neither is on VPATH — a resource is not a source file.
ifneq ($(ISMINGW),)
machpsdr_res.o: src/core/machpsdr.rc assets/machpsdr.ico
	$(WINDRES) -I assets -I src/core -O coff \
	  -D MACHPSDR_VERSION='$(GIT_VERSION)' \
	  -D MACHPSDR_VERSION_NUM='$(WIN_VER_NUM)' \
	  -o $@ $<
endif

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
acars_offline: | hfdl-asn1
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
else ifneq ($(ISMINGW),)
# Windows has no rpath: the loader looks next to the .exe and along PATH, so the
# freshly built DLL is copied beside the binary. (The link itself resolves
# through the import library wdsp/libwdsp.dll.a.)
wdsp-local:
	$(MAKE) -C $(WDSP_DIR)
	cp -f $(WDSP_LIB) .
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
# objects are what pull the UI in, so they are filtered out.
hfdl_offline: tools/hfdl_offline.c $(HFDL_OBJS) log.o
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(HFDL_INCLUDES) $(BREW_INCLUDES) \
	  $(shell pkg-config --cflags glib-2.0) -o $@ tools/hfdl_offline.c \
	  $(filter-out $(HFDL_PANEL_OBJS),$(HFDL_OBJS)) log.o \
	  $(shell pkg-config --libs glib-2.0) $(HFDL_LIBS) -lm

# Headless VHF ACARS harness: `--selftest` needs no recording, and it also eats
# I/Q recordings or the AM-demodulated-audio WAVs ACARS circulates as.  Same
# reason as hfdl-offline: verify the decoder without starting the app.
#   make acars-offline && ./acars_offline --selftest
#   ./acars_offline rec_iq.wav 131550000 131550000
#   ./acars_offline --audio acars_audio.wav
.PHONY: acars-offline
acars-offline: acars_offline
# Links the ACARS chain plus the HFDL application layer it decodes messages
# with; the two panel objects are what pull GTK in, so they are filtered out.
acars_offline: tools/acars_offline.c $(HFDL_OBJS) log.o
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(HFDL_INCLUDES) $(BREW_INCLUDES) \
	  $(shell pkg-config --cflags glib-2.0) -o $@ tools/acars_offline.c \
	  $(filter-out $(HFDL_PANEL_OBJS),$(HFDL_OBJS)) log.o \
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
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(SGP4_INCLUDES) $(BREW_INCLUDES) \
	  $(shell pkg-config --cflags glib-2.0 gdk-pixbuf-2.0 cairo) -o $@ tools/apt_offline.c \
	  apt_decoder.o apt_geo.o apt_coast.o apt_map.o image_save.o log.o $(SGP4_LIB) \
	  $(shell pkg-config --libs glib-2.0 gdk-pixbuf-2.0 cairo) -lm

# Headless SSTV harness: the encoder's own waveform back through the decoder,
# which is the only way this pair is guarded at all — both are self-contained DSP
# with no WDSP and no radio, so a refactor can break the shared timing tables
# silently.  Compares the decoded picture against the source numerically, with a
# flat-green and a wrong-clock control to prove the thresholds discriminate.
# Decodes in Auto (VIS): a forced mode anchors on the VIS break (see CLAUDE.md).
#   make sstv-offline && ./sstv_offline --selftest
.PHONY: sstv-offline
sstv-offline: sstv_offline
# GTK cflags are needed (both modules include <gtk/gtk.h>), but no GTK symbol is
# referenced — glib + gdk-pixbuf is the whole link, as with apt_offline.
sstv_offline: tools/sstv_offline.c sstv_encoder.o sstv_decoder.o image_save.o log.o
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(GTKINCLUDES) $(BREW_INCLUDES) \
	  $(shell pkg-config --cflags glib-2.0 gdk-pixbuf-2.0) -o $@ tools/sstv_offline.c \
	  sstv_encoder.o sstv_decoder.o image_save.o log.o \
	  $(shell pkg-config --libs glib-2.0 gdk-pixbuf-2.0) -lm

# Headless CW harness: the encoder's audio straight back into the decoder, plus
# the keyer driven on a mock clock through its own test hook.  It is the only
# thing guarding three properties that cannot be checked by inspection — that the
# decoder stays SILENT on band noise, that the keyer never strands the TX keyed,
# and that the two Morse timing conventions still agree.
#   make cw-offline && ./cw_offline --selftest
.PHONY: cw-offline
cw-offline: cw_offline
# gtk4 is in the link only because cw_encoder.c/cw_keyer.c reach <gtk/gtk.h>
# through radio.h; the application surface they need is stubbed in the harness.
cw_offline: tools/cw_offline.c cw_decoder.o cw_encoder.o cw_keyer.o log.o
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(GTKINCLUDES) $(BREW_INCLUDES) \
	  -o $@ tools/cw_offline.c cw_decoder.o cw_encoder.o cw_keyer.o log.o $(GTKLIBS) -lm

qo100-offline: qo100_offline
# The QO-100 beacon lock is a closed loop that retunes the radio, so its sign has
# to be provable off air. Links qo100.o alone; the handful of application
# functions it calls are stubbed inside the harness (see tools/qo100_offline.c).
qo100_offline: tools/qo100_offline.c qo100.o log.o
	$(CC) $(CFLAGS) $(OPTIONS) $(SRC_INCLUDES) $(GTKINCLUDES) $(BREW_INCLUDES) \
	  -o $@ tools/qo100_offline.c qo100.o log.o $(GTKLIBS) -lm

# `make check` — every offline self-test in one command, so a regression is
# caught without starting the GUI (which would raise a window over whatever the
# operator is doing and rewrite the saved settings on exit) and without anyone
# having to remember four different invocations.
#
# The list is assembled from the feature flags rather than hardcoded: with
# HFDL_INCLUDE or SSTV_INCLUDE commented out those harnesses cannot even be
# linked, and a `check` that fails to BUILD teaches nobody anything — it must
# run whatever this tree actually has. qo100_offline has no feature flag (qo100.o
# is unconditionally in OBJS), so it is always in.
#
# qo100_offline is the odd one out at the command line: it takes no argument at
# all (the binary is nothing but the self-test), the other three want --selftest,
# which is their mode that needs no recording.  All four already exit non-zero on
# a failed assertion, so the loop below stops at the first one.
CHECK_BINS=qo100_offline
ifeq ($(HFDL_INCLUDE),HFDL)
CHECK_BINS+=hfdl_offline acars_offline
endif
ifeq ($(SSTV_INCLUDE),SSTV)
CHECK_BINS+=apt_offline sstv_offline cw_offline
endif

.PHONY: check
check: $(CHECK_BINS)
	@fail=0; \
	for h in $(CHECK_BINS); do \
	  case $$h in \
	    qo100_offline) args="" ;; \
	    *)             args="--selftest" ;; \
	  esac; \
	  echo ""; \
	  echo "=== $$h $$args ============================================"; \
	  ./$$h $$args || { echo "*** FAILED: $$h $$args"; fail=1; break; }; \
	  echo "--- $$h: OK"; \
	done; \
	echo ""; \
	if [ $$fail -ne 0 ]; then echo "make check: FAILED"; exit 1; fi; \
	echo "make check: all self-tests passed"

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
	-rm -f $(PROGRAM) hfdl_offline acars_offline apt_offline qo100_offline sstv_offline cw_offline
	-rm -rf $(PROGRAM).dSYM hfdl_offline.dSYM acars_offline.dSYM apt_offline.dSYM qo100_offline.dSYM \
	        sstv_offline.dSYM cw_offline.dSYM
	-rm -rf $(APP_NAME).app
	-rm -rf $(WIN_PKG_DIR)

# Windows counterpart of the `app` target: a self-contained folder built around
# the .exe.  Only useful from MSYS2 — the loader cache it has to generate is made
# by dlopen()ing the modules, so it needs the native tool.  See tools/.
WIN_PKG_DIR=machpsdr-win64

.PHONY: win-package
win-package: $(PROGRAM)
	bash tools/win-package.sh $(WIN_PKG_DIR)

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

	@# LC_RPATH is NOT de-duplicated here.  The binary is linked with two rpaths
	@# (@loader_path/wdsp and @executable_path/../Frameworks) and dylibbundler
	@# rewrites both to its -p prefix, so duplicates appear — but they appear in
	@# the copied dylibs too, and more bundling follows this step.  There is one
	@# normalisation pass over every Mach-O near the end of this target; doing it
	@# here as well would be a second copy of the same rule, which is how the
	@# executable came to be handled and the libraries not.

	@# Copy and fix gdk-pixbuf loaders
	@echo "Copying gdk-pixbuf loaders..."
	@if [ -d "$(BREW_PREFIX)/lib/gdk-pixbuf-2.0" ]; then \
		cp -r $(BREW_PREFIX)/lib/gdk-pixbuf-2.0 $(APP_BUNDLE)/Contents/Resources/lib/; \
		find $(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0 \( -name "*.dylib" -o -name "*.so" \) | while read lib; do \
			dylibbundler -of -b -x "$$lib" -d $(APP_BUNDLE)/Contents/Frameworks/ \
				-s $(BREW_PREFIX)/lib -p @executable_path/../Frameworks/ </dev/null 2>/dev/null || true; \
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

	@# Copy application PNG resources.
	@# Resources/ and Resources/share/machpsdr/ ONLY — never Contents/MacOS/.
	@# Both lookup paths (main.c and about_dialog.c) build
	@# <exe dir>/../Resources/machpsdr.png explicitly, and the launcher cds to
	@# Resources, so a copy beside the executable is never read — but codesign
	@# treats everything in Contents/MacOS as code and refuses to seal the bundle
	@# over a PNG it cannot sign ("In subcomponent: .../MacOS/machpsdr_icon.png").
	@echo "Copying application resources..."
	@mkdir -p $(APP_BUNDLE)/Contents/Resources/share/machpsdr
	@for png in machpsdr.png machpsdr_icon.png machpsdr_small.png; do \
		if [ -f "assets/$$png" ]; then \
			cp "assets/$$png" $(APP_BUNDLE)/Contents/Resources/; \
			cp "assets/$$png" $(APP_BUNDLE)/Contents/Resources/share/machpsdr/; \
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
		sips -z 1024 1024 assets/machpsdr_icon.png --out $(APP_NAME).iconset/icon_512x512@2x.png >/dev/null 2>&1; \
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
	@# ---- one LC_RPATH normalisation pass over EVERYTHING ---------------------
	@# Not just the executable.  The runner's build failed the check below on
	@# Contents/Frameworks/libSoapySDR.0.8.1.dylib: dylibbundler copies Homebrew
	@# dylibs in as they are and adds its own -p prefix, so a dylib whose bottle
	@# already carries that rpath ends up with it twice — and dyld rejects a
	@# duplicate LC_RPATH in ANY image it loads, not only in the main binary.
	@# Which libraries this hits depends on how the bottle was built, i.e. on the
	@# machine, which is why it appeared on CI and not here.
	@#
	@# So normalise every Mach-O in the bundle, and do it AFTER all bundling
	@# (SoapySDR modules included) and BEFORE the signatures, since rewriting a
	@# load command invalidates them.  Each distinct rpath value is deleted until
	@# the tool says there is none left and then added back once; the values are
	@# read from otool only to know what to normalise — the end state is asserted
	@# by the check, not by the parse.
	@echo "Normalising LC_RPATH across the bundle..."
	@for f in $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin \
	          $$(find $(APP_BUNDLE)/Contents/Frameworks $(APP_BUNDLE)/Contents/Resources/lib \
	                  \( -name '*.dylib' -o -name '*.so' \) 2>/dev/null); do \
		for v in $$(otool -l "$$f" 2>/dev/null | awk '$$1=="path" && $$3=="(offset" {print $$2}' | sort -u); do \
			while install_name_tool -delete_rpath "$$v" "$$f" 2>/dev/null; do :; done; \
			install_name_tool -add_rpath "$$v" "$$f" 2>/dev/null || true; \
		done; \
		codesign --force --sign - "$$f" 2>/dev/null || true; \
	done

	@echo "Re-signing binary (plain ad-hoc)..."
	@codesign --force --sign - $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin 2>/dev/null || true

	@# ...and the BUNDLE, which is a separate object from the binary inside it.
	@# Signing only the executable leaves the .app itself "not signed at all"
	@# (no Contents/_CodeSignature), which is invisible when the bundle is built
	@# and launched on the same machine — nothing evaluates it — and is what
	@# Gatekeeper looks at the moment the .app is COPIED anywhere: a download, an
	@# artifact, an AirDrop, a USB stick, all of which set the quarantine bit.
	@# Ad-hoc still is not notarized, so a first launch elsewhere needs
	@# System Settings -> Privacy & Security -> "Open Anyway" (or
	@# `xattr -dr com.apple.quarantine`), but an unsigned bundle cannot even get
	@# that far reliably. Same plain ad-hoc identity, and the entitlement warning
	@# above applies here too.
	@echo "Signing the bundle (plain ad-hoc)..."
	@codesign --force --sign - $(APP_BUNDLE) 2>/dev/null \
		&& codesign --verify --strict $(APP_BUNDLE) 2>/dev/null \
		&& echo "  bundle signature verifies" \
		|| echo "  WARNING: bundle is unsigned - Gatekeeper will block it on another Mac"

	@# ---- checks, and they FAIL the build ------------------------------------
	@# The bundle that crashed at launch was built, uploaded and installed by a
	@# green CI run: every step "succeeded" because nothing ever looked at what
	@# came out.  These are the equivalent of win-package.sh's loaders.cache
	@# assertions — cheap, and they turn a silently broken .app into a build
	@# failure at the machine that built it.
	@echo "Checking the bundle..."
	@fail=0; \
	BIN=$(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)-bin; \
	for f in "$$BIN" $$(find $(APP_BUNDLE)/Contents/Frameworks $(APP_BUNDLE)/Contents/Resources/lib \
	                      \( -name '*.dylib' -o -name '*.so' \) 2>/dev/null); do \
		dup=$$(otool -l "$$f" 2>/dev/null | awk '$$1=="path" && $$3=="(offset" {print $$2}' | sort | uniq -d); \
		if [ -n "$$dup" ]; then \
			echo "  FAIL: duplicate LC_RPATH in $${f#$(APP_BUNDLE)/}: $$dup"; \
			echo "        dyld refuses to load this - the app would die at launch."; \
			fail=1; \
		fi; \
	done; \
	miss=$$(otool -L "$$BIN" | tail -n +2 | awk '{print $$1}' | grep -v '^@' | grep -v '^/usr/lib/' | grep -v '^/System/'); \
	if [ -n "$$miss" ]; then \
		echo "  FAIL: the executable still loads libraries from outside the bundle:"; \
		echo "$$miss" | sed 's/^/        /'; \
		fail=1; \
	fi; \
	[ -f $(APP_BUNDLE)/Contents/Resources/share/glib-2.0/schemas/gschemas.compiled ] || \
		{ echo "  FAIL: no compiled GSettings schemas - GTK aborts at startup"; fail=1; }; \
	[ -f $(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache ] || \
		{ echo "  FAIL: no gdk-pixbuf loaders.cache - no icon or image loads"; fail=1; }; \
	[ $$fail -eq 0 ] || { echo "Bundle is broken; deleting it so it cannot be zipped, copied or installed by accident. Re-run 'make app' to reproduce."; rm -rf $(APP_BUNDLE); exit 1; }; \
	echo "  rpaths single, dependencies self-contained, schemas and loaders present"

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
	@# There is no `install` target in this Makefile (`make install` answers "No
	@# rule to make target"), so do not send anyone to one.
	@echo "Install by dragging $(APP_BUNDLE) to /Applications"
	@echo "=========================================="
