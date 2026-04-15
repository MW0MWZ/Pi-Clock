/*
 * applet_dxfeed.c - DX Cluster live feed applet
 *
 * Shows a scrolling live feed of recent DX cluster spots
 * in a compact side panel. The band colour legend is drawn
 * by the DX spots layer (layers_dxspots.c) at the bottom
 * centre of the map.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet.h"
#include "dxspot.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

/*
 * pic_applet_render_dxfeed - Render the DX cluster live feed panel.
 *
 * When cr is NULL, returns the total height without drawing (measurement
 * pass for the applet layout engine — see applet.h).
 *
 * Shows a title, separator, then up to FEED_LINES most recent spots
 * from the circular feed buffer. Lines fade from dim (oldest) to
 * bright (newest).
 *
 *   user_data - Pointer to pic_spotlist_t.
 */
double pic_applet_render_dxfeed(cairo_t *cr, double width,
                                time_t now, void *user_data)
{
    pic_spotlist_t *spots = (pic_spotlist_t *)user_data;
    double title_fs = width / 12.0;   /* Same as all other applets */
    double data_fs = width / 16.0;    /* Smaller for dense feed data */
    double line_h = data_fs * 0.95 + 2;
    double total_h = title_fs * 1.5 + FEED_LINES * line_h;

    (void)now;

    if (!cr) return total_h;

    /* Title — consistent size across all applets */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, title_fs);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 0, title_fs);
    cairo_show_text(cr, "DX Cluster");

    /* Separator */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, title_fs * 1.3);
    cairo_line_to(cr, width, title_fs * 1.3);
    cairo_stroke(cr);

    /* Live feed — smaller font for data density */
    if (spots) {
        cairo_select_font_face(cr, "monospace",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, data_fs * 0.85);

        pthread_mutex_lock(&spots->mutex);
        {
            double fy = title_fs * 1.5;
            int fi;

            for (fi = 0; fi < spots->feed_count && fi < FEED_LINES; fi++) {
                /* Circular buffer index: feed_head points to the next
                 * write slot. To read oldest-first, start at
                 * (head - count) and advance by fi. The + FEED_LINES
                 * prevents negative modulo on some architectures. */
                int idx = (spots->feed_head - spots->feed_count + fi +
                          FEED_LINES) % FEED_LINES;
                /* Fade: oldest line = 0.3 alpha, newest = 0.9 alpha */
                double fade = 0.3 + 0.6 * ((double)fi / FEED_LINES);

                cairo_set_source_rgba(cr, 0.7, 0.8, 0.7, fade);
                cairo_move_to(cr, 0, fy + fi * line_h + data_fs * 0.85);
                cairo_show_text(cr, spots->feed[idx]);
            }
        }
        pthread_mutex_unlock(&spots->mutex);
    }

    return total_h;
}
