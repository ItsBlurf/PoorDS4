# SPDX-License-Identifier: GPL-3.0-or-later
"""Real game callbacks, route/read APIs, relay, UDP submit and reader service.

Explicit host memory/socket/Sony/clock mocks; not console/gameplay evidence.
"""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload',type=Path,default=ROOT/'payload')
parser.add_argument('--idle-only',action='store_true',help='Reproduce the live idle-route observation failure before contract/startup checks.')
parser.add_argument('--multi-only',action='store_true',help='Run actual supervisor routes and complete four-feed motor path with explicit source mocks.')
parser.add_argument('--alias-only',action='store_true',help='Reproduce output refusal of an already attributed game handle generation alias.')
opts=parser.parse_args();payload=opts.payload;source=(payload/'wireless_ds4.c').read_text()
types=source[source.index('#define POORDS4_GAME_BRIDGE_MAGIC'):source.index('static pid_t g_game_bridge_direct_pid')]
end=source.index('} RemotePadReaderArgs;')+len('} RemotePadReaderArgs;')
reader_types=source[source.rfind('typedef struct {',0,end):end]
start=source.rfind('static __attribute__',0,source.index('\ngame_bridge_find_slot('))
end=source.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))',start)
helpers=source[start:end]
game_helpers='static inline PoorDS4GameOutput *'+function(source,'game_bridge_rumble_output')+'\n'+\
    'static inline int '+function(source,'game_bridge_publish_rumble')
callbacks='\n'.join('int32_t '+function(source,n) for n in ('game_pad_set_vibration_stub','game_pad_set_vibration_mode_stub'))
api_names=['wireless_ds4_reader_rumble_status','wireless_ds4_reader_rumble_submit','game_rumble_transfer','game_rumble_header',
              'wireless_ds4_game_rumble_route','wireless_ds4_game_rumble_request']
multi_available='wireless_ds4_reader_rumble_status_slot' in source
if multi_available:api_names[:0]=['wireless_ds4_reader_rumble_status_slot','wireless_ds4_reader_rumble_submit_slot','wireless_ds4_game_bridge_reader_slot']
apis='\n'.join('static int '+function(source,n) if n in ('game_rumble_header','game_rumble_transfer') else 'int '+function(source,n) for n in api_names)
main=(payload/'game_pad_bridge_main.c').read_text()
routes='\n'.join(('static int ' if n.endswith('close') else 'static void ')+function(main,n) for n in
    ('game_rumble_routes_open','game_rumble_routes_tick','game_rumble_routes_close')) if multi_available else ''
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "wireless_ds4.h"
#define __PROSPERO__ 1
#define MSG_DONTWAIT 0x80
#define POORDS4_REMOTE_PAD_CAPACITY 256u
#undef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4
'''+types+reader_types+r'''
#include "reader_rumble_channel.h"
static GamePadBridgeTriggerArgs game;
static RemotePadReaderTransport reader;
static pid_t g_game_bridge_direct_pid=59,g_reader_rumble_pid=99;
static intptr_t g_game_bridge_direct_args,g_reader_rumble_args;
static int g_reader_rumble_fd=17;
static uint64_t g_reader_rumble_sent[4];
static int g_game_reader_slots[4];
static void poords4_log(const char *format,...){(void)format;}
static uint64_t now_ns=1000000000;
static int motor_calls,mode_calls,native_game_calls,native_game_mode_calls,fail_send,change_on_status,status_reads;
static int fail_prefix_at,fail_copy_at,copy_calls;
static uint8_t motors[2],datagrams[16][48],slot_motors[4][2];static int queued,queue_head,queue_tail,force_public;
static uint16_t device_products[4]={0x05c4,0x09cc,0x05c4,0x09cc};
static int fixture_clock(int id,void *out){if(id!=4)return -1;int64_t *t=out;t[0]=now_ns/1000000000;t[1]=now_ns%1000000000;return 0;}
static int source_identity(int h){return !force_public && h>=44 && h<48?0x3300441:0;}
static int source_device_info(int h,void *out){if(h<44 || h>=48)return -1;uint8_t *b=out;memset(b,0,256);b[16]=0x4c;b[17]=5;b[18]=device_products[h-44];b[19]=device_products[h-44]>>8;return 0;}
static int source_mode(int h,int mode){if(h<44 || h>=48 || mode!=2)return -1;mode_calls++;return 0;}
static int source_motors(int h,const void *value){if(h<44 || h>=48)return -1;memcpy(motors,value,2);memcpy(slot_motors[h-44],value,2);motor_calls++;return 0;}
static intptr_t receive(int fd,void *out,size_t n,int flags,void *a,void *b){
 if(fd!=17 || n!=49 || flags!=MSG_DONTWAIT || a || b)return -1;
 if(!queued)return -1;memcpy(out,datagrams[queue_head++&15],48);queued--;return 48;
}
static ssize_t fixture_send(int fd,const void *data,size_t size,int flags){
 if(fd!=17 || size!=48 || flags!=MSG_DONTWAIT || queued==16 || fail_send)return -1;
 memcpy(datagrams[queue_tail++&15],data,48);queued++;return 48;
}
static int fixture_sleep(unsigned usec){now_ns+=(uint64_t)usec*1000;reader_rumble_service(&reader,0);return 0;}
static int native_game(int h,const void *value){(void)h;(void)value;native_game_calls++;return 77;}
static int native_game_mode(int h,int mode){(void)h;(void)mode;native_game_mode_calls++;return 78;}
static int copy_range(intptr_t address,void *out,size_t size,void *base,size_t bound,int write){
 uintptr_t offset=(uintptr_t)address-(uintptr_t)base;
 if(offset>bound || size>bound-offset)return -1;
 if(write)memcpy((uint8_t*)base+offset,out,size);else memcpy(out,(uint8_t*)base+offset,size);
 return 0;
}
static int game_bridge_process_read(pid_t pid,intptr_t address,void *out,size_t size){return pid==59?copy_range(address,out,size,&game,sizeof game,0):-1;}
static int game_bridge_process_write(pid_t pid,intptr_t address,const void *data,size_t size){return pid==59?copy_range(address,(void*)data,size,&game,sizeof game,1):-1;}
static int remote_reader_copyout(pid_t pid,intptr_t address,void *out,size_t size){
 if(++copy_calls==fail_copy_at)return -1;
 if(address==(intptr_t)&reader){
  status_reads++;
  if(status_reads==fail_prefix_at)return -1;
  if(change_on_status && status_reads==change_on_status){
   ((ScePadData*)(void*)reader.input.pad_data)->count++;reader_rumble_service(&reader,0);change_on_status=0;
  }
 }
 return pid==99?copy_range(address,out,size,&reader,sizeof reader,0):-1;
}
static int game_bridge_copy_args(pid_t pid,intptr_t address,GamePadBridgeArgs *out){return game_bridge_process_read(pid,address,out,sizeof *out);}
#define clock_gettime fixture_clock
#define send fixture_send
#define usleep fixture_sleep
'''+helpers+game_helpers+callbacks+apis+r'''
#include "game_rumble_relay.h"
'''+routes+r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
static void setup(void){
 memset(&game,0,sizeof game);memset(&reader,0,sizeof reader);
 g_game_bridge_direct_args=(intptr_t)&game;g_reader_rumble_args=(intptr_t)&reader;memset(&g_reader_rumble_sent,0,sizeof g_reader_rumble_sent);
 queued=queue_head=queue_tail=force_public=0;memset(slot_motors,0,sizeof slot_motors);
 fail_send=0;change_on_status=0;status_reads=0;fail_prefix_at=0;fail_copy_at=0;copy_calls=0;
 GamePadBridgeArgs *a=&game.input;a->magic=POORDS4_GAME_BRIDGE_MAGIC;a->reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;
 a->active=1;a->pad_handle=0x3300;a->direct_active=1;a->direct_lease=100000;
 for(unsigned i=0;i<4;i++){a->slots[i].pad_handle=-1;a->slots[i].pad_index=-1;}
 a->slots[0].pad_handle=0x3300;a->slots[0].pad_index=0;a->slots[0].active=1;a->slots[0].direct_lease=100000;
 game.magic=POORDS4_TRIGGER_ARGS_MAGIC;game.rumble.magic=POORDS4_GAME_OUTPUT_MAGIC;
 game.rumble.version=1;game.rumble.size=sizeof game.rumble;game.rumble.slots=4;
 game.rumble.fp_vibration=(intptr_t)native_game;game.rumble.fp_mode=(intptr_t)native_game_mode;game.rumble.fp_clock=(intptr_t)fixture_clock;
 reader.socket_fd=17;reader.fp_recvfrom=(intptr_t)receive;
 reader.input.ready=1;reader.input.owner_pid=getpid();reader.input.pad_handle=44;reader.input.last_result=0;
 ScePadData *pad=(void*)reader.input.pad_data;pad->connected=1;pad->count=1;
 reader.output.magic=POORDS4_READER_OUTPUT_MAGIC;reader.output.version=1;
 reader.output.size=sizeof reader.output;reader.output.slots=4;reader.output.session=88;
 reader.output.fp_clock=(intptr_t)fixture_clock;reader.output.fp_is_ds4=(intptr_t)source_identity;
 reader.output.fp_mode=(intptr_t)source_mode;reader.output.fp_vibration=(intptr_t)source_motors;
 for(unsigned i=0;i<4;i++){reader.output.channels[i].state.handle=-1;reader.output.channels[i].state.user=-1;}
 reader.output.channels[0].state.handle=44;reader.output.channels[0].state.user=9;
 reader_rumble_service(&reader,0);
}
static void tick(PoorDS4GameRumbleRelay *r){poords4_game_rumble_tick(r,now_ns,1,0);reader_rumble_service(&reader,0);}
'''+(r'''
static int multi(void){
 setup();force_public=1;
 game.input.pad_handle=0x3302;game.input.reserved0=2;
 const unsigned mapping[4]={1,3,0,2};
 for(unsigned i=0;i<4;i++){
  g_game_reader_slots[i]=i==2?-1:(int)mapping[i];
  game.input.slots[i].pad_handle=0x3300+i;game.input.slots[i].pad_index=i;
  game.input.slots[i].active=1;game.input.slots[i].direct_lease=100000;game.input.slots[i].direct_packets=1;
  reader.input.slot_handles[i]=44+i;reader.input.slot_results[i]=0;
  ScePadData *p=i?(void*)reader.input.slot_data[i]:(void*)reader.input.pad_data;p->connected=1;p->count=1;
  reader.output.channels[i].state.handle=44+i;reader.output.channels[i].state.user=9+i;
  reader.output.channels[i].state.reserved=0x054c|((uint32_t)device_products[i]<<16);
 }
 reader.output.fp_device_info=(intptr_t)source_device_info;reader_rumble_service(&reader,0);
 PoorDS4GameRumbleRelay routes[4];game_rumble_routes_open(routes,99,(intptr_t)&reader,59,(intptr_t)&game,2,150);
 for(unsigned i=0;i<4;i++){
  CHECK(routes[i].enabled && routes[i].source_slot==mapping[i]);
  PoorDS4Vibration value={17+i,81+i};
  CHECK(game_pad_set_vibration_stub(0x01003300+i,&value,&game.input)==0);
 }
 game_rumble_routes_tick(routes,now_ns,1);CHECK(queued==4);reader_rumble_service(&reader,0);
 game_rumble_routes_tick(routes,now_ns,1);
 for(unsigned i=0;i<4;i++){
  CHECK(routes[i].requests==1 && routes[i].submissions==1 && routes[i].acknowledgments==1 && !routes[i].pending && !routes[i].failed);
  CHECK(slot_motors[mapping[i]][0]==17+i && slot_motors[mapping[i]][1]==81+i);
  CHECK(reader.output.channels[mapping[i]].state.identity_path==2);
 }
 ((ScePadData*)(void*)reader.input.slot_data[1])->connected=0;reader_rumble_service(&reader,0);game_rumble_routes_tick(routes,now_ns,1);
 CHECK(!routes[0].enabled && routes[0].waiting && !slot_motors[1][0] && !slot_motors[1][1]);
 for(unsigned i=1;i<4;i++)CHECK(routes[i].enabled && slot_motors[mapping[i]][0]==17+i);
 ((ScePadData*)(void*)reader.input.slot_data[1])->connected=1;reader_rumble_service(&reader,0);game_rumble_routes_tick(routes,now_ns,1);
 CHECK(routes[0].enabled && routes[0].generation==151 && !slot_motors[1][0]);
 PoorDS4Vibration fresh={71,72};CHECK(game_pad_set_vibration_stub(0x3300,&fresh,&game.input)==0);
 game_rumble_routes_tick(routes,now_ns,1);reader_rumble_service(&reader,0);game_rumble_routes_tick(routes,now_ns,1);
 CHECK(routes[0].acknowledgments==2 && slot_motors[1][0]==71 && slot_motors[1][1]==72);
 CHECK(game_rumble_routes_close(routes,1)==0);
 for(unsigned i=0;i<4;i++)CHECK(!slot_motors[i][0] && !slot_motors[i][1] && !game.rumble.channels[i].route.enabled);
 /* A secondary device cannot borrow the primary's admitted DS4 identity. */
 device_products[1]=0x0ce6;
 game_rumble_routes_open(routes,99,(intptr_t)&reader,59,(intptr_t)&game,2,175);
 PoorDS4Vibration wrong_type={91,92};CHECK(game_pad_set_vibration_stub(0x3300,&wrong_type,&game.input)==0);
 game_rumble_routes_tick(routes,now_ns,1);reader_rumble_service(&reader,0);game_rumble_routes_tick(routes,now_ns,1);
 CHECK(routes[0].failed && !slot_motors[1][0] && !slot_motors[1][1]);
 for(unsigned i=1;i<4;i++)CHECK(routes[i].enabled && !routes[i].failed);
 device_products[1]=0x09cc;CHECK(game_rumble_routes_close(routes,1)==0);
 /* Native game handles and unassigned source lanes stay native. */
 g_game_reader_slots[1]=-1;game.input.slots[1].is_dualsense=1;
 game_rumble_routes_open(routes,99,(intptr_t)&reader,59,(intptr_t)&game,2,200);
 CHECK(!routes[1].enabled && !routes[1].generation);
 int old_native=native_game_calls;CHECK(game_pad_set_vibration_stub(0x3301,&fresh,&game.input)==77 && native_game_calls==old_native+1);
 CHECK(game_rumble_routes_close(routes,1)==0);
 puts("actual supervisor open/tick/close -> game callbacks -> attributed/permuted source channels -> UDP -> Sony mocks: four independent ACK sequences, mixed public DS4 revisions, secondary DS5 identity refusal, isolated disconnect/reconnect, fresh-only recovery, all-channel neutral and native DS5");return 0;
}
''' if multi_available else '')+r'''
int main(int argc,char **argv){
'''+(' if(argc>1 && !strcmp(argv[1],"multi"))return multi();\n' if multi_available else ' if(argc>1 && !strcmp(argv[1],"multi")){fputs("secondary output APIs absent in this source\\n",stderr);return 1;}\n')+r'''
 if(argc>1 && !strcmp(argv[1],"alias")){
  setup();PoorDS4GameRumbleRelay alias;
  poords4_game_rumble_open(&alias,99,(intptr_t)&reader,59,(intptr_t)&game,0,1);CHECK(alias.enabled);
  CHECK(game_bridge_find_slot(&game.input,0x01003300)==0);
  PoorDS4Vibration value={31,15};CHECK(game_pad_set_vibration_mode_stub(0x01003300,2,&game.input)==0);
  CHECK(game_pad_set_vibration_stub(0x01003300,&value,&game.input)==0);tick(&alias);tick(&alias);
  CHECK(alias.acknowledgments==1 && motors[0]==31 && motors[1]==15 && !native_game_calls && !native_game_mode_calls);
  CHECK(poords4_game_rumble_close(&alias,1)==0);puts("actual game handle alias -> route -> native mock ACK; Sony passthrough is not used for the attributed DS4 alias");return 0;
 }
 setup();PoorDS4GameRumbleRelay r;
 poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,1);CHECK(r.enabled);
 PoorDS4Vibration value={31,15};
 CHECK(game_pad_set_vibration_mode_stub(0x3300,2,&game.input)==0 && !native_game_mode_calls);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0 && !native_game_calls);
 tick(&r);tick(&r);CHECK(r.requests==1 && r.acknowledgments==1 && motors[0]==31 && motors[1]==15);
 now_ns+=300000000;tick(&r);tick(&r);CHECK(r.submissions==2 && r.acknowledgments==2);
 CHECK(poords4_game_rumble_close(&r,1)==0 && !motors[0] && !motors[1] && !game.rumble.channels[0].route.enabled);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==77);
 CHECK(game_pad_set_vibration_stub(0x3301,&value,&game.input)==77);
 CHECK(game_pad_set_vibration_mode_stub(0x3300,3,&game.input)==78);
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,10);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);tick(&r);tick(&r);
 ScePadData *pad=(void*)reader.input.pad_data;
 pad->connected=0;reader_rumble_service(&reader,0);tick(&r);
 CHECK(!r.failed && !r.enabled && !motors[0] && !motors[1] && !game.rumble.channels[0].route.enabled);
 pad->connected=1;pad->count=2;reader_rumble_service(&reader,0);tick(&r);
 CHECK(r.enabled && !r.failed && r.generation==11 && r.requests==1 && !motors[0] && !motors[1]);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);tick(&r);tick(&r);
 CHECK(r.requests==2 && r.acknowledgments==2 && motors[0]==31 && motors[1]==15);
 /* Epoch change while a command is queued: reject it, never carry motors
  * across the new binding, and admit only a new game publication. */
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);
 poords4_game_rumble_tick(&r,now_ns,1,0);CHECK(r.pending && queued);
 pad->count=3;reader_rumble_service(&reader,0);tick(&r);
 CHECK(r.enabled && !r.failed && !r.pending && r.generation==12 && !motors[0] && !motors[1]);
 uint64_t old_submissions=r.submissions;tick(&r);CHECK(r.submissions==old_submissions);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);tick(&r);tick(&r);
 CHECK(motors[0]==31 && motors[1]==15 && r.acknowledgments==3);
 CHECK(poords4_game_rumble_close(&r,1)==0);
 /* A delayed old-generation writer can publish after route replacement. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,20);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);
 PoorDS4GameOutputCommand old_command=game.rumble.channels[0].commands[1];
 pad=(void*)reader.input.pad_data;pad->count=2;reader_rumble_service(&reader,0);tick(&r);
 CHECK(r.enabled && r.generation==21 && !r.submissions);
 old_command.sequence=2;game.rumble.channels[0].commands[0]=old_command;game.rumble.channels[0].published=2;
 tick(&r);CHECK(!r.failed && !r.submissions && r.enabled);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);tick(&r);tick(&r);CHECK(r.acknowledgments==1);
 CHECK(poords4_game_rumble_close(&r,1)==0);
 /* A source which starts disconnected becomes routable when it connects. */
 setup();pad=(void*)reader.input.pad_data;pad->connected=0;reader_rumble_service(&reader,0);
 poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,30);CHECK(!r.enabled);
 pad->connected=1;reader_rumble_service(&reader,0);tick(&r);CHECK(r.enabled && !r.failed);
 CHECK(poords4_game_rumble_close(&r,1)==0);
 /* A failed native stop cannot be bypassed by reconnection. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,40);
 reader.output.channels[0].state.quarantined=1;
 pad=(void*)reader.input.pad_data;pad->count=2;reader_rumble_service(&reader,0);tick(&r);
 CHECK(r.failed && !r.enabled && !r.submissions);
 /* Reconnect between relay validation and the submit function's fresh check. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,50);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);
 change_on_status=2;status_reads=0;tick(&r);
 CHECK(!r.failed && r.waiting && !r.pending && !r.submissions && !queued);
 tick(&r);CHECK(r.enabled && !r.failed && r.generation==51 && !r.submissions);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);tick(&r);tick(&r);CHECK(r.acknowledgments==1);
 CHECK(poords4_game_rumble_close(&r,1)==0);
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,2);
 game.input.slots[0].is_dualsense=1;CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==77);
 game.input.slots[0].is_dualsense=0;game.input.slots[0].is_simulated=1;
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==77);
 game.input.slots[0].is_simulated=0;game.rumble.channels[0].lock=1;
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==-1);game.rumble.channels[0].lock=0;
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);
 now_ns+=1100000000;tick(&r);CHECK(r.failed && !r.submissions && !game.rumble.channels[0].route.enabled);
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,3);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);fail_send=1;
 tick(&r);uint64_t submissions=r.submissions;tick(&r);CHECK(r.failed && r.pending && r.submissions==submissions);
 CHECK(poords4_game_rumble_close(&r,1)==-1 && r.submissions==submissions && !game.rumble.channels[0].route.enabled);
 /* A bounded observation failure has not attempted a source command. */
 for(unsigned phase=1;phase<=5;phase++){
  setup();PoorDS4RumbleStatus observation;copy_calls=0;fail_copy_at=phase;
  CHECK(wireless_ds4_reader_rumble_status(99,(intptr_t)&reader,&observation)==(phase==3?0:-2));
  fail_copy_at=0;CHECK(wireless_ds4_reader_rumble_status(99,(intptr_t)&reader,&observation)==0);
 }
 setup();PoorDS4RumbleStatus observation;reader.output.channels[0].state.sequence|=1;
 CHECK(wireless_ds4_reader_rumble_status(99,(intptr_t)&reader,&observation)==-2);
 reader.output.channels[0].state.sequence++;reader.input.owner_pid++;
 CHECK(wireless_ds4_reader_rumble_status(99,(intptr_t)&reader,&observation)==-1);
 reader.input.owner_pid=getpid();reader.output.magic=0;
 CHECK(wireless_ds4_reader_rumble_status(99,(intptr_t)&reader,&observation)==-1);
 /* Initial status contention can recover without a permanently closed route. */
 setup();fail_prefix_at=1;
 poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,60);
 CHECK(r.waiting && !r.failed && !r.enabled);tick(&r);
 CHECK(r.enabled && !r.failed && !r.submissions);
 CHECK(poords4_game_rumble_close(&r,1)==0);
 /* Idle transient read: recover fresh generation, without sending motors. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,70);
 fail_prefix_at=status_reads+1;tick(&r);
 CHECK(r.waiting && !r.failed && !r.enabled && !r.submissions);
 tick(&r);CHECK(r.enabled && !r.failed && r.generation==71 && !r.submissions);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);tick(&r);tick(&r);
 CHECK(r.acknowledgments==1);CHECK(poords4_game_rumble_close(&r,1)==0);
 /* In-flight ACK is retained. No renew/send until its lease becomes neutral. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,80);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);
 poords4_game_rumble_tick(&r,now_ns,1,0);CHECK(r.pending && queued);
 submissions=r.submissions;fail_prefix_at=status_reads+1;tick(&r);
 CHECK(r.waiting && r.pending && !r.failed && r.submissions==submissions);
 tick(&r);CHECK(!r.pending && r.waiting && !r.enabled && r.acknowledgments==1 && r.submissions==submissions);
 now_ns+=POORDS4_OUTPUT_TIMEOUT_NS+1;reader_rumble_service(&reader,0);tick(&r);
 CHECK(r.enabled && !r.failed && r.generation==81 && !motors[0] && !motors[1] && r.submissions==submissions);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);tick(&r);tick(&r);
 CHECK(r.acknowledgments==2);CHECK(poords4_game_rumble_close(&r,1)==0);
 /* Status loss inside submit has not attempted send; no pending uncertainty. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,90);
 CHECK(game_pad_set_vibration_stub(0x3300,&value,&game.input)==0);
 fail_prefix_at=status_reads+2;tick(&r);
 CHECK(r.waiting && !r.pending && !r.failed && !r.submissions && !queued);
 tick(&r);CHECK(r.enabled && r.generation==91 && !r.submissions);
 CHECK(poords4_game_rumble_close(&r,1)==0);
 /* Invalid fresh state cannot use the temporary-observation recovery path. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,95);
 fail_prefix_at=status_reads+1;tick(&r);reader.output.session++;
 tick(&r);CHECK(r.failed && !r.enabled && !r.submissions);
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,96);
 fail_prefix_at=status_reads+1;tick(&r);reader.output.channels[0].state.quarantined=1;
 tick(&r);CHECK(r.failed && !r.enabled && !r.submissions);
 /* Stop cannot be undone by observational recovery. */
 setup();poords4_game_rumble_open(&r,99,(intptr_t)&reader,59,(intptr_t)&game,0,100);
 fail_prefix_at=status_reads+1;poords4_game_rumble_tick(&r,now_ns,1,1);tick(&r);
 CHECK(r.failed && !r.enabled && !r.waiting && !r.submissions);
 puts("real game callback -> route/read -> relay -> UDP submit -> reader -> Sony mock ACK; refresh/neutral cleanup; disconnect/reconnect and count change; queued old-epoch cancellation; late old-generation discard; initially disconnected admission; quarantine retained; native DS5/unmatched/modes; simulated/busy/stale refusal; uncertain send never replayed; bounded status-read and seqlock loss; initial/idle/pending/pre-send recovery; new-session/quarantine/stop refusal");return 0;
}
'''
cc=os.environ.get('POORDS4_HOST_CC','gcc')
if not multi_available:program=program.replace('static uint64_t g_reader_rumble_sent[4];','static uint64_t g_reader_rumble_sent;')
if opts.idle_only:
    start=program.index(' /* A bounded observation failure')
    end=program.index(' /* Idle transient read',start)
    program=program[:start]+program[end:]
with tempfile.TemporaryDirectory(prefix='game-rumble-path-') as tmp:
    c,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe';c.write_text(program)
    build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
    run=subprocess.run([str(exe)]+(['multi'] if opts.multi_only else ['alias'] if opts.alias_only else []),capture_output=True,text=True) if not build.returncode else None
    passed=run is not None and run.returncode==0
    print(json.dumps({'compiled':not build.returncode,'passed':passed,'exit':run.returncode if run else None,'output':run.stdout.strip() if run else '',
        'error':run.stderr if run else build.stderr[-2800:],'scope':'full four-feed supervisor/output path' if opts.multi_only else 'attributed output alias' if opts.alias_only else 'idle reproduction and relay regressions' if opts.idle_only else 'all relay and observation cases','sources':{n:hashlib.sha256((payload/n).read_bytes()).hexdigest() for n in
            ('wireless_ds4.c','wireless_ds4.h','game_pad_bridge_main.c','pad_feedback.h','reader_rumble_channel.h','game_rumble_relay.h')},'native_console_verified':False},indent=2))
sys.exit(0 if passed else 1)
