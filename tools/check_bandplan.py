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

sys.exit(rc)
