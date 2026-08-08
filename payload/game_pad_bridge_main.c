/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wireless DS4 -> native PS5 game ScePadData bridge.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/klog.h>
#include <ps5/payload.h>

#include "pad_types.h"
#include "wireless_ds4.h"

#define POORDS4_DATA_DIR   "/data/poords4"
#define GAME_BRIDGE_STOP_FILE  POORDS4_DATA_DIR "/stop-game-pad-bridge"
#define GAME_BRIDGE_LOG_FILE   POORDS4_DATA_DIR "/game-pad-bridge.log"
#define GAME_BRIDGE_LOG_BACKUP POORDS4_DATA_DIR "/game-pad-bridge.log.1"
#define GAME_BRIDGE_LOCK_FILE  POORDS4_DATA_DIR "/game-pad-bridge-supervisor.lock"
#define GAME_BRIDGE_STATE_FILE POORDS4_DATA_DIR "/game-pad-bridge-supervisor.txt"
#define GAME_BRIDGE_LAUNCH_GRACE_MS UINT64_C(1500)
#define SOURCE_DISCONNECT_GRACE_MS  UINT64_C(8000)
#define SOURCE_REDISCOVERY_MS       UINT64_C(10000)
#define SOURCE_DISCOVERY_RETRY_US   5000000u
#ifndef GAME_BRIDGE_LOG_LIMIT
#define GAME_BRIDGE_LOG_LIMIT  (1024u * 1024u)
#endif

#ifndef POORDS4_AUTO_WATCH
#define POORDS4_AUTO_WATCH 0
#endif
#ifndef POORDS4_RC_VERSION
#define POORDS4_RC_VERSION 0
#endif

static time_t g_last_supervisor_write;
static int g_supervisor_lock_fd = -1;
static PoorDS4PadSource g_pad_source = {-1, -1, -1, 0};
static pid_t g_reader_pid = -1;
static intptr_t g_reader_args;
static size_t g_log_bytes;
static volatile sig_atomic_t g_shutdown_requested;
static volatile sig_atomic_t g_suspend_requested;
static volatile sig_atomic_t g_resume_gap_detected;
static volatile sig_atomic_t g_skip_remote_cleanup;
static volatile sig_atomic_t g_lifecycle_reason;
static intptr_t g_app_focus_flag = -1;
static intptr_t g_system_state_info_flag = -1;
static intptr_t g_system_state_status_flag = -1;
static int g_app_focus_open_result = INT32_MIN;
static int g_system_state_info_open_result = INT32_MIN;
static int g_system_state_status_open_result = INT32_MIN;
static int g_app_focus_poll_result = INT32_MIN;
static int g_system_state_info_poll_result = INT32_MIN;
static int g_system_state_status_poll_result = INT32_MIN;
static uint64_t g_app_focus_pattern;
static unsigned g_app_focus_pattern_known;
static uint64_t g_system_state_info_pattern;
static uint64_t g_system_state_status_pattern;
static unsigned g_system_state_info_state;
static unsigned g_system_state_info_state_known;
static unsigned g_system_state_status_shutdown;
static unsigned g_system_state_status_known;
static uint64_t g_last_lifecycle_ms;
static uint64_t g_last_lifecycle_wall_ms;
static uint64_t g_last_state_poll_ms;

void poords4_log(const char *format, ...);

#define POORDS4_EVENT_WAITMODE_OR 2u
#define SYSTEM_STATE_SHUTDOWN_ON_GOING 100u
#define SYSTEM_STATE_SUSPEND_ON_GOING  300u
#define SYSTEM_STATE_MAIN_ON_STANDBY   500u
#define SYSTEM_STATE_STATUS_SHELLUI_SHUTDOWN_IN_PROGRESS \
    UINT64_C(0x0000000000200000)

enum {
    LIFECYCLE_REASON_NONE = 0,
    LIFECYCLE_REASON_SIGNAL = 1,
    LIFECYCLE_REASON_STOP_FILE = 2,
    LIFECYCLE_REASON_STATE_INFO = 3,
    LIFECYCLE_REASON_STATE_STATUS = 4,
    LIFECYCLE_REASON_RESUME_GAP = 5
};

extern int sceKernelOpenEventFlag(intptr_t *event_flag, const char *name);
extern int sceKernelPollEventFlag(intptr_t event_flag, uint64_t bits,
                                  unsigned int wait_mode,
                                  uint64_t *result_pattern);
extern int sceKernelCloseEventFlag(intptr_t event_flag);

static uint64_t
monotonic_milliseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) +
           (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static uint64_t
wallclock_milliseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) +
           (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static void
shutdown_signal_handler(int signal_number)
{
    (void)signal_number;
    g_lifecycle_reason = LIFECYCLE_REASON_SIGNAL;
    g_shutdown_requested = 1;
}

static void
install_shutdown_signal_handlers(void)
{
    (void)signal(SIGTERM, shutdown_signal_handler);
    (void)signal(SIGINT, shutdown_signal_handler);
    (void)signal(SIGHUP, shutdown_signal_handler);
}

static void
open_system_state_monitor(void)
{
    intptr_t focus_flag = -1;
    g_app_focus_open_result = sceKernelOpenEventFlag(
        &focus_flag, "SceShellCoreUtilAppFocus");
    if (g_app_focus_open_result == 0) {
        g_app_focus_flag = focus_flag;
        poords4_log(
            "[PoorDS4] app-focus flag opened handle=0x%lx\n",
            (unsigned long)focus_flag);
    } else {
        poords4_log(
            "[PoorDS4] app-focus flag unavailable result=0x%08x\n",
            (uint32_t)g_app_focus_open_result);
    }

    intptr_t info_flag = -1;
    g_system_state_info_open_result = sceKernelOpenEventFlag(
        &info_flag, "SceSystemStateMgrInfo");
    if (g_system_state_info_open_result == 0) {
        g_system_state_info_flag = info_flag;
        poords4_log(
            "[PoorDS4] lifecycle info flag opened handle=0x%lx\n",
            (unsigned long)info_flag);
    } else {
        poords4_log(
            "[PoorDS4] lifecycle info flag unavailable result=0x%08x\n",
            (uint32_t)g_system_state_info_open_result);
    }

    intptr_t status_flag = -1;
    g_system_state_status_open_result = sceKernelOpenEventFlag(
        &status_flag, "SceSystemStateMgrStatus");
    if (g_system_state_status_open_result == 0) {
        g_system_state_status_flag = status_flag;
        poords4_log(
            "[PoorDS4] lifecycle status flag opened handle=0x%lx\n",
            (unsigned long)status_flag);
    } else {
        poords4_log(
            "[PoorDS4] lifecycle status flag unavailable result=0x%08x\n",
            (uint32_t)g_system_state_status_open_result);
    }

    if (g_app_focus_flag < 0 &&
        g_system_state_info_flag < 0 &&
        g_system_state_status_flag < 0) {
        poords4_log(
            "[PoorDS4] lifecycle event flags unavailable; "
            "using resume-gap failsafe\n");
    }
}

static void
poll_app_focus(void)
{
    if (g_app_focus_flag < 0)
        return;
    uint64_t pattern = 0;
    int result = sceKernelPollEventFlag(
        g_app_focus_flag, UINT64_MAX,
        POORDS4_EVENT_WAITMODE_OR, &pattern);
    if (result != g_app_focus_poll_result) {
        poords4_log(
            "[PoorDS4] app-focus poll result=0x%08x\n",
            (uint32_t)result);
        g_app_focus_poll_result = result;
    }
    if (result != 0)
        return;
    if (!g_app_focus_pattern_known || pattern != g_app_focus_pattern) {
        poords4_log(
            "[PoorDS4] app-focus pattern=0x%016llx "
            "app_id=0x%08x high=0x%08x\n",
            (unsigned long long)pattern, (uint32_t)pattern,
            (uint32_t)(pattern >> 32));
        g_app_focus_pattern = pattern;
        g_app_focus_pattern_known = 1;
    }
}

static void
request_suspend_stop(int reason, const char *source,
                     unsigned state, uint64_t pattern)
{
    if (g_shutdown_requested)
        return;
    g_lifecycle_reason = reason;
    g_suspend_requested = 1;
    g_skip_remote_cleanup = 1;
    g_shutdown_requested = 1;
    poords4_log(
        "[PoorDS4] lifecycle transition source=%s state=%u "
        "pattern=0x%016llx; quiescing game imports before sleep\n",
        source ? source : "unknown", state,
        (unsigned long long)pattern);
}

static void
poll_system_state_info(void)
{
    if (g_system_state_info_flag < 0)
        return;
    uint64_t pattern = 0;
    int result = sceKernelPollEventFlag(
        g_system_state_info_flag, UINT64_MAX,
        POORDS4_EVENT_WAITMODE_OR, &pattern);
    if (result != g_system_state_info_poll_result) {
        poords4_log(
            "[PoorDS4] lifecycle info poll result=0x%08x\n",
            (uint32_t)result);
        g_system_state_info_poll_result = result;
    }
    if (result != 0)
        return;

    unsigned state = (unsigned)(pattern & UINT64_C(0xffff));
    g_system_state_info_pattern = pattern;
    if (!g_system_state_info_state_known ||
        state != g_system_state_info_state) {
        poords4_log(
            "[PoorDS4] lifecycle info state=%u pattern=0x%016llx\n",
            state, (unsigned long long)pattern);
        g_system_state_info_state = state;
        g_system_state_info_state_known = 1;
    }
    if (state == SYSTEM_STATE_SHUTDOWN_ON_GOING ||
        state == SYSTEM_STATE_SUSPEND_ON_GOING ||
        state == SYSTEM_STATE_MAIN_ON_STANDBY) {
        request_suspend_stop(
            LIFECYCLE_REASON_STATE_INFO, "SceSystemStateMgrInfo",
            state, pattern);
    }
}

static void
poll_system_state_status(void)
{
    if (g_system_state_status_flag < 0)
        return;
    uint64_t pattern = 0;
    int result = sceKernelPollEventFlag(
        g_system_state_status_flag, UINT64_MAX,
        POORDS4_EVENT_WAITMODE_OR, &pattern);
    if (result != g_system_state_status_poll_result) {
        poords4_log(
            "[PoorDS4] lifecycle status poll result=0x%08x\n",
            (uint32_t)result);
        g_system_state_status_poll_result = result;
    }
    if (result != 0)
        return;

    unsigned shutdown_in_progress =
        (pattern & SYSTEM_STATE_STATUS_SHELLUI_SHUTDOWN_IN_PROGRESS) != 0;
    g_system_state_status_pattern = pattern;
    if (!g_system_state_status_known ||
        shutdown_in_progress != g_system_state_status_shutdown) {
        poords4_log(
            "[PoorDS4] lifecycle status shutdown=%u "
            "pattern=0x%016llx\n",
            shutdown_in_progress, (unsigned long long)pattern);
        g_system_state_status_shutdown = shutdown_in_progress;
        g_system_state_status_known = 1;
    }
    if (shutdown_in_progress) {
        request_suspend_stop(
            LIFECYCLE_REASON_STATE_STATUS,
            "SceSystemStateMgrStatus", 0, pattern);
    }
}

static int
lifecycle_should_stop(void)
{
    uint64_t now = monotonic_milliseconds();
    uint64_t wall_now = wallclock_milliseconds();
    uint64_t monotonic_gap = now && g_last_lifecycle_ms &&
        now > g_last_lifecycle_ms ? now - g_last_lifecycle_ms : 0;
    uint64_t wall_gap = wall_now && g_last_lifecycle_wall_ms &&
        wall_now > g_last_lifecycle_wall_ms
            ? wall_now - g_last_lifecycle_wall_ms : 0;
    uint64_t detected_gap = monotonic_gap > wall_gap
        ? monotonic_gap : wall_gap;
    if (detected_gap > UINT64_C(15000) &&
        !g_suspend_requested) {
        g_lifecycle_reason = LIFECYCLE_REASON_RESUME_GAP;
        g_resume_gap_detected = 1;
        g_skip_remote_cleanup = 1;
        g_shutdown_requested = 1;
        poords4_log(
            "[PoorDS4] lifecycle resume gap=%llu ms; "
            "quiescing game imports after resume boundary\n",
            (unsigned long long)detected_gap);
    }
    if (now != 0)
        g_last_lifecycle_ms = now;
    if (wall_now != 0)
        g_last_lifecycle_wall_ms = wall_now;

    if (!g_shutdown_requested &&
        (g_app_focus_flag >= 0 ||
         g_system_state_info_flag >= 0 ||
         g_system_state_status_flag >= 0) &&
        (g_last_state_poll_ms == 0 || now == 0 ||
         now >= g_last_state_poll_ms + UINT64_C(100))) {
        if (now != 0)
            g_last_state_poll_ms = now;
        poll_app_focus();
        poll_system_state_info();
        if (!g_shutdown_requested)
            poll_system_state_status();
    }
    if (!g_shutdown_requested &&
        access(GAME_BRIDGE_STOP_FILE, F_OK) == 0) {
        g_lifecycle_reason = LIFECYCLE_REASON_STOP_FILE;
        g_shutdown_requested = 1;
    }
    return g_shutdown_requested != 0;
}

static int
sleep_interruptible(unsigned microseconds)
{
    while (microseconds != 0) {
        if (lifecycle_should_stop())
            return -1;
        unsigned slice = microseconds > 100000u ? 100000u : microseconds;
        usleep(slice);
        microseconds -= slice;
    }
    return lifecycle_should_stop() ? -1 : 0;
}

static int
process_alive(pid_t pid)
{
    errno = 0;
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static int
teardown_game_bridge(pid_t game_pid, intptr_t bridge_args,
                     int allow_target_syscalls, const char **out_mode)
{
    if (!process_alive(game_pid)) {
        if (out_mode) *out_mode = "abandon-dead";
        return wireless_ds4_game_bridge_abandon();
    }
    if (allow_target_syscalls) {
        if (out_mode) *out_mode = "remove";
        return wireless_ds4_game_bridge_remove(game_pid, bridge_args);
    }
    int quiesce_result = wireless_ds4_game_bridge_quiesce(
        game_pid, bridge_args);
    if (quiesce_result == 0) {
        if (out_mode) *out_mode = "quiesce";
        return 0;
    }
    if (out_mode) *out_mode = "abandon-unverified";
    (void)wireless_ds4_game_bridge_abandon();
    return quiesce_result;
}

static void
write_supervisor_state(const char *state, pid_t game_pid,
                       unsigned sessions,
                       unsigned long long output_frames, int force)
{
    time_t now = time(NULL);
    if (!force && g_last_supervisor_write != 0 &&
        now - g_last_supervisor_write < 5)
        return;

    char report[1024];
    int length = snprintf(
        report, sizeof(report),
        "pid=%d\nheartbeat_epoch=%lld\nrc_version=%d\n"
        "auto_watch=%d\nstate=%s\n"
        "game_pid=%d\nsessions=%u\noutput_frames=%llu\n"
        "reader_pid=%d\nreader_args=0x%lx\n"
        "source_user=0x%08x\nsource_index=%d\n"
        "source_handle=0x%08x\nsource_is_ds4=0x%08x\n"
        "source_disconnect_grace_ms=%llu\n"
        "source_rediscovery_ms=%llu\n"
        "lifecycle_reason=%d\nlifecycle_suspend=%d\n"
        "lifecycle_skip_remote_cleanup=%d\n"
        "app_focus_open_result=0x%08x\n"
        "app_focus_poll_result=0x%08x\n"
        "app_focus_pattern=0x%016llx\n"
        "lifecycle_info_open_result=0x%08x\n"
        "lifecycle_info_poll_result=0x%08x\n"
        "lifecycle_info_pattern=0x%016llx\n"
        "lifecycle_info_state=%u\n"
        "lifecycle_status_open_result=0x%08x\n"
        "lifecycle_status_poll_result=0x%08x\n"
        "lifecycle_status_pattern=0x%016llx\n"
        "lifecycle_status_shutdown=%u\n",
        getpid(), (long long)now, POORDS4_RC_VERSION,
        POORDS4_AUTO_WATCH,
        state ? state : "unknown", game_pid, sessions, output_frames,
        g_reader_pid, (unsigned long)g_reader_args,
        (uint32_t)g_pad_source.user_id, g_pad_source.pad_index,
        (uint32_t)g_pad_source.pad_handle,
        (uint32_t)g_pad_source.ds4_connected,
        (unsigned long long)SOURCE_DISCONNECT_GRACE_MS,
        (unsigned long long)SOURCE_REDISCOVERY_MS,
        (int)g_lifecycle_reason, (int)g_suspend_requested,
        (int)g_skip_remote_cleanup,
        (uint32_t)g_app_focus_open_result,
        (uint32_t)g_app_focus_poll_result,
        (unsigned long long)g_app_focus_pattern,
        (uint32_t)g_system_state_info_open_result,
        (uint32_t)g_system_state_info_poll_result,
        (unsigned long long)g_system_state_info_pattern,
        g_system_state_info_state,
        (uint32_t)g_system_state_status_open_result,
        (uint32_t)g_system_state_status_poll_result,
        (unsigned long long)g_system_state_status_pattern,
        g_system_state_status_shutdown);
    if (length <= 0)
        return;
    size_t write_length = (size_t)length;
    if (write_length >= sizeof(report))
        write_length = sizeof(report) - 1u;

    int state_fd = open(
        GAME_BRIDGE_STATE_FILE,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (state_fd >= 0) {
        (void)write(state_fd, report, write_length);
        close(state_fd);
    }

    char lock[128];
    int lock_length = snprintf(
        lock, sizeof(lock), "pid=%d\nheartbeat_epoch=%lld\n",
        getpid(), (long long)now);
    if (g_supervisor_lock_fd >= 0) {
        (void)ftruncate(g_supervisor_lock_fd, 0);
        (void)lseek(g_supervisor_lock_fd, 0, SEEK_SET);
        if (lock_length > 0)
            (void)write(
                g_supervisor_lock_fd, lock, (size_t)lock_length);
    }
    g_last_supervisor_write = now;
}

static int
acquire_supervisor_lock(void)
{
    int fd = open(
        GAME_BRIDGE_LOCK_FILE,
        O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_errno = errno;
        close(fd);
        return lock_errno == EWOULDBLOCK || lock_errno == EAGAIN
            ? 0 : -1;
    }
    g_supervisor_lock_fd = fd;
    return 1;
}

extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *out_user_id);
extern int32_t sceUserServiceGetForegroundUser(int32_t *out_user_id);
extern int32_t sceUserServiceGetLoginUserIdList(int32_t out_user_ids[4]);
extern int32_t sceKernelSendNotificationRequest(
    int unk0, void *request, size_t size, int unk1);

typedef struct {
    char unknown[45];
    char message[3075];
} GameBridgeNotifyRequest;

void
poords4_log_reset(void)
{
    mkdir(POORDS4_DATA_DIR, 0755);
    int fd = open(
        GAME_BRIDGE_LOG_FILE,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
        close(fd);
    g_log_bytes = 0;
}

void
poords4_log(const char *format, ...)
{
    char buffer[768];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    size_t write_length = (size_t)length;
    if (write_length >= sizeof(buffer))
        write_length = sizeof(buffer) - 1;
    klog_printf("%s", buffer);
    if (g_log_bytes + write_length > GAME_BRIDGE_LOG_LIMIT) {
        (void)unlink(GAME_BRIDGE_LOG_BACKUP);
        if (rename(GAME_BRIDGE_LOG_FILE, GAME_BRIDGE_LOG_BACKUP) != 0) {
            int truncate_fd = open(
                GAME_BRIDGE_LOG_FILE,
                O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (truncate_fd >= 0)
                close(truncate_fd);
        }
        g_log_bytes = 0;
    }
    int fd = open(
        GAME_BRIDGE_LOG_FILE,
        O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        ssize_t written = write(fd, buffer, write_length);
        if (written > 0)
            g_log_bytes += (size_t)written;
        close(fd);
    }
}

static void
game_bridge_notify(const char *message)
{
    GameBridgeNotifyRequest request;
    memset(&request, 0, sizeof(request));
    if (message)
        snprintf(request.message, sizeof(request.message), "%s", message);
    int32_t result = sceKernelSendNotificationRequest(
        0, &request, sizeof(request), 0);
    poords4_log(
        "[PoorDS4] notification result=0x%08x message=%s\n",
        (uint32_t)result, message ? message : "");
}

typedef enum {
    SESSION_END_STOP_REQUESTED = 0,
    SESSION_END_GAME_EXITED = 1,
    SESSION_END_READER_FAILED = 2,
    SESSION_END_WRITER_FAILED = 3,
    SESSION_END_BRIDGE_HEALTH_FAILED = 4,
    SESSION_END_LIFECYCLE = 5,
    SESSION_END_CLEANUP_FAILED = 6
} GameSessionEndReason;

static const char *
game_session_end_reason_name(GameSessionEndReason reason)
{
    switch (reason) {
    case SESSION_END_STOP_REQUESTED:
        return "stop_requested";
    case SESSION_END_GAME_EXITED:
        return "game_exited";
    case SESSION_END_READER_FAILED:
        return "reader_failed";
    case SESSION_END_WRITER_FAILED:
        return "writer_failed";
    case SESSION_END_BRIDGE_HEALTH_FAILED:
        return "bridge_health_failed";
    case SESSION_END_LIFECYCLE:
        return "lifecycle";
    case SESSION_END_CLEANUP_FAILED:
        return "cleanup_failed";
    }
    return "unknown";
}

static void
make_neutral_connected_pad(ScePadData *pad)
{
    uint64_t timestamp = pad->timestamp;
    uint8_t count = pad->count;
    memset(pad, 0, sizeof(*pad));
    pad->leftStick.x = 128;
    pad->leftStick.y = 128;
    pad->rightStick.x = 128;
    pad->rightStick.y = 128;
    pad->quat.w = 1.0f;
    pad->connected = 1;
    pad->timestamp = timestamp;
    pad->count = count;
}

static unsigned
run_game_session(pid_t reader_pid, intptr_t reader_args,
                 pid_t game_pid, intptr_t bridge_args,
                 unsigned session,
                 unsigned long long previous_output_frames,
                 GameSessionEndReason *out_end_reason)
{
    unsigned input_frames = 0;
    unsigned output_frames = 0;
    unsigned stale_frames = 0;
    unsigned read_failures = 0;
    unsigned write_failures = 0;
    uint32_t last_seq = 0;
    unsigned consecutive_read_failures = 0;
    unsigned consecutive_write_failures = 0;
    unsigned consecutive_bridge_health_failures = 0;
    unsigned loop_count = 0;
    unsigned last_health_frame = 0;
    uint64_t disconnect_started_ms = 0;
    unsigned disconnect_frames = 0;
    unsigned disconnect_grace_events = 0;
    unsigned disconnect_grace_expired = 0;
    PoorDS4GameBridgeStatus last_bridge_status;
    memset(&last_bridge_status, 0, sizeof(last_bridge_status));
    int game_alive = 1;
    GameSessionEndReason end_reason = SESSION_END_STOP_REQUESTED;
    write_supervisor_state(
        "active", game_pid, session, previous_output_frames, 1);
    while (!lifecycle_should_stop()) {
        ScePadData pad;
        uint32_t seq = 0;
        memset(&pad, 0, sizeof(pad));
        if (wireless_ds4_remote_reader_read(
                reader_pid, reader_args, &pad, sizeof(pad), &seq) == 0) {
            consecutive_read_failures = 0;
            if (seq != last_seq) {
                input_frames++;
                if (!pad.connected) {
                    uint64_t now = monotonic_milliseconds();
                    if (disconnect_started_ms == 0) {
                        disconnect_started_ms = now ? now : 1;
                        disconnect_frames = 0;
                        disconnect_grace_expired = 0;
                        disconnect_grace_events++;
                        poords4_log(
                            "[PoorDS4] source disconnect grace begin "
                            "event=%u limit_ms=%llu\n",
                            disconnect_grace_events,
                            (unsigned long long)
                                SOURCE_DISCONNECT_GRACE_MS);
                    }
                    disconnect_frames++;
                    uint64_t disconnect_elapsed_ms =
                        now && disconnect_started_ms > 1
                            ? now - disconnect_started_ms : 0;
                    /* Never expose the transient physical disconnect to the
                     * game. A neutral connected frame keeps the same player
                     * slot alive while we decide whether rediscovery is
                     * necessary. */
                    make_neutral_connected_pad(&pad);
                    if (now && disconnect_started_ms > 1 &&
                        disconnect_elapsed_ms >= SOURCE_DISCONNECT_GRACE_MS &&
                        !disconnect_grace_expired) {
                        poords4_log(
                            "[PoorDS4] source disconnect grace elapsed "
                            "after=%llu ms frames=%u\n",
                            (unsigned long long)disconnect_elapsed_ms,
                            disconnect_frames);
                        disconnect_grace_expired = 1;
                    }
                    if (now && disconnect_started_ms > 1 &&
                        disconnect_elapsed_ms >= SOURCE_REDISCOVERY_MS) {
                        poords4_log(
                            "[PoorDS4] source rediscovery requested "
                            "after=%llu ms frames=%u\n",
                            (unsigned long long)disconnect_elapsed_ms,
                            disconnect_frames);
                        end_reason = SESSION_END_READER_FAILED;
                        break;
                    }
                } else if (disconnect_started_ms != 0) {
                    uint64_t now = monotonic_milliseconds();
                    poords4_log(
                        "[PoorDS4] source reconnected after=%llu ms "
                        "frames=%u grace_expired=%u\n",
                        (unsigned long long)(
                            now && disconnect_started_ms > 1
                                ? now - disconnect_started_ms : 0),
                        disconnect_frames, disconnect_grace_expired);
                    disconnect_started_ms = 0;
                    disconnect_frames = 0;
                    disconnect_grace_expired = 0;
                }
                if (wireless_ds4_game_bridge_update(
                        game_pid, bridge_args,
                        &pad, sizeof(pad)) == 0) {
                    output_frames++;
                    consecutive_write_failures = 0;
                } else {
                    write_failures++;
                    consecutive_write_failures++;
                }
                last_seq = seq;
            } else {
                stale_frames++;
            }
        } else {
            read_failures++;
            consecutive_read_failures++;
        }

        if (input_frames != 0 && (input_frames % 600u) == 0 &&
            input_frames != last_health_frame) {
            poords4_log(
                "[PoorDS4] BRIDGE HEALTH in=%u out=%u seq=%u conn=%u "
                "buttons=0x%08x stale=%u read_fail=%u write_fail=%u "
                "bridge_health_fail=%u bridge_ready=%d active=%u "
                "packets=%llu lease_expirations=%llu pad_index=%d "
                "contention=%llu info=%llu/%llu "
                "info_result_overrides=%llu native_backing=%llu/%llu "
                "imports=%u\n",
                input_frames, output_frames, last_seq, pad.connected,
                pad.buttons, stale_frames, read_failures, write_failures,
                consecutive_bridge_health_failures,
                last_bridge_status.bridge_ready,
                last_bridge_status.active,
                (unsigned long long)last_bridge_status.published_packets,
                (unsigned long long)
                    last_bridge_status.lease_expirations,
                last_bridge_status.game_pad_index,
                (unsigned long long)
                    last_bridge_status.snapshot_contention_fallbacks,
                (unsigned long long)
                    last_bridge_status.controller_info_spoofs,
                (unsigned long long)
                    last_bridge_status.controller_info_calls,
                (unsigned long long)
                    last_bridge_status.controller_info_result_overrides,
                (unsigned long long)
                    last_bridge_status.native_backing_calls,
                (unsigned long long)
                    last_bridge_status.native_backing_errors,
                last_bridge_status.import_hook_count);
            write_supervisor_state(
                "active", game_pid, session,
                previous_output_frames + output_frames, 0);
            last_health_frame = input_frames;
        }
        loop_count++;
        if ((loop_count % 120u) == 0) {
            errno = 0;
            game_alive = kill(game_pid, 0) == 0 || errno == EPERM;
            /* Never copy from a PID once kill(2) says it is gone. RC22 did
             * exactly that, then tried ptrace cleanup on the dead game. */
            if (game_alive) {
                PoorDS4GameBridgeStatus bridge_status;
                memset(&bridge_status, 0, sizeof(bridge_status));
                if (wireless_ds4_game_bridge_status(
                        game_pid, bridge_args, &bridge_status) == 0 &&
                    bridge_status.bridge_ready == 1) {
                    last_bridge_status = bridge_status;
                    consecutive_bridge_health_failures = 0;
                } else {
                    consecutive_bridge_health_failures++;
                }
            }
        }
        if (consecutive_read_failures >= 240u ||
            consecutive_write_failures >= 1200u ||
            consecutive_bridge_health_failures >= 5u ||
            !game_alive) {
            poords4_log(
                "[PoorDS4] bridge stopping read_streak=%u "
                "write_streak=%u bridge_health_streak=%u "
                "game_alive=%d game_pid=%d\n",
                consecutive_read_failures,
                consecutive_write_failures,
                consecutive_bridge_health_failures,
                game_alive, game_pid);
            if (!game_alive)
                end_reason = SESSION_END_GAME_EXITED;
            else if (consecutive_read_failures >= 240u)
                end_reason = SESSION_END_READER_FAILED;
            else if (consecutive_bridge_health_failures >= 5u)
                end_reason = SESSION_END_BRIDGE_HEALTH_FAILED;
            else
                end_reason = SESSION_END_WRITER_FAILED;
            break;
        }
        if (sleep_interruptible(8333) != 0) {
            end_reason = SESSION_END_LIFECYCLE;
            break;
        }
    }
    if (g_shutdown_requested && end_reason == SESSION_END_STOP_REQUESTED)
        end_reason = SESSION_END_LIFECYCLE;
    int keep_bridge_for_reader_recovery =
        end_reason == SESSION_END_READER_FAILED &&
        !g_shutdown_requested && !g_skip_remote_cleanup &&
        process_alive(game_pid);
    int remove_result = 0;
    if (keep_bridge_for_reader_recovery) {
        ScePadData neutral;
        memset(&neutral, 0, sizeof(neutral));
        neutral.leftStick.x = 128;
        neutral.leftStick.y = 128;
        neutral.rightStick.x = 128;
        neutral.rightStick.y = 128;
        neutral.quat.w = 1.0f;
        neutral.connected = 1;
        (void)wireless_ds4_game_bridge_update(
            game_pid, bridge_args, &neutral, sizeof(neutral));
        poords4_log(
            "[PoorDS4] game bridge retained for in-place reader "
            "recovery pid=%d\n", game_pid);
    } else {
        int safe_remote_cleanup = !g_skip_remote_cleanup;
        const char *cleanup_mode = NULL;
        remove_result = teardown_game_bridge(
            game_pid, bridge_args, safe_remote_cleanup,
            &cleanup_mode);
        poords4_log(
            "[PoorDS4] game bridge cleanup=%s result=%d pid=%d\n",
            cleanup_mode ? cleanup_mode : "unknown",
            remove_result, game_pid);
        if (process_alive(game_pid) && remove_result != 0)
            end_reason = SESSION_END_CLEANUP_FAILED;
    }
    poords4_log(
        "[PoorDS4] game session end pid=%d reason=%d "
        "reason_name=%s in=%u out=%u\n",
        game_pid, end_reason, game_session_end_reason_name(end_reason),
        input_frames, output_frames);
    if (out_end_reason)
        *out_end_reason = end_reason;
    return output_frames;
}

static int
wait_for_game_bridge_ready(pid_t game_pid, intptr_t bridge_args)
{
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (lifecycle_should_stop() || !process_alive(game_pid))
            return -2;
        PoorDS4GameBridgeStatus status;
        memset(&status, 0, sizeof(status));
        if (wireless_ds4_game_bridge_status(
                game_pid, bridge_args, &status) == 0) {
            if (status.bridge_ready == 1)
                return 0;
            if (status.bridge_ready == 2 || status.bridge_ready < 0)
                return -1;
        }
        if (sleep_interruptible(10000) != 0)
            return -2;
    }
    return -1;
}

static void
wait_for_game_exit_or_stop(pid_t game_pid, const char *state,
                           unsigned sessions,
                           unsigned long long output_frames)
{
    while (!lifecycle_should_stop() && process_alive(game_pid)) {
        write_supervisor_state(
            state, game_pid, sessions, output_frames, 0);
        if (sleep_interruptible(500000) != 0)
            break;
    }
}

static void
add_user_candidate(int32_t user_id, int32_t *user_ids, uint32_t *count)
{
    if (user_id < 0 || !user_ids || !count ||
        *count >= POORDS4_MAX_USER_CANDIDATES)
        return;
    for (uint32_t index = 0; index < *count; ++index) {
        if (user_ids[index] == user_id)
            return;
    }
    user_ids[(*count)++] = user_id;
}

static uint32_t
collect_user_candidates(int32_t user_ids[POORDS4_MAX_USER_CANDIDATES])
{
    int32_t initial_user = -1;
    int32_t foreground_user = -1;
    int32_t login_users[4] = {-1, -1, -1, -1};
    uint32_t count = 0;
    (void)sceUserServiceGetForegroundUser(&foreground_user);
    (void)sceUserServiceGetInitialUser(&initial_user);
    (void)sceUserServiceGetLoginUserIdList(login_users);
    add_user_candidate(foreground_user, user_ids, &count);
    for (unsigned index = 0; index < 4; ++index)
        add_user_candidate(login_users[index], user_ids, &count);
    add_user_candidate(initial_user, user_ids, &count);
    return count;
}

static int
start_wireless_reader(PoorDS4PadSource *source, pid_t *reader_pid,
                      intptr_t *reader_args, unsigned sessions,
                      unsigned long long output_frames)
{
    unsigned retry_count = 0;
    pid_t observed_game_pid = -1;
    for (;;) {
        if (lifecycle_should_stop())
            return -1;
        int32_t user_ids[POORDS4_MAX_USER_CANDIDATES];
        memset(user_ids, 0xff, sizeof(user_ids));
        uint32_t user_count = collect_user_candidates(user_ids);
        if (wireless_ds4_remote_reader_start(
                user_ids, user_count, source,
                reader_pid, reader_args) == 0) {
            g_reader_pid = *reader_pid;
            g_reader_args = *reader_args;
            g_pad_source = *source;
            poords4_log(
                "[PoorDS4] DS4 source user=0x%08x index=%d "
                "handle=0x%08x is_ds4=0x%08x\n",
                (uint32_t)source->user_id, source->pad_index,
                (uint32_t)source->pad_handle,
                (uint32_t)source->ds4_connected);
            return 0;
        }
        if (!POORDS4_AUTO_WATCH || lifecycle_should_stop())
            return -1;
        /* Keep the supervisor status tied to the currently visible game while
         * waiting for the DS4, without inspecting or modifying that game. */
        if (user_count > 0 &&
            (retry_count == 0 || (retry_count % 12u) == 0)) {
            pid_t probe_game_pid = -1;
            int probe_result = wireless_ds4_game_bridge_find_target(
                &probe_game_pid);
            observed_game_pid = probe_game_pid;
            poords4_log(
                "[PoorDS4] game visibility while waiting pid=%d "
                "result=%d\n",
                probe_game_pid, probe_result);
        }
        retry_count++;
        if (retry_count == 1 || (retry_count % 12u) == 0)
            poords4_log(
                "[PoorDS4] wireless reader unavailable; retry=%u\n",
                retry_count);
        write_supervisor_state(
            "waiting_for_wireless_controller", observed_game_pid, sessions,
            output_frames, 1);
        if (sleep_interruptible(SOURCE_DISCOVERY_RETRY_US) != 0)
            return -1;
    }
}

/* Keep the already-installed game hooks fed with a connected neutral frame
 * while the physical DS4 is re-published by Bluetooth/user services. This
 * avoids the old remove/reinstall cycle that appeared to games as a quick
 * disconnect and could race their launch/teardown paths. */
static int
recover_wireless_reader_in_place(
    PoorDS4PadSource *source, pid_t *reader_pid,
    intptr_t *reader_args, pid_t game_pid, intptr_t bridge_args,
    unsigned sessions, unsigned long long output_frames)
{
    uint64_t last_attempt_ms = 0;
    uint64_t last_status_ms = 0;
    unsigned attempts = 0;
    ScePadData neutral;
    memset(&neutral, 0, sizeof(neutral));
    make_neutral_connected_pad(&neutral);

    for (;;) {
        if (lifecycle_should_stop())
            return -3;
        if (!process_alive(game_pid))
            return -2;
        if (wireless_ds4_game_bridge_update(
                game_pid, bridge_args, &neutral, sizeof(neutral)) != 0)
            return -1;

        uint64_t now = monotonic_milliseconds();
        if (last_status_ms == 0 || now == 0 ||
            now >= last_status_ms + UINT64_C(1000)) {
            PoorDS4GameBridgeStatus status;
            memset(&status, 0, sizeof(status));
            if (wireless_ds4_game_bridge_status(
                    game_pid, bridge_args, &status) != 0 ||
                status.bridge_ready != 1 || !status.active)
                return -1;
            last_status_ms = now;
        }

        if (last_attempt_ms == 0 || now == 0 ||
            now >= last_attempt_ms + UINT64_C(2000)) {
            int32_t users[POORDS4_MAX_USER_CANDIDATES];
            memset(users, 0xff, sizeof(users));
            uint32_t user_count = collect_user_candidates(users);
            PoorDS4PadSource candidate = {-1, -1, -1, 0};
            pid_t candidate_pid = -1;
            intptr_t candidate_args = 0;
            attempts++;
            if (wireless_ds4_remote_reader_start(
                    users, user_count, &candidate,
                    &candidate_pid, &candidate_args) == 0) {
                *source = candidate;
                *reader_pid = candidate_pid;
                *reader_args = candidate_args;
                g_reader_pid = candidate_pid;
                g_reader_args = candidate_args;
                g_pad_source = candidate;
                poords4_log(
                    "[PoorDS4] in-place reader recovery complete "
                    "attempts=%u user=0x%08x index=%d\n",
                    attempts, (uint32_t)candidate.user_id,
                    candidate.pad_index);
                return 0;
            }
            last_attempt_ms = now;
            if (attempts == 1 || (attempts % 10u) == 0)
                poords4_log(
                    "[PoorDS4] waiting for DS4 re-publication "
                    "attempt=%u\n", attempts);
        }
        write_supervisor_state(
            "recovering_wireless_controller", game_pid,
            sessions, output_frames, 0);
        if (sleep_interruptible(100000) != 0)
            return -3;
    }
}

int
main(void)
{
    _Static_assert(sizeof(ScePadData) == 120,
                   "supported ScePadData must be 120 bytes");
    pid_t reader_pid = -1;
    intptr_t reader_args = 0;
    unsigned sessions = 0;
    unsigned long long total_output_frames = 0;
    pid_t retry_pid = -1;
    unsigned install_retries = 0;
    int reader_restart_required = 0;
    int exit_code = 1;
    /* The singleton lock lives below this directory.  Create it before the
     * first lock attempt so a clean console can produce both logs and a lock;
     * previously a missing directory made Start exit before observability was
     * initialized. */
    (void)mkdir(POORDS4_DATA_DIR, 0755);
    int lock_result = acquire_supervisor_lock();
    if (lock_result == 0) {
        game_bridge_notify("PoorDS4: wireless DS4 bridge is already running");
        payload_exit(0);
        return 0;
    }
    if (lock_result < 0) {
        game_bridge_notify("PoorDS4: bridge could not acquire its lock");
        payload_exit(1);
        return 0;
    }

    poords4_log_reset();
    (void)unlink(GAME_BRIDGE_STOP_FILE);
    install_shutdown_signal_handlers();
    g_last_lifecycle_ms = monotonic_milliseconds();
    g_last_lifecycle_wall_ms = wallclock_milliseconds();
    open_system_state_monitor();
    write_supervisor_state("starting", -1, 0, 0, 1);
    (void)sceUserServiceInitialize(NULL);
    poords4_log(
        "[PoorDS4] game bridge start rc=%d auto_watch=%d\n",
        POORDS4_RC_VERSION, POORDS4_AUTO_WATCH);
    game_bridge_notify(
        "PoorDS4: wireless DS4 bridge injected; starting reader");

    if (start_wireless_reader(
            &g_pad_source, &reader_pid, &reader_args,
            sessions, total_output_frames) != 0) {
        poords4_log("[PoorDS4] wireless reader start failed\n");
        goto cleanup;
    }
    /* Reaching a validated DS4 reader is a successful payload startup even
     * when the user stops it before launching a game. */
    exit_code = 0;
    write_supervisor_state("waiting_for_game", -1, 0, 0, 1);
    game_bridge_notify(
        "PoorDS4: wireless DS4 ready; waiting for a PS5 game");

    pid_t launch_candidate_pid = -1;
    uint64_t launch_candidate_since_ms = 0;
    for (;;) {
        if (lifecycle_should_stop())
            break;

        /* Never modify a newly launched game while the source reader is
         * absent. A reader can be lost during Bluetooth re-publication at the
         * same time that the old game exits. RC34 then carried its stale
         * source description into the next game, installed a bridge with
         * reader_pid=-1, and recovered only after an avoidable zero-frame
         * session. Restore a freshly validated reader first; the discovery
         * loop observes the visible game read-only while it waits. */
        PoorDS4RemoteReaderStatus reader_preflight;
        memset(&reader_preflight, 0, sizeof(reader_preflight));
        int reader_preflight_result =
            reader_pid > 0 && reader_args != 0 &&
            process_alive(reader_pid)
                ? wireless_ds4_remote_reader_status(
                      reader_pid, reader_args, &reader_preflight)
                : -1;
        int reader_ready = reader_preflight_result == 0 &&
            reader_preflight.ready == 1 &&
            reader_preflight.stop == 0 &&
            reader_preflight.last_result == 0 &&
            reader_preflight.connected != 0;
        if (reader_restart_required || !reader_ready) {
            poords4_log(
                "[PoorDS4] restoring wireless reader before game scan "
                "pid=%d args=0x%lx snapshot=%d ready=%d stop=%d "
                "result=0x%08x connected=%u\n",
                reader_pid, (unsigned long)reader_args,
                reader_preflight_result, reader_preflight.ready,
                reader_preflight.stop,
                (uint32_t)reader_preflight.last_result,
                (unsigned)reader_preflight.connected);
            if (reader_pid > 0 && reader_args != 0 &&
                process_alive(reader_pid)) {
                int stop_result = wireless_ds4_remote_reader_stop(
                    reader_pid, reader_args);
                poords4_log(
                    "[PoorDS4] pre-game reader stop=%d\n",
                    stop_result);
                if (stop_result != 0) {
                    write_supervisor_state(
                        "waiting_reader_stop", -1, sessions,
                        total_output_frames, 1);
                    if (sleep_interruptible(1000000) != 0)
                        break;
                    continue;
                }
            }
            reader_pid = -1;
            reader_args = 0;
            g_reader_pid = -1;
            g_reader_args = 0;
            g_pad_source = (PoorDS4PadSource){-1, -1, -1, 0};
            PoorDS4PadSource recovered_source = {-1, -1, -1, 0};
            if (start_wireless_reader(
                    &recovered_source, &reader_pid, &reader_args,
                    sessions, total_output_frames) != 0) {
                poords4_log(
                    "[PoorDS4] wireless reader restore failed\n");
                break;
            }
            reader_restart_required = 0;
            launch_candidate_pid = -1;
            launch_candidate_since_ms = 0;
            retry_pid = -1;
            install_retries = 0;
            game_bridge_notify(
                "PoorDS4: wireless DS4 restored; waiting for a PS5 game");
            continue;
        }

        pid_t observed_game_pid = -1;
        int find_result = wireless_ds4_game_bridge_find_target(
            &observed_game_pid);
        if (find_result != 1) {
            launch_candidate_pid = -1;
            launch_candidate_since_ms = 0;
            write_supervisor_state(
                find_result == -3
                    ? "waiting_for_unique_game"
                    : (find_result == -4
                        ? "waiting_game_modules" : "waiting_for_game"),
                find_result == -4 ? observed_game_pid : -1,
                sessions, total_output_frames, 0);
            if (sleep_interruptible(1000000) != 0)
                break;
            continue;
        }
        uint64_t now_ms = monotonic_milliseconds();
        if (launch_candidate_pid != observed_game_pid) {
            launch_candidate_pid = observed_game_pid;
            launch_candidate_since_ms = now_ms;
            poords4_log(
                "[PoorDS4] game modules ready pid=%d; "
                "launch grace=%llu ms\n",
                observed_game_pid,
                (unsigned long long)GAME_BRIDGE_LAUNCH_GRACE_MS);
        }
        if (now_ms != 0 && launch_candidate_since_ms != 0 &&
            now_ms < launch_candidate_since_ms +
                     GAME_BRIDGE_LAUNCH_GRACE_MS) {
            write_supervisor_state(
                "waiting_game_launch_grace", observed_game_pid,
                sessions, total_output_frames, 0);
            if (sleep_interruptible(250000) != 0)
                break;
            continue;
        }

        pid_t game_pid = -1;
        intptr_t bridge_args = 0;
        int install_result = wireless_ds4_game_bridge_install(
            &g_pad_source, &game_pid, &bridge_args);
        if ((install_result == -2 || install_result == -3) &&
            POORDS4_AUTO_WATCH) {
            retry_pid = -1;
            install_retries = 0;
            write_supervisor_state(
                install_result == -2
                    ? "waiting_for_game"
                    : "waiting_for_unique_game",
                -1, sessions, total_output_frames, 0);
            if (sleep_interruptible(1000000) != 0)
                break;
            continue;
        }
        if (install_result != 1) {
            if (POORDS4_AUTO_WATCH && game_pid > 0) {
                if (retry_pid != game_pid) {
                    retry_pid = game_pid;
                    install_retries = 0;
                }
                install_retries++;
                if (install_result == -6) {
                    poords4_log(
                        "[PoorDS4] verified stale bridge recovery failed "
                        "pid=%d; refusing repeated attach attempts until "
                        "the game exits\n", game_pid);
                    wait_for_game_exit_or_stop(
                        game_pid, "stale_recovery_failed", sessions,
                        total_output_frames);
                    retry_pid = -1;
                    install_retries = 0;
                    continue;
                }
                /* A fail-closed structural snapshot can be transient while a
                 * newly launched title is still publishing its pad imports.
                 * RC33 treated result 0 as permanent immediately, which could
                 * strand one game switch until close/reinject. Every failed
                 * install rolls back hooks and its anonymous mapping, so a
                 * short bounded retry is safe. */
                unsigned retry_limit = install_result == -4
                    ? 120u : (install_result == 0 ? 5u : 15u);
                if (install_retries <= retry_limit) {
                    unsigned retry_delay_us = install_retries <= 3u
                        ? 1000000u
                        : (install_retries <= 15u
                            ? 2000000u : 5000000u);
                    poords4_log(
                        "[PoorDS4] game not ready pid=%d result=%d "
                        "retry=%u/%u delay_ms=%u\n",
                        game_pid, install_result, install_retries,
                        retry_limit, retry_delay_us / 1000u);
                    write_supervisor_state(
                        "waiting_game_ready", game_pid, sessions,
                        total_output_frames, 0);
                    if (sleep_interruptible(retry_delay_us) != 0)
                        break;
                    continue;
                }
                poords4_log(
                    "[PoorDS4] game skipped pid=%d result=%d retries=%u; "
                    "waiting for it to exit\n",
                    game_pid, install_result, install_retries);
                wait_for_game_exit_or_stop(
                    game_pid, "game_skipped", sessions,
                    total_output_frames);
                retry_pid = -1;
                install_retries = 0;
                continue;
            }
            poords4_log(
                "[PoorDS4] game bridge install failed result=%d\n",
                install_result);
            break;
        }
        if (wait_for_game_bridge_ready(game_pid, bridge_args) != 0) {
            poords4_log(
                "[PoorDS4] game bridge did not become ready pid=%d\n",
                game_pid);
            int bridge_safe_cleanup = !g_skip_remote_cleanup;
            const char *bridge_cleanup_mode = NULL;
            int bridge_cleanup = teardown_game_bridge(
                game_pid, bridge_args, bridge_safe_cleanup,
                &bridge_cleanup_mode);
            poords4_log(
                "[PoorDS4] bridge-ready failure cleanup=%d mode=%s\n",
                bridge_cleanup,
                bridge_cleanup_mode ? bridge_cleanup_mode : "unknown");
            if (bridge_cleanup != 0 && process_alive(game_pid)) {
                poords4_log(
                    "[PoorDS4] bridge cleanup failed pid=%d; "
                    "not reinstalling until this game exits\n",
                    game_pid);
                wait_for_game_exit_or_stop(
                    game_pid, "bridge_cleanup_failed", sessions,
                    total_output_frames);
                continue;
            }
            if (POORDS4_AUTO_WATCH &&
                process_alive(game_pid)) {
                if (retry_pid != game_pid) {
                    retry_pid = game_pid;
                    install_retries = 0;
                }
                install_retries++;
                if (install_retries <= 5) {
                    poords4_log(
                        "[PoorDS4] bridge-ready retry pid=%d retry=%u/5\n",
                        game_pid, install_retries);
                    write_supervisor_state(
                        "waiting_bridge_ready", game_pid, sessions,
                        total_output_frames, 0);
                    if (sleep_interruptible(1000000) != 0)
                        break;
                    continue;
                }
                poords4_log(
                    "[PoorDS4] bridge-ready failed repeatedly pid=%d; "
                    "waiting for it to exit\n",
                    game_pid);
                wait_for_game_exit_or_stop(
                    game_pid, "bridge_ready_failed", sessions,
                    total_output_frames);
                retry_pid = -1;
                install_retries = 0;
                continue;
            }
            if (POORDS4_AUTO_WATCH &&
                !process_alive(game_pid)) {
                poords4_log(
                    "[PoorDS4] game exited before bridge-ready; "
                    "returning to watcher pid=%d\n",
                    game_pid);
                launch_candidate_pid = -1;
                launch_candidate_since_ms = 0;
                retry_pid = -1;
                install_retries = 0;
                continue;
            }
            break;
        }
        sessions++;
        poords4_log(
            "[PoorDS4] game bridge installed reader_pid=%d "
            "reader_args=0x%lx game_pid=%d bridge_args=0x%lx "
            "session=%u\n",
            reader_pid, (unsigned long)reader_args,
            game_pid, (unsigned long)bridge_args, sessions);
        game_bridge_notify(
            "PoorDS4: wireless DS4 active in the PS5 game");
        GameSessionEndReason end_reason = SESSION_END_STOP_REQUESTED;
        for (;;) {
            unsigned session_output_frames = run_game_session(
                reader_pid, reader_args, game_pid, bridge_args,
                sessions, total_output_frames, &end_reason);
            total_output_frames += session_output_frames;
            if (total_output_frames > 0)
                exit_code = 0;

            if (end_reason != SESSION_END_READER_FAILED ||
                lifecycle_should_stop() || !process_alive(game_pid))
                break;

            int reader_stop = process_alive(reader_pid)
                ? wireless_ds4_remote_reader_stop(reader_pid, reader_args)
                : 0;
            poords4_log(
                "[PoorDS4] wireless reader recovery stop=%d\n",
                reader_stop);
            reader_restart_required = 1;
            if (reader_stop == 0) {
                reader_pid = -1;
                reader_args = 0;
                g_reader_pid = -1;
                g_reader_args = 0;
                if (recover_wireless_reader_in_place(
                        &g_pad_source, &reader_pid, &reader_args,
                        game_pid, bridge_args, sessions,
                        total_output_frames) == 0) {
                    reader_restart_required = 0;
                    poords4_log(
                        "[PoorDS4] wireless reader recovered; "
                        "continuing current game without reinstall\n");
                    end_reason = SESSION_END_STOP_REQUESTED;
                    continue;
                }
            }

            int safe_cleanup = !g_skip_remote_cleanup;
            const char *recovery_cleanup_mode = NULL;
            int recovery_cleanup = teardown_game_bridge(
                game_pid, bridge_args, safe_cleanup,
                &recovery_cleanup_mode);
            poords4_log(
                "[PoorDS4] retained bridge recovery failed; "
                "cleanup=%s result=%d\n",
                recovery_cleanup_mode
                    ? recovery_cleanup_mode : "unknown",
                recovery_cleanup);
            if (process_alive(game_pid) && recovery_cleanup != 0)
                end_reason = SESSION_END_CLEANUP_FAILED;
            else if (!process_alive(game_pid))
                end_reason = SESSION_END_GAME_EXITED;
            else if (lifecycle_should_stop())
                end_reason = SESSION_END_LIFECYCLE;
            else
                end_reason = SESSION_END_READER_FAILED;
            break;
        }

        if (!POORDS4_AUTO_WATCH || lifecycle_should_stop())
            break;
        if (end_reason == SESSION_END_CLEANUP_FAILED &&
            process_alive(game_pid)) {
            poords4_log(
                "[PoorDS4] bridge cleanup failed pid=%d; "
                "not reinstalling until this game exits\n",
                game_pid);
            wait_for_game_exit_or_stop(
                game_pid, "bridge_cleanup_failed", sessions,
                total_output_frames);
            continue;
        }
        if (end_reason == SESSION_END_READER_FAILED &&
            process_alive(game_pid)) {
            poords4_log(
                "[PoorDS4] reader unavailable after clean bridge removal; "
                "restoring before reinstall pid=%d\n",
                game_pid);
            continue;
        }
        poords4_log(
            "[PoorDS4] waiting for next game after session=%u\n",
            sessions);
        write_supervisor_state(
            "waiting_for_next_game", -1, sessions,
            total_output_frames, 1);
        if (sleep_interruptible(1000000) != 0)
            break;
    }

cleanup:
    if (!g_skip_remote_cleanup && reader_pid >= 0 && reader_args != 0 &&
        process_alive(reader_pid)) {
        int reader_stop =
            wireless_ds4_remote_reader_stop(reader_pid, reader_args);
        poords4_log(
            "[PoorDS4] wireless reader stop=%d\n", reader_stop);
    }
    else if (reader_pid >= 0 && reader_args != 0) {
        poords4_log(
            "[PoorDS4] wireless reader cleanup skipped "
            "skip_remote=%d alive=%d\n",
            g_skip_remote_cleanup, process_alive(reader_pid));
    }
    if (g_system_state_status_flag >= 0) {
        int close_result = sceKernelCloseEventFlag(
            g_system_state_status_flag);
        poords4_log(
            "[PoorDS4] lifecycle status flag close=0x%08x\n",
            (uint32_t)close_result);
        g_system_state_status_flag = -1;
    }
    if (g_system_state_info_flag >= 0) {
        int close_result = sceKernelCloseEventFlag(
            g_system_state_info_flag);
        poords4_log(
            "[PoorDS4] lifecycle info flag close=0x%08x\n",
            (uint32_t)close_result);
        g_system_state_info_flag = -1;
    }
    if (g_app_focus_flag >= 0) {
        int close_result = sceKernelCloseEventFlag(g_app_focus_flag);
        poords4_log(
            "[PoorDS4] app-focus flag close=0x%08x\n",
            (uint32_t)close_result);
        g_app_focus_flag = -1;
    }
    (void)unlink(GAME_BRIDGE_STOP_FILE);
    write_supervisor_state(
        "stopped", -1, sessions, total_output_frames, 1);
    if (!g_suspend_requested && !g_resume_gap_detected)
        game_bridge_notify("PoorDS4: wireless DS4 bridge stopped");
    if (g_supervisor_lock_fd >= 0) {
        (void)flock(g_supervisor_lock_fd, LOCK_UN);
        close(g_supervisor_lock_fd);
        g_supervisor_lock_fd = -1;
    }
    poords4_log(
        "[PoorDS4] game bridge exit=%d sessions=%u total_out=%llu\n",
        exit_code, sessions, total_output_frames);
    payload_exit(exit_code);
    return 0;
}
