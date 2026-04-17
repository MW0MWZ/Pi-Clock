/*
 * config.h - Global renderer configuration
 *
 * Holds display settings that affect all layers, such as the
 * center longitude for map wrapping. This is intentionally global
 * rather than threaded through every function call — Pi-Clock
 * is a single-purpose display application running one instance,
 * so global config is simple and appropriate.
 *
 * Values are set once at startup from command-line arguments
 * (and eventually from the dashboard config file) and read by
 * all layers during rendering.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_CONFIG_H
#define PIC_CONFIG_H

/*
 * pic_config_t - Global renderer configuration.
 *
 *   center_lon - The longitude (in degrees, -180 to +180) that
 *                should appear at the horizontal centre of the map
 *                at zoom=0. Default 0.0 (Prime Meridian).
 *
 *   map_zoom   - Zoom fraction 0.0..1.0 (saved as MAP_ZOOM=0..100
 *                percent in the renderer config). 0 = full world
 *                view, 1 = maximum zoom (factor PIC_ZOOM_MAX, see
 *                pic_zoom_factor()), progressively panning toward
 *                QTH as zoom increases.
 *
 *   qth_lat,
 *   qth_lon    - Home QTH used as the pan target during zoom. If
 *                either QTH is at exactly 0 (the "unset" sentinel
 *                in the rest of the renderer) pan-to-QTH is disabled
 *                and the viewport stays centred on center_lon/0°.
 *
 *   view_center_lat,
 *   view_center_lon - The lat/lon at the centre of the visible
 *                     viewport. Derived from center_lon, map_zoom,
 *                     and qth_* by pic_config_recompute_viewport().
 *
 *   view_span_lat,
 *   view_span_lon   - Degrees of latitude/longitude covered by the
 *                     full screen height/width at the current zoom.
 *                     At zoom=0 these are 180/360 (whole world),
 *                     shrinking linearly toward 180/PIC_ZOOM_MAX
 *                     and 360/PIC_ZOOM_MAX at zoom=1.
 *
 *   viewport_gen    - Incremented every time the viewport changes
 *                     (via pic_config_recompute_viewport). Layers
 *                     with internal caches (cloud/precip/aurora/
 *                     wind) compare the generation to decide when
 *                     to rebuild.
 */
typedef struct {
    double center_lon;
    double map_zoom;
    double qth_lat;
    double qth_lon;

    double view_center_lat;
    double view_center_lon;
    double view_span_lat;
    double view_span_lon;

    unsigned int viewport_gen;
} pic_config_t;

/* Maximum zoom factor at map_zoom=1.0.
 *
 * At 3x the viewport shows 120° longitude × 60° latitude — roughly
 * a continent-plus-ocean view from a mid-latitude QTH (e.g. all of
 * Europe + the North Atlantic + the eastern USA from a UK QTH).
 *
 * Earlier iterations tried 6x and 4x but both clipped too tight
 * for a dashboard-scale overview. If you change this, update the
 * dashboard hint text too (templates/dashboard.html).
 */
#define PIC_ZOOM_MAX 3.0

/*
 * pic_zoom_factor - Current zoom factor (1.0..PIC_ZOOM_MAX).
 *
 * Linear interpolation from 1.0 at map_zoom=0 to PIC_ZOOM_MAX at
 * map_zoom=1. At the returned factor z: view_span_lon = 360/z and
 * view_span_lat = 180/z.
 */
double pic_zoom_factor(void);

/*
 * pic_wrap_threshold_px - Pixel distance above which two consecutive
 * projected points are considered to be on opposite sides of the
 * longitudinal seam (and the line between them should be broken).
 *
 * At zoom=1 (full world), this is width/2 — matching the previous
 * hard-coded heuristic. At zoom=z, view_span_lon = 360/z, so 180°
 * of angular separation maps to (180/view_span_lon) * width =
 * (z/2) * width screen pixels.
 *
 * Callers: layers_borders.c, layers_cqzone.c, layers_ituzone.c,
 * layers_timezone.c, layers_wind.c — anywhere a polyline segment
 * needs wrap detection.
 */
double pic_wrap_threshold_px(int width);

/*
 * pic_config_recompute_viewport - Derive viewport fields from the
 * primary inputs (center_lon, map_zoom, qth_lat, qth_lon). Also
 * bumps viewport_gen so layer caches invalidate.
 *
 * Call after any change to those inputs — at startup, in --snapshot
 * CLI parsing, and on SIGHUP reload. Safe to call repeatedly; only
 * derived state is affected.
 */
void pic_config_recompute_viewport(void);

/* Global config instance — defined in config.c, read everywhere */
extern pic_config_t pic_config;

/*
 * pic_lon_to_x - Convert a longitude to an X pixel coordinate.
 *
 * This is the central projection function that all layers use to
 * convert geographic longitude to screen position. It applies the
 * center_lon offset and wraps correctly across the date line.
 *
 *   lon_deg - Longitude in degrees (-180 to +180, east positive).
 *   width   - Output width in pixels.
 *
 * Returns an X coordinate in the range [0, width). Values outside
 * this range have been wrapped.
 */
double pic_lon_to_x(double lon_deg, int width);

/*
 * pic_lat_to_y - Convert a latitude to a Y pixel coordinate.
 *
 * Standard equirectangular projection: latitude maps linearly to Y.
 * North is at the top (Y=0), south at the bottom (Y=height).
 *
 *   lat_deg - Latitude in degrees (-90 to +90, north positive).
 *   height  - Output height in pixels.
 *
 * Returns a Y coordinate in the range [0, height).
 */
double pic_lat_to_y(double lat_deg, int height);

/*
 * pic_x_to_lon - Convert an X pixel coordinate back to longitude.
 *
 * Inverse of pic_lon_to_x(). Used by layers that need to convert
 * pixel positions back to geographic coordinates (e.g., the daylight
 * mask which computes solar illumination per pixel).
 *
 *   x     - X coordinate in pixels.
 *   width - Output width in pixels.
 *
 * Returns longitude in degrees (-180 to +180).
 */
double pic_x_to_lon(double x, int width);

/*
 * pic_y_to_lat - Convert a Y pixel coordinate back to latitude.
 *
 * Inverse of pic_lat_to_y().
 *
 *   y      - Y coordinate in pixels.
 *   height - Output height in pixels.
 *
 * Returns latitude in degrees (-90 to +90).
 */
double pic_y_to_lat(double y, int height);

/*
 * pic_load_layer_config - Apply layer settings from config file.
 *
 * Reads /data/etc/pi-clock-layers.conf and applies enable/disable and
 * opacity settings to matching layers in the stack. File format:
 *
 *   layername=enabled,opacity
 *   e.g., grid=1,0.5
 *         timezone=0,1.0
 *         borders=1,0.8
 *
 * Layers not mentioned in the file keep their current settings.
 */
#include "layer.h"
void pic_load_layer_config(pic_layer_stack_t *stack);

/*
 * pic_wait_for_network - Wait until an HTTP fetch works.
 *
 * Polls every 10 seconds up to max_attempts times. Used by
 * background fetch threads to wait for DNS/networking at boot.
 *
 *   label    - Log prefix (e.g. "solar", "ticker")
 *   running  - Pointer to the thread's running flag (stop if cleared)
 *
 * Returns 1 if network is ready, 0 if timed out or stopped.
 */
/*
 * pic_renderer_conf_t - Parsed renderer configuration values.
 * Loaded once from /data/etc/pi-clock-renderer.conf.
 */
typedef struct {
    char callsign[16];
    double qth_lat;
    double qth_lon;
    double center_lon;
    double map_zoom;     /* 0.0..1.0, parsed from MAP_ZOOM=0..100 */
} pic_renderer_conf_t;

/*
 * pic_load_renderer_conf - Parse the renderer config file.
 * Fills out the struct with values found, leaves defaults for missing keys.
 */
void pic_load_renderer_conf(pic_renderer_conf_t *conf);

int pic_wait_for_network(const char *label, const volatile int *running);

#endif /* PIC_CONFIG_H */
