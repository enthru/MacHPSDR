#!/usr/bin/env python3
"""Build assets/coastline.bin — the world coastline the APT georeferencing draws.

Source: Natural Earth 1:50m coastline, which is public domain (naturalearthdata.com
states no permission, fee or attribution is required).  Fetch the GeoJSON from

  https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_coastline.geojson

and run:  python3 tools/make_coastline.py ne_50m_coastline.geojson assets/coastline.bin

Why 1:50m and not 1:110m or 1:10m: APT lands ~4 km on a pixel, so 110m (tens of
km of simplification) would visibly miss the coast, while 10m is four times the
size for detail the image cannot resolve.

Why a binary and not the GeoJSON: the app would otherwise carry a JSON parser for
one file, and the coordinates are stored as int16 hundred-and-eightieths of a
degree — 1/180 deg is 620 m, well under a pixel — which is 4 bytes a point
instead of about 20.

Format (little-endian):
  char[8]  "APTCOAST"
  uint32   version = 1
  uint32   number of polylines
  per polyline:  uint32 number of points, then that many int16 lon, int16 lat
                 pairs, each = degrees * 180 (lon in -32400..32400, lat in
                 -16200..16200)
"""
import json, struct, sys

SCALE = 180.0          # int16 units per degree; 1/180 deg = 620 m


def main(src, dst):
    data = json.load(open(src))
    polys = []
    for f in data["features"]:
        g = f["geometry"]
        if g["type"] == "LineString":
            polys.append(g["coordinates"])
        elif g["type"] == "MultiLineString":
            polys.extend(g["coordinates"])
    with open(dst, "wb") as out:
        out.write(b"APTCOAST" + struct.pack("<II", 1, len(polys)))
        n = 0
        for p in polys:
            out.write(struct.pack("<I", len(p)))
            for lon, lat in p:
                out.write(struct.pack("<hh", round(lon * SCALE), round(lat * SCALE)))
                n += 1
    print(f"{dst}: {len(polys)} polylines, {n} points")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
