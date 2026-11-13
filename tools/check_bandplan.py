#!/usr/bin/env python3
# Verify src/core/frequency.c's band-plan tables.  The draw and the point lookups
# walk each table in order and bail the moment a frequency is below the current
# row's minFrequency, so a row out of minFrequency order silently deletes itself
# and everything after it.  This checks every table stays sorted and well formed.
#
# Run after ANY edit to a table:  python3 tools/check_bandplan.py
import re, sys, pathlib

src = pathlib.Path(__file__).resolve().parent.parent / "src" / "core" / "frequency.c"
text = src.read_text()

# Match:  { <min>LL?, <max>LL?, "name", band, TRUE/FALSE }
row = re.compile(r'\{\s*(\d+)LL?\s*,\s*(\d+)LL?\s*,\s*"([^"]*)"\s*,\s*([A-Za-z0-9_\-]+)\s*,\s*(TRUE|FALSE)\s*\}')
tbl = re.compile(r'(frequencyInfo(?:R1|R3)?)\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};', re.S)

rc = 0
tables = tbl.findall(text)
if len(tables) != 3:
    print(f"FAIL: expected 3 tables (US/R1/R3), found {len(tables)}: {[t[0] for t in tables]}")
    sys.exit(1)

for name, body in tables:
    rows = row.findall(body)
    # drop the {0,0,"",...} terminator
    rows = [r for r in rows if not (int(r[0]) == 0 and int(r[1]) == 0)]
    prev_min = -1
    errors = 0
    for mn, mx, label, band, tx in rows:
        mn, mx = int(mn), int(mx)
        if mn > mx:
            print(f"FAIL {name}: min>max in row {label!r} ({mn} > {mx})")
            errors += 1
        if mn < prev_min:
            print(f"FAIL {name}: out of order at {label!r} ({mn} < previous {prev_min})")
            errors += 1
        prev_min = mn
    status = "OK" if errors == 0 else f"{errors} ERROR(S)"
    print(f"{name}: {len(rows)} rows, {status}")
    rc |= (1 if errors else 0)

# The panadapter's FT8 markers are a product-level list rather than regulatory
# band-plan rows.  Pin the conventional primary dial frequency for every
# supported FT8 band: a typo here would make the click target tune exactly to
# the wrong place while still looking superficially plausible in the UI.
ft8_expected = {
    "160m": 1840000, "80m": 3573000, "60m": 5357000, "40m": 7074000,
    "30m": 10136000, "20m": 14074000, "17m": 18100000,
    "15m": 21074000, "12m": 24915000, "10m": 28074000,
    "6m": 50313000, "4m": 70154000, "2m": 144174000,
    "1.25m": 222065000, "70cm": 432174000, "23cm": 1296174000,
}
ft8_block = re.search(
    r'ft8_frequencies\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};', text, re.S)
if ft8_block is None:
    print("FAIL: ft8_frequencies table not found")
    rc = 1
else:
    ft8_row = re.compile(r'\{\s*(\d+)LL\s*,\s*"([^"]+)"\s*\}')
    actual = {band: int(hz) for hz, band in ft8_row.findall(ft8_block.group(1))}
    if actual != ft8_expected:
        print(f"FAIL ft8_frequencies: expected {ft8_expected}, got {actual}")
        rc = 1
    elif list(actual.values()) != sorted(actual.values()):
        print("FAIL ft8_frequencies: table is not sorted by frequency")
        rc = 1
    else:
        print(f"ft8_frequencies: {len(actual)} bands, OK")

sys.exit(rc)
