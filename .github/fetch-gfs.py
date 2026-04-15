#!/usr/bin/env python3
"""
fetch-gfs.py - Download NOAA GFS weather data and convert to a
compact binary format for the Pi-Clock renderer.

Data source: NOAA Global Forecast System (GFS) via NOMADS
URL: https://nomads.ncep.noaa.gov/cgi-bin/filter_gfs_1p00.pl

Downloads multiple GFS fields at 1-degree resolution (360x181 grid)
and writes them to a compact tagged binary format. Fields:

  UGRD  - U-component of wind at 10m (m/s)
  VGRD  - V-component of wind at 10m (m/s)
  TCDC  - Total cloud cover (0-100%)
  APCP  - Accumulated precipitation over 6h forecast (kg/m² = mm)

Binary format (GFS2):
    4 bytes:  magic "GFS2"
    4 bytes:  nx (uint32 LE) = 360
    4 bytes:  ny (uint32 LE) = 181
    8 bytes:  reference time (uint64 LE, unix timestamp)
    4 bytes:  field_count (uint32 LE)
    4 bytes:  reserved (zero)
    For each field:
        4 bytes:  field ID (4-char ASCII, e.g. "UGRD")
        nx*ny*4:  float32 LE array (row-major, 90N to 90S)

Requires: Python 3.6+, grib_get_data (from libeccodes-tools)

Usage:
    python3 fetch-gfs.py output.gfs.gz

Copyright (C) 2026 Andy Taylor (MW0MWZ)
SPDX-License-Identifier: GPL-2.0-only
"""

import struct
import gzip
import sys
import os
import io
import time
import urllib.request
import urllib.error
import tempfile
import subprocess
from datetime import datetime, timezone, timedelta

# Grid dimensions — 1-degree global
GRID_NX = 360
GRID_NY = 181
GRID_POINTS = GRID_NX * GRID_NY  # 65160


def get_latest_cycle():
    """Determine the most recent GFS cycle that should be available.

    GFS runs at 00, 06, 12, 18 UTC. Data is typically available
    3.5-4 hours after the cycle time. We look for the most recent
    cycle that should have data ready.
    """
    now = datetime.now(timezone.utc)

    cycles = [0, 6, 12, 18]
    availability_delay = 4  # hours

    for hours_back in range(0, 48, 6):
        candidate = now - timedelta(hours=hours_back)
        cycle_hour = max(c for c in cycles if c <= candidate.hour)
        cycle_time = candidate.replace(
            hour=cycle_hour, minute=0, second=0, microsecond=0
        )
        if now >= cycle_time + timedelta(hours=availability_delay):
            return cycle_time

    # Fallback: yesterday's 00z
    yesterday = now - timedelta(days=1)
    return yesterday.replace(hour=0, minute=0, second=0, microsecond=0)


def build_nomads_url(cycle_time, variable, level='lev_10_m_above_ground',
                     forecast_hour=0):
    """Build the NOMADS GRIB filter URL for a single variable.

    Args:
        cycle_time: datetime of the GFS cycle
        variable: NOMADS variable name (e.g., 'UGRD', 'VGRD', 'TCDC')
        level: NOMADS level parameter
        forecast_hour: 0=analysis, 6=6h forecast (needed for APCP)
    """
    date_str = cycle_time.strftime('%Y%m%d')
    cycle_str = f'{cycle_time.hour:02d}'

    url = (
        f'https://nomads.ncep.noaa.gov/cgi-bin/filter_gfs_1p00.pl'
        f'?file=gfs.t{cycle_str}z.pgrb2.1p00.f{forecast_hour:03d}'
        f'&{level}=on'
        f'&var_{variable}=on'
        f'&leftlon=0&rightlon=360'
        f'&toplat=90&bottomlat=-90'
        f'&dir=%2Fgfs.{date_str}%2F{cycle_str}%2Fatmos'
    )
    return url


def download_grib(url, output_path, retries=3):
    """Download a GRIB2 file from NOMADS with retries."""
    for attempt in range(retries):
        try:
            print(f'  Downloading: {url[:80]}...')
            req = urllib.request.Request(url, headers={
                'User-Agent': 'Pi-Clock (GFS data fetch)'
            })
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = resp.read()
                with open(output_path, 'wb') as f:
                    f.write(data)
                print(f'  Downloaded {len(data)} bytes')
                return True
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as e:
            print(f'  Attempt {attempt + 1}/{retries} failed: {e}')
            if attempt < retries - 1:
                time.sleep(5 * (attempt + 1))
    return False


def extract_grib_data(grib_path, step_range=None):
    """Extract grid data from a GRIB2 file using grib_get_data.

    Args:
        grib_path: path to the GRIB2 file
        step_range: optional step range filter (e.g., '0-6') to select
                    a specific message when files contain multiple
                    (like APCP which has both 0-6h and 3-6h periods)

    Returns a list of float values (the grid data), or None on failure.
    """
    try:
        cmd = ['grib_get_data']
        if step_range:
            cmd.extend(['-w', f'stepRange={step_range}'])
        cmd.append(grib_path)

        result = subprocess.run(
            cmd, capture_output=True, timeout=30, text=True
        )
        if result.returncode != 0:
            print(f'  grib_get_data failed: {result.stderr[:200]}')
            return None

        # Output format: "  lat   lon   value" per line, header first
        values = []
        for line in result.stdout.strip().split('\n'):
            line = line.strip()
            if not line or line.startswith('Lat'):
                continue
            parts = line.split()
            if len(parts) >= 3:
                try:
                    values.append(float(parts[2]))
                except ValueError:
                    continue

        if not values:
            print('  No values extracted')
            return None

        if len(values) < GRID_POINTS:
            print(f'  Short read: {len(values)}/{GRID_POINTS} values')
            return None

        # Safety: if still got too many (shouldn't happen with -w filter
        # but defensive), take the first grid's worth
        if len(values) > GRID_POINTS:
            print(f'  Got {len(values)} values, taking first {GRID_POINTS}')
            values = values[:GRID_POINTS]

        print(f'  Extracted {len(values)} values')
        return values

    except FileNotFoundError:
        print('  ERROR: grib_get_data not found')
        print('  Install: apt install libeccodes-tools')
        return None
    except subprocess.TimeoutExpired:
        print('  grib_get_data timed out')
        return None


def write_gfs_binary(fields, ref_time, output_path):
    """Write the compact GFS2 binary file.

    Uses io.BytesIO to build the blob efficiently in memory,
    then gzip-compresses to the output file.
    """
    ref_unix = int(ref_time.timestamp())

    for fid, fdata in fields:
        if len(fdata) != GRID_POINTS:
            print(f'  ERROR: field {fid} has {len(fdata)} values, '
                  f'expected {GRID_POINTS}')
            return False

    buf = io.BytesIO()

    # Header
    buf.write(struct.pack('<4sIIQII',
                          b'GFS2',
                          GRID_NX,
                          GRID_NY,
                          ref_unix,
                          len(fields),
                          0))

    # Fields
    for fid, fdata in fields:
        buf.write(struct.pack('<4s', fid.encode('ascii').ljust(4)[:4]))
        buf.write(struct.pack(f'<{GRID_POINTS}f', *fdata))

    raw = buf.getvalue()

    with gzip.open(output_path, 'wb', compresslevel=9) as f:
        f.write(raw)

    raw_kb = len(raw) / 1024
    gz_kb = os.path.getsize(output_path) / 1024

    print(f'  Written: {output_path} ({len(fields)} fields)')
    print(f'  Raw: {raw_kb:.0f} KB, Compressed: {gz_kb:.0f} KB')
    return True


def fetch_field(cycle, tmpdir, variable, level, label,
                forecast_hour=0, step_range=None):
    """Download and extract a single GFS field.

    Returns the data list, or None on failure.
    """
    grib_path = os.path.join(tmpdir, f'{variable.lower()}.grib2')
    url = build_nomads_url(cycle, variable, level, forecast_hour)

    if not download_grib(url, grib_path):
        return None

    print(f'Extracting {label}...')
    data = extract_grib_data(grib_path, step_range)
    if not data:
        print(f'ERROR: Failed to extract {label}')
    return data


def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} output.gfs.gz')
        sys.exit(1)

    output_path = sys.argv[1]

    cycle = get_latest_cycle()
    print(f'GFS cycle: {cycle.strftime("%Y-%m-%d %H:%M UTC")}')

    # Fields to fetch:
    # (variable, level, field_id, label, required, forecast_hour, step_range)
    #
    # APCP uses f006 (6-hour forecast) because f000 (analysis) has no
    # accumulated precipitation. The step_range filter '0-6' selects
    # the 0-6h total accumulation message and ignores the 3-6h sub-period.
    gfs_fields = [
        ('UGRD', 'lev_10_m_above_ground', 'UGRD', 'U-wind',       True,  0, None),
        ('VGRD', 'lev_10_m_above_ground', 'VGRD', 'V-wind',       True,  0, None),
        ('TCDC', 'lev_entire_atmosphere',  'TCDC', 'Cloud cover',  False, 0, None),
        ('APCP', 'lev_surface',            'APCP', 'Precipitation',False, 6, '0-6'),
    ]

    with tempfile.TemporaryDirectory() as tmpdir:
        fields = []
        fallback_cycle = None

        for variable, level, field_id, label, required, fhour, step in gfs_fields:
            data = fetch_field(cycle, tmpdir, variable, level, label,
                               fhour, step)

            # Try previous cycle if latest not available.
            # If we fall back, ALL subsequent fields use the same cycle
            # so they're from the same model run.
            if data is None and required:
                if fallback_cycle is None:
                    fallback_cycle = cycle - timedelta(hours=6)
                    print(f'  Trying fallback cycle: '
                          f'{fallback_cycle.strftime("%Y-%m-%d %H:%M UTC")}')
                data = fetch_field(fallback_cycle, tmpdir,
                                   variable, level, label, fhour, step)
                if data is None:
                    print(f'ERROR: Required field {label} unavailable')
                    sys.exit(1)
                cycle = fallback_cycle

            if data is not None:
                fields.append((field_id, data))
                print(f'  {label}: {len(data)} values')
            elif required:
                sys.exit(1)
            else:
                print(f'  {label}: skipped (optional, not available)')

        print(f'Writing binary ({len(fields)} fields)...')
        if not write_gfs_binary(fields, cycle, output_path):
            sys.exit(1)

    print('Done.')


if __name__ == '__main__':
    main()
