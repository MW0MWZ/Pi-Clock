/*
 * layers_cqzone.c - CQ Zone overlay layer
 *
 * Draws the 40 CQ Zone boundaries and zone number labels.
 * Boundaries are loaded from a binary data file. Each polygon
 * corresponds to one zone (in order 1-40), and the label is
 * placed at the centroid of each polygon.
 *
 * Drawn in orange/amber to distinguish from other overlays.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "borders.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>

void pic_layer_render_cqzone(cairo_t *cr, int width, int height,
                             time_t now, void *user_data)
{
    pic_borders_t *cq_data = (pic_borders_t *)user_data;
    uint32_t i;
    double half_width;
    double dashes[] = {5.0, 3.0};
    double font_size;

    (void)now;

    if (!cq_data || cq_data->num_polygons == 0) {
        return;
    }

    /* Pixel distance above which two consecutive points lie on
     * opposite sides of the longitudinal seam — scales with zoom. */
    half_width = pic_wrap_threshold_px(width);
    font_size = height / 80.0;

    /* Draw CQ Zone boundaries as dashed orange lines */
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_set_source_rgba(cr, 1.0, 0.7, 0.2, 0.45);
    cairo_set_line_width(cr, 1.2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    for (i = 0; i < cq_data->num_polygons; i++) {
        pic_polygon_t *poly = &cq_data->polygons[i];
        uint32_t j;
        double prev_x;

        if (poly->num_points < 2) {
            continue;
        }

        prev_x = pic_lon_to_x((double)poly->lons[0], width);
        cairo_move_to(cr,
                      prev_x,
                      pic_lat_to_y((double)poly->lats[0], height));

        for (j = 1; j < poly->num_points; j++) {
            double x = pic_lon_to_x((double)poly->lons[j], width);
            double y = pic_lat_to_y((double)poly->lats[j], height);

            if (fabs(x - prev_x) > half_width) {
                cairo_move_to(cr, x, y);
            } else {
                cairo_line_to(cr, x, y);
            }

            prev_x = x;
        }
    }

    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);

    /*
     * Draw zone number labels at the centroid of each polygon.
     * Zone numbers are 1-40, matching the polygon index + 1.
     */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);

    for (i = 0; i < cq_data->num_polygons; i++) {
        pic_polygon_t *poly = &cq_data->polygons[i];
        uint32_t j;
        double min_lon, max_lon, min_lat, max_lat;
        double cx, cy;
        char label[8];
        cairo_text_extents_t extents;
        int zone_num = (int)i + 1;

        if (poly->num_points < 3) {
            continue;
        }

        /*
         * Use bounding box centre instead of vertex centroid.
         * Vertex centroids get pulled toward coastline detail
         * (e.g., Norwegian fjords have thousands of points that
         * skew the average northward). The bounding box centre
         * gives the visual middle of the zone.
         */
        min_lon = max_lon = poly->lons[0];
        min_lat = max_lat = poly->lats[0];
        for (j = 1; j < poly->num_points; j++) {
            if (poly->lons[j] < min_lon) min_lon = poly->lons[j];
            if (poly->lons[j] > max_lon) max_lon = poly->lons[j];
            if (poly->lats[j] < min_lat) min_lat = poly->lats[j];
            if (poly->lats[j] > max_lat) max_lat = poly->lats[j];
        }

        /* Convert bounding box centre to pixel coordinates,
         * with manual nudges for zones where the bbox centre
         * doesn't land in a visually good spot. */
        {
            double center_lon = (min_lon + max_lon) / 2.0;
            double center_lat = (min_lat + max_lat) / 2.0;

            switch (zone_num) {
            case 16: center_lon -= 5.0; break;
            case 36: center_lat -= 10.0; break;
            case 37: center_lat += 5.0; break;
            }

            cx = pic_lon_to_x(center_lon, width);
            cy = pic_lat_to_y(center_lat, height);
        }

        /* Skip if off-screen */
        if (cx < 0 || cx > width || cy < 0 || cy > height) {
            continue;
        }

        snprintf(label, sizeof(label), "%d", zone_num);

        cairo_text_extents(cr, label, &extents);

        /* Draw label with a subtle background for readability */
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.3);
        cairo_rectangle(cr,
                        cx - extents.width / 2 - 2,
                        cy - extents.height / 2 - 2,
                        extents.width + 4,
                        extents.height + 4);
        cairo_fill(cr);

        /* Zone number in orange */
        cairo_set_source_rgba(cr, 1.0, 0.75, 0.25, 0.8);
        cairo_move_to(cr,
                      cx - extents.width / 2,
                      cy + extents.height / 2);
        cairo_show_text(cr, label);
    }
}
