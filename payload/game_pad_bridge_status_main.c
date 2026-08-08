/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only status snapshot for the persistent wireless DS4 game bridge.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/klog.h>
#include <ps5/payload.h>

#include "wireless_ds4.h"

#define SUPERVISOR_REPORT "/data/poords4/game-pad-bridge-supervisor.txt"
#define STATUS_REPORT     "/data/poords4/game-pad-bridge-status.txt"
#define STATUS_REPORT_TMP STATUS_REPORT ".tmp"

void
poords4_log_reset(void)
{
}

void
poords4_log(const char *format, ...)
{
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    klog_printf("%s", buffer);
}

static ssize_t
read_text(const char *path, char *buffer, size_t capacity)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t length = read(fd, buffer, capacity - 1u);
    close(fd);
    if (length < 0)
        return -1;
    buffer[length] = '\0';
    return length;
}

static int
parse_reader_identity(const char *buffer, pid_t *out_pid,
                      intptr_t *out_args)
{
    long pid_value = -1;
    unsigned long args_value = 0;
    const char *line = buffer;
    while (line && *line) {
        if (!strncmp(line, "reader_pid=", 11))
            pid_value = strtol(line + 11, NULL, 0);
        else if (!strncmp(line, "reader_args=", 12))
            args_value = strtoul(line + 12, NULL, 0);
        const char *next = strchr(line, '\n');
        line = next ? next + 1 : NULL;
    }
    if (pid_value <= 0 || args_value == 0)
        return -1;
    *out_pid = (pid_t)pid_value;
    *out_args = (intptr_t)args_value;
    return 0;
}

int
main(void)
{
    char supervisor[4096];
    ssize_t supervisor_length = read_text(
        SUPERVISOR_REPORT, supervisor, sizeof(supervisor));
    if (supervisor_length < 0)
        supervisor[0] = '\0';

    pid_t reader_pid = -1;
    intptr_t reader_args = 0;
    int identity_result = supervisor_length >= 0
        ? parse_reader_identity(supervisor, &reader_pid, &reader_args) : -1;
    PoorDS4RemoteReaderStatus reader;
    memset(&reader, 0, sizeof(reader));
    int reader_result = identity_result == 0
        ? wireless_ds4_remote_reader_status(
              reader_pid, reader_args, &reader) : -1;

    char report[6144];
    int length = snprintf(
        report, sizeof(report),
        "mode=read-only-game-bridge-status\n"
        "supervisor_read_result=%d\n"
        "reader_identity_result=%d\nreader_pid=%d\n"
        "reader_args=0x%lx\nreader_snapshot_result=%d\n"
        "reader_ready=%d\nreader_stop=%d\n"
        "reader_last_result=%d\nreader_seq=%u\n"
        "reader_pad_handle=0x%08x\nreader_owner_pid=%d\n"
        "reader_owner_check_interval=%u\n"
        "reader_owner_miss_count=%u\n"
        "reader_owner_watchdog_exits=%u\n"
        "reader_close_pad_on_exit=%d\nreader_connected=%u\n"
        "--- supervisor ---\n%s",
        (int)supervisor_length, identity_result, reader_pid,
        (unsigned long)reader_args, reader_result,
        reader.ready, reader.stop, reader.last_result, reader.seq,
        (uint32_t)reader.pad_handle, reader.owner_pid,
        reader.owner_check_interval, reader.owner_miss_count,
        reader.owner_watchdog_exits, reader.close_pad_on_exit,
        reader.connected, supervisor);

    int success = length > 0;
    size_t used = success && (size_t)length < sizeof(report)
        ? (size_t)length : sizeof(report) - 1u;
    size_t offset = 0;
    int fd = open(STATUS_REPORT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        success = 0;
    } else {
        while (success && offset < used) {
            ssize_t written = write(fd, report + offset, used - offset);
            if (written > 0) {
                offset += (size_t)written;
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            success = 0;
        }
        if (close(fd) != 0)
            success = 0;
    }
    if (success && rename(STATUS_REPORT_TMP, STATUS_REPORT) != 0)
        success = 0;
    if (!success)
        (void)unlink(STATUS_REPORT_TMP);
    payload_exit(success && supervisor_length >= 0 ? 0 : 1);
    return 0;
}
