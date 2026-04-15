/*
 * lightning.c - Blitzortung lightning strike fetcher
 *
 * Runs a background thread that connects to the Blitzortung
 * community lightning detection network via WebSocket (RFC 6455).
 * The server sends JSON-encoded strike events in real time:
 *
 *   {"time":1718000000000000000,"lat":48.12,"lon":11.58,...}
 *
 * Each strike is stored in a circular buffer with a local
 * timestamp. The lightning map layer reads this buffer each
 * frame and draws strikes that are within the fade window.
 *
 * WebSocket protocol (minimal client):
 *   1. TCP connect to ws1.blitzortung.org:80
 *   2. Send HTTP Upgrade request with Sec-WebSocket-Key
 *   3. Read 101 Switching Protocols response
 *   4. Send subscription frame: {"a":1}
 *   5. Read text frames containing strike JSON
 *
 * The WebSocket implementation is intentionally minimal — we
 * only handle text frames (opcode 0x1), ping/pong (0x9/0xA),
 * and close (0x8). No extensions, no fragmentation. This is
 * all Blitzortung sends.
 *
 * Reconnects automatically on disconnect with exponential
 * backoff (5s -> 10s -> 20s -> ... -> 300s max).
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "lightning.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/*
 * Blitzortung WebSocket server details.
 * Port 443 (wss://) with TLS. Multiple servers available
 * (ws1 through ws8) for load balancing.
 */
#define BLITZ_HOST "ws1.blitzortung.org"
#define BLITZ_PORT 443

/* Maximum reconnect backoff in seconds */
#define MAX_BACKOFF 300

/* Thread state */
static pthread_t lightning_thread;
static volatile int lightning_running = 0;
static pic_lightning_t *cfg_data = NULL;

/*
 * pic_lightning_init - Initialise the strike buffer to empty.
 */
void pic_lightning_init(pic_lightning_t *data)
{
    memset(data, 0, sizeof(*data));
    data->fade_ms = PIC_LIGHTNING_FADE_DEFAULT_MS;
    pthread_mutex_init(&data->mutex, NULL);
}

/*
 * add_strike - Thread-safe insertion into the circular buffer.
 *
 * Overwrites the oldest entry when the buffer is full. The
 * renderer only draws strikes within the fade window, so old
 * entries are already invisible by the time they're overwritten.
 */
static void add_strike(pic_lightning_t *data, double lat, double lon)
{
    pic_strike_t *s;

    pthread_mutex_lock(&data->mutex);
    s = &data->strikes[data->head];
    s->lat = lat;
    s->lon = lon;
    gettimeofday(&s->when, NULL);

    data->head = (data->head + 1) % PIC_MAX_STRIKES;
    if (data->count < PIC_MAX_STRIKES) data->count++;
    pthread_mutex_unlock(&data->mutex);
}

/*
 * lzw_decode - Decompress Blitzortung's LZW-encoded JSON.
 *
 * Blitzortung encodes their WebSocket payloads using a variant of
 * LZW compression (Lempel-Ziv-Welch). This is the C equivalent of
 * their JavaScript decode() function:
 *
 *   function decode(b) {
 *       var a, e = {}, d = b.split(""), c = d[0], f = c, g = [c],
 *           h = 256; o = h;
 *       for (b = 1; b < d.length; b++)
 *           a = d[b].charCodeAt(0),
 *           a = h > a ? d[b] : e[a] ? e[a] : f + c,
 *           g.push(a), c = a.charAt(0), e[o] = f + c, o++, f = a;
 *       return g.join("")
 *   }
 *
 * The dictionary starts with entries 0-255 (single characters).
 * New entries are built by appending the first character of each
 * decoded string to the previous decoded string, indexed from 256
 * upward. Input characters with code >= 256 are dictionary lookups.
 *
 * Returns the number of decoded bytes, or -1 on error.
 * The output buffer is null-terminated.
 */
/*
 * LZW dictionary limits. Blitzortung payloads decode to ~2-4 KB
 * of JSON, so 4096 entries and 32 KB of string storage is generous.
 */
#define LZW_MAX_ENTRIES  4096
#define LZW_DICT_BUF_SZ 32768

/*
 * lzw_workspace_t - Heap-allocated workspace for the LZW decoder.
 *
 * Allocated once per decode call and freed on return. Avoids
 * static buffers (not thread-safe) and large stack frames
 * (dangerous on musl's default thread stack).
 */
typedef struct {
    char dict_buf[LZW_DICT_BUF_SZ]; /* Flat string storage         */
    int  dict_off[LZW_MAX_ENTRIES];  /* Offset of each entry        */
    int  dict_len[LZW_MAX_ENTRIES];  /* Length of each entry         */
} lzw_workspace_t;

static int lzw_decode(const unsigned char *input, int input_len,
                      char *output, int output_size)
{
    lzw_workspace_t *ws;
    int dict_count, dict_buf_pos;
    int out_pos = 0;
    int i;

    /* Local copy of previous decoded string — avoids dangling
     * pointer into dict_buf when the buffer advances. */
    char prev_buf[256];
    int prev_len;

    /* Current decoded string (pointer into ws->dict_buf) */
    const char *curr;
    int curr_len;

    if (input_len < 1) return -1;

    ws = (lzw_workspace_t *)malloc(sizeof(lzw_workspace_t));
    if (!ws) return -1;

    /* Initialise dictionary with single-byte entries 0-255 */
    dict_buf_pos = 0;
    for (i = 0; i < 256; i++) {
        ws->dict_off[i] = dict_buf_pos;
        ws->dict_buf[dict_buf_pos] = (char)i;
        ws->dict_len[i] = 1;
        dict_buf_pos++;
    }
    dict_count = 256;

    /* Decode the first UTF-8 codepoint */
    {
        unsigned int cp;
        int bytes_consumed;

        cp = input[0];
        if ((cp & 0x80) == 0) {
            bytes_consumed = 1;
        } else if ((cp & 0xE0) == 0xC0 && input_len > 1) {
            cp = ((cp & 0x1F) << 6) | (input[1] & 0x3F);
            bytes_consumed = 2;
        } else if ((cp & 0xF0) == 0xE0 && input_len > 2) {
            cp = ((cp & 0x0F) << 12) | ((input[1] & 0x3F) << 6) |
                 (input[2] & 0x3F);
            bytes_consumed = 3;
        } else {
            bytes_consumed = 1;
        }

        if (cp < 256 && out_pos < output_size - 1) {
            output[out_pos++] = (char)cp;
        }

        /* Copy first character into prev_buf */
        prev_buf[0] = (cp < 256) ? (char)cp : '\0';
        prev_len = 1;

        /* Process remaining input codepoints */
        i = bytes_consumed;
        while (i < input_len && out_pos < output_size - 2) {
            unsigned int code;
            int consumed;
            unsigned char b = input[i];

            /* Decode UTF-8 codepoint */
            if ((b & 0x80) == 0) {
                code = b;
                consumed = 1;
            } else if ((b & 0xE0) == 0xC0 && i + 1 < input_len) {
                code = ((b & 0x1F) << 6) | (input[i+1] & 0x3F);
                consumed = 2;
            } else if ((b & 0xF0) == 0xE0 && i + 2 < input_len) {
                code = ((b & 0x0F) << 12) | ((input[i+1] & 0x3F) << 6) |
                       (input[i+2] & 0x3F);
                consumed = 3;
            } else {
                i++;
                continue;
            }
            i += consumed;

            /* Look up the code in the dictionary */
            if (code < 256) {
                curr = ws->dict_buf + ws->dict_off[code];
                curr_len = 1;
            } else if (code < (unsigned int)dict_count &&
                       ws->dict_len[code] > 0) {
                curr = ws->dict_buf + ws->dict_off[code];
                curr_len = ws->dict_len[code];
            } else {
                /* Unknown code (LZW special case): prev + prev[0] */
                if (dict_buf_pos + prev_len + 1 >= LZW_DICT_BUF_SZ) {
                    printf("lightning: LZW dict_buf overflow\n");
                    break;
                }
                memcpy(ws->dict_buf + dict_buf_pos, prev_buf, prev_len);
                ws->dict_buf[dict_buf_pos + prev_len] = prev_buf[0];
                curr = ws->dict_buf + dict_buf_pos;
                curr_len = prev_len + 1;
                dict_buf_pos += curr_len;
            }

            /* Append decoded string to output */
            {
                int copy_len = curr_len;
                if (out_pos + copy_len >= output_size - 1)
                    copy_len = output_size - 1 - out_pos;
                memcpy(output + out_pos, curr, copy_len);
                out_pos += copy_len;
            }

            /* Add new dictionary entry: prev + curr[0] */
            if (dict_count < LZW_MAX_ENTRIES &&
                dict_buf_pos + prev_len + 1 < LZW_DICT_BUF_SZ) {
                ws->dict_off[dict_count] = dict_buf_pos;
                memcpy(ws->dict_buf + dict_buf_pos, prev_buf, prev_len);
                ws->dict_buf[dict_buf_pos + prev_len] = curr[0];
                ws->dict_len[dict_count] = prev_len + 1;
                dict_buf_pos += prev_len + 1;
                dict_count++;
            }

            /* Copy curr into prev_buf for next iteration */
            if (curr_len < (int)sizeof(prev_buf)) {
                memcpy(prev_buf, curr, curr_len);
                prev_len = curr_len;
            }
        }
    }

    free(ws);
    output[out_pos] = '\0';
    return out_pos;
}

/*
 * parse_strike_json - Extract lat and lon from a JSON strike line.
 *
 * Blitzortung sends compact JSON objects. We use simple string
 * scanning rather than a full JSON parser.
 *
 * Example input (after LZW decode):
 *   {"time":1718000000000000000,"lat":48.1234,"lon":11.5678,...}
 *
 * Returns 1 on success, 0 if the line doesn't contain valid data.
 */
static int parse_strike_json(const char *json, double *lat, double *lon)
{
    const char *p;

    p = strstr(json, "\"lat\":");
    if (!p) return 0;
    *lat = strtod(p + 6, NULL);

    p = strstr(json, "\"lon\":");
    if (!p) return 0;
    *lon = strtod(p + 6, NULL);

    if (*lat < -90 || *lat > 90 || *lon < -180 || *lon > 180) {
        return 0;
    }

    return 1;
}

/*
 * tcp_connect_blitz - Connect to the Blitzortung server via TCP.
 *
 * Non-blocking connect with 10-second timeout, same pattern as
 * the DX cluster client. Returns socket fd or -1 on failure.
 */
static int tcp_connect_blitz(void)
{
    struct addrinfo hints, *res, *rp;
    char port_str[8];
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", BLITZ_PORT);

    printf("lightning: resolving %s...\n", BLITZ_HOST);
    if (getaddrinfo(BLITZ_HOST, port_str, &hints, &res) != 0) {
        printf("lightning: DNS lookup failed for %s\n", BLITZ_HOST);
        return -1;
    }

    fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        int flags, ret;
        fd_set wfds;
        struct timeval tv;

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) { close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }

        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }

        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        ret = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (ret > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) {
                fcntl(fd, F_SETFL, flags);
                break;
            }
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* ── Minimal TLS WebSocket client (RFC 6455 over TLS) ────────── */

/*
 * The Blitzortung WebSocket servers require TLS (wss:// on port 443).
 * We use OpenSSL for the TLS layer and implement the WebSocket framing
 * manually — it's simple enough that no WebSocket library is needed.
 *
 * All I/O goes through SSL_read/SSL_write instead of recv/send.
 */

/*
 * ssl_read_fully - Read exactly n bytes through the TLS connection.
 *
 * Returns 1 on success, 0 on EOF/error.
 */
static int ssl_read_fully(SSL *ssl, unsigned char *buf, int n)
{
    int total = 0;
    while (total < n) {
        int r = SSL_read(ssl, buf + total, n - total);
        if (r <= 0) return 0;
        total += r;
    }
    return 1;
}

/*
 * ws_handshake - Perform the WebSocket opening handshake over TLS.
 *
 * Sends an HTTP Upgrade request and reads the 101 response.
 * Returns 1 on success, 0 on failure.
 */
static int ws_handshake(SSL *ssl)
{
    char req[512];
    char resp[4096];
    int n, total;

    /*
     * Blitzortung WebSocket path: different servers may use different
     * paths. Try the known working paths from open-source clients.
     * The key path is typically just "/" but nginx may need a
     * specific location block. We also try the full map host as Origin.
     */
    n = snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: https://map.blitzortung.org\r\n"
        "\r\n",
        BLITZ_HOST, pic_fetch_get_useragent());

    if (SSL_write(ssl, req, n) != n) {
        printf("lightning: failed to send handshake\n");
        return 0;
    }

    /* Read until we see the complete HTTP response header */
    total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int r = SSL_read(ssl, resp + total, sizeof(resp) - 1 - total);
        if (r <= 0) {
            printf("lightning: handshake recv failed\n");
            return 0;
        }
        total += r;
        resp[total] = '\0';

        if (strstr(resp, "\r\n\r\n")) break;
    }

    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        /* Log the full response for debugging */
        printf("lightning: handshake failed (%d bytes):\n%s\n",
               total, resp);
        return 0;
    }

    printf("lightning: WebSocket handshake complete (TLS)\n");
    return 1;
}

/*
 * ws_send_text - Send a masked text frame over TLS WebSocket.
 *
 * Client-to-server frames MUST be masked (RFC 6455 §5.1).
 */
static int ws_send_text(SSL *ssl, const char *text)
{
    unsigned char frame[256];
    int len = strlen(text);
    unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    int i;

    if (len > 125) return -1;

    frame[0] = 0x81;           /* FIN + text opcode */
    frame[1] = 0x80 | len;    /* MASK bit + length  */
    memcpy(frame + 2, mask, 4);

    for (i = 0; i < len; i++) {
        frame[6 + i] = text[i] ^ mask[i % 4];
    }

    return (SSL_write(ssl, frame, 6 + len) == 6 + len) ? 0 : -1;
}

/*
 * ws_send_pong - Reply to a ping frame with a masked pong.
 */
static void ws_send_pong(SSL *ssl, const unsigned char *ping_data,
                         int ping_len)
{
    unsigned char frame[256];
    unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    int i;

    if (ping_len > 125) ping_len = 125;

    frame[0] = 0x8A;           /* FIN + pong opcode  */
    frame[1] = 0x80 | ping_len;
    memcpy(frame + 2, mask, 4);

    for (i = 0; i < ping_len; i++) {
        frame[6 + i] = ping_data[i] ^ mask[i % 4];
    }

    SSL_write(ssl, frame, 6 + ping_len);
}

/*
 * ws_read_frame - Read one WebSocket frame over TLS.
 *
 * Returns the payload length on success, -1 on error/close.
 */
static int ws_read_frame(SSL *ssl, unsigned char *buf, int buf_size,
                         int *opcode)
{
    unsigned char hdr[2];
    int masked, payload_len;
    unsigned char mask_key[4];

    if (!ssl_read_fully(ssl, hdr, 2)) return -1;

    *opcode = hdr[0] & 0x0F;
    masked = (hdr[1] & 0x80) != 0;
    payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        unsigned char ext[2];
        if (!ssl_read_fully(ssl, ext, 2)) return -1;
        payload_len = (ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        /* 64-bit payload length — reject anything beyond INT_MAX.
         * Blitzortung never sends frames this large, but we must
         * handle it safely to avoid truncation bugs (C2 fix). */
        unsigned char ext[8];
        uint64_t big_len;
        if (!ssl_read_fully(ssl, ext, 8)) return -1;
        big_len = ((uint64_t)ext[0] << 56) | ((uint64_t)ext[1] << 48) |
                  ((uint64_t)ext[2] << 40) | ((uint64_t)ext[3] << 32) |
                  ((uint64_t)ext[4] << 24) | ((uint64_t)ext[5] << 16) |
                  ((uint64_t)ext[6] <<  8) |  (uint64_t)ext[7];
        if (big_len > (uint64_t)INT_MAX) {
            printf("lightning: frame length too large, dropping\n");
            return -1;
        }
        payload_len = (int)big_len;
    }

    /* Frame too large for our buffer — drain it to stay in sync.
     * Read mask key separately before draining payload (I2 fix). */
    if (payload_len >= buf_size) {
        printf("lightning: frame too large (%d bytes), skipping\n",
               payload_len);
        if (masked) {
            unsigned char discard_mask[4];
            if (!ssl_read_fully(ssl, discard_mask, 4)) return -1;
        }
        {
            int skip = payload_len;
            unsigned char drain[256];
            while (skip > 0) {
                int chunk = skip > (int)sizeof(drain) ? (int)sizeof(drain) : skip;
                if (!ssl_read_fully(ssl, drain, chunk)) return -1;
                skip -= chunk;
            }
        }
        return 0;
    }

    if (masked) {
        if (!ssl_read_fully(ssl, mask_key, 4)) return -1;
    }

    if (payload_len > 0) {
        if (!ssl_read_fully(ssl, buf, payload_len)) return -1;

        if (masked) {
            int i;
            for (i = 0; i < payload_len; i++) {
                buf[i] ^= mask_key[i % 4];
            }
        }
    }

    buf[payload_len] = '\0';
    return payload_len;
}

/* ── Main thread ─────────────────────────────────────────────── */

/*
 * lightning_thread_func - Background thread for receiving strikes.
 *
 * Maintains a persistent WebSocket connection to Blitzortung.
 * On disconnect, reconnects with exponential backoff.
 */
static void *lightning_thread_func(void *arg)
{
    int backoff = 5;
    SSL_CTX *ctx;

    (void)arg;

    /* Initialise OpenSSL — once per thread lifetime */
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        fprintf(stderr, "lightning: SSL_CTX_new failed\n");
        return NULL;
    }
    /* Don't verify server certificate — we trust Blitzortung
     * and the Pi may not have up-to-date CA certificates. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    /* Wait for network connectivity before first connect attempt */
    if (!pic_wait_for_network("lightning", &lightning_running)) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    while (lightning_running) {
        int fd;
        SSL *ssl;

        printf("lightning: connecting to %s:%d (TLS)\n",
               BLITZ_HOST, BLITZ_PORT);
        fd = tcp_connect_blitz();
        if (fd < 0) {
            printf("lightning: TCP connect failed, retry in %ds\n", backoff);
            {
                int s;
                for (s = 0; s < backoff && lightning_running; s++)
                    sleep(1);
            }
            if (backoff < MAX_BACKOFF) backoff *= 2;
            continue;
        }

        /* Set socket read timeout — 60s without data = reconnect */
        {
            struct timeval tv;
            tv.tv_sec = 60;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        /* Wrap the socket in TLS */
        ssl = SSL_new(ctx);
        if (!ssl) {
            printf("lightning: SSL_new failed\n");
            close(fd);
            continue;
        }
        SSL_set_fd(ssl, fd);

        /* Set SNI hostname — required by many TLS servers */
        SSL_set_tlsext_host_name(ssl, BLITZ_HOST);

        if (SSL_connect(ssl) != 1) {
            printf("lightning: TLS handshake failed\n");
            SSL_free(ssl);
            close(fd);
            {
                int s;
                for (s = 0; s < backoff && lightning_running; s++)
                    sleep(1);
            }
            if (backoff < MAX_BACKOFF) backoff *= 2;
            continue;
        }

        printf("lightning: TLS connected, starting WebSocket handshake\n");

        /* Perform WebSocket handshake over TLS */
        if (!ws_handshake(ssl)) {
            printf("lightning: WS handshake failed, retry in %ds\n", backoff);
            SSL_shutdown(ssl);
            SSL_free(ssl);
            close(fd);
            {
                int s;
                for (s = 0; s < backoff && lightning_running; s++)
                    sleep(1);
            }
            if (backoff < MAX_BACKOFF) backoff *= 2;
            continue;
        }

        backoff = 5;

        /*
         * Subscribe to global strike data.
         * The Blitzortung WebSocket protocol requires a JSON subscription
         * message that specifies which region to receive strikes for.
         * Region 1 = Europe, region 5 = global. The "s" key enables
         * signal data.
         */
        if (ws_send_text(ssl, "{\"a\": 111}") < 0) {
            printf("lightning: failed to send subscription\n");
            SSL_shutdown(ssl);
            SSL_free(ssl);
            close(fd);
            continue;
        }
        printf("lightning: subscribed\n");

        /* Read WebSocket frames until disconnect */
        while (lightning_running) {
            unsigned char frame_buf[4096];
            int opcode;
            int len;

            len = ws_read_frame(ssl, frame_buf, sizeof(frame_buf) - 1,
                                &opcode);
            if (len < 0) {
                printf("lightning: connection lost (last opcode=0x%X)\n",
                       opcode);
                break;
            }

            switch (opcode) {
            case 0x1: {
                /* Text frame — LZW decode then parse strike JSON */
                char decoded[8192];
                double lat, lon;
                int dec_len;

                dec_len = lzw_decode(frame_buf, len,
                                     decoded, sizeof(decoded));
                if (dec_len > 0 &&
                    parse_strike_json(decoded, &lat, &lon)) {
                    add_strike(cfg_data, lat, lon);
                }
                break;
            }
            case 0x8:
                /* Close frame */
                printf("lightning: server sent close frame\n");
                len = -1;
                break;
            case 0x9:
                /* Ping — reply with pong */
                ws_send_pong(ssl, frame_buf, len);
                break;
            case 0xA:
                /* Pong — ignore */
                break;
            default:
                printf("lightning: unknown opcode 0x%X, len=%d\n",
                       opcode, len);
                break;
            }

            if (len < 0) break;
        }

        printf("lightning: disconnected\n");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);

        if (lightning_running) {
            int s;
            printf("lightning: reconnecting in %ds\n", backoff);
            for (s = 0; s < backoff && lightning_running; s++)
                sleep(1);
            if (backoff < MAX_BACKOFF) backoff *= 2;
        }
    }

    SSL_CTX_free(ctx);
    return NULL;
}

/*
 * pic_lightning_start - Start the Blitzortung background thread.
 */
int pic_lightning_start(pic_lightning_t *data)
{
    if (lightning_running) return 0;

    cfg_data = data;
    pic_lightning_reload(data);

    lightning_running = 1;

    if (pthread_create(&lightning_thread, NULL,
                       lightning_thread_func, NULL) != 0) {
        lightning_running = 0;
        fprintf(stderr, "lightning: failed to create thread\n");
        return -1;
    }

    printf("lightning: fetcher thread started (fade=%dms)\n",
           data->fade_ms);
    return 0;
}

/*
 * pic_lightning_stop - Stop the background thread cleanly.
 */
void pic_lightning_stop(void)
{
    if (!lightning_running) return;
    lightning_running = 0;
    pthread_join(lightning_thread, NULL);
    cfg_data = NULL;
    printf("lightning: fetcher thread stopped\n");
}

/*
 * pic_lightning_reload - Re-read fade time from config.
 * Clamped to 5000-120000ms (5s to 120s).
 */
void pic_lightning_reload(pic_lightning_t *data)
{
    FILE *f;
    char line[256];
    int new_fade = data->fade_ms;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "LIGHTNING_FADE_MS=", 18) == 0) {
                new_fade = atoi(line + 18);
            }
        }
        fclose(f);
    }

    if (new_fade < 5000) new_fade = 5000;
    if (new_fade > 120000) new_fade = 120000;

    pthread_mutex_lock(&data->mutex);
    data->fade_ms = new_fade;
    pthread_mutex_unlock(&data->mutex);

    printf("lightning: fade time = %dms\n", new_fade);
}
