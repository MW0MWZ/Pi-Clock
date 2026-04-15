#!/bin/sh
#
# process_maps.sh - Download and resize world maps for Pi-Clock
#
# Downloads public domain equirectangular world maps from NASA and
# Natural Earth, then resizes each to standard display resolutions.
#
# All source maps are public domain (US Government / Natural Earth).
# See CREDITS.md for full attribution details.
#
# Output structure:
#   /output/<map-name>/<map-name>_<resolution>.jpg
#
# Copyright (C) 2026 Andy Taylor (MW0MWZ)
# SPDX-License-Identifier: GPL-2.0-only

set -e

OUTPUT_DIR="/output"
DOWNLOAD_DIR="/tmp/maps_src"
JPEG_QUALITY=95

mkdir -p "$DOWNLOAD_DIR"
mkdir -p "$OUTPUT_DIR"

# ----------------------------------------------------------------
# Helper: resize a source image to all target resolutions
#
# Arguments:
#   $1 - source image file path
#   $2 - output subdirectory name (e.g., "blue_marble")
#
# Uses Lanczos resampling (-filter Lanczos) for the highest quality
# downscaling. The -strip flag removes metadata to reduce file size.
# ----------------------------------------------------------------
resize_map() {
    src="$1"
    name="$2"
    outdir="$OUTPUT_DIR/$name"

    mkdir -p "$outdir"

    # Get source dimensions for logging
    src_dims=$(magick identify -format "%wx%h" "$src" 2>/dev/null || identify -format "%wx%h" "$src")
    echo "  source: $src_dims"

    # Resize to each target resolution
    for spec in "3840x2160:4k" "2560x1440:1440p" "1920x1080:1080p" "1280x720:720p"; do
        res="${spec%%:*}"
        res_name="${spec##*:}"
        outfile="$outdir/${name}_${res_name}.jpg"

        echo "  -> $res_name ($res)..."

        magick "$src" \
            -filter Lanczos \
            -resize "${res}!" \
            -strip \
            -quality "$JPEG_QUALITY" \
            "$outfile" 2>/dev/null \
        || convert "$src" \
            -filter Lanczos \
            -resize "${res}!" \
            -strip \
            -quality "$JPEG_QUALITY" \
            "$outfile"

        # Report output file size
        size=$(du -h "$outfile" | cut -f1)
        echo "     saved: $outfile ($size)"
    done
}

# ----------------------------------------------------------------
# Helper: download a file with verification
#
# Arguments:
#   $1 - URL to download
#   $2 - output file path
#   $3 - description for logging
#
# Exits with error if download fails or file is too small
# (indicating an error page was returned instead of the image).
# ----------------------------------------------------------------
download() {
    url="$1"
    dest="$2"
    desc="$3"

    echo "  downloading $desc..."
    curl -sL -o "$dest" "$url"

    # Verify the download succeeded (not an HTML error page)
    size=$(wc -c < "$dest")
    if [ "$size" -lt 100000 ]; then
        echo "  ERROR: download too small (${size} bytes) — may be an error page"
        echo "  URL: $url"
        cat "$dest"
        return 1
    fi

    echo "  downloaded: $(du -h "$dest" | cut -f1)"
}

# ================================================================
# Download and process each map
# ================================================================

echo "========================================"
echo "Pi-Clock Map Processing Pipeline"
echo "========================================"
echo ""

# ----------------------------------------------------------------
# 1. NASA Blue Marble Next Generation (July, topo + bathymetry)
#
# July composite chosen for Northern Hemisphere summer vegetation
# which gives the most visually appealing green landmasses.
# The topo+bathy variant includes ocean floor shading.
#
# Source: NASA Earth Observatory NEO archive
# ----------------------------------------------------------------
echo "[1/6] NASA Blue Marble Next Generation"

download \
    "https://neo.gsfc.nasa.gov/archive/bluemarble/bmng/world_2km/world.topo.bathy.200407.3x21600x10800.jpg" \
    "$DOWNLOAD_DIR/blue_marble.jpg" \
    "(21600x10800, ~27MB)"

resize_map "$DOWNLOAD_DIR/blue_marble.jpg" "blue_marble"
rm "$DOWNLOAD_DIR/blue_marble.jpg"
echo ""

# ----------------------------------------------------------------
# 2. NASA Black Marble — Earth at Night (2012)
#
# City lights composite. Used for the nighttime hemisphere in the
# day/night blended display.
# ----------------------------------------------------------------
echo "[2/6] NASA Black Marble (Earth at Night 2012)"

download \
    "https://eoimages.gsfc.nasa.gov/images/imagerecords/79000/79765/dnb_land_ocean_ice.2012.13500x6750.jpg" \
    "$DOWNLOAD_DIR/black_marble.jpg" \
    "(13500x6750, ~7MB)"

resize_map "$DOWNLOAD_DIR/black_marble.jpg" "black_marble"
rm "$DOWNLOAD_DIR/black_marble.jpg"
echo ""

# ----------------------------------------------------------------
# 3. Natural Earth I (shaded relief + water)
#
# Soft, painted/airbrush style. The ZIP contains a large TIFF
# that we extract and convert.
# ----------------------------------------------------------------
echo "[3/6] Natural Earth I (Shaded Relief + Water)"

download \
    "https://naciscdn.org/naturalearth/10m/raster/NE1_HR_LC_SR_W.zip" \
    "$DOWNLOAD_DIR/ne1.zip" \
    "(~308MB ZIP)"

echo "  extracting..."
unzip -q -o "$DOWNLOAD_DIR/ne1.zip" -d "$DOWNLOAD_DIR/ne1"
ne1_tif=$(find "$DOWNLOAD_DIR/ne1" -name "*.tif" | head -1)
echo "  found: $ne1_tif"

resize_map "$ne1_tif" "natural_earth_1"
rm -rf "$DOWNLOAD_DIR/ne1" "$DOWNLOAD_DIR/ne1.zip"
echo ""

# ----------------------------------------------------------------
# 4. Natural Earth II (shaded relief + water)
#
# Warmer, more vibrant colours than NE1.
# ----------------------------------------------------------------
echo "[4/6] Natural Earth II (Shaded Relief + Water)"

download \
    "https://naciscdn.org/naturalearth/10m/raster/NE2_HR_LC_SR_W.zip" \
    "$DOWNLOAD_DIR/ne2.zip" \
    "(~270MB ZIP)"

echo "  extracting..."
unzip -q -o "$DOWNLOAD_DIR/ne2.zip" -d "$DOWNLOAD_DIR/ne2"
ne2_tif=$(find "$DOWNLOAD_DIR/ne2" -name "*.tif" | head -1)
echo "  found: $ne2_tif"

resize_map "$ne2_tif" "natural_earth_2"
rm -rf "$DOWNLOAD_DIR/ne2" "$DOWNLOAD_DIR/ne2.zip"
echo ""

# ----------------------------------------------------------------
# 5. Cross-Blended Hypsometric Tints (shaded relief + water)
#
# Classic atlas style with pastel terrain tints.
# ----------------------------------------------------------------
echo "[5/6] Cross-Blended Hypsometric Tints"

download \
    "https://naciscdn.org/naturalearth/10m/raster/HYP_HR_SR_OB_DR.zip" \
    "$DOWNLOAD_DIR/hyp.zip" \
    "(~270MB ZIP)"

echo "  extracting..."
unzip -q -o "$DOWNLOAD_DIR/hyp.zip" -d "$DOWNLOAD_DIR/hyp"
hyp_tif=$(find "$DOWNLOAD_DIR/hyp" -name "*.tif" | head -1)
echo "  found: $hyp_tif"

resize_map "$hyp_tif" "hypsometric"
rm -rf "$DOWNLOAD_DIR/hyp" "$DOWNLOAD_DIR/hyp.zip"
echo ""

# ----------------------------------------------------------------
# 6. Shaded Relief (greyscale only)
#
# Pure greyscale terrain. Good for dark themes or custom colouring.
# ----------------------------------------------------------------
echo "[6/6] Shaded Relief (Greyscale)"

download \
    "https://naciscdn.org/naturalearth/10m/raster/SR_HR.zip" \
    "$DOWNLOAD_DIR/sr.zip" \
    "(~270MB ZIP)"

echo "  extracting..."
unzip -q -o "$DOWNLOAD_DIR/sr.zip" -d "$DOWNLOAD_DIR/sr"
sr_tif=$(find "$DOWNLOAD_DIR/sr" -name "*.tif" | head -1)
echo "  found: $sr_tif"

resize_map "$sr_tif" "shaded_relief"
rm -rf "$DOWNLOAD_DIR/sr" "$DOWNLOAD_DIR/sr.zip"
echo ""

# ----------------------------------------------------------------
# Summary
# ----------------------------------------------------------------
echo "========================================"
echo "Processing complete!"
echo "========================================"
echo ""
echo "Output directory contents:"
du -sh "$OUTPUT_DIR"/*
echo ""
echo "Total size:"
du -sh "$OUTPUT_DIR"
