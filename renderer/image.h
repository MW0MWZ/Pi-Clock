/*
 * image.h - Image loading utilities for Pi-Clock
 *
 * Provides functions to load JPEG and PNG images from disk and
 * convert them into Cairo image surfaces ready for compositing.
 *
 * Uses stb_image.h (public domain, by Sean Barrett) for decoding.
 * This avoids adding libjpeg/libpng as build dependencies — the
 * single header file handles everything.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_IMAGE_H
#define PIC_IMAGE_H

#include <cairo/cairo.h>

/*
 * pic_image_load - Load an image file into a Cairo surface.
 *
 * Reads a JPEG or PNG file from disk, decodes it, and creates a
 * Cairo ARGB32 image surface with the pixel data. The surface
 * dimensions match the source image.
 *
 * The caller is responsible for destroying the returned surface
 * with cairo_surface_destroy() when it's no longer needed.
 *
 *   path - File path to the image (JPEG or PNG).
 *
 * Returns a Cairo image surface on success, or NULL on failure
 * (file not found, decode error, or memory allocation failure).
 */
cairo_surface_t *pic_image_load(const char *path);

/*
 * pic_image_load_scaled - Load an image and scale it to target dimensions.
 *
 * Same as pic_image_load(), but after loading the source image, it
 * creates a second surface at the requested dimensions and paints
 * the source onto it with bilinear filtering. The source surface
 * is freed internally.
 *
 * This is used to match map images to the display resolution. For
 * example, loading a 21600x10800 Blue Marble JPEG and scaling it
 * to 1920x1080 for a 1080p display.
 *
 *   path   - File path to the image (JPEG or PNG).
 *   width  - Target width in pixels.
 *   height - Target height in pixels.
 *
 * Returns a Cairo image surface at the requested dimensions on
 * success, or NULL on failure.
 */
cairo_surface_t *pic_image_load_scaled(const char *path,
                                       int width, int height);

#endif /* PIC_IMAGE_H */
