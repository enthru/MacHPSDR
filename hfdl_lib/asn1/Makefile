# Vendored ASN.1 tree for FANS-1/A CPDLC (libacars' libacars/asn1/, verbatim).
#
# 242 generated + runtime sources, built into one archive so the main Makefile's
# flat OBJS list does not have to carry them — the same shape as wdsp/.
#
# The tree is asn1c OUTPUT plus the asn1c runtime, not hand-written code, so it
# is never edited here: to update, re-generate or re-copy from
# github.com/szpajder/libacars. The one external reference it makes is
# <libacars/vstring.h> from constr_TYPE.c, satisfied by ../libacars/vstring.h
# (upstream header) and ../vstring.c (our implementation of it).

LIB=libfansasn1.a

CC?=gcc
# -w: this is generated code and it is not ours to tidy; asn1c output warns
# freely under -Wall -Wextra and drowns real warnings from our own files.
CFLAGS?=-g -O2 -w -fPIC
INCLUDES=-I. -I..

SOURCES=$(wildcard *.c)
OBJS=$(SOURCES:.c=.o)

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
