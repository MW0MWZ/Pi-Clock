#!/usr/bin/env python3
"""
generate_ituzones.py - Download and convert ITU Zone GeoJSON to binary

Downloads real ITU Zone boundaries from HB9HIL's hamradio-zones-geojson
repository (MIT license) and converts to the binary polygon format.

Source: https://github.com/HB9HIL/hamradio-zones-geojson
License: MIT

Usage:
    python3 generate_ituzones.py <output.ituzones>

Copyright (C) 2026 Andy Taylor (MW0MWZ)
SPDX-License-Identifier: GPL-2.0-only
"""

import json
import os
import struct
import sys
import urllib.request

GEOJSON_URL = (
    "https://raw.githubusercontent.com/HB9HIL/hamradio-zones-geojson/"
    "main/ituzones.geojson"
)


def extract_polygons(geojson):
    """Extract polygon outlines sorted by zone number."""
    zones = []

    for feature in geojson["features"]:
        geom = feature["geometry"]
        props = feature.get("properties", {})
        zone_num = int(props.get("itu_zone_number", 0))

        polys = []
        if geom["type"] == "Polygon":
            polys.append(geom["coordinates"][0])
        elif geom["type"] == "MultiPolygon":
            for polygon in geom["coordinates"]:
                polys.append(polygon[0])

        for ring in polys:
            if len(ring) >= 3:
                zones.append((zone_num, ring))

    # Sort by zone number so polygon index maps to zone
    zones.sort(key=lambda x: x[0])
    return zones


def write_binary(zones, output_path):
    """Write polygons to binary format."""
    with open(output_path, "wb") as f:
        f.write(struct.pack("<I", len(zones)))

        for zone_num, coords in zones:
            num_points = len(coords)
            lons = [c[0] for c in coords]
            lats = [c[1] for c in coords]

            f.write(struct.pack("<I", num_points))
            f.write(struct.pack(f"<{num_points}f", *lons))
            f.write(struct.pack(f"<{num_points}f", *lats))


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output.ituzones>", file=sys.stderr)
        sys.exit(1)

    output_path = sys.argv[1]

    print("Downloading ITU Zone boundaries from HB9HIL...")
    with urllib.request.urlopen(GEOJSON_URL) as response:
        data = response.read().decode("utf-8")

    geojson = json.loads(data)
    print(f"  {len(geojson['features'])} zones found")

    zones = extract_polygons(geojson)
    total_points = sum(len(z[1]) for z in zones)
    zone_nums = sorted(set(z[0] for z in zones))
    print(f"  {len(zones)} polygons, {total_points} total points")
    print(f"  Zones: {min(zone_nums)}-{max(zone_nums)}")

    # Sanity check — if the property key is wrong, all zone numbers
    # will be 0. Fail loudly rather than producing wrong data.
    if not zone_nums or min(zone_nums) < 1 or max(zone_nums) > 90:
        print(f"ERROR: unexpected zone numbers: {zone_nums[:10]}",
              file=sys.stderr)
        print("Check 'itu_zone_number' key against the GeoJSON.",
              file=sys.stderr)
        sys.exit(1)

    write_binary(zones, output_path)

    size = os.path.getsize(output_path)
    print(f"  wrote {output_path} ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
