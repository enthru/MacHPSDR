# Vendored SGP4/SDP4 orbit propagator (see README-MACHPSDR).
#
# Built into its own archive, the same shape as wdsp/ and hfdl_lib/asn1/: this
# is upstream code, not ours to tidy, and it must not be rebuilt with our
# warning flags or carried in the main Makefile's flat OBJS list.
#
# sgp_in.c uses g_ascii_strtod (glib) for locale-proof TLE parsing, hence the
# glib cflags — glib is a dependency of the app anyway.

LIB=libsgp4sdp4.a

CC?=gcc
# -w: upstream code, warns freely under -Wall -Wextra and would drown warnings
# from our own files.
CFLAGS?=-g -O2 -w -fPIC
INCLUDES=-I. $(shell pkg-config --cflags glib-2.0)

SOURCES=sgp4sdp4.c sgp_in.c sgp_math.c sgp_obs.c sgp_time.c solar.c
OBJS=$(SOURCES:.c=.o)

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
