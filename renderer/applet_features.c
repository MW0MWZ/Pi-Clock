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
 * friendly_name - Map internal layer/applet names to display labels.
 */
static const char *friendly_name(const char *name)
{
    static const struct { const char *key; const char *label; } names[] = {
        /* Layers */
        {"basemap",    "Base Map"},
        {"daylight",   "Daylight"},
        {"borders",    "Borders"},
        {"grid",       "Lat/Lon Grid"},
        {"timezone",   "Time Zones"},
        {"cqzone",     "CQ Zones"},
        {"maidenhead", "Maidenhead"},
        {"bandconditions","Propagation Map"},
        {"dxspots",    "DX Spots"},
        {"satellites", "Satellites"},
        {"lightning",  "Lightning"},
        {"earthquakes","Earthquakes"},
        {"precip",     "Rain Radar"},
        {"cloud",      "Cloud Cover"},
        {"wind",       "Wind"},
        {"aurora",     "Aurora"},
        {"qth",        "Home QTH"},
        {"sun",        "Sun"},
        {"moon",       "Moon"},
        {"ticker",     "News Ticker"},
        {"hud",        "Clock / HUD"},
        /* Applets */
        {"dxfeed",     "DX Feed"},
        {"muf",        "MUF Estimate"},
        {"voacap",     "Propagation Prediction"},
        {"solar",      "Solar Weather"},
        {"sysinfo",    "System Info"},
        {"features",   "Features"},
        {NULL, NULL}
    };
    int i;
    for (i = 0; names[i].key; i++) {
        if (strcmp(name, names[i].key) == 0) return names[i].label;
    }
    return name;
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

    /* Total height: title + separator + layers header + items + applets header + items */
    {
        int layer_enabled = 0, applet_enabled = 0;
        for (i = 0; i < layers->count; i++)
            if (layers->layers[i].enabled) layer_enabled++;
        for (i = 0; i < applets->count; i++)
            if (applets->applets[i].enabled) applet_enabled++;

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
        for (i = 0; i < layers->count; i++) {
            if (!layers->layers[i].enabled) continue;
            any = 1;

            /* Green dot + name */
            cairo_set_source_rgba(cr, 0.2, 0.85, 0.3, 0.9);
            cairo_arc(cr, fs * 0.3, y - fs * 0.2, fs * 0.15, 0, 6.283);
            cairo_fill(cr);

            cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
            cairo_move_to(cr, fs * 0.7, y);
            cairo_show_text(cr, friendly_name(layers->layers[i].name));
            y += line_h;
        }
        if (!any) {
            cairo_set_source_rgba(cr, 0.5, 0.5, 0.6, 0.6);
            cairo_move_to(cr, fs * 0.7, y);
            cairo_show_text(cr, "None");
            y += line_h;
        }
    }

    /* Applets section — only show if any are enabled */
    {
        int any = 0;
        for (i = 0; i < applets->count; i++) {
            if (applets->applets[i].enabled) { any = 1; break; }
        }
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

            for (i = 0; i < applets->count; i++) {
                if (!applets->applets[i].enabled) continue;

                cairo_set_source_rgba(cr, 0.3, 0.7, 0.95, 0.9);
                cairo_arc(cr, fs * 0.3, y - fs * 0.2, fs * 0.15, 0, 6.283);
                cairo_fill(cr);

                cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
                cairo_move_to(cr, fs * 0.7, y);
                cairo_show_text(cr, friendly_name(applets->applets[i].name));
                y += line_h;
            }
        }
    }

    return y + fs * 0.3;
}
