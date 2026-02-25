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
AUDIO_LIBS=-lsoundio
AUDIO_SOURCES=portaudio.c
AUDIO_HEADERS=portaudio.h
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
#PURESIGNAL_INCLUDE=PURESIGNAL

ifeq ($(PURESIGNAL_INCLUDE),PURESIGNAL)
PURESIGNAL_OPTIONS=-D PURESIGNAL
PURESIGNAL_SOURCES= \
puresignal.c
PURESIGNAL_HEADERS= \
puresignal.h
PURESIGNAL_OBJS= \
puresignal.o
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
          $(CWDAEMON_OPTIONS) $(OPENGL_OPTIONS) \
          -D USE_VFO_B_MODE_AND_FILTER="USE_VFO_B_MODE_AND_FILTER" \
          -D GIT_DATE='"$(GIT_DATE)"' -D GIT_VERSION='"$(GIT_VERSION)"'

ifeq ($(UNAME_S), Linux)
LIBS=-lrt -lm -lpthread -lwdsp $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) $(CWDAEMON_LIBS) $(OPENGL_LIBS) $(MIDI_LIBS)
endif
ifeq ($(UNAME_S), Darwin)
LIBS=-lm -lpthread -lwdsp $(GTKLIBS) $(AUDIO_LIBS) $(SOAPYSDR_LIBS) $(MIDI_LIBS)
endif

INCLUDES=$(GTKINCLUDES) $(PULSEINCLUDES) $(OPGL_INCLUDES)

COMPILE=$(CC) $(CFLAGS) $(OPTIONS) $(INCLUDES)

.c.o:
	$(COMPILE) -c -o $@ $<

PROGRAM=linhpsdr

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
protocol2.c\
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
protocol2.o\
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
waterfall_theme.o


$(PROGRAM): $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS)
	$(LINK) -o $(PROGRAM) $(OBJS) $(SOAPYSDR_OBJS) $(CWDAEMON_OBJS) $(MIDI_OBJS) $(PURESIGNAL_OBJS) $(LIBS)


all: prebuild $(PROGRAM) $(HEADERS) $(MIDI_HEADERS) $(SOURCES) $(SOAPYSDR_SOURCES) \
                         $(CWDAEMON_SOURCES) $(MIDI_SOURCES) $(PURESIGNAL_SOURCES)

prebuild:
	rm -f version.o


clean:
	-rm -f *.o
	-rm -f $(PROGRAM)
	-rm -rf $(APP_NAME).app

APP_NAME=LinHPSDR
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
		-p @executable_path/../Frameworks/ 2>&1 | grep -v "^Warning" || true

	@# Copy and fix gdk-pixbuf loaders
	@echo "Copying gdk-pixbuf loaders..."
	@if [ -d "/usr/local/lib/gdk-pixbuf-2.0" ]; then \
		cp -r /usr/local/lib/gdk-pixbuf-2.0 $(APP_BUNDLE)/Contents/Resources/lib/; \
		find $(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0 -name "*.dylib" | while read lib; do \
			dylibbundler -of -b -x "$$lib" -d $(APP_BUNDLE)/Contents/Frameworks/ \
				-p @executable_path/../Frameworks/ 2>/dev/null || true; \
		done; \
		if [ -f "$(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ]; then \
			sed -i '' 's|/usr/local/.*lib|@executable_path/../Resources/lib|g' \
				$(APP_BUNDLE)/Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache; \
		fi; \
	fi

	@# Copy GLib schemas
	@echo "Copying GLib schemas..."
	@if [ -d "/usr/local/share/glib-2.0/schemas" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/share/glib-2.0; \
		cp -r /usr/local/share/glib-2.0/schemas $(APP_BUNDLE)/Contents/Resources/share/glib-2.0/; \
		glib-compile-schemas $(APP_BUNDLE)/Contents/Resources/share/glib-2.0/schemas 2>/dev/null || true; \
	fi

	@# Copy GTK themes
	@echo "Copying GTK themes..."
	@if [ -d "/usr/local/share/themes" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/share/themes; \
		for theme in Adwaita Default; do \
			if [ -d "/usr/local/share/themes/$$theme" ]; then \
				cp -r /usr/local/share/themes/$$theme $(APP_BUNDLE)/Contents/Resources/share/themes/ 2>/dev/null || true; \
			fi; \
		done; \
	fi

	@# Copy icon themes
	@echo "Copying icon themes..."
	@if [ -d "/usr/local/share/icons" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/share/icons; \
		for icon_theme in gnome Adwaita hicolor; do \
			if [ -d "/usr/local/share/icons/$$icon_theme" ]; then \
				echo "  Copying $$icon_theme icon theme..."; \
				cp -r /usr/local/share/icons/$$icon_theme $(APP_BUNDLE)/Contents/Resources/share/icons/ 2>/dev/null || true; \
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
	@if [ -d "/usr/local/lib/gtk-3.0" ]; then \
		cp -r /usr/local/lib/gtk-3.0 $(APP_BUNDLE)/Contents/Resources/lib/ 2>/dev/null || true; \
		find $(APP_BUNDLE)/Contents/Resources/lib/gtk-3.0 -name "*.so" -o -name "*.dylib" | while read lib; do \
			dylibbundler -of -b -x "$$lib" -d $(APP_BUNDLE)/Contents/Frameworks/ \
				-p @executable_path/../Frameworks/ 2>/dev/null || true; \
		done; \
	fi

	@# Copy GTK settings
	@if [ -f "/usr/local/etc/gtk-3.0/settings.ini" ]; then \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/etc/gtk-3.0; \
		cp /usr/local/etc/gtk-3.0/settings.ini $(APP_BUNDLE)/Contents/Resources/etc/gtk-3.0/ 2>/dev/null || true; \
	fi

	@# Copy application PNG resources
	@echo "Copying application resources..."
	@mkdir -p $(APP_BUNDLE)/Contents/Resources/share/linhpsdr
	@for png in hpsdr.png hpsdr_icon.png hpsdr_small.png; do \
		if [ -f "$$png" ]; then \
			cp "$$png" $(APP_BUNDLE)/Contents/Resources/; \
			cp "$$png" $(APP_BUNDLE)/Contents/Resources/share/linhpsdr/; \
			cp "$$png" $(APP_BUNDLE)/Contents/MacOS/; \
		fi; \
	done

	@# Create app icon
	@echo "Creating app icon..."
	@if [ -f "hpsdr_icon.png" ]; then \
		mkdir -p $(APP_NAME).iconset; \
		sips -z 16 16     hpsdr_icon.png --out $(APP_NAME).iconset/icon_16x16.png >/dev/null 2>&1; \
		sips -z 32 32     hpsdr_icon.png --out $(APP_NAME).iconset/icon_16x16@2x.png >/dev/null 2>&1; \
		sips -z 32 32     hpsdr_icon.png --out $(APP_NAME).iconset/icon_32x32.png >/dev/null 2>&1; \
		sips -z 64 64     hpsdr_icon.png --out $(APP_NAME).iconset/icon_32x32@2x.png >/dev/null 2>&1; \
		sips -z 128 128   hpsdr_icon.png --out $(APP_NAME).iconset/icon_128x128.png >/dev/null 2>&1; \
		sips -z 256 256   hpsdr_icon.png --out $(APP_NAME).iconset/icon_128x128@2x.png >/dev/null 2>&1; \
		sips -z 256 256   hpsdr_icon.png --out $(APP_NAME).iconset/icon_256x256.png >/dev/null 2>&1; \
		sips -z 512 512   hpsdr_icon.png --out $(APP_NAME).iconset/icon_256x256@2x.png >/dev/null 2>&1; \
		sips -z 512 512   hpsdr_icon.png --out $(APP_NAME).iconset/icon_512x512.png >/dev/null 2>&1; \
		sips -z 1024 1024 hpsdr_icon.png --out $(APP_NAME).iconset/icon_512x512@2x.png >/dev/null 2>&1; \
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
	@echo 'export XDG_DATA_DIRS="$$RES/share:/usr/local/share:/usr/share"' >> $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME)
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
	@echo '  <string>LinHPSDR</string>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <key>CFBundleIdentifier</key>' >> $(APP_BUNDLE)/Contents/Info.plist
	@echo '  <string>com.linhpsdr.app</string>' >> $(APP_BUNDLE)/Contents/Info.plist
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
