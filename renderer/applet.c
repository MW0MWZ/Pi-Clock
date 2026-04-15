/*
 * applet.c - Applet system implementation
 *
 * Side-panel overlay system for information widgets (DX feed, MUF,
 * solar weather). Key design points:
 *
 *   - All applets on the same side share one panel width (the widest
 *     min_width determines the column). Gives clean alignment.
 *   - Panel width is specified relative to 1920px and auto-scaled
 *     to the actual display resolution.
 *   - Each applet is called twice: cr=NULL to measure height, then
 *     with real cr to render (standard Cairo "measure then draw").
 *   - Applets stack upward from the bottom corner, beside the ticker.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet.h"
#include "newsticker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "math_utils.h"

/*
 * pic_applet_stack_init - Initialise an empty applet stack.
 *
 * After calling, set stack->ticker to the pic_ticker_t pointer if
 * applets should export margin data to the ticker bar.
 */
void pic_applet_stack_init(pic_applet_stack_t *stack)
{
    memset(stack, 0, sizeof(*stack));
}

/*
 * pic_applet_stack_add - Register an applet in the stack.
 *
 *   min_width - Minimum panel width in pixels at 1920×1080 reference.
 *               Auto-scaled at other resolutions.
 *   side      - APPLET_SIDE_LEFT or APPLET_SIDE_RIGHT.
 *   user_data - Passed to the render function (e.g., solar data pointer).
 *
 * Returns 0 on success, -1 if the stack is full (MAX_APPLETS).
 */
int pic_applet_stack_add(pic_applet_stack_t *stack, const char *name,
                         const char *label, pic_applet_render_fn render,
                         int update_interval, double min_width, int side,
                         void *user_data)
{
    pic_applet_t *a;

    if (stack->count >= MAX_APPLETS) return -1;

    a = &stack->applets[stack->count];
    a->name = name;
    a->label = label;
    a->render = render;
    a->user_data = user_data;
    a->enabled = 0;  /* Off by default — user enables via dashboard */
    a->side = side;
    a->min_width = min_width;
    a->update_interval = update_interval;
    a->last_rendered = 0;

    stack->count++;
    printf("applet: added '%s' (side=%s, min_w=%.0f)\n",
           name, side == APPLET_SIDE_LEFT ? "left" : "right", min_width);
    return 0;
}

/*
 * draw_rounded_rect - Draw a rounded rectangle path.
 */
static void draw_rounded_rect(cairo_t *cr, double x, double y,
                              double w, double h, double r)
{
    cairo_move_to(cr, x + r, y);
    cairo_line_to(cr, x + w - r, y);
    cairo_arc(cr, x + w - r, y + r, r, -PI/2, 0);
    cairo_line_to(cr, x + w, y + h - r);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI/2);
    cairo_line_to(cr, x + r, y + h);
    cairo_arc(cr, x + r, y + h - r, r, PI/2, PI);
    cairo_line_to(cr, x, y + r);
    cairo_arc(cr, x + r, y + r, r, PI, 3*PI/2);
    cairo_close_path(cr);
}

void pic_applet_stack_render(pic_applet_stack_t *stack, cairo_t *cr,
                             int width, int height, time_t now)
{
    /* Enable full font hinting — without this, Cairo defaults to
     * no hinting on headless systems, which makes text look soft
     * and blurry at 1080p. At 4K the pixels are small enough that
     * it doesn't matter, but at 1080p it's very noticeable. */
    {
        cairo_font_options_t *fo = cairo_font_options_create();
        cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
        cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
        cairo_set_font_options(cr, fo);
        cairo_font_options_destroy(fo);
    }

    /*
     * Layout metrics scale with display height so panels look the
     * same physical size at 1080p and 4K.
     *   margin  ≈ 1.25% of height (~13px at 1080p, ~27px at 4K)
     *   padding ≈ 0.83% of height (~9px at 1080p, ~18px at 4K)
     *   corner  = same as padding for rounded-rectangle radius
     */
    double margin = height / 80.0;
    double padding = height / 120.0;
    double corner = padding;
    double y_left, y_right;  /* Per-side bottom-up cursors */
    double w_left, w_right;  /* Per-side panel widths */
    int i;

    /*
     * Calculate panel width per side.
     *
     * min_width is specified relative to 1920px (the reference
     * resolution). Scale to actual screen width so panels look
     * the same physical size at 4K as they do at 1080p.
     */
    {
        double scale = (double)width / 1920.0;

        w_left = 0;
        w_right = 0;
        for (i = 0; i < stack->count; i++) {
            pic_applet_t *a = &stack->applets[i];
            double scaled_min;
            if (!a->enabled) continue;

            scaled_min = a->min_width * scale;
            if (a->side == APPLET_SIDE_LEFT) {
                if (scaled_min > w_left) w_left = scaled_min;
            } else {
                if (scaled_min > w_right) w_right = scaled_min;
            }
        }

        /* Fall back to 22% of screen if no min_width was set */
        if (w_left == 0)  w_left = width * 0.22;
        if (w_right == 0) w_right = width * 0.22;
    }

    /*
     * Export the panel edges to the ticker so it can centre itself
     * between the applets and avoid overlapping them.
     */
    if (stack->ticker) {
        pic_ticker_t *t = (pic_ticker_t *)stack->ticker;
        int has_left = 0, has_right = 0;

        for (i = 0; i < stack->count; i++) {
            if (!stack->applets[i].enabled) continue;
            if (stack->applets[i].side == APPLET_SIDE_LEFT) has_left = 1;
            else has_right = 1;
        }

        t->left_margin = has_left ? margin + w_left + margin * 0.5 : 0;
        t->right_margin = has_right ? margin + w_right + margin * 0.5 : 0;
    }

    /*
     * Applets sit on the sides, beside the ticker (not above it).
     * Their bottom edges align with the ticker bar's bottom edge.
     */
    {
        double bottom;
        if (stack->ticker) {
            pic_ticker_t *t = (pic_ticker_t *)stack->ticker;
            if (t->bar_bottom > 0) {
                bottom = t->bar_bottom;
            } else {
                bottom = height - margin;
            }
        } else {
            bottom = height - margin;
        }
        y_left = bottom;
        y_right = bottom;
    }

    for (i = stack->count - 1; i >= 0; i--) {
        pic_applet_t *a = &stack->applets[i];
        double applet_w, applet_h;
        double ax, ay;
        double *y_cursor;

        if (!a->enabled) continue;

        /* Pick side-specific width and cursor */
        if (a->side == APPLET_SIDE_LEFT) {
            applet_w = w_left;
            y_cursor = &y_left;
        } else {
            applet_w = w_right;
            y_cursor = &y_right;
        }

        cairo_save(cr);

        /* Measure height */
        applet_h = a->render(NULL, applet_w - padding * 2,
                             now, a->user_data);

        /* X position: left side or right side */
        if (a->side == APPLET_SIDE_LEFT) {
            ax = margin;
        } else {
            ax = width - applet_w - margin;
        }

        /* Y position: stacking upward from bottom */
        ay = *y_cursor - applet_h - padding * 2;

        /* Draw background — round positions to integer pixels */
        ax = (int)ax;
        ay = (int)ay;
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.05, 0.7);
        draw_rounded_rect(cr, ax, ay, (int)applet_w,
                          (int)(applet_h + padding * 2), (int)corner);
        cairo_fill(cr);

        /* Render content — round to integer pixels so text hinting
         * works correctly. Sub-pixel translate causes soft/blurry text. */
        cairo_translate(cr, (int)(ax + padding), (int)(ay + padding));
        a->render(cr, applet_w - padding * 2, now, a->user_data);

        cairo_restore(cr);

        /* Move cursor up for next applet on this side */
        *y_cursor = ay - margin * 0.5;
    }
}

#define APPLET_CONF "/data/etc/pi-clock-applets.conf"

void pic_applet_load_config(pic_applet_stack_t *stack)
{
    FILE *f;
    char line[128];
    int i;

    f = fopen(APPLET_CONF, "r");
    if (!f) return;

    printf("applet: loading settings from %s\n", APPLET_CONF);

    while (fgets(line, sizeof(line), f)) {
        char name[32];
        int enabled;
        char side_str[16];

        if (line[0] == '#' || line[0] == '\n') continue;

        /* Format: name=enabled,side  (e.g. dxfeed=1,right) */
        if (sscanf(line, "%31[^=]=%d,%15s", name, &enabled, side_str) >= 2) {
            for (i = 0; i < stack->count; i++) {
                if (strcmp(stack->applets[i].name, name) == 0) {
                    stack->applets[i].enabled = enabled;
                    if (strcmp(side_str, "left") == 0) {
                        stack->applets[i].side = APPLET_SIDE_LEFT;
                    } else if (strcmp(side_str, "right") == 0) {
                        stack->applets[i].side = APPLET_SIDE_RIGHT;
                    }
                    printf("applet: '%s' enabled=%d side=%s\n",
                           name, enabled, side_str);
                    break;
                }
            }
        }
    }

    fclose(f);
}
