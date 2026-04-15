/*
 * layers_base.c - Base map layer renderer (Black Marble, with wrapping)
 *
 * Renders the NASA Black Marble (Earth at Night) image as the bottom
 * layer. Supports horizontal centering: the map can be shifted so
 * any longitude appears at the centre of the screen.
 *
 * How wrapping works:
 *
 *   The source map image covers the full 360 degrees of longitude
 *   (-180 to +180). When center_lon is non-zero, we need to shift
 *   the image horizontally. This creates a seam where the left and
 *   right edges of the shifted image meet.
 *
 *   We handle this by painting the map image twice:
 *
 *     1. Paint at offset position (shifted left or right)
 *     2. Paint again at offset +/- width to fill the gap
 *
 *   Cairo clips to the surface bounds automatically, so the two
 *   paints seamlessly fill the full output width.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"

/*
 * paint_wrapped - Paint a map image with horizontal wrapping.
 *
 * This helper is shared by both the basemap and daylight layers.
 * It paints the source surface shifted by the center_lon offset,
 * then paints a second copy to fill the wrap-around gap.
 *
 *   cr      - Cairo context to paint onto.
 *   source  - The map image surface (full 360-degree coverage).
 *   width   - Output width in pixels.
 */
static void paint_wrapped(cairo_t *cr, cairo_surface_t *source, int width)
{
    int src_w;
    double offset_x;
    (void)width; /* Source image covers full 360°, no width scaling needed */

    src_w = cairo_image_surface_get_width(source);

    /*
     * Calculate the pixel offset for the center longitude.
     *
     * center_lon=0 means no shift (standard view with date line at edges).
     * center_lon=139.7 means Tokyo at centre, so we shift the map left
     * by the number of pixels corresponding to 139.7 degrees.
     *
     * The offset is calculated in source image coordinates, then
     * scaled to the output width if they differ (though in practice
     * our pre-scaled maps already match the output dimensions).
     */
    offset_x = (pic_config.center_lon / 360.0) * src_w;

    /*
     * Paint the map shifted by the offset. We use the output width
     * for the scale calculation since source and output should match.
     *
     * First paint: the main body of the map
     * Second paint: the wrap-around portion that fills the gap
     *
     * Cairo's surface clipping ensures we don't draw outside bounds.
     */
    cairo_set_source_surface(cr, source, -offset_x, 0);
    cairo_paint(cr);

    /*
     * Second paint for the wrap-around. If we shifted left (positive
     * center_lon), the gap is on the right, so we paint again at
     * x + source_width. If shifted right, the gap is on the left,
     * so we paint at x - source_width. We do both to cover either case.
     */
    if (offset_x > 0) {
        cairo_set_source_surface(cr, source, -offset_x + src_w, 0);
        cairo_paint(cr);
    } else if (offset_x < 0) {
        cairo_set_source_surface(cr, source, -offset_x - src_w, 0);
        cairo_paint(cr);
    }
}

/*
 * pic_layer_render_basemap - Paint the Black Marble with wrapping.
 */
void pic_layer_render_basemap(cairo_t *cr, int width, int height,
                              time_t now, void *user_data)
{
    cairo_surface_t *map_surface = (cairo_surface_t *)user_data;

    (void)height;
    (void)now;

    if (!map_surface) {
        /* No map loaded — fill with near-black as fallback */
        cairo_set_source_rgb(cr, 0.02, 0.02, 0.05);
        cairo_paint(cr);
        return;
    }

    paint_wrapped(cr, map_surface, width);
}
