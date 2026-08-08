/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#define POORDS4_MAX_USER_CANDIDATES 6u

typedef struct {
    int32_t user_id;
    int32_t pad_index;
    int32_t pad_handle;
    int32_t ds4_connected;
} PoorDS4PadSource;

int wireless_ds4_remote_reader_start(
    const int32_t *user_ids, uint32_t user_count,
    PoorDS4PadSource *out_source, pid_t *out_pid,
    intptr_t *out_args_address);
int wireless_ds4_remote_reader_read(pid_t pid, intptr_t args_address,
                                   void *pad_data, uint32_t pad_data_len,
                                   uint32_t *out_seq);
int wireless_ds4_remote_reader_stop(pid_t pid, intptr_t args_address);

typedef struct {
    int32_t ready;
    int32_t stop;
    int32_t last_result;
    uint32_t seq;
    int32_t pad_handle;
    int32_t owner_pid;
    uint32_t owner_check_interval;
    uint32_t owner_miss_count;
    uint32_t owner_watchdog_exits;
    int32_t close_pad_on_exit;
    uint8_t connected;
} PoorDS4RemoteReaderStatus;

int wireless_ds4_remote_reader_status(
    pid_t pid, intptr_t args_address,
    PoorDS4RemoteReaderStatus *out_status);

int wireless_ds4_game_bridge_install(
    const PoorDS4PadSource *source, pid_t *out_game_pid,
    intptr_t *out_args_address);
int wireless_ds4_game_bridge_find_target(pid_t *out_game_pid);
int wireless_ds4_game_bridge_update(pid_t game_pid, intptr_t args_address,
                                   const void *pad_data,
                                   uint32_t pad_data_len);
int wireless_ds4_game_bridge_abandon(void);

typedef struct {
    uint32_t layout_marker;
    uint32_t active;
    uint32_t seq;
    int32_t pad_handle;
    int32_t bridge_ready;
    uint64_t published_packets;
    uint64_t lease_expirations;
    uint64_t read_state_calls;
    uint64_t read_state_ext_calls;
    uint64_t read_calls;
    uint64_t read_ext_calls;
    uint64_t data_internal_calls;
    uint64_t controller_info_calls;
    uint64_t controller_info_spoofs;
    uint64_t snapshot_contention_fallbacks;
    uint64_t controller_info_result_overrides;
    uint64_t native_backing_calls;
    uint64_t native_backing_errors;
    uint32_t import_hook_count;
    int32_t game_pad_index;
    uint32_t buttons;
    uint8_t connected;
} PoorDS4GameBridgeStatus;

int wireless_ds4_game_bridge_status(pid_t game_pid, intptr_t args_address,
                                   PoorDS4GameBridgeStatus *out_status);
int wireless_ds4_game_bridge_remove(pid_t game_pid,
                                   intptr_t args_address);
/* Suspend-safe teardown: restore game imports without target syscalls,
 * ptrace, or unmapping memory that an in-flight pad call may still use. */
int wireless_ds4_game_bridge_quiesce(pid_t game_pid,
                                    intptr_t args_address);
