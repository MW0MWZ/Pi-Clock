/*
 * main.c - Pi-Clock renderer entry point
 *
 * Supports two modes:
 *   1. Snapshot mode (--snapshot): render one frame to PNG
 *   2. Display mode (future): live SDL2 + OpenGL ES render loop
 *
 * Usage:
 *   pi-clock --snapshot output.png --maps-dir /maps
 *   pi-clock --snapshot output.png --maps-dir /maps --center-lon 139.7
 *   pi-clock --snapshot output.png --maps-dir /maps --zoom 60
 *   pi-clock --snapshot output.png --maps-dir /maps --time "2026-06-21 12:00:00"
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "image.h"
#include "config.h"
#include "borders.h"
#include "display.h"
#include <cairo/cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define MAX_PATH 512

/*
 * Forward declarations for layer render functions.
 * Each is defined in its own source file (layers_*.c).
 */
extern void pic_layer_render_basemap(cairo_t *cr, int width, int height,
                                     time_t now, void *user_data);
extern void pic_layer_render_daylight(cairo_t *cr, int width, int height,
                                      time_t now, void *user_data);
extern void pic_layer_render_grid(cairo_t *cr, int width, int height,
                                  time_t now, void *user_data);
extern void pic_layer_render_borders(cairo_t *cr, int width, int height,
                                     time_t now, void *user_data);
extern void pic_layer_render_timezone(cairo_t *cr, int width, int height,
                                      time_t now, void *user_data);
extern void pic_layer_render_hud(cairo_t *cr, int width, int height,
                                 time_t now, void *user_data);

/*
 * parse_utc_time - Parse a "YYYY-MM-DD HH:MM:SS" string to time_t.
 */
static time_t parse_utc_time(const char *str)
{
    struct tm tm;

    memset(&tm, 0, sizeof(tm));
    if (strptime(str, "%Y-%m-%d %H:%M:%S", &tm) == NULL) {
        return (time_t)-1;
    }
    return timegm(&tm);
}

static void print_usage(const char *progname)
{
    fprintf(stderr,
        "Pi-Clock Renderer\n"
        "\n"
        "Usage:\n"
        "  %s --snapshot <output.png> --maps-dir <dir> [options]\n"
        "\n"
        "Options:\n"
        "  --snapshot <file>      Render a single frame to PNG and exit\n"
        "  --maps-dir <dir>       Directory containing map images\n"
        "  --center-lon <deg>     Center longitude (-180 to 180, default: 0)\n"
        "  --zoom <0-100>         Map zoom percent, pans toward QTH (default: 0)\n"
        "  --qth-lat <deg>        QTH latitude for zoom pan (default: 0)\n"
        "  --qth-lon <deg>        QTH longitude for zoom pan (default: 0)\n"
        "  --width <pixels>       Output width (default: 1920)\n"
        "  --height <pixels>      Output height (default: 1080)\n"
        "  --time <datetime>      Render at specific UTC time\n"
        "                         Format: \"YYYY-MM-DD HH:MM:SS\"\n"
        "  --help                 Show this help message\n"
        "\n"
        "Examples:\n"
        "  %s --snapshot test.png --maps-dir /maps\n"
        "  %s --snapshot test.png --maps-dir /maps --center-lon -98.5\n"
        "  %s --snapshot test.png --maps-dir /maps --zoom 60 --qth-lat 52 --qth-lon -1\n",
        progname, progname, progname, progname);
}

/*
 * run_snapshot - Render a single frame and save to PNG.
 *
 * Layer stack (bottom to top):
 *   1. Basemap    — Black Marble (static, wraps with center_lon)
 *   2. Daylight   — Blue Marble masked by solar illumination
 *   3. Borders    — Country boundary lines
 *   4. Grid       — Lat/lon graticule
 *   5. HUD        — Clock, date, labels
 */
static int run_snapshot(const char *output_path, const char *maps_dir,
                        int width, int height, time_t render_time)
{
    pic_layer_stack_t stack;
    cairo_surface_t *output;
    cairo_surface_t *black_marble = NULL;
    cairo_surface_t *blue_marble = NULL;
    pic_borders_t borders;
    pic_borders_t timezones;
    int borders_loaded = 0;
    int timezones_loaded = 0;
    cairo_t *cr;
    cairo_status_t status = CAIRO_STATUS_SUCCESS;
    char path[MAX_PATH];

    printf("snapshot: rendering %dx%d frame "
           "(center_lon=%.1f zoom=%.0f%%)\n",
           width, height, pic_config.center_lon,
           pic_config.map_zoom * 100.0);

    /* Load map images. Snapshot is always run on dev hardware with
     * plenty of RAM, so always opt into the 4k source for zoom
     * headroom. */
    if (maps_dir) {
        printf("snapshot: loading maps from %s\n", maps_dir);
        black_marble = pic_map_load(maps_dir, "black_marble",
                                    "black_marble", width, height, 1);
        blue_marble = pic_map_load(maps_dir, "blue_marble",
                                   "blue_marble", width, height, 1);

        /* Load country border data */
        snprintf(path, sizeof(path), "%s/borders.dat", maps_dir);
        if (pic_borders_load(path, &borders) == 0) {
            borders_loaded = 1;
        }

        /* Load timezone boundary data */
        snprintf(path, sizeof(path), "%s/timezones.dat", maps_dir);
        if (pic_borders_load(path, &timezones) == 0) {
            timezones_loaded = 1;
        }
    }

    /* Create output surface */
    output = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(output) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "snapshot: failed to create output surface\n");
        return 1;
    }

    /* Build the layer stack */
    pic_layer_stack_init(&stack, width, height);

    if (pic_layer_stack_add(&stack, "basemap",
                            pic_layer_render_basemap, 0, 1.0,
                            black_marble) != 0) {
        goto cleanup;
    }

    if (pic_layer_stack_add(&stack, "daylight",
                            pic_layer_render_daylight, 60, 1.0,
                            blue_marble) != 0) {
        goto cleanup;
    }

    if (borders_loaded) {
        if (pic_layer_stack_add(&stack, "borders",
                                pic_layer_render_borders, 0, 1.0,
                                &borders) != 0) {
            goto cleanup;
        }
    }

    if (pic_layer_stack_add(&stack, "grid",
                            pic_layer_render_grid, 0, 1.0,
                            NULL) != 0) {
        goto cleanup;
    }

    if (pic_layer_stack_add(&stack, "timezone",
                            pic_layer_render_timezone, 0, 1.0,
                            timezones_loaded ? &timezones : NULL) != 0) {
        goto cleanup;
    }

    if (pic_layer_stack_add(&stack, "hud",
                            pic_layer_render_hud, 1, 1.0,
                            NULL) != 0) {
        goto cleanup;
    }

    /* Render and composite */
    printf("snapshot: rendering layers at UTC %s", ctime(&render_time));
    pic_layer_stack_update(&stack, render_time);

    cr = cairo_create(output);
    pic_layer_stack_composite(&stack, cr);
    cairo_destroy(cr);

    /* Write PNG */
    status = cairo_surface_write_to_png(output, output_path);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "snapshot: failed to write PNG: %s\n",
                cairo_status_to_string(status));
    } else {
        printf("snapshot: saved to %s\n", output_path);
    }

cleanup:
    pic_layer_stack_destroy(&stack);
    cairo_surface_destroy(output);
    if (black_marble) cairo_surface_destroy(black_marble);
    if (blue_marble) cairo_surface_destroy(blue_marble);
    if (borders_loaded) pic_borders_free(&borders);
    if (timezones_loaded) pic_borders_free(&timezones);

    return (status == CAIRO_STATUS_SUCCESS) ? 0 : 1;
}

int main(int argc, char *argv[])
{
    const char *snapshot_path = NULL;
    const char *maps_dir = NULL;
    int width = 1920;
    int height = 1080;
    time_t render_time = time(NULL);
    int i;

    /* Seed pic_config from the saved renderer config file.
     *
     * This picks up dashboard-written values (CENTER_LON, MAP_ZOOM,
     * QTH_LAT/LON) without requiring the init script to forward them
     * as CLI flags. CLI flags below still win because they run after.
     *
     * On a clean install the file doesn't exist yet and pic_load_renderer_conf
     * is a no-op, so pic_config keeps its compile-time defaults.
     */
    {
        pic_renderer_conf_t rconf;
        pic_load_renderer_conf(&rconf);
        pic_config.center_lon = rconf.center_lon;
        pic_config.map_zoom   = rconf.map_zoom;
        pic_config.qth_lat    = rconf.qth_lat;
        pic_config.qth_lon    = rconf.qth_lon;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot_path = argv[++i];
        } else if (strcmp(argv[i], "--maps-dir") == 0 && i + 1 < argc) {
            maps_dir = argv[++i];
        } else if (strcmp(argv[i], "--center-lon") == 0 && i + 1 < argc) {
            double v = strtod(argv[++i], NULL);
            /* isfinite guards against nan/inf slipping through
             * the clamps below (nan compares false to everything). */
            if (isfinite(v)) pic_config.center_lon = v;
        } else if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            double z = strtod(argv[++i], NULL);
            if (!isfinite(z)) z = 0.0;
            if (z < 0.0)   z = 0.0;
            if (z > 100.0) z = 100.0;
            pic_config.map_zoom = z / 100.0;
        } else if (strcmp(argv[i], "--qth-lat") == 0 && i + 1 < argc) {
            double v = strtod(argv[++i], NULL);
            if (isfinite(v)) pic_config.qth_lat = v;
        } else if (strcmp(argv[i], "--qth-lon") == 0 && i + 1 < argc) {
            double v = strtod(argv[++i], NULL);
            if (isfinite(v)) pic_config.qth_lon = v;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            render_time = parse_utc_time(argv[++i]);
            if (render_time == (time_t)-1) {
                fprintf(stderr, "error: invalid time format '%s'\n",
                        argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Derive viewport from whatever combination of conf+CLI landed
     * in pic_config. Both snapshot and display-mode rely on the
     * derived view_* fields for all projection/paint math. */
    pic_config_recompute_viewport();

    if (snapshot_path) {
        return run_snapshot(snapshot_path, maps_dir, width, height,
                           render_time);
    }

    /*
     * Display mode: run the SDL2 render loop.
     * This drives the HDMI output on the Pi via KMSDRM, or opens
     * a window on a desktop system via X11/Wayland.
     *
     * If width/height are the defaults (1920x1080) and no explicit
     * --width/--height was given, pass 0 to auto-detect from the
     * connected display.
     */
    if (!maps_dir) {
        /* Try the standard install location */
        maps_dir = "/usr/share/pi-clock/maps";
    }
    return pic_display_run(maps_dir, width, height);
}
