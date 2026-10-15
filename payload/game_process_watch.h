/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <sys/event.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Kernel notifications distinguish exec from a still-live PID. This observes
 * the game only; it does not suspend it or borrow one of its threads. */
typedef struct {
    int fd, error;
    pid_t pid;
    uint32_t events;
} PoorDS4GameProcessWatch;

static void
poords4_game_process_watch_close(PoorDS4GameProcessWatch *watch)
{
    if (watch->fd >= 0) close(watch->fd);
    *watch = (PoorDS4GameProcessWatch){.fd = -1, .pid = -1};
}

static int
poords4_game_process_watch_open(PoorDS4GameProcessWatch *watch, pid_t pid)
{
    poords4_game_process_watch_close(watch);
    if (pid <= 0) return -1;
    int fd = kqueue();
    if (fd < 0) return -1;
    struct kevent change;
    EV_SET(&change, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_CLEAR,
           NOTE_EXEC | NOTE_EXIT, 0, NULL);
    if (kevent(fd, &change, 1, NULL, 0, NULL) < 0) {
        int saved_error = errno;
        close(fd);
        errno = saved_error;
        return -1;
    }
    watch->fd = fd;
    watch->pid = pid;
    return 0;
}

/* 0 unchanged/unavailable, 1 exec, 2 exited, -1 observation failed. Observed
 * events remain sticky until the old bridge is abandoned or removed. */
static int
poords4_game_process_watch_poll(PoorDS4GameProcessWatch *watch, pid_t pid)
{
    if (watch->pid != pid || watch->fd < 0) return 0;
    if (watch->error) return -1;
    if (!watch->events) {
        struct kevent event;
        const struct timespec immediate = {0, 0};
        memset(&event, 0, sizeof(event));
        int count = kevent(watch->fd, NULL, 0, &event, 1, &immediate);
        if (count < 0) {
            if (errno == EINTR) return 0;
            watch->error = errno ? errno : EIO;
            return -1;
        }
        if (count && (event.flags & EV_ERROR)) {
            watch->error = event.data ? (int)event.data : EIO;
            return -1;
        }
        if (count && event.filter == EVFILT_PROC &&
            event.ident == (uintptr_t)pid)
            watch->events |= event.fflags & (NOTE_EXEC | NOTE_EXIT);
    }
    if (watch->events & NOTE_EXIT) return 2;
    return (watch->events & NOTE_EXEC) ? 1 : 0;
}
