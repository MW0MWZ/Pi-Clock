#!/usr/bin/env python3
"""
generate_timezones.py - Convert Natural Earth timezone GeoJSON to binary data

Downloads the Natural Earth 10m time zones GeoJSON and converts the
polygon outlines to a compact binary format for the renderer.

The output uses the same binary format as borders.dat:
    uint32  num_polygons
    For each polygon:
        uint32  num_points
        float[] longitudes
        float[] latitudes

Only the polygon outlines (edges) are stored — these are the actual
time zone boundary lines that will be drawn as dotted lines on the map.

Usage:
    python3 generate_timezones.py <output.timezones>

Copyright (C) 2026 Andy Taylor (MW0MWZ)
SPDX-License-Identifier: GPL-2.0-only
"""

import json
import struct
import sys
import urllib.request

# Natural Earth 10m time zones — real-world boundaries following
# political borders, not neat 15-degree verticals
GEOJSON_URL = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "master/geojson/ne_10m_time_zones.geojson"
)


def extract_outlines(geojson):
    """
    Extract polygon outlines from the timezone GeoJSON.

    Each timezone feature is a Polygon or MultiPolygon. We take
    the outer ring of each polygon — these are the boundary lines
    between adjacent time zones.

    Returns a list of coordinate arrays (each a list of [lon, lat]).
    """
    outlines = []

    for feature in geojson["features"]:
        geom = feature["geometry"]

        if geom["type"] == "Polygon":
            outlines.append(geom["coordinates"][0])

        elif geom["type"] == "MultiPolygon":
            for polygon in geom["coordinates"]:
                outlines.append(polygon[0])

    return outlines


def simplify_ring(coords, min_points=4):
    """
    Filter out degenerate polygons that are too small to render.
    """
    if len(coords) < min_points:
        return None
    return coords


def write_binary(polygons, output_path):
    """
    Write polygon outlines to the binary format.
    Same format as borders.dat for code reuse.
    """
    with open(output_path, "wb") as f:
        f.write(struct.pack("<I", len(polygons)))

        for coords in polygons:
            num_points = len(coords)
            lons = [c[0] for c in coords]
            lats = [c[1] for c in coords]

            f.write(struct.pack("<I", num_points))
            f.write(struct.pack(f"<{num_points}f", *lons))
            f.write(struct.pack(f"<{num_points}f", *lats))


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output.timezones>", file=sys.stderr)
        sys.exit(1)

    output_path = sys.argv[1]

    print("Downloading Natural Earth 10m time zones...")
    with urllib.request.urlopen(GEOJSON_URL) as response:
        data = response.read().decode("utf-8")

    geojson = json.loads(data)
    print(f"  {len(geojson['features'])} timezone features found")

    # Extract polygon outlines
    raw_outlines = extract_outlines(geojson)
    outlines = []
    for ring in raw_outlines:
        simplified = simplify_ring(ring)
        if simplified:
            outlines.append(simplified)

    total_points = sum(len(p) for p in outlines)
    print(f"  {len(outlines)} polygons, {total_points} total points")

    write_binary(outlines, output_path)

    import os
    size = os.path.getsize(output_path)
    print(f"  wrote {output_path} ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
