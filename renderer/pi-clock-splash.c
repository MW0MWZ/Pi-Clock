/*
 * pi-clock-splash.c - Minimal boot splash for Pi-Clock
 *
 * Tiny standalone tool that displays the splash logo on the Linux
 * framebuffer and exits. Runs as an early boot service so the logo
 * appears within 1-2 seconds of kernel handoff, well before the
 * main renderer starts loading maps.
 *
 * No SDL, no Cairo, no dependencies beyond libc and stb_image.
 * Opens /dev/fb0 directly, loads the JPEG, centres it on a dark
 * background, writes pixels, and exits. The framebuffer retains
 * the image until the renderer overwrites it.
 *
 * Usage: pi-clock-splash /path/to/splash.jpg
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

int main(int argc, char *argv[])
{
    const char *img_path;
    int fb_fd;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    unsigned char *fb_mem;
    size_t fb_size;
    int fb_w, fb_h, fb_stride;

    unsigned char *img_data;
    int img_w, img_h, img_ch;
    FILE *f;
    long fsize;
    unsigned char *fdata;

    int logo_size, logo_x, logo_y;
    double scale;
    int x, y;

    if (argc < 2) {
        fprintf(stderr, "usage: pi-clock-splash <image.jpg>\n");
        return 1;
    }
    img_path = argv[1];

    /* Open framebuffer */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return 1;

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(fb_fd);
        return 1;
    }

    if (vinfo.bits_per_pixel != 32) {
        close(fb_fd);
        return 1;
    }

    fb_w = vinfo.xres;
    fb_h = vinfo.yres;
    fb_stride = finfo.line_length;
    fb_size = fb_stride * vinfo.yres_virtual;

    fb_mem = mmap(NULL, fb_size, PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        close(fb_fd);
        return 1;
    }

    /* Load the JPEG from file into memory for stb_image */
    f = fopen(img_path, "rb");
    if (!f) { munmap(fb_mem, fb_size); close(fb_fd); return 1; }

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    fdata = malloc(fsize);
    if (!fdata) { fclose(f); munmap(fb_mem, fb_size); close(fb_fd); return 1; }
    fread(fdata, 1, fsize, f);
    fclose(f);

    img_data = stbi_load_from_memory(fdata, fsize, &img_w, &img_h, &img_ch, 3);
    free(fdata);
    if (!img_data) { munmap(fb_mem, fb_size); close(fb_fd); return 1; }

    /* Flat #3a3a3a background — matches the splash.jpg logo fill
     * exactly so there's no visible boundary around the medallion. */
    for (y = 0; y < fb_h; y++) {
        unsigned char *row = fb_mem + y * fb_stride;
        for (x = 0; x < fb_w; x++) {
            /* BGRA format */
            row[x * 4 + 0] = 0x3a;  /* B */
            row[x * 4 + 1] = 0x3a;  /* G */
            row[x * 4 + 2] = 0x3a;  /* R */
            row[x * 4 + 3] = 0xff;  /* A */
        }
    }

    /* Scale and centre the logo at ~40% of screen height */
    logo_size = fb_h * 2 / 5;
    scale = (double)logo_size / (img_w > img_h ? img_w : img_h);
    logo_x = (fb_w - (int)(img_w * scale)) / 2;
    logo_y = (fb_h - (int)(img_h * scale)) / 2;

    /* Nearest-neighbour blit — fast and good enough for a splash */
    for (y = 0; y < (int)(img_h * scale) && (y + logo_y) < fb_h; y++) {
        int src_y = (int)(y / scale);
        if (src_y >= img_h) break;

        unsigned char *dst_row = fb_mem + (y + logo_y) * fb_stride;
        unsigned char *src_row = img_data + src_y * img_w * 3;

        for (x = 0; x < (int)(img_w * scale) && (x + logo_x) < fb_w; x++) {
            int src_x = (int)(x / scale);
            if (src_x >= img_w) break;

            int di = (x + logo_x) * 4;
            int si = src_x * 3;

            dst_row[di + 0] = src_row[si + 2]; /* B */
            dst_row[di + 1] = src_row[si + 1]; /* G */
            dst_row[di + 2] = src_row[si + 0]; /* R */
            dst_row[di + 3] = 0xff;            /* A */
        }
    }

    stbi_image_free(img_data);
    munmap(fb_mem, fb_size);
    close(fb_fd);

    return 0;
}
