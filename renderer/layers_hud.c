/*
 * layers_hud.c - Heads-up display layer (clocks, date, labels)
 *
 * Topmost layer in the compositing stack. Draws:
 *
 *   - UTC time and date (top-left)
 *   - Local time and date (top-right)
 *
 * Local time uses the system's configured timezone (set via
 * pi-clock-config.txt or the dashboard). The timezone is read from
 * /etc/timezone and applied via the TZ environment variable.
 *
 * Updates every second (update_interval = 1).
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * draw_text_with_shadow - Draw text with a dark shadow for readability.
 */
static void draw_text_with_shadow(cairo_t *cr, const char *text,
                                  double x, double y)
{
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
    cairo_move_to(cr, x + 2, y + 2);
    cairo_show_text(cr, text);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
}

/*
 * draw_text_right_aligned - Draw right-aligned text with shadow.
 *
 * Measures the text width first, then positions it so the right
 * edge aligns with the given x coordinate.
 */
static void draw_text_right_aligned(cairo_t *cr, const char *text,
                                    double right_x, double y)
{
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);
    draw_text_with_shadow(cr, text, right_x - extents.width, y);
}

/*
 * get_local_tz_abbr - Get the local timezone abbreviation.
 *
 * Uses strftime("%Z") to get the abbreviation (e.g., "BST", "EDT").
 * Falls back to "UTC±N" format when the TZ is set to a POSIX offset
 * string like "UTC-7" — strftime returns just "UTC" in that case,
 * which is misleading. We detect this by comparing the UTC/local
 * hour difference and synthesise a proper "UTC-7" label.
 */
static const char *get_local_tz_abbr(time_t now)
{
    static char tz_abbr[16];
    struct tm utc_copy, *local_tm;
    struct tm *tmp;
    int diff_h;

    /*
     * POSIX gotcha: gmtime() and localtime() may return pointers
     * to the same internal static struct tm. Calling localtime()
     * would overwrite the gmtime() result. We copy the UTC values
     * into a stack variable before the second call.
     */
    tmp = gmtime(&now);
    if (!tmp) return "LCL";
    utc_copy = *tmp;
    diff_h = utc_copy.tm_hour;

    local_tm = localtime(&now);
    if (!local_tm) return "LCL";

    /* Try strftime — works well with Olsen names (e.g. "Europe/London" → "BST") */
    if (strftime(tz_abbr, sizeof(tz_abbr), "%Z", local_tm) > 0) {
        /*
         * Detect the POSIX offset problem: when TZ="UTC-7" (Mountain),
         * strftime("%Z") returns "UTC" — hiding the actual offset.
         * Compare the UTC and local hours to detect this mismatch.
         * Wrap the difference to handle midnight crossings.
         */
        diff_h = local_tm->tm_hour - diff_h;
        if (diff_h > 12) diff_h -= 24;
        if (diff_h < -12) diff_h += 24;

        if (diff_h != 0 && strcmp(tz_abbr, "UTC") == 0) {
            /* There IS an offset but strftime hid it — make it explicit */
            snprintf(tz_abbr, sizeof(tz_abbr), "UTC%+d", diff_h);
        }
        return tz_abbr;
    }
    return "LCL";
}

/*
 * is_single_core - Detect single-core Pi (Zero, Zero W, Pi 1).
 * Checked once, cached.
 *
 * On single-core models the renderer loop is already under heavy
 * load — updating the HUD every second with seconds display adds
 * unnecessary CPU pressure. Hide seconds, update once per minute.
 */
static int is_single_core(void)
{
    static int cached = -1;
    if (cached < 0) {
        FILE *f = fopen("/proc/cpuinfo", "r");
        int cores = 0;
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f))
                if (strncmp(line, "processor", 9) == 0) cores++;
            fclose(f);
        }
        cached = (cores <= 1) ? 1 : 0;
    }
    return cached;
}

/*
 * pic_layer_render_hud - Render the heads-up display.
 *
 * Left side:  UTC time + date
 * Right side: Local time + date (using system timezone)
 *
 * On the original Pi 1 (700MHz), seconds are hidden to reduce
 * CPU load — the HUD only needs to update once per minute.
 */
void pic_layer_render_hud(cairo_t *cr, int width, int height,
                          time_t now, void *user_data)
{
    struct tm *utc;
    struct tm *local;
    char utc_time_str[32];
    char utc_date_str[64];
    char local_time_str[48];
    char local_date_str[64];
    double font_size_time, font_size_date;
    double margin;
    const char *tz_abbr;
    int slow = is_single_core();

    (void)user_data;

    /* Get UTC time */
    utc = gmtime(&now);
    if (slow) {
        strftime(utc_time_str, sizeof(utc_time_str), "%H:%M UTC", utc);
    } else {
        strftime(utc_time_str, sizeof(utc_time_str), "%H:%M:%S UTC", utc);
    }
    strftime(utc_date_str, sizeof(utc_date_str), "%a %d %b %Y", utc);

    /* Get local time */
    tz_abbr = get_local_tz_abbr(now);
    local = localtime(&now);
    if (!local) return;  /* Corrupt TZ or out-of-range time_t */
    if (slow) {
        snprintf(local_time_str, sizeof(local_time_str), "%02d:%02d %s",
                 local->tm_hour, local->tm_min, tz_abbr);
    } else {
        snprintf(local_time_str, sizeof(local_time_str), "%02d:%02d:%02d %s",
                 local->tm_hour, local->tm_min, local->tm_sec, tz_abbr);
    }
    strftime(local_date_str, sizeof(local_date_str), "%a %d %b %Y", local);

    /* Scale font size relative to display height */
    font_size_time = height / 25.0;
    font_size_date = height / 50.0;
    margin = height / 40.0;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);

    /* ── Left side: UTC ──────────────────────────────────── */
    cairo_set_font_size(cr, font_size_time);
    draw_text_with_shadow(cr, utc_time_str,
                          margin, margin + font_size_time);

    cairo_set_font_size(cr, font_size_date);
    draw_text_with_shadow(cr, utc_date_str,
                          margin, margin + font_size_time + font_size_date + 5);

    /* ── Right side: Local time ──────────────────────────── */
    cairo_set_font_size(cr, font_size_time);
    draw_text_right_aligned(cr, local_time_str,
                            width - margin, margin + font_size_time);

    cairo_set_font_size(cr, font_size_date);
    draw_text_right_aligned(cr, local_date_str,
                            width - margin,
                            margin + font_size_time + font_size_date + 5);
}
