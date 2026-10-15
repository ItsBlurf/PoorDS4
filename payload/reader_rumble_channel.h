/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pad_feedback.h"
#include "source_device_discovery.h"

/* Appended after the unchanged 1440-byte input prefix. Connected loopback UDP
 * delivers commands without running writes/ptrace into the source. Only the
 * reader changes feedback state and invokes Sony. Input stays independent. */
typedef struct {
    RemotePadReaderArgs input;
    PoorDS4ReaderOutput output;
    int32_t socket_fd;
    uint32_t rejected_packets;
    intptr_t fp_recvfrom, fp_socket_close;
    /* Only secondary handles opened by this bootstrap may be closed on exit.
     * Primary ownership remains the unchanged input.close_pad_on_exit bool. */
    uint32_t owned_secondary_pad_slots;
    PoorDS4SourceDiscovery discovery;
} RemotePadReaderTransport;
_Static_assert(offsetof(RemotePadReaderTransport,output)==1440,
               "Input reader prefix changed");

static __attribute__((always_inline)) inline void
reader_rumble_receive(RemotePadReaderTransport *transport)
{
    if(transport->socket_fd<0 || !poords4_reader_output_valid(&transport->output))return;
    intptr_t (*receive)(int,void*,size_t,int,void*,void*)=
        (void*)transport->fp_recvfrom;
    if(!poords4_output_pointer((intptr_t)receive))return;
    for(unsigned attempt=0;attempt<4;attempt++) {
        uint8_t bytes[sizeof(PoorDS4OutputCommand)+1];
        intptr_t length=receive(transport->socket_fd,bytes,sizeof bytes,MSG_DONTWAIT,0,0);
        if(length<0)break;
        if(length!=(intptr_t)sizeof(PoorDS4OutputCommand)) {
            transport->rejected_packets++;continue;
        }
        PoorDS4OutputCommand command;
        volatile const uint8_t *source=bytes;
        uint8_t *destination=(uint8_t*)&command;
        for(unsigned byte=0;byte<sizeof command;byte++)destination[byte]=source[byte];
        PoorDS4ReaderOutputSlot *channel=0;
        unsigned matches=0;
        for(unsigned slot=0;slot<4;slot++) {
            PoorDS4ReaderOutputSlot *candidate=&transport->output.channels[slot];
            if(command.handle>=0 && command.user>=0 &&
               command.handle==candidate->state.handle && command.user==candidate->state.user) {
                channel=candidate;matches++;
            }
        }
        if(matches!=1){transport->rejected_packets++;continue;}
        uint64_t previous=__atomic_load_n(&channel->published,__ATOMIC_RELAXED);
        if(command.session!=transport->output.session || !command.sequence ||
           previous==UINT64_MAX || command.sequence!=previous+1 ||
           command.handle!=channel->state.handle || command.user!=channel->state.user) {
            transport->rejected_packets++;continue;
        }
        volatile uint8_t *record=(volatile uint8_t*)&channel->commands[command.sequence&1u];
        const uint8_t *input=(const uint8_t*)&command;
        for(unsigned byte=0;byte<sizeof command;byte++)record[byte]=input[byte];
        __atomic_store_n(&channel->published,command.sequence,__ATOMIC_RELEASE);
    }
}

static __attribute__((always_inline)) inline void
reader_rumble_service(RemotePadReaderTransport *transport,int stop)
{
    if(!stop)reader_rumble_receive(transport);
    for(unsigned slot=0;slot<4;slot++) {
        if(transport->output.channels[slot].state.handle<0 ||
           transport->output.channels[slot].state.user<0)continue;
        poords4_reader_output_service(&transport->output,slot,
            slot?transport->input.slot_handles[slot]:transport->input.pad_handle,
            (const ScePadData*)(const void*)(slot?transport->input.slot_data[slot]:transport->input.pad_data),
            slot?transport->input.slot_results[slot]:transport->input.last_result,stop);
    }
    if(stop && transport->socket_fd>=0) {
        int (*close_socket)(int)=(void*)transport->fp_socket_close;
        if(poords4_output_pointer((intptr_t)close_socket))close_socket(transport->socket_fd);
        transport->socket_fd=-1;
    }
}
