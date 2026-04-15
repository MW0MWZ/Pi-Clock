/*
 * applet.h - Applet system for overlay information panels
 *
 * Applets are semi-transparent information panels that stack
 * vertically along screen edges. They are separate from map
 * layers — layers draw on the map, applets overlay on top.
 *
 * Each applet declares a minimum width and a screen side
 * (left or right). All applets on the same side share the
 * same width — the widest min_width on that side wins.
 * Panels stack bottom-to-top from the bottom corner.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_APPLET_H
#define PIC_APPLET_H

#include <cairo/cairo.h>
#include <time.h>

#define MAX_APPLETS 8

/* Screen side for applet positioning */
#define APPLET_SIDE_RIGHT 0
#define APPLET_SIDE_LEFT  1

/*
 * pic_applet_render_fn - Renders an applet's content.
 *
 * The function draws into a Cairo context positioned at the
 * applet's top-left corner. It should draw within the given
 * width and return the actual height used.
 *
 *   cr        - Cairo context (NULL for measuring pass)
 *   width     - Available width in pixels
 *   now       - Current UTC time
 *   user_data - Applet-specific data
 *
 * Returns the height consumed by this applet.
 */
typedef double (*pic_applet_render_fn)(cairo_t *cr, double width,
                                      time_t now, void *user_data);

typedef struct {
    const char          *name;      /* ID for config matching         */
    const char          *label;     /* Display name for dashboard     */
    pic_applet_render_fn render;    /* Render function                */
    void                *user_data; /* Passed to render function      */
    int                  enabled;   /* Non-zero if visible            */
    int                  side;      /* APPLET_SIDE_LEFT or _RIGHT     */
    double               min_width; /* Minimum panel width in pixels  */
    int                  update_interval; /* Seconds between redraws  */
    time_t               last_rendered;
} pic_applet_t;

typedef struct {
    pic_applet_t applets[MAX_APPLETS];
    int count;

    /* Optional pointer to ticker — if set, applet renderer writes
     * the left/right panel edges so the ticker can avoid overlap. */
    void *ticker;
} pic_applet_stack_t;

void pic_applet_stack_init(pic_applet_stack_t *stack);

int pic_applet_stack_add(pic_applet_stack_t *stack, const char *name,
                         const char *label, pic_applet_render_fn render,
                         int update_interval, double min_width, int side,
                         void *user_data);

/*
 * pic_applet_stack_render - Render all enabled applets.
 *
 * Draws applets stacked from the bottom corner upward on each
 * side. All applets on the same side share the same width
 * (the widest min_width determines the panel width for that side).
 *
 *   stack  - The applet stack
 *   cr     - Cairo context for the full display
 *   width  - Display width
 *   height - Display height
 *   now    - Current UTC time
 */
void pic_applet_stack_render(pic_applet_stack_t *stack, cairo_t *cr,
                             int width, int height, time_t now);

/*
 * pic_applet_load_config - Load applet settings from config.
 *
 * Reads /data/etc/pi-clock-applets.conf. Format per line:
 *   name=enabled,side    (e.g. dxfeed=1,right)
 */
void pic_applet_load_config(pic_applet_stack_t *stack);

#endif /* PIC_APPLET_H */
