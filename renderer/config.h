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
 *                should appear at the horizontal centre of the map.
 *                Default is 0.0 (Prime Meridian / Greenwich).
 *
 *                Setting this to, say, 139.7 (Tokyo) shifts the
 *                map so that Tokyo is in the centre and the seam
 *                appears in the Atlantic Ocean instead of at the
 *                date line.
 *
 *                This affects all layers: base map images are
 *                wrapped, coordinate-based overlays (grid, borders,
 *                daylight mask) adjust their projection math.
 */
typedef struct {
    double center_lon;
} pic_config_t;

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
} pic_renderer_conf_t;

/*
 * pic_load_renderer_conf - Parse the renderer config file.
 * Fills out the struct with values found, leaves defaults for missing keys.
 */
void pic_load_renderer_conf(pic_renderer_conf_t *conf);

int pic_wait_for_network(const char *label, const volatile int *running);

#endif /* PIC_CONFIG_H */
