#!/usr/bin/env python3
"""
generate_tzgrid.py - Generate timezone lookup grid

Creates a compact binary file mapping each 1-degree lat/lon cell
to an IANA timezone name. Used by the renderer to look up proper
timezone names (e.g. "America/Denver") from QTH coordinates.

Requires: pip install timezonefinder

Usage: python3 generate_tzgrid.py /output/tzgrid.dat

File format:
  - Header: "TZGRID1\\0" (8 bytes)
  - Grid: 360 * 180 uint16 values (129600 bytes), little-endian
  - String table: null-terminated timezone names

Total file size: ~135 KB

Copyright (C) 2026 Andy Taylor (MW0MWZ)
SPDX-License-Identifier: GPL-2.0-only
"""

import sys
import struct

def main():
    if len(sys.argv) < 2:
        print("Usage: generate_tzgrid.py <output_file>")
        sys.exit(1)

    output_path = sys.argv[1]

    try:
        from timezonefinder import TimezoneFinder
    except ImportError:
        print("Installing timezonefinder...")
        import subprocess
        subprocess.check_call([sys.executable, "-m", "pip", "install",
                               "timezonefinder"])
        from timezonefinder import TimezoneFinder

    tf = TimezoneFinder()

    # Build the grid: 720 columns (lon) x 360 rows (lat) at 0.5-degree
    # Cell centre at (lat + 0.25, lon + 0.25) for each half-degree cell
    tz_names = [""]  # Index 0 = unknown/ocean
    tz_index = {"": 0}
    grid = []

    GRID_W = 720
    GRID_H = 360

    print(f"Generating {GRID_W}x{GRID_H} timezone grid (0.5 degree)...")
    for lat_idx in range(GRID_H):  # 0 = -90, 359 = +89.5
        lat = -90.0 + lat_idx * 0.5 + 0.25
        if lat_idx % 60 == 0:
            print(f"  lat {lat:+6.1f}...")
        for lon_idx in range(GRID_W):  # 0 = -180, 719 = +179.5
            lon = -180.0 + lon_idx * 0.5 + 0.25

            tz = tf.timezone_at(lat=lat, lng=lon)

            if tz is None:
                grid.append(0)
            else:
                if tz not in tz_index:
                    tz_index[tz] = len(tz_names)
                    tz_names.append(tz)
                grid.append(tz_index[tz])

    print(f"  {len(tz_names)} unique timezones found")
    print(f"  {sum(1 for g in grid if g > 0)} land cells")
    assert len(grid) == GRID_W * GRID_H

    # Write binary file
    with open(output_path, "wb") as f:
        # Header
        f.write(b"TZGRID1\x00")

        # Grid (uint16 little-endian)
        for cell in grid:
            f.write(struct.pack("<H", cell))

        # String table (null-terminated strings)
        for name in tz_names:
            f.write(name.encode("utf-8") + b"\x00")

    import os
    size = os.path.getsize(output_path)
    print(f"  wrote {output_path} ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
