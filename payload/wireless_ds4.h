/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <sys/types.h>
#include "pad_feedback.h"
#include "version.h"

#define POORDS4_MAX_USER_CANDIDATES 6u
/* Positive PS4 title identification: leave its controller path native. */
#define POORDS4_GAME_BRIDGE_NATIVE_PS4 (-5)
#define POORDS4_GAME_BRIDGE_IMAGE_CHANGED (-6)
/* Current game's kernel exec/exit notification: 0 unchanged/unavailable,
 * 1 image replaced, 2 exited, -1 an established observation failed. */
int wireless_ds4_game_bridge_image_state(pid_t game_pid);

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
/* Read a secondary physical DS4 through the unambiguous game/user mapping. */
int wireless_ds4_remote_reader_read_game_slot(pid_t pid, intptr_t args_address,
                                             unsigned slot, void *pad_data,
                                             uint32_t pad_data_len, uint32_t *out_seq);
/* Refresh supervisor-local routes after reader recovery. Zero preserves the
 * installed bridge; other results request its existing normal reinstall path. */
int wireless_ds4_game_bridge_rebind_readers(pid_t pid, intptr_t args_address,
                                           const PoorDS4PadSource *source);

/* Physical reader output. Native acknowledgment is separate from UDP
 * acceptance. The current supervisor alone owns the connected sender socket. */
typedef struct {
    uint64_t session, epoch, published, observed, applied, now;
    int32_t handle, user, mode_result, command_result, stop_result;
    int32_t identity_result,device_info_result;
    uint16_t device_vendor,device_product;
    uint32_t identity_path;
    uint32_t connected, effect_active, quarantined, rejection;
} PoorDS4RumbleStatus;
/* -2: no coherent snapshot this time (no command attempted); other failures
 * indicate invalid/stopped/foreign reader state or unavailable ownership. */
int wireless_ds4_reader_rumble_status(pid_t pid,intptr_t args,PoorDS4RumbleStatus *out);
int wireless_ds4_reader_rumble_status_slot(pid_t pid,intptr_t args,unsigned slot,
                                          PoorDS4RumbleStatus *out);
int wireless_ds4_reader_rumble_submit(pid_t pid,intptr_t args,uint8_t large,
                                     uint8_t small,const PoorDS4RumbleStatus *binding,
                                     uint64_t *sequence);
int wireless_ds4_reader_rumble_submit_slot(pid_t pid,intptr_t args,unsigned slot,
    uint8_t large,uint8_t small,const PoorDS4RumbleStatus *binding,uint64_t *sequence);
/* Current supervisor-local secondary input attribution; -1 is unassigned.
 * The caller already knows the validated logical primary maps to reader zero. */
int wireless_ds4_game_bridge_reader_slot(unsigned game_slot);
int wireless_ds4_game_rumble_route(pid_t pid,intptr_t args,unsigned slot,
    uint64_t generation,const PoorDS4RumbleStatus *source,int enabled);
int wireless_ds4_game_rumble_request(pid_t pid,intptr_t args,unsigned slot,
    PoorDS4GameOutputCommand *command,uint64_t *published);
#if defined(POORDS4_GAME_RUMBLE_QUALIFY)
int wireless_ds4_game_rumble_native_qualification(pid_t reader_pid,intptr_t reader_args);
#endif

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
    uint32_t discovery_available, discovery_queries, discovery_open_calls;
    uint32_t discovery_opened, discovery_closed, discovery_close_errors;
    uint32_t discovery_disabled, discovery_refresh_requested;
    int32_t discovery_user, discovery_index;
    int32_t discovery_last_lookup_result, discovery_last_open_result;
} PoorDS4RemoteReaderStatus;

int wireless_ds4_remote_reader_status(
    pid_t pid, intptr_t args_address,
    PoorDS4RemoteReaderStatus *out_status);

#define POORDS4_MAX_SLOTS 4u

int wireless_ds4_game_bridge_install(
    const PoorDS4PadSource *source, pid_t *out_game_pid,
    intptr_t *out_args_address);
int wireless_ds4_game_bridge_find_target(pid_t *out_game_pid);
/* Publish a recovery/grace neutral to the installed physical primary while
 * retaining its last connected packet's count/timestamp. Fresh source input
 * uses update_slot below and keeps the source's complete packet metadata. */
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
    uint32_t empty_read_diag_available;
    uint64_t empty_read_buffer_restores, empty_read_native_disconnected;
    uint64_t read_zero_returns;
    uint64_t read_nonzero_returns;
    uint32_t max_read_streak;
    int32_t observed_handles[4];
    uint64_t observed_handle_calls[4];
    uint32_t event_ring_head;
    PoorDS4InputEvent event_ring[POORDS4_GAME_BRIDGE_EVENT_RING_SIZE];
    PoorDS4SlotStatus slots[POORDS4_MAX_SLOTS];
    uint64_t trigger_state_calls, trigger_state_overrides;
    int32_t trigger_state_native_result;
    uint32_t trigger_state_available;
    uint64_t trigger_effect_set_calls, trigger_effect_set_accepts, trigger_effect_state_overrides;
    int32_t trigger_effect_native_result;
    uint32_t trigger_effect_available;
    uint32_t trigger_effect_diag_available, trigger_effect_last_skip;
    int32_t trigger_effect_last_slot, trigger_effect_last_call_handle;
    uint32_t trigger_effect_last_mask, trigger_effect_last_left_mode, trigger_effect_last_right_mode;
    uint64_t trigger_effect_skip_counts[9];
    uint64_t trigger_effect_compat_successes, trigger_effect_cached_unavailable;
    uint32_t trigger_effect_legacy_receiver_active;
    uint64_t vibration_calls,vibration_mode_calls,vibration_publications,vibration_native_calls,vibration_failures;
    int32_t vibration_last_caller,vibration_native_result,vibration_last_mode;
    uint32_t vibration_available;
    uint32_t device_class_available;
    uint64_t device_class_calls, device_class_errors, device_class_matched_calls;
    int32_t device_class_last_result, device_class_last_handle;
    int32_t device_class_last_slot, device_class_last_value;
    uint32_t client_binding_available;
    uint64_t client_bindings, client_native_rejects, client_missing;
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
