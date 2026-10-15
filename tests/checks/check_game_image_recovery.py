# SPDX-License-Identifier: GPL-3.0-or-later
"""Production status, IO, session teardown and exec watch with explicit host mocks.

No process is executed on a console. Kqueue delivery is modeled; SDK/native
admission is separate evidence. Use --payload to reproduce the old failure.
"""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests'))
from fixture_support import function

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload', type=Path, default=ROOT/'payload')
opts = parser.parse_args()
p = opts.payload
source = (p/'wireless_ds4.c').read_text()
main = (p/'game_pad_bridge_main.c').read_text()
watch = ROOT/'projects/PoorDS4/payload/game_process_watch.h'
types = source[source.index('#define POORDS4_GAME_BRIDGE_MAGIC'):source.index('static pid_t g_game_bridge_direct_pid')]
enum_start = main.rfind('typedef enum {', 0, main.index('} GameSessionEndReason;'))
enum = main[enum_start:main.index('} GameSessionEndReason;')+len('} GameSessionEndReason;')]
relay_text = (p/'game_rumble_relay.h').read_text()
relay = relay_text[relay_text.index('typedef struct {'):relay_text.index('} PoorDS4GameRumbleRelay;')+len('} PoorDS4GameRumbleRelay;')]
session = function(main, 'run_game_session')
# Keep actual initialization, pre-IO loop admission, and entire actual final
# cleanup. The omitted middle publishes normal input; those callbacks already
# have independent tests. This test targets a pending exec before first IO.
session = session[:session.index('        ScePadData pad;')] + '        break;\n    }\n' + session[session.index('    if (g_shutdown_requested && end_reason'):]
marker = '        if (end_reason == SESSION_END_GAME_IMAGE_CHANGED && process_alive(game_pid)) {'
reacquire = ''
if marker in main:
    start = main.index(marker)
    brace = main.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (main[end] == '{') - (main[end] == '}')
        end += 1
    reacquire = main[start:end]

event_header = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
struct timespec;
struct kevent {uintptr_t ident;short filter;unsigned short flags;unsigned int fflags;intptr_t data;void *udata;};
#define EVFILT_PROC (-5)
#define EV_ADD 0x0001
#define EV_CLEAR 0x0020
#define EV_ERROR 0x4000
#define NOTE_EXIT 0x80000000U
#define NOTE_EXEC 0x20000000U
#define EV_SET(e,i,f,fl,ff,d,u) do{*(e)=(struct kevent){(i),(f),(fl),(ff),(d),(u)};}while(0)
int fake_kqueue(void);
int fake_kevent(int,const struct kevent*,int,struct kevent*,int,const struct timespec*);
'''
program = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define __PROSPERO__ 1
#define close fake_close
#define kqueue fake_kqueue
#define kevent fake_kevent
#include "wireless_ds4.h"
#include "pad_types.h"
#include "game_process_watch.h"
static int close_calls,open_fail,register_fail,poll_fail,poll_interrupt,error_event;
static uint32_t pending_event;
int fake_close(int fd){(void)fd;close_calls++;return 0;}
int fake_kqueue(void){if(open_fail){errno=ENOSYS;return -1;}return 47;}
int fake_kevent(int fd,const struct kevent *changes,int nchanges,struct kevent *events,int nevents,const struct timespec *timeout){
 (void)fd;(void)nevents;
 if(nchanges){if(register_fail){errno=EPERM;return -1;}
   if(changes->filter!=EVFILT_PROC || changes->ident!=94 || changes->fflags!=(NOTE_EXEC|NOTE_EXIT)){errno=EINVAL;return -1;}return 0;}
 if(!timeout || timeout->tv_sec || timeout->tv_nsec){errno=EINVAL;return -1;}
 if(poll_interrupt){poll_interrupt=0;errno=EINTR;return -1;}
 if(poll_fail){errno=EIO;return -1;}
 if(error_event){*events=(struct kevent){.flags=EV_ERROR,.data=EPERM};return 1;}
 if(!pending_event)return 0;
 *events=(struct kevent){.ident=94,.filter=EVFILT_PROC,.fflags=pending_event};pending_event=0;return 1;
}
'''+types+relay+enum+r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
#define POORDS4_PG_FRAME UINT64_C(0x000ffffffffff000)
static PoorDS4GameProcessWatch g_game_process_watch={.fd=-1,.pid=-1};
static pid_t g_game_bridge_direct_pid=-1;
static intptr_t g_game_bridge_direct_args;
static uint32_t g_game_bridge_direct_seq,g_game_bridge_direct_packets;
static uint64_t g_game_bridge_direct_cr3,g_game_bridge_direct_dmap;
static int paging_calls,physical_reads,physical_writes,remove_calls,quiesce_calls,update_calls,rumble_close_calls,copy_fail,seq_fail;
static GamePadBridgeArgs fixture;
static PoorDS4PadSource g_pad_source={11,0,44,1};
static intptr_t g_bridge_args;
static int g_shutdown_requested,g_skip_remote_cleanup;
static int sleep_calls;
static int process_alive(pid_t pid){return pid==94;}
static void poords4_log(const char *format,...){(void)format;}
static int game_bridge_get_process_paging(pid_t pid,intptr_t address,uint64_t *cr3,uint64_t *dmap){
 (void)pid;(void)address;paging_calls++;*cr3=0x1000;*dmap=0x2000;return 0;}
static int game_bridge_process_virt_to_phys(uint64_t address,uint64_t *physical,uint64_t *end){
 (void)address;*physical=0x1000;*end=0x2000;return 0;}
static int kernel_copyin(const void *from,intptr_t to,size_t size){(void)from;(void)to;(void)size;physical_writes++;return 0;}
static int kernel_copyout(intptr_t from,void *to,size_t size){(void)from;physical_reads++;memset(to,0,size);return 0;}
static int game_bridge_copy_args(pid_t pid,intptr_t address,GamePadBridgeArgs *out){
 (void)pid;(void)address;if(copy_fail)return -1;*out=fixture;return 0;}
static int remote_reader_copyout(pid_t pid,intptr_t address,void *out,size_t size){
 (void)pid;(void)address;if(seq_fail)return -1;memset(out,0,size);if(size==sizeof(uint32_t))memcpy(out,(const void*)&fixture.direct_seq,size);return 0;}
int wireless_ds4_game_bridge_remove(pid_t pid,intptr_t address){(void)pid;(void)address;remove_calls++;return 0;}
int wireless_ds4_game_bridge_quiesce(pid_t pid,intptr_t address){(void)pid;(void)address;quiesce_calls++;return 0;}
int wireless_ds4_game_bridge_update(pid_t pid,intptr_t address,const void *pad,uint32_t size){(void)pid;(void)address;(void)pad;(void)size;update_calls++;return 0;}
static int lifecycle_should_stop(void){return 0;}
static int sleep_interruptible(unsigned usec){(void)usec;sleep_calls++;return 0;}
static uint64_t monotonic_milliseconds(void){return 1000;}
static void write_supervisor_state(const char *name,...){(void)name;}
static void write_game_session_summary(pid_t pid,...){(void)pid;}
static void make_neutral_connected_pad(ScePadData *pad){memset(pad,0,sizeof *pad);pad->connected=1;}
static void poords4_game_rumble_open(PoorDS4GameRumbleRelay *r,pid_t rp,intptr_t ra,pid_t gp,intptr_t ga,unsigned slot,uint64_t session){
 (void)rp;(void)ra;(void)gp;(void)ga;(void)slot;(void)session;memset(r,0,sizeof *r);r->enabled=1;}
static int poords4_game_rumble_close(PoorDS4GameRumbleRelay *r,int alive){(void)r;(void)alive;rumble_close_calls++;return 0;}
static void poords4_game_rumble_open_slot(PoorDS4GameRumbleRelay *r,pid_t rp,intptr_t ra,pid_t gp,intptr_t ga,unsigned slot,unsigned source_slot,uint64_t generation){
 (void)source_slot;poords4_game_rumble_open(r,rp,ra,gp,ga,slot,generation);}
static void poords4_game_rumble_tick(PoorDS4GameRumbleRelay *r,uint64_t now,int alive,int stop){(void)r;(void)now;(void)alive;(void)stop;}
int wireless_ds4_game_bridge_reader_slot(unsigned slot){(void)slot;return -1;}
'''
if '\nwireless_ds4_game_bridge_image_state(' in source:
    program += 'int '+function(source,'wireless_ds4_game_bridge_image_state')
else:
    # Before implementation: event model is ready, old production IO/session
    # still ignore it. This isolates the missing integration rather than failing
    # because the old snapshot lacks the newly introduced test header.
    program += 'int wireless_ds4_game_bridge_image_state(pid_t pid){return poords4_game_process_watch_poll(&g_game_process_watch,pid);}\n'
program += 'int '+function(source,'wireless_ds4_game_bridge_abandon')
program += 'static int '+function(source,'game_bridge_process_write')
program += 'static int '+function(source,'game_bridge_process_read')
program += 'static int '+function(source,'game_bridge_find_slot')
program += 'int '+function(source,'wireless_ds4_game_bridge_status')
program += 'static int '+function(main,'teardown_game_bridge')
program += 'static const char *'+function(main,'game_session_end_reason_name')
if '\ngame_rumble_routes_open(' in main:
    for name in ('game_rumble_routes_open','game_rumble_routes_tick','game_rumble_routes_close'):
        program+=('static int ' if name.endswith('close') else 'static void ')+function(main,name)
program += 'static unsigned '+session
program += r'''
static void setup(void){
 poords4_game_process_watch_close(&g_game_process_watch);
 close_calls=open_fail=register_fail=poll_fail=poll_interrupt=error_event=0;pending_event=0;
 paging_calls=physical_reads=physical_writes=remove_calls=quiesce_calls=update_calls=rumble_close_calls=copy_fail=seq_fail=0;
 g_game_bridge_direct_pid=-1;g_game_bridge_direct_args=0;g_game_bridge_direct_cr3=g_game_bridge_direct_dmap=0;
 memset(&fixture,0,sizeof fixture);fixture.magic=POORDS4_GAME_BRIDGE_MAGIC;fixture.reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;
 fixture.pad_size=POORDS4_GAME_BRIDGE_PAD_SIZE;fixture.remote_block=0x2000;fixture.remote_block_size=0x4000;
 fixture.direct_active=1;fixture.direct_packets=65;fixture.read_state_calls=17;fixture.reserved0=2;
 fixture.pad_handle=0x03200702;fixture.slots[2].pad_handle=fixture.pad_handle;fixture.slots[2].pad_index=2;fixture.slots[2].active=1;
}
static int status_failure(void){
 PoorDS4GameBridgeStatus status,zero={0};
 for(int mode=0;mode<5;mode++){
   setup();memset(&status,0xa5,sizeof status);
   if(mode==0)copy_fail=1;if(mode==1)fixture.magic=0;if(mode==2)seq_fail=1;if(mode==3)fixture.pad_size=1;
   CHECK(wireless_ds4_game_bridge_status(mode==4?-1:94,0x2000,&status)==-1);
   CHECK(!memcmp(&status,&zero,sizeof status));
 }
 setup();CHECK(wireless_ds4_game_bridge_status(94,0x2000,&status)==0);
 CHECK(status.bridge_ready && status.active && status.game_pad_index==2 && status.read_state_calls==17);
 return 0;
}
static int status_high_client_index(void){
 PoorDS4GameBridgeStatus status;
 for(int raw=4;raw<8;raw++){
  setup();memset(fixture.slots,0,sizeof fixture.slots);
  fixture.pad_handle=0x03280700|raw;fixture.reserved0=(uint32_t)raw;
  fixture.slots[0].pad_handle=fixture.pad_handle;fixture.slots[0].pad_index=raw;fixture.slots[0].active=1;
  CHECK(wireless_ds4_game_bridge_status(94,0x2000,&status)==0);
  CHECK(status.game_pad_index==0 && status.slots[0].pad_index==raw);
 }
 return 0;
}
static int exec_io_and_cleanup(void){
 setup();CHECK(poords4_game_process_watch_open(&g_game_process_watch,94)==0);
 uint64_t value=0;CHECK(game_bridge_process_write(94,0x2000,&value,sizeof value)==0 && physical_writes==1);
 paging_calls=physical_reads=physical_writes=0;pending_event=NOTE_EXEC;
 CHECK(game_bridge_process_write(94,0x2000,&value,sizeof value)==-1);
 CHECK(game_bridge_process_read(94,0x2000,&value,sizeof value)==-1);
 CHECK(!paging_calls && !physical_reads && !physical_writes);
 CHECK(wireless_ds4_game_bridge_image_state(94)==1 && wireless_ds4_game_bridge_image_state(94)==1);
 const char *mode=NULL;CHECK(teardown_game_bridge(94,0x2000,1,&mode)==0);
 CHECK(!remove_calls && !quiesce_calls && !strcmp(mode,"abandon-replaced-image"));
 CHECK(!g_game_bridge_direct_cr3 && !g_game_bridge_direct_dmap && close_calls==1);
 CHECK(poords4_game_process_watch_open(&g_game_process_watch,94)==0);
 CHECK(game_bridge_process_write(94,0x4000,&value,sizeof value)==0 && paging_calls==1 && physical_writes==1);
 return 0;
}
static int exec_session(void){
 setup();CHECK(poords4_game_process_watch_open(&g_game_process_watch,94)==0);pending_event=NOTE_EXEC;
 GameSessionEndReason reason=SESSION_END_STOP_REQUESTED;
 CHECK(run_game_session(63,0x1000,94,0x2000,1,0,&reason)==0);
 CHECK(!strcmp(game_session_end_reason_name(reason),"game_image_changed"));
 CHECK(!rumble_close_calls && !update_calls && !remove_calls && !quiesce_calls && close_calls==1);
 return 0;
}
static int reacquire_decision(void){
 setup();sleep_calls=0;
 GameSessionEndReason end_reason=(GameSessionEndReason)8;
 pid_t game_pid=94,launch_candidate_pid=-1,retry_pid=94;
 uint64_t launch_candidate_since_ms=0;
 unsigned install_retries=999,turns=0;
 while(turns++==0){
'''+reacquire+r'''
   break;
 }
 CHECK(launch_candidate_pid==94 && launch_candidate_since_ms==1 && retry_pid==-1 && install_retries==0 && sleep_calls==1);
 return 0;
}
static int watch_errors(void){
 setup();register_fail=1;CHECK(poords4_game_process_watch_open(&g_game_process_watch,94)==-1 && close_calls==1);
 CHECK(wireless_ds4_game_bridge_image_state(94)==0);
 setup();CHECK(poords4_game_process_watch_open(&g_game_process_watch,94)==0);
 poll_interrupt=1;CHECK(wireless_ds4_game_bridge_image_state(94)==0);
 pending_event=NOTE_EXEC;CHECK(wireless_ds4_game_bridge_image_state(94)==1);
 setup();CHECK(poords4_game_process_watch_open(&g_game_process_watch,94)==0);
 pending_event=NOTE_EXEC|NOTE_EXIT;CHECK(wireless_ds4_game_bridge_image_state(94)==2);
 setup();CHECK(poords4_game_process_watch_open(&g_game_process_watch,94)==0);poll_fail=1;
 CHECK(wireless_ds4_game_bridge_image_state(94)==-1);poll_fail=0;
 CHECK(wireless_ds4_game_bridge_image_state(94)==-1);
 return 0;
}
int main(void){
 int failed=0;
 failed+=status_failure();failed+=status_high_client_index();failed+=exec_io_and_cleanup();failed+=exec_session();failed+=reacquire_decision();failed+=watch_errors();
 printf("production status failure/logical primary for higher Sony indexes, exec IO/cache refusal, actual session cleanup and same-PID reacquisition, notification delivery/error contract: failed=%d\n",failed);
 return failed?1:0;
}
'''
cc = os.environ.get('POORDS4_HOST_CC','gcc')
with tempfile.TemporaryDirectory(prefix='game-image-recovery-') as tmp:
    tmp = Path(tmp)
    (tmp/'sys').mkdir()
    (tmp/'sys/event.h').write_text(event_header)
    c, exe = tmp/'test.c', tmp/'test.exe'
    c.write_text(program)
    build = subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Wno-unused-variable','-I',str(tmp),'-I',str(watch.parent),'-I',str(p),str(c),'-o',str(exe)],capture_output=True,text=True)
    run = subprocess.run([str(exe)],capture_output=True,text=True) if not build.returncode else None
    passed = run is not None and run.returncode==0
    print(json.dumps({'compiled':not build.returncode,'passed':passed,'exit':run.returncode if run else None,
      'output':run.stdout.strip() if run else '', 'error':run.stderr if run else build.stderr[:4000],
      'sources':{n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in ('wireless_ds4.c','wireless_ds4.h','game_pad_bridge_main.c')},
      'native_console_verified':False,'kqueue_delivery':'explicit host model'},indent=2))
sys.exit(0 if passed else 1)
