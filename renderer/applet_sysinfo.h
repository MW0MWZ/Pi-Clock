/*
 * applet_sysinfo.h - System information data for the sysinfo applet
 *
 * Holds static and dynamic system data gathered at startup and
 * periodically refreshed. Display width/height and refresh rate
 * are set by display.c after framebuffer init; the rest is read
 * from the OS at init time.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_APPLET_SYSINFO_H
#define PIC_APPLET_SYSINFO_H

#include "applet.h"
#include "layer.h"

typedef struct {
    char hostname[64];
    char ip_addr[46];       /* IPv4 or IPv6 */
    char os_name[64];       /* "Pi-Clock OS" or "Linux" */
    char os_version[32];    /* From /etc/pi-clock-os-release */
    char arch[16];          /* e.g. "armhf", "aarch64" */
    char version[16];       /* Renderer version string */
    int  display_w;         /* Framebuffer width */
    int  display_h;         /* Framebuffer height */
    int  refresh_hz;        /* Display refresh rate */
    int  cpu_cores;         /* Number of CPU cores */

    /* Pointers for live status — set by display.c */
    pic_layer_stack_t  *layer_stack;
    void               *applet_stack; /* pic_applet_stack_t* */
} pic_sysinfo_t;

/* Populate static fields (hostname, arch, cores, OS info).
 * Call once at startup. Display fields set separately by display.c. */
void pic_sysinfo_init(pic_sysinfo_t *info, const char *version);

/* Refresh dynamic fields (IP, uptime). Called by the applet render. */
void pic_sysinfo_refresh(pic_sysinfo_t *info);

/* Applet render functions */
double pic_applet_render_sysinfo(cairo_t *cr, double width,
                                 time_t now, void *user_data);

double pic_applet_render_features(cairo_t *cr, double width,
                                  time_t now, void *user_data);

#endif /* PIC_APPLET_SYSINFO_H */
