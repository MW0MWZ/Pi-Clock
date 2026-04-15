#!/usr/bin/env python3
"""
generate_borders.py - Convert Natural Earth GeoJSON to binary border data

Downloads the Natural Earth 110m admin-0 countries GeoJSON and converts
it to a compact binary format for the Pi-Clock renderer.

Binary output format:
    uint32  num_polygons
    For each polygon:
        uint32  num_points
        float[] longitudes (num_points floats)
        float[] latitudes  (num_points floats)

Usage:
    python3 generate_borders.py <output.borders>

The script downloads the GeoJSON from Natural Earth's GitHub repository
(public domain data). The 110m resolution is the most simplified version
— about 200KB of binary output, suitable for a world-scale display where
fine coastal detail isn't needed.

Copyright (C) 2026 Andy Taylor (MW0MWZ)
SPDX-License-Identifier: GPL-2.0-only
"""

import json
import struct
import sys
import urllib.request

# Natural Earth 110m admin-0 countries GeoJSON
# This is the most simplified version — good for world-scale display
GEOJSON_URL = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "master/geojson/ne_110m_admin_0_countries.geojson"
)


def extract_polygons(geojson):
    """
    Extract all polygon rings from a GeoJSON FeatureCollection.

    GeoJSON polygons can be:
      - Polygon: one outer ring + optional inner rings (holes)
      - MultiPolygon: multiple polygons, each with rings

    We extract only the outer rings (index 0 of each polygon's
    coordinate array) since we're drawing borders, not filling.
    Inner rings (holes like lakes) are skipped for simplicity.

    Returns a list of coordinate arrays, each being a list of
    [lon, lat] pairs.
    """
    polygons = []

    for feature in geojson["features"]:
        geom = feature["geometry"]

        if geom["type"] == "Polygon":
            # Polygon has one or more rings; take the outer ring
            outer_ring = geom["coordinates"][0]
            polygons.append(outer_ring)

        elif geom["type"] == "MultiPolygon":
            # MultiPolygon has multiple polygons, each with rings
            for polygon in geom["coordinates"]:
                outer_ring = polygon[0]
                polygons.append(outer_ring)

    return polygons


def simplify_polygon(coords, min_points=3):
    """
    Skip polygons that are too small to be visible at world scale.

    At 110m resolution, most polygons are already quite simplified.
    We just filter out degenerate ones with fewer than min_points.
    """
    if len(coords) < min_points:
        return None
    return coords


def write_binary(polygons, output_path):
    """
    Write polygons to the binary .borders format.

    Format:
        uint32  num_polygons
        For each polygon:
            uint32  num_points
            float[] lons
            float[] lats
    """
    with open(output_path, "wb") as f:
        # Write polygon count
        f.write(struct.pack("<I", len(polygons)))

        for coords in polygons:
            num_points = len(coords)
            lons = [c[0] for c in coords]  # GeoJSON is [lon, lat]
            lats = [c[1] for c in coords]

            # Write point count
            f.write(struct.pack("<I", num_points))

            # Write longitude array
            f.write(struct.pack(f"<{num_points}f", *lons))

            # Write latitude array
            f.write(struct.pack(f"<{num_points}f", *lats))


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output.borders>", file=sys.stderr)
        sys.exit(1)

    output_path = sys.argv[1]

    # Download the GeoJSON
    print(f"Downloading Natural Earth 110m countries...")
    with urllib.request.urlopen(GEOJSON_URL) as response:
        data = response.read().decode("utf-8")

    geojson = json.loads(data)
    print(f"  {len(geojson['features'])} features found")

    # Extract and filter polygons
    raw_polygons = extract_polygons(geojson)
    polygons = []
    for p in raw_polygons:
        simplified = simplify_polygon(p)
        if simplified:
            polygons.append(simplified)

    total_points = sum(len(p) for p in polygons)
    print(f"  {len(polygons)} polygons, {total_points} total points")

    # Write binary output
    write_binary(polygons, output_path)

    import os
    size = os.path.getsize(output_path)
    print(f"  wrote {output_path} ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
