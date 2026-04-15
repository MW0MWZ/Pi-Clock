/*
 * newsticker.c - DX and ham radio news ticker data fetcher
 *
 * Background thread that periodically fetches news headlines
 * from multiple sources and builds a concatenated ticker string.
 *
 * Sources:
 *   - NOAA SWPC space weather alerts (JSON)
 *   - DX World expedition news (RSS)
 *   - ARRL News headlines (RSS)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "newsticker.h"
#include "config.h"
#include "fetch.h"
#include "layer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Fetch interval: 30 minutes for RSS, alerts checked each cycle */
#define TICKER_FETCH_INTERVAL 1800

static pthread_t ticker_thread;
static volatile int ticker_running = 0;
static volatile int ticker_refetch = 0;  /* Set by config reload to trigger immediate re-fetch */
static pic_ticker_t *ticker_data;

/* Separator between ticker items */
#define TICKER_SEP "  \xE2\x80\xA2  "  /* bullet character */

/* fetch_to_buf wraps pic_fetch_to_buf for local use */
static int fetch_to_buf(const char *url, char *buf, int buf_size)
{
    return pic_fetch_to_buf(url, buf, buf_size);
}

/*
 * strip_html_tags - Remove all HTML/XML tags from a string in place.
 * Turns "<b>hello</b> world" into "hello world".
 */
static void strip_html_tags(char *s)
{
    char *r = s, *w = s;
    int in_tag = 0;

    while (*r) {
        if (*r == '<') { in_tag = 1; r++; continue; }
        if (*r == '>') { in_tag = 0; r++; continue; }
        if (!in_tag) *w++ = *r;
        r++;
    }
    *w = '\0';
}

/*
 * html_decode_inplace - Decode common HTML entities in place.
 * Also strips any remaining HTML tags first.
 */
static void html_decode_inplace(char *s)
{
    char *r, *w;

    /* First strip any HTML tags */
    strip_html_tags(s);

    /* Then decode entities */
    r = s;
    w = s;
    while (*r) {
        if (*r == '&') {
            if (strncmp(r, "&amp;", 5) == 0)   { *w++ = '&'; r += 5; continue; }
            if (strncmp(r, "&lt;", 4) == 0)    { *w++ = '<'; r += 4; continue; }
            if (strncmp(r, "&gt;", 4) == 0)    { *w++ = '>'; r += 4; continue; }
            if (strncmp(r, "&quot;", 6) == 0)  { *w++ = '"'; r += 6; continue; }
            if (strncmp(r, "&apos;", 6) == 0)  { *w++ = '\''; r += 6; continue; }
            if (strncmp(r, "&nbsp;", 6) == 0)  { *w++ = ' '; r += 6; continue; }
            if (strncmp(r, "&#8211;", 7) == 0) { *w++ = '-'; r += 7; continue; }
            if (strncmp(r, "&#8212;", 7) == 0) { *w++ = '-'; r += 7; continue; }
            if (strncmp(r, "&#8216;", 7) == 0) { *w++ = '\''; r += 7; continue; }
            if (strncmp(r, "&#8217;", 7) == 0) { *w++ = '\''; r += 7; continue; }
            if (strncmp(r, "&#8220;", 7) == 0) { *w++ = '"'; r += 7; continue; }
            if (strncmp(r, "&#8221;", 7) == 0) { *w++ = '"'; r += 7; continue; }
            if (strncmp(r, "&#8230;", 7) == 0) {
                *w++ = '.'; *w++ = '.'; *w++ = '.'; r += 7; continue;
            }
            /* Generic numeric entity &#NNN; — skip it */
            if (r[1] == '#') {
                const char *semi = strchr(r, ';');
                if (semi && (semi - r) < 10) {
                    *w++ = '?';
                    r = (char *)semi + 1;
                    continue;
                }
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/*
 * clean_text - Strip whitespace, HTML tags, decode entities.
 */
static void clean_text(char *out, const char *in, int max_len)
{
    char *s;
    int len;

    strncpy(out, in, max_len - 1);
    out[max_len - 1] = '\0';

    /* Two passes because some RSS feeds double-encode entities.
     * E.g., "&amp;amp;" first decodes to "&amp;" then to "&".
     * Most feeds need only one pass; the second is a no-op. */
    html_decode_inplace(out);
    html_decode_inplace(out);

    /* Strip leading whitespace */
    s = out;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);

    /* Strip trailing whitespace */
    len = strlen(out);
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\n' ||
           out[len-1] == '\r' || out[len-1] == '\t')) {
        out[--len] = '\0';
    }
}

/*
 * add_item - Add a headline + description to the ticker.
 */
static void add_item(pic_ticker_item_t items[], int *count,
                     const char *title, const char *desc)
{
    int i;
    char clean_title[TICKER_MAX_TITLELEN];
    char clean_desc[TICKER_MAX_DESCLEN];

    if (*count >= TICKER_MAX_ITEMS) return;
    if (!title || strlen(title) < 5) return;

    clean_text(clean_title, title, sizeof(clean_title));
    if (strlen(clean_title) < 5) return;

    /* Deduplicate by title */
    for (i = 0; i < *count; i++) {
        if (strcmp(items[i].title, clean_title) == 0) return;
    }

    strncpy(items[*count].title, clean_title, TICKER_MAX_TITLELEN - 1);

    if (desc && strlen(desc) > 5) {
        clean_text(clean_desc, desc, sizeof(clean_desc));
        /* Store full description — truncation for display happens
         * at render time based on ticker mode and available width. */
        strncpy(items[*count].desc, clean_desc, TICKER_MAX_DESCLEN - 1);
    } else {
        items[*count].desc[0] = '\0';
    }

    (*count)++;
}

/*
 * extract_tag - Extract content of an XML tag starting at *pos.
 * Handles CDATA. Returns content length, advances *pos past the tag.
 */
static int extract_tag(const char **pos,
                       const char *tag, char *out, int out_size)
{
    char open_tag[32], close_tag[32];
    const char *start, *end;
    int len;

    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    start = strstr(*pos, open_tag);
    if (!start) return -1;
    start += strlen(open_tag);

    /* Handle CDATA sections */
    if (strncmp(start, "<![CDATA[", 9) == 0) {
        start += 9;
        end = strstr(start, "]]>");
    } else {
        end = strstr(start, close_tag);
    }

    if (!end) return -1;

    len = end - start;
    if (len >= out_size) len = out_size - 1;
    if (len > 0) {
        memcpy(out, start, len);
        out[len] = '\0';
    } else {
        out[0] = '\0';
    }

    *pos = end + 1;
    return len;
}

/*
 * extract_rss_items - Extract <title> and <description> from RSS items.
 * Skips the first <title> (channel title).
 */
static void extract_rss_items(const char *xml,
                              pic_ticker_item_t items[],
                              int *count, int max_items)
{
    const char *p = xml;
    int skipped_first = 0;

    while (*count < max_items) {
        char title[TICKER_MAX_TITLELEN];
        char desc[TICKER_MAX_DESCLEN];
        const char *title_pos = p;

        if (extract_tag(&title_pos, "title",
                        title, sizeof(title)) < 0)
            break;

        /* Skip the channel-level title (first one) */
        if (!skipped_first) {
            skipped_first = 1;
            p = title_pos;
            continue;
        }

        /* Try to find a description near this title */
        desc[0] = '\0';
        {
            const char *desc_pos = title_pos;
            extract_tag(&desc_pos, "description",
                        desc, sizeof(desc));
        }

        add_item(items, count, title, desc[0] ? desc : NULL);
        p = title_pos;
    }
}

/*
 * build_ticker - Copy items into the ticker struct.
 */
static void build_ticker(pic_ticker_t *ticker,
                         pic_ticker_item_t items[], int count)
{
    int i;

    pthread_mutex_lock(&ticker->mutex);

    ticker->count = count;
    for (i = 0; i < count && i < TICKER_MAX_ITEMS; i++) {
        ticker->items[i] = items[i];
    }
    ticker->last_updated = time(NULL);

    pthread_mutex_unlock(&ticker->mutex);
}

/*
 * ── Built-in news source fetch functions ──────────────────
 *
 * Each function populates items[] and increments *count.
 * They are registered as sources during ticker init so users
 * can independently enable/disable each one from the dashboard.
 */

/*
 * fetch_noaa_alerts - Extract headlines from NOAA space weather alerts.
 *
 * The alerts endpoint returns a JSON array where each element has a
 * "message" field containing multi-line plain text. We search for
 * keywords (WATCH, WARNING, ALERT, SUMMARY) that mark meaningful
 * lines and extract them as ticker headlines. The JSON is not
 * formally parsed — we scan for keywords directly in the raw bytes.
 */
void fetch_noaa_alerts(pic_ticker_item_t items[],
                              int *count, int max_items)
{
    char buf[32768];
    const char *p;
    int alert_count = 0;

    if (fetch_to_buf("https://services.swpc.noaa.gov/products/alerts.json",
                     buf, sizeof(buf)) <= 0)
        return;

    p = buf;
    while (alert_count < 5 && *count < max_items) {
        const char *msg_start = strstr(p, "\"message\"");
        const char *colon, *quote_start;
        char headline[TICKER_MAX_TITLELEN];

        if (!msg_start) break;
        colon = strchr(msg_start + 9, ':');
        if (!colon) break;
        quote_start = strchr(colon, '"');
        if (!quote_start) break;
        quote_start++;

        {
            const char *scan = quote_start;
            int found = 0;
            while (*scan && *scan != '"') {
                if (strncmp(scan, "## ", 3) == 0 ||
                    strncmp(scan, "Space Weather", 13) == 0 ||
                    strncmp(scan, "WATCH:", 6) == 0 ||
                    strncmp(scan, "WARNING:", 8) == 0 ||
                    strncmp(scan, "ALERT:", 6) == 0 ||
                    strncmp(scan, "SUMMARY:", 8) == 0) {
                    int hi = 0;
                    while (*scan && *scan != '\\' && *scan != '"' &&
                           hi < TICKER_MAX_TITLELEN - 1) {
                        headline[hi++] = *scan++;
                    }
                    headline[hi] = '\0';
                    if (hi > 10) {
                        add_item(items, count, headline, NULL);
                        found = 1;
                        alert_count++;
                    }
                    break;
                }
                if (*scan == '\\' && *(scan+1) == 'n') scan += 2;
                else scan++;
            }
            if (!found) p = quote_start;
        }
        p = msg_start + 10;
    }
    printf("ticker: noaa_alerts: %d items\n", alert_count);
}

/* DX World RSS — DX expedition and ham radio news */
void fetch_dxworld(pic_ticker_item_t items[],
                          int *count, int max_items)
{
    char buf[32768];
    int before = *count;

    if (fetch_to_buf("https://www.dx-world.net/feed/",
                     buf, sizeof(buf)) <= 0)
        return;

    extract_rss_items(buf, items, count, max_items);
    printf("ticker: dxworld: %d items\n", *count - before);
}

/* ARRL News RSS — official ARRL headlines */
void fetch_arrl(pic_ticker_item_t items[],
                       int *count, int max_items)
{
    char buf[32768];
    int before = *count;

    if (fetch_to_buf("https://www.arrl.org/news/rss",
                     buf, sizeof(buf)) <= 0)
        return;

    extract_rss_items(buf, items, count, max_items);
    printf("ticker: arrl: %d items\n", *count - before);
}

/* RSGB GB2RS News — UK ham radio weekly news bulletin */
void fetch_rsgb(pic_ticker_item_t items[],
                       int *count, int max_items)
{
    char buf[32768];
    int before = *count;

    if (fetch_to_buf("https://rsgb.org/main/blog/category/news/gb2rs/feed/",
                     buf, sizeof(buf)) <= 0)
        return;

    extract_rss_items(buf, items, count, max_items);
    printf("ticker: rsgb: %d items\n", *count - before);
}

/* Southgate ARC — prolific ham radio news aggregator.
 * Southgate posts dozens of items/day so the feed is larger (~50KB). */
void fetch_southgate(pic_ticker_item_t items[],
                            int *count, int max_items)
{
    char buf[65536];
    int before = *count;

    if (fetch_to_buf("https://www.southgatearc.org/sarc.rss",
                     buf, sizeof(buf)) <= 0)
        return;

    extract_rss_items(buf, items, count, max_items);
    printf("ticker: southgate: %d items\n", *count - before);
}


/*
 * fetch_ticker_data - Fetch all enabled sources and rebuild ticker.
 *
 * Calls each registered source's fetch function in order. All items
 * are collected into a temporary array, then atomically swapped into
 * the ticker struct under mutex lock. If no items are fetched, inserts
 * a placeholder so the ticker bar always shows something.
 */
static void fetch_ticker_data(void)
{
    pic_ticker_item_t items[TICKER_MAX_ITEMS];
    int count = 0;
    int i;

    memset(items, 0, sizeof(items));
    printf("ticker: fetching news...\n");

    /* Call each enabled source's fetch function.
     * Cap each source to 10 items so no single prolific feed
     * (e.g. Southgate with 30+ daily posts) starves the others.
     * With TICKER_MAX_ITEMS=128 and 5 sources of 10, there is
     * ample headroom; the cap mainly prevents display dominance. */
    for (i = 0; i < ticker_data->source_count; i++) {
        if (ticker_data->sources[i].enabled &&
            ticker_data->sources[i].fetch) {
            int source_cap = count + 10;
            if (source_cap > TICKER_MAX_ITEMS)
                source_cap = TICKER_MAX_ITEMS;
            ticker_data->sources[i].fetch(items, &count, source_cap);
        }
    }

    if (count == 0) {
        add_item(items, &count,
                 "Pi-Clock", "Waiting for news feeds...");
    }

    printf("ticker: %d total items\n", count);
    build_ticker(ticker_data, items, count);
}

/*
 * ticker_layer_enabled - Check if the ticker layer is on.
 *
 * Reads layer->enabled (volatile int) from the ticker thread.
 * Safe on single-core Pi Zero W: volatile int load is atomic.
 * On SMP targets, replace with atomic_load or a mutex read.
 */
static int ticker_layer_enabled(void)
{
    pic_layer_stack_t *stack;
    int i;

    if (!ticker_data || !ticker_data->layer_stack) return 1;

    stack = (pic_layer_stack_t *)ticker_data->layer_stack;
    for (i = 0; i < stack->count; i++) {
        if (strcmp(stack->layers[i].name, "ticker") == 0) {
            return stack->layers[i].enabled;
        }
    }
    return 1;
}

static void *ticker_thread_func(void *arg)
{
    (void)arg;

    /* Lower priority so RSS fetches don't starve the renderer */
    nice(15);

    if (ticker_layer_enabled()) {
        if (pic_wait_for_network("ticker", &ticker_running)) {
            fetch_ticker_data();
        }
    }

    while (ticker_running) {
        int elapsed = 0;
        while (ticker_running && elapsed < TICKER_FETCH_INTERVAL) {
            sleep(5);
            elapsed += 5;
            /* Config reload sets ticker_refetch to trigger immediate re-fetch
             * when sources are enabled/disabled via the dashboard. */
            if (ticker_refetch) {
                ticker_refetch = 0;
                break;
            }
        }
        if (ticker_running && ticker_layer_enabled()) {
            fetch_ticker_data();
        }
    }

    return NULL;
}

/* Initialise ticker. Set ticker->layer_stack after init so the fetch
 * thread can check whether the ticker layer is enabled. */
void pic_ticker_init(pic_ticker_t *ticker)
{
    memset(ticker, 0, sizeof(*ticker));
    pthread_mutex_init(&ticker->mutex, NULL);
}

/* Register a news source. Enabled by default; use pic_ticker_load_config
 * to apply user preferences. Returns 0 on success, -1 if limit reached. */
int pic_ticker_add_source(pic_ticker_t *ticker, const char *name,
                          const char *label, pic_ticker_fetch_fn fetch)
{
    pic_ticker_source_t *s;

    if (ticker->source_count >= TICKER_MAX_SOURCES) return -1;

    s = &ticker->sources[ticker->source_count];
    s->name = name;
    s->label = label;
    s->fetch = fetch;
    s->enabled = 0;  /* Off until config file says otherwise */

    ticker->source_count++;
    printf("ticker: registered source '%s'\n", name);
    return 0;
}

#define TICKER_CONF "/data/etc/pi-clock-ticker.conf"

void pic_ticker_load_config(pic_ticker_t *ticker)
{
    FILE *f;
    char line[128];
    int i;

    f = fopen(TICKER_CONF, "r");
    if (!f) return;

    printf("ticker: loading config from %s\n", TICKER_CONF);

    while (fgets(line, sizeof(line), f)) {
        char name[32];
        int enabled;

        if (line[0] == '#' || line[0] == '\n') continue;

        if (sscanf(line, "%31[^=]=%d", name, &enabled) == 2) {
            /* Check for ticker mode setting */
            if (strcmp(name, "mode") == 0) {
                /* Clamp to valid range — old configs may have mode=2 or 3 */
                if (enabled < TICKER_MODE_SCROLL) enabled = TICKER_MODE_HEADLINES;
                if (enabled > TICKER_MODE_HEADLINES) enabled = TICKER_MODE_HEADLINES;
                /* Single-core Pis cannot do smooth scroll — force headlines
                 * regardless of what the config says. This handles SD cards
                 * moved from a multi-core to a single-core Pi. */
                if (ticker->cpu_cores <= 1 && enabled == TICKER_MODE_SCROLL) {
                    printf("ticker: forcing headlines mode (single-core)\n");
                    enabled = TICKER_MODE_HEADLINES;
                }
                ticker->mode = enabled;
                printf("ticker: mode=%d\n", enabled);
                continue;
            }
            for (i = 0; i < ticker->source_count; i++) {
                if (strcmp(ticker->sources[i].name, name) == 0) {
                    ticker->sources[i].enabled = enabled;
                    printf("ticker: '%s' enabled=%d\n", name, enabled);
                    break;
                }
            }
        }
    }

    fclose(f);

    /* Trigger immediate re-fetch so source changes take effect
     * without waiting for the next 30-minute fetch cycle. */
    ticker_refetch = 1;
}

int pic_ticker_start(pic_ticker_t *ticker)
{
    if (ticker_running) return 0;

    ticker_data = ticker;
    ticker_running = 1;

    if (pthread_create(&ticker_thread, NULL, ticker_thread_func, NULL) != 0) {
        ticker_running = 0;
        fprintf(stderr, "ticker: failed to create thread\n");
        return -1;
    }

    /* Thread is joinable — pic_ticker_stop() calls pthread_join */
    printf("ticker: fetch thread started (interval=%ds)\n",
           TICKER_FETCH_INTERVAL);
    return 0;
}

void pic_ticker_stop(void)
{
    if (!ticker_running) return;
    ticker_running = 0;
    pthread_join(ticker_thread, NULL);
}
