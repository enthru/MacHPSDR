#
# Makefile for WDSP library (libwdsp.so on Linux and libwdsp.dylib on MacOS)
#

# get the OS Name
UNAME_S := $(shell uname -s)

PREFIX?=/usr/local
LIBDIR=$(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include

CC?=gcc					# this will always be set upon entry
LINK?=$(CC)				# if unset upon entry, use $(CC)

#GTK_INCLUDE=GTK
ifeq ($GTK_INCLUDE),GTK)
GTKINCLUDES=`pkg-config --cflags gtk+-3.0`
GTKLIBS=`pkg-config --libs gtk+-3.0`
GTKOPTIONS=-D GTK
endif

ifeq ($(UNAME_S), Darwin)
OPTIONS=-g -O3 -D _GNU_SOURCE   	# MacOS: do not need PIC
LIBS=`pkg-config --libs fftw3 fftw3f`	# MacOS: FFTW double (WDSP) + float (libspecbleach/NR4)
PROGRAM=libwdsp.dylib			# MacOS: shared libs end in .dylib
JAVA_PROGRAM=libwdspj.dylib		# MacOS: shared libs end in .dylib
NOEXECSTACK=				# MacOS: Compiler does not have -z option
else
OPTIONS=-g -fPIC -O3 -D _GNU_SOURCE	# Linux: legacy compiler options for WDSP
LIBS=-lfftw3 -lfftw3f -lpthread		# Linux: FFTW3 double (WDSP) + float (libspecbleach/NR4)
PROGRAM=libwdsp.so			# Linux: shared libs end in .so
JAVA_PROGRAM=libwdspj.so		# Linux: shared libs end in .so
NOEXECSTACK=	-z noexecstack		# Linux: link option for gcc
endif

JAVA_LIBS=-L. -lwdsp

INCLUDES=-I $(JAVA_HOME)/include -I $(JAVA_HOME)/include/linux
# Vendored NR3/NR4 public headers + float FFTW for libspecbleach's <fftw3.h>
NR_INCLUDES=-I rnnoise/include -I libspecbleach/include `pkg-config --cflags fftw3f`

COMPILE=$(CC) $(INCLUDES) $(NR_INCLUDES) $(GTKINCLUDES)

SOURCES= amd.c\
ammod.c\
amsq.c\
analyzer.c\
anf.c\
anr.c\
bandpass.c\
calcc.c\
calculus.c\
cblock.c\
cfcomp.c\
cfir.c\
channel.c\
comm.c\
compress.c\
delay.c\
dexp.c\
div.c\
eer.c\
emnr.c\
rnnr.c\
sbnr.c\
emph.c\
eq.c\
fcurve.c\
fir.c\
firmin.c\
fmd.c\
wfmd.c\
rds.c\
fmmod.c\
fmsq.c\
gain.c\
gen.c\
icfir.c\
iir.c\
iobuffs.c\
iqc.c\
linux_port.c\
lmath.c\
main.c\
meter.c\
meterlog10.c\
nbp.c\
nob.c\
nobII.c\
osctrl.c\
patchpanel.c\
resample.c\
rmatch.c\
RXA.c\
sender.c\
shift.c\
siphon.c\
slew.c\
snb.c\
syncbuffs.c\
TXA.c\
utilities.c\
varsamp.c\
version.c\
wcpAGC.c\
wisdom.c

JAVA_SOURCES= org_openhpsdr_dsp_Wdsp.c

HEADERS=amd.h\
ammod.h\
amsq.h\
analyzer.h\
anf.h\
anr.h\
bandpass.h\
calcc.h\
calculus.h\
cblock.h\
cfcomp.h\
cfir.h\
channel.h\
comm.h\
compress.h\
delay.h\
dexp.h\
div.h\
eer.h\
emnr.h\
emph.h\
eq.h\
fastmath.h\
fcurve.h\
fir.h\
firmin.h \
fmd.h\
wfmd.h\
rds.h\
fmmod.h\
fmsq.h\
gain.h\
gen.h\
icfir.h\
iir.h\
iobuffs.h\
iqc.h\
linux_port.h\
lmath.h\
main.h\
meter.h\
meterlog10.h\
nbp.h\
nob.h\
nobII.h\
osctrl.h\
patchpanel.h\
resample.h\
resource.h\
rmatch.h\
RXA.h\
sender.h\
shift.h\
siphon.h\
slew.h\
snb.h\
syncbuffs.h\
TXA.h\
utilities.h\
wcpAGC.h

JAVA_HEADERS= org_openhpsdr_dsp_Wdsp.h

OBJS=linux_port.o\
amd.o\
ammod.o\
amsq.o\
analyzer.o\
anf.o\
anr.o\
bandpass.o\
calcc.o\
calculus.o\
cblock.o\
cfcomp.o\
cfir.o\
channel.o\
comm.o\
compress.o\
delay.o\
dexp.o\
div.o\
eer.o\
emnr.o\
rnnr.o\
sbnr.o\
emph.o\
eq.o\
fcurve.o\
fir.o\
firmin.o\
fmd.o\
wfmd.o\
rds.o\
fmmod.o\
fmsq.o\
gain.o\
gen.o\
icfir.o\
iir.o\
iobuffs.o\
iqc.o\
lmath.o\
main.o\
meter.o\
meterlog10.o\
nbp.o\
nob.o\
nobII.o\
osctrl.o\
patchpanel.o\
resample.o\
rmatch.o\
RXA.o\
sender.o\
shift.o\
siphon.o\
slew.o\
snb.o\
syncbuffs.o\
TXA.o\
utilities.o\
version.o\
varsamp.o\
wcpAGC.o\
wisdom.o

# Vendored NR3/NR4 libraries (compiled in-tree into libwdsp; see rnnr.c/sbnr.c).
# RNNoise (Xiph, BSD-3) — self-contained, model baked into rnn_data.c, own kiss_fft.
# libspecbleach (LGPL-2.1+) — needs fftw3f (float FFTW), shipped by the same Homebrew fftw.
NR_OBJS=rnnoise/src/celt_lpc.o\
rnnoise/src/denoise.o\
rnnoise/src/kiss_fft.o\
rnnoise/src/pitch.o\
rnnoise/src/rnn.o\
rnnoise/src/rnn_data.o\
rnnoise/src/rnn_reader.o\
libspecbleach/src/processors/adaptivedenoiser/adaptive_denoiser.o\
libspecbleach/src/processors/denoiser/spectral_denoiser.o\
libspecbleach/src/processors/specbleach_adenoiser.o\
libspecbleach/src/processors/specbleach_denoiser.o\
libspecbleach/src/shared/gain_estimation/gain_estimators.o\
libspecbleach/src/shared/noise_estimation/adaptive_noise_estimator.o\
libspecbleach/src/shared/noise_estimation/noise_estimator.o\
libspecbleach/src/shared/noise_estimation/noise_profile.o\
libspecbleach/src/shared/post_estimation/postfilter.o\
libspecbleach/src/shared/post_estimation/spectral_whitening.o\
libspecbleach/src/shared/pre_estimation/absolute_hearing_thresholds.o\
libspecbleach/src/shared/pre_estimation/critical_bands.o\
libspecbleach/src/shared/pre_estimation/masking_estimator.o\
libspecbleach/src/shared/pre_estimation/noise_scaling_criterias.o\
libspecbleach/src/shared/pre_estimation/spectral_smoother.o\
libspecbleach/src/shared/pre_estimation/transient_detector.o\
libspecbleach/src/shared/stft/fft_transform.o\
libspecbleach/src/shared/stft/stft_buffer.o\
libspecbleach/src/shared/stft/stft_processor.o\
libspecbleach/src/shared/stft/stft_windows.o\
libspecbleach/src/shared/utils/denoise_mixer.o\
libspecbleach/src/shared/utils/general_utils.o\
libspecbleach/src/shared/utils/spectral_features.o\
libspecbleach/src/shared/utils/spectral_trailing_buffer.o\
libspecbleach/src/shared/utils/spectral_utils.o

JAVA_OBJS= org_openhpsdr_dsp_Wdsp.o

all: $(PROGRAM) $(HEADERS) $(SOURCES)

java: $(JAVA_PROGRAM) $(JAVA_HEADERS) $(JAVA_SOURCES)

$(PROGRAM): $(OBJS) $(NR_OBJS)
	$(LINK) -shared $(NOEXECSTACK) $(LDFLAGS) -o $(PROGRAM) $(OBJS) $(NR_OBJS) $(LIBS) $(GTKLIBS)

$(JAVA_PROGRAM): $(JAVA_OBJS)
	$(LINK) -shared $(NOEXECSTACK) $(LDFLAGS) -o $(JAVA_PROGRAM) $(JAVA_OBJS) $(JAVA_LIBS)

.c.o:
	$(COMPILE) $(OPTIONS) $(GTKOPTIONS) $(CFLAGS) -c -o $@ $<

install-dirs:
	mkdir -p $(LIBDIR) $(INCLUDEDIR)

install: $(PROGRAM) install-dirs
	cp wdsp.h $(INCLUDEDIR)
ifeq ($(UNAME_S), Darwin)
	/usr/bin/install_name_tool -id $(LIBDIR)/$(PROGRAM) $(PROGRAM)
	cp $(PROGRAM) $(LIBDIR)
else
	cp $(PROGRAM) $(LIBDIR)
	ldconfig || :
endif

install_java: $(JAVA_PROGRAM)
	cp $(JAVA_PROGRAM) /usr/local/lib

release: $(PROGRAM)
	cp $(PROGRAM) ../pihpsdr/release/pihpsdr

clean:
	-rm -f *.o
	-rm -f $(NR_OBJS)
	-rm -f $(PROGRAM)
	-rm -f $(JAVA_PROGRAM)

