/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "wireless_ds4.h"
#include <time.h>
#include <unistd.h>

/* Each attributed physical feed owns its own command/ACK sequence.
 * Publication != native ACK. Reader and game slot ordinals can differ. */
typedef struct {
    pid_t reader_pid,game_pid;
    intptr_t reader_args,game_args;
    unsigned slot,source_slot,enabled,failed,desired,pending,waiting;
    uint64_t generation,session,epoch,last_request,sent,refresh_at;
    uint64_t requests,submissions,acknowledgments,errors;
    uint64_t rebindings,cancellations,stale_requests,status_waits;
    uint8_t large,small;
} PoorDS4GameRumbleRelay;

static inline void
poords4_game_rumble_open_slot(PoorDS4GameRumbleRelay *relay,pid_t reader_pid,intptr_t reader_args,
    pid_t game_pid,intptr_t game_args,unsigned slot,unsigned source_slot,uint64_t generation)
{
    *relay=(PoorDS4GameRumbleRelay){.reader_pid=reader_pid,.reader_args=reader_args,
        .game_pid=game_pid,.game_args=game_args,.slot=slot,.source_slot=source_slot,.generation=generation};
    PoorDS4RumbleStatus source;
    if(!generation)return;
    int snapshot_result=wireless_ds4_reader_rumble_status_slot(reader_pid,reader_args,source_slot,&source);
    if(snapshot_result==-2){relay->waiting=1;relay->status_waits++;return;}
    if(snapshot_result || !source.session || source.quarantined)return;
    relay->session=source.session;relay->epoch=source.epoch;
    if(!source.connected){relay->waiting=1;return;}
    if(wireless_ds4_game_rumble_route(game_pid,game_args,slot,generation,&source,1))return;
    relay->enabled=1;
}

static inline void
poords4_game_rumble_open(PoorDS4GameRumbleRelay *relay,pid_t reader_pid,intptr_t reader_args,
    pid_t game_pid,intptr_t game_args,unsigned slot,uint64_t generation)
{
    poords4_game_rumble_open_slot(relay,reader_pid,reader_args,game_pid,game_args,slot,0,generation);
}

static inline void
poords4_game_rumble_fail(PoorDS4GameRumbleRelay *relay,int game_alive)
{
    if(!relay->failed)relay->errors++;
    relay->failed=1;relay->desired=0;relay->waiting=0;
    if(relay->enabled && game_alive)
        (void)wireless_ds4_game_rumble_route(relay->game_pid,relay->game_args,
            relay->slot,relay->generation,0,0);
    relay->enabled=0;
}

static inline void
poords4_game_rumble_tick(PoorDS4GameRumbleRelay *relay,uint64_t now,int game_alive,int stop)
{
    if(!relay->enabled && !relay->pending && !relay->failed && !relay->waiting)return;
    if(stop || !game_alive) {
        if(relay->enabled && game_alive)
            (void)wireless_ds4_game_rumble_route(relay->game_pid,relay->game_args,
                relay->slot,relay->generation,0,0);
        relay->enabled=0;relay->desired=0;relay->failed=1;relay->waiting=0;
    }
    PoorDS4RumbleStatus source;
    int snapshot_result=wireless_ds4_reader_rumble_status_slot(relay->reader_pid,relay->reader_args,relay->source_slot,&source);
    if(snapshot_result==-2 && !relay->failed && game_alive && !stop) {
        /* Observation did not attempt a command. Retain an outstanding ACK,
         * drop old desire and wait for the source's existing lease to become
         * neutral before restoring a fresh route generation. */
        if(relay->enabled && wireless_ds4_game_rumble_route(relay->game_pid,
                relay->game_args,relay->slot,relay->generation,0,0)) {
            poords4_game_rumble_fail(relay,game_alive);return;
        }
        relay->enabled=0;relay->desired=0;relay->waiting=1;relay->status_waits++;
        return;
    }
    if(snapshot_result) {
        poords4_game_rumble_fail(relay,game_alive);return;
    }
    /* Only an initially unobserved route can acquire its first session here. */
    if(!relay->session && relay->waiting && !relay->failed && !relay->pending)
        relay->session=source.session;
    if(source.session!=relay->session || source.quarantined) {
        poords4_game_rumble_fail(relay,game_alive);return;
    }
    /* A benign physical connection/count change suspends output, rather than
     * failing the game session. The reader owns neutralization and epoch.
     * Genuine failures/uncertain sends are never cleared by a new connection. */
    if(source.epoch!=relay->epoch || !source.connected) {
        if(relay->failed)return;
        if(relay->enabled && wireless_ds4_game_rumble_route(relay->game_pid,
            relay->game_args,relay->slot,relay->generation,0,0)) {
            poords4_game_rumble_fail(relay,game_alive);return;
        }
        relay->enabled=0;relay->desired=0;relay->waiting=1;
    }
    if(relay->pending) {
        if(source.observed<relay->sent)return;
        relay->pending=0;
        if(source.applied==relay->sent && !source.mode_result && !source.command_result &&
           !source.rejection)relay->acknowledgments++;
        else if(relay->waiting && source.epoch!=relay->epoch && !source.effect_active &&
                source.rejection==1)relay->cancellations++;
        else poords4_game_rumble_fail(relay,game_alive);
    }
    if(relay->waiting) {
        if(!source.connected || source.effect_active || source.observed<source.published)return;
        PoorDS4GameOutputCommand discarded;uint64_t published=0;
        if(relay->generation==UINT64_MAX ||
           wireless_ds4_game_rumble_request(relay->game_pid,relay->game_args,
               relay->slot,&discarded,&published)) {
            poords4_game_rumble_fail(relay,game_alive);return;
        }
        relay->generation++;
        if(wireless_ds4_game_rumble_route(relay->game_pid,relay->game_args,
                relay->slot,relay->generation,&source,1)) {
            poords4_game_rumble_fail(relay,game_alive);return;
        }
        relay->epoch=source.epoch;relay->last_request=published;relay->refresh_at=0;
        relay->waiting=0;relay->enabled=1;relay->rebindings++;
        return; /* A new publication, never the old desired motors, is required. */
    }
    if(relay->enabled) {
        PoorDS4GameOutputCommand command;uint64_t published=0;
        if(wireless_ds4_game_rumble_request(relay->game_pid,relay->game_args,
                relay->slot,&command,&published)) {
            poords4_game_rumble_fail(relay,game_alive);
        } else if(published>relay->last_request) {
            relay->last_request=published;
            /* Sample after reading the concurrently written game record. */
            if(poords4_output_time((intptr_t)clock_gettime,&now)) {
                poords4_game_rumble_fail(relay,game_alive);return;
            }
            if(command.generation<relay->generation && command.session==source.session) {
                /* A callback already in flight during disable can publish late. */
                relay->stale_requests++;
            } else if(command.generation!=relay->generation || command.session!=source.session ||
               command.epoch!=source.epoch || command.handle!=source.handle ||
               command.user!=source.user || command.created_at>now ||
               now-command.created_at>POORDS4_OUTPUT_TIMEOUT_NS) {
                poords4_game_rumble_fail(relay,game_alive);
            } else {
                relay->requests++;relay->large=command.vibration.large_motor;
                relay->small=command.vibration.small_motor;relay->desired=1;
                relay->refresh_at=0;
            }
        }
    }
    int stopping=relay->failed && source.effect_active;
    if(stopping || (relay->desired && (!relay->refresh_at || now>=relay->refresh_at))) {
        uint8_t large=stopping?0:relay->large,small=stopping?0:relay->small;
        /* Record uncertainty before send: never replay an unacknowledged seq. */
        relay->sent=source.published+1;relay->pending=1;relay->submissions++;
        uint64_t sequence=0;
        int result=wireless_ds4_reader_rumble_submit_slot(relay->reader_pid,relay->reader_args,relay->source_slot,
                large,small,&source,&sequence);
        if(result==-4 || result==-2) {
            relay->pending=0;relay->submissions--;relay->desired=0;
            if(relay->enabled && wireless_ds4_game_rumble_route(relay->game_pid,
                relay->game_args,relay->slot,relay->generation,0,0)) {
                poords4_game_rumble_fail(relay,game_alive);return;
            }
            relay->enabled=0;relay->waiting=1;
            if(result==-2)relay->status_waits++;
            return;
        }
        if(result || sequence!=relay->sent) {
            poords4_game_rumble_fail(relay,game_alive);return;
        }
        relay->refresh_at=now>UINT64_MAX-UINT64_C(250000000)?UINT64_MAX:now+UINT64_C(250000000);
        if(!large && !small)relay->desired=0;
    }
}

static inline int
poords4_game_rumble_close(PoorDS4GameRumbleRelay *relay,int game_alive)
{
    if(!relay->enabled && !relay->pending && !relay->failed && !relay->waiting)return 0;
    for(unsigned attempt=0;attempt<50;attempt++) {
        uint64_t now=0;
        if(poords4_output_time((intptr_t)clock_gettime,&now))return -1;
        poords4_game_rumble_tick(relay,now,game_alive,1);
        PoorDS4RumbleStatus source;
        if(!wireless_ds4_reader_rumble_status_slot(relay->reader_pid,relay->reader_args,relay->source_slot,&source) &&
           !source.effect_active && !relay->pending)return source.quarantined?-1:0;
        usleep(10000);
    }
    return -1;
}
