/*
 * newsticker.h - DX and ham radio news ticker
 *
 * Fetches headlines from NOAA SWPC alerts, DX World RSS,
 * and ARRL News in the background. Headlines are stored in
 * a circular buffer and rendered as a scrolling ticker bar
 * at the bottom of the display.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_NEWSTICKER_H
#define PIC_NEWSTICKER_H

#include <time.h>
#include <pthread.h>

#define TICKER_MAX_ITEMS   128
#define TICKER_MAX_TITLELEN 200
#define TICKER_MAX_DESCLEN  1024

typedef struct {
    char title[TICKER_MAX_TITLELEN];
    char desc[TICKER_MAX_DESCLEN];
} pic_ticker_item_t;

/*
 * pic_ticker_source_t - A pluggable news source.
 *
 * Each source has a fetch function that populates items.
 * Sources can be independently enabled/disabled from the dashboard.
 */
#define TICKER_MAX_SOURCES 8

typedef void (*pic_ticker_fetch_fn)(pic_ticker_item_t items[],
                                    int *count, int max_items);

typedef struct {
    const char         *name;     /* Config key (e.g. "dxworld")     */
    const char         *label;    /* Display name for dashboard      */
    pic_ticker_fetch_fn fetch;    /* Fetch function                  */
    int                 enabled;  /* Non-zero if active              */
} pic_ticker_source_t;

typedef struct {
    pic_ticker_item_t items[TICKER_MAX_ITEMS];
    int count;

    /* Registered news sources */
    pic_ticker_source_t sources[TICKER_MAX_SOURCES];
    int source_count;

    /*
     * Applet margins — set by pic_applet_stack_render() each frame
     * so the ticker can avoid overlapping the side panels.
     */
    double left_margin;
    double right_margin;

    /*
     * Ticker bar geometry — set by the ticker layer each frame.
     * Read by the DX legend and applet renderer for alignment.
     */
    double bar_top;
    double bar_bottom;

    /* Ticker display mode:
     *   0 = scroll    — continuous smooth scroll (multi-core only)
     *   1 = headlines  — fly-in title, hold, then chunk through full
     *                     article text. Default on all systems.
     */
    int mode;
    int cpu_cores;  /* Set at startup — used to force headlines on single-core */
    #define TICKER_MODE_SCROLL     0
    #define TICKER_MODE_HEADLINES  1

    /* Headlines mode state */
    int    headline_idx;       /* Current headline being displayed */
    int    headline_prev_idx;  /* Previous headline (scrolling out) */
    double headline_scroll_x;  /* Current draw X position */
    double headline_prev_x;    /* Previous headline's scroll-out position */
    time_t headline_parked_at; /* When current phase parked */
    int    headline_phase;     /* 0=scroll-in, 1=title hold, 2=slide-chunk, 3=chunk hold */
    double headline_anim_t;    /* Animation start time */
    double headline_target_x;  /* Where the current slide is heading */
    int    headline_chunk_start; /* Char offset into desc for current chunk */

    /* Pointer to the layer stack — used to check if the ticker
     * layer is enabled before fetching. Set by display.c. */
    void *layer_stack;

    /* Cached total text width — invalidated when content changes */
    double cached_total_w;
    int    cache_count;       /* count when cache was computed */
    time_t cache_updated;     /* last_updated when cache was computed */

    /*
     * Time-based scroll state for scroll mode.
     *
     * scroll_offset is set each frame by the ticker render thread
     * as (int)(CLOCK_MONOTONIC_seconds * scroll_speed). Because
     * it tracks real time, a late frame still shows the correct
     * position for that instant — delivery jitter is invisible.
     */
    int scroll_offset;               /* Current scroll position   */

    time_t last_updated;
    pthread_mutex_t mutex;
} pic_ticker_t;

void pic_ticker_init(pic_ticker_t *ticker);

/*
 * pic_ticker_add_source - Register a news source.
 */
int pic_ticker_add_source(pic_ticker_t *ticker, const char *name,
                          const char *label, pic_ticker_fetch_fn fetch);

/*
 * pic_ticker_load_config - Load source enable/disable from config.
 * File: /data/etc/pi-clock-ticker.conf, format: name=0|1
 */
void pic_ticker_load_config(pic_ticker_t *ticker);

int  pic_ticker_start(pic_ticker_t *ticker);
void pic_ticker_stop(void);

/* Built-in news source fetch functions */
void fetch_noaa_alerts(pic_ticker_item_t items[], int *count, int max_items);
void fetch_dxworld(pic_ticker_item_t items[], int *count, int max_items);
void fetch_arrl(pic_ticker_item_t items[], int *count, int max_items);
void fetch_rsgb(pic_ticker_item_t items[], int *count, int max_items);
void fetch_southgate(pic_ticker_item_t items[], int *count, int max_items);

#endif /* PIC_NEWSTICKER_H */
