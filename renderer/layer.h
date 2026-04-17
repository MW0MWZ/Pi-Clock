/*
 * layer.h - Layer compositing system for Pi-Clock
 *
 * This file defines the layer abstraction used to build up the final
 * display image. Each visual element (map, daylight, grid, HUD, etc.)
 * is a separate layer with its own:
 *
 *   - Render function:  draws the layer content onto a Cairo surface
 *   - Update interval:  how often the layer needs to be re-rendered
 *   - Opacity:          alpha transparency for compositing (0.0 - 1.0)
 *   - Enabled flag:     whether the layer is visible at all
 *   - User data:        arbitrary pointer for passing resources (e.g.,
 *                        loaded map images) to the render function
 *
 * Layers are stored in a stack and composited bottom-to-top. The base
 * map sits at the bottom, and HUD elements sit on top. Each layer
 * renders to its own Cairo image surface (ARGB32), and the compositor
 * paints them in order onto a final output surface.
 *
 * This design means that layers with slow update intervals (e.g., the
 * base map which never changes) don't waste CPU time re-rendering
 * every frame. Only layers whose data has changed get re-drawn.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_LAYER_H
#define PIC_LAYER_H

#include <cairo/cairo.h>
#include <time.h>

/*
 * Maximum number of layers in the compositing stack.
 * Keeping this small and fixed avoids dynamic allocation
 * on a memory-constrained Pi Zero W (512MB RAM).
 */
#define PIC_MAX_LAYERS 24

/*
 * pic_layer_render_fn - Function pointer type for layer renderers.
 *
 * Each layer provides a function matching this signature. The compositor
 * calls it when the layer needs to be re-drawn. The function receives:
 *
 *   cr        - A Cairo context bound to the layer's surface. The function
 *               should draw its content here. The surface is pre-cleared
 *               to fully transparent before each call.
 *   width     - Output width in pixels (e.g., 1920 or 3840).
 *   height    - Output height in pixels (e.g., 1080 or 2160).
 *   now       - Current UTC time, used for time-dependent layers like
 *               the daylight overlay or HUD clock.
 *   user_data - Arbitrary pointer set when the layer was added. Used to
 *               pass resources like loaded map images or configuration
 *               to the render function without globals.
 */
typedef void (*pic_layer_render_fn)(cairo_t *cr, int width, int height,
                                    time_t now, void *user_data);

/*
 * pic_layer_t - A single compositing layer.
 *
 * Each layer owns a Cairo image surface that holds its rendered content.
 * The compositor manages the surface lifecycle (allocation, clearing,
 * and freeing).
 */
typedef struct {
    const char         *name;           /* Human-readable name for logging  */
    pic_layer_render_fn render;         /* Function that draws this layer   */
    pic_layer_render_fn render_legend;  /* Optional: draws legend overlay.
                                         * Called post-composite at full
                                         * opacity so legends are always
                                         * readable regardless of the
                                         * layer's opacity setting. NULL
                                         * if the layer has no legend.     */
    cairo_surface_t    *surface;        /* Backing ARGB32 image surface     */
    void               *user_data;     /* Passed to render function         */
    double              opacity;        /* Compositing alpha: 0.0 to 1.0   */
    volatile int        enabled;        /* Non-zero if layer is visible.
                                         * volatile: read by ticker thread,
                                         * written by main thread on reload. */
    int                 update_interval;/* Seconds between re-renders       */
    time_t              last_rendered;  /* Timestamp of last render (UTC)   */
    int                 dirty;          /* Set by update() when re-rendered */
} pic_layer_t;

/*
 * pic_layer_stack_t - The full compositing stack.
 *
 * Layers are stored in draw order: index 0 is drawn first (bottom)
 * and index (count-1) is drawn last (top). This mirrors the visual
 * stacking: the base map is at index 0, HUD at the highest index.
 */
typedef struct {
    pic_layer_t layers[PIC_MAX_LAYERS]; /* Fixed-size layer array          */
    int         count;                  /* Number of active layers         */
    int         width;                  /* Output width in pixels          */
    int         height;                 /* Output height in pixels         */
} pic_layer_stack_t;

/*
 * pic_layer_stack_init - Initialise a layer stack.
 *
 * Sets up an empty stack with the given output dimensions.
 * Must be called before adding any layers.
 *
 *   stack  - Pointer to the stack to initialise.
 *   width  - Output width in pixels.
 *   height - Output height in pixels.
 */
void pic_layer_stack_init(pic_layer_stack_t *stack, int width, int height);

/*
 * pic_layer_stack_add - Add a layer to the top of the stack.
 *
 * The new layer is placed above all existing layers. Its backing
 * surface is allocated here (ARGB32, matching the stack dimensions).
 *
 * Returns 0 on success, -1 if the stack is full or surface allocation
 * fails.
 *
 *   stack           - The layer stack to add to.
 *   name            - Human-readable name (pointer must remain valid).
 *   render          - Render function for this layer.
 *   update_interval - How often to re-render, in seconds. Use 0 for
 *                     layers that only render once (e.g., base map).
 *   opacity         - Initial opacity (0.0 fully transparent, 1.0 opaque).
 *   user_data       - Arbitrary pointer passed to the render function.
 *                     The layer does NOT take ownership — the caller is
 *                     responsible for freeing it after the stack is destroyed.
 */
int pic_layer_stack_add(pic_layer_stack_t *stack, const char *name,
                        pic_layer_render_fn render, int update_interval,
                        double opacity, void *user_data);

/*
 * pic_layer_stack_update - Re-render any layers that are due for update.
 *
 * Walks the stack and checks each enabled layer's last_rendered time
 * against its update_interval. If enough time has passed (or the layer
 * has never been rendered), its render function is called and the
 * layer's dirty flag is set.
 *
 * This is the main per-frame call in the render loop.
 *
 *   stack - The layer stack to update.
 *   now   - Current UTC time.
 *
 * Returns the number of layers that were re-rendered this tick.
 * Callers can use this to skip compositing when nothing changed.
 */
int pic_layer_stack_update(pic_layer_stack_t *stack, time_t now);

/*
 * pic_layer_stack_composite - Composite all layers onto an output surface.
 *
 * Paints each enabled layer's surface onto the destination Cairo context,
 * in stack order (bottom to top), respecting each layer's opacity.
 *
 *   stack - The layer stack to composite.
 *   cr    - Cairo context bound to the output surface.
 */
void pic_layer_stack_composite(pic_layer_stack_t *stack, cairo_t *cr);

/*
 * pic_layer_stack_render_legends - Draw all layer legends at full opacity.
 *
 * Called AFTER pic_layer_stack_composite(). Iterates enabled layers
 * and calls each layer's render_legend function (if set) directly
 * onto the output surface. This ensures legends are always readable
 * regardless of layer opacity.
 *
 *   stack - The layer stack.
 *   cr    - Cairo context bound to the output surface.
 *   now   - Current UTC time (passed to legend render functions).
 */
void pic_layer_stack_render_legends(pic_layer_stack_t *stack, cairo_t *cr,
                                    time_t now);

/*
 * pic_layer_set_legend - Set the legend render function for a named layer.
 *
 *   stack  - The layer stack.
 *   name   - Layer name to set the legend for.
 *   legend - Legend render function, or NULL to clear.
 */
void pic_layer_set_legend(pic_layer_stack_t *stack, const char *name,
                          pic_layer_render_fn legend);

/*
 * pic_layer_free_surface - Free a single layer's backing surface.
 *
 * Called when a layer is disabled to reclaim memory. On a 256MB
 * Pi 1 Model A at 1080p, each surface is ~8MB — freeing disabled
 * layers saves significant RAM. The surface is re-allocated
 * automatically if the layer is later re-enabled.
 */
void pic_layer_free_surface(pic_layer_t *layer);

/*
 * pic_layer_stack_destroy - Free all layer surfaces and reset the stack.
 *
 *   stack - The layer stack to clean up.
 */
void pic_layer_stack_destroy(pic_layer_stack_t *stack);

/*
 * pic_paint_viewport - Paint a full-globe source surface through the
 * current viewport onto the Cairo context.
 *
 * The source is assumed to cover the whole world (src_w pixels of
 * longitude from -180° to +180°, src_h pixels of latitude from +90°
 * to -90°). This helper builds a pattern matrix that maps destination
 * pixels (0..width, 0..height) to the correct source pixels given
 * pic_config.view_center_lat/lon and view_span_lat/lon, then paints
 * the result.
 *
 * Longitudinal wrapping is handled via CAIRO_EXTEND_REPEAT — the
 * pattern tiles in both axes, but because the viewport latitude is
 * clamped to stay inside [-90°, +90°], only the longitude wrap
 * actually fires.
 *
 * Used by layers_base (Black Marble), layers_daylight (Blue Marble
 * and the solar illumination mask), and the grid-based overlays
 * (cloud, precip, aurora). Keeping one place for this math means
 * only one file needs updating when the projection evolves.
 *
 *   cr     - Destination Cairo context.
 *   src    - Full-globe source surface (any format: ARGB32 or A8).
 *   width  - Destination width in pixels.
 *   height - Destination height in pixels.
 */
void pic_paint_viewport(cairo_t *cr, cairo_surface_t *src,
                        int width, int height);

/*
 * pic_viewport_pattern - Build a Cairo pattern that samples `src`
 * through the current viewport, with CAIRO_EXTEND_REPEAT + bilinear
 * filter already applied.
 *
 * Exposed so callers that need a custom paint operator (the daylight
 * layer uses cairo_mask_surface rather than cairo_paint) can reuse
 * the same matrix math as pic_paint_viewport. Caller owns the
 * returned pattern and must cairo_pattern_destroy it.
 *
 * Returns NULL if pattern creation fails.
 */
cairo_pattern_t *pic_viewport_pattern(cairo_surface_t *src,
                                      int width, int height);

#endif /* PIC_LAYER_H */
