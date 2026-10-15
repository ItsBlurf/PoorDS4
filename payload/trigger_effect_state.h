/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Independent implementation of the public PS5 interface facts documented in
 * EFFECT-STATE-LEAD-20261014.md. No physical actuator transport is added. */
typedef struct {
    uint32_t mode;
    uint8_t reserved[4], data[48];
} PoorDS4TriggerCommand;
typedef struct {
    uint8_t mask, reserved[7];
    PoorDS4TriggerCommand commands[2];
} PoorDS4TriggerParam;
_Static_assert(sizeof(PoorDS4TriggerParam)==120,"PS5 trigger command ABI");
typedef struct {
    volatile uint32_t lock, sequence;
    uint32_t mask;
    PoorDS4TriggerCommand commands[2];
} PoorDS4TriggerCache;

static __attribute__((always_inline)) inline int
poords4_trigger_param_valid(const PoorDS4TriggerParam *p)
{
    if(p->mask & ~3u)return 0;
    for(unsigned side=0;side<2;side++) {
        if(!(p->mask & (1u<<side)))continue;
        const PoorDS4TriggerCommand *c=&p->commands[side];
        if(c->mode>6u)return 0;
        /* A weapon's release position must follow its start; otherwise an
         * untouched trigger could falsely report a complete press. */
        if(c->mode==2u && (c->data[0]>=c->data[1] || c->data[1]>9u))return 0;
    }
    return 1;
}

static __attribute__((always_inline)) inline int32_t
poords4_trigger_state(const PoorDS4TriggerCommand *c,uint8_t pressure)
{
    unsigned position=(unsigned)pressure*10u/256u;
    int pressed=pressure>=8u;
    if(c->mode==2u) {
        if(position>=c->data[1])return 5;
        return position>=c->data[0]?4:3;
    }
    if(c->mode==1u)return pressed && c->data[1] && position>=c->data[0]?2:1;
    if(c->mode==4u)return pressed && c->data[position]?2:1;
    if(c->mode==5u)return pressed && position>=c->data[0]?2:1;
    if(c->mode==3u)return pressed && c->data[1] && c->data[2] && position>=c->data[0]?7:6;
    if(c->mode==6u)return pressed && c->data[0] && c->data[1+position]?7:6;
    return 0;
}

static __attribute__((always_inline)) inline int
poords4_trigger_publish(PoorDS4TriggerCache *cache,const PoorDS4TriggerParam *p)
{
    unsigned acquired=0;
    for(unsigned attempt=0;attempt<8;attempt++) {
        uint32_t unlocked=0;
        if(__atomic_compare_exchange_n(&cache->lock,&unlocked,1,0,
                                      __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            acquired=1;break;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    if(!acquired)return -1;
    uint32_t even=__atomic_load_n(&cache->sequence,__ATOMIC_RELAXED)&~1u;
    __atomic_store_n(&cache->sequence,even+1u,__ATOMIC_RELEASE);
    for(unsigned side=0;side<2;side++) {
        if(!(p->mask&(1u<<side)))continue;
        volatile uint8_t *dst=(volatile uint8_t*)&cache->commands[side];
        const volatile uint8_t *src=(const volatile uint8_t*)&p->commands[side];
        for(unsigned byte=0;byte<sizeof(PoorDS4TriggerCommand);byte++)
            __atomic_store_n(&dst[byte],src[byte],__ATOMIC_RELAXED);
    }
    __atomic_store_n(&cache->mask,__atomic_load_n(&cache->mask,__ATOMIC_RELAXED)|p->mask,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cache->sequence,even+2u,__ATOMIC_RELEASE);
    __atomic_store_n(&cache->lock,0,__ATOMIC_RELEASE);
    return 0;
}

static __attribute__((always_inline)) inline int
poords4_trigger_snapshot(PoorDS4TriggerCache *cache,PoorDS4TriggerParam *p)
{
    for(unsigned attempt=0;attempt<3;attempt++) {
        uint32_t before=__atomic_load_n(&cache->sequence,__ATOMIC_ACQUIRE);
        if(!before || (before&1u))continue;
        p->mask=(uint8_t)__atomic_load_n(&cache->mask,__ATOMIC_RELAXED);
        for(unsigned side=0;side<2;side++) {
            const volatile uint8_t *src=(const volatile uint8_t*)&cache->commands[side];
            uint8_t *dst=(uint8_t*)&p->commands[side];
            for(unsigned byte=0;byte<sizeof(PoorDS4TriggerCommand);byte++)
                dst[byte]=__atomic_load_n(&src[byte],__ATOMIC_RELAXED);
        }
        if(before==__atomic_load_n(&cache->sequence,__ATOMIC_ACQUIRE))return 1;
    }
    return 0;
}
