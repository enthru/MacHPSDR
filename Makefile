# find what system we are running on
UNAME_S := $(shell uname -s)

# Get git commit version and date
GIT_DATE := $(firstword $(shell git --no-pager show --date=short --format="%ai" --name-only))
GIT_VERSION := $(shell git describe --abbrev=0 --tags 2>/dev/null || echo "unknown")

CC=gcc
LINK=gcc

GTKINCLUDES=`pkg-config --cflags gtk+-3.0`
GTKLIBS=`pkg-config --libs gtk+-3.0`

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


ifeq ($(UNAME_S), Linux)
# cwdaemon support. Allows linux based logging software to key an Hermes/HermesLite2
# needs :
#			https://github.com/m5evt/unixcw-3.5.1.git

CWDAEMON_INCLUDE=CWDAEMON

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

CFLAGS= -g -Wno-deprecated-declarations -O3
OPTIONS=  $(MIDI_OPTIONS) $(AUDIO_OPTIONS) $(PURESIGNAL_OPTIONS) $(SOAPYSDR_OPTIONS) \
          $(CWDAEMON_OPTIONS) $(OPENGL_OPTIONS) $(FT8_OPTIONS) \
          -D USE_VFO_B_MODE_AND_FILTER="USE_VFO_B_MODE_AND_FILTER" \
          -D GIT_DATE='"$(GIT_DATE)"' -D GIT_VERSION='"$(GIT_VERSION)"'

# WDSP: use the in-tree copy (./wdsp) rather than the system-installed library.
WDSP_DIR=wdsp
WDSP_LIB=$(WDSP_DIR)/libwdsp.dylib

ifeq ($(UNAME_S), Linux)
LIBS=-lrt -lm -lpthread -lwdsp $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) $(CWDAEMON_LIBS) $(OPENGL_LIBS) $(MIDI_LIBS)
WDSP_INCLUDE=
RPATH_FLAGS=
endif
ifeq ($(UNAME_S), Darwin)
# Link against ./wdsp/libwdsp.dylib (not /usr/local/lib) and use the in-tree header.
LIBS=-lm -lpthread -L$(WDSP_DIR) -lwdsp $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) $(MIDI_LIBS)
WDSP_INCLUDE=-I$(WDSP_DIR)
# rpaths so the dylib (id @rpath/libwdsp.dylib) resolves both when running
# ./machpsdr from the repo (@loader_path/wdsp) and inside the .app (Frameworks).
RPATH_FLAGS=-Wl,-rpath,@loader_path/$(WDSP_DIR) -Wl,-rpath,@executable_path/../Frameworks
endif

INCLUDES=$(GTKINCLUDES) $(PULSEINCLUDES) $(OPGL_INCLUDES) $(WDSP_INCLUDE) $(FT8_INCLUDES)

COMPILE=$(CC) $(CFLAGS) $(OPTIONS) $(INCLUDES)

.c.o:
	$(COMPILE) -MMD -MP -c -o $@ $<

PROGRAM=machpsdr

SOURCES=\
main.c\
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
receiver.c\
transmitter.c\
vfo.c\
meter.c\
rx_panadapter.c\
tx_panadapter.c\
mic_level.c\
mic_gain.c\
drive_level.c\
waterfall.c\
wideband_panadapter.c\
wideband_waterfall.c\
protocol1.c\
fake_protocol.c\
protocol2.c\
reconnect.c\
radio_dialog.c\
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
bpsk.c \
ringbuffer.c \
hl2.c \
level_meter.c \
tx_info.c \
tx_info_meter.c \
peak_detect.c \
subrx.c \
actions.c

HEADERS=\
main.h\
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
bpsk.h \
ringbuffer.h \
hl2.h \
level_meter.h \
tx_info.h \
tx_info_meter.h \
peak_detect.h \
subrx.h \
actions.h

OBJS=\
main.o\
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
receiver.o\
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
protocol1.o\
fake_protocol.o\
protocol2.o\
reconnect.o\
radio_dialog.o\
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
waterfall_theme.o


$(PROGRAM): $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS)
	$(LINK) -o $(PROGRAM) $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS) $(LIBS) $(RPATH_FLAGS)

# Header dependencies: the .c.o rule emits a .d per object (-MMD -MP). Pulling
# them in here makes a plain `make` recompile every object that includes a
# changed header (e.g. a struct field added to radio.h) — without this, stale
# objects keep the old struct layout and corrupt memory at run time.
ALL_OBJS=$(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(FT8_OBJS)
-include $(ALL_OBJS:.o=.d)

ifeq ($(UNAME_S), Darwin)
# Build the in-tree WDSP and stamp its install-id to @rpath so it can be found
# via rpath (repo run) or bundled into the .app. Order-only prereq of $(PROGRAM):
# it must exist before linking but a rebuild here does not force a relink.
.PHONY: wdsp-local
wdsp-local:
	$(MAKE) -C $(WDSP_DIR)
	install_name_tool -id @rpath/libwdsp.dylib $(WDSP_LIB)

$(PROGRAM): | wdsp-local
endif


all: prebuild $(PROGRAM) $(HEADERS) $(MIDI_HEADERS) $(SOURCES) $(SOAPYSDR_SOURCES) \
                         $(CWDAEMON_SOURCES) $(MIDI_SOURCES) $(PURESIGNAL_SOURCES)

prebuild:
	rm -f version.o


clean:
	-rm -f *.o *.d
	-rm -f ft8_lib/ft8/*.o ft8_lib/fft/*.o ft8_lib/common/*.o
	-rm -f $(PROGRAM)
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
		find $(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0 -name "*.dylib" | while read lib; do \
			dylibbundler -of -b -x "$$lib" -d $(APP_BUNDLE)/Contents/Frameworks/ \
				-s $(BREW_PREFIX)/lib -p @executable_path/../Frameworks/ </dev/null 2>/dev/null || true; \
		done; \
		if [ -f "$(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ]; then \
			sed -i '' 's|$(BREW_PREFIX)/.*lib|@executable_path/../Resources/lib|g' \
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

	@# Copy GTK im modules (input methods)
	@echo "Copying GTK modules..."
	@if [ -d "$(BREW_PREFIX)/lib/gtk-3.0" ]; then \
		cp -r $(BREW_PREFIX)/lib/gtk-3.0 $(APP_BUNDLE)/Contents/Resources/lib/ 2>/dev/null || true; \
		find $(APP_BUNDLE)/Contents/Resources/lib/gtk-3.0 -name "*.so" -o -name "*.dylib" | while read lib; do \
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

	@# Copy GTK settings
	@if [ -f "$(BREW_PREFIX)/etc/gtk-3.0/settings.ini" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/etc/gtk-3.0; \
		cp $(BREW_PREFIX)/etc/gtk-3.0/settings.ini $(APP_BUNDLE)/Contents/Resources/etc/gtk-3.0/ 2>/dev/null || true; \
	fi

	@# Copy application PNG resources
	@echo "Copying application resources..."
	@mkdir -p $(APP_BUNDLE)/Contents/Resources/share/machpsdr
	@for png in machpsdr.png machpsdr_icon.png machpsdr_small.png; do \
		if [ -f "$$png" ]; then \
			cp "$$png" $(APP_BUNDLE)/Contents/Resources/; \
			cp "$$png" $(APP_BUNDLE)/Contents/Resources/share/machpsdr/; \
			cp "$$png" $(APP_BUNDLE)/Contents/MacOS/; \
		fi; \
	done
	@# Bundle the FT8 DXCC country file (loaded from ../Resources/cty.dat).
	@if [ -f cty.dat ]; then \
		cp cty.dat $(APP_BUNDLE)/Contents/Resources/; \
		cp cty.dat $(APP_BUNDLE)/Contents/Resources/share/machpsdr/; \
	fi

	@# Create app icon
	@echo "Creating app icon..."
	@if [ -f "machpsdr_icon.png" ]; then \
		mkdir -p $(APP_NAME).iconset; \
		sips -z 16 16     machpsdr_icon.png --out $(APP_NAME).iconset/icon_16x16.png >/dev/null 2>&1; \
		sips -z 32 32     machpsdr_icon.png --out $(APP_NAME).iconset/icon_16x16@2x.png >/dev/null 2>&1; \
		sips -z 32 32     machpsdr_icon.png --out $(APP_NAME).iconset/icon_32x32.png >/dev/null 2>&1; \
		sips -z 64 64     machpsdr_icon.png --out $(APP_NAME).iconset/icon_32x32@2x.png >/dev/null 2>&1; \
		sips -z 128 128   machpsdr_icon.png --out $(APP_NAME).iconset/icon_128x128.png >/dev/null 2>&1; \
		sips -z 256 256   machpsdr_icon.png --out $(APP_NAME).iconset/icon_128x128@2x.png >/dev/null 2>&1; \
		sips -z 256 256   machpsdr_icon.png --out $(APP_NAME).iconset/icon_256x256.png >/dev/null 2>&1; \
		sips -z 512 512   machpsdr_icon.png --out $(APP_NAME).iconset/icon_256x256@2x.png >/dev/null 2>&1; \
		sips -z 512 512   machpsdr_icon.png --out $(APP_NAME).iconset/icon_512x512.png >/dev/null 2>&1; \
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
	@echo '# GTK theme and modules' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GTK_THEME="Adwaita"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
	@echo 'export GTK_IM_MODULE_FILE="$$RES/lib/gtk-3.0/3.0.0/immodules.cache"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
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
