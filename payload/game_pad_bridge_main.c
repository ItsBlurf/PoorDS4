/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wireless DS4 -> native PS5 game ScePadData bridge.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/klog.h>
#include <ps5/kernel.h>
#include <ps5/payload.h>

#include "pad_types.h"
#include "wireless_ds4.h"

typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char     title_id[14];
    char     unknown2[0x3c];
} app_info_t;
extern int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

#define POORDS4_DATA_DIR   "/data/poords4"
#define GAME_BRIDGE_STOP_FILE  POORDS4_DATA_DIR "/stop-game-pad-bridge"
#define GAME_BRIDGE_RESET_FILE POORDS4_DATA_DIR "/reset-game-pad-bridge"
#define GAME_BRIDGE_LOG_FILE   POORDS4_DATA_DIR "/game-pad-bridge.log"
#define GAME_BRIDGE_LOG_BACKUP POORDS4_DATA_DIR "/game-pad-bridge.log.1"
#define GAME_BRIDGE_LOCK_FILE  POORDS4_DATA_DIR "/game-pad-bridge-supervisor.lock"
#define GAME_BRIDGE_STATE_FILE POORDS4_DATA_DIR "/game-pad-bridge-supervisor.txt"
#define GAME_BRIDGE_LAUNCH_GRACE_MS UINT64_C(1500)
#define SOURCE_DISCONNECT_GRACE_MS  UINT64_C(1500)
#define SOURCE_REDISCOVERY_MS       UINT64_C(2500)
#define SOURCE_DISCOVERY_RETRY_US   500000u
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
static intptr_t g_bridge_args;
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
    (void)signal(SIGHUP, SIG_IGN);
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
        "bridge_args=0x%lx\n"
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
        (unsigned long)g_bridge_args,
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
    SESSION_END_CLEANUP_FAILED = 6,
    SESSION_END_RESET_REQUESTED = 7
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
    case SESSION_END_RESET_REQUESTED:
        return "reset_requested";
    }
    return "unknown";
}

static uint32_t
collect_user_candidates(int32_t user_ids[POORDS4_MAX_USER_CANDIDATES]);

static inline int
is_reset_combo_held(uint32_t buttons, uint8_t l2, uint8_t r2)
{
    int l1_held = (buttons & 0x00000400u) != 0;
    int r1_held = (buttons & 0x00000800u) != 0;
    int l2_held = ((buttons & 0x00000100u) != 0) || (l2 >= 32u);
    int r2_held = ((buttons & 0x00000200u) != 0) || (r2 >= 32u);
    return l1_held && r1_held && l2_held && r2_held;
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
    pad->touchData.fingers = 0;
    pad->touchData.touch[0].finger = 0x80;
    pad->touchData.touch[1].finger = 0x80;
    pad->timestamp = timestamp;
    pad->count = count;
}

static void
format_buttons_str(uint32_t buttons, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (buttons == 0) {
        snprintf(out, cap, "NONE");
        return;
    }
    struct { uint32_t mask; const char *name; } map[] = {
        { 0x00000001u, "SELECT" },
        { 0x00000002u, "L3" },
        { 0x00000004u, "R3" },
        { 0x00000008u, "START" },
        { 0x00000010u, "UP" },
        { 0x00000020u, "RIGHT" },
        { 0x00000040u, "DOWN" },
        { 0x00000080u, "LEFT" },
        { 0x00000100u, "L2" },
        { 0x00000200u, "R2" },
        { 0x00000400u, "L1" },
        { 0x00000800u, "R1" },
        { 0x00001000u, "TRIANGLE" },
        { 0x00002000u, "CIRCLE" },
        { 0x00004000u, "CROSS" },
        { 0x00008000u, "SQUARE" },
        { 0x00100000u, "TOUCHPAD" },
    };
    size_t written = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (buttons & map[i].mask) {
            int n = snprintf(out + written, cap - written, "%s%s",
                             written > 0 ? "|" : "", map[i].name);
            if (n > 0)
                written += (size_t)n;
            if (written >= cap - 1u)
                break;
        }
    }
}

static const char *
event_stub_name(uint8_t stub_kind)
{
    switch (stub_kind) {
    case POORDS4_EVT_KIND_READ_STATE: return "read_state";
    case POORDS4_EVT_KIND_READ_STATE_EXT: return "read_state_ext";
    case POORDS4_EVT_KIND_READ: return "read";
    case POORDS4_EVT_KIND_READ_EXT: return "read_ext";
    case POORDS4_EVT_KIND_DATA_INTERNAL: return "data_internal";
    case POORDS4_EVT_KIND_CONTROLLER_INFO: return "controller_info";
    case POORDS4_EVT_KIND_EXT_CONTROLLER_INFO: return "ext_controller_info";
    default: return "unknown";
    }
}

static void
write_game_session_summary(
    pid_t game_pid, const PoorDS4GameBridgeStatus *status,
    const PoorDS4PadSource *source,
    unsigned input_frames, unsigned output_frames,
    unsigned stale_frames, unsigned activity_frames,
    GameSessionEndReason end_reason,
    uint64_t session_start_ms, uint64_t session_end_ms)
{
    if (!status || game_pid <= 0)
        return;

    mkdir(POORDS4_DATA_DIR, 0755);
    mkdir(POORDS4_DATA_DIR "/reports", 0755);

    char title_id[32] = "UNKNOWN";
    app_info_t appinfo;
    memset(&appinfo, 0, sizeof(appinfo));
    if (sceKernelGetAppInfo(game_pid, &appinfo) == 0 && appinfo.title_id[0] != '\0') {
        snprintf(title_id, sizeof(title_id), "%s", appinfo.title_id);
    }

    uint32_t fw = 0;
#if defined(__PROSPERO__)
    fw = kernel_get_fw_version();
#endif

    char archive_path[160];
    snprintf(archive_path, sizeof(archive_path),
             POORDS4_DATA_DIR "/reports/session-summary-fw-%08x-pid-%d.txt",
             fw, game_pid);
    const char *paths[2] = {
        POORDS4_DATA_DIR "/game-pad-bridge-summary.txt",
        archive_path
    };

    uint64_t duration_s = (session_end_ms > session_start_ms)
        ? (session_end_ms - session_start_ms) / 1000ull : 0ull;

    for (int p = 0; p < 2; ++p) {
        FILE *f = fopen(paths[p], "w");
        if (!f)
            continue;

        fprintf(f, "================================================================================\n");
        fprintf(f, "PoorDS4 Game Bridge Diagnostic & Post-Mortem Session Summary\n");
        fprintf(f, "================================================================================\n");
        fprintf(f, "Session Details:\n");
        fprintf(f, "  Firmware:          0x%08x\n", fw);
        fprintf(f, "  Game PID:          %d\n", game_pid);
        fprintf(f, "  Title ID:          %s\n", title_id);
        fprintf(f, "  End Reason:        %s (%d)\n", game_session_end_reason_name(end_reason), end_reason);
        fprintf(f, "  Duration:          %llu seconds\n", (unsigned long long)duration_s);
        fprintf(f, "  Total Frames In:   %u\n", input_frames);
        fprintf(f, "  Total Frames Out:  %u\n", output_frames);
        fprintf(f, "  Stale Frames:      %u\n", stale_frames);
        fprintf(f, "  Activity Frames:   %u\n", activity_frames);
        if (source) {
            fprintf(f, "  Source User:       0x%08x\n", (uint32_t)source->user_id);
            fprintf(f, "  Source Pad Index:  %d\n", source->pad_index);
            fprintf(f, "  Source Handle:     0x%08x\n", (uint32_t)source->pad_handle);
            fprintf(f, "  Source Is DS4:     %d\n", source->ds4_connected);
        }
        fprintf(f, "\n");

        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "Controller Handle & Dispatch Telemetry:\n");
        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "  Configured Bridge Handle: 0x%08x (Slot: %d)\n", (uint32_t)status->pad_handle, status->game_pad_index);
        fprintf(f, "  Last Caller Handle:       0x%08x\n", (uint32_t)status->last_caller_handle);
        fprintf(f, "  Last Mismatched Handle:   0x%08x\n", (uint32_t)status->last_mismatched_handle);
        fprintf(f, "  Handle Match Calls:       %llu\n", (unsigned long long)status->handle_match_calls);
        fprintf(f, "  Handle Mismatch Calls:    %llu\n", (unsigned long long)status->handle_mismatch_calls);
        fprintf(f, "  Observed Caller Handles:\n");
        for (unsigned i = 0; i < 4u; ++i) {
            if (status->observed_handles[i] != -1 && status->observed_handles[i] != 0) {
                fprintf(f, "    [%u] Handle 0x%08x: %llu calls%s\n",
                        i, (uint32_t)status->observed_handles[i],
                        (unsigned long long)status->observed_handle_calls[i],
                        (status->observed_handles[i] == status->pad_handle) ? " [BRIDGE TARGET]" : " [MISMATCH]");
            }
        }
        fprintf(f, "\n");

        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "API Import Hook Distribution:\n");
        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "  scePadReadState:          %llu calls\n", (unsigned long long)status->read_state_calls);
        fprintf(f, "  scePadReadStateExt:       %llu calls\n", (unsigned long long)status->read_state_ext_calls);
        fprintf(f, "  scePadRead:               %llu calls\n", (unsigned long long)status->read_calls);
        fprintf(f, "  scePadReadExt:            %llu calls\n", (unsigned long long)status->read_ext_calls);
        fprintf(f, "  scePadGetDataInternal:    %llu calls\n", (unsigned long long)status->data_internal_calls);
        fprintf(f, "  scePadGetControllerInfo:  %llu calls (spoofs: %llu, overrides: %llu)\n",
                (unsigned long long)status->controller_info_calls,
                (unsigned long long)status->controller_info_spoofs,
                (unsigned long long)status->controller_info_result_overrides);
        fprintf(f, "  Installed Import Hooks:   %u\n", status->import_hook_count);
        fprintf(f, "\n");

        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "Loop Pacing & Drain Diagnostics (Kena / Visage / Slate Detection):\n");
        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "  Read Non-Zero Returns:    %llu (Packets Delivered)\n", (unsigned long long)status->read_nonzero_returns);
        fprintf(f, "  Read Zero Returns:        %llu (Drained / No Packet)\n", (unsigned long long)status->read_zero_returns);
        fprintf(f, "  Max Unpaced Read Streak:  %u (Reads in one frame without pause)\n", status->max_read_streak);
        if (status->read_nonzero_returns + status->read_zero_returns > 0) {
            double drain_ratio = (double)status->read_zero_returns /
                (double)(status->read_nonzero_returns + status->read_zero_returns) * 100.0;
            fprintf(f, "  Drain Ratio:              %.2f%% empty reads\n", drain_ratio);
        }
        fprintf(f, "\n");

        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "Frame Delivery & Backing Hardware Fallback:\n");
        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "  Direct Packets Published: %llu\n", (unsigned long long)status->published_packets);
        fprintf(f, "  Direct Leases Expired:    %llu\n", (unsigned long long)status->lease_expirations);
        fprintf(f, "  Direct Fallback Frames:   %llu (active: %llu)\n",
                (unsigned long long)status->direct_fallback_frames,
                (unsigned long long)status->direct_active_fallbacks);
        fprintf(f, "  Snapshot Contentions:     %llu\n", (unsigned long long)status->snapshot_contention_fallbacks);
        fprintf(f, "  Native Backing Calls:     %llu (errors: %llu)\n",
                (unsigned long long)status->native_backing_calls,
                (unsigned long long)status->native_backing_errors);
        fprintf(f, "  Native Passthrough:       %llu frames\n", (unsigned long long)status->native_passthrough_frames);
        fprintf(f, "  Native Connected Frames:  %llu\n", (unsigned long long)status->native_connected_frames);
        fprintf(f, "  Native Input Activity:    %llu frames\n", (unsigned long long)status->native_input_activity_frames);
        fprintf(f, "  Last Native Result:       0x%08x\n", (uint32_t)status->last_native_result);
        fprintf(f, "\n");

        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "Input Event Ring Buffer (Last 64 Events Chronological):\n");
        fprintf(f, "--------------------------------------------------------------------------------\n");
        fprintf(f, "Seq    | Timestamp (ms) | Delta | API Stub        | Handle     | Flags | Sticks (L/R)        | Triggers | Buttons\n");
        fprintf(f, "-------+----------------+-------+-----------------+------------+-------+---------------------+----------+-------------------------\n");

        uint32_t head = status->event_ring_head;
        uint32_t start_seq = (head >= POORDS4_GAME_BRIDGE_EVENT_RING_SIZE)
            ? (head - POORDS4_GAME_BRIDGE_EVENT_RING_SIZE) : 0u;
        uint32_t prev_time = 0;

        for (uint32_t s = start_seq; s < head; ++s) {
            uint32_t idx = s % POORDS4_GAME_BRIDGE_EVENT_RING_SIZE;
            const PoorDS4InputEvent *evt = &status->event_ring[idx];
            if (evt->seq != s && head >= POORDS4_GAME_BRIDGE_EVENT_RING_SIZE)
                continue;

            uint32_t delta = (prev_time > 0 && evt->timestamp_ms >= prev_time)
                ? (evt->timestamp_ms - prev_time) : 0u;
            prev_time = evt->timestamp_ms;

            char flag_str[16] = {0};
            snprintf(flag_str, sizeof(flag_str), "%s%s%s%s",
                     (evt->flags & POORDS4_EVT_FLAG_DIRECT) ? "D" : "",
                     (evt->flags & POORDS4_EVT_FLAG_NATIVE) ? "N" : "",
                     (evt->flags & POORDS4_EVT_FLAG_CHANGED) ? "C" : "",
                     (evt->flags & POORDS4_EVT_FLAG_MISMATCH) ? "M" : "");
            if (flag_str[0] == '\0')
                snprintf(flag_str, sizeof(flag_str), "-");

            char btn_str[128];
            format_buttons_str(evt->buttons, btn_str, sizeof(btn_str));

            fprintf(f, "%-6u | %-14u | +%-4u | %-15s | 0x%08x | %-5s | (%3u,%3u)(%3u,%3u) | L2:%-3u R2:%-3u | %s\n",
                    evt->seq, evt->timestamp_ms, delta,
                    event_stub_name(evt->stub_kind),
                    (uint32_t)evt->handle, flag_str,
                    evt->lx, evt->ly, evt->rx, evt->ry,
                    evt->l2, evt->r2, btn_str);
        }
        fprintf(f, "================================================================================\n");
        fclose(f);
    }
}

static uint64_t g_sim_last_check_ms = 0;
static int g_sim_mask = 0;
static uint32_t g_sim_buttons[POORDS4_MAX_SLOTS] = {0, 0, 0, 0};
static uint8_t g_sim_lx[POORDS4_MAX_SLOTS] = {128, 128, 128, 128};
static uint8_t g_sim_ly[POORDS4_MAX_SLOTS] = {128, 128, 128, 128};
static uint8_t g_sim_rx[POORDS4_MAX_SLOTS] = {128, 128, 128, 128};
static uint8_t g_sim_ry[POORDS4_MAX_SLOTS] = {128, 128, 128, 128};
static uint8_t g_sim_l2[POORDS4_MAX_SLOTS] = {0, 0, 0, 0};
static uint8_t g_sim_r2[POORDS4_MAX_SLOTS] = {0, 0, 0, 0};

static void
refresh_simulated_pads_config(void)
{
    uint64_t now_ms = monotonic_milliseconds();
    if (now_ms - g_sim_last_check_ms < 500u && g_sim_last_check_ms != 0)
        return;
    g_sim_last_check_ms = now_ms;

    int fd = open("/data/poords4/simulated_pads.txt", O_RDONLY);
    if (fd < 0) {
        g_sim_mask = 0;
        return;
    }
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        g_sim_mask = 0;
        return;
    }
    buf[n] = '\0';
    g_sim_mask = 0;
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        g_sim_buttons[s] = 0;
        g_sim_lx[s] = 128;
        g_sim_ly[s] = 128;
        g_sim_rx[s] = 128;
        g_sim_ry[s] = 128;
        g_sim_l2[s] = 0;
        g_sim_r2[s] = 0;
    }

    if (strstr(buf, "slots=none")) {
        g_sim_mask = 0;
        return;
    }

    char *p_slots = strstr(buf, "slots=");
    if (strstr(buf, "slots=all") || (p_slots && strstr(p_slots, "all"))) {
        g_sim_mask = 0x0f;
    } else if (p_slots) {
        char *end = strchr(p_slots, '\n');
        size_t len = end ? (size_t)(end - p_slots) : strlen(p_slots);
        char line[128];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p_slots, len);
        line[len] = '\0';
        for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
            char digit = (char)('0' + s);
            if (strchr(line + 6, digit))
                g_sim_mask |= (1 << s);
        }
    } else if (strstr(buf, "all")) {
        g_sim_mask = 0x0f;
    }

    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        char key[16];
        snprintf(key, sizeof(key), "btn%u=", s);
        char *p = strstr(buf, key);
        if (p) g_sim_buttons[s] = (uint32_t)strtoul(p + strlen(key), NULL, 0);

        snprintf(key, sizeof(key), "lx%u=", s);
        p = strstr(buf, key);
        if (p) g_sim_lx[s] = (uint8_t)strtoul(p + strlen(key), NULL, 0);

        snprintf(key, sizeof(key), "ly%u=", s);
        p = strstr(buf, key);
        if (p) g_sim_ly[s] = (uint8_t)strtoul(p + strlen(key), NULL, 0);

        snprintf(key, sizeof(key), "rx%u=", s);
        p = strstr(buf, key);
        if (p) g_sim_rx[s] = (uint8_t)strtoul(p + strlen(key), NULL, 0);

        snprintf(key, sizeof(key), "ry%u=", s);
        p = strstr(buf, key);
        if (p) g_sim_ry[s] = (uint8_t)strtoul(p + strlen(key), NULL, 0);

        snprintf(key, sizeof(key), "l2_%u=", s);
        p = strstr(buf, key);
        if (p) g_sim_l2[s] = (uint8_t)strtoul(p + strlen(key), NULL, 0);

        snprintf(key, sizeof(key), "r2_%u=", s);
        p = strstr(buf, key);
        if (p) g_sim_r2[s] = (uint8_t)strtoul(p + strlen(key), NULL, 0);
    }
}

static void
feed_multi_controller_slots(pid_t reader_pid, intptr_t reader_args,
                            pid_t game_pid, intptr_t bridge_args,
                            uint64_t input_frames, unsigned primary_slot,
                            const PoorDS4GameBridgeStatus *bridge_status,
                            const ScePadData *primary_pad)
{
    static int s_prev_fed_mask = 0;
    int current_fed_mask = 0;
    refresh_simulated_pads_config();

    uint64_t now_ms = monotonic_milliseconds();
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        if (s == primary_slot)
            continue;
        /* A native DualSense slot keeps Sony's path. Publishing into it
         * clears is_dualsense and hands that player another controller. */
        if (bridge_status && bridge_status->slots[s].is_dualsense)
            continue;
        if (g_sim_mask & (1 << s)) {
            current_fed_mask |= (1 << s);
            ScePadData sim_pad;
            memset(&sim_pad, 0, sizeof(sim_pad));
            sim_pad.buttons = g_sim_buttons[s];
            sim_pad.leftStick.x = g_sim_lx[s];
            sim_pad.leftStick.y = g_sim_ly[s];
            sim_pad.rightStick.x = g_sim_rx[s];
            sim_pad.rightStick.y = g_sim_ry[s];
            sim_pad.analogButtons.l2 = g_sim_l2[s];
            sim_pad.analogButtons.r2 = g_sim_r2[s];
            sim_pad.connected = 1;
            sim_pad.timestamp = (uint64_t)now_ms * 1000u;
            sim_pad.count = (uint8_t)(input_frames & 0xff);

            (void)wireless_ds4_game_bridge_update_slot(
                game_pid, bridge_args, s, &sim_pad, sizeof(sim_pad),
                1 /* is_simulated */, 0 /* is_dualsense */);
        } else {
            ScePadData physical_pad;
            uint32_t slot_seq = 0;
            /* Reader slot 0 is the primary DS4, already published to
             * primary_slot; only reader slots 1..3 are other DS4s. */
            if (s != 0 && reader_pid > 0 && reader_args > 0 &&
                wireless_ds4_remote_reader_read_slot(
                    reader_pid, reader_args, s,
                    &physical_pad, sizeof(physical_pad), &slot_seq) == 0 &&
                physical_pad.connected != 0) {
                current_fed_mask |= (1 << s);
                (void)wireless_ds4_game_bridge_update_slot(
                    game_pid, bridge_args, s, &physical_pad, sizeof(physical_pad),
                    0 /* is_simulated */, 0 /* is_dualsense */);
            } else if (bridge_status && bridge_status->slots[s].pad_handle > 0 &&
                       bridge_status->slots[s].user_matches &&
                       !bridge_status->slots[s].is_dualsense && primary_pad) {
                current_fed_mask |= (1 << s);
                (void)wireless_ds4_game_bridge_update_slot(
                    game_pid, bridge_args, s, primary_pad, sizeof(*primary_pad),
                    0 /* is_simulated */, 0 /* is_dualsense */);
            } else if (s_prev_fed_mask & (1 << s)) {
                (void)wireless_ds4_game_bridge_deactivate_slot(game_pid, bridge_args, s);
            }
        }
    }
    s_prev_fed_mask = current_fed_mask;
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
    unsigned activity_frames = 0;
    unsigned input_transitions = 0;
    unsigned transition_logs = 0;
    uint32_t previous_buttons = 0;
    uint8_t previous_lx = 0;
    uint8_t previous_ly = 0;
    uint8_t previous_rx = 0;
    uint8_t previous_ry = 0;
    uint8_t previous_l2 = 0;
    uint8_t previous_r2 = 0;
    int have_previous_input = 0;
    uint64_t disconnect_started_ms = 0;
    unsigned disconnect_frames = 0;
    unsigned disconnect_grace_events = 0;
    unsigned disconnect_grace_expired = 0;
    PoorDS4GameBridgeStatus last_bridge_status;
    memset(&last_bridge_status, 0, sizeof(last_bridge_status));
    PoorDS4RemoteReaderStatus last_reader_status;
    memset(&last_reader_status, 0, sizeof(last_reader_status));
    int game_alive = 1;
    uint32_t session_reset_combo_ticks = 0;
    GameSessionEndReason end_reason = SESSION_END_STOP_REQUESTED;
    uint64_t session_start_ms = monotonic_milliseconds();
    g_bridge_args = bridge_args;
    PoorDS4GameBridgeStatus initial_status;
    unsigned primary_slot = 0;
    if (wireless_ds4_game_bridge_status(game_pid, bridge_args, &initial_status) == 0 &&
        initial_status.game_pad_index >= 0 && initial_status.game_pad_index < (int32_t)POORDS4_MAX_SLOTS) {
        primary_slot = (unsigned)initial_status.game_pad_index;
    } else if (g_pad_source.pad_index >= 0 && g_pad_source.pad_index < (int32_t)POORDS4_MAX_SLOTS) {
        primary_slot = (unsigned)g_pad_source.pad_index;
    }
    last_bridge_status = initial_status;
    write_supervisor_state(
        "active", game_pid, session, previous_output_frames, 1);
    while (!lifecycle_should_stop()) {
        ScePadData pad;
        uint32_t seq = 0;
        memset(&pad, 0, sizeof(pad));
        refresh_simulated_pads_config();
        int sim_on_primary = (g_sim_mask & (1 << primary_slot)) != 0;

        int read_result = wireless_ds4_remote_reader_read(
            reader_pid, reader_args, &pad, sizeof(pad), &seq);
        if (read_result == 0 || sim_on_primary) {
            consecutive_read_failures = 0;
            if (read_result != 0 && sim_on_primary) {
                memset(&pad, 0, sizeof(pad));
                pad.leftStick.x = 128;
                pad.leftStick.y = 128;
                pad.rightStick.x = 128;
                pad.rightStick.y = 128;
                pad.connected = 1;
                pad.timestamp = (uint64_t)monotonic_milliseconds() * 1000u;
                pad.count = (uint8_t)(input_frames & 0xff);
                seq = (uint32_t)input_frames + 1u;
            }
            if (sim_on_primary) {
                pad.buttons |= g_sim_buttons[primary_slot];
                if (g_sim_lx[primary_slot] != 128) pad.leftStick.x = g_sim_lx[primary_slot];
                if (g_sim_ly[primary_slot] != 128) pad.leftStick.y = g_sim_ly[primary_slot];
                if (g_sim_rx[primary_slot] != 128) pad.rightStick.x = g_sim_rx[primary_slot];
                if (g_sim_ry[primary_slot] != 128) pad.rightStick.y = g_sim_ry[primary_slot];
                if (g_sim_l2[primary_slot] != 0) pad.analogButtons.l2 = g_sim_l2[primary_slot];
                if (g_sim_r2[primary_slot] != 0) pad.analogButtons.r2 = g_sim_r2[primary_slot];
                pad.connected = 1;
            }
            if (seq != last_seq) {
                input_frames++;
                if (pad.touchData.fingers == 0) {
                    pad.touchData.touch[0].finger |= 0x80;
                    pad.touchData.touch[1].finger |= 0x80;
                } else {
                    for (unsigned int f = 0; f < 2; ++f) {
                        if ((pad.touchData.touch[f].finger & 0x80) == 0) {
                            uint32_t y = pad.touchData.touch[f].y;
                            if (y < 942u) {
                                y = (y * 1080u + 471u) / 942u;
                                if (y > 1079u)
                                    y = 1079u;
                                pad.touchData.touch[f].y = (uint16_t)y;
                            }
                        }
                    }
                }
                /* DualShock 4 potentiometer resting snap: eliminate mechanical deadband drift
                 * within +/- 12 of 128 so menus and sensitive controls remain rock-solid. */
                if (pad.leftStick.x >= 116 && pad.leftStick.x <= 140)
                    pad.leftStick.x = 128;
                if (pad.leftStick.y >= 116 && pad.leftStick.y <= 140)
                    pad.leftStick.y = 128;
                if (pad.rightStick.x >= 116 && pad.rightStick.x <= 140)
                    pad.rightStick.x = 128;
                if (pad.rightStick.y >= 116 && pad.rightStick.y <= 140)
                    pad.rightStick.y = 128;
                int input_changed = have_previous_input &&
                    (pad.buttons != previous_buttons ||
                     pad.leftStick.x > previous_lx + 4 ||
                     previous_lx > pad.leftStick.x + 4 ||
                     pad.leftStick.y > previous_ly + 4 ||
                     previous_ly > pad.leftStick.y + 4 ||
                     pad.rightStick.x > previous_rx + 4 ||
                     previous_rx > pad.rightStick.x + 4 ||
                     pad.rightStick.y > previous_ry + 4 ||
                     previous_ry > pad.rightStick.y + 4 ||
                     pad.analogButtons.l2 != previous_l2 ||
                     pad.analogButtons.r2 != previous_r2);
                int input_active = pad.buttons != 0 ||
                    pad.analogButtons.l2 != 0 ||
                    pad.analogButtons.r2 != 0 ||
                    pad.leftStick.x < 112 || pad.leftStick.x > 144 ||
                    pad.leftStick.y < 112 || pad.leftStick.y > 144 ||
                    pad.rightStick.x < 112 || pad.rightStick.x > 144 ||
                    pad.rightStick.y < 112 || pad.rightStick.y > 144;
                if (input_active)
                    activity_frames++;
                if (input_changed) {
                    input_transitions++;
                    if (transition_logs < 32u ||
                        (input_transitions % 128u) == 0) {
                        poords4_log(
                            "[PoorDS4] SOURCE INPUT transition=%u frame=%u "
                            "seq=%u buttons=0x%08x sticks=%u,%u,%u,%u "
                            "triggers=%u,%u connected=%u timestamp=%llu "
                            "count=%u\n",
                            input_transitions, input_frames, seq,
                            pad.buttons, pad.leftStick.x, pad.leftStick.y,
                            pad.rightStick.x, pad.rightStick.y,
                            pad.analogButtons.l2, pad.analogButtons.r2,
                            pad.connected,
                            (unsigned long long)pad.timestamp, pad.count);
                        transition_logs++;
                    }
                }
                previous_buttons = pad.buttons;
                previous_lx = pad.leftStick.x;
                previous_ly = pad.leftStick.y;
                previous_rx = pad.rightStick.x;
                previous_ry = pad.rightStick.y;
                previous_l2 = pad.analogButtons.l2;
                previous_r2 = pad.analogButtons.r2;
                have_previous_input = 1;
                if (is_reset_combo_held(pad.buttons, pad.analogButtons.l2, pad.analogButtons.r2)) {
                    session_reset_combo_ticks++;
                    if (session_reset_combo_ticks >= 45u) {
                        poords4_log(
                            "[PoorDS4] reset requested via supervisor DS4 stream\n");
                        end_reason = SESSION_END_RESET_REQUESTED;
                        break;
                    }
                } else {
                    if (session_reset_combo_ticks >= 1u)
                        session_reset_combo_ticks -= 1u;
                    else
                        session_reset_combo_ticks = 0;
                }
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
                        disconnect_elapsed_ms >= 1000u) {
                        int32_t current_users[POORDS4_MAX_USER_CANDIDATES];
                        memset(current_users, 0xff, sizeof(current_users));
                        uint32_t current_user_count =
                            collect_user_candidates(current_users);
                        int found_other_user = 0;
                        for (uint32_t u = 0; u < current_user_count; ++u) {
                            if (current_users[u] >= 0 &&
                                current_users[u] != g_pad_source.user_id) {
                                found_other_user = 1;
                                break;
                            }
                        }
                        if (found_other_user) {
                            poords4_log(
                                "[PoorDS4] alternate user active during disconnect "
                                "elapsed=%llu ms; initiating rediscovery\n",
                                (unsigned long long)disconnect_elapsed_ms);
                            end_reason = SESSION_END_READER_FAILED;
                            break;
                        }
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
                if (wireless_ds4_game_bridge_update_slot(
                        game_pid, bridge_args, primary_slot,
                        &pad, sizeof(pad),
                        sim_on_primary ? 1 : 0, 0) == 0) {
                    output_frames++;
                    consecutive_write_failures = 0;
                } else {
                    write_failures++;
                    consecutive_write_failures++;
                }
                feed_multi_controller_slots(
                    reader_pid, reader_args,
                    game_pid, bridge_args, input_frames, primary_slot,
                    &last_bridge_status, &pad);
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
                "buttons=0x%08x sticks=%u,%u,%u,%u triggers=%u,%u "
                "timestamp=%llu count=%u activity=%u transitions=%u "
                "source_path=mode:%u queued:0x%08x ok:%u empty:%u "
                "errors:%u state:%u "
                "stale=%u read_fail=%u write_fail=%u "
                "bridge_health_fail=%u bridge_ready=%d active=%u "
                "packets=%llu lease_expirations=%llu pad_index=%d "
                "contention=%llu info=%llu/%llu "
                "info_result_overrides=%llu native_backing=%llu/%llu "
                "paths=native:%llu connected:%llu fallback:%llu "
                "direct_active:%llu native_result=0x%08x "
                "native_sample=%llu activity:%llu conn:%u "
                "buttons:0x%08x sticks:%u,%u,%u,%u triggers:%u,%u "
                "timestamp:%llu count:%u "
                "imports=%u\n",
                input_frames, output_frames, last_seq, pad.connected,
                pad.buttons, pad.leftStick.x, pad.leftStick.y,
                pad.rightStick.x, pad.rightStick.y,
                pad.analogButtons.l2, pad.analogButtons.r2,
                (unsigned long long)pad.timestamp, pad.count,
                activity_frames, input_transitions,
                last_reader_status.reader_mode,
                (uint32_t)last_reader_status.last_read_result,
                last_reader_status.read_success_frames,
                last_reader_status.read_empty_frames,
                last_reader_status.read_error_frames,
                last_reader_status.state_fallback_frames,
                stale_frames, read_failures, write_failures,
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
                (unsigned long long)
                    last_bridge_status.native_passthrough_frames,
                (unsigned long long)
                    last_bridge_status.native_connected_frames,
                (unsigned long long)
                    last_bridge_status.direct_fallback_frames,
                (unsigned long long)
                    last_bridge_status.direct_active_fallbacks,
                (uint32_t)last_bridge_status.last_native_result,
                (unsigned long long)
                    last_bridge_status.native_success_frames,
                (unsigned long long)
                    last_bridge_status.native_input_activity_frames,
                last_bridge_status.last_native_connected,
                last_bridge_status.last_native_buttons,
                last_bridge_status.last_native_lx,
                last_bridge_status.last_native_ly,
                last_bridge_status.last_native_rx,
                last_bridge_status.last_native_ry,
                last_bridge_status.last_native_l2,
                last_bridge_status.last_native_r2,
                (unsigned long long)
                    last_bridge_status.last_native_timestamp,
                last_bridge_status.last_native_count,
                last_bridge_status.import_hook_count);
            write_supervisor_state(
                "active", game_pid, session,
                previous_output_frames + output_frames, 0);
            last_health_frame = input_frames;
        }
        loop_count++;
        if ((loop_count % 4u) == 0 && bridge_args != 0 && game_alive) {
            if (wireless_ds4_game_bridge_check_reset(game_pid, bridge_args) == 1) {
                poords4_log(
                    "[PoorDS4] reset requested via fast-poll direct atomic flag\n");
                end_reason = SESSION_END_RESET_REQUESTED;
                break;
            }
            if (access(GAME_BRIDGE_RESET_FILE, F_OK) == 0) {
                (void)unlink(GAME_BRIDGE_RESET_FILE);
                poords4_log(
                    "[PoorDS4] reset requested via reset trigger file\n");
                end_reason = SESSION_END_RESET_REQUESTED;
                break;
            }
        }
        if ((loop_count % (input_frames < 300u ? 6u : 30u)) == 0) {
            if (game_alive) {
                PoorDS4GameBridgeStatus bridge_status;
                memset(&bridge_status, 0, sizeof(bridge_status));
                if (wireless_ds4_game_bridge_status(
                        game_pid, bridge_args, &bridge_status) == 0 &&
                    bridge_status.bridge_ready == 1) {
                    last_bridge_status = bridge_status;
                    if ((loop_count % 120u) == 0)
                        consecutive_bridge_health_failures = 0;
                } else if ((loop_count % 120u) == 0) {
                    consecutive_bridge_health_failures++;
                }
            }
            PoorDS4RemoteReaderStatus reader_status;
            memset(&reader_status, 0, sizeof(reader_status));
            if (wireless_ds4_remote_reader_status(
                    reader_pid, reader_args, &reader_status) == 0)
                last_reader_status = reader_status;
        }
        if ((loop_count % 120u) == 0) {
            errno = 0;
            game_alive = kill(game_pid, 0) == 0 || errno == EPERM;
        }
        if (last_bridge_status.reset_requested || last_reader_status.reset_requested) {
            poords4_log(
                "[PoorDS4] reset requested via status bridge=%u reader=%u\n",
                last_bridge_status.reset_requested,
                last_reader_status.reset_requested);
            end_reason = SESSION_END_RESET_REQUESTED;
            break;
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
    if (process_alive(game_pid)) {
        PoorDS4GameBridgeStatus final_status;
        memset(&final_status, 0, sizeof(final_status));
        if (wireless_ds4_game_bridge_status(game_pid, bridge_args, &final_status) == 0 &&
            final_status.bridge_ready == 1) {
            last_bridge_status = final_status;
        }
    }
    int keep_bridge_for_reader_recovery =
        end_reason == SESSION_END_READER_FAILED &&
        !g_shutdown_requested && !g_skip_remote_cleanup &&
        process_alive(game_pid);
    int remove_result = 0;
    if (keep_bridge_for_reader_recovery) {
        ScePadData neutral;
        make_neutral_connected_pad(&neutral);
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
    write_game_session_summary(
        game_pid, &last_bridge_status, &g_pad_source,
        input_frames, output_frames, stale_frames, activity_frames,
        end_reason, session_start_ms, monotonic_milliseconds());
    g_bridge_args = 0;
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
    int32_t old_user_id = source ? source->user_id : -1;
    int32_t old_pad_index = source ? source->pad_index : -1;
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
            now >= last_status_ms + UINT64_C(100)) {
            PoorDS4GameBridgeStatus status;
            memset(&status, 0, sizeof(status));
            if (wireless_ds4_game_bridge_status(
                    game_pid, bridge_args, &status) != 0 ||
                status.bridge_ready != 1 || !status.active)
                return -1;
            if (status.reset_requested != 0) {
                poords4_log(
                    "[PoorDS4] reset shortcut detected during reader recovery\n");
                return -4;
            }
            last_status_ms = now;
        }

        if (last_attempt_ms == 0 || now == 0 ||
            now >= last_attempt_ms + UINT64_C(250)) {
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
                int user_changed = (old_user_id >= 0 &&
                    (candidate.user_id != old_user_id ||
                     candidate.pad_index != old_pad_index));
                *source = candidate;
                *reader_pid = candidate_pid;
                *reader_args = candidate_args;
                g_reader_pid = candidate_pid;
                g_reader_args = candidate_args;
                g_pad_source = candidate;
                poords4_log(
                    "[PoorDS4] in-place reader recovery complete "
                    "attempts=%u user=0x%08x index=%d user_changed=%d\n",
                    attempts, (uint32_t)candidate.user_id,
                    candidate.pad_index, user_changed);
                return user_changed ? 1 : 0;
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
        if (sleep_interruptible(50000) != 0)
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
    (void)unlink(GAME_BRIDGE_RESET_FILE);
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
    uint64_t last_waiting_source_log_ms = 0;
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
                "result=0x%08x connected=%u mode=%u "
                "queued=0x%08x queued_ok=%u empty=%u errors=%u "
                "state_fallback=%u\n",
                reader_pid, (unsigned long)reader_args,
                reader_preflight_result, reader_preflight.ready,
                reader_preflight.stop,
                (uint32_t)reader_preflight.last_result,
                (unsigned)reader_preflight.connected,
                reader_preflight.reader_mode,
                (uint32_t)reader_preflight.last_read_result,
                reader_preflight.read_success_frames,
                reader_preflight.read_empty_frames,
                reader_preflight.read_error_frames,
                reader_preflight.state_fallback_frames);
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

        {
            uint64_t now = monotonic_milliseconds();
            if (last_waiting_source_log_ms == 0 ||
                (now != 0 && now >= last_waiting_source_log_ms + 10000u)) {
                poords4_log(
                    "[PoorDS4] SOURCE HEALTH waiting seq=%u conn=%u "
                    "buttons=0x%08x sticks=%u,%u,%u,%u triggers=%u,%u "
                    "timestamp=%llu count=%u mode=%u queued=0x%08x "
                    "queued_ok=%u empty=%u errors=%u state_fallback=%u\n",
                    reader_preflight.seq,
                    (unsigned)reader_preflight.connected,
                    reader_preflight.buttons,
                    reader_preflight.left_x, reader_preflight.left_y,
                    reader_preflight.right_x, reader_preflight.right_y,
                    reader_preflight.left_trigger,
                    reader_preflight.right_trigger,
                    (unsigned long long)reader_preflight.timestamp,
                    reader_preflight.count, reader_preflight.reader_mode,
                    (uint32_t)reader_preflight.last_read_result,
                    reader_preflight.read_success_frames,
                    reader_preflight.read_empty_frames,
                    reader_preflight.read_error_frames,
                    reader_preflight.state_fallback_frames);
                last_waiting_source_log_ms = now ? now : 1;
            }
        }

        if (reader_preflight.reset_requested) {
            poords4_log(
                "[PoorDS4] reset requested while waiting for game\n");
            game_bridge_notify(
                "PoorDS4: reset requested via controller shortcut");
            reader_restart_required = 1;
            sleep_interruptible(500000);
            continue;
        }

        if (access(GAME_BRIDGE_RESET_FILE, F_OK) == 0) {
            (void)unlink(GAME_BRIDGE_RESET_FILE);
            poords4_log(
                "[PoorDS4] reset requested via file while waiting for game\n");
            game_bridge_notify(
                "PoorDS4: reset requested via trigger file");
            reader_restart_required = 1;
            sleep_interruptible(500000);
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
                    ? 300u : (install_result == 0 ? 5u : 15u);
                if (install_retries <= retry_limit) {
                    unsigned retry_delay_us;
                    if (install_result == -4) {
                        retry_delay_us = 200000u;
                    } else if (install_retries <= 3u) {
                        retry_delay_us = 1000000u;
                    } else if (install_retries <= 15u) {
                        retry_delay_us = 2000000u;
                    } else {
                        retry_delay_us = 5000000u;
                    }
                    if ((install_result == -4 && (install_retries % 25u) == 0) ||
                        (install_result != -4 && ((install_retries % 10u) == 0 || install_retries <= 5u))) {
                        poords4_log(
                            "[PoorDS4] game not ready pid=%d result=%d "
                            "retry=%u/%u delay_ms=%u\n",
                            game_pid, install_result, install_retries,
                            retry_limit, retry_delay_us / 1000u);
                    }
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
                int recovery_result = recover_wireless_reader_in_place(
                    &g_pad_source, &reader_pid, &reader_args,
                    game_pid, bridge_args, sessions,
                    total_output_frames);
                if (recovery_result == 0) {
                    reader_restart_required = 0;
                    poords4_log(
                        "[PoorDS4] wireless reader recovered; "
                        "continuing current game without reinstall\n");
                    end_reason = SESSION_END_STOP_REQUESTED;
                    continue;
                }
                if (recovery_result == 1) {
                    poords4_log(
                        "[PoorDS4] wireless reader recovered on new user/slot; "
                        "reinstalling game bridge\n");
                    const char *recovery_cleanup_mode = NULL;
                    int recovery_cleanup = teardown_game_bridge(
                        game_pid, bridge_args, !g_skip_remote_cleanup,
                        &recovery_cleanup_mode);
                    poords4_log(
                        "[PoorDS4] user switch bridge cleanup=%s result=%d\n",
                        recovery_cleanup_mode
                            ? recovery_cleanup_mode : "unknown",
                        recovery_cleanup);
                    reader_restart_required = 0;
                    launch_candidate_pid = game_pid;
                    launch_candidate_since_ms = 1;
                    retry_pid = -1;
                    install_retries = 0;
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
                if (recovery_result == -4) {
                    poords4_log(
                        "[PoorDS4] reset shortcut detected during reader recovery pid=%d\n",
                        game_pid);
                    const char *reset_cleanup_mode = NULL;
                    int reset_cleanup = teardown_game_bridge(
                        game_pid, bridge_args, !g_skip_remote_cleanup,
                        &reset_cleanup_mode);
                    poords4_log(
                        "[PoorDS4] recovery reset bridge cleanup=%s result=%d\n",
                        reset_cleanup_mode ? reset_cleanup_mode : "unknown",
                        reset_cleanup);
                    end_reason = SESSION_END_RESET_REQUESTED;
                    break;
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
        if (end_reason == SESSION_END_RESET_REQUESTED &&
            process_alive(game_pid)) {
            poords4_log(
                "[PoorDS4] controller reset shortcut triggered; "
                "resetting reader and re-attaching pid=%d\n",
                game_pid);
            game_bridge_notify(
                "PoorDS4: reset requested via controller shortcut");
            if (reader_pid > 0 && reader_args != 0 &&
                process_alive(reader_pid)) {
                (void)wireless_ds4_remote_reader_stop(
                    reader_pid, reader_args);
            }
            reader_pid = -1;
            reader_args = 0;
            g_reader_pid = -1;
            g_reader_args = 0;
            g_pad_source = (PoorDS4PadSource){-1, -1, -1, 0};
            reader_restart_required = 1;
            launch_candidate_pid = game_pid;
            launch_candidate_since_ms = 1;
            retry_pid = -1;
            install_retries = 0;
            sleep_interruptible(250000);
            continue;
        }
        if ((end_reason == SESSION_END_BRIDGE_HEALTH_FAILED ||
             end_reason == SESSION_END_WRITER_FAILED) &&
            process_alive(game_pid)) {
            poords4_log(
                "[PoorDS4] bridge health/writer error reason=%d (%s); "
                "recovering pid=%d\n",
                end_reason, game_session_end_reason_name(end_reason),
                game_pid);
            reader_restart_required = 1;
            launch_candidate_pid = game_pid;
            launch_candidate_since_ms = 1;
            retry_pid = -1;
            install_retries = 0;
            sleep_interruptible(500000);
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
