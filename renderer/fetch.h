/*
 * fetch.h - Safe HTTP fetch helpers (no shell involved)
 *
 * Uses fork/execvp to call wget directly, bypassing the shell.
 * Prevents command injection from URLs or file paths.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_FETCH_H
#define PIC_FETCH_H

/*
 * pic_fetch_set_useragent - Set the User-Agent string for all fetches.
 *
 * Format: "Pi-Clock v{version} for {callsign}"
 * Call once at startup after reading the config.
 */
void pic_fetch_set_useragent(const char *version, const char *callsign);

/*
 * pic_fetch_to_buf - Fetch a URL and read the response into a buffer.
 *
 * Equivalent to: wget -q -O - --timeout=10 -U "user-agent" URL
 * Returns the number of bytes read, or -1 on failure.
 * The buffer is null-terminated on success.
 */
int pic_fetch_to_buf(const char *url, char *buf, int buf_size);

/*
 * pic_fetch_to_file - Download a URL to a file.
 *
 * Equivalent to: wget -q -O path --timeout=timeout URL
 * Returns 0 on success, -1 on failure.
 */
int pic_fetch_to_file(const char *url, const char *path, int timeout);

/*
 * pic_fetch_get_useragent - Get the current User-Agent string.
 *
 * Returns a pointer to the global User-Agent string set by
 * pic_fetch_set_useragent(). Used by modules that make their
 * own HTTP connections (e.g., lightning WebSocket).
 */
const char *pic_fetch_get_useragent(void);

#endif /* PIC_FETCH_H */
