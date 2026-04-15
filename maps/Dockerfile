# Dockerfile - Map download and processing pipeline
#
# Downloads public domain world maps from NASA and Natural Earth,
# then resizes each to 720p, 1080p, 1440p, and 4K using high-quality
# Lanczos resampling. Output is JPEG at 95% quality.
#
# Usage:
#   docker build -t pi-clock-maps -f maps/Dockerfile maps/
#   docker run --rm -v $(pwd)/maps/output:/output pi-clock-maps
#
# Copyright (C) 2026 Pi-Clock Contributors
# SPDX-License-Identifier: GPL-2.0-only

FROM alpine:3.21

# Install download tool, image processing, and archive handling
RUN apk add --no-cache \
    curl \
    imagemagick \
    imagemagick-jpeg \
    imagemagick-tiff \
    libjpeg-turbo \
    libjpeg-turbo-utils \
    tiff \
    unzip

# Copy the processing script into the container
COPY process_maps.sh /process_maps.sh
RUN chmod +x /process_maps.sh

ENTRYPOINT ["/process_maps.sh"]
