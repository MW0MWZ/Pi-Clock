/*
 * applet_features.c - Enabled features/layers applet
 *
 * Shows which map layers and overlay panels are currently
 * enabled. Provides a quick visual reference of what's active
 * without needing to check the dashboard.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet_sysinfo.h"
#include <stdio.h>
#include <string.h>

/*
 * Layer / applet display order + labels.
 *
 * Both arrays MUST match dashboard/internal/api/handlers.go's
 * layerDefs and appletDefs (same names in the same order). The
 * on-screen Active Features panel walks these arrays in order
 * and looks each one up in the live stack — so the user sees
 * the same ordering and wording on the Pi as in the web UI.
 *
 * Layers in the live stack are added in *render* order (bottom
 * to top: basemap, daylight, ...) which is not the same as the
 * UI's reading order (Day Map first, then Night Map, etc.). The
 * lookup-by-name decoupling lets the two stay independent.
 */
static const struct { const char *key; const char *label; } layer_order[] = {
    {"daylight",      "Day Map"},
    {"basemap",       "Night Map"},
    {"borders",       "Country Borders"},
    {"grid",          "Lat/Lon Grid"},
    {"timezone",      "Time Zones"},
    {"cqzone",        "CQ Zones"},
    {"ituzone",       "ITU Zones"},
    {"maidenhead",    "Maidenhead Grid"},
    {"bandconditions","Propagation Prediction"},
    {"dxspots",       "DX Cluster Spots"},
    {"satellites",    "Satellite Tracker"},
    {"lightning",     "Lightning Strikes"},
    {"earthquakes",   "Earthquakes"},
    {"precip",        "Rain Radar"},
    {"cloud",         "Cloud Cover"},
    {"wind",          "Wind"},
    {"aurora",        "Aurora"},
    {"qth",           "Home QTH Marker"},
    {"sun",           "Sun Position"},
    {"moon",          "Moon Position"},
    {"ticker",        "News Ticker"},
    {"hud",           "Clock / HUD"},
    {NULL, NULL}
};

static const struct { const char *key; const char *label; } applet_order[] = {
    {"dxfeed",        "DX Cluster Feed"},
    {"muf",           "MUF Estimate"},
    {"voacap",        "Propagation Prediction"},
    {"solar",         "Solar Weather"},
    {"sysinfo",       "System Info"},
    {"features",      "Active Features"},
    {NULL, NULL}
};

/*
 * is_layer_enabled - Look up `name` in the live layer stack and
 * return its enabled flag, or 0 if not present.
 */
static int is_layer_enabled(pic_layer_stack_t *stack, const char *name)
{
    int i;
    for (i = 0; i < stack->count; i++) {
        if (strcmp(stack->layers[i].name, name) == 0) {
            return stack->layers[i].enabled;
        }
    }
    return 0;
}

/*
 * applet_label - Look up the dashboard-friendly label for `name`.
 * Falls back to whatever label the live applet has if it isn't
 * listed in applet_order (defensive for future renames).
 */
static const char *applet_label(const char *name, const char *fallback)
{
    int i;
    for (i = 0; applet_order[i].key; i++) {
        if (strcmp(name, applet_order[i].key) == 0) {
            return applet_order[i].label;
        }
    }
    return fallback;
}

/*
 * count_enabled_applets - Total enabled across both sides.
 */
static int count_enabled_applets(pic_applet_stack_t *stack)
{
    int i, n = 0;
    for (i = 0; i < stack->count; i++) {
        if (stack->applets[i].enabled) n++;
    }
    return n;
}

/*
 * pic_applet_render_features - Render the enabled features panel.
 */
double pic_applet_render_features(cairo_t *cr, double width,
                                  time_t now, void *user_data)
{
    pic_sysinfo_t *info = (pic_sysinfo_t *)user_data;
    pic_layer_stack_t *layers;
    pic_applet_stack_t *applets;
    double fs = width / 12.0;
    double line_h = fs * 1.4;
    double y;
    int i;

    (void)now;

    if (!info || !info->layer_stack || !info->applet_stack) {
        return fs * 3;
    }

    layers = info->layer_stack;
    applets = (pic_applet_stack_t *)info->applet_stack;

    /* Total height: title + separator + layers header + items + applets header + items.
     * Layer count walks the dashboard-defined display order; applet
     * count walks the live stack since applet ordering is dynamic
     * (user drag-and-drop in the dashboard). */
    {
        int layer_enabled = 0, applet_enabled;
        for (i = 0; layer_order[i].key; i++)
            if (is_layer_enabled(layers, layer_order[i].key)) layer_enabled++;
        applet_enabled = count_enabled_applets(applets);

        double total_h = fs * 2.0;  /* title + separator */
        total_h += line_h;          /* "Layers" header */
        total_h += line_h * (layer_enabled > 0 ? layer_enabled : 1);
        if (applet_enabled > 0) {
            total_h += line_h * 0.6; /* gap */
            total_h += line_h;       /* "Applets" header */
            total_h += line_h * applet_enabled;
        }
        /* No extra bottom padding — match MUF/DX Feed style */

        if (!cr) return total_h;
    }

    /* Title */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 0, fs);
    cairo_show_text(cr, "Active Features");

    /* Separator */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, fs + 4);
    cairo_line_to(cr, width, fs + 4);
    cairo_stroke(cr);

    y = fs * 2.0;

    /* Layers section */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs * 0.9);
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.8, 0.8);
    cairo_move_to(cr, 0, y);
    cairo_show_text(cr, "Layers");
    y += line_h;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.9);

    {
        int any = 0;
        /* Iterate the dashboard-defined order so the user sees the
         * same layer ordering on the screen as in the web UI. */
        for (i = 0; layer_order[i].key; i++) {
            if (!is_layer_enabled(layers, layer_order[i].key)) continue;
            any = 1;

            /* Green dot + name */
            cairo_set_source_rgba(cr, 0.2, 0.85, 0.3, 0.9);
            cairo_arc(cr, fs * 0.3, y - fs * 0.2, fs * 0.15, 0, 6.283);
            cairo_fill(cr);

            cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
            cairo_move_to(cr, fs * 0.7, y);
            cairo_show_text(cr, layer_order[i].label);
            y += line_h;
        }
        if (!any) {
            cairo_set_source_rgba(cr, 0.5, 0.5, 0.6, 0.6);
            cairo_move_to(cr, fs * 0.7, y);
            cairo_show_text(cr, "None");
            y += line_h;
        }
    }

    /* Applets section — only show if any are enabled.
     *
     * Reading order on screen: left column top-to-bottom, then
     * right column top-to-bottom. The live applet stack stores
     * stack-order (== top-to-bottom on each side) plus a side
     * tag, so we iterate it twice — first emitting LEFT, then
     * RIGHT — to mirror what the user actually sees on the Pi
     * display. */
    {
        int any = (count_enabled_applets(applets) > 0);
        if (any) {
            y += line_h * 0.4;

            cairo_select_font_face(cr, "sans-serif",
                                   CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs * 0.9);
            cairo_set_source_rgba(cr, 0.7, 0.7, 0.8, 0.8);
            cairo_move_to(cr, 0, y);
            cairo_show_text(cr, "Applets");
            y += line_h;

            cairo_select_font_face(cr, "sans-serif",
                                   CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, fs * 0.9);

            /* Two passes: LEFT side first (in stack order), then RIGHT.
             * Stack index order = visual top-to-bottom on each side,
             * so this gives a "read the left column, then the right
             * column" sequence matching the on-screen layout. */
            int pass;
            for (pass = 0; pass < 2; pass++) {
                int wanted_side = (pass == 0) ? APPLET_SIDE_LEFT
                                              : APPLET_SIDE_RIGHT;
                for (i = 0; i < applets->count; i++) {
                    pic_applet_t *a = &applets->applets[i];
                    if (!a->enabled) continue;
                    if (a->side != wanted_side) continue;

                    cairo_set_source_rgba(cr, 0.3, 0.7, 0.95, 0.9);
                    cairo_arc(cr, fs * 0.3, y - fs * 0.2, fs * 0.15, 0, 6.283);
                    cairo_fill(cr);

                    cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
                    cairo_move_to(cr, fs * 0.7, y);
                    cairo_show_text(cr, applet_label(a->name, a->label));
                    y += line_h;
                }
            }
        }
    }

    return y + fs * 0.3;
}
