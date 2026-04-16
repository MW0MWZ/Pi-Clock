/*
 * display.c - SDL2 display output for Pi-Clock
 *
 * Implements the full-screen display render loop using SDL2.
 * On the Raspberry Pi, SDL2 uses the KMSDRM backend to drive
 * the HDMI output directly — no X11, no Wayland, no compositor.
 *
 * The approach:
 *
 *   1. Initialise SDL2 with video subsystem
 *   2. Create a full-screen window (or windowed if DISPLAY is set)
 *   3. Create an SDL_Renderer with software or hardware backend
 *   4. Main loop:
 *      a. Update the layer stack (re-render stale layers)
 *      b. Composite all layers onto a Cairo surface
 *      c. Copy the Cairo surface pixels to an SDL texture
 *      d. Present the texture to the display
 *      e. Sleep until next frame (1 second for the HUD clock)
 *   5. Clean up on exit
 *
 * We use SDL2's software renderer + streaming texture approach
 * rather than OpenGL ES directly. This is simpler and still
 * fast enough for our 1 FPS update rate. The compositing is done
 * by Cairo (which uses SIMD on ARM), and SDL2 just blits the
 * final frame to the display hardware.
 *
 * Environment variables:
 *   SDL_VIDEODRIVER=KMSDRM   (default on Pi without X11)
 *   SDL_VIDEODRIVER=x11      (for testing on desktop)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "display.h"
#include "image.h"
#include "config.h"
#include "borders.h"
#include "ctydat.h"
#include "dxspot.h"
#include "dxcluster.h"
#include "applet.h"
#include "applet_sysinfo.h"
#include "solarweather.h"
#include "satellite.h"
#include "newsticker.h"
#include "tzlookup.h"
#include "fetch.h"
#include "lightning.h"
#include "earthquake.h"
#include "wind.h"
#include "aurora.h"

extern double pic_applet_render_dxfeed(cairo_t *cr, double width,
                                       time_t now, void *user_data);
extern double pic_applet_render_muf(cairo_t *cr, double width,
                                    time_t now, void *user_data);
extern double pic_applet_render_solar(cairo_t *cr, double width,
                                      time_t now, void *user_data);
extern double pic_applet_render_voacap(cairo_t *cr, double width,
                                       time_t now, void *user_data);
extern void pic_voacap_invalidate_qth(void);
extern void pic_muf_invalidate_qth(void);
extern void pic_prop_invalidate_config(void);
extern void pic_layer_render_dxspots_legend(cairo_t *cr, int width, int height,
                                             time_t now, void *user_data);
extern void pic_layer_render_propagation_legend(cairo_t *cr, int width, int height,
                                                 time_t now, void *user_data);
extern void pic_prop_cleanup(void);
extern void pic_qth_invalidate(void);

#include <SDL2/SDL.h>
#include <cairo/cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

/* Maximum path length for constructing file paths */
#define MAX_PATH 512

/*
 * Forward declarations for layer render functions.
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
extern void pic_layer_render_cqzone(cairo_t *cr, int width, int height,
                                    time_t now, void *user_data);
extern void pic_layer_render_ituzone(cairo_t *cr, int width, int height,
                                     time_t now, void *user_data);
extern void pic_layer_render_sun(cairo_t *cr, int width, int height,
                                 time_t now, void *user_data);
extern void pic_layer_render_moon(cairo_t *cr, int width, int height,
                                  time_t now, void *user_data);
extern void pic_layer_render_maidenhead(cairo_t *cr, int width, int height,
                                        time_t now, void *user_data);
extern void pic_layer_render_dxspots(cairo_t *cr, int width, int height,
                                     time_t now, void *user_data);
extern void pic_layer_render_qth(cairo_t *cr, int width, int height,
                                 time_t now, void *user_data);
extern void pic_layer_render_satellite(cairo_t *cr, int width, int height,
                                       time_t now, void *user_data);
extern void pic_layer_render_hud(cairo_t *cr, int width, int height,
                                 time_t now, void *user_data);
extern void pic_layer_render_propagation(cairo_t *cr, int width, int height,
                                         time_t now, void *user_data);
extern void pic_layer_render_ticker(cairo_t *cr, int width, int height,
                                    time_t now, void *user_data);
extern void pic_layer_render_lightning(cairo_t *cr, int width, int height,
                                       time_t now, void *user_data);
extern void pic_layer_render_earthquake(cairo_t *cr, int width, int height,
                                        time_t now, void *user_data);
extern void pic_layer_render_wind(cairo_t *cr, int width, int height,
                                  time_t now, void *user_data);
extern void pic_wind_cleanup(void);
extern void pic_layer_render_cloud(cairo_t *cr, int width, int height,
                                   time_t now, void *user_data);
extern void pic_cloud_cleanup(void);
extern void pic_layer_render_precip(cairo_t *cr, int width, int height,
                                    time_t now, void *user_data);
extern void pic_precip_cleanup(void);
extern void pic_layer_render_aurora(cairo_t *cr, int width, int height,
                                    time_t now, void *user_data);
extern void pic_aurora_cleanup(void);

/* Shared legend stacking — the ticker bar_top value, exported so
 * legend-drawing layers (DX spots, band conditions) can position
 * themselves relative to the ticker without needing a direct pointer.
 * Updated each frame from the ticker struct. 0 = no ticker active. */
double pic_ticker_bar_top = 0;

/* Linux framebuffer and VT headers — used when SDL2/DRM is unavailable */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <sched.h>

/* DRM vblank timing — used by the ticker thread to synchronise
 * framebuffer writes with the display's vertical blanking interval.
 * The actual display still uses /dev/fb0; we only open the DRM
 * device for its vblank timing signal, not for rendering. */
#include <xf86drm.h>

/*
 * PIC_VT - The virtual terminal used for graphical output.
 * tty7, like X11 does. tty1-6 remain available for console login.
 * The user can switch with Ctrl+Alt+F1 (console) / F7 (clock).
 */
#define PIC_VT 7

/*
 * vt_is_active - Check if our VT is currently the active one.
 * Returns 1 if tty7 is active, 0 otherwise. Used to skip
 * framebuffer writes when the user has switched to a console.
 *
 * Keeps /dev/tty0 open to avoid repeated open/close on every
 * frame — on single-core Pi that overhead stalls the main thread.
 */
static int vt_is_active(void)
{
    static int tty0_fd = -2; /* -2 = not initialised */
    struct vt_stat vtstate;

    if (tty0_fd == -2) {
        tty0_fd = open("/dev/tty0", O_RDONLY | O_NOCTTY);
    }
    if (tty0_fd < 0) return 1; /* Can't check — assume active */

    if (ioctl(tty0_fd, VT_GETSTATE, &vtstate) < 0) {
        return 1;
    }
    return (vtstate.v_active == PIC_VT);
}

/*
 * vt_activate - Switch to our VT for graphical output.
 *
 * Uses /dev/tty0 (the current console) to issue VT_ACTIVATE,
 * which switches the display to tty7. We don't open tty7 as
 * our controlling terminal (that would capture Ctrl+C) and we
 * don't set KD_GRAPHICS (that blocks VT switching on the Pi).
 *
 * Instead, the init script hides the cursor and the framebuffer
 * writes cover the entire screen, so no console text is visible.
 */
static void vt_activate(void)
{
    int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        printf("display: cannot open /dev/tty0\n");
        return;
    }

    ioctl(fd, VT_ACTIVATE, PIC_VT);
    ioctl(fd, VT_WAITACTIVE, PIC_VT);
    close(fd);

    /* Hide the cursor on tty7 */
    {
        char tty_path[16];
        int tfd;
        snprintf(tty_path, sizeof(tty_path), "/dev/tty%d", PIC_VT);
        tfd = open(tty_path, O_WRONLY | O_NOCTTY);
        if (tfd >= 0) {
            write(tfd, "\033[?25l", 6);
            close(tfd);
        }
    }

    printf("display: switched to tty%d\n", PIC_VT);
}

/*
 * Global flags for signal handlers.
 * g_quit: set by SIGINT/SIGTERM to break the render loop.
 * g_reload: set by SIGHUP to trigger config reload and full re-render.
 */
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_reload = 0;

/*
 * signal_handler - Handle OS signals for clean shutdown and config reload.
 *
 * SIGINT/SIGTERM: set g_quit to break the render loop and exit cleanly.
 * SIGHUP: set g_reload to re-read config without restarting.
 *
 * We use sig_atomic_t so the write is atomic on all architectures.
 * The main loop reads these flags without a mutex.
 */
static void signal_handler(int sig)
{
    if (sig == SIGHUP) {
        g_reload = 1;
    } else {
        g_quit = 1;
    }
}

/*
 * get_resolution_suffix - Map height to map filename suffix.
 */
static const char *get_resolution_suffix(int height)
{
    switch (height) {
    case 720:  return "720p";
    case 1080: return "1080p";
    case 1440: return "1440p";
    case 2160: return "4k";
    default:   return "1080p";
    }
}

/*
 * show_splash - Display the boot splash logo on the framebuffer.
 *
 * Loads the splash JPEG, centres it on a dark background matching
 * the logo's background colour (#1e2024), and writes directly to
 * the framebuffer. Called once before map loading begins so the
 * user sees the logo within a second of the renderer starting.
 */
static void show_splash(unsigned char *fb_mem, int fb_stride,
                        int width, int height, const char *maps_dir)
{
    cairo_surface_t *splash;
    cairo_surface_t *frame;
    cairo_t *cr;
    char path[MAX_PATH];
    int logo_size, logo_x, logo_y, y;
    unsigned char *data;
    int stride;

    snprintf(path, sizeof(path), "%s/splash.jpg", maps_dir);
    splash = pic_image_load(path);
    if (!splash) {
        printf("display: no splash image at %s\n", path);
        return;
    }

    /* Create a frame-sized surface with the logo's dark background */
    frame = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cr = cairo_create(frame);

    /* Flat #3a3a3a — matches the splash.jpg logo fill exactly */
    cairo_set_source_rgb(cr, 0.227, 0.227, 0.227);
    cairo_paint(cr);

    /* Scale logo to ~40% of screen height, centred */
    logo_size = height * 2 / 5;
    logo_x = (width - logo_size) / 2;
    logo_y = (height - logo_size) / 2;

    {
        int src_w = cairo_image_surface_get_width(splash);
        int src_h = cairo_image_surface_get_height(splash);
        double scale = (double)logo_size / (src_w > src_h ? src_w : src_h);

        cairo_save(cr);
        cairo_translate(cr, logo_x, logo_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, splash, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(frame);
    cairo_surface_destroy(splash);

    /* Blit to framebuffer */
    data = cairo_image_surface_get_data(frame);
    stride = cairo_image_surface_get_stride(frame);

    for (y = 0; y < height; y++) {
        memcpy(fb_mem + y * fb_stride, data + y * stride, width * 4);
    }

    cairo_surface_destroy(frame);
    printf("display: splash screen shown\n");

    /* Hold the splash for a few seconds so the user sees it.
     * Without this delay, fast map loading can overwrite the
     * splash before the display has finished HDMI re-sync. */
    sleep(3);
}

/*
 * install_signal_handlers - Set up sigaction for clean shutdown.
 *
 * Shared between the framebuffer and SDL render paths.
 * Uses sigaction with SA_RESTART so sleep/recv aren't interrupted.
 * SIGPIPE is ignored (DX cluster socket may trigger it).
 */
static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/*
 * get_total_ram_mb - Read total physical RAM from /proc/meminfo.
 *
 * Returns MemTotal in megabytes. This is the total RAM reported by
 * the kernel, which already excludes GPU memory on the Pi (the GPU
 * split is handled by the firmware before Linux boots).
 *
 * Returns 0 on failure (file not readable, parse error).
 */
static int get_total_ram_mb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    int mb = 0;

    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        unsigned long kb;
        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
            mb = (int)(kb / 1024);
            break;
        }
    }
    fclose(f);
    return mb;
}

/*
 * RAM budget system — prevent OOM on memory-constrained Pis.
 *
 * Each enabled layer allocates a full-screen ARGB32 Cairo surface:
 *   1080p = 1920 * 1080 * 4 = ~8 MB per layer
 *   4K    = 3840 * 2160 * 4 = ~32 MB per layer
 *
 * On a Pi 1 Model A (256MB total, ~221MB after GPU), enabling 12+
 * layers at 1080p easily exceeds available RAM. Rather than letting
 * the OOM killer terminate us, we calculate a RAM budget at startup
 * and force-disable low-priority layers that exceed it.
 *
 * Each layer has a worst-case memory cost: its Cairo surface (which
 * scales with resolution) plus any data buffers, fetch threads, and
 * network overhead it needs. Heavy layers like wind (surface + 4MB
 * data arrays + decompression) cost more than lightweight ones like
 * grid (surface only).
 *
 * Fixed overhead covers base maps, compositing buffers, geographic
 * data, libraries, OS, and safety headroom. This is deliberately
 * conservative — running out of RAM is worse than disabling a layer.
 */

/* Fixed RAM reservation in MB. This is subtracted from total RAM
 * before budgeting layer surfaces. It must cover EVERYTHING that
 * isn't a layer surface — our process overhead AND the OS itself,
 * since MemTotal is the total RAM on the system, not just what's
 * free for us.
 *
 *   Base maps (day + night):     16 MB
 *   Compositing (frame + base):  16 MB
 *   Borders/timezone data:        4 MB
 *   Solar/DX/ticker (always on): 10 MB
 *   Thread stacks (~10 threads):  8 MB
 *   Cairo font caches:            5 MB
 *   Shared libraries (libc etc): 15 MB
 *   OS kernel + slab + buffers:  30 MB
 *   Dashboard process:            8 MB
 *   tmpfs (/tmp, /var/log):       4 MB
 *   Safety headroom:             14 MB
 *   ─────────────────────────────────
 *   Total:                      130 MB
 */
#define RAM_FIXED_OVERHEAD_MB 130

/*
 * Layer budget table — priority order and per-layer memory cost.
 *
 * Priority: highest first. When the budget is exceeded, layers are
 * force-disabled from the bottom (lowest priority) up.
 *
 * extra_mb: worst-case memory BEYOND the Cairo surface. Includes
 * data arrays, fetch buffers, TLS connections, TLE caches, etc.
 * The surface cost itself is calculated at runtime from the display
 * resolution (width * height * 4 bytes).
 */
static const struct {
    const char *name;
    int extra_mb;  /* Additional RAM beyond the surface */
} layer_budget[] = {
    /* Essential — always try to keep these */
    {"basemap",        0},  /* Static map render                     */
    {"daylight",       0},  /* Solar math only                       */
    {"hud",            0},  /* System time only                      */

    /* Lightweight — surface only, no data */
    {"grid",           0},  /* Calculated locally                    */
    {"borders",        0},  /* Data loaded in fixed overhead         */
    {"timezone",       0},  /* Data loaded in fixed overhead         */
    {"qth",            0},  /* Calculated locally                    */
    {"sun",            0},  /* Solar math only                       */
    {"moon",           0},  /* Lunar math only                       */

    /* Ham radio core — moderate overhead */
    {"dxspots",        1},  /* Spot list + cluster telnet thread     */
    {"bandconditions", 1},  /* Propagation grid calculation          */
    {"ticker",         1},  /* Item buffer + RSS fetch thread        */

    /* Moderate — network fetchers with data */
    {"satellites",     2},  /* TLE cache + track arrays + SGP4       */
    {"earthquakes",    1},  /* USGS JSON fetch buffer                */
    {"lightning",      2},  /* TLS WebSocket + LZW decode workspace  */

    /* Optional overlays */
    {"cqzone",         0},  /* Static data                           */
    {"ituzone",        0},  /* Static data                           */
    {"maidenhead",     0},  /* Calculated locally                    */

    /* Heavy — large data fetchers */
    {"aurora",         4},  /* 3MB fetch buffer + 360x181 grid       */
    {"wind",           4},  /* 4x 360x181 float grids + gunzip       */
    {"cloud",          0},  /* Shares wind data arrays               */
    {"precip",         0},  /* Shares wind data arrays               */
    {NULL,             0}
};

/*
 * get_layer_cost_mb - Return the total worst-case cost for a layer
 * in megabytes (surface + extra overhead).
 */
static int get_layer_cost_mb(const char *name, int surface_mb)
{
    int i;
    for (i = 0; layer_budget[i].name; i++) {
        if (strcmp(layer_budget[i].name, name) == 0)
            return surface_mb + layer_budget[i].extra_mb;
    }
    /* Unknown layer — assume surface only */
    return surface_mb;
}

/*
 * get_layer_priority - Return the priority index for a layer name.
 * Lower index = higher priority. Returns 9999 for unknown layers.
 */
static int get_layer_priority(const char *name)
{
    int i;
    for (i = 0; layer_budget[i].name; i++) {
        if (strcmp(layer_budget[i].name, name) == 0) return i;
    }
    return 9999;
}

/*
 * enforce_ram_budget - Force-disable low-priority layers that exceed
 * the RAM budget.
 *
 * Called after config load, before any surfaces are allocated. Sums
 * the worst-case memory cost of all enabled layers and compares
 * against available RAM. If over budget, disables the lowest-priority
 * enabled layers until the total fits.
 *
 * Returns the number of layers that were force-disabled.
 */
static int enforce_ram_budget(pic_layer_stack_t *stack,
                              int total_ram_mb, int surface_mb)
{
    int used_mb = 0;
    int budget_mb;
    int disabled = 0;
    int li, worst_pri, worst_li;

    /* If /proc/meminfo was unreadable, skip enforcement entirely
     * rather than making wrong decisions with zero data. */
    if (total_ram_mb <= 0) {
        printf("display: RAM budget: skipped (meminfo unavailable)\n");
        return 0;
    }

    budget_mb = total_ram_mb - RAM_FIXED_OVERHEAD_MB;
    if (budget_mb < surface_mb) budget_mb = surface_mb;

    /* Sum cost of all enabled layers */
    for (li = 0; li < stack->count; li++) {
        if (stack->layers[li].enabled)
            used_mb += get_layer_cost_mb(stack->layers[li].name, surface_mb);
    }

    /* Repeatedly find and disable the lowest-priority enabled layer
     * until we're within budget */
    while (used_mb > budget_mb) {
        worst_pri = -1;
        worst_li = -1;

        for (li = 0; li < stack->count; li++) {
            if (stack->layers[li].enabled) {
                int pri = get_layer_priority(stack->layers[li].name);
                if (pri > worst_pri) {
                    worst_pri = pri;
                    worst_li = li;
                }
            }
        }

        if (worst_li < 0) break;  /* Nothing left to disable */

        {
            int cost = get_layer_cost_mb(stack->layers[worst_li].name,
                                         surface_mb);
            printf("display: RAM budget: force-disabled '%s'"
                   " (%dMB > %dMB budget)\n",
                   stack->layers[worst_li].name, used_mb, budget_mb);
            used_mb -= cost;
        }
        stack->layers[worst_li].enabled = 0;
        disabled++;
    }

    if (disabled > 0)
        printf("display: RAM budget: %dMB used of %dMB available"
               " (%d layers disabled)\n", used_mb, budget_mb, disabled);

    return disabled;
}

/*
 * Ticker thread argument block — passed via pthread_create.
 */
struct ticker_thread_args {
    pic_ticker_t *ticker;
    pic_layer_stack_t *stack;
    unsigned char *fb_mem;
    int fb_stride;
    int drm_fd;       /* DRM device fd for vblank timing (-1 if unavailable) */
    int width;
    int height;
    int ticker_fps;   /* Matched to display refresh rate */
    int total_ram_mb; /* Total system RAM — skip mlockall on low-RAM systems */
    volatile int *quit;
};

/*
 * ticker_render_func - Dedicated ticker rendering thread.
 *
 * Runs at display refresh rate in its own thread, rendering only
 * the ticker bar region and copying those rows directly to the
 * framebuffer. The main thread skips these rows during its
 * full-frame blit, so there's no contention or stutter.
 *
 * Smoothness is achieved via three techniques:
 *
 *   1. Time-based scroll position — the scroll offset is calculated
 *      as a pure function of CLOCK_MONOTONIC wall time. A frame
 *      that delivers late shows the correct position for that
 *      instant, so delivery jitter is invisible to the eye.
 *
 *   2. DRM vblank sync — drmWaitVBlank on /dev/dri/cardN blocks
 *      until the vertical blanking interval. The memcpy happens
 *      during blanking, before the scan-out reaches the ticker
 *      bar at the bottom of the screen. Eliminates tearing.
 *
 *   3. SCHED_FIFO priority — reduces wake-up jitter from ~2ms
 *      (SCHED_OTHER) to ~50-100us, giving tighter vblank timing.
 *
 *   4. Margin-clipped blit — only the bar region between the
 *      applet margins is copied to the framebuffer each frame,
 *      keeping the memcpy small (~900KB at 4K) and fast.
 */
static void *ticker_render_func(void *arg)
{
    struct ticker_thread_args *a = (struct ticker_thread_args *)arg;
    cairo_surface_t *surface = NULL;
    int surface_y0 = 0, surface_h = 0;
    int y;
    struct timespec next_frame;
    long frame_ns = 1000000000L / a->ticker_fps;
    const int bpp = 4;  /* Bytes per pixel — BGRA32 framebuffer */
    double scroll_speed;  /* Pixels per second */

    /* Elevate to SCHED_FIFO priority 2 for low-jitter wake-ups.
     * Priority 2 is safely below kernel threads (99) and audio (50+)
     * but above all normal SCHED_OTHER threads. Requires root or
     * CAP_SYS_NICE — falls back to nice(10) if not available. */
    {
        struct sched_param sp;
        sp.sched_priority = 2;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
            printf("display: ticker thread using SCHED_FIFO priority 2\n");

            /* Lock currently mapped pages to prevent page faults
             * during the render loop. Only useful with SCHED_FIFO
             * since page faults under real-time scheduling cause
             * priority inversion. MCL_CURRENT only — MCL_FUTURE
             * would trap every subsequent mmap (Cairo surfaces,
             * allocator) and pin excessive RAM.
             *
             * Skip on systems with <= 512MB — pinning pages removes
             * the kernel's ability to reclaim file-backed pages
             * under memory pressure, which caused OOM kills on the
             * Pi 1 Model A (256MB). */
            if (a->total_ram_mb > 512) {
                if (mlockall(MCL_CURRENT) == 0)
                    printf("display: mlockall active\n");
            }
        } else {
            printf("display: ticker thread using default priority (SCHED_FIFO unavailable)\n");
        }
    }

    /* Scroll speed: pixels per second, proportional to display width.
     * The scroll position is calculated from CLOCK_MONOTONIC each
     * frame, so delivery jitter doesn't affect perceived smoothness —
     * a late frame simply shows where the text would be at that
     * instant. The eye integrates over time and sees smooth motion.
     *
     *   4K:   3840 * 0.04 = 153.6 px/s
     *   1080p: 1920 * 0.04 = 76.8 px/s
     *   720p:  1280 * 0.04 = 51.2 px/s
     */
    scroll_speed = a->width * 0.04;

    printf("display: ticker render thread started (%d FPS, %.0f px/s)\n",
           a->ticker_fps, scroll_speed);

    /* Seed the frame pacer from the monotonic clock */
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (!(*a->quit)) {
        int y0, y1, x0, x1;
        int ticker_enabled = 0;
        int is_holding = 0;
        int li;

        /* ── Frame pacing ──────────────────────────────────────
         * Sleep until the next frame time. The absolute-time mode
         * prevents drift — each frame is scheduled at an exact
         * multiple of frame_ns from the epoch, regardless of how
         * long rendering took. */
        next_frame.tv_nsec += frame_ns;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_nsec -= 1000000000L;
            next_frame.tv_sec++;
        }

        /* In headlines mode, hold phases show static text — drop
         * to 2 FPS. Reset the epoch on exit to avoid catch-up.
         * Read mode/phase under the mutex to prevent a data race
         * with pic_layer_render_ticker on multi-core Pi 3/4/5. */
        {
            int mode_snap, phase_snap;
            pthread_mutex_lock(&a->ticker->mutex);
            mode_snap = a->ticker->mode;
            phase_snap = a->ticker->headline_phase;
            pthread_mutex_unlock(&a->ticker->mutex);
            is_holding = (mode_snap >= TICKER_MODE_HEADLINES &&
                         (phase_snap == 1 || phase_snap == 3));
        }
        if (is_holding) {
            struct timespec hold = { 0, 500000000L };
            nanosleep(&hold, NULL);
            clock_gettime(CLOCK_MONOTONIC, &next_frame);
        } else {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                            &next_frame, NULL);
        }

        /* Check if ticker layer is enabled */
        for (li = 0; li < a->stack->count; li++) {
            if (strcmp(a->stack->layers[li].name, "ticker") == 0) {
                ticker_enabled = a->stack->layers[li].enabled;
                break;
            }
        }

        /* Read bar geometry under the mutex */
        pthread_mutex_lock(&a->ticker->mutex);
        y0 = (int)a->ticker->bar_top;
        y1 = (int)a->ticker->bar_bottom;
        x0 = a->ticker->left_margin > 0
             ? (int)a->ticker->left_margin
             : (int)(a->width * 0.05);
        x1 = a->ticker->right_margin > 0
             ? a->width - (int)a->ticker->right_margin
             : a->width - (int)(a->width * 0.05);
        pthread_mutex_unlock(&a->ticker->mutex);

        if (!ticker_enabled || y0 <= 0 || y1 <= y0) {
            pthread_mutex_lock(&a->ticker->mutex);
            a->ticker->bar_top = 0;
            a->ticker->bar_bottom = 0;
            pthread_mutex_unlock(&a->ticker->mutex);
            continue;
        }

        /* Clamp horizontal bar bounds to framebuffer width */
        if (x0 < 0) x0 = 0;
        if (x1 > a->width) x1 = a->width;
        if (x0 >= x1) { x0 = 0; x1 = a->width; }

        /* Allocate/reallocate surface sized to the ticker bar.
         * Full width, bar height — simple and reliable. */
        if (!surface || y0 != surface_y0 || (y1 - y0) != surface_h) {
            if (surface) cairo_surface_destroy(surface);
            surface_y0 = y0;
            surface_h = y1 - y0;
            surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                  a->width, surface_h);
            if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(surface);
                surface = NULL;
                continue;
            }
        }

        if (!surface) continue;

        /* ── Scroll position from monotonic clock ──────────────
         * Calculate the scroll offset as a pure function of time.
         * A frame that wakes up late shows the correct position
         * for that instant — delivery jitter is invisible.
         * Written under the mutex since the main thread can also
         * call pic_layer_render_ticker which reads scroll_offset. */
        if (a->ticker->mode == TICKER_MODE_SCROLL) {
            struct timespec mono;
            double t;
            clock_gettime(CLOCK_MONOTONIC, &mono);
            t = (double)mono.tv_sec + (double)mono.tv_nsec / 1000000000.0;
            pthread_mutex_lock(&a->ticker->mutex);
            /* Cap before cast: 2e9 < INT_MAX ensures (int) never
             * overflows. After fmod the value wraps harmlessly;
             * % scroll_total in the render path gives the correct
             * pixel position. */
            a->ticker->scroll_offset = (int)fmod(t * scroll_speed, 2e9);
            pthread_mutex_unlock(&a->ticker->mutex);
        }

        /* Render the ticker into the surface */
        {
            cairo_t *cr = cairo_create(surface);

            cairo_set_source_rgba(cr, 0, 0, 0, 0);
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_paint(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

            cairo_translate(cr, 0, -y0);
            pic_layer_render_ticker(cr, a->width, a->height,
                                    time(NULL), a->ticker);

            cairo_destroy(cr);
            cairo_surface_flush(surface);
        }

        /* Skip framebuffer write when user is on another VT */
        if (!vt_is_active()) continue;

        /* ── DRM vblank sync ───────────────────────────────────
         * Wait for the next vertical blanking interval before
         * writing to the framebuffer. The display scans top-to-
         * bottom; the ticker is at the bottom. After vblank fires
         * we have the entire blanking period plus the scan time
         * from top to ticker-bar-y to complete the copy. At 4K
         * the strip is ~900KB — well under 1ms on Cortex-A76.
         * If the DRM fd is unavailable, we proceed without sync
         * and rely on the fast copy to minimise the tear window. */
        if (a->drm_fd >= 0) {
            drmVBlank vbl;
            memset(&vbl, 0, sizeof(vbl));
            vbl.request.type = DRM_VBLANK_RELATIVE;
            vbl.request.sequence = 1;
            drmWaitVBlank(a->drm_fd, &vbl);
        }

        /* Copy only the bar region (between applet margins) to the
         * framebuffer. The ticker renders full-width into the surface
         * but we only blit the margin-to-margin region so we don't
         * overwrite applet pixels and cause flicker. */
        {
            unsigned char *data = cairo_image_surface_get_data(surface);
            int stride = cairo_image_surface_get_stride(surface);

            for (y = 0; y < surface_h && (y + surface_y0) < a->height; y++) {
                memcpy(a->fb_mem + (y + surface_y0) * a->fb_stride + x0 * bpp,
                       data + y * stride + x0 * bpp,
                       (x1 - x0) * bpp);
            }
        }
    }

    if (surface) cairo_surface_destroy(surface);
    printf("display: ticker render thread stopped\n");
    return NULL;
}

/*
 * pic_display_run_framebuffer - Direct Linux framebuffer render loop.
 *
 * Fallback when SDL2/DRM is unavailable. Opens /dev/fb0, queries
 * the resolution, memory-maps the framebuffer, and renders frames
 * directly by copying Cairo surface pixels into the mapped memory.
 *
 * This works on every Pi regardless of DRM/KMS status since the
 * legacy bcm2708_fb framebuffer is always available.
 *
 * The framebuffer pixel format is typically BGRA (32-bit), which
 * matches Cairo's ARGB32 format on little-endian systems — so
 * copying is a straight memcpy per row.
 */
static int pic_display_run_framebuffer(const char *maps_dir,
                                       int req_width, int req_height)
{
    int fb_fd;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    unsigned char *fb_mem;
    size_t fb_size;
    int width, height, fb_stride, display_hz, ticker_fps;
    int total_ram_mb, surface_mb;
    cairo_surface_t *frame_surface = NULL;
    cairo_surface_t *base_surface = NULL;   /* Cached composite of all layers except HUD */
    int base_valid = 0;                     /* Non-zero when base cache is usable */
    cairo_surface_t *black_marble = NULL;
    cairo_surface_t *blue_marble = NULL;
    pic_borders_t borders;
    pic_borders_t timezones;
    pic_borders_t cqzones;
    pic_borders_t ituzones;
    int borders_loaded = 0, timezones_loaded = 0;
    int cqzones_loaded = 0, ituzones_loaded = 0;
    pic_ctydat_t *ctydb = NULL;
    pic_spotlist_t dxspots;
    pic_solar_data_t solar;
    pic_satlist_t satellites;
    pic_ticker_t ticker;
    pic_lightning_t lightning;
    pic_earthquake_t earthquakes;
    pic_wind_t wind;
    pic_aurora_t aurora;
    pic_layer_stack_t stack;
    pic_applet_stack_t applets;
    pic_sysinfo_t sysinfo;
    char path[MAX_PATH];
    const char *res;

    install_signal_handlers();

    /* Open the framebuffer device */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "display: cannot open /dev/fb0\n");
        return 1;
    }

    /* Query framebuffer properties */
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "display: framebuffer ioctl failed\n");
        close(fb_fd);
        return 1;
    }

    /*
     * Use whatever resolution the firmware set at boot.
     * The Pi firmware auto-negotiates the best resolution from the
     * monitor's EDID. Runtime resolution switching via FBIOPUT
     * kills the HDMI signal on the legacy bcm2708_fb driver, so
     * we never try to change it.
     */
    (void)req_width;
    (void)req_height;

    width = vinfo.xres;
    height = vinfo.yres;
    fb_stride = finfo.line_length;
    fb_size = fb_stride * vinfo.yres_virtual;

    printf("display: framebuffer %dx%d, %d bpp, stride %d\n",
           width, height, vinfo.bits_per_pixel, fb_stride);

    /*
     * Calculate display refresh rate from framebuffer timing.
     * pixclock is in picoseconds. If the driver doesn't report
     * useful values (BCM2708 often doesn't), infer from resolution:
     *   4K (2160p) = typically 30 Hz on Pi HDMI
     *   Everything else = typically 60 Hz
     */
    display_hz = 0;
    if (vinfo.pixclock > 100) {
        unsigned long htotal = width + vinfo.left_margin +
                               vinfo.right_margin + vinfo.hsync_len;
        unsigned long vtotal = height + vinfo.upper_margin +
                               vinfo.lower_margin + vinfo.vsync_len;
        if (htotal > 0 && vtotal > 0) {
            display_hz = (int)(1000000000000ULL /
                         ((unsigned long long)vinfo.pixclock * htotal * vtotal));
        }
    }
    if (display_hz < 20 || display_hz > 240) {
        /* Driver didn't give useful timing — read from cmdline video=
         * parameter (e.g., video=HDMI-A-1:3840x2160-32@60 → 60 Hz).
         * Falls back to resolution-based guess if not found. */
        display_hz = 0;
        {
            FILE *cmdf = fopen("/proc/cmdline", "r");
            if (cmdf) {
                char cmdline[1024];
                if (fgets(cmdline, sizeof(cmdline), cmdf)) {
                    char *vp = strstr(cmdline, "video=");
                    if (vp) {
                        char *at = strchr(vp, '@');
                        if (at) display_hz = atoi(at + 1);
                    }
                }
                fclose(cmdf);
            }
        }
        if (display_hz < 20 || display_hz > 240)
            display_hz = (height >= 2160) ? 30 : 60;
    }

    /* Ticker FPS: match display refresh on multi-core, cap on single-core.
     * The ticker thread renders and blits every frame — on a single-core
     * ARM11 (Pi Zero) at 60 FPS this consumes ~70% CPU. 15 FPS is still
     * smooth enough for scrolling text and cuts ticker CPU by 75%. */
    {
        int cores = 1;
        FILE *ci = fopen("/proc/cpuinfo", "r");
        if (ci) {
            char cl[256];
            cores = 0;
            while (fgets(cl, sizeof(cl), ci))
                if (strncmp(cl, "processor", 9) == 0) cores++;
            fclose(ci);
            if (cores < 1) cores = 1;
        }
        /* Cap ticker FPS on single-core to reduce CPU. 30 FPS is
         * still smooth for the 0.9s scroll-in animation. */
        ticker_fps = (cores <= 1) ? 30 : display_hz;
    }

    printf("display: refresh=%d Hz, ticker=%d FPS\n", display_hz, ticker_fps);

    /* ── RAM budget ──────────────────────────────────────────
     * Calculate how many layer surfaces we can afford based on
     * total physical RAM and the display resolution. Each layer
     * surface is width * height * 4 bytes (ARGB32). On a Pi 1
     * Model A (256MB) at 1080p that's ~8MB per layer — with 12
     * layers the surfaces alone exceed available RAM.
     *
     * MemTotal from /proc/meminfo already excludes GPU memory
     * (the firmware carves it out before Linux boots), so we
     * don't need to subtract the GPU split. */
    total_ram_mb = get_total_ram_mb();
    surface_mb = (int)((long)width * height * 4 / (1024 * 1024));
    if (surface_mb < 1) surface_mb = 1;

    printf("display: RAM %dMB, surface %dMB, budget %dMB for layers\n",
           total_ram_mb, surface_mb,
           total_ram_mb - RAM_FIXED_OVERHEAD_MB);

    /* Write display status for the dashboard to read.
     * The renderer knows the actual resolution and calculated refresh
     * rate — the dashboard just reads this file. */
    {
        mkdir("/tmp/pi-clock-cache", 0755);
        FILE *sf = fopen("/tmp/pi-clock-cache/display-status", "w");
        if (sf) {
            int cores = 1;
            FILE *ci = fopen("/proc/cpuinfo", "r");
            if (ci) {
                char cl[256];
                cores = 0;
                while (fgets(cl, sizeof(cl), ci))
                    if (strncmp(cl, "processor", 9) == 0) cores++;
                fclose(ci);
                if (cores < 1) cores = 1;
            }
            fprintf(sf, "ACTUAL_RESOLUTION=%d,%d\n", width, height);
            fprintf(sf, "ACTUAL_REFRESH=%d\n", display_hz);
            fprintf(sf, "CPU_CORES=%d\n", cores);
            fprintf(sf, "RAM_TOTAL=%d\n", total_ram_mb);
            fprintf(sf, "RAM_BUDGET=%d\n",
                    total_ram_mb - RAM_FIXED_OVERHEAD_MB);
            fclose(sf);
        }
    }

    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "display: only 32-bit framebuffers supported\n");
        close(fb_fd);
        return 1;
    }

    /* Memory-map the framebuffer for direct pixel access */
    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        fprintf(stderr, "display: framebuffer mmap failed\n");
        close(fb_fd);
        return 1;
    }

    /* Claim tty7 for graphical output — like X11 does.
     * Sets KD_GRAPHICS mode so the kernel doesn't render a text
     * cursor or console on this VT. */
    vt_activate();

    /* Show splash logo immediately — before loading maps */
    if (maps_dir) {
        show_splash(fb_mem, fb_stride, width, height, maps_dir);
    }

    /* Create Cairo frame surface matching the framebuffer dimensions */
    frame_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                               width, height);

    /*
     * Base composite cache — holds the composited result of all layers
     * except HUD and ticker. Rebuilt only when a non-HUD layer changes
     * (every 5-60 seconds). Most frames just copy this and paint the
     * HUD on top, saving ~75% of composite bandwidth at 4K.
     */
    base_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                              width, height);
    if (cairo_surface_status(base_surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "display: base_surface allocation failed\n");
    }

    /* Load maps */
    res = get_resolution_suffix(height);
    printf("display: loading maps at %s resolution...\n", res);

    if (maps_dir) {
        snprintf(path, sizeof(path), "%s/black_marble/black_marble_%s.jpg",
                 maps_dir, res);
        black_marble = pic_image_load_scaled(path, width, height);

        snprintf(path, sizeof(path), "%s/blue_marble/blue_marble_%s.jpg",
                 maps_dir, res);
        blue_marble = pic_image_load_scaled(path, width, height);

        snprintf(path, sizeof(path), "%s/borders.dat", maps_dir);
        if (pic_borders_load(path, &borders) == 0) borders_loaded = 1;

        snprintf(path, sizeof(path), "%s/timezones.dat", maps_dir);
        if (pic_borders_load(path, &timezones) == 0) timezones_loaded = 1;

        snprintf(path, sizeof(path), "%s/cqzones.dat", maps_dir);
        if (pic_borders_load(path, &cqzones) == 0) cqzones_loaded = 1;

        snprintf(path, sizeof(path), "%s/ituzones.dat", maps_dir);
        if (pic_borders_load(path, &ituzones) == 0) ituzones_loaded = 1;

        /* Load callsign prefix database for DX spot resolution */
        snprintf(path, sizeof(path), "%s/cty.dat", maps_dir);
        ctydb = pic_ctydat_load(path);

        /* Load timezone grid for coordinate → timezone lookup */
        snprintf(path, sizeof(path), "%s/tzgrid.dat", maps_dir);
        pic_tz_load(path);
    }

    /* Start solar weather data fetcher */
    pic_solar_init(&solar);
    pic_solar_start(&solar);

    /* Initialise lightning, earthquake, and wind buffers — threads
     * started later, after layer config is loaded, only if on. */
    pic_lightning_init(&lightning);
    pic_earthquake_init(&earthquakes);
    pic_wind_init(&wind);
    pic_aurora_init(&aurora);

    /* Start satellite tracker — register satellites and fetch TLEs */
    pic_sat_init(&satellites);

    /* Popular / commonly used */
    pic_sat_add(&satellites, "iss",      "ISS",         25544);
    pic_sat_add(&satellites, "qo100",    "QO-100",      43700);

    /* FM voice repeaters */
    pic_sat_add(&satellites, "ao91",     "AO-91",       43017);  /* Sun-only */
    pic_sat_add(&satellites, "so50",     "SO-50",       27607);
    pic_sat_add(&satellites, "po101",    "PO-101",      43678);
    pic_sat_add(&satellites, "ao123",    "AO-123",      61781);  /* ASRTU-1 */
    pic_sat_add(&satellites, "so125",    "SO-125",      63492);  /* HADES-ICM */

    /* Linear transponders (SSB/CW) */
    pic_sat_add(&satellites, "ao7",      "AO-7",        7530);   /* Sun-only */
    pic_sat_add(&satellites, "ao73",     "AO-73",       39444);
    pic_sat_add(&satellites, "rs44",     "RS-44",       44909);
    pic_sat_add(&satellites, "jo97",     "JO-97",       43803);

    /* Digipeaters / APRS */
    pic_sat_add(&satellites, "sonate2",  "SONATE-2",    59112);

    /* Weather satellites (LRPT imagery) */
    pic_sat_add(&satellites, "meteor_m2",  "Meteor-M 2",   40069);
    pic_sat_add(&satellites, "meteor_m22", "Meteor-M2-2",  44387);
    pic_sat_add(&satellites, "meteor_m23", "Meteor-M2-3",  57166);
    pic_sat_add(&satellites, "meteor_m24", "Meteor-M2-4",  59051);

    pic_sat_load_config(&satellites);
    pic_sat_start(&satellites);

    /* Start news ticker — register sources, load config, start fetching.
     * Default mode: headlines on single-core (text parks for reading),
     * scroll on multi-core (smooth continuous scroll). */
    /* Detect CPU cores — single-core Pis are forced to headlines mode
     * regardless of config, since smooth scroll is too heavy. */
    pic_ticker_init(&ticker);
    {
        static int cpu_cores = 1;
        FILE *tci = fopen("/proc/cpuinfo", "r");
        if (tci) {
            char tcl[256];
            cpu_cores = 0;
            while (fgets(tcl, sizeof(tcl), tci))
                if (strncmp(tcl, "processor", 9) == 0) cpu_cores++;
            fclose(tci);
            if (cpu_cores < 1) cpu_cores = 1;
        }
        ticker.mode = (cpu_cores <= 1) ? TICKER_MODE_HEADLINES : TICKER_MODE_SCROLL;
        ticker.cpu_cores = cpu_cores;
        {
            const char *mname[] = {"scroll", "headlines"};
            printf("display: ticker mode=%s (cores=%d)\n",
                   mname[ticker.mode], cpu_cores);
        }
    }
    {
        pic_ticker_add_source(&ticker, "noaa_alerts",
                              "NOAA Space Weather Alerts", fetch_noaa_alerts);
        pic_ticker_add_source(&ticker, "dxworld",
                              "DX World News", fetch_dxworld);
        pic_ticker_add_source(&ticker, "arrl",
                              "ARRL News", fetch_arrl);
        pic_ticker_add_source(&ticker, "rsgb",
                              "RSGB GB2RS News", fetch_rsgb);
        pic_ticker_add_source(&ticker, "southgate",
                              "Southgate ARC News", fetch_southgate);
    }
    pic_ticker_load_config(&ticker);
    ticker.layer_stack = &stack;
    pic_ticker_start(&ticker);

    /* Initialise DX spot list and start cluster client */
    pic_spotlist_init(&dxspots);
    dxspots.ticker = &ticker;
    if (ctydb) {
        /*
         * Read cluster config from the renderer config file.
         * Default: dxc.ve7cc.net:7373 with the user's callsign.
         */
        const char *cluster_host = "dxc.wa9pie.net";
        int cluster_port = 8000;
        const char *callsign = "N0CALL";
        const char *home_prefix = "";

        /* Load all config values in one pass */
        {
            pic_renderer_conf_t rconf;
            double qth_lat, qth_lon;

            pic_load_renderer_conf(&rconf);
            if (rconf.callsign[0]) callsign = rconf.callsign;

            /* Set User-Agent for all HTTP requests */
            pic_fetch_set_useragent(VERSION, callsign);
            qth_lat = rconf.qth_lat;
            qth_lon = rconf.qth_lon;

            const pic_country_t *home = pic_ctydat_lookup(ctydb, callsign);
            if (home) {
                home_prefix = home->prefix;
                printf("display: home country: %s (%s)\n",
                       home->name, home->prefix);
            }

            printf("display: QTH %.4f, %.4f\n", qth_lat, qth_lon);

            /* Set timezone from QTH coordinates.
             * Priority: tzgrid lookup → system /etc/timezone (leave TZ alone).
             * Never fall back to a POSIX UTC offset — it doesn't handle DST. */
            {
                const char *tz_name = pic_tz_lookup(qth_lat, qth_lon);
                if (tz_name) {
                    setenv("TZ", tz_name, 1);
                    tzset();
                    printf("display: timezone %s (from grid)\n", tz_name);
                } else {
                    /* No grid — use system timezone as-is (set by pi-clock-config
                     * at boot from /etc/timezone). Don't override with a UTC offset
                     * that can't handle DST. */
                    const char *sys_tz = getenv("TZ");
                    if (!sys_tz || !sys_tz[0]) {
                        /* TZ not set — read /etc/timezone directly */
                        FILE *tzf = fopen("/etc/timezone", "r");
                        if (tzf) {
                            char tzline[64];
                            if (fgets(tzline, sizeof(tzline), tzf)) {
                                char *nl = strchr(tzline, '\n');
                                if (nl) *nl = '\0';
                                setenv("TZ", tzline, 1);
                                tzset();
                            }
                            fclose(tzf);
                        }
                    }
                    printf("display: timezone %s (from system)\n",
                           getenv("TZ") ? getenv("TZ") : "UTC");
                }
            }

            pic_dxcluster_start(cluster_host, cluster_port,
                                callsign, &dxspots, ctydb, home_prefix,
                                qth_lat, qth_lon);
        }
    }

    /* Build layer stack — ordered bottom-to-top as if looking
     * down from space. Earth surface at the bottom, HUD on top.
     *
     * EARTH SURFACE:
     *   basemap, daylight, borders, grid, timezone, cqzone, maidenhead
     * GROUND LEVEL (surface data):
     *   bandconditions, dxspots, earthquakes, qth, wind, lightning
     * ATMOSPHERE:
     *   cloud
     * SPACE (orbit / magnetosphere):
     *   aurora, satellites, sun, moon
     * HUD (always on top):
     *   ticker, hud
     */
    pic_layer_stack_init(&stack, width, height);

    /* ── Earth surface ────────────────────────────────────── */
    pic_layer_stack_add(&stack, "basemap",
                        pic_layer_render_basemap, 0, 1.0, black_marble);
    pic_layer_stack_add(&stack, "daylight",
                        pic_layer_render_daylight, 60, 1.0, blue_marble);
    if (borders_loaded)
        pic_layer_stack_add(&stack, "borders",
                            pic_layer_render_borders, 0, 1.0, &borders);
    pic_layer_stack_add(&stack, "grid",
                        pic_layer_render_grid, 0, 1.0, NULL);
    if (timezones_loaded)
        pic_layer_stack_add(&stack, "timezone",
                            pic_layer_render_timezone, 0, 1.0, &timezones);
    if (cqzones_loaded)
        pic_layer_stack_add(&stack, "cqzone",
                            pic_layer_render_cqzone, 0, 1.0, &cqzones);
    if (ituzones_loaded)
        pic_layer_stack_add(&stack, "ituzone",
                            pic_layer_render_ituzone, 0, 1.0, &ituzones);
    pic_layer_stack_add(&stack, "maidenhead",
                        pic_layer_render_maidenhead, 0, 1.0, NULL);

    /* ── Ground level (surface data) ──────────────────────── */
    pic_layer_stack_add(&stack, "bandconditions",
                        pic_layer_render_propagation, 10, 1.0, &solar);
    pic_layer_stack_add(&stack, "dxspots",
                        pic_layer_render_dxspots, 5, 1.0, &dxspots);
    pic_layer_stack_add(&stack, "earthquakes",
                        pic_layer_render_earthquake, 1, 1.0, &earthquakes);
    pic_layer_stack_add(&stack, "precip",
                        pic_layer_render_precip, 60, 1.0, &wind);
    pic_layer_stack_add(&stack, "qth",
                        pic_layer_render_qth, 0, 1.0, NULL);
    pic_layer_stack_add(&stack, "wind",
                        pic_layer_render_wind, 60, 1.0, &wind);
    pic_layer_stack_add(&stack, "lightning",
                        pic_layer_render_lightning, 1, 1.0, &lightning);

    /* ── Atmosphere ───────────────────────────────────────── */
    pic_layer_stack_add(&stack, "cloud",
                        pic_layer_render_cloud, 60, 1.0, &wind);

    /* ── Space (orbit / magnetosphere) ────────────────────── */
    pic_layer_stack_add(&stack, "aurora",
                        pic_layer_render_aurora, 60, 1.0, &aurora);
    pic_layer_stack_add(&stack, "satellites",
                        pic_layer_render_satellite, 5, 1.0, &satellites);
    pic_layer_stack_add(&stack, "sun",
                        pic_layer_render_sun, 60, 1.0, NULL);
    pic_layer_stack_add(&stack, "moon",
                        pic_layer_render_moon, 60, 1.0, NULL);

    /* ── HUD (always on top) ──────────────────────────────── */
    pic_layer_stack_add(&stack, "ticker",
                        pic_layer_render_ticker, 1, 1.0, &ticker);
    pic_layer_stack_add(&stack, "hud",
                        pic_layer_render_hud, 1, 1.0, NULL);

    /* Set sensible defaults for a fresh install (no config file).
     * Only the core map layers + HUD are on. Everything else is off
     * until the user enables it via the dashboard. These are
     * overridden by pic_load_layer_config() if a config exists. */
    {
        static const char *off_by_default[] = {
            "grid", "cqzone", "maidenhead", "dxspots",
            "satellites", "lightning", "earthquakes", "precip",
            "cloud", "wind", "aurora",
            "sun", "moon", "ticker", NULL
        };
        int li, di;
        for (di = 0; off_by_default[di]; di++) {
            for (li = 0; li < stack.count; li++) {
                if (strcmp(stack.layers[li].name, off_by_default[di]) == 0) {
                    stack.layers[li].enabled = 0;
                    break;
                }
            }
        }
    }

    /* Register legend render functions — these are called post-composite
     * at full opacity so legends are always readable regardless of
     * layer opacity. */
    pic_layer_set_legend(&stack, "dxspots",
                         pic_layer_render_dxspots_legend);
    pic_layer_set_legend(&stack, "bandconditions",
                         pic_layer_render_propagation_legend);

    /* Apply layer settings from config file (if it exists).
     * This overrides the defaults above with the user's saved prefs. */
    pic_load_layer_config(&stack);

    /* Enforce RAM budget — force-disable low-priority layers if the
     * user's config enables more layers than this system can afford.
     * Must run after config load but before surface allocation. */
    enforce_ram_budget(&stack, total_ram_mb, surface_mb);

    /*
     * Stagger heavy layer updates so they never fire on the same frame.
     *
     * On the Pi Zero W (single-core 1GHz), the propagation layer takes
     * ~400ms, VOACAP/MUF applets ~300ms each, satellites ~80ms, and
     * DX spots ~50ms. If these all fire on the same second, the frame
     * stalls for over a second and the clock visibly pauses.
     *
     * By offsetting each layer's last_rendered timestamp, we spread
     * the load across different seconds. The offset formula:
     *
     *   last_rendered = now - update_interval + offset
     *
     * This makes the layer first render after 'offset' seconds, then
     * at its normal interval from there. Heavy layers are spread
     * across the 10-second propagation cycle so at most one fires
     * per second.
     *
     * Stagger schedule (within a 10-second window):
     *   t+0s: daylight (60s interval, only fires every minute)
     *   t+1s: satellites
     *   t+3s: DX spots
     *   t+5s: propagation (heaviest — gets a clear window)
     *   t+7s: (reserved for future heavy layers)
     *
     * Layers with 1-second intervals (HUD, lightning, earthquakes,
     * ticker) are lightweight and don't need staggering.
     */
    {
        time_t now_stagger = time(NULL);
        int li;
        /*
         * Two groups to stagger:
         *
         * 1. The 5/10-second group (fire frequently, moderate cost):
         *    Spread across a 10-second window so at most one fires
         *    per second.
         *
         * 2. The 60-second group (normally cheap but expensive when
         *    new data arrives): Spread across a 60-second window so
         *    the GFS layers (wind 2-3s, cloud 100ms, precip 100ms)
         *    don't all recompute on the same frame when data arrives.
         *    Daylight (200ms every 60s) also offset to avoid overlap.
         *
         * Stagger schedule:
         *   t+0s:  daylight (200ms solar mask)
         *   t+1s:  satellites (50-100ms SGP4)
         *   t+3s:  dxspots (30-60ms arcs)
         *   t+5s:  bandconditions (400ms grid — heaviest)
         *   t+10s: wind (2-3s streamlines on data change)
         *   t+20s: cloud (100ms bilinear on data change)
         *   t+30s: precip (100ms bilinear on data change)
         *   t+40s: aurora (100ms bilinear every 15min)
         *   t+50s: sun/moon (trivial)
         */
        static const struct { const char *name; int offset; } stagger[] = {
            /* 5/10-second group */
            {"bandconditions", 5},
            {"satellites",     1},
            {"dxspots",        3},
            /* 60-second group — spread across the minute */
            {"daylight",       0},
            {"wind",          10},
            {"cloud",         20},
            {"precip",        30},
            {"aurora",        40},
            {"sun",           50},
            {"moon",          52},
            {NULL, 0}
        };
        int si;

        for (si = 0; stagger[si].name; si++) {
            for (li = 0; li < stack.count; li++) {
                if (strcmp(stack.layers[li].name, stagger[si].name) == 0) {
                    /* Set last_rendered so first render fires after
                     * 'offset' seconds from now. */
                    int interval = stack.layers[li].update_interval;
                    if (interval > 0) {
                        stack.layers[li].last_rendered =
                            now_stagger - interval + stagger[si].offset;
                    }
                    break;
                }
            }
        }
        printf("display: staggered heavy layer updates\n");
    }

    /* Start lightning/earthquake fetchers only if layer is enabled.
     * No point opening network connections for disabled layers. */
    {
        int li;
        for (li = 0; li < stack.count; li++) {
            if (strcmp(stack.layers[li].name, "lightning") == 0 &&
                stack.layers[li].enabled) {
                pic_lightning_start(&lightning);
            }
            if (strcmp(stack.layers[li].name, "earthquakes") == 0 &&
                stack.layers[li].enabled) {
                pic_earthquake_start(&earthquakes);
            }
            if ((strcmp(stack.layers[li].name, "wind") == 0 ||
                 strcmp(stack.layers[li].name, "cloud") == 0 ||
                 strcmp(stack.layers[li].name, "precip") == 0) &&
                stack.layers[li].enabled) {
                pic_wind_start(&wind); /* No-op if already running */
            }
            if (strcmp(stack.layers[li].name, "aurora") == 0 &&
                stack.layers[li].enabled) {
                pic_aurora_start(&aurora);
            }
        }
    }

    /* Initialise applets — link ticker for margin export */
    pic_applet_stack_init(&applets);
    applets.ticker = &ticker;
    pic_applet_stack_add(&applets, "dxfeed", "DX Cluster Feed",
                         pic_applet_render_dxfeed, 5,
                         200, APPLET_SIDE_RIGHT, &dxspots);
    pic_applet_stack_add(&applets, "muf", "MUF Estimate",
                         pic_applet_render_muf, 60,
                         175, APPLET_SIDE_LEFT, &solar);
    pic_applet_stack_add(&applets, "voacap", "Propagation",
                         pic_applet_render_voacap, 60,
                         225, APPLET_SIDE_RIGHT, &solar);
    pic_applet_stack_add(&applets, "solar", "Solar Weather",
                         pic_applet_render_solar, 60,
                         175, APPLET_SIDE_LEFT, &solar);

    /* System info applet — init and populate static fields */
    pic_sysinfo_init(&sysinfo, VERSION);
    sysinfo.display_w = width;
    sysinfo.display_h = height;
    sysinfo.refresh_hz = display_hz;
    sysinfo.layer_stack = &stack;
    sysinfo.applet_stack = &applets;

    pic_applet_stack_add(&applets, "sysinfo", "System Info",
                         pic_applet_render_sysinfo, 30,
                         200, APPLET_SIDE_LEFT, &sysinfo);
    pic_applet_stack_add(&applets, "features", "Active Features",
                         pic_applet_render_features, 5,
                         175, APPLET_SIDE_RIGHT, &sysinfo);

    /* Enable sysinfo and features by default for fresh installs.
     * All other applets default to off (set in applet.c). */
    {
        int ai;
        for (ai = 0; ai < applets.count; ai++) {
            if (strcmp(applets.applets[ai].name, "sysinfo") == 0 ||
                strcmp(applets.applets[ai].name, "features") == 0) {
                applets.applets[ai].enabled = 1;
            }
        }
    }

    pic_applet_load_config(&applets);

    /*
     * Smooth ticker rendering thread.
     *
     * Runs at display refresh rate in its own thread, rendering
     * only the ticker bar region to the framebuffer. Uses:
     *   - Time-based scroll position (hides delivery jitter)
     *   - DRM vblank sync (eliminates tearing)
     *   - SCHED_FIFO for tight timing
     *   - Pre-rendered text strip (no per-frame Cairo)
     * The main thread skips the ticker rows during its full-frame
     * blit, so there's no contention.
     */
    static struct ticker_thread_args targs;
    static pthread_t ticker_render_thread;
    static int ticker_thread_started = 0;
    static int drm_fd = -1;

    /* Pre-initialise vt_is_active() before spawning the ticker thread.
     * The function uses a lazily-opened static fd — calling it here
     * ensures the open() happens in the main thread, avoiding a
     * data race if both threads entered it simultaneously. */
    vt_is_active();

    /* Open the DRM device for vblank timing only. We do NOT use it
     * for display output — /dev/fb0 handles that. We just need the
     * vblank interrupt to synchronise our framebuffer writes with
     * the display scan-out, eliminating tearing.
     *
     * On Pi 5: card0 = vc4-drm (display controller, has vblank)
     *          card1 = v3d (3D GPU, no vblank)
     * On Pi 0-4: card0 or card1 may be vc4 depending on dtoverlay.
     * We probe both and pick the one with a connected display. */
    {
        int ci;
        for (ci = 0; ci <= 1 && drm_fd < 0; ci++) {
            char drm_path[32];
            int fd;
            snprintf(drm_path, sizeof(drm_path), "/dev/dri/card%d", ci);
            fd = open(drm_path, O_RDWR);
            if (fd >= 0) {
                drmVBlank vbl;
                memset(&vbl, 0, sizeof(vbl));
                vbl.request.type = DRM_VBLANK_RELATIVE;
                vbl.request.sequence = 0;
                if (drmWaitVBlank(fd, &vbl) == 0) {
                    drm_fd = fd;
                    printf("display: DRM vblank via %s\n", drm_path);
                } else {
                    close(fd);
                }
            }
        }
        if (drm_fd < 0)
            printf("display: DRM vblank unavailable (no tearing sync)\n");
    }

    targs.ticker = &ticker;
    targs.stack = &stack;
    targs.fb_mem = fb_mem;
    targs.fb_stride = fb_stride;
    targs.drm_fd = drm_fd;
    targs.width = width;
    targs.height = height;
    targs.ticker_fps = ticker_fps;
    targs.total_ram_mb = total_ram_mb;
    targs.quit = &g_quit;

    if (pthread_create(&ticker_render_thread, NULL,
                       ticker_render_func, &targs) == 0) {
        ticker_thread_started = 1;
    } else {
        fprintf(stderr, "display: ticker thread failed to start\n");
    }

    printf("display: entering framebuffer render loop at %dx%d\n",
           width, height);

    /* Main render loop */
    while (!g_quit) {
        time_t now = time(NULL);
        cairo_t *cr;
        unsigned char *cairo_data;
        int cairo_stride, y;

        /*
         * Check for config reload trigger file.
         * The dashboard creates /tmp/pi-clock-reload after saving config.
         * SIGHUP also sets g_reload for manual "kill -HUP" usage.
         */
        {
            struct stat trigger_st;
            if (stat("/tmp/pi-clock-reload", &trigger_st) == 0) {
                g_reload = 1;
                unlink("/tmp/pi-clock-reload");
            }
        }

        if (g_reload) {
            printf("display: config reload triggered\n");
            g_reload = 0;

            /* Re-read renderer config — single parse for all values */
            {
                pic_renderer_conf_t rconf;
                pic_load_renderer_conf(&rconf);

                pic_config.center_lon = rconf.center_lon;
                printf("display: center_lon=%.1f\n", rconf.center_lon);

                /* Update timezone from QTH */
                {
                    const char *tz_name = pic_tz_lookup(rconf.qth_lat, rconf.qth_lon);
                    if (tz_name) {
                        setenv("TZ", tz_name, 1);
                    } else {
                        char tz[32];
                        int offset_h;
                        if (rconf.qth_lon >= 0)
                            offset_h = (int)((rconf.qth_lon + 7.5) / 15.0);
                        else
                            offset_h = -(int)((-rconf.qth_lon + 7.5) / 15.0);
                        if (offset_h > 12) offset_h = 12;
                        if (offset_h < -12) offset_h = -12;
                        snprintf(tz, sizeof(tz), "UTC%+d", -offset_h);
                        setenv("TZ", tz, 1);
                    }
                    tzset();
                }
            }

            /* Reload subsystem configs */
            pic_load_layer_config(&stack);
            enforce_ram_budget(&stack, total_ram_mb, surface_mb);
            pic_dxcluster_reload(&dxspots);

            /* Start/stop lightning and earthquake fetchers based on
             * whether their layers are now enabled or disabled. */
            {
                int li2;
                int light_on = 0, quake_on = 0, wind_on = 0;
                int cloud_on = 0, precip_on = 0, aurora_on = 0;
                for (li2 = 0; li2 < stack.count; li2++) {
                    if (strcmp(stack.layers[li2].name, "lightning") == 0)
                        light_on = stack.layers[li2].enabled;
                    if (strcmp(stack.layers[li2].name, "earthquakes") == 0)
                        quake_on = stack.layers[li2].enabled;
                    if (strcmp(stack.layers[li2].name, "wind") == 0)
                        wind_on = stack.layers[li2].enabled;
                    if (strcmp(stack.layers[li2].name, "cloud") == 0)
                        cloud_on = stack.layers[li2].enabled;
                    if (strcmp(stack.layers[li2].name, "precip") == 0)
                        precip_on = stack.layers[li2].enabled;
                    if (strcmp(stack.layers[li2].name, "aurora") == 0)
                        aurora_on = stack.layers[li2].enabled;
                }
                if (light_on) {
                    pic_lightning_reload(&lightning);
                    pic_lightning_start(&lightning);
                } else {
                    pic_lightning_stop();
                }
                if (quake_on) {
                    pic_earthquake_reload(&earthquakes);
                    pic_earthquake_start(&earthquakes);
                } else {
                    pic_earthquake_stop();
                }
                if (wind_on || cloud_on || precip_on) {
                    pic_wind_start(&wind);
                } else {
                    pic_wind_stop();
                }
                if (aurora_on) {
                    pic_aurora_start(&aurora);
                } else {
                    pic_aurora_stop();
                }
            }
            pic_applet_load_config(&applets);
            pic_ticker_load_config(&ticker);
            pic_sat_load_config(&satellites);

            /* Link dxfeed applet to dxspots layer — hide applet when
             * the layer is disabled so the feed panel doesn't show
             * while DX spots are turned off on the map. */
            {
                int dx_on = 0, ai;
                int li;
                for (li = 0; li < stack.count; li++) {
                    if (strcmp(stack.layers[li].name, "dxspots") == 0) {
                        dx_on = stack.layers[li].enabled;
                        break;
                    }
                }
                if (!dx_on) {
                    for (ai = 0; ai < applets.count; ai++) {
                        if (strcmp(applets.applets[ai].name, "dxfeed") == 0) {
                            applets.applets[ai].enabled = 0;
                            break;
                        }
                    }
                }
            }

            /* Force MUF, propagation, and QTH marker to re-read */
            pic_muf_invalidate_qth();
            pic_voacap_invalidate_qth();
            pic_prop_invalidate_config();
            pic_qth_invalidate();

            /* Force all layers to re-render by resetting timestamps */
            {
                int li;
                for (li = 0; li < stack.count; li++) {
                    stack.layers[li].last_rendered = 0;
                }
            }
            base_valid = 0;
        }

        /* Export ticker position for legend stacking — read under
         * the ticker mutex to prevent torn double reads on
         * multi-core ARM (Pi 3/4/5). */
        pthread_mutex_lock(&ticker.mutex);
        pic_ticker_bar_top = ticker.bar_top;
        pthread_mutex_unlock(&ticker.mutex);

        /* Update layers — returns how many were re-rendered */
        pic_layer_stack_update(&stack, now);

        /*
         * Two-tier compositing for CPU efficiency.
         *
         * The base_surface caches the composite of all layers except
         * the HUD (which changes every second). It's only rebuilt
         * when a non-HUD layer actually re-renders — typically every
         * 5 seconds (DX/satellites) or 60 seconds (daylight/sun/moon).
         *
         * Most frames: copy base_surface (1 op) + paint HUD (tiny)
         * vs. old path: composite 14 full-screen layers every frame
         *
         * At 4K this saves ~400 MB of pixel reads per second.
         */
        {
            int base_dirty = 0;
            int li;

            /* Check if any non-HUD, non-ticker layer was re-rendered.
             * The ticker layer is skipped because the ticker thread
             * renders it directly to the framebuffer — compositing
             * it here would be wasted work. */
            for (li = 0; li < stack.count; li++) {
                if (stack.layers[li].dirty &&
                    strcmp(stack.layers[li].name, "hud") != 0 &&
                    strcmp(stack.layers[li].name, "ticker") != 0) {
                    base_dirty = 1;
                    break;
                }
            }

            /* Rebuild base composite if any base layer changed */
            if (base_dirty || !base_valid) {
                cairo_t *bcr = cairo_create(base_surface);
                cairo_set_source_rgb(bcr, 0, 0, 0);
                cairo_paint(bcr);

                for (li = 0; li < stack.count; li++) {
                    pic_layer_t *layer = &stack.layers[li];
                    if (!layer->enabled || !layer->surface) continue;
                    /* Skip HUD and ticker — they're handled separately */
                    if (strcmp(layer->name, "hud") == 0) continue;
                    if (strcmp(layer->name, "ticker") == 0) continue;
                    cairo_set_source_surface(bcr, layer->surface, 0, 0);
                    cairo_paint_with_alpha(bcr, layer->opacity);
                }

                /* Render applets into the base cache too */
                pic_applet_stack_render(&applets, bcr, width, height, now);

                cairo_destroy(bcr);
                cairo_surface_flush(base_surface);
                base_valid = 1;
            }

            /* Build final frame: base cache + legends + HUD on top */
            cr = cairo_create(frame_surface);
            cairo_set_source_surface(cr, base_surface, 0, 0);
            cairo_paint(cr);

            /* Draw legends at full opacity — not affected by layer opacity */
            pic_layer_stack_render_legends(&stack, cr, now);

            /* Paint just the HUD layer — clock text, very cheap */
            for (li = 0; li < stack.count; li++) {
                if (strcmp(stack.layers[li].name, "hud") == 0 &&
                    stack.layers[li].enabled) {
                    cairo_set_source_surface(cr, stack.layers[li].surface, 0, 0);
                    cairo_paint_with_alpha(cr, stack.layers[li].opacity);
                    break;
                }
            }

            cairo_destroy(cr);
            cairo_surface_flush(frame_surface);
        }

        /* Copy Cairo pixels to the framebuffer.
         * Cairo ARGB32 on little-endian = BGRA in memory,
         * which matches the Pi's 32-bit BGRA framebuffer. */
        cairo_data = cairo_image_surface_get_data(frame_surface);
        cairo_stride = cairo_image_surface_get_stride(frame_surface);

        /* Only blit to the framebuffer when our VT (tty7) is active.
         * When the user switches to tty1 for console login, we skip
         * the blit so their console isn't overwritten. Rendering
         * continues internally so the display is instantly correct
         * when they switch back to tty7 (Ctrl+Alt+F7). */
        if (!vt_is_active()) goto skip_blit;

        /* Blit the frame to the framebuffer, skipping the ticker
         * bar region. The ticker thread owns those pixels and
         * writes them at 30 FPS — if we overwrote them here,
         * it would cause a visible jitter each second. */
        {
            int ty0, ty1, tx0, tx1;

            /* Read ticker geometry under the mutex to prevent torn
             * 64-bit double reads on multi-core ARM (Pi 3/4/5).
             * Without this, a concurrent write from the ticker
             * render thread could produce garbage offsets. */
            pthread_mutex_lock(&ticker.mutex);
            ty0 = (int)ticker.bar_top;
            ty1 = (int)ticker.bar_bottom;
            /* Use 5% default when no applets set the margin —
             * matches the ticker renderer's default so the blit
             * area matches the drawn background bar exactly. */
            tx0 = ticker.left_margin > 0 ? (int)ticker.left_margin
                                         : (int)(width * 0.05);
            tx1 = ticker.right_margin > 0 ? width - (int)ticker.right_margin
                                          : width - (int)(width * 0.05);
            pthread_mutex_unlock(&ticker.mutex);

            /* Clamp to framebuffer bounds */
            if (ty0 < 0) ty0 = 0;
            if (ty1 > height) ty1 = height;
            if (tx0 < 0) tx0 = 0;
            if (tx1 > width) tx1 = width;

            if (ty0 > 0 && ty1 > ty0 && tx0 < tx1) {
                for (y = 0; y < height; y++) {
                    if (y >= ty0 && y < ty1) {
                        /* Ticker row: blit left and right of bar */
                        if (tx0 > 0)
                            memcpy(fb_mem + y * fb_stride,
                                   cairo_data + y * cairo_stride,
                                   tx0 * 4);
                        if (tx1 < width)
                            memcpy(fb_mem + y * fb_stride + tx1 * 4,
                                   cairo_data + y * cairo_stride + tx1 * 4,
                                   (width - tx1) * 4);
                    } else {
                        memcpy(fb_mem + y * fb_stride,
                               cairo_data + y * cairo_stride,
                               width * 4);
                    }
                }
            } else {
                for (y = 0; y < height; y++) {
                    memcpy(fb_mem + y * fb_stride,
                           cairo_data + y * cairo_stride,
                           width * 4);
                }
            }
        }

skip_blit:
        /* Sleep precisely until the next second boundary.
         * Using clock_gettime + nanosleep instead of sleep(1) ensures
         * the clock never drifts regardless of rendering time. On a
         * Pi Zero where a heavy frame can take 300ms+, a naive sleep(1)
         * would make the loop 1.3s and cause visible second-skipping. */
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            if (ts.tv_nsec > 0) {
                ts.tv_sec = 0;
                ts.tv_nsec = 1000000000L - ts.tv_nsec;
                nanosleep(&ts, NULL);
            }
        }
    }

    printf("display: exiting framebuffer render loop\n");

    /* Cleanup */
    pic_prop_cleanup();
    pic_wind_cleanup();
    pic_cloud_cleanup();
    pic_precip_cleanup();
    pic_aurora_cleanup();
    pic_layer_stack_destroy(&stack);
    if (frame_surface) cairo_surface_destroy(frame_surface);
    if (base_surface) cairo_surface_destroy(base_surface);
    if (black_marble) cairo_surface_destroy(black_marble);
    if (blue_marble) cairo_surface_destroy(blue_marble);
    if (borders_loaded) pic_borders_free(&borders);
    if (timezones_loaded) pic_borders_free(&timezones);
    if (cqzones_loaded) pic_borders_free(&cqzones);
    if (ituzones_loaded) pic_borders_free(&ituzones);
    /* Stop all background threads and wait for them to exit.
     * Must complete before munmap — the ticker render thread
     * writes directly to fb_mem and would crash on SIGBUS. */
    pic_dxcluster_stop();
    pic_solar_stop();
    pic_lightning_stop();
    pic_earthquake_stop();
    pic_wind_stop();
    pic_aurora_stop();
    pic_sat_stop();
    pic_ticker_stop();
    if (ticker_thread_started)
        pthread_join(ticker_render_thread, NULL);
    if (drm_fd >= 0)
        close(drm_fd);
    if (ctydb) pic_ctydat_free(ctydb);
    munmap(fb_mem, fb_size);
    close(fb_fd);

    return 0;
}

/*
 * pic_display_run - Main display function.
 *
 * Initialises SDL2, loads resources, and runs the render loop
 * until the user exits or a signal is received.
 * Falls back to direct framebuffer if SDL2 init fails.
 */
int pic_display_run(const char *maps_dir, int width, int height)
{
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    cairo_surface_t *frame_surface = NULL;
    cairo_surface_t *black_marble = NULL;
    cairo_surface_t *blue_marble = NULL;
    /*
     * NOTE: The SDL path is a minimal display-only mode used for
     * desktop development and testing. It renders the base map with
     * borders, grid, timezone, and HUD layers only. It does NOT
     * start DX cluster, satellite tracking, solar weather, news
     * ticker, or applets. For full functionality, use the direct
     * framebuffer path (pic_display_run_framebuffer) which is the
     * production code path on the Raspberry Pi.
     */
    pic_borders_t borders;
    pic_borders_t timezones;
    int borders_loaded = 0;
    int timezones_loaded = 0;
    pic_layer_stack_t stack;
    char path[MAX_PATH];
    const char *res;
    Uint32 window_flags;
    int ret = 0;

    /* Install signal handlers (sigaction + SIGPIPE ignore) */
    install_signal_handlers();

    /*
     * Try SDL2 first, fall back to direct framebuffer if it fails
     * or if it can only offer a dummy/offscreen driver (no real display).
     */
    printf("display: initialising SDL2...\n");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("display: SDL_Init failed: %s\n", SDL_GetError());
        printf("display: falling back to direct framebuffer mode\n");
        SDL_Quit();
        return pic_display_run_framebuffer(maps_dir, width, height);
    }

    /* Check that we got a real display driver, not offscreen/dummy */
    {
        const char *driver = SDL_GetCurrentVideoDriver();
        printf("display: video driver: %s\n", driver);

        if (driver && (strcmp(driver, "offscreen") == 0 ||
                       strcmp(driver, "dummy") == 0)) {
            printf("display: no real display driver available\n");
            printf("display: falling back to direct framebuffer mode\n");
            SDL_Quit();
            return pic_display_run_framebuffer(maps_dir, width, height);
        }
    }

    /*
     * Determine display resolution.
     *
     * If width/height are 0, auto-detect from the current display mode.
     * On the Pi via KMSDRM this gives the HDMI output resolution.
     * On a desktop with X11 this gives the desktop resolution.
     */
    if (width == 0 || height == 0) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
            width = mode.w;
            height = mode.h;
            printf("display: auto-detected %dx%d\n", width, height);
        } else {
            /* Fallback to 1080p if detection fails */
            width = 1920;
            height = 1080;
            printf("display: detection failed, using %dx%d\n", width, height);
        }
    }

    /* Create full-screen window.
     * SDL_WINDOW_FULLSCREEN_DESKTOP uses the native display resolution.
     * On KMSDRM this gives us exclusive access to the display. */
    window_flags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN;

    window = SDL_CreateWindow("Pi-Clock",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              width, height, window_flags);
    if (!window) {
        fprintf(stderr, "display: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        ret = 1;
        goto cleanup;
    }

    /* Hide the mouse cursor — this is a dedicated display */
    SDL_ShowCursor(SDL_DISABLE);

    /* Create SDL renderer — prefer hardware, fall back to software */
    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED |
                                  SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        printf("display: hardware renderer not available, using software\n");
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "display: SDL_CreateRenderer failed: %s\n",
                SDL_GetError());
        ret = 1;
        goto cleanup;
    }

    {
        SDL_RendererInfo info;
        SDL_GetRendererInfo(renderer, &info);
        printf("display: renderer: %s\n", info.name);
    }

    /*
     * Create a streaming texture for the frame.
     * ARGB8888 matches Cairo's ARGB32 format on little-endian systems.
     * SDL_TEXTUREACCESS_STREAMING allows us to lock and write pixels.
     */
    texture = SDL_CreateTexture(renderer,
                                SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                width, height);
    if (!texture) {
        fprintf(stderr, "display: SDL_CreateTexture failed: %s\n",
                SDL_GetError());
        ret = 1;
        goto cleanup;
    }

    /*
     * Create a Cairo surface for compositing.
     * This is where all layers are painted before being copied
     * to the SDL texture each frame.
     */
    frame_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                               width, height);
    if (cairo_surface_status(frame_surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "display: failed to create frame surface\n");
        ret = 1;
        goto cleanup;
    }

    /* Load map images */
    res = get_resolution_suffix(height);
    printf("display: loading maps at %s resolution...\n", res);

    if (maps_dir) {
        snprintf(path, sizeof(path), "%s/black_marble/black_marble_%s.jpg",
                 maps_dir, res);
        black_marble = pic_image_load_scaled(path, width, height);

        snprintf(path, sizeof(path), "%s/blue_marble/blue_marble_%s.jpg",
                 maps_dir, res);
        blue_marble = pic_image_load_scaled(path, width, height);

        snprintf(path, sizeof(path), "%s/borders.dat", maps_dir);
        if (pic_borders_load(path, &borders) == 0) {
            borders_loaded = 1;
        }

        snprintf(path, sizeof(path), "%s/timezones.dat", maps_dir);
        if (pic_borders_load(path, &timezones) == 0) {
            timezones_loaded = 1;
        }
    }

    /* Build the layer stack — same as snapshot mode */
    pic_layer_stack_init(&stack, width, height);

    pic_layer_stack_add(&stack, "basemap",
                        pic_layer_render_basemap, 0, 1.0, black_marble);
    pic_layer_stack_add(&stack, "daylight",
                        pic_layer_render_daylight, 60, 1.0, blue_marble);
    if (borders_loaded) {
        pic_layer_stack_add(&stack, "borders",
                            pic_layer_render_borders, 0, 1.0, &borders);
    }
    pic_layer_stack_add(&stack, "grid",
                        pic_layer_render_grid, 0, 1.0, NULL);
    if (timezones_loaded) {
        pic_layer_stack_add(&stack, "timezone",
                            pic_layer_render_timezone, 0, 1.0, &timezones);
    }
    pic_layer_stack_add(&stack, "hud",
                        pic_layer_render_hud, 1, 1.0, NULL);

    printf("display: entering render loop at %dx%d\n", width, height);

    /*
     * ════════════════════════════════════════════════════════
     * Main render loop
     * ════════════════════════════════════════════════════════
     *
     * Runs at ~1 FPS (driven by the HUD clock's 1-second interval).
     * Each iteration:
     *   1. Poll SDL events (quit, keyboard)
     *   2. Update stale layers
     *   3. Composite layers onto the Cairo frame surface
     *   4. Copy Cairo pixels to the SDL texture
     *   5. Present to the display
     *   6. Sleep until the next second boundary
     */
    while (!g_quit) {
        SDL_Event event;
        time_t now;
        cairo_t *cr;
        void *tex_pixels;
        int tex_pitch;
        unsigned char *cairo_data;
        int cairo_stride;

        /* Poll events — handle quit and keyboard */
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                g_quit = 1;
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    g_quit = 1;
                }
                break;
            }
        }

        if (g_quit) break;

        /* Get current time and update any stale layers */
        now = time(NULL);
        pic_layer_stack_update(&stack, now);

        /* Composite all layers onto the frame surface */
        cr = cairo_create(frame_surface);
        /* Clear to black first */
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        pic_layer_stack_composite(&stack, cr);
        cairo_destroy(cr);
        cairo_surface_flush(frame_surface);

        /*
         * Copy Cairo surface pixels to the SDL texture.
         *
         * Both use ARGB32 / ARGB8888 format so this is a straight
         * memcpy per row (accounting for potentially different strides).
         */
        cairo_data = cairo_image_surface_get_data(frame_surface);
        cairo_stride = cairo_image_surface_get_stride(frame_surface);

        if (SDL_LockTexture(texture, NULL, &tex_pixels, &tex_pitch) == 0) {
            int y;

            if (cairo_stride == tex_pitch) {
                /* Strides match — single memcpy for the whole frame */
                memcpy(tex_pixels, cairo_data, cairo_stride * height);
            } else {
                /* Different strides — copy row by row */
                int row_bytes = width * 4;
                for (y = 0; y < height; y++) {
                    memcpy((unsigned char *)tex_pixels + y * tex_pitch,
                           cairo_data + y * cairo_stride,
                           row_bytes);
                }
            }

            SDL_UnlockTexture(texture);
        }

        /* Present the frame to the display */
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /*
         * Sleep until the next second boundary.
         * This keeps CPU usage minimal — we only wake up once per
         * second to update the HUD clock.
         */
        {
            time_t next = time(NULL);
            if (next == now) {
                /* Still in the same second — sleep the remainder */
                SDL_Delay(200);
            }
        }
    }

    printf("display: exiting render loop\n");

cleanup:
    pic_prop_cleanup();
    pic_wind_cleanup();
    pic_cloud_cleanup();
    pic_precip_cleanup();
    pic_aurora_cleanup();
    pic_layer_stack_destroy(&stack);
    if (frame_surface) cairo_surface_destroy(frame_surface);
    if (black_marble) cairo_surface_destroy(black_marble);
    if (blue_marble) cairo_surface_destroy(blue_marble);
    if (borders_loaded) pic_borders_free(&borders);
    if (timezones_loaded) pic_borders_free(&timezones);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();

    return ret;
}
