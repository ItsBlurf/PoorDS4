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
int wireless_ds4_remote_reader_read_slot(pid_t pid, intptr_t args_address,
                                        unsigned slot,
                                        void *pad_data, uint32_t pad_data_len,
                                        uint32_t *out_seq);
int wireless_ds4_remote_reader_stop(pid_t pid, intptr_t args_address);
/* The reader slot (1..3) of the secondary DS4 signed in as user_id, or -1. */
int wireless_ds4_remote_reader_slot_for_user(int32_t user_id);

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
    int32_t last_read_result;
    uint32_t read_success_frames;
    uint32_t read_empty_frames;
    uint32_t read_error_frames;
    uint32_t state_fallback_frames;
    uint32_t reader_mode;
    uint32_t buttons;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
    uint8_t left_trigger;
    uint8_t right_trigger;
    uint8_t count;
    uint8_t reserved0;
    uint64_t timestamp;
    uint32_t reset_combo_ticks;
    uint32_t reset_requested;
} PoorDS4RemoteReaderStatus;

int wireless_ds4_remote_reader_status(
    pid_t pid, intptr_t args_address,
    PoorDS4RemoteReaderStatus *out_status);

#define POORDS4_MAX_SLOTS 4u

int wireless_ds4_game_bridge_install(
    const PoorDS4PadSource *source, pid_t *out_game_pid,
    intptr_t *out_args_address);
int wireless_ds4_game_bridge_find_target(pid_t *out_game_pid);
int wireless_ds4_game_bridge_update(pid_t game_pid, intptr_t args_address,
                                   const void *pad_data,
                                   uint32_t pad_data_len);
int wireless_ds4_game_bridge_update_slot(pid_t game_pid, intptr_t args_address,
                                        uint32_t slot_idx,
                                        const void *pad_data,
                                        uint32_t pad_data_len,
                                        int is_simulated,
                                        int is_dualsense);
int wireless_ds4_game_bridge_deactivate_slot(pid_t game_pid, intptr_t args_address,
                                            uint32_t slot_idx);
int wireless_ds4_game_bridge_abandon(void);

#define POORDS4_GAME_BRIDGE_EVENT_RING_SIZE 64u

#define POORDS4_EVT_KIND_READ_STATE      0u
#define POORDS4_EVT_KIND_READ_STATE_EXT  1u
#define POORDS4_EVT_KIND_READ            2u
#define POORDS4_EVT_KIND_READ_EXT        3u
#define POORDS4_EVT_KIND_DATA_INTERNAL   4u
#define POORDS4_EVT_KIND_CONTROLLER_INFO 5u
#define POORDS4_EVT_KIND_EXT_CONTROLLER_INFO 6u

#define POORDS4_EVT_FLAG_DIRECT   0x01u
#define POORDS4_EVT_FLAG_NATIVE   0x02u
#define POORDS4_EVT_FLAG_CHANGED  0x04u
#define POORDS4_EVT_FLAG_MISMATCH 0x08u

typedef struct {
    uint32_t seq;
    uint32_t timestamp_ms;
    int32_t handle;
    uint32_t buttons;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t l2;
    uint8_t r2;
    uint8_t stub_kind;
    uint8_t flags;
} PoorDS4InputEvent;

typedef struct {
    int32_t  pad_handle;
    int32_t  pad_index;
    uint32_t active;
    uint32_t is_dualsense;
    uint32_t is_simulated;
    uint32_t seq;
    uint32_t packets;
    uint64_t read_state_calls;
    uint64_t read_calls;
    uint64_t direct_fallback_frames;
    uint64_t native_passthrough_frames;
    uint32_t buttons;
    uint8_t  lx;
    uint8_t  ly;
    uint8_t  rx;
    uint8_t  ry;
    uint8_t  l2;
    uint8_t  r2;
    uint8_t  connected;
    uint8_t  user_matches;
    uint8_t  reserved[2];
    int32_t  user_id;
} PoorDS4SlotStatus;

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
    uint64_t native_passthrough_frames;
    uint64_t native_connected_frames;
    uint64_t direct_fallback_frames;
    uint64_t direct_active_fallbacks;
    int32_t last_native_result;
    uint64_t native_success_frames;
    uint64_t native_input_activity_frames;
    uint64_t last_native_timestamp;
    uint32_t last_native_buttons;
    uint8_t last_native_lx;
    uint8_t last_native_ly;
    uint8_t last_native_rx;
    uint8_t last_native_ry;
    uint8_t last_native_l2;
    uint8_t last_native_r2;
    uint8_t last_native_connected;
    uint8_t last_native_count;
    uint32_t import_hook_count;
    int32_t game_pad_index;
    uint32_t buttons;
    uint8_t connected;
    uint32_t reset_combo_ticks;
    uint32_t reset_requested;
    int32_t last_caller_handle;
    int32_t last_mismatched_handle;
    uint64_t handle_match_calls;
    uint64_t handle_mismatch_calls;
    uint64_t read_zero_returns;
    uint64_t read_nonzero_returns;
    uint32_t max_read_streak;
    int32_t observed_handles[4];
    uint64_t observed_handle_calls[4];
    uint32_t event_ring_head;
    PoorDS4InputEvent event_ring[POORDS4_GAME_BRIDGE_EVENT_RING_SIZE];
    PoorDS4SlotStatus slots[POORDS4_MAX_SLOTS];
} PoorDS4GameBridgeStatus;

int wireless_ds4_game_bridge_status(pid_t game_pid, intptr_t args_address,
                                   PoorDS4GameBridgeStatus *out_status);
int wireless_ds4_game_bridge_check_reset(pid_t game_pid,
                                        intptr_t args_address);
int wireless_ds4_game_bridge_remove(pid_t game_pid,
                                   intptr_t args_address);
/* Suspend-safe teardown: restore game imports without target syscalls,
 * ptrace, or unmapping memory that an in-flight pad call may still use. */
int wireless_ds4_game_bridge_quiesce(pid_t game_pid,
                                    intptr_t args_address);
