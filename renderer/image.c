/*
 * image.c - Image loading utilities implementation
 *
 * Loads JPEG/PNG images using stb_image.h and converts the pixel
 * data into Cairo ARGB32 surfaces.
 *
 * The tricky part is pixel format conversion: stb_image decodes to
 * RGBA (4 bytes per pixel, R first), but Cairo uses ARGB32 in native
 * byte order with pre-multiplied alpha. On little-endian systems
 * (ARM, x86), Cairo's ARGB32 is stored in memory as BGRA. So we
 * need to swizzle the channels during conversion.
 *
 * Since our map images are photographs (no transparency), the alpha
 * channel is always 255 and pre-multiplication is a no-op. But we
 * do the channel swap correctly so this code works for any image.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * STB_IMAGE_IMPLEMENTATION must be defined in exactly one .c file
 * to include the stb_image function bodies. All other files that
 * #include "stb_image.h" get only the declarations.
 */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "image.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * pic_image_load - Load an image file into a Cairo surface.
 *
 * Steps:
 *   1. Use stbi_load() to decode the file into raw RGBA pixels.
 *   2. Create a Cairo ARGB32 surface of the same dimensions.
 *   3. Copy pixels from stb's RGBA layout to Cairo's ARGB32 layout,
 *      swapping R and B channels (RGBA → BGRA on little-endian).
 *   4. Free the stb pixel buffer.
 *   5. Return the Cairo surface.
 */
cairo_surface_t *pic_image_load(const char *path)
{
    int img_w, img_h, channels;
    unsigned char *pixels;
    cairo_surface_t *surface;
    unsigned char *cairo_data;
    int cairo_stride;
    int x, y;

    /* Decode the image, requesting 4 channels (RGBA) regardless of
     * the source format. stb_image handles the conversion. */
    pixels = stbi_load(path, &img_w, &img_h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "image: failed to load '%s': %s\n",
                path, stbi_failure_reason());
        return NULL;
    }

    printf("image: loaded '%s' (%dx%d, %d channels)\n",
           path, img_w, img_h, channels);

    /* Create a Cairo surface to hold the converted pixel data */
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img_w, img_h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "image: surface creation failed for '%s'\n", path);
        stbi_image_free(pixels);
        return NULL;
    }

    /*
     * Convert pixel format: stb RGBA → Cairo ARGB32.
     *
     * Cairo's ARGB32 format stores pixels as 32-bit values in native
     * byte order. On little-endian systems (ARM, x86), the byte layout
     * in memory is B, G, R, A. On big-endian it's A, R, G, B.
     *
     * We write directly to the Cairo surface's pixel buffer, which
     * requires calling cairo_surface_flush() before and
     * cairo_surface_mark_dirty() after to maintain cache coherency.
     *
     * Cairo's stride (bytes per row) may be larger than width * 4
     * due to alignment padding, so we use cairo_image_surface_get_stride()
     * rather than assuming width * 4.
     */
    cairo_surface_flush(surface);
    cairo_data = cairo_image_surface_get_data(surface);
    cairo_stride = cairo_image_surface_get_stride(surface);

    for (y = 0; y < img_h; y++) {
        /* Pointer to the start of this row in the stb buffer */
        unsigned char *src_row = pixels + y * img_w * 4;
        /* Pointer to the start of this row in the Cairo buffer */
        unsigned char *dst_row = cairo_data + y * cairo_stride;

        for (x = 0; x < img_w; x++) {
            unsigned char r = src_row[x * 4 + 0];
            unsigned char g = src_row[x * 4 + 1];
            unsigned char b = src_row[x * 4 + 2];
            unsigned char a = src_row[x * 4 + 3];

            /*
             * Write in Cairo's ARGB32 format (native byte order).
             * Using a uint32_t write ensures correct byte ordering
             * regardless of endianness. The macro below packs the
             * channels into a 32-bit value as: (A << 24 | R << 16 | G << 8 | B).
             *
             * For photographic images (a == 255), pre-multiplication
             * is a no-op since (channel * 255 / 255) == channel.
             */
            ((unsigned int *)dst_row)[x] =
                ((unsigned int)a << 24) |
                ((unsigned int)r << 16) |
                ((unsigned int)g <<  8) |
                ((unsigned int)b);
        }
    }

    /* Tell Cairo that we modified its pixel buffer directly */
    cairo_surface_mark_dirty(surface);

    /* Free the stb pixel buffer — Cairo now owns a copy of the data */
    stbi_image_free(pixels);

    return surface;
}

/*
 * pic_image_load_scaled - Load an image and scale to target dimensions.
 *
 * Loads the full-resolution source image, then creates a new surface
 * at the target dimensions and paints the source onto it using Cairo's
 * bilinear filtering (CAIRO_FILTER_BILINEAR). The full-res source is
 * freed immediately to avoid holding both in memory longer than needed.
 *
 * On a Pi Zero W with 512MB RAM, this matters: a 21600x10800 ARGB32
 * surface consumes ~933MB (!) so we must scale it down quickly. The
 * stb_image loader provides the raw pixels in a compact buffer (~933MB
 * for the decoded RGBA), and we copy row-by-row into the Cairo surface
 * which is immediately scaled down and the source freed.
 *
 * For very large source images (21600x10800), this function will use
 * significant memory briefly. On the Pi Zero W, we should pre-scale
 * maps to the target resolution at install time rather than doing it
 * at runtime. The process_maps.sh script already handles this.
 */
cairo_surface_t *pic_image_load_scaled(const char *path,
                                       int width, int height)
{
    cairo_surface_t *source, *scaled;
    cairo_t *cr;
    int src_w, src_h;
    double sx, sy;

    /* Load the full-resolution image */
    source = pic_image_load(path);
    if (!source) {
        return NULL;
    }

    src_w = cairo_image_surface_get_width(source);
    src_h = cairo_image_surface_get_height(source);

    /* If the source already matches the target, return it directly */
    if (src_w == width && src_h == height) {
        printf("image: '%s' already at %dx%d, no scaling needed\n",
               path, width, height);
        return source;
    }

    /* Create the target surface */
    scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(scaled) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "image: scaled surface creation failed\n");
        cairo_surface_destroy(source);
        return NULL;
    }

    /*
     * Paint the source onto the scaled surface.
     *
     * We compute scale factors, apply them as a transformation matrix
     * to the Cairo context, then paint the source. Cairo handles the
     * resampling using bilinear filtering, which gives good quality
     * for photographic images.
     */
    cr = cairo_create(scaled);
    sx = (double)width / src_w;
    sy = (double)height / src_h;
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, source, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_destroy(cr);

    printf("image: scaled '%s' from %dx%d to %dx%d\n",
           path, src_w, src_h, width, height);

    /* Free the full-resolution source — we only keep the scaled version */
    cairo_surface_destroy(source);

    return scaled;
}

/*
 * suffix_for_height - Matched-resolution filename suffix.
 *
 * Defaults to "1080p" for unrecognised heights, matching the
 * behaviour of the display.c/main.c helpers that existed before
 * this function was factored out.
 */
static const char *suffix_for_height(int height)
{
    switch (height) {
    case 720:  return "720p";
    case 1080: return "1080p";
    case 1440: return "1440p";
    case 2160: return "4k";
    default:   return "1080p";
    }
}

cairo_surface_t *pic_map_load(const char *maps_dir,
                              const char *folder,
                              const char *basename,
                              int display_w, int display_h,
                              int allow_hires)
{
    char path[512];

    if (!maps_dir || !folder || !basename) return NULL;

    /* Hi-res upgrade: sample from the 4k source even on a smaller
     * display so zoom retains detail. Only attempted if the caller
     * permits (RAM gate) and the display isn't already 4k. */
    if (allow_hires && display_h < 2160) {
        snprintf(path, sizeof(path), "%s/%s/%s_4k.jpg",
                 maps_dir, folder, basename);
        if (access(path, R_OK) == 0) {
            cairo_surface_t *surf = pic_image_load(path);
            if (surf) {
                printf("image: hi-res upgrade — loaded 4k '%s' for "
                       "%dx%d display (zoom headroom)\n",
                       path, display_w, display_h);
                return surf;
            }
            /* Fall through to matched-res on load failure */
        }
    }

    /* Matched-resolution fallback — original behaviour. */
    snprintf(path, sizeof(path), "%s/%s/%s_%s.jpg",
             maps_dir, folder, basename, suffix_for_height(display_h));
    return pic_image_load_scaled(path, display_w, display_h);
}
