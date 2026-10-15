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
#include "pad_types.h"

/* Same published application-info ABI already used by the supervisor. */
typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;
extern int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

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

static int
parse_bridge_identity(const char *buffer, pid_t *out_pid,
                      intptr_t *out_args)
{
    long pid_value = -1;
    unsigned long args_value = 0;
    const char *line = buffer;
    while (line && *line) {
        if (!strncmp(line, "game_pid=", 9))
            pid_value = strtol(line + 9, NULL, 0);
        else if (!strncmp(line, "bridge_args=", 12))
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

/* Export this helper's own fresh stream, rather than reopening a potentially
 * historical report or loading another file-transfer payload. */
static int
export_status_report(FILE *report)
{
    enum { REPORT_LIMIT = 64 * 1024 };
    if (fflush(report) != 0 || fseek(report, 0, SEEK_SET) != 0) {
        printf("POORDS4_STATUS_REPORT_ERROR schema=1 phase=prepare errno=%d\n", errno);
        return -1;
    }
    char buffer[4096];
    size_t total = 0;
    int io_result = 0;
    printf("POORDS4_STATUS_REPORT_BEGIN schema=1\n");
    while (total < REPORT_LIMIT) {
        size_t remaining = REPORT_LIMIT - total;
        size_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t received = fread(buffer, 1, amount, report);
        if (received == 0)
            break;
        if (fwrite(buffer, 1, received, stdout) != received) {
            io_result = -1;
            break;
        }
        total += received;
    }
    if (ferror(report))
        io_result = -1;
    int truncated = total == REPORT_LIMIT;
    printf("POORDS4_STATUS_REPORT_END schema=1 bytes=%zu io_result=%d truncated=%d\n",
           total, io_result, truncated);
    return io_result == 0 && !truncated ? 0 : -1;
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

    pid_t bridge_pid = -1;
    intptr_t bridge_args = 0;
    int bridge_identity_result = supervisor_length >= 0
        ? parse_bridge_identity(supervisor, &bridge_pid, &bridge_args) : -1;
    PoorDS4GameBridgeStatus bridge;
    memset(&bridge, 0, sizeof(bridge));
    int bridge_result = bridge_identity_result == 0
        ? wireless_ds4_game_bridge_status(
              bridge_pid, bridge_args, &bridge) : -1;
    app_info_t appinfo;
    memset(&appinfo, 0, sizeof(appinfo));
    int app_info_result = bridge_pid > 0
        ? sceKernelGetAppInfo(bridge_pid, &appinfo) : -1;

    FILE *f = fopen(STATUS_REPORT_TMP, "w+");
    if (!f) {
        payload_exit(1);
        return 0;
    }

    fprintf(f, "game_app_info_result=0x%08x\ngame_app_id=0x%08x\ngame_title_id=%.14s\n",
            (uint32_t)app_info_result, appinfo.app_id,
            app_info_result == 0 ? appinfo.title_id : "UNKNOWN");
    fprintf(f, "reader_discovery_available=%u\nreader_discovery_queries=%u\n"
        "reader_discovery_open_calls=%u\nreader_discovery_opened=%u\n"
        "reader_discovery_closed=%u\nreader_discovery_close_errors=%u\n"
        "reader_discovery_disabled=%u\nreader_discovery_refresh_requested=%u\n"
        "reader_discovery_last_lookup_result=0x%08x\nreader_discovery_last_open_result=0x%08x\n",
        reader.discovery_available, reader.discovery_queries,
        reader.discovery_open_calls, reader.discovery_opened,
        reader.discovery_closed, reader.discovery_close_errors,
        reader.discovery_disabled, reader.discovery_refresh_requested,
        (uint32_t)reader.discovery_last_lookup_result,
        (uint32_t)reader.discovery_last_open_result);
    fprintf(f, "reader_discovery_probe_held=%u\nreader_discovery_probe_polls=%u\n"
        "reader_discovery_probe_waits=%u\nreader_discovery_probe_timeouts=%u\n"
        "reader_discovery_last_device_result=0x%08x\nreader_discovery_last_info_result=0x%08x\n"
        "reader_discovery_last_state_result=0x%08x\n",
        reader.discovery_probe_held, reader.discovery_probe_polls,
        reader.discovery_probe_waits, reader.discovery_probe_timeouts,
        (uint32_t)reader.discovery_last_device_result, (uint32_t)reader.discovery_last_info_result,
        (uint32_t)reader.discovery_last_state_result);

    fprintf(f,
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
        "reader_mode=%u\n"
        "reader_last_queued_result=0x%08x\n"
        "reader_queued_success_frames=%u\n"
        "reader_queued_empty_frames=%u\n"
        "reader_queued_error_frames=%u\n"
        "reader_state_fallback_frames=%u\n"
        "reader_buttons=0x%08x\n"
        "reader_sticks=%u,%u,%u,%u\n"
        "reader_triggers=%u,%u\n"
        "reader_timestamp=%llu\n"
        "reader_count=%u\n"
        "--- bridge telemetry ---\n"
        "bridge_identity_result=%d\nbridge_pid=%d\n"
        "bridge_args=0x%lx\nbridge_snapshot_result=%d\n"
        "bridge_ready=%d\nbridge_active=%u\nbridge_seq=%u\n"
        "bridge_pad_handle=0x%08x\nbridge_game_pad_index=%d\n"
        "bridge_last_caller_handle=0x%08x\nbridge_last_mismatched_handle=0x%08x\n"
        "bridge_handle_match_calls=%llu\nbridge_handle_mismatch_calls=%llu\n"
        "bridge_read_state_calls=%llu\nbridge_read_state_ext_calls=%llu\n"
        "bridge_read_calls=%llu\nbridge_read_ext_calls=%llu\n"
        "bridge_data_internal_calls=%llu\nbridge_controller_info_calls=%llu\n"
        "bridge_read_nonzero_returns=%llu\nbridge_read_zero_returns=%llu\n"
        "bridge_max_read_streak=%u\n"
        "bridge_published_packets=%llu\nbridge_direct_fallback_frames=%llu\n"
        "bridge_native_backing_calls=%llu\nbridge_native_backing_errors=%llu\n"
        "bridge_native_passthrough_frames=%llu\nbridge_native_connected_frames=%llu\n"
        "bridge_event_ring_head=%u\n",
        (int)supervisor_length, identity_result, reader_pid,
        (unsigned long)reader_args, reader_result,
        reader.ready, reader.stop, reader.last_result, reader.seq,
        (uint32_t)reader.pad_handle, reader.owner_pid,
        reader.owner_check_interval, reader.owner_miss_count,
        reader.owner_watchdog_exits, reader.close_pad_on_exit,
        reader.connected, reader.reader_mode,
        (uint32_t)reader.last_read_result,
        reader.read_success_frames, reader.read_empty_frames,
        reader.read_error_frames, reader.state_fallback_frames,
        reader.buttons,
        reader.left_x, reader.left_y,
        reader.right_x, reader.right_y,
        reader.left_trigger, reader.right_trigger,
        (unsigned long long)reader.timestamp, reader.count,
        bridge_identity_result, bridge_pid,
        (unsigned long)bridge_args, bridge_result,
        bridge.bridge_ready, bridge.active, bridge.seq,
        (uint32_t)bridge.pad_handle, bridge.game_pad_index,
        (uint32_t)bridge.last_caller_handle, (uint32_t)bridge.last_mismatched_handle,
        (unsigned long long)bridge.handle_match_calls,
        (unsigned long long)bridge.handle_mismatch_calls,
        (unsigned long long)bridge.read_state_calls,
        (unsigned long long)bridge.read_state_ext_calls,
        (unsigned long long)bridge.read_calls,
        (unsigned long long)bridge.read_ext_calls,
        (unsigned long long)bridge.data_internal_calls,
        (unsigned long long)bridge.controller_info_calls,
        (unsigned long long)bridge.read_nonzero_returns,
        (unsigned long long)bridge.read_zero_returns,
        bridge.max_read_streak,
        (unsigned long long)bridge.published_packets,
        (unsigned long long)bridge.direct_fallback_frames,
        (unsigned long long)bridge.native_backing_calls,
        (unsigned long long)bridge.native_backing_errors,
        (unsigned long long)bridge.native_passthrough_frames,
        (unsigned long long)bridge.native_connected_frames,
        bridge.event_ring_head);

    fprintf(f,"empty_read_diag_available=%u\nempty_read_buffer_restores=%llu\n"
        "empty_read_native_disconnected=%llu\n",
        bridge.empty_read_diag_available,
        (unsigned long long)bridge.empty_read_buffer_restores,
        (unsigned long long)bridge.empty_read_native_disconnected);

    /* Source ordinals are separate from game player indices. Packet evidence
     * uses the guarded reader, independently of the bulk health snapshot. */
    fprintf(f, "--- source packet snapshots ---\n");
    for (unsigned source_slot = 0; source_slot < POORDS4_MAX_SLOTS; ++source_slot) {
        ScePadData source_pad;
        uint32_t source_seq = 0;
        memset(&source_pad, 0, sizeof(source_pad));
        int packet_result = reader_result == 0
            ? wireless_ds4_remote_reader_read_slot(
                reader_pid, reader_args, source_slot,
                &source_pad, sizeof(source_pad), &source_seq)
            : -1;
        fprintf(f, "source_feed[%u]: packet_result=%d", source_slot, packet_result);
        if (packet_result == 0)
            fprintf(f,
                " seq=%u connected=%u count=%u timestamp=%llu buttons=0x%08x "
                "sticks=%u,%u,%u,%u triggers=%u,%u",
                source_seq, source_pad.connected, source_pad.count,
                (unsigned long long)source_pad.timestamp, source_pad.buttons,
                source_pad.leftStick.x, source_pad.leftStick.y,
                source_pad.rightStick.x, source_pad.rightStick.y,
                source_pad.analogButtons.l2, source_pad.analogButtons.r2);
        fputc('\n', f);
    }

    fprintf(f,"trigger_state_available=%u\ntrigger_state_calls=%llu\n"
        "trigger_state_overrides=%llu\ntrigger_state_native_result=0x%08x\n",
        bridge.trigger_state_available,(unsigned long long)bridge.trigger_state_calls,
        (unsigned long long)bridge.trigger_state_overrides,
        (uint32_t)bridge.trigger_state_native_result);
    fprintf(f,"trigger_effect_available=%u\ntrigger_effect_set_calls=%llu\n"
        "trigger_effect_set_accepts=%llu\ntrigger_effect_state_overrides=%llu\n"
        "trigger_effect_native_result=0x%08x\n",
        bridge.trigger_effect_available,(unsigned long long)bridge.trigger_effect_set_calls,
        (unsigned long long)bridge.trigger_effect_set_accepts,
        (unsigned long long)bridge.trigger_effect_state_overrides,
        (uint32_t)bridge.trigger_effect_native_result);
    fprintf(f,"trigger_effect_diag_available=%u\n",bridge.trigger_effect_diag_available);
    if(bridge.trigger_effect_diag_available){
        fprintf(f,"trigger_effect_compat_successes=%llu\ntrigger_effect_cached_unavailable=%llu\n"
            "trigger_effect_legacy_receiver_active=%u\n",
            (unsigned long long)bridge.trigger_effect_compat_successes,
            (unsigned long long)bridge.trigger_effect_cached_unavailable,
            bridge.trigger_effect_legacy_receiver_active);
        static const char *const reasons[9]={"accepted","session_inactive","unmatched_handle",
            "native_dualsense","simulated","invalid_input_pointer","input_lease_unavailable",
            "invalid_command","cache_contention"};
        fprintf(f,"trigger_effect_last_skip=%u\ntrigger_effect_last_slot=%d\n"
            "trigger_effect_last_call_handle=0x%08x\ntrigger_effect_last_mask=0x%08x\n"
            "trigger_effect_last_left_mode=0x%08x\ntrigger_effect_last_right_mode=0x%08x\n",
            bridge.trigger_effect_last_skip,bridge.trigger_effect_last_slot,
            (uint32_t)bridge.trigger_effect_last_call_handle,bridge.trigger_effect_last_mask,
            bridge.trigger_effect_last_left_mode,bridge.trigger_effect_last_right_mode);
        for(unsigned reason=0;reason<9;reason++)
            fprintf(f,"trigger_effect_route_%s=%llu\n",reasons[reason],
                (unsigned long long)bridge.trigger_effect_skip_counts[reason]);
    }
    fprintf(f,"device_class_available=%u\ndevice_class_calls=%llu\ndevice_class_errors=%llu\n"
        "device_class_matched_calls=%llu\ndevice_class_last_result=0x%08x\n"
        "device_class_last_handle=0x%08x\ndevice_class_last_slot=%d\ndevice_class_last_value=%d\n",
        bridge.device_class_available,(unsigned long long)bridge.device_class_calls,
        (unsigned long long)bridge.device_class_errors,(unsigned long long)bridge.device_class_matched_calls,
        (uint32_t)bridge.device_class_last_result,(uint32_t)bridge.device_class_last_handle,
        bridge.device_class_last_slot,bridge.device_class_last_value);
    fprintf(f,"client_binding_available=%u\nclient_bindings=%llu\n"
        "client_native_rejects=%llu\nclient_missing=%llu\n",
        bridge.client_binding_available, (unsigned long long)bridge.client_bindings,
        (unsigned long long)bridge.client_native_rejects, (unsigned long long)bridge.client_missing);
    fprintf(f,"consumer_samples=%llu\nconsumer_disconnected=%llu\n"
        "consumer_output=0x%lx\nconsumer_handle=0x%08x\nconsumer_result=%d\n"
        "consumer_connected=%u\nconsumer_count=%u\n",
        (unsigned long long)bridge.consumer_samples,
        (unsigned long long)bridge.consumer_disconnected,
        (unsigned long)bridge.consumer_output, (unsigned)bridge.consumer_handle,
        bridge.consumer_result, bridge.consumer_connected, bridge.consumer_count);
    fprintf(f,"vibration_available=%u\nvibration_calls=%llu\nvibration_mode_calls=%llu\n"
        "vibration_publications=%llu\nvibration_native_calls=%llu\nvibration_failures=%llu\n"
        "vibration_last_caller=0x%08x\nvibration_native_result=0x%08x\nvibration_last_mode=%d\n",
        bridge.vibration_available,(unsigned long long)bridge.vibration_calls,
        (unsigned long long)bridge.vibration_mode_calls,(unsigned long long)bridge.vibration_publications,
        (unsigned long long)bridge.vibration_native_calls,(unsigned long long)bridge.vibration_failures,
        (uint32_t)bridge.vibration_last_caller,(uint32_t)bridge.vibration_native_result,bridge.vibration_last_mode);
    fprintf(f, "--- observed handles ---\n");
    for (unsigned i = 0; i < 4u; ++i) {
        fprintf(f, "observed_handle[%u]=0x%08x (calls=%llu)\n",
                i, (uint32_t)bridge.observed_handles[i],
                (unsigned long long)bridge.observed_handle_calls[i]);
    }

    fprintf(f, "--- multi-controller slots ---\n");
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        const PoorDS4SlotStatus *sl = &bridge.slots[s];
        char role_buf[48];
        const char *role;
        if (sl->is_dualsense) {
            snprintf(role_buf, sizeof(role_buf), "Player %u (Native DualSense)", s + 1);
            role = role_buf;
        } else if (sl->is_simulated) {
            snprintf(role_buf, sizeof(role_buf), "Player %u (Simulated DS4)", s + 1);
            role = role_buf;
        } else if (sl->active || (int32_t)s == bridge.game_pad_index) {
            if (sl->user_matches && (int32_t)s != bridge.game_pad_index) {
                snprintf(role_buf, sizeof(role_buf), "Player %u Alias (Primary DS4)", s + 1);
            } else if ((int32_t)s != bridge.game_pad_index) {
                snprintf(role_buf, sizeof(role_buf), "Player %u (Physical DS4)", s + 1);
            } else {
                snprintf(role_buf, sizeof(role_buf), "Player %u (Primary DS4)", s + 1);
            }
            role = role_buf;
        } else {
            role = "Unassigned/Empty";
        }
        fprintf(f, "slot[%u]: role='%s' active=%u user_matches=%u handle=0x%08x dualsense=%u sim=%u seq=%u pkts=%u calls[state=%llu,read=%llu] fallback_frames=%llu passthrough=%llu\n",
                s, role, sl->active, sl->user_matches, (uint32_t)sl->pad_handle, sl->is_dualsense, sl->is_simulated,
                sl->seq, sl->packets, (unsigned long long)sl->read_state_calls, (unsigned long long)sl->read_calls,
                (unsigned long long)sl->direct_fallback_frames, (unsigned long long)sl->native_passthrough_frames);
    }

    fprintf(f, "--- last events (ring buffer) ---\n");
    fprintf(f, "Seq    | Timestamp (ms) | Delta | API Stub        | Handle     | Flags | Sticks (L/R)        | Triggers | Buttons\n");
    fprintf(f, "-------+----------------+-------+-----------------+------------+-------+---------------------+----------+-------------------------\n");

    uint32_t head = bridge.event_ring_head;
    uint32_t start_seq = (head >= POORDS4_GAME_BRIDGE_EVENT_RING_SIZE)
        ? (head - POORDS4_GAME_BRIDGE_EVENT_RING_SIZE) : 0u;
    uint32_t prev_time = 0;

    for (uint32_t s = start_seq; s < head; ++s) {
        uint32_t idx = s % POORDS4_GAME_BRIDGE_EVENT_RING_SIZE;
        const PoorDS4InputEvent *evt = &bridge.event_ring[idx];
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

        const char *stub_name = "unknown";
        switch (evt->stub_kind) {
        case POORDS4_EVT_KIND_READ_STATE: stub_name = "read_state"; break;
        case POORDS4_EVT_KIND_READ_STATE_EXT: stub_name = "read_state_ext"; break;
        case POORDS4_EVT_KIND_READ: stub_name = "read"; break;
        case POORDS4_EVT_KIND_READ_EXT: stub_name = "read_ext"; break;
        case POORDS4_EVT_KIND_DATA_INTERNAL: stub_name = "data_internal"; break;
        case POORDS4_EVT_KIND_CONTROLLER_INFO: stub_name = "controller_info"; break;
        case POORDS4_EVT_KIND_EXT_CONTROLLER_INFO: stub_name = "ext_info"; break;
        }

        char btn_str[128] = "NONE";
        if (evt->buttons != 0) {
            btn_str[0] = '\0';
            struct { uint32_t mask; const char *name; } map[] = {
                { 0x00000001u, "SELECT" }, { 0x00000002u, "L3" }, { 0x00000004u, "R3" },
                { 0x00000008u, "START" },  { 0x00000010u, "UP" }, { 0x00000020u, "RIGHT" },
                { 0x00000040u, "DOWN" },   { 0x00000080u, "LEFT" }, { 0x00000100u, "L2" },
                { 0x00000200u, "R2" },     { 0x00000400u, "L1" }, { 0x00000800u, "R1" },
                { 0x00001000u, "TRIANGLE"},{ 0x00002000u, "CIRCLE"}, { 0x00004000u, "CROSS" },
                { 0x00008000u, "SQUARE" }, { 0x00100000u, "TOUCHPAD" }
            };
            size_t written = 0;
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
                if (evt->buttons & map[i].mask) {
                    int n = snprintf(btn_str + written, sizeof(btn_str) - written, "%s%s",
                                     written > 0 ? "|" : "", map[i].name);
                    if (n > 0) written += (size_t)n;
                    if (written >= sizeof(btn_str) - 1u) break;
                }
            }
        }

        fprintf(f, "%-6u | %-14u | +%-4u | %-15s | 0x%08x | %-5s | (%3u,%3u)(%3u,%3u) | L2:%-3u R2:%-3u | %s\n",
                evt->seq, evt->timestamp_ms, delta,
                stub_name, (uint32_t)evt->handle, flag_str,
                evt->lx, evt->ly, evt->rx, evt->ry,
                evt->l2, evt->r2, btn_str);
    }

    fprintf(f, "--- supervisor ---\n%s", supervisor);
    int report_export_result = export_status_report(f);
    int report_file_result = fclose(f);
    if (report_file_result == 0)
        report_file_result = rename(STATUS_REPORT_TMP, STATUS_REPORT);
    /* Export this local snapshot directly, rather than requiring an FTP
     * deployment to read a possibly historical status file. Completion below
     * means this helper completed; the result fields establish snapshot validity. */
    printf("POORDS4_STATUS_BEGIN schema=2\n"
           "game_app_info_result=0x%08x\ngame_app_id=0x%08x\ngame_title_id=%.14s\n"
           "reader_snapshot_result=%d\nreader_owner_pid=%d\nreader_connected=%u\n"
           "reader_buttons=0x%08x\nreader_triggers=%u,%u\n"
           "bridge_snapshot_result=%d\nbridge_pid=%d\nbridge_ready=%d\nbridge_active=%u\n"
           "bridge_read_state_calls=%llu\nbridge_read_calls=%llu\n"
           "bridge_handle_mismatch_calls=%llu\n"
           "report_export_result=%d\nreport_file_result=%d\nPOORDS4_STATUS_END schema=2\n",
           (uint32_t)app_info_result, appinfo.app_id,
           app_info_result == 0 ? appinfo.title_id : "UNKNOWN",
           reader_result, reader.owner_pid, reader.connected, reader.buttons,
           reader.left_trigger, reader.right_trigger, bridge_result, bridge_pid,
           bridge.bridge_ready, bridge.active, (unsigned long long)bridge.read_state_calls,
           (unsigned long long)bridge.read_calls,
           (unsigned long long)bridge.handle_mismatch_calls,
           report_export_result, report_file_result);
    fflush(stdout);
    payload_exit(supervisor_length >= 0 && report_export_result == 0 &&
                 report_file_result == 0 ? 0 : 1);
    return 0;
}
