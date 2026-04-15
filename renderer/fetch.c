/*
 * fetch.c - Safe HTTP fetch helpers using fork/execvp
 *
 * All downloads go through wget called via execvp — no shell
 * interpretation of URLs or paths. This prevents command injection
 * even if a URL or filename contains shell metacharacters.
 *
 * A global User-Agent string is sent with all requests:
 *   "Pi-Clock v{version} for {callsign}"
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

/* Global User-Agent string — set once at startup */
static char g_useragent[128] = "Pi-Clock";

/*
 * pic_fetch_set_useragent - Set the User-Agent for all HTTP requests.
 */
void pic_fetch_set_useragent(const char *version, const char *callsign)
{
    if (callsign && callsign[0]) {
        snprintf(g_useragent, sizeof(g_useragent),
                 "Pi-Clock v%s for %s", version, callsign);
    } else {
        snprintf(g_useragent, sizeof(g_useragent),
                 "Pi-Clock v%s", version);
    }
    printf("fetch: User-Agent: %s\n", g_useragent);
}

/*
 * pic_fetch_get_useragent - Return the current User-Agent string.
 */
const char *pic_fetch_get_useragent(void)
{
    return g_useragent;
}

/*
 * pic_fetch_to_buf - Fetch a URL into a memory buffer via wget.
 *
 * Creates a pipe, forks a child that execs wget with stdout redirected
 * to the write end. The parent reads from the read end into buf.
 *
 * Returns the number of bytes read, or -1 on failure.
 */
int pic_fetch_to_buf(const char *url, char *buf, int buf_size)
{
    int pipefd[2];
    pid_t pid;
    int n;

    if (pipe(pipefd) < 0) return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe, exec wget */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Redirect stderr to /dev/null */
        freopen("/dev/null", "w", stderr);

        execlp("wget", "wget", "-q", "-O", "-",
               "--timeout=10", "-U", g_useragent, url, NULL);
        _exit(127);
    }

    /* Parent: read from pipe */
    close(pipefd[1]);

    n = 0;
    while (n < buf_size - 1) {
        int r = read(pipefd[0], buf + n, buf_size - 1 - n);
        if (r <= 0) break;
        n += r;
    }
    close(pipefd[0]);
    buf[n] = '\0';

    /* Wait for child with a timeout — a hung wget must not block
     * the calling thread indefinitely (prevents clean shutdown). */
    {
        int status = -1, waited = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && waited < 30) {
            sleep(1);
            waited++;
        }
        if (waited >= 30) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }

    return n > 0 ? n : -1;
}

/*
 * pic_fetch_to_file - Download a URL to a file via wget.
 *
 * Returns 0 on success (wget exit 0), -1 on any failure.
 */
int pic_fetch_to_file(const char *url, const char *path, int timeout)
{
    pid_t pid;
    int status = -1;
    char timeout_str[16];

    snprintf(timeout_str, sizeof(timeout_str), "--timeout=%d", timeout);

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Redirect stderr to /dev/null */
        freopen("/dev/null", "w", stderr);

        execlp("wget", "wget", "-q", "-O", path,
               timeout_str, "-U", g_useragent, url, NULL);
        _exit(127);
    }

    /* Wait with timeout — prevent hung wget from blocking the thread */
    {
        int waited = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && waited < 30) {
            sleep(1);
            waited++;
        }
        if (waited >= 30) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}
