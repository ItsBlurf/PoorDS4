/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef POORDS4_DATA_DIR
#define POORDS4_DATA_DIR "/data/poords4"
#endif
#define POORDS4_FIRST_FAILURE_FILE POORDS4_DATA_DIR "/game-pad-bridge-first-failure.txt"
#define POORDS4_VERBOSE_FILE POORDS4_DATA_DIR "/verbose"
#define POORDS4_FIRST_FAILURE_LIMIT 65536u

/* Keep one bounded failure until explicitly removed. Never seek the live
 * report, overwrite an earlier failure, follow links or alter install results. */
static inline int
poords4_capture_first_failure(int report_fd, int install_result)
{
    int saved_errno = errno;
    int captured = 0;
    struct stat info;
    if ((install_result != -1 && install_result != 0) || report_fd < 0 ||
        fstat(report_fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
        goto done;
    int output = open(POORDS4_FIRST_FAILURE_FILE,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (output < 0)
        goto done;
    size_t size = (uint64_t)info.st_size > POORDS4_FIRST_FAILURE_LIMIT
        ? POORDS4_FIRST_FAILURE_LIMIT : (size_t)info.st_size;
    size_t copied = 0;
    unsigned interrupts = 0;
    while (copied < size) {
        char chunk[1024];
        size_t amount = size - copied;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        ssize_t count = pread(report_fd, chunk, amount, (off_t)copied);
        if (count < 0 && errno == EINTR && interrupts++ < 8u) continue;
        if (count <= 0 || (size_t)count > amount) goto failed;
        size_t written = 0;
        while (written < (size_t)count) {
            ssize_t result = write(output, chunk + written, (size_t)count - written);
            if (result < 0 && errno == EINTR && interrupts++ < 8u) continue;
            if (result <= 0 || (size_t)result > (size_t)count - written) goto failed;
            written += (size_t)result;
        }
        copied += (size_t)count;
    }
    /* A footer distinguishes a complete bounded capture from an interrupted
     * write after power loss. This file is diagnostic, never configuration. */
    char footer[96];
    int length = snprintf(footer, sizeof(footer),
        "\nfirst_failure_capture_complete=1\nfirst_failure_truncated=%u\n",
        (uint64_t)info.st_size > size ? 1u : 0u);
    size_t written = 0;
    if (length <= 0 || (size_t)length >= sizeof(footer)) goto failed;
    while (written < (size_t)length) {
        ssize_t result = write(output, footer + written, (size_t)length - written);
        if (result < 0 && errno == EINTR && interrupts++ < 8u) continue;
        if (result <= 0 || (size_t)result > (size_t)length - written) goto failed;
        written += (size_t)result;
    }
    if (close(output) != 0) {
        (void)unlink(POORDS4_FIRST_FAILURE_FILE);
        goto done;
    }
    captured = 1;
    goto done;
failed:
    (void)close(output);
    (void)unlink(POORDS4_FIRST_FAILURE_FILE);
done:
    errno = saved_errno;
    return captured;
}

typedef struct {
    uint64_t next_poll_ms;
    unsigned enabled;
} PoorDS4DiagnosticLogging;

/* Supervisor-only, once per five seconds. Neither callback nor reader calls
 * this helper. A regular flag file enables extra cached slot summaries. */
static inline unsigned
poords4_diagnostic_logging(PoorDS4DiagnosticLogging *state, uint64_t now_ms)
{
    int saved_errno = errno;
    if (now_ms && now_ms >= state->next_poll_ms) {
        struct stat info;
        state->enabled = lstat(POORDS4_VERBOSE_FILE, &info) == 0 &&
                         S_ISREG(info.st_mode);
        state->next_poll_ms = now_ms > UINT64_MAX - 5000u
            ? UINT64_MAX : now_ms + 5000u;
    }
    errno = saved_errno;
    return state->enabled;
}
