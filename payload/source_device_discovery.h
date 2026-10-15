/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#define POORDS4_DISCOVERY_MAGIC UINT64_C(0x4453344449534331)
#define POORDS4_DISCOVERY_PORTS 10u

/* Private reader tail, outside the unchanged input/output prefixes. The source
 * thread alone calls Sony and writes this state. */
typedef struct {
    uint64_t magic;
    intptr_t fp_gethandle, fp_open, fp_getdeviceid, fp_getdeviceinfo;
    int32_t users[POORDS4_MAX_USER_CANDIDATES];
    int32_t admitted_users[POORDS4_MAX_SLOTS], admitted_indices[POORDS4_MAX_SLOTS];
    uint32_t user_count, admitted_count, interval_frames;
    uint32_t countdown, cursor, pending;
    int32_t pending_user, pending_index, pending_device;
    volatile uint32_t queries, open_calls, opened, closed, identity_queries;
    volatile uint32_t close_errors, disabled, refresh_requested;
    volatile int32_t close_result, refresh_user, refresh_index, refresh_device;
    volatile int32_t last_lookup_result, last_open_result;
    volatile uint32_t refresh_vendor, refresh_product;
} PoorDS4SourceDiscovery;

/* Resolved source functions use the same six register arguments as bootstrap.
 * No local symbol, global, allocation or ptrace is called by the copied code. */
static __attribute__((always_inline)) inline void
poords4_source_discovery_poll(PoorDS4SourceDiscovery *d, RemotePadReaderArgs *a)
{
    if (d->magic != POORDS4_DISCOVERY_MAGIC || d->disabled ||
        __atomic_load_n(&d->refresh_requested, __ATOMIC_ACQUIRE) ||
        !d->interval_frames || !d->user_count ||
        d->user_count > POORDS4_MAX_USER_CANDIDATES ||
        d->admitted_count > POORDS4_MAX_SLOTS ||
        !d->fp_gethandle || !d->fp_open || !d->fp_getdeviceid ||
        !d->fp_getdeviceinfo || !a->fp_closepad || !a->fp_readstate)
        return;
    if (++d->countdown < d->interval_frames) return;
    d->countdown = 0;
    const uint32_t candidates = d->user_count * POORDS4_DISCOVERY_PORTS;
    if (d->cursor >= candidates) d->cursor = 0;
    int32_t user = d->pending ? d->pending_user :
        d->users[d->cursor / POORDS4_DISCOVERY_PORTS];
    int32_t index = d->pending ? d->pending_index :
        (int32_t)(d->cursor % POORDS4_DISCOVERY_PORTS);
    if (user < 0 || index < 0 || index >= (int32_t)POORDS4_DISCOVERY_PORTS) {
        d->pending = 0; d->cursor = (d->cursor + 1u) % candidates; return;
    }
    uint32_t admitted_pair = 0;
    for (unsigned slot = 0; slot < d->admitted_count; ++slot)
        admitted_pair |= user == d->admitted_users[slot] &&
            index == d->admitted_indices[slot];
    /* At capacity only a replacement of an admitted user/port can refresh.
     * Otherwise an ignored fifth controller would repeatedly restart all four. */
    if (d->admitted_count == POORDS4_MAX_SLOTS && !admitted_pair) {
        d->pending = 0; d->cursor = (d->cursor + 1u) % candidates; return;
    }
    typedef int32_t (*pad_call_t)(uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t, uintptr_t, uintptr_t);
    pad_call_t get_handle = (pad_call_t)d->fp_gethandle;
    pad_call_t open_pad = (pad_call_t)d->fp_open;
    pad_call_t get_device_id = (pad_call_t)d->fp_getdeviceid;
    pad_call_t get_device_info = (pad_call_t)d->fp_getdeviceinfo;
    int32_t (*close_pad)(int32_t) = (void *)a->fp_closepad;
    int32_t (*read_state)(int32_t, void *) = (void *)a->fp_readstate;
    int32_t (*read_events)(int32_t, void *, int32_t) = (void *)a->fp_read;
    d->queries++;
    int32_t handle = get_handle((uint32_t)user, 0, (uint32_t)index, 0, 0, 0);
    d->last_lookup_result = handle;
    uint32_t owned = 0;
    if (handle < 0) {
        d->open_calls++;
        handle = open_pad((uint32_t)user, 0, (uint32_t)index, 0, 0, 0);
        d->last_open_result = handle;
        owned = handle >= 0;
    }
    uint32_t known = handle >= 0 && handle == a->pad_handle;
    for (unsigned slot = 1; slot < POORDS4_MAX_SLOTS; ++slot)
        known |= handle >= 0 && handle == a->slot_handles[slot];
    /* An alias of an admitted handle is never an additional owned resource.
     * In particular, do not close the current primary after an Open alias. */
    if (handle < 0 || known) {
        d->pending = 0; d->cursor = (d->cursor + 1u) % candidates; return;
    }
    d->opened += owned;
    int32_t device = -1;
    volatile uint8_t info[256];
    for (unsigned byte = 0; byte < sizeof info; ++byte) info[byte] = 0;
    d->identity_queries++;
    int32_t id_result = get_device_id((uint32_t)handle, (uintptr_t)&device,
                                    0, 0, 0, 0);
    int32_t info_result = -1;
    if (id_result == 0 && device >= 0)
        info_result = get_device_info((uint32_t)device, (uintptr_t)info,
                                     0, 0, 0, 0);
    uint16_t vendor = (uint16_t)info[16] | ((uint16_t)info[17] << 8);
    uint16_t product = (uint16_t)info[18] | ((uint16_t)info[19] << 8);
    /* Positive Sony DS4 identity is required. Unknown product zero is not
     * evidence for interrupting a healthy game. Bootstrap retains its existing
     * API/private-table admission for other devices. */
    uint32_t ds4 = id_result == 0 && device >= 0 && info_result == 0 &&
        vendor == 0x054cu &&
        (product == 0x05c4u || product == 0x09ccu || product == 0x0ba0u);
    uint32_t connected = 0;
    if (ds4) {
        ScePadData sample;
        volatile uint8_t *bytes = (volatile uint8_t *)&sample;
        for (unsigned byte = 0; byte < sizeof sample; ++byte) bytes[byte] = 0;
        if (read_state(handle, &sample) == 0) connected = sample.connected != 0;
        if (!connected && read_events) {
            for (unsigned byte = 0; byte < sizeof sample; ++byte) bytes[byte] = 0;
            if (read_events(handle, &sample, 1) > 0)
                connected = sample.connected != 0;
        }
    }
    if (owned) {
        d->close_result = close_pad(handle);
        if (d->close_result != 0) {
            /* Ownership is uncertain. Keep existing input, stop probing and
             * refuse automatic replacement after shutdown. Do not retry this
             * handle or open more probes. */
            d->close_errors++; d->disabled = 1; d->pending = 0; return;
        }
        d->closed++;
    }
    if (ds4 && connected) {
        if (d->pending && d->pending_user == user &&
            d->pending_index == index && d->pending_device == device) {
            d->refresh_user = user; d->refresh_index = index;
            d->refresh_device = device;
            d->refresh_vendor = vendor; d->refresh_product = product;
            __atomic_store_n(&d->refresh_requested, 1, __ATOMIC_RELEASE);
            return;
        }
        if (d->pending) {
            d->pending = 0; d->cursor = (d->cursor + 1u) % candidates; return;
        }
        d->pending_user = user; d->pending_index = index;
        d->pending_device = device; d->pending = 1;
        return;
    }
    d->pending = 0; d->cursor = (d->cursor + 1u) % candidates;
}
