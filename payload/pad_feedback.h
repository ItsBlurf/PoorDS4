/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include "pad_types.h"

/* Source-side output extension. Input prefixes and queue offsets stay fixed.
 * One supervisor publishes commands; only the reader writes status. Native
 * success is distinct from publication. No motor/firmware acceptance implied. */
#define POORDS4_READER_OUTPUT_MAGIC UINT32_C(0x31524f44)
#define POORDS4_OUTPUT_TIMEOUT_NS UINT64_C(1000000000)
typedef struct { uint8_t large_motor, small_motor; } PoorDS4Vibration;
typedef struct {
    uint64_t session, epoch, sequence, deadline;
    int32_t handle, user;
    PoorDS4Vibration vibration;
    uint8_t reserved[6];
} PoorDS4OutputCommand;
typedef struct {
    volatile uint64_t sequence;
    uint64_t epoch, observed_sequence, applied_sequence, deadline, now;
    int32_t handle, user, native_result, command_result, mode_result, stop_result;
    int32_t identity_result, device_info_result;
    uint16_t device_vendor, device_product;
    uint32_t identity_path;
    uint32_t connected, count, mode_ready, effect_active, quarantined, rejection;
    uint32_t reserved; /* Bootstrap DS4 vendor/product for this physical feed. */
} PoorDS4OutputState;
typedef struct {
    PoorDS4OutputState state;
    volatile uint64_t published;
    PoorDS4OutputCommand commands[2];
} PoorDS4ReaderOutputSlot;
typedef struct {
    uint32_t magic, version, size, slots;
    uint64_t session;
    intptr_t fp_vibration, fp_mode, fp_clock, fp_is_ds4;
    intptr_t fp_device_info;
    uint16_t accepted_vendor, accepted_product;
    uint32_t reserved;
    PoorDS4ReaderOutputSlot channels[4];
} PoorDS4ReaderOutput;

#define POORDS4_GAME_OUTPUT_MAGIC UINT32_C(0x31474f44)
typedef struct {
    uint64_t generation, session, epoch, sequence, created_at;
    int32_t handle, user;
    PoorDS4Vibration vibration;
    uint8_t reserved[6];
} PoorDS4GameOutputCommand;
typedef struct {
    volatile uint32_t sequence;
    uint32_t enabled;
    uint64_t generation, session, epoch;
    int32_t user, handle;
} PoorDS4GameOutputRoute;
typedef struct {
    PoorDS4GameOutputRoute route;
    volatile uint32_t lock;
    uint32_t reserved;
    volatile uint64_t published;
    PoorDS4GameOutputCommand commands[2];
} PoorDS4GameOutputSlot;
typedef struct {
    uint32_t magic, version, size, slots;
    intptr_t fp_vibration, fp_mode, fp_clock;
    PoorDS4GameOutputSlot channels[4];
    volatile uint64_t vibration_calls,mode_calls,publications,native_calls,failed_publications;
    volatile int32_t last_caller,last_native_result,last_mode;
    uint32_t reserved;
} PoorDS4GameOutput;
_Static_assert(sizeof(PoorDS4Vibration)==2 && sizeof(PoorDS4OutputCommand)==48,
               "Legacy motor ABI changed");

static __attribute__((always_inline)) inline int
poords4_output_pointer(intptr_t pointer)
{ return pointer>=0x10000 && (uint64_t)pointer<UINT64_C(0x0000800000000000); }

static __attribute__((always_inline)) inline int
poords4_reader_output_valid(const PoorDS4ReaderOutput *output)
{
    return output && output->magic==POORDS4_READER_OUTPUT_MAGIC &&
        output->version==1 && output->size==sizeof(*output) && output->slots==4 &&
        output->session && poords4_output_pointer(output->fp_vibration) &&
        poords4_output_pointer(output->fp_mode) && poords4_output_pointer(output->fp_clock) &&
        poords4_output_pointer(output->fp_is_ds4);
}

static __attribute__((always_inline)) inline int
poords4_output_time(intptr_t pointer, uint64_t *now)
{
    struct { int64_t seconds, nanos; } time={0,0};
    int (*clock_fn)(int,void*)=(void*)pointer;
    if(!poords4_output_pointer(pointer) || clock_fn(4,&time) || time.seconds<0 ||
       time.nanos<0 || time.nanos>=INT64_C(1000000000) ||
       (uint64_t)time.seconds>(UINT64_MAX-(uint64_t)time.nanos)/UINT64_C(1000000000))return -1;
    *now=(uint64_t)time.seconds*UINT64_C(1000000000)+(uint64_t)time.nanos;
    return 0;
}

/* Same public identity admitted by RC51, freshly queried for the bound handle.
 * The narrow API can return zero or a positive handle-derived value. Public
 * fallback must match the source bootstrap identity; it never admits DS5/Xbox. */
static __attribute__((always_inline)) inline int
poords4_output_identity(PoorDS4ReaderOutput *output,PoorDS4OutputState *state)
{
    int32_t (*is_ds4)(int32_t)=(void*)output->fp_is_ds4;
    state->identity_result=is_ds4(state->handle);
    state->device_info_result=-1;state->identity_path=0;
    state->device_vendor=0;state->device_product=0;
    if(state->identity_result>0){state->identity_path=1;return 1;}
    /* Legacy primary headers remain readable. A secondary cannot borrow the
     * primary's accepted identity when its narrow API returns zero. */
    uint16_t vendor=state->reserved?(uint16_t)state->reserved:
        state==&output->channels[0].state?output->accepted_vendor:0;
    uint16_t product=state->reserved?(uint16_t)(state->reserved>>16):
        state==&output->channels[0].state?output->accepted_product:0;
    if(vendor!=UINT16_C(0x054c) ||
       (product!=UINT16_C(0x05c4) && product!=UINT16_C(0x09cc) &&
        product!=UINT16_C(0x0ba0) && product!=0) ||
       !poords4_output_pointer(output->fp_device_info))return 0;
    uint8_t info[256];
    for(unsigned byte=0;byte<sizeof info;byte++)((volatile uint8_t*)info)[byte]=0xa5;
    int32_t (*device_info)(int32_t,void*)=(void*)output->fp_device_info;
    state->device_info_result=device_info(state->handle,info);
    if(state->device_info_result)return 0;
    state->device_vendor=(uint16_t)(info[16]|((uint16_t)info[17]<<8));
    state->device_product=(uint16_t)(info[18]|((uint16_t)info[19]<<8));
    if(state->device_vendor!=vendor ||
       state->device_product!=product)return 0;
    state->identity_path=2;return 1;
}

/* A failed native stop is retained as uncertain output, not repeated blindly.
 * Quarantine forbids further nonzero output until a new reader installation. */
static __attribute__((always_inline)) inline void
poords4_output_stop(PoorDS4ReaderOutput *output, PoorDS4OutputState *state)
{
    if(!state->effect_active || state->quarantined)return;
    if(!poords4_output_identity(output,state)) {
        state->rejection=2;state->quarantined=1;return;
    }
    PoorDS4Vibration zero={0,0};
    int32_t (*vibrate)(int32_t,const void*)=(void*)output->fp_vibration;
    state->stop_result=vibrate(state->handle,&zero);
    state->native_result=state->stop_result;
    if(state->native_result)state->quarantined=1;
    else {state->effect_active=0;state->deadline=0;}
}

/* Called after the reader's stable input publication and once on exit. All
 * helpers inline into the copied reader; only resolved target calls escape. */
static __attribute__((always_inline)) inline void
poords4_reader_output_service(PoorDS4ReaderOutput *output, unsigned slot,
                             int32_t handle, const ScePadData *pad, int result, int stop)
{
    if(slot>=4 || !poords4_reader_output_valid(output))return;
    PoorDS4ReaderOutputSlot *channel=&output->channels[slot];
    PoorDS4OutputState *state=&channel->state;
    uint64_t status_seq=__atomic_load_n(&state->sequence,__ATOMIC_RELAXED);
    if(status_seq>=UINT64_MAX-2) {
        poords4_output_stop(output,state);state->quarantined=1;return;
    }
    __atomic_store_n(&state->sequence,status_seq+1,__ATOMIC_RELEASE);
    int configured=state->handle>=0 && state->user>=0 && state->handle==handle;
    for(unsigned other=0;other<4;other++)
        if(other!=slot && output->channels[other].state.handle==handle)configured=0;
    int connected=configured && !stop && result==0 && pad && pad->connected;
    uint32_t count=connected?pad->count:0;
    int changed=state->connected!=(uint32_t)connected || (connected && state->count!=count);
    uint64_t now=0;
    int clock_bad=poords4_output_time(output->fp_clock,&now);
    state->now=now;
    if(stop || !connected || changed || clock_bad ||
       (state->effect_active && now>=state->deadline))poords4_output_stop(output,state);
    if(changed) {
        if(state->epoch==UINT64_MAX)state->quarantined=1;
        else state->epoch++;
        state->mode_ready=0;
    }
    state->connected=(uint32_t)connected;state->count=count;
    uint64_t published=__atomic_load_n(&channel->published,__ATOMIC_ACQUIRE);
    if(published>state->observed_sequence) {
        const volatile PoorDS4OutputCommand *record=&channel->commands[published&1u];
        PoorDS4OutputCommand command;
        const volatile uint8_t *source=(const volatile uint8_t*)record;
        uint8_t *destination=(uint8_t*)&command;
        for(unsigned byte=0;byte<sizeof(command);byte++)destination[byte]=source[byte];
        if(published==__atomic_load_n(&channel->published,__ATOMIC_ACQUIRE) && command.sequence==published) {
            state->observed_sequence=published;
            state->rejection=0;
            if(stop || !connected || clock_bad || state->quarantined ||
               command.session!=output->session || command.epoch!=state->epoch ||
               command.handle!=state->handle || command.user!=state->user ||
               command.deadline<=now || command.deadline-now>POORDS4_OUTPUT_TIMEOUT_NS)state->rejection=1;
            else {
                if(!poords4_output_identity(output,state)) {state->rejection=2;poords4_output_stop(output,state);}
                else {
                    int32_t (*mode)(int32_t,int32_t)=(void*)output->fp_mode;
                    int32_t (*vibrate)(int32_t,const void*)=(void*)output->fp_vibration;
                    if(!state->mode_ready) {
                        state->native_result=mode(handle,2);
                        state->mode_result=state->native_result;
                        if(state->native_result)state->quarantined=1;
                        else state->mode_ready=1;
                    }
                    if(!state->quarantined) {
                        /* A failed call may have changed output: keep an
                         * uncertain effect owned until an explicit stop. */
                        state->effect_active=1;
                        state->native_result=vibrate(handle,&command.vibration);
                        state->command_result=state->native_result;
                        state->deadline=command.deadline;
                        if(state->native_result)poords4_output_stop(output,state);
                        else {
                            state->applied_sequence=published;
                            state->effect_active=command.vibration.large_motor || command.vibration.small_motor;
                        }
                    }
                }
            }
        }
    }
    __atomic_store_n(&state->sequence,status_seq+2,__ATOMIC_RELEASE);
}
