/*
 * layers_ticker.c - Scrolling news ticker layer
 *
 * Renders a full-width scrolling text bar at the bottom of the
 * screen, between the applet panels.
 *
 * Two display modes:
 *   - Scroll:    continuous smooth scroll (default on multi-core)
 *   - Headlines: scroll each headline in fast, park at left edge
 *                for 10 seconds so user can read it, then advance
 *                to next headline (default on single-core)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "newsticker.h"
#include <cairo/cairo.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#include "math_utils.h"

/* Separator between items in scroll mode */
#define SEP "   \xE2\x80\xA2   "

/* Forward declarations */
static double truncate_to_width(cairo_t *cr, char *text, double max_w,
                                double font_size);

/* Headlines mode timing:
 *   Phase 0: scroll-in from right, old scrolls out left (< 0.5s)
 *   Phase 1: hold at title for 5 seconds
 *   Phase 2: slide left to show description (< 0.5s)
 *   Phase 3: hold at description for 15 seconds
 */
#define HEADLINE_TITLE_HOLD    5
#define HEADLINE_DESC_HOLD     15
#define HEADLINE_SCROLL_SECS   0.9  /* Target duration for scroll animations */

/*
 * draw_ticker_item - Draw a single headline (title + description).
 *
 * Draws at position cx,text_y. Returns the x advance (width consumed).
 */
/*
 * draw_ticker_item parameters:
 *   max_desc_w > 0:  truncate description at word boundary to fit (summaries)
 *   max_desc_w == 0: show full description (articles / scroll mode)
 *   max_desc_w < 0:  hide description entirely (headlines only)
 */
static double draw_ticker_item(cairo_t *cr, pic_ticker_item_t *item,
                               double cx, double text_y, double font_size,
                               double max_desc_w)
{
    cairo_text_extents_t ext;
    double start_x = cx;

    /* Title in gold, bold */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);
    cairo_set_source_rgba(cr, 1.0, 0.85, 0.3, 0.95);
    cairo_move_to(cr, cx, text_y);
    cairo_show_text(cr, item->title);
    cairo_text_extents(cr, item->title, &ext);
    cx += ext.x_advance;

    /* Description — skip if headlines-only (max_desc_w < 0) */
    if (item->desc[0] && max_desc_w >= 0) {
        char desc_buf[TICKER_MAX_DESCLEN];
        const char *desc_text = item->desc;

        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);

        cairo_set_source_rgba(cr, 0.5, 0.5, 0.6, 0.7);
        cairo_move_to(cr, cx, text_y);
        cairo_show_text(cr, " \xE2\x80\x94 ");
        cairo_text_extents(cr, " \xE2\x80\x94 ", &ext);
        cx += ext.x_advance;

        /* Truncate description at word boundary if it won't fit.
         * In summaries mode (max_desc_w > 0): truncate to bar width.
         * In articles mode (max_desc_w == 0): no truncation, text
         * scrolls into view via phase 2 slide. */
        if (max_desc_w > 0) {
            double avail = max_desc_w - (cx - start_x);
            strncpy(desc_buf, item->desc, sizeof(desc_buf) - 4);
            desc_buf[sizeof(desc_buf) - 4] = '\0';
            truncate_to_width(cr, desc_buf, avail, font_size);
            desc_text = desc_buf;
        }

        cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
        cairo_move_to(cr, cx, text_y);
        cairo_show_text(cr, desc_text);
        cairo_text_extents(cr, desc_text, &ext);
        cx += ext.x_advance;
    }

    return cx - start_x;
}

/*
 * truncate_to_width - Truncate text at a word boundary to fit within
 * max_w pixels. Modifies the string in place, appending "..." if
 * truncated. Returns the pixel width of the truncated text.
 */
static double truncate_to_width(cairo_t *cr, char *text, double max_w,
                                double font_size)
{
    cairo_text_extents_t ext;
    double ellipsis_w;
    int last_space = -1;
    int i;

    /* Ensure correct font face is set for accurate measurement */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents(cr, text, &ext);
    if (ext.x_advance <= max_w) return ext.x_advance;

    /* Measure the suffix we'll append */
    cairo_text_extents(cr, " ...", &ext);
    ellipsis_w = ext.x_advance;

    /* Walk forward, tracking the last space where
     * text[0..space] + " ..." fits within max_w */
    for (i = 0; text[i]; i++) {
        if (text[i] == ' ') {
            text[i] = '\0';
            cairo_text_extents(cr, text, &ext);
            if (ext.x_advance + ellipsis_w > max_w) {
                /* This space is too far — use the previous one */
                text[i] = ' ';
                break;
            }
            text[i] = ' ';
            last_space = i;
        }
    }

    if (last_space > 0) {
        text[last_space] = '\0';
        strcat(text, " ...");
    }

    cairo_text_extents(cr, text, &ext);
    return ext.x_advance;
}

/*
 * find_chunk_end - Find where to break text for a bar-width chunk.
 *
 * Given text starting at char offset `start`, find the last complete
 * word that fits within `max_w` pixels (including " ..." suffix if
 * there's more text after).
 *
 * Returns the char offset of the START of the last fitting word
 * (this becomes the anchor / start of the next chunk).
 * Sets *end_offset to the char offset AFTER the last fitting word
 * (where to place " ...").
 *
 * If the entire remaining text fits, returns -1 (no more chunks).
 */
static int find_chunk_end(cairo_t *cr, const char *text, int start,
                          double max_w, double font_size, int *end_offset)
{
    cairo_text_extents_t ext;
    double ellipsis_w;
    int last_word_end = start;
    int i;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    /* Check if remaining text fits without truncation */
    cairo_text_extents(cr, text + start, &ext);
    if (ext.x_advance <= max_w) {
        *end_offset = strlen(text);
        return -1; /* All fits — this is the last chunk */
    }

    /* Measure " ..." suffix */
    cairo_text_extents(cr, " ...", &ext);
    ellipsis_w = ext.x_advance;

    /* Walk word by word from start */
    i = start;
    while (text[i]) {
        /* Find end of current word */
        while (text[i] && text[i] != ' ') i++;
        int word_end = i;

        /* Measure text from chunk start to end of this word */
        {
            char saved = text[word_end];
            ((char *)text)[word_end] = '\0';
            cairo_text_extents(cr, text + start, &ext);
            ((char *)text)[word_end] = saved;
        }

        if (ext.x_advance + ellipsis_w > max_w) {
            /* This word doesn't fit — break before it */
            break;
        }

        last_word_end = word_end;

        /* Skip spaces */
        while (text[i] == ' ') i++;
    }

    *end_offset = last_word_end;
    /* Next chunk starts AFTER the last fitting word (skip the space).
     * No word duplication between chunks. */
    i = last_word_end;
    while (text[i] == ' ') i++;
    return i;
}

/*
 * draw_ticker_chunk - Draw a chunk of description text at the left bar edge.
 * If not the last chunk, appends " ...".
 */
static void draw_ticker_chunk(cairo_t *cr, const char *desc, int start,
                              int end, int is_last, double cx, double text_y,
                              double font_size)
{
    char buf[TICKER_MAX_DESCLEN];
    int len = end - start;
    if (len <= 0) return;
    if (len >= (int)sizeof(buf) - 5) len = sizeof(buf) - 5;

    memcpy(buf, desc + start, len);
    buf[len] = '\0';

    /* Trim trailing spaces */
    while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';

    if (!is_last) strcat(buf, " ...");

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
    cairo_move_to(cr, cx, text_y);
    cairo_show_text(cr, buf);
}

/*
 * measure_ticker_title - Measure just the title width.
 */
static double measure_ticker_title(cairo_t *cr, pic_ticker_item_t *item,
                                   double font_size)
{
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents(cr, item->title, &ext);
    return ext.x_advance;
}

/*
 * measure_ticker_item - Measure the full width (title + description).
 */
static double measure_ticker_item(cairo_t *cr, pic_ticker_item_t *item,
                                  double font_size)
{
    cairo_text_extents_t ext;
    double w = 0;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents(cr, item->title, &ext);
    w += ext.x_advance;

    if (item->desc[0]) {
        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);
        cairo_text_extents(cr, " \xE2\x80\x94 ", &ext);
        w += ext.x_advance;
        cairo_text_extents(cr, item->desc, &ext);
        w += ext.x_advance;
    }

    return w;
}

/*
 * pic_layer_render_ticker - Render the news ticker bar.
 */
void pic_layer_render_ticker(cairo_t *cr, int width, int height,
                             time_t now, void *user_data)
{
    pic_ticker_t *ticker = (pic_ticker_t *)user_data;
    double margin_l, margin_r, bar_w;
    double bar_h = height / 30.0;
    double bottom_margin = height / 60.0;
    double bar_y = height - bar_h - bottom_margin;
    double corner = bar_h * 0.4;
    double font_size = bar_h * 0.55;
    double text_y = bar_y + bar_h * 0.65;
    int i;

    if (!ticker) return;

    /* Use applet margins or 5% default */
    margin_l = ticker->left_margin > 0 ? ticker->left_margin : width * 0.05;
    margin_r = ticker->right_margin > 0 ? ticker->right_margin : width * 0.05;
    bar_w = width - margin_l - margin_r;

    pthread_mutex_lock(&ticker->mutex);

    /* Export bar geometry under the mutex so the main thread's blit
     * loop (which reads these under the same mutex) never sees a
     * torn 64-bit double on multi-core ARM (Pi 3/4/5). */
    ticker->bar_top = bar_y;
    ticker->bar_bottom = bar_y + bar_h;

    if (ticker->count == 0) {
        pthread_mutex_unlock(&ticker->mutex);
        return;
    }

    /* Draw the background bar */
    cairo_set_source_rgba(cr, 0.02, 0.02, 0.06, 1.0);
    {
        double x = margin_l, y = bar_y;
        double w = bar_w, h = bar_h;
        double r = corner;
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
    cairo_fill(cr);

    /* Set up font for measurement */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    /* Clip to the bar area */
    cairo_save(cr);
    cairo_rectangle(cr, margin_l, bar_y, bar_w, bar_h);
    cairo_clip(cr);

    if (ticker->mode == TICKER_MODE_HEADLINES) {
        /*
         * Headlines mode — 4-phase state machine:
         *   Phase 0: New headline scrolls in from right, old scrolls out left (< 0.5s)
         *   Phase 1: Hold at title + first chunk for 15 seconds
         *   Phase 2: Crossfade to next chunk (< 0.5s)
         *   Phase 3: Hold current chunk for 15 seconds, then next chunk or next headline
         *
         * All scroll positions are rounded to integers to prevent
         * sub-pixel blurriness on text rendering.
         */
        int idx = ticker->headline_idx % ticker->count;
        pic_ticker_item_t *item = &ticker->items[idx];
        double pad = 10;
        /* Full article text, chunked to fit the bar width */
        double dlim = 0;
        double title_x = margin_l + pad;
        double title_w = measure_ticker_title(cr, item, font_size);
        double start_x = margin_l + bar_w;
        struct timeval atv;
        double anim_now;

        gettimeofday(&atv, NULL);
        anim_now = (double)atv.tv_sec + (double)atv.tv_usec / 1000000.0;

        switch (ticker->headline_phase) {
        case 0: /* Crossfade scroll: old out left, new in from right */
            if (ticker->headline_anim_t <= 0) {
                ticker->headline_anim_t = anim_now;
            }

            {
                double elapsed = anim_now - ticker->headline_anim_t;
                double t = elapsed / HEADLINE_SCROLL_SECS;
                if (t > 1.0) t = 1.0;

                /* Ease-out: fast start, smooth stop */
                double ease = 1.0 - (1.0 - t) * (1.0 - t);

                /* Position new headline — slides from right to title_x */
                int new_x = (int)(start_x + (title_x - start_x) * ease);

                /* Old headline is pushed left BY the new one — they're
                 * linked so there's never any overlap. The old headline
                 * sits just to the left of the new one with a gap. */
                if (ticker->headline_prev_idx >= 0 &&
                    ticker->headline_prev_idx != idx) {
                    int prev_i = ticker->headline_prev_idx % ticker->count;
                    double prev_w = measure_ticker_item(cr,
                        &ticker->items[prev_i], font_size);
                    int gap = (int)(bar_w * 0.02);
                    int out_x = new_x - (int)prev_w - gap;
                    if (out_x + prev_w > margin_l) {
                        draw_ticker_item(cr, &ticker->items[prev_i],
                                       out_x, text_y, font_size, dlim);
                    }
                }
                /* Show title+desc truncated to bar width minus padding.
                 * Must match find_chunk_end's desc_avail (bar_w - pad*2)
                 * so the last visible word here isn't also chunk 2's first. */
                draw_ticker_item(cr, item, new_x, text_y, font_size, bar_w - pad * 2);

                if (t >= 1.0) {
                    ticker->headline_scroll_x = title_x;
                    ticker->headline_parked_at = now;
                    ticker->headline_phase = 1;
                    ticker->headline_anim_t = 0;
                }
            }
            break;

        case 1: /* Hold current view for 15 seconds */
            if ((int)(now - ticker->headline_parked_at) >= HEADLINE_DESC_HOLD) {
                if (!item->desc[0]) {
                    /* No description — nothing to chunk, next headline */
                    ticker->headline_prev_idx = idx;
                    ticker->headline_idx = (idx + 1) % ticker->count;
                    ticker->headline_phase = 0;
                    ticker->headline_parked_at = 0;
                    ticker->headline_anim_t = 0;
                } else {
                    /* Articles: check if there's more text beyond chunk 1.
                     * Chunk 1 was the title+separator+desc. Measure the
                     * separator " — " properly to find available desc width. */
                    cairo_text_extents_t sep_ext;
                    cairo_select_font_face(cr, "sans-serif",
                        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, font_size);
                    cairo_text_extents(cr, " \xE2\x80\x94 ", &sep_ext);

                    int chunk_end = 0;
                    double desc_avail = bar_w - pad * 2 - title_w - sep_ext.x_advance;
                    int anchor = find_chunk_end(cr, item->desc, 0,
                        desc_avail, font_size, &chunk_end);
                    if (anchor < 0) {
                        /* Everything fit — next headline */
                        ticker->headline_prev_idx = idx;
                        ticker->headline_idx = (idx + 1) % ticker->count;
                        ticker->headline_phase = 0;
                        ticker->headline_parked_at = 0;
                        ticker->headline_anim_t = 0;
                    } else {
                        /* More text — start chunking from the anchor */
                        ticker->headline_chunk_start = anchor;
                        ticker->headline_prev_x = -1; /* prev is title+summary */
                        ticker->headline_phase = 2;
                        ticker->headline_anim_t = anim_now;
                    }
                }
            }
            /* Draw title + description truncated to fit bar (minus padding,
             * matching find_chunk_end's desc_avail calculation) */
            draw_ticker_item(cr, item, (int)title_x, text_y, font_size, bar_w - pad * 2);
            break;

        case 2: /* Crossfade: old content out left, new chunk in from right */
            {
                double elapsed = anim_now - ticker->headline_anim_t;
                double t = elapsed / HEADLINE_SCROLL_SECS;
                if (t > 1.0) t = 1.0;
                double ease = 1.0 - (1.0 - t) * (1.0 - t);

                /* Compute this chunk's text boundaries */
                int chunk_end = 0;
                int anchor = find_chunk_end(cr, item->desc,
                    ticker->headline_chunk_start,
                    bar_w - pad * 2, font_size, &chunk_end);
                int is_last = (anchor < 0);

                /* New chunk slides in from right */
                int to_x = (int)(margin_l + pad);
                int new_cx = (int)((margin_l + bar_w) + (to_x - (margin_l + bar_w)) * ease);

                /* Old content slides out left, linked to new chunk */
                int gap = (int)(bar_w * 0.02);
                if (ticker->headline_prev_x < 0) {
                    /* Previous was title+summary (chunk 1) */
                    int old_x = new_cx - (int)bar_w - gap;
                    draw_ticker_item(cr, item, old_x, text_y, font_size, bar_w);
                } else {
                    /* Subsequent chunk — old content is the previous chunk */
                    int prev_end = 0;
                    find_chunk_end(cr, item->desc,
                        ticker->headline_prev_x > 0 ? (int)ticker->headline_prev_x : 0,
                        bar_w - pad * 2, font_size, &prev_end);
                    int old_x = new_cx - (int)(bar_w) - gap;
                    draw_ticker_chunk(cr, item->desc,
                        (int)ticker->headline_prev_x, prev_end,
                        0, old_x, text_y, font_size);
                }

                /* Draw new chunk */
                draw_ticker_chunk(cr, item->desc,
                    ticker->headline_chunk_start, chunk_end,
                    is_last, new_cx, text_y, font_size);

                if (t >= 1.0) {
                    ticker->headline_prev_x = ticker->headline_chunk_start;
                    ticker->headline_parked_at = now;
                    ticker->headline_phase = 3;
                    ticker->headline_anim_t = 0;
                }
            }
            break;

        case 3: /* Hold current chunk */
            {
                int chunk_end = 0;
                int anchor = find_chunk_end(cr, item->desc,
                    ticker->headline_chunk_start,
                    bar_w - pad * 2, font_size, &chunk_end);
                int is_last = (anchor < 0);

                /* Draw chunk at left edge */
                draw_ticker_chunk(cr, item->desc,
                    ticker->headline_chunk_start, chunk_end,
                    is_last, (int)(margin_l + pad), text_y, font_size);

                if ((int)(now - ticker->headline_parked_at) >= HEADLINE_DESC_HOLD) {
                    if (!is_last) {
                        /* More chunks — save current as prev, advance */
                        ticker->headline_prev_x = ticker->headline_chunk_start;
                        ticker->headline_chunk_start = anchor;
                        ticker->headline_phase = 2;
                        ticker->headline_anim_t = anim_now;
                    } else {
                        /* Done — next headline */
                        ticker->headline_prev_idx = idx;
                        ticker->headline_idx = (idx + 1) % ticker->count;
                        ticker->headline_phase = 0;
                        ticker->headline_parked_at = 0;
                        ticker->headline_anim_t = 0;
                        ticker->headline_chunk_start = 0;
                    }
                }
            }
            break;

        default:
            ticker->headline_phase = 0;
            break;
        }
    } else {
        /*
         * Scroll mode — time-based position.
         *
         * The scroll offset is set each frame by the ticker thread
         * as a pure function of CLOCK_MONOTONIC time. A frame that
         * delivers late still shows the correct position for that
         * instant — the eye sees smooth motion because position
         * tracks real time, not frame count.
         *
         * Text is rendered directly each frame (Cairo font cache
         * makes repeat glyph draws fast). The position is rounded
         * to integer pixels so glyphs never sub-pixel shift.
         */
        cairo_text_extents_t ext;
        double total_w;

        /* Cache total text width */
        if (ticker->cache_count != ticker->count ||
            ticker->cache_updated != ticker->last_updated) {
            double sep_w;
            cairo_text_extents(cr, SEP, &ext);
            sep_w = ext.x_advance;

            total_w = 0;
            for (i = 0; i < ticker->count; i++) {
                if (i > 0) total_w += sep_w;
                total_w += measure_ticker_item(cr, &ticker->items[i],
                                              font_size);
            }

            ticker->cached_total_w = total_w;
            ticker->cache_count = ticker->count;
            ticker->cache_updated = ticker->last_updated;
        }
        total_w = ticker->cached_total_w;

        /* If content fits, centre it — otherwise scroll */
        if (total_w <= bar_w - 20) {
            double cx = margin_l + (bar_w - total_w) / 2.0;

            for (i = 0; i < ticker->count; i++) {
                if (i > 0) {
                    cairo_select_font_face(cr, "sans-serif",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, font_size);
                    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.7);
                    cairo_move_to(cr, cx, text_y);
                    cairo_show_text(cr, SEP);
                    cairo_text_extents(cr, SEP, &ext);
                    cx += ext.x_advance;
                }
                cx += draw_ticker_item(cr, &ticker->items[i],
                                       cx, text_y, font_size, 0);
            }
        } else {
            /* Scrolling — integer pixel position from monotonic time.
             * scroll_offset is set by the ticker thread each frame as
             * (int)(monotonic_seconds * scroll_speed). We modulo-wrap
             * it by scroll_total for seamless looping. */
            int scroll_total = (int)(total_w + bar_w);
            int pos = (scroll_total > 0)
                    ? ticker->scroll_offset % scroll_total : 0;
            int cx = (int)(margin_l + bar_w) - pos;

            for (i = 0; i < ticker->count; i++) {
                if (i > 0) {
                    cairo_select_font_face(cr, "sans-serif",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, font_size);
                    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.7);
                    cairo_move_to(cr, cx, text_y);
                    cairo_show_text(cr, SEP);
                    cairo_text_extents(cr, SEP, &ext);
                    cx += (int)ext.x_advance;
                }
                cx += (int)draw_ticker_item(cr, &ticker->items[i],
                                            cx, text_y, font_size, 0);
            }
        }
    }

    cairo_restore(cr);
    pthread_mutex_unlock(&ticker->mutex);
}
