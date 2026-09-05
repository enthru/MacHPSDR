#!/usr/bin/env python3
"""Instrument the pinned libad9361 rate setter without changing its sequence."""
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
source = path.read_text()
marker = 'MACHPSDR AD9361 operation timing'
if marker in source:
    print('==> libad9361 already patched (operation timing)')
    sys.exit(0)
helper = r'''
/* MACHPSDR AD9361 operation timing: distinguish network reads, FIR upload,
 * enable/disable and clock writes. Only runs during sample-rate setup. */
#ifndef _WIN32
#include <time.h>
#endif
static double machpsdr_rate_time(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
#endif
}
'''
anchor = 'int ad9361_set_bb_rate(struct iio_device *dev, unsigned long rate)'
assert source.count(anchor) == 1, 'unexpected libad9361 rate setter'
prefix, body = source.split(anchor)
# These calls are single statements in the pinned upstream version. Wrapping
# in do/while preserves the unbraced conditional frequency writes as well.
pattern = re.compile(r'^(\s*)(?P<call>(?:ret = )?(?:iio_channel_attr_(?:read|write)_longlong|iio_device_attr_(?:read|write_raw)|ad9361_(?:get|set)_trx_fir_enable)\([^;\n]*\));$', re.M)
count = 0

def instrument(match):
    global count
    count += 1
    indent = match.group(1)
    call = match.group('call')
    label = call.replace('ret = ', '').replace('"', '').replace('\\', '')
    return (indent + 'do {\n' + indent + '\tdouble started = machpsdr_rate_time();\n'
            + indent + '\t' + call + ';\n'
            + indent + '\tfprintf(stderr, "[AD9361 timing] %.3f ms: ' + label
            + '\\n", (machpsdr_rate_time() - started) * 1000.0);\n'
            + indent + '} while (0);')
body = pattern.sub(instrument, body)
assert count == 11, f'unexpected operation count: {count}'
path.write_text(prefix + helper + '\n' + anchor + body)
print('==> patched libad9361: timing 11 rate/FIR operations')
