/*
 * applet_sysinfo.c - System information applet
 *
 * Displays hostname, IP address, OS info, uptime, architecture,
 * display resolution, refresh rate, CPU cores, and renderer version.
 *
 * Static fields (hostname, arch, cores) are read once at init.
 * Dynamic fields (IP, uptime) are refreshed each render cycle.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet_sysinfo.h"
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * pic_sysinfo_init - Populate static system fields.
 *
 * Reads hostname, architecture, CPU cores, and OS version.
 * These don't change at runtime so we only read them once.
 */
void pic_sysinfo_init(pic_sysinfo_t *info, const char *version)
{
    struct utsname un;
    FILE *f;

    memset(info, 0, sizeof(*info));

    /* Version string */
    strncpy(info->version, version, sizeof(info->version) - 1);

    /* Hostname */
    if (gethostname(info->hostname, sizeof(info->hostname) - 1) != 0) {
        strcpy(info->hostname, "unknown");
    }

    /* Architecture from uname */
    if (uname(&un) == 0) {
        strncpy(info->arch, un.machine, sizeof(info->arch) - 1);
    } else {
        strcpy(info->arch, "unknown");
    }

    /* CPU cores */
    info->cpu_cores = 1;
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        int count = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "processor", 9) == 0) count++;
        }
        fclose(f);
        if (count > 0) info->cpu_cores = count;
    }

    /* OS name and version from Pi-Clock OS release file */
    f = fopen("/etc/pi-clock-os-release", "r");
    if (f) {
        char line[128];
        strcpy(info->os_name, "Pi-Clock OS");
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (strncmp(line, "VERSION=", 8) == 0) {
                strncpy(info->os_version, line + 8,
                        sizeof(info->os_version) - 1);
            }
        }
        fclose(f);
    } else {
        /* Not running on Pi-Clock OS */
        if (uname(&un) == 0) {
            strncpy(info->os_name, un.sysname, sizeof(info->os_name) - 1);
            strncpy(info->os_version, un.release, sizeof(info->os_version) - 1);
            /* Truncate long kernel versions for display */
            {
                char *dash = strchr(info->os_version, '-');
                if (dash) *dash = '\0';
            }
        }
    }

    /* Initial IP refresh */
    pic_sysinfo_refresh(info);
}

/*
 * pic_sysinfo_refresh - Update dynamic fields (IP address).
 *
 * Walks the interface list and picks the first non-loopback IPv4
 * address. Called periodically by the applet renderer.
 */
void pic_sysinfo_refresh(pic_sysinfo_t *info)
{
    struct ifaddrs *addrs, *ifa;

    info->ip_addr[0] = '\0';

    if (getifaddrs(&addrs) == 0) {
        for (ifa = addrs; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;

            /* Skip loopback */
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;

            inet_ntop(AF_INET,
                      &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                      info->ip_addr, sizeof(info->ip_addr));
            break;
        }
        freeifaddrs(addrs);
    }

    if (!info->ip_addr[0]) {
        strcpy(info->ip_addr, "No network");
    }
}

/*
 * format_uptime - Format seconds into "Xd Xh Xm" or "Xh Xm".
 */
static void format_uptime(long secs, char *buf, int buf_size)
{
    int days = (int)(secs / 86400);
    int hours = (int)((secs % 86400) / 3600);
    int mins = (int)((secs % 3600) / 60);

    if (days > 0) {
        snprintf(buf, buf_size, "%dd %dh %dm", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, buf_size, "%dh %dm", hours, mins);
    } else {
        snprintf(buf, buf_size, "%dm", mins);
    }
}

/*
 * get_uptime - Read system uptime from /proc/uptime.
 */
static long get_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0;
    if (f) {
        fscanf(f, "%lf", &up);
        fclose(f);
    }
    return (long)up;
}

/*
 * draw_row - Draw a label: value row.
 */
static double draw_row(cairo_t *cr, double y, double fs,
                       const char *label, const char *value)
{
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.9);

    /* Label in muted colour */
    cairo_set_source_rgba(cr, 0.55, 0.55, 0.65, 0.7);
    cairo_move_to(cr, 0, y);
    cairo_show_text(cr, label);

    /* Value in bright colour */
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
    {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);
        cairo_move_to(cr, ext.x_advance + fs * 0.3, y);
    }
    cairo_show_text(cr, value);

    return fs * 1.3;
}

/*
 * pic_applet_render_sysinfo - Render the system information panel.
 */
double pic_applet_render_sysinfo(cairo_t *cr, double width,
                                 time_t now, void *user_data)
{
    pic_sysinfo_t *info = (pic_sysinfo_t *)user_data;
    double fs = width / 12.0;
    double line_h = fs * 1.4;
    double y;
    int rows = 9;
    double total_h = fs * 2.5 + line_h * (rows - 0.5);
    char buf[64];

    (void)now;

    if (!cr) return total_h;
    if (!info) return total_h;

    /* Refresh IP every render (cheap syscall) */
    pic_sysinfo_refresh(info);

    /* Title */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 0, fs);
    cairo_show_text(cr, "System Info");

    /* Separator */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, fs + 4);
    cairo_line_to(cr, width, fs + 4);
    cairo_stroke(cr);

    y = fs * 2.5;

    /* Hostname */
    y += draw_row(cr, y, fs, "Host:", info->hostname);

    /* IP address */
    y += draw_row(cr, y, fs, "IP:", info->ip_addr);

    /* OS */
    if (info->os_version[0]) {
        snprintf(buf, sizeof(buf), "%s %s", info->os_name, info->os_version);
    } else {
        snprintf(buf, sizeof(buf), "%s", info->os_name);
    }
    y += draw_row(cr, y, fs, "OS:", buf);

    /* Architecture */
    y += draw_row(cr, y, fs, "Arch:", info->arch);

    /* CPU cores */
    snprintf(buf, sizeof(buf), "%d", info->cpu_cores);
    y += draw_row(cr, y, fs, "Cores:", buf);

    /* Display resolution */
    snprintf(buf, sizeof(buf), "%dx%d", info->display_w, info->display_h);
    y += draw_row(cr, y, fs, "Display:", buf);

    /* Refresh rate */
    snprintf(buf, sizeof(buf), "%d Hz", info->refresh_hz);
    y += draw_row(cr, y, fs, "Refresh:", buf);

    /* Uptime */
    format_uptime(get_uptime(), buf, sizeof(buf));
    y += draw_row(cr, y, fs, "Uptime:", buf);

    /* Renderer version */
    y += draw_row(cr, y, fs, "Version:", info->version);

    return y + fs * 0.3;
}
