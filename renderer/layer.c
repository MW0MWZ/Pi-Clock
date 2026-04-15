/*
 * layer.c - Layer compositing system implementation
 *
 * Implements the layer stack described in layer.h. The key design
 * goals are efficiency and minimal memory use on a Pi Zero W:
 *
 *   - Layers are only re-rendered when their update interval expires.
 *   - Each layer's surface is cleared to transparent before rendering,
 *     so layers only need to draw their own content (no background).
 *   - Compositing is a simple sequence of cairo_paint_with_alpha()
 *     calls, which Cairo handles efficiently with SIMD on ARM.
 *   - Surfaces are allocated on-demand when a layer first renders
 *     and freed when a layer is disabled. On a 256MB Pi 1 Model A,
 *     each 1080p ARGB32 surface is ~8MB — allocating only for
 *     enabled layers saves 50-80MB of RAM.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include <stdio.h>
#include <string.h>

/*
 * pic_layer_stack_init - Set up an empty layer stack.
 *
 * Zeroes out the entire stack structure and stores the output
 * dimensions. No surfaces are allocated until layers are added.
 */
void pic_layer_stack_init(pic_layer_stack_t *stack, int width, int height)
{
    memset(stack, 0, sizeof(*stack));
    stack->width = width;
    stack->height = height;
}

/*
 * pic_layer_stack_add - Add a new layer to the top of the stack.
 *
 * Does NOT allocate a Cairo surface — surfaces are allocated
 * on-demand when the layer first renders (in pic_layer_stack_update).
 * This means disabled layers consume zero memory beyond their
 * 64-byte layer_t record.
 */
int pic_layer_stack_add(pic_layer_stack_t *stack, const char *name,
                        pic_layer_render_fn render, int update_interval,
                        double opacity, void *user_data)
{
    pic_layer_t *layer;

    /* Check we haven't exceeded the fixed layer limit */
    if (stack->count >= PIC_MAX_LAYERS) {
        fprintf(stderr, "layer: cannot add '%s' — stack full (%d max)\n",
                name, PIC_MAX_LAYERS);
        return -1;
    }

    /* Fill in the layer record — surface starts as NULL */
    layer = &stack->layers[stack->count];
    layer->name = name;
    layer->render = render;
    layer->render_legend = NULL;    /* Set later via pic_layer_set_legend */
    layer->surface = NULL;          /* Allocated on first render      */
    layer->user_data = user_data;
    layer->opacity = opacity;
    layer->enabled = 1;             /* Layers are visible by default  */
    layer->update_interval = update_interval;
    layer->last_rendered = 0;       /* Force render on first update   */

    stack->count++;
    printf("layer: added '%s' (interval=%ds, opacity=%.1f)\n",
           name, update_interval, opacity);

    return 0;
}

/*
 * ensure_surface - Allocate the backing surface if not yet created.
 *
 * Called before rendering. On a 256MB Pi 1 at 1080p, each ARGB32
 * surface is 1920 x 1080 x 4 = 8,294,400 bytes (~8MB). By
 * deferring allocation until the layer actually renders, disabled
 * layers cost nothing.
 *
 * Returns 1 on success, 0 on failure.
 */
static int ensure_surface(pic_layer_t *layer, int width, int height)
{
    if (layer->surface) return 1;

    layer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                 width, height);
    if (cairo_surface_status(layer->surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "layer: surface allocation failed for '%s': %s\n",
                layer->name,
                cairo_status_to_string(
                    cairo_surface_status(layer->surface)));
        cairo_surface_destroy(layer->surface);
        layer->surface = NULL;
        return 0;
    }

    printf("layer: allocated surface for '%s' (%dx%d, ~%d KB)\n",
           layer->name, width, height, width * height * 4 / 1024);
    return 1;
}

/*
 * pic_layer_stack_update - Re-render layers whose data is stale.
 *
 * Walks the stack and checks each enabled layer's last_rendered time
 * against its update_interval. If enough time has passed (or the layer
 * has never been rendered), its render function is called and the
 * layer's dirty flag is set.
 *
 * Surfaces are allocated on-demand here — a disabled layer that has
 * never been enabled will never have a surface allocated.
 */
int pic_layer_stack_update(pic_layer_stack_t *stack, time_t now)
{
    int i;
    int dirty_count = 0;

    for (i = 0; i < stack->count; i++) {
        pic_layer_t *layer = &stack->layers[i];
        int elapsed;
        cairo_t *cr;

        /* Clear the dirty flag at the start of each frame */
        layer->dirty = 0;

        /* Skip disabled layers entirely — no CPU cost */
        if (!layer->enabled) {
            continue;
        }

        /*
         * Check if this layer needs re-rendering:
         *   - last_rendered == 0 means it has never been drawn
         *   - update_interval == 0 means "render once, never update"
         *   - Otherwise, re-render when interval seconds have passed
         */
        elapsed = (int)(now - layer->last_rendered);
        if (layer->last_rendered != 0) {
            if (layer->update_interval == 0) {
                /* Static layer, already rendered once — skip */
                continue;
            }
            if (elapsed < layer->update_interval) {
                /* Not enough time has passed — skip */
                continue;
            }
        }

        /* Allocate surface on first render (lazy allocation) */
        if (!ensure_surface(layer, stack->width, stack->height)) {
            continue; /* Allocation failed — skip this frame */
        }

        /*
         * Clear the surface to transparent black (all bytes zero).
         * We use CAIRO_OPERATOR_CLEAR which sets every pixel to
         * (0, 0, 0, 0) regardless of the current content.
         */
        cr = cairo_create(layer->surface);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);

        /* Switch back to normal compositing for the render function */
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        /* Enable full font hinting for crisp text at 1080p */
        {
            cairo_font_options_t *fo = cairo_font_options_create();
            cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
            cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
            cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
            cairo_set_font_options(cr, fo);
            cairo_font_options_destroy(fo);
        }

        /* Let the layer draw its content, passing its user_data */
        layer->render(cr, stack->width, stack->height, now,
                      layer->user_data);

        cairo_destroy(cr);
        layer->last_rendered = now;
        layer->dirty = 1;
        dirty_count++;
    }

    return dirty_count;
}

/*
 * pic_layer_stack_composite - Paint all layers onto an output surface.
 *
 * Paints each enabled layer's surface onto the destination Cairo context,
 * in stack order (bottom to top), respecting each layer's opacity.
 * Skips layers with no surface (disabled layers that never rendered).
 */
void pic_layer_stack_composite(pic_layer_stack_t *stack, cairo_t *cr)
{
    int i;

    for (i = 0; i < stack->count; i++) {
        pic_layer_t *layer = &stack->layers[i];

        /* Skip disabled layers or layers with no surface */
        if (!layer->enabled || !layer->surface) {
            continue;
        }

        /*
         * Position the layer surface at (0, 0) on the output.
         * Since all surfaces share the same dimensions, no scaling
         * or offset is needed — they align pixel-for-pixel.
         */
        cairo_set_source_surface(cr, layer->surface, 0, 0);
        cairo_paint_with_alpha(cr, layer->opacity);
    }
}

/*
 * pic_layer_stack_render_legends - Draw legends at full opacity.
 *
 * Called after compositing. Legends bypass layer opacity so they
 * are always readable. Only enabled layers with a render_legend
 * callback are processed.
 */
void pic_layer_stack_render_legends(pic_layer_stack_t *stack, cairo_t *cr,
                                    time_t now)
{
    int i;

    for (i = 0; i < stack->count; i++) {
        pic_layer_t *layer = &stack->layers[i];

        if (!layer->enabled || !layer->render_legend) continue;

        layer->render_legend(cr, stack->width, stack->height,
                             now, layer->user_data);
    }
}

/*
 * pic_layer_set_legend - Set the legend render function for a named layer.
 */
void pic_layer_set_legend(pic_layer_stack_t *stack, const char *name,
                          pic_layer_render_fn legend)
{
    int i;
    for (i = 0; i < stack->count; i++) {
        if (strcmp(stack->layers[i].name, name) == 0) {
            stack->layers[i].render_legend = legend;
            return;
        }
    }
    fprintf(stderr, "layer: set_legend: no layer named '%s'\n", name);
}

/*
 * pic_layer_free_surface - Free a single layer's surface.
 *
 * Called when a layer is disabled to reclaim memory. The surface
 * will be re-allocated if the layer is later re-enabled.
 */
void pic_layer_free_surface(pic_layer_t *layer)
{
    if (layer->surface) {
        printf("layer: freed surface for '%s'\n", layer->name);
        cairo_surface_destroy(layer->surface);
        layer->surface = NULL;
        layer->last_rendered = 0; /* Force re-render when re-enabled */
    }
}

/*
 * pic_layer_stack_destroy - Free all allocated resources.
 *
 * Destroys each layer's Cairo surface and resets the stack.
 * Safe to call on an already-destroyed or never-used stack
 * (surfaces will be NULL and count will be 0).
 */
void pic_layer_stack_destroy(pic_layer_stack_t *stack)
{
    int i;

    for (i = 0; i < stack->count; i++) {
        if (stack->layers[i].surface) {
            cairo_surface_destroy(stack->layers[i].surface);
            stack->layers[i].surface = NULL;
        }
    }

    stack->count = 0;
    printf("layer: stack destroyed\n");
}
